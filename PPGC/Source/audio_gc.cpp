// audio::Engine on the GameCube (engine/audio/audio.h), in place of XAudio2.
//
// The sounds are the game's, made Microsoft ADPCM at 32 kHz by
// tools/convert_audio.py (the xWMA the PC plays can't be decoded here). One
// thread mixes every voice into one stereo stream at 32 kHz, which one ASND
// voice plays; it decodes as it mixes, a block (a few hundred samples) at a
// time.
//
// Effects live in ARAM, below the renderer's cache (aram_gc.h): loaded from
// the disc when a movie attaches them, and read back a block or two at a
// time as they play. When ARAM is full, the effects played longest ago (and
// not playing) leave it, to be loaded again if played again. Music streams
// from the disc: a reader thread keeps a couple of seconds ahead of it.
//
// Volumes and pans as the PC's (audio.cpp): a voice's volume multiplies it,
// and a pan lowers the far side linearly.
#include "audio/audio.h"

#include <gccore.h>
#include <asndlib.h>
#include <malloc.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/semaphore.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "System/System.h"
#include "aram_gc.h"

#include "trace_gc.h"

namespace audio_gc {

namespace {

constexpr int kRate = 32000;
constexpr int kMixFrames = 1024;  // 32 ms a buffer: over one of ASND's mixing blocks, or it runs dry between buffers
constexpr int kBuffers = 3;
constexpr int kAsndVoice = 15;   // above Octave's (0-7) and its streams' (8-14, unused here)
constexpr int kVoices = 64;
constexpr int kEffects = 512, kMusic = 64;
constexpr uint32_t kWindow = 512;  // bytes of an effect read from ARAM at a time (two blocks)
constexpr int kEffectSamples = 512;  // a block's, decoded: 500 frames mono, 244 stereo
constexpr int kMusicSamples = 2048;  // 1012 frames stereo
constexpr uint32_t kMusicBlock = 1024;
constexpr uint32_t kMusicRingBlocks = 64;  // 2 s of music
constexpr uint32_t kMusicRead = 16;        // blocks read at a time

// ---- Microsoft ADPCM

const int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

struct Format {
    int channels = 0;
    uint32_t block_align = 0;
    int block_frames = 0;
    int16_t coef[7][2] = {};
    uint32_t frames = 0;       // in all (the fact chunk)
    uint32_t data_offset = 0;  // in the file
    uint32_t data_size = 0;
};

uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24);
}

// The RIFF header, up to the data chunk (the first `size` bytes of a file).
bool parse(const uint8_t* d, uint32_t size, Format& f) {
    if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return false;
    bool fmt = false;
    for (uint32_t pos = 12; pos + 8 <= size;) {
        uint32_t len = le32(d + pos + 4);
        const uint8_t* body = d + pos + 8;
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && pos + 8 + 32 <= size) {
            if (le16(body) != 2) return false;  // WAVE_FORMAT_ADPCM
            f.channels = le16(body + 2);
            // WAVEFORMATEX (to +16, then cbSize), then the samples a block,
            // the number of coefficient pairs and the pairs.
            f.block_align = le16(body + 12);
            f.block_frames = le16(body + 18);
            int ncoef = std::min<int>(le16(body + 20), 7);
            for (int i = 0; i < ncoef; i++) {
                f.coef[i][0] = int16_t(le16(body + 22 + i * 4));
                f.coef[i][1] = int16_t(le16(body + 24 + i * 4));
            }
            fmt = true;
        } else if (std::memcmp(d + pos, "fact", 4) == 0 && pos + 12 <= size) {
            f.frames = le32(body);
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            f.data_offset = pos + 8;
            f.data_size = len;
            break;
        }
        pos += 8 + len + (len & 1);
    }
    return fmt && f.data_offset && (f.channels == 1 || f.channels == 2) && f.block_frames > 2 &&
           f.block_align >= uint32_t(7 * f.channels) &&
           uint32_t(f.block_frames - 2) * uint32_t(f.channels) == (f.block_align - 7 * uint32_t(f.channels)) * 2;
}

