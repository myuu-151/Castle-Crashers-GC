// audio::Engine on the GameCube (engine/audio/audio.h), in place of XAudio2.
//
// The sounds are the game's, made Microsoft ADPCM by tools/convert_audio.py
// (the xWMA the PC plays can't be decoded here). One thread mixes every
// voice into one stereo stream at 32 kHz, which one ASND voice plays; it
// decodes as it mixes, a block (a few hundred samples) at a time, and
// resamples what isn't at 32 kHz (the effects are at 24), linearly.
//
// The effects are one bank (audio/sounds.bank, with its index), read into
// ARAM once at boot by the reader thread, in the background: attaching an
// effect is then only finding it (loading each file when attached, the
// console spent 40 s reading hundreds of small files). A playing effect is
// read back from ARAM a block or two at a time. The music streams from the
// disc into ARAM too, the space between the bank and the renderer's cache
// (256 KB, 8 s of music), and plays from there a block at a
// time: the same thread keeps it filled in the main thread's spare time.
//
// Volumes and pans as the PC's (audio.cpp): a voice's volume multiplies it,
// and a pan lowers the far side linearly.
#include "audio/audio.h"
#include "words_gc.h"

#include <gccore.h>
#include <asndlib.h>
#include <malloc.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/semaphore.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
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
constexpr int kEffectSamples = 1024;  // a block's, decoded: 500 frames mono, 244 stereo
constexpr int kMusicSamples = 2048;   // 1012 frames stereo
constexpr uint32_t kMusicBlock = 1024;
// Blocks: 256 KB, 8 s of music -- more than a failing read's 5 s of retries
// (feed_music), and the reader never fell more than a few blocks behind on the
// console. The rest of the room above the bank goes to the shape cache.
constexpr uint32_t kMusicRingMost = 256;
constexpr uint32_t kMusicRead = 16;        // blocks read at a time
constexpr uint32_t kBankPiece = kMusicRead * kMusicBlock;  // (the music's buffer serves)
const char* const kBankPath = "CCGC/Scripts/Data/audio/sounds.bank";
const char* const kIndexPath = "CCGC/Scripts/Data/audio/sounds.idx";

// ---- Microsoft ADPCM

const int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
// Every file's (convert_audio.py checks).
const int16_t kCoef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};

struct Format {
    int channels = 0;
    uint32_t rate = 0;
    uint32_t block_align = 0;
    int block_frames = 0;
    uint32_t frames = 0;       // in all (the fact chunk)
    uint32_t data_offset = 0;  // in the file, or the bank
    uint32_t data_size = 0;
};

uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24);
}

bool valid(const Format& f) {
    return (f.channels == 1 || f.channels == 2) && f.rate >= 8000 && f.rate <= kRate && f.block_frames > 2 &&
           f.block_align >= uint32_t(7 * f.channels) &&
           uint32_t(f.block_frames - 2) * uint32_t(f.channels) == (f.block_align - 7 * uint32_t(f.channels)) * 2;
}

// A WAV's RIFF header, up to the data chunk (the first `size` bytes of it).
bool parse(const uint8_t* d, uint32_t size, Format& f) {
    if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return false;
    bool fmt = false;
    for (uint32_t pos = 12; pos + 8 <= size;) {
        uint32_t len = le32(d + pos + 4);
        const uint8_t* body = d + pos + 8;
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && pos + 8 + 32 <= size) {
            if (le16(body) != 2) return false;  // WAVE_FORMAT_ADPCM
            // WAVEFORMATEX (to +16, then cbSize), then the samples a block and
            // the coefficient pairs (the standard ones).
            f.channels = le16(body + 2);
            f.rate = le32(body + 4);
            f.block_align = le16(body + 12);
            f.block_frames = le16(body + 18);
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
    return fmt && f.data_offset && valid(f);
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
        int predicted = (s1[c] * kCoef[pred[c]][0] + s2[c] * kCoef[pred[c]][1]) >> 8;
        int sample = std::clamp(predicted + (nib >= 8 ? nib - 16 : nib) * delta[c], -32768, 32767);
        s2[c] = s1[c];
        s1[c] = sample;
        delta[c] = std::max(16, (kAdapt[nib] * delta[c]) >> 8);
        *o++ = int16_t(sample);
    }
}