// A block to 16-bit samples, the channels interleaved. Its header gives each
// channel's predictor, step and first two samples (the second first); then a
// nibble a sample, the high one first, alternating channels.
void decode(const Format& f, const uint8_t* b, int16_t* out) {
    int ch = f.channels;
    int pred[2], delta[2], s1[2], s2[2];
    for (int c = 0; c < ch; c++) {
        pred[c] = std::min<int>(b[c], 6);
        delta[c] = int16_t(le16(b + ch + c * 2));
        s1[c] = int16_t(le16(b + ch * 3 + c * 2));
        s2[c] = int16_t(le16(b + ch * 5 + c * 2));
        out[c] = int16_t(s2[c]);
        out[ch + c] = int16_t(s1[c]);
    }
    const uint8_t* p = b + 7 * ch;
    int16_t* o = out + 2 * ch;
    int count = (f.block_frames - 2) * ch;
    for (int k = 0; k < count; k++) {
        int c = k & (ch - 1);
        int nib = (k & 1) ? (p[k >> 1] & 15) : (p[k >> 1] >> 4);
        int predicted = (s1[c] * f.coef[pred[c]][0] + s2[c] * f.coef[pred[c]][1]) >> 8;
        int sample = std::clamp(predicted + (nib >= 8 ? nib - 16 : nib) * delta[c], -32768, 32767);
        s2[c] = s1[c];
        s1[c] = sample;
        delta[c] = std::max(16, (kAdapt[nib] * delta[c]) >> 8);
        *o++ = int16_t(sample);
    }
}

// ---- effects, in ARAM

struct Effect {
    bool loaded = false;     // attached (its slot taken)
    bool resident = false;   // in ARAM now
    std::string path;        // to load it again
    Format fmt;
    uint32_t at = 0, bytes = 0;  // in ARAM
    uint32_t last_used = 0;
    int playing = 0;             // voices on it
};

struct AramBlock {
    uint32_t at, size;
    bool used;
};

Effect g_effects[kEffects];
std::vector<AramBlock> g_aram;
uint32_t g_clock = 0;  // for last_used

uint32_t aram_alloc(uint32_t len) {
    for (size_t i = 0; i < g_aram.size(); i++) {
        AramBlock& b = g_aram[i];
        if (b.used || b.size < len) continue;
        if (b.size > len) g_aram.insert(g_aram.begin() + ptrdiff_t(i) + 1, {b.at + len, b.size - len, false});
        g_aram[i].size = len;
        g_aram[i].used = true;
        return g_aram[i].at;
    }
    return 0;
}

void aram_free(uint32_t at) {
    for (size_t i = 0; i < g_aram.size(); i++) {
        if (g_aram[i].at != at || !g_aram[i].used) continue;
        g_aram[i].used = false;
        if (i + 1 < g_aram.size() && !g_aram[i + 1].used) {
            g_aram[i].size += g_aram[i + 1].size;
            g_aram.erase(g_aram.begin() + ptrdiff_t(i) + 1);
        }
        if (i > 0 && !g_aram[i - 1].used) {
            g_aram[i - 1].size += g_aram[i].size;
            g_aram.erase(g_aram.begin() + ptrdiff_t(i));
        }
        return;
    }
}

// ---- voices

struct Voice {
    bool active = false;
    bool music = false;
    bool loop = false;
    int effect = -1;
    float left = 1, right = 1;  // gains
    // Decoding.
    const Format* fmt = nullptr;
    uint32_t block = 0;   // the next to decode
    uint32_t blocks = 0;  // in all
    uint32_t frames_left = 0;
    int16_t* pcm = nullptr;  // a block, decoded (channels interleaved)
    int pcm_pos = 0, pcm_count = 0;  // frames
    // An effect's window of ARAM.
    alignas(32) uint8_t window[kWindow];
    uint32_t window_block = UINT32_MAX;  // the first block in it
    uint32_t window_blocks = 0;
};

Voice* g_voices = nullptr;  // kVoices, then the music's
Voice* g_music_voice = nullptr;
mutex_t g_lock = LWP_MUTEX_NULL;

struct Lock {
    Lock() { LWP_MutexLock(g_lock); }
    ~Lock() { LWP_MutexUnlock(g_lock); }
};

// ---- music, from the disc

struct Track {
    bool loaded = false;
    std::string path;
    Format fmt;
};

Track g_tracks[kMusic];
int g_track = -1;              // playing
uint32_t g_generation = 0;     // bumped when the track changes
uint8_t* g_ring = nullptr;     // kMusicRingBlocks blocks
uint32_t g_ring_read = 0, g_ring_write = 0;  // blocks, counting up
uint32_t g_read_block = 0;     // the next block of the file to read
bool g_read_end = false;       // not looping, and all read
sem_t g_reader_sem = LWP_SEM_NULL;

void* reader_main(void*) {
    static uint8_t chunk[kMusicRead * kMusicBlock];
    for (;;) {
        trace::at(trace::kReader, "waiting");
        LWP_SemWait(g_reader_sem);
        for (;;) {
            std::string path;
            uint32_t generation, first, count, offset;
            {
                Lock lock;
                if (g_track < 0 || g_read_end) break;
                uint32_t room = kMusicRingBlocks - (g_ring_write - g_ring_read);
                if (room < kMusicRead) break;
                const Track& t = g_tracks[g_track];
                uint32_t blocks = t.fmt.data_size / t.fmt.block_align;
                if (g_read_block >= blocks) {
                    if (!g_music_voice->loop) {
                        g_read_end = true;
                        break;
                    }
                    g_read_block = 0;
                }
                path = t.path;
                generation = g_generation;
                first = g_read_block;
                count = std::min(kMusicRead, blocks - first);
                offset = t.fmt.data_offset + first * t.fmt.block_align;
            }
            trace::at(trace::kReader, "reading music", path.c_str());
            bool ok = SYS_ReadFileRange(path.c_str(), true, offset, count * kMusicBlock, reinterpret_cast<char*>(chunk));
            trace::at(trace::kReader, "music read; taking the lock");
            Lock lock;
            if (!ok) PpgcLog("audio: reading %s at %u failed", path.c_str(), unsigned(offset));
            if (generation != g_generation) continue;  // another track now
            if (!ok) {
                g_read_end = true;
                break;
            }
            for (uint32_t i = 0; i < count; i++)
                std::memcpy(g_ring + ((g_ring_write + i) % kMusicRingBlocks) * kMusicBlock, chunk + i * kMusicBlock,
                            kMusicBlock);
            g_ring_write += count;
            g_read_block = first + count;
        }
    }
    return nullptr;
}

// ---- the mixer

// The next block of a voice decoded; false if it has none (it has ended, or
// the music's data hasn't come from the disc yet: `starved`).
bool next_block(Voice& v, bool& starved) {
    const Format& f = *v.fmt;
    if (v.frames_left == 0 || v.block >= v.blocks) {
        if (!v.loop || v.music) return false;  // (the music's loop is the reader's)
        v.block = 0;
        v.frames_left = f.frames;
    }
    const uint8_t* data;
    if (v.music) {
        if (g_ring_read == g_ring_write) {
            starved = true;
            return false;
        }
        data = g_ring + (g_ring_read % kMusicRingBlocks) * kMusicBlock;
    } else {
        if (v.block < v.window_block || v.block >= v.window_block + v.window_blocks) {
            Effect& e = g_effects[v.effect];
            uint32_t offset = v.block * f.block_align;
            uint32_t len = std::min(kWindow, e.bytes - offset);
            aram::from_aram(v.window, e.at + offset, (len + 31) & ~31u);
            v.window_block = v.block;
            v.window_blocks = std::max<uint32_t>(1, len / f.block_align);
        }
        data = v.window + (v.block - v.window_block) * f.block_align;
    }
    decode(f, data, v.pcm);
    v.pcm_count = int(std::min<uint32_t>(uint32_t(f.block_frames), v.frames_left));
    v.pcm_pos = 0;
    v.frames_left -= uint32_t(v.pcm_count);
    v.block++;
    if (v.music) {
        g_ring_read++;
        if (v.frames_left == 0 && v.loop) {  // round again, as the reader goes
            v.block = 0;
            v.frames_left = f.frames;
        }
    }
    return true;
}

int32_t g_mix[kMixFrames * 2];

void end_voice(Voice& v) {
    if (!v.music && v.effect >= 0) g_effects[v.effect].playing--;
    v.active = false;
    v.effect = -1;
}