// ---- the effects' bank, in ARAM

struct Entry {
    Format fmt;  // data_offset: in the bank
    uint32_t bytes = 0;  // 32-byte aligned
};

std::vector<Entry> g_bank;
std::map<std::string, int> g_bank_names;
uint32_t g_bank_at = 0, g_bank_size = 0;  // ARAM, bytes (to read)
volatile uint32_t g_bank_loaded = 0;      // bytes read into ARAM so far

bool load_index() {
    char* data = nullptr;
    uint32_t size = 0;
    SYS_AcquireFileData(kIndexPath, true, 0, data, size);
    if (!data) {
        PpgcLog("audio: no %s", kIndexPath);
        return false;
    }
    // Each effect: its name, then data offset, data size, rate, channels, block
    // align, block frames and frames (tools/convert_audio.py).
    const std::string text(data, size);
    SYS_ReleaseFileData(data);
    const char* p = text.data();
    const char* end = p + text.size();
    std::string name;
    Entry e;
    uint32_t channels = 0, block_frames = 0;
    while (words::next(p, end, name) && words::number(p, end, e.fmt.data_offset) &&
           words::number(p, end, e.fmt.data_size) && words::number(p, end, e.fmt.rate) &&
           words::number(p, end, channels) && words::number(p, end, e.fmt.block_align) &&
           words::number(p, end, block_frames) && words::number(p, end, e.fmt.frames)) {
        e.fmt.channels = int(channels);
        e.fmt.block_frames = int(block_frames);
        e.bytes = (e.fmt.data_size + 31) & ~31u;
        if (!valid(e.fmt) || e.fmt.block_frames * e.fmt.channels > kEffectSamples || e.fmt.block_align > kWindow) {
            PpgcLog("audio: can't play effect %s", name.c_str());
            continue;
        }
        g_bank_names[name] = int(g_bank.size());
        g_bank.push_back(e);
        g_bank_size = std::max(g_bank_size, e.fmt.data_offset + e.bytes);
    }
    return !g_bank.empty();
}

// Effect slots: which entry of the bank.
int g_effects[kEffects];

// ---- voices