void mix(int16_t* out) {
    std::memset(g_mix, 0, sizeof(g_mix));
    trace::at(trace::kMixer, "taking the lock");
    Lock lock;
    trace::at(trace::kMixer, "mixing");
    bool wake_reader = false;
    for (int i = 0; i <= kVoices; i++) {
        Voice& v = g_voices[i];
        if (!v.active) continue;
        int32_t gl = int32_t(v.left * 256.0f), gr = int32_t(v.right * 256.0f);
        int step = v.fmt->channels;
        for (int n = 0; n < kMixFrames;) {
            if (v.pcm_pos >= v.pcm_count) {
                bool starved = false;
                if (!next_block(v, starved)) {
                    if (!starved) end_voice(v);
                    break;
                }
                if (v.music) wake_reader = true;
            }
            int count = std::min(kMixFrames - n, v.pcm_count - v.pcm_pos);
            const int16_t* l = v.pcm + v.pcm_pos * step;
            const int16_t* r = l + (step - 1);
            int32_t* m = g_mix + n * 2;
            for (int k = 0; k < count; k++) {
                m[k * 2] += l[k * step] * gl;
                m[k * 2 + 1] += r[k * step] * gr;
            }
            v.pcm_pos += count;
            n += count;
        }
    }
    for (int k = 0; k < kMixFrames * 2; k++) out[k] = int16_t(std::clamp(g_mix[k] >> 8, -32768, 32767));
    if (wake_reader) LWP_SemPost(g_reader_sem);
}

int16_t* g_out[kBuffers];
bool g_started = false;
volatile uint32_t g_mixed = 0;  // buffers, so far (for the status line)

void voice_callback(s32) {}  // (non-null: an underrun waits, rather than ending the voice)

void* mixer_main(void*) {
    int16_t* pending = nullptr;
    for (;;) {
        trace::at(trace::kMixer, "feeding ASND");
        if (!g_started) {
            mix(g_out[0]);
            DCFlushRange(g_out[0], kMixFrames * 4);
            if (ASND_SetVoice(kAsndVoice, VOICE_STEREO_16BIT, kRate, 0, g_out[0], kMixFrames * 4, MAX_VOLUME, MAX_VOLUME,
                              voice_callback) == SND_OK)
                g_started = true;
        } else if (ASND_TestVoiceBufferReady(kAsndVoice) == 1) {
            // A buffer mixed and not yet taken is offered again as it is: ASND
            // refuses one until its mixer has picked up the one before.
            if (!pending) {
                for (int16_t* buf : g_out) {
                    if (ASND_TestPointer(kAsndVoice, buf) == SND_BUSY) continue;
                    mix(buf);
                    DCFlushRange(buf, kMixFrames * 4);
                    pending = buf;
                    break;
                }
            }
            if (pending && ASND_AddVoice(kAsndVoice, pending, kMixFrames * 4) == SND_OK) {
                pending = nullptr;
                g_mixed++;
            }
        }
        trace::at(trace::kMixer, "sleeping");
        usleep(4000);
    }
    return nullptr;
}

void gains(Voice& v, float volume, float pan) {
    float left = pan == 0.0f ? 1.0f : std::min(1.0f, 1.0f - pan);
    float right = pan == 0.0f ? 1.0f : std::min(1.0f, 1.0f + pan);
    // (At most 2: 65 voices at full scale can't overflow the mix.)
    v.left = std::clamp(volume * left, 0.0f, 2.0f);
    v.right = std::clamp(volume * right, 0.0f, 2.0f);
}

// An effect into ARAM, making room by sending out the effects played longest
// ago (not playing). Main thread.
bool make_resident(Effect& e) {
    if (e.resident) return true;
    char* data = nullptr;
    uint32_t size = 0;
    trace::at(trace::kMain, "loading an effect", e.path.c_str());
    SYS_AcquireFileData(e.path.c_str(), true, 0, data, size);
    if (!data) return false;
    bool ok = parse(reinterpret_cast<uint8_t*>(data), size, e.fmt) && e.fmt.data_offset + e.fmt.data_size <= size &&
              e.fmt.block_frames * e.fmt.channels <= kEffectSamples && e.fmt.block_align <= kWindow;
    uint32_t bytes = (e.fmt.data_size + 31) & ~31u;
    uint32_t at = 0;
    if (ok) {
        Lock lock;
        while (!(at = aram_alloc(bytes))) {
            Effect* oldest = nullptr;
            for (Effect& o : g_effects)
                if (o.resident && o.playing == 0 && (!oldest || o.last_used < oldest->last_used)) oldest = &o;
            if (!oldest) break;
            aram_free(oldest->at);
            oldest->resident = false;
            PpgcLog("audio: %s leaves ARAM for room", oldest->path.c_str());
        }
    }
    if (at) {
        // Through a 32-byte aligned copy, a piece at a time.
        constexpr uint32_t kPiece = 16 * 1024;
        static uint8_t* piece = static_cast<uint8_t*>(memalign(32, kPiece));
        const uint8_t* src = reinterpret_cast<uint8_t*>(data) + e.fmt.data_offset;
        for (uint32_t done = 0; done < bytes; done += kPiece) {
            uint32_t n = std::min(kPiece, bytes - done);
            uint32_t have = done < e.fmt.data_size ? std::min(n, e.fmt.data_size - done) : 0;
            std::memcpy(piece, src + done, have);
            std::memset(piece + have, 0, n - have);
            aram::to_aram(piece, at + done, n);
        }
        e.at = at;
        e.bytes = bytes;
        e.resident = true;
        PpgcLog("audio: effect %s, %u KB at ARAM %06x", e.path.c_str(), unsigned(bytes / 1024), unsigned(at));
    }
    if (!ok) PpgcLog("audio: %s isn't Microsoft ADPCM", e.path.c_str());
    else if (!at) PpgcLog("audio: no room in ARAM for %s (%u KB)", e.path.c_str(), unsigned(bytes / 1024));
    SYS_ReleaseFileData(data);
    return e.resident;
}

class GcEngine : public audio::Engine {
public:
    bool init() override {
        if (!aram::init()) return false;
        g_aram.reserve(1024);
        g_aram.push_back({aram::base(), aram::top() - aram::kShapeCache - aram::base(), false});
        g_voices = static_cast<Voice*>(memalign(32, sizeof(Voice) * (kVoices + 1)));
        g_ring = static_cast<uint8_t*>(memalign(32, kMusicRingBlocks * kMusicBlock));
        for (int16_t*& b : g_out) b = static_cast<int16_t*>(memalign(32, kMixFrames * 4));
        if (!g_voices || !g_ring || !g_out[kBuffers - 1]) return false;
        for (int i = 0; i <= kVoices; i++) {
            new (&g_voices[i]) Voice();
            g_voices[i].pcm = static_cast<int16_t*>(memalign(32, (i < kVoices ? kEffectSamples : kMusicSamples) * 2));
            if (!g_voices[i].pcm) return false;
        }
        g_music_voice = &g_voices[kVoices];
        g_music_voice->music = true;
        LWP_MutexInit(&g_lock, false);
        LWP_SemInit(&g_reader_sem, 0, 16);
        // The mixer above the main thread (64), so a long frame can't starve
        // the sound; the reader below it (as Octave's audio reader, 50).
        static uint8_t mixer_stack[16 * 1024] __attribute__((aligned(32)));
        static uint8_t reader_stack[64 * 1024] __attribute__((aligned(32)));
        lwp_t thread;
        if (LWP_CreateThread(&thread, reader_main, nullptr, reader_stack, sizeof(reader_stack), 50) != 0) return false;
        if (LWP_CreateThread(&thread, mixer_main, nullptr, mixer_stack, sizeof(mixer_stack), 80) != 0) return false;
        PpgcLog("audio: %u KB of ARAM for effects", unsigned(g_aram[0].size / 1024));
        return true;
    }

    bool load_effect(uint16_t slot, const std::filesystem::path& path) override {
        Effect& e = g_effects[slot];
        e = Effect{};
        e.path = path.string() + ".wav";
        e.loaded = true;
        e.last_used = ++g_clock;
        if (!make_resident(e)) {
            e = Effect{};
            return false;
        }
        return true;
    }

    bool load_music(int id, const std::filesystem::path& path) override {
        Track& t = g_tracks[id];
        t = Track{};
        t.path = path.string() + ".wav";
        uint8_t head[256];
        if (!SYS_ReadFileRange(t.path.c_str(), true, 0, sizeof(head), reinterpret_cast<char*>(head)) ||
            !parse(head, sizeof(head), t.fmt) || t.fmt.block_align != kMusicBlock ||
            t.fmt.block_frames * t.fmt.channels > kMusicSamples) {
            PpgcLog("audio: can't play %s", t.path.c_str());
            return false;
        }
        PpgcLog("audio: music %d is %s (%u s)", id, t.path.c_str(), unsigned(t.fmt.frames / kRate));
        t.loaded = true;
        return true;
    }