struct Voice {
    bool active = false;
    bool music = false;
    bool loop = false;
    int entry = -1;  // an effect's, in the bank
    int slot = -1;   // and its slot
    float left = 1, right = 1;  // gains
    // Decoding.
    const Format* fmt = nullptr;
    uint32_t block = 0;   // the next to decode
    uint32_t blocks = 0;  // in all
    uint32_t frames_left = 0;
    int16_t* pcm = nullptr;  // a block, decoded (channels interleaved)
    int pcm_pos = 0, pcm_count = 0;  // frames
    // Resampling: source frames an output frame (16.16), where between `cur`
    // and `next` the output is, and whether `next` is the last.
    uint32_t step = 1 << 16, frac = 0;
    int16_t cur[2] = {}, next[2] = {};
    bool ending = false;
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

void start_voice(Voice& v, const Format& f, bool loop) {
    v.fmt = &f;
    v.loop = loop;
    v.block = 0;
    v.frames_left = f.frames;
    v.pcm_pos = v.pcm_count = 0;
    v.step = (f.rate << 16) / kRate;
    v.frac = 2 << 16;  // the first two frames are read before the first is played
    v.cur[0] = v.cur[1] = v.next[0] = v.next[1] = 0;
    v.ending = false;
    v.window_block = UINT32_MAX;
    v.window_blocks = 0;
}

// ---- music, from the disc

struct Track {
    bool loaded = false;
    std::string path;
    Format fmt;
};

Track g_tracks[kMusic];
int g_track = -1;              // playing
uint32_t g_generation = 0;     // bumped when the track changes
// The music's ring, in ARAM. It was 2 s in main memory, and a level drawing
// for longer than a frame (level 30's busiest parts) left the reader, below
// the main thread, too little time to keep it filled: the music cut in and
// out. 8 s outlasts any such stretch, and costs the CPU nothing (a
// 1 KB DMA a block).
uint32_t g_ring_at = 0, g_ring_blocks = 0;   // ARAM address; size in blocks
uint8_t* g_ring_block = nullptr;              // the block being decoded
uint32_t g_ring_read = 0, g_ring_write = 0;  // blocks, counting up
uint32_t g_read_block = 0;     // the next block of the file to read
bool g_read_end = false;       // not looping, and all read
sem_t g_reader_sem = LWP_SEM_NULL;

// ---- the reader: the music first, then the bank, else it waits

// One read of the music, if its ring has room; false if there was nothing to do.
bool feed_music(uint8_t* chunk) {
    std::string path;
    uint32_t generation, first, count, offset;
    {
        Lock lock;
        if (g_track < 0 || g_read_end) return false;
        uint32_t room = g_ring_blocks - (g_ring_write - g_ring_read);
        if (room < kMusicRead) return false;
        const Track& t = g_tracks[g_track];
        uint32_t blocks = t.fmt.data_size / t.fmt.block_align;
        if (g_read_block >= blocks) {
            if (!g_music_voice->loop) {
                g_read_end = true;
                return false;
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
    // A failed read is tried again (the console's disc reads fail now and then:
    // ending the track on one left a level silent); only after many in a row
    // (5 s of them; the ring holds far more) is the track given up.
    static uint32_t failures = 0;
    if (!ok) {
        failures++;
        PpgcLog("audio: reading %s at %u failed (%u in a row)", path.c_str(), unsigned(offset), unsigned(failures));
        if (failures < 100) {
            usleep(50 * 1000);
            return true;
        }
    }
    Lock lock;
    if (generation != g_generation) {  // another track now
        failures = 0;
        return true;
    }
    if (!ok) {
        failures = 0;
        g_read_end = true;
        return true;
    }
    failures = 0;
    for (uint32_t i = 0; i < count; i++)
        aram::to_aram(chunk + i * kMusicBlock, g_ring_at + ((g_ring_write + i) % g_ring_blocks) * kMusicBlock,
                      kMusicBlock);
    g_ring_write += count;
    g_read_block = first + count;
    return true;
}

// The next piece of the bank into ARAM; false when it is all there.
bool feed_bank(uint8_t* piece) {
    static uint32_t started = 0;
    uint32_t at = g_bank_loaded;
    if (at >= g_bank_size) return false;
    if (at == 0) started = trace::now_ms();
    uint32_t n = std::min(kBankPiece, g_bank_size - at);
    trace::at(trace::kReader, "reading the effects bank");
    if (!SYS_ReadFileRange(kBankPath, true, at, n, reinterpret_cast<char*>(piece))) {
        PpgcLog("audio: reading %s at %u failed; the effects after it are silent", kBankPath, unsigned(at));
        g_bank_size = at;
        return false;
    }
    aram::to_aram(piece, g_bank_at + at, n);
    g_bank_loaded = at + n;
    if (g_bank_loaded == g_bank_size)
        PpgcLog("audio: effects bank in ARAM, %u KB in %u ms", unsigned(g_bank_size / 1024),
                unsigned(trace::now_ms() - started));
    return true;
}

// Octave's SD driver: this thread's card reads then run above the main thread,
// so a busy frame can't starve one mid-transfer (SdGeckoDma.c).
extern "C" void OctSd_NoteThreadPriority(u32 prio);

void* reader_main(void*) {
    OctSd_NoteThreadPriority(50);  // (as created in init)
    static uint8_t chunk[kMusicRead * kMusicBlock] __attribute__((aligned(32)));
    for (;;) {
        if (feed_music(chunk)) continue;
        if (feed_bank(chunk)) continue;
        trace::at(trace::kReader, "waiting");
        LWP_SemWait(g_reader_sem);
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
        aram::from_aram(g_ring_block, g_ring_at + (g_ring_read % g_ring_blocks) * kMusicBlock, kMusicBlock);
        data = g_ring_block;
    } else {
        if (v.block < v.window_block || v.block >= v.window_block + v.window_blocks) {
            const Entry& e = g_bank[v.entry];
            uint32_t offset = v.block * f.block_align;
            uint32_t len = std::min(kWindow, e.bytes - offset);
            aram::from_aram(v.window, g_bank_at + f.data_offset + offset, (len + 31) & ~31u);
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

// The voice's next frame (both channels); false at its end, or starved.
bool next_frame(Voice& v, int16_t* frame, bool& starved) {
    if (v.pcm_pos >= v.pcm_count && !next_block(v, starved)) return false;
    const int16_t* s = v.pcm + v.pcm_pos * v.fmt->channels;
    frame[0] = s[0];
    frame[1] = s[v.fmt->channels - 1];
    v.pcm_pos++;
    return true;
}

int32_t g_mix[kMixFrames * 2];

void end_voice(Voice& v) {
    v.active = false;
    v.entry = v.slot = -1;
}

// One voice into the mix; false if it wants its reader woken.
bool mix_voice(Voice& v) {
    int32_t gl = int32_t(v.left * 256.0f), gr = int32_t(v.right * 256.0f);
    bool read = false;
    for (int n = 0; n < kMixFrames; n++) {
        while (v.frac >= (1u << 16)) {
            if (v.ending) {
                end_voice(v);
                return read;
            }
            int16_t frame[2];
            bool starved = false;
            if (!next_frame(v, frame, starved)) {
                if (starved) return read;  // the music: wait for the disc
                v.ending = true;           // one more step, to the last frame
                frame[0] = v.next[0];
                frame[1] = v.next[1];
            }
            if (v.music && v.pcm_pos == 1) read = true;  // a block taken from the ring
            v.cur[0] = v.next[0];
            v.cur[1] = v.next[1];
            v.next[0] = frame[0];
            v.next[1] = frame[1];
            v.frac -= 1u << 16;
        }
        int32_t t = int32_t(v.frac);
        int32_t l = v.cur[0] + (((v.next[0] - v.cur[0]) * t) >> 16);
        int32_t r = v.cur[1] + (((v.next[1] - v.cur[1]) * t) >> 16);
        g_mix[n * 2] += l * gl;
        g_mix[n * 2 + 1] += r * gr;
        v.frac += v.step;
    }
    return read;
}

void mix(int16_t* out) {
    std::memset(g_mix, 0, sizeof(g_mix));
    trace::at(trace::kMixer, "taking the lock");
    Lock lock;
    trace::at(trace::kMixer, "mixing");
    bool wake_reader = false;
    for (int i = 0; i <= kVoices; i++)
        if (g_voices[i].active && mix_voice(g_voices[i])) wake_reader = true;
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

class GcEngine : public audio::Engine {
public:
    bool init() override {
        if (!aram::init()) return false;
        std::fill(std::begin(g_effects), std::end(g_effects), -1);
        if (load_index()) {
            // The bank at the bottom of ARAM; the renderer's cache is at the top.
            g_bank_at = aram::base();
            uint32_t room = aram::top() - aram::kShapeCache - g_bank_at;
            if (g_bank_size > room) {
                PpgcLog("audio: the effects bank (%u KB) is over ARAM's room (%u KB); the effects past it are silent",
                        unsigned(g_bank_size / 1024), unsigned(room / 1024));
                g_bank_size = room & ~31u;
            }
        }
        // The music's ring above the bank, in what is left below the cache.
        g_ring_at = (aram::base() + g_bank_size + 31u) & ~31u;
        g_ring_blocks = std::min(kMusicRingMost, (aram::top() - aram::kShapeCache - g_ring_at) / kMusicBlock);
        g_ring_blocks -= g_ring_blocks % kMusicRead;
        if (g_ring_blocks < 2 * kMusicRead) {
            PpgcLog("audio: no room in ARAM for the music (%u blocks)", unsigned(g_ring_blocks));
            return false;
        }
        PpgcLog("audio: the music's ring in ARAM, %u KB (%u s)", unsigned(g_ring_blocks * kMusicBlock / 1024),
                unsigned(g_ring_blocks / 32));
        g_voices = static_cast<Voice*>(memalign(32, sizeof(Voice) * (kVoices + 1)));
        g_ring_block = static_cast<uint8_t*>(memalign(32, kMusicBlock));
        for (int16_t*& b : g_out) b = static_cast<int16_t*>(memalign(32, kMixFrames * 4));
        if (!g_voices || !g_ring_block || !g_out[kBuffers - 1]) return false;
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
        // the sound; the reader below it (as Octave's audio reader, 50): the
        // music's ring is long enough to wait for the main thread's spare time.
        static uint8_t mixer_stack[16 * 1024] __attribute__((aligned(32)));
        static uint8_t reader_stack[64 * 1024] __attribute__((aligned(32)));
        lwp_t thread;
        if (LWP_CreateThread(&thread, reader_main, nullptr, reader_stack, sizeof(reader_stack), 50) != 0) return false;
        if (LWP_CreateThread(&thread, mixer_main, nullptr, mixer_stack, sizeof(mixer_stack), 80) != 0) return false;
        PpgcLog("audio: %u effects in a bank of %u KB, being read into ARAM", unsigned(g_bank.size()),
                unsigned(g_bank_size / 1024));
        return true;
    }

    bool load_effect(uint16_t slot, const std::filesystem::path& path) override {
        auto it = g_bank_names.find(path.filename().string());
        if (it == g_bank_names.end()) {
            PpgcLog("audio: no effect %s", path.filename().string().c_str());
            return false;
        }
        g_effects[slot] = it->second;
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
        PpgcLog("audio: music %d is %s (%u s)", id, t.path.c_str(), unsigned(t.fmt.frames / t.fmt.rate));
        t.loaded = true;
        return true;
    }

    int free_effect_slot() const override {
        for (int i = 0; i < kEffects; i++)
            if (g_effects[i] < 0) return i;
        return -1;
    }

    bool has_music(int id) const override { return g_tracks[id].loaded; }

    void unload_effect(uint16_t slot) override {
        trace::at(trace::kMain, "unload effect; taking the lock");
        Lock lock;
        // Its voices stop, as the PC's; the bank stays.
        for (int i = 0; i < kVoices; i++)
            if (g_voices[i].active && g_voices[i].slot == slot) end_voice(g_voices[i]);
        g_effects[slot] = -1;
    }

    void unload_music(int id) override {
        trace::at(trace::kMain, "unload music; taking the lock");
        Lock lock;
        if (g_track == id) stop_track();
        g_tracks[id] = Track{};
    }

    int16_t play_effect(uint16_t slot, float volume, float pan, bool loop) override {
        int entry = g_effects[slot];
        if (entry < 0) return -1;
        const Entry& e = g_bank[entry];
        if (e.fmt.data_offset + e.bytes > g_bank_loaded) {  // not read in yet
            static uint32_t early = 0;
            if (early++ == 0) PpgcLog("audio: mismatch: an effect played before the bank was in (silent)");
            return -1;
        }
        Lock lock;
        for (int i = 0; i < kVoices; i++) {
            Voice& v = g_voices[i];
            if (v.active) continue;
            v.entry = entry;
            v.slot = slot;
            start_voice(v, e.fmt, loop);
            v.blocks = e.fmt.data_size / e.fmt.block_align;
            gains(v, volume, pan);
            v.active = true;
            return int16_t(i);
        }
        static uint32_t busy = 0;
        if (busy++ == 0) PpgcLog("audio: mismatch: all %d voices busy (an effect not played)", kVoices);
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
            start_voice(v, t.fmt, loop);
            v.blocks = UINT32_MAX;  // (the reader knows)
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

// For the status line: buffers mixed so far, voices playing, the effects
// bank read into ARAM (KB), and the music's blocks read ahead.
void stats(uint32_t& mixed, uint32_t& voices, uint32_t& effects_kb, uint32_t& music_ahead) {
    mixed = g_mixed;
    voices = 0;
    effects_kb = g_bank_loaded / 1024;
    music_ahead = 0;
    if (!g_voices) return;
    Lock lock;
    for (int i = 0; i <= kVoices; i++) voices += g_voices[i].active;
    music_ahead = g_ring_write - g_ring_read;
}

}  // namespace audio_gc