    int free_effect_slot() const override {
        for (int i = 0; i < kEffects; i++)
            if (!g_effects[i].loaded) return i;
        return -1;
    }

    bool has_music(int id) const override { return g_tracks[id].loaded; }

    void unload_effect(uint16_t slot) override {
        trace::at(trace::kMain, "unload effect; taking the lock");
        Lock lock;
        Effect& e = g_effects[slot];
        for (int i = 0; i < kVoices; i++)
            if (g_voices[i].active && g_voices[i].effect == slot) end_voice(g_voices[i]);
        if (e.resident) aram_free(e.at);
        e = Effect{};
    }

    void unload_music(int id) override {
        trace::at(trace::kMain, "unload music; taking the lock");
        Lock lock;
        if (g_track == id) stop_track();
        g_tracks[id] = Track{};
    }

    int16_t play_effect(uint16_t slot, float volume, float pan, bool loop) override {
        Effect& e = g_effects[slot];
        if (!e.loaded) return -1;
        e.last_used = ++g_clock;
        if (!make_resident(e)) return -1;
        Lock lock;
        for (int i = 0; i < kVoices; i++) {
            Voice& v = g_voices[i];
            if (v.active) continue;
            v.effect = slot;
            v.loop = loop;
            v.fmt = &e.fmt;
            v.block = 0;
            v.blocks = e.fmt.data_size / e.fmt.block_align;
            v.frames_left = e.fmt.frames;
            v.pcm_pos = v.pcm_count = 0;
            v.window_block = UINT32_MAX;
            v.window_blocks = 0;
            gains(v, volume, pan);
            e.playing++;
            v.active = true;
            return int16_t(i);
        }
        return -1;
    }

    bool play_music(int id, float volume, bool loop) override {
        PpgcLog("audio: play music %d%s", id, loop ? " (looping)" : "");
        trace::at(trace::kMain, "play music; taking the lock");
        if (id < 0 || id >= kMusic || !g_tracks[id].loaded) return false;
        {
            Lock lock;
            stop_track();
            Voice& v = *g_music_voice;
            const Track& t = g_tracks[id];
            g_track = id;
            v.fmt = &t.fmt;
            v.loop = loop;
            v.block = 0;
            v.blocks = UINT32_MAX;  // (the reader knows)
            v.frames_left = t.fmt.frames;
            v.pcm_pos = v.pcm_count = 0;
            gains(v, volume, 0.0f);
            v.active = true;
        }
        LWP_SemPost(g_reader_sem);
        return true;
    }

    void set_music_volume(float volume) override {
        Lock lock;
        if (g_music_voice->active) gains(*g_music_voice, volume, 0.0f);
    }

    void stop_music() override {
        PpgcLog("audio: stop music");
        trace::at(trace::kMain, "stop music; taking the lock");
        Lock lock;
        stop_track();
    }

    void stop_voice(int16_t voice) override {
        if (voice < 0 || voice >= kVoices) return;
        Lock lock;
        if (g_voices[voice].active) end_voice(g_voices[voice]);
    }

    void set_voice(int16_t voice, float volume, float pan) override {
        if (voice < 0 || voice >= kVoices) return;
        Lock lock;
        if (g_voices[voice].active) gains(g_voices[voice], volume, pan);
    }

    void update() override {}  // (the mixer ends voices as they finish)

private:
    // With the lock held.
    static void stop_track() {
        g_music_voice->active = false;
        g_track = -1;
        g_generation++;
        g_ring_read = g_ring_write = 0;
        g_read_block = 0;
        g_read_end = false;
    }
};

}  // namespace

std::unique_ptr<audio::Engine> make_engine() {
    return std::make_unique<GcEngine>();
}

// For the status line: buffers mixed so far, voices playing, effects in
// ARAM (KB), and the music's blocks read ahead.
void stats(uint32_t& mixed, uint32_t& voices, uint32_t& effects_kb, uint32_t& music_ahead) {
    mixed = g_mixed;
    voices = 0;
    effects_kb = 0;
    music_ahead = 0;
    if (!g_voices) return;
    Lock lock;
    for (int i = 0; i <= kVoices; i++) voices += g_voices[i].active;
    for (const Effect& e : g_effects)
        if (e.resident) effects_kb += e.bytes / 1024;
    music_ahead = g_ring_write - g_ring_read;
}

}  // namespace audio_gc
