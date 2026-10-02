#include "CastleGame.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <new>

#include "audio/audio.h"
#include "input/input.h"
#include "memory_gc.h"
#include "menu/main_menu.h"
#include "player/game.h"
#include "player/player.h"
#include "render/renderer.h"
#include "save/storage.h"
#include "swf/types.h"
#include "text/fonts.h"
#include "text/layout.h"

#include "trace_gc.h"
#include "System/System.h"
#ifdef CASTLE_REPLAY
#include "replay_gc.h"
#include "common/files.h"
#endif

namespace audio_gc {
std::unique_ptr<audio::Engine> make_engine();  // audio_gc.cpp
void stats(uint32_t& mixed, uint32_t& voices, uint32_t& effects_kb, uint32_t& music_ahead);
}

namespace render {
void gx_memory(uint32_t& shape_bytes, uint32_t& texture_bytes, uint32_t& aram_bytes, uint32_t& put_off);  // renderer_gx.cpp
void gx_mismatch_log();
int gx_mask_mode();
void gx_set_mask_mode(int mode);
void gx_flicker_copy(int efb_w, int efb_h, bool ticked);
void gx_set_scene(const char* name);
void gx_diag_set_skip(int skip);
void gx_diag_log(const char* label);
}

#ifdef CASTLE_REPLAY
// Replay builds: once past the fast-forward (into the part to time), kinds
// of drawing left out in turn, 15 seconds each, each stretch logged with what
// its frames drew; the perf and GPU lines logged meanwhile time it. On the
// console, where the GPU's speed is real (Dolphin's isn't).
static void DiagCycle()
{
    struct Phase { const char* name; int skip; int maskMode; };
    static const Phase kPhases[] = {
        {"everything", 0, 0}, {"no culling", 8, 0}, {"no bitmaps", 1, 0}, {"no shapes", 2, 0},
        {"no masks", 0, 2}, {"no text", 4, 0}, {"everything again", 0, 0},
    };
    constexpr int kCount = int(sizeof(kPhases) / sizeof(kPhases[0]));
    constexpr uint32_t kTicks = 450;
    static int phase = -1;
    static uint32_t ticks = 0;
    if (phase >= kCount || !replay::fast_forward() || replay::updates() < replay::fast_forward())
    {
        return;
    }
    if (phase >= 0 && ++ticks < kTicks)
    {
        return;
    }
    ticks = 0;
    if (phase >= 0)
    {
        render::gx_diag_log(kPhases[phase].name);
    }
    else
    {
        render::gx_diag_log("(before)");  // (resets the counts)
    }
    phase++;
    if (phase >= kCount)
    {
        render::gx_diag_set_skip(0);
        render::gx_set_mask_mode(0);
        PpgcLog("diag: done");
        return;
    }
    render::gx_diag_set_skip(kPhases[phase].skip);
    render::gx_set_mask_mode(kPhases[phase].maskMode);
    PpgcLog("diag: now %s, for %u ticks", kPhases[phase].name, unsigned(kTicks));
}
#endif

// Where the packager puts CCGC/Scripts/ inside the disc image; the data is the
// Castle-Crashers-Recomp repository's assets/ (see tools/copy_data.py).
static const char* kDataRoot = "CCGC/Scripts/Data";

// The stage, twips (as the engine's main.cpp).
static const swf::Rect kStage{0, 16960, 0, 9600};

// castle.exe runs 30 ticks a second, and slows down rather than skipping.
static constexpr float kTickSeconds = 1.0f / 30.0f;

// Octave's SYS_GetTimeMicroseconds (gettimeofday) is too coarse to time a tick.
static uint64_t NowUs()
{
    return ticks_to_microsecs(gettime());
}

static uint32_t FreeMemoryKb()
{
    // Free blocks inside the heap, plus the part of MEM1 the heap hasn't grown into yet.
    const struct mallinfo info = mallinfo();
    const uint32_t unclaimed = uint32_t((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo());
    return (uint32_t(info.fordblks) + unclaimed) / 1024;
}

// The pads, as PAD_ScanPads would read them (nothing calls it): a port
// with no controller is reset, or one plugged in again is never seen; and
// a read that failed on the way keeps the reading before.
static void ReadPadStatus(PADStatus (&pads)[PAD_CHANMAX])
{
    static PADStatus last[PAD_CHANMAX] = {};
    PAD_Read(pads);
    uint32_t reset = 0;
    for (int i = 0; i < PAD_CHANMAX; i++)
    {
        if (pads[i].err == PAD_ERR_NO_CONTROLLER)
        {
            reset |= PAD_CHAN0_BIT >> i;
        }
        else if (pads[i].err == PAD_ERR_TRANSFER || pads[i].err == PAD_ERR_NOT_READY)
        {
            pads[i] = last[i];
        }
        last[i] = pads[i];
    }
    if (reset)
    {
        PAD_Reset(reset);
    }
}

// ---- The Save / Load page
//
// Online Multiplayer has no place on the GameCube: the main menu's item is
// "Save / Load" instead, a page of the game's own menus (after its 41) with
// the card in slot A, A to save and X to load.

static CastleGame* sGame = nullptr;

enum StringId
{
    kMainMenuOnline = 0x183,  // "Online Multiplayer"
    kCardTitle = 60000,
    kCardStatus,
    kCardSaving,
    kCardSave,
    kCardLoad,
};

static constexpr int kCardPage = 41;
static menu::TextItem* sCardLines[2] = {};

static std::string CardStatusText(const card::Status& c)
{
    char line[128];
    switch (c.state)
    {
    case card::State::Exists: return "A Castle Crashers save is on the card.";
    case card::State::Ready:
        snprintf(line, sizeof(line), "No save. It needs %d block%s; %d free.", c.blocks_needed,
            c.blocks_needed == 1 ? "" : "s", c.blocks_free);
        return line;
    case card::State::Full:
        snprintf(line, sizeof(line), "Not enough free blocks: %d needed, %d free.", c.blocks_needed, c.blocks_free);
        return line;
    case card::State::NoCard: return "There is no Memory Card in Slot A.";
    default: return "The Memory Card in Slot A can't be used.";
    }
}

// The page's lines, from the card as it is now.
static void RefreshCardPage(const char* result = nullptr)
{
    text::set_string(kCardStatus, CardStatusText(card::query()));
    text::set_string(kCardSaving, result ? result
        : sGame && sGame->IsSaving() ? "Progress is saved to the card." : "Progress is not being saved.");
    if (sCardLines[0]) sCardLines[0]->set_text_id(kCardStatus);
    if (sCardLines[1]) sCardLines[1]->set_text_id(kCardSaving);
}

static void BuildCardPage(menu::MainMenu& m)
{
    menu::Page& p = m.page(kCardPage);
    m.text(p, kCardTitle);
    sCardLines[0] = m.text(p, kCardStatus);
    sCardLines[1] = m.text(p, kCardSaving);
    m.text(p, 0);
    p.ctx = &m;
    p.on_accept = [](menu::BaseMenu&) { RefreshCardPage(sGame && sGame->SaveNow() ? "Saved." : "Could not save."); };
    m.button(p, 3)->set_text_id(kCardSave);
    p.ctx = &m;
    p.on_x = [](menu::BaseMenu&) { RefreshCardPage(sGame && sGame->LoadNow() ? "Loaded." : "Could not load."); };
    m.button(p, 2)->set_text_id(kCardLoad);
    p.ctx = &m;
    p.on_back = [](menu::BaseMenu& b) { b.set_page(b.current->return_page); };
    p.return_page = 18;
    m.button(p, 0);
}

static void OpenCardPage(menu::BaseMenu& m)
{
    RefreshCardPage();
    m.set_page(kCardPage);
}

CastleGame::CastleGame() = default;
CastleGame::~CastleGame() = default;

bool CastleGame::Initialize()
{
    sGame = this;
    trace::start();
    PpgcLog("castle: %u KB free before loading", FreeMemoryKb());

    if (!text::load(kDataRoot, "en"))
    {
        PpgcLog("castle: fonts or strings missing in %s", kDataRoot);
    }
    text::set_string(kMainMenuOnline, "Save / Load");
    text::set_string(kCardTitle, "Memory Card in Slot A");
    text::set_string(kCardSave, "Save");
    text::set_string(kCardLoad, "Load");
    menu::MainMenu::platform_pages = 1;
    menu::MainMenu::build_platform_pages = BuildCardPage;
    menu::MainMenu::on_online = OpenCardPage;

    mRenderer = std::make_unique<render::Renderer>();
    mRenderer->init();
    // The sound (audio_gc.cpp): the menus' two effects load now.
    audio::Manager::make_engine = audio_gc::make_engine;
    audio::manager().init(std::string(kDataRoot) + "/audio");
    card::init();
    return true;
}

// ---- Boot: slot A
//
// A save on the card is loaded. With none, the player is asked whether to
// make one; with no room for one, no card, or a card that can't be used,
// they are told, and can try again or play without saving.

void CastleGame::UpdateBoot()
{
    if (mBoot == Boot::Check)
    {
#ifdef CASTLE_REPLAY
        // Test builds (make REPLAY=1): a recorded session played again, from
        // the save it began with; the Memory Card isn't touched.
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (replay::open())
            {
                mSaving = false;
                mSaveBytes.clear();
                StartGame();
                return;
            }
        }
#endif
        mCard = card::query();
        mPrompt.clear();
        char line[128];
        switch (mCard.state)
        {
        case card::State::Exists:
            if (card::read(mSaveBytes))
            {
                mSaving = true;
                StartGame();
                return;
            }
            mPrompt = {"The save on the Memory Card in Slot A", "could not be read.", "Progress will not be saved.",
                "A  Continue      B  Try again"};
            break;
        case card::State::Ready:
            snprintf(line, sizeof(line), "Create one? (%d block%s)", mCard.blocks_needed,
                mCard.blocks_needed == 1 ? "" : "s");
            mPrompt = {"There is no Castle Crashers save", "on the Memory Card in Slot A.", line, "A  Yes      B  No"};
            break;
        case card::State::Full:
            snprintf(line, sizeof(line), "free blocks to save (%d needed, %d free).", mCard.blocks_needed,
                mCard.blocks_free);
            mPrompt = {"The Memory Card in Slot A doesn't have enough", line, "Progress will not be saved.",
                "A  Continue      B  Try again"};
            break;
        case card::State::NoCard:
            mPrompt = {"There is no Memory Card in Slot A.", "Progress will not be saved.",
                "A  Continue      B  Try again"};
            break;
        case card::State::Unusable:
            mPrompt = {"The Memory Card in Slot A can't be used.", "(It may be damaged or unformatted.)",
                "Progress will not be saved.", "A  Continue      B  Try again"};
            break;
        }
        mBoot = Boot::Prompt;
        return;
    }

    if (mBoot == Boot::Prompt)
    {
        // Pad 1's new presses (PAD_ButtonsDown needs PAD_ScanPads, which
        // nothing calls: read the pad as the game does).
        static uint16_t held = 0xffff;  // nothing counts until it is let go once
        PADStatus pads[PAD_CHANMAX];
        ReadPadStatus(pads);
        uint16_t now = pads[0].err == PAD_ERR_NONE ? pads[0].button : 0;
        uint16_t down = now & ~held;
        held = now;
#if defined(CASTLE_AUTOPRESS) || defined(CASTLE_CARDTEST)
        down |= PAD_BUTTON_A;  // test builds answer yes / continue
#endif
        bool ready = mCard.state == card::State::Ready;
        if (down & PAD_BUTTON_A)
        {
            mSaving = ready;
            mCreateSave = ready;
            StartGame();
        }
        else if (down & PAD_BUTTON_B)
        {
            if (ready)
            {
                StartGame();  // no save made, none kept
            }
            else
            {
                mBoot = Boot::Check;
            }
        }
    }
}

void CastleGame::StartGame()
{
    trace::at(trace::kMain, "start game");
    mGame = std::make_unique<player::Game>(std::filesystem::path(kDataRoot) / "swf");
    // TESTING, files beside the data, each a number (not on a normal disc):
    // level.txt -- the first level the game loads is that one instead
    // (player::Game::boot_level); max.txt -- that character maxed in the save
    // (player::Game::max_character; 2 the red knight).
    // (Asked of the disc itself: files::exists knows only files.txt's list.)
    auto test_number = [](const char* name) -> int
    {
        const std::string path = std::string(kDataRoot) + "/" + name;
        char* data = nullptr;
        uint32_t size = 0;
        {
            trace::SdLock lock;         // (as files::exists)
            if (!SYS_DoesFileExist(path.c_str(), true)) return 0;
        }
        SYS_AcquireFileData(path.c_str(), true, 64, data, size);
        if (!data) return 0;
        std::string text(data, size);
        SYS_ReleaseFileData(data);
        return std::atoi(text.c_str());
    };
    mGame->boot_level = test_number("level.txt");
    mGame->max_character = test_number("max.txt");
    if (mGame->boot_level || mGame->max_character)
        PpgcLog("castle: testing -- boot level %d, maxed character %d", mGame->boot_level, mGame->max_character);
    // The save: the one read from the card, and written back to it while
    // saving is on.
    mGame->read_save_data = [this](std::vector<uint8_t>& bytes) {
        bytes = mSaveBytes;
        return !bytes.empty();
    };
    mGame->write_save_data = [this](const std::vector<uint8_t>& bytes) {
        if (mSaving)
        {
            trace::at(trace::kMain, "autosave to card");
            PpgcLog("castle: autosave, %u bytes", unsigned(bytes.size()));
            card::write(bytes);
        }
    };
#ifdef CASTLE_REPLAY
    // As `castle.exe --replay` (engine/main.cpp): the session's gamer tag and
    // DLC, its changes before any update, a fresh start, then its save if it
    // differs from a fresh one; each update then applies its changes.
    const bool replaying = replay::active();
    if (replaying)
    {
        const std::vector<uint8_t>& captured = replay::storage();
        mGame->gamer_tag = replay::gamer_tag();
        mGame->captured_storage = captured;
        if (captured.size() > 0x5b0)
        {
            mGame->dlc = (captured[0x580] & 0x80 ? 1u : 0u) | (captured[0x5b0] & 0x80 ? 2u : 0u);
        }
        replay::start(*mGame);
    }
#endif
    uint64_t start = NowUs();
    mGame->start("");
    PpgcLog("castle: started in %u ms, %u KB free", unsigned((NowUs() - start) / 1000), FreeMemoryKb());
#ifdef CASTLE_REPLAY
    if (replaying)
    {
        const std::vector<uint8_t>& captured = replay::storage();
        const std::vector<uint8_t>& ours = mGame->storage.bytes();
        size_t differ = 0;
        for (size_t i = 0; i < ours.size(); i++)
        {
            differ += i >= captured.size() || ours[i] != captured[i];
        }
        if (differ && captured.size() >= save::Storage::kSize)
        {
            mGame->saved = captured;
            mGame->storage.assign(captured);
            mGame->storage.sanitize();
        }
        PpgcLog("replay: save %u of %u bytes differ from a fresh one%s", unsigned(differ), unsigned(ours.size()),
            differ ? ", so it is loaded" : "");
        player::Player::on_updated = [this](player::Player& p) { replay::on_update(*mGame, p); };
    }
#endif
    if (mCreateSave)
    {
        mGame->saved = mGame->storage.bytes();
        card::write(mGame->saved);
        mCreateSave = false;
    }
    mSaveBytes.clear();
    mBoot = Boot::Running;
}

bool CastleGame::IsQuitting() const
{
    return mGame && mGame->quitting();
}

bool CastleGame::SaveNow()
{
    trace::at(trace::kMain, "save to card");
#ifdef CASTLE_REPLAY
    return false;  // (test builds leave the card alone)
#endif
    if (!mGame)
    {
        return false;
    }
    card::Status c = card::query();
    if (c.state != card::State::Exists && c.state != card::State::Ready)
    {
        return false;
    }
    mGame->saved = mGame->storage.bytes();
    if (!card::write(mGame->saved))
    {
        return false;
    }
    mSaving = true;
    return true;
}

bool CastleGame::LoadNow()
{
    trace::at(trace::kMain, "load from card");
#ifdef CASTLE_REPLAY
    return false;  // (test builds leave the card alone)
#endif
    std::vector<uint8_t> bytes;
    if (!mGame || !card::read(bytes) || bytes.size() < save::Storage::kSize)
    {
        return false;
    }
    bytes.resize(save::Storage::kSize);
    mGame->saved = bytes;
    mGame->read_save(0);  // as signing in loads it
    mSaving = true;
    return true;
}

// The question, in the game's lettering, centred on a black screen.
void CastleGame::RenderPrompt()
{
    text::Font* font = text::game_font();
    if (!font || mPrompt.empty())
    {
        return;
    }
    const int lineTwips = 600;
    int top = (kStage.ymax - int(mPrompt.size()) * lineTwips) / 2;
    for (size_t i = 0; i < mPrompt.size(); i++)
    {
        swf::EditTextCharacter field;
        field.bounds = swf::Rect{0, kStage.xmax, top + int(i) * lineTwips, top + int(i + 1) * lineTwips};
        field.font_height = 440;
        field.align = 2;  // centred
        auto quads = text::layout(*font, field, mPrompt[i]);
        bool buttons = i + 1 == mPrompt.size();
        swf::Rgba color = buttons ? swf::Rgba{255, 255, 0, 255} : swf::Rgba{255, 255, 255, 255};
        mRenderer->draw_text(*font, quads, color, swf::Matrix{}, swf::CXform{});
    }
}

void CastleGame::ReadPads()
{
    // GameCube pads as XInput pads (engine/main.cpp read_gamepads): the face
    // buttons by position, Z as the right shoulder, the triggers analog.
    PADStatus status[PAD_CHANMAX];
    ReadPadStatus(status);

    // L + R held and D-pad up pressed: the next way of drawing masks (for
    // finding what differs on hardware; it shows on the status line).
    // Diagnostic builds only: in a game it was pressed by accident, in a fight
    // (triggers and the d-pad are both in use), and masks went off unseen --
    // health bars drawn whole, a boss's bar never going down.
#ifdef PPGC_DIAG
    {
        uint16_t now = status[0].err == PAD_ERR_NONE ? status[0].button : 0;
        uint16_t down = now & ~mComboHeld;
        mComboHeld = now;
        if ((now & PAD_TRIGGER_L) && (now & PAD_TRIGGER_R) && (down & PAD_BUTTON_UP))
        {
            render::gx_set_mask_mode((render::gx_mask_mode() + 1) % 3);
        }
    }
#endif

    for (int i = 0; i < 4; i++)
    {
        input::PadReading r;
        const PADStatus& s = status[i];
        if (s.err == PAD_ERR_NONE)
        {
            r.connected = true;
            struct { uint16_t gc; uint16_t x; } map[] = {
                {PAD_BUTTON_UP, 0x1}, {PAD_BUTTON_DOWN, 0x2}, {PAD_BUTTON_LEFT, 0x4}, {PAD_BUTTON_RIGHT, 0x8},
                {PAD_BUTTON_START, 0x10}, {PAD_TRIGGER_Z, 0x200},
                {PAD_BUTTON_A, 0x1000}, {PAD_BUTTON_B, 0x2000}, {PAD_BUTTON_X, 0x4000}, {PAD_BUTTON_Y, 0x8000},
            };
            for (auto& m : map)
            {
                if (s.button & m.gc)
                {
                    r.buttons |= m.x;
                }
            }
            r.left_trigger = s.triggerL;
            r.right_trigger = s.triggerR;
            // The GameCube stick reaches about +-100; XInput's +-32767.
            r.thumb_lx = int16_t(std::clamp(int(s.stickX) * 327, -32768, 32767));
            r.thumb_ly = int16_t(std::clamp(int(s.stickY) * 327, -32768, 32767));
        }
#ifdef CASTLE_REPLAY
        if (replay::active())
        {
            continue;  // the session's pads, until it runs out
        }
#endif
        mGame->input.pads[i] = r;
    }

#ifdef CASTLE_CARDTEST
    // Test builds (make CARDTEST=1): on the title menu, down to Save / Load,
    // open it, save, load, and back out.
    {
        static uint32_t t = 0;
        t++;
        input::PadReading& p = mGame->input.pads[0];
        p.connected = true;
        struct { uint32_t at; uint16_t buttons; } steps[] = {
            {900, 0x2}, {960, 0x1000}, {1020, 0x1000}, {1080, 0x4000}, {1140, 0x2000},
        };
        for (auto& s : steps)
        {
            if (t >= s.at && t < s.at + 2)
            {
                p.buttons |= s.buttons;
            }
        }
        if (t % 60 == 0 && mGame->active_controller())
        {
            PpgcLog("cardtest: tick %u page %d", t, int(mGame->active_controller()->current_index));
        }
    }
#endif

#ifdef CASTLE_AUTOPRESS
    // Test builds (make AUTOPRESS=N): pad 0 presses A every N ticks once the
    // menu is up, Start with every other press, to walk into the game; from
    // tick 1700 (in the first level) it walks right and attacks (X) instead.
    static uint32_t tick = 0;
    tick++;
    input::PadReading& p = mGame->input.pads[0];
    p.connected = true;
    if (tick > 800 && tick < 1700 && tick % CASTLE_AUTOPRESS < 2)
    {
        p.buttons |= (tick / CASTLE_AUTOPRESS) % 2 ? 0x1000 : 0x1010;
    }
    if (tick >= 1700)
    {
        if ((tick / 90) % 3 != 2)
        {
            p.thumb_lx = 32000;
        }
        if (tick % 20 < 2)
        {
            p.buttons |= (tick / 20) % 4 == 3 ? 0x1000 : 0x4000;  // X, and A (resume, jump) now and then
        }
    }
#endif
}

void CastleGame::Update(float deltaTime)
{
    trace::ticked();
    trace::at(trace::kMain, "update");
    if (mBoot != Boot::Running)
    {
        trace::at(trace::kMain, "boot (slot A)");
        UpdateBoot();
        trace::at(trace::kMain, "octave (after update)");
        return;
    }
    if (!mGame)
    {
        return;
    }

    // Once a main-loop iteration, as the PC's (fades, finished voices).
    trace::at(trace::kMain, "audio update");
    audio::manager().update(std::clamp(deltaTime, 1.0f / 60.0f, 0.1f));

    mTickTime += deltaTime;
    if (mTickTime >= kTickSeconds)
    {
        mTickTime -= kTickSeconds;
        if (mTickTime > kTickSeconds)
        {
            mTickTime = 0.0f;  // slow down, don't skip
        }

        trace::at(trace::kMain, "pads");
        ReadPads();
        uint64_t start = NowUs();
        try
        {
            trace::at(trace::kMain, "game tick", mGame->current() ? mGame->current()->name().c_str() : "-");
            mGame->tick();
            mTickedSinceFrame = true;
            TraceChanges();
#ifdef CASTLE_REPLAY
            DiagCycle();
#endif
        }
        catch (const std::bad_alloc&)
        {
            // Without this the abort spins in libogc's exit and the picture freezes.
            PpgcLog("castle: OUT OF MEMORY in a tick, %u KB free", FreeMemoryKb());
            mStatus = "out of memory";
            mGame.reset();
            return;
        }
        uint64_t us = NowUs() - start;
        mPerfTickUs += us;
        mPerfMaxTickUs = std::max(mPerfMaxTickUs, us);
        mPerfTicks++;
#ifdef CASTLE_REPLAY
        // Replaying, before the session's fast-forward update: more ticks this
        // frame (3 at most, while they take under ~25 ms), to reach the part
        // to look at quickly; from there, one a tick as played.
        for (int extra = 0; extra < 3 && mGame && replay::active() && replay::updates() < replay::fast_forward() &&
                            NowUs() - start < 25000;
             extra++)
        {
            try
            {
                trace::at(trace::kMain, "game tick (replay, fast)");
                mGame->tick();
                TraceChanges();
            }
            catch (const std::bad_alloc&)
            {
                PpgcLog("castle: OUT OF MEMORY in a tick, %u KB free", FreeMemoryKb());
                mStatus = "out of memory";
                mGame.reset();
                return;
            }
        }
#endif
    }

    trace::at(trace::kMain, "perf log");
    LogPerformance(deltaTime);
    trace::at(trace::kMain, "octave (after update)");
}

// Movies and menu pages as they change.
void CastleGame::TraceChanges()
{
    player::Player* movie = mGame->current();
    std::string name = movie ? movie->name() : "-";
    menu::BaseMenu* active = mGame->active_controller();
    int page = active && active->current ? int(active->current_index) : -1;
    if (name != mTraceMovie || page != mTracePage)
    {
        PpgcLog("castle: tick %u: %s%s, page %d, %u KB free (%u in one piece)", unsigned(trace::ticks()),
            name.c_str(), mGame->quitting() ? " (quitting)" : "", page, FreeMemoryKb(), memory::largest_free_kb());
        if (name != mTraceMovie) memory::census_log(name.c_str());
        char scene[48];
        snprintf(scene, sizeof(scene), "%s_page%d", name.c_str(), page);
        render::gx_set_scene(scene);
        mTraceMovie = name;
        mTracePage = page;
    }
#ifdef PPGC_DIAG
    // The census in play too, every 10 seconds (diagnostic builds): what a
    // level takes as it goes on (enemies, effects, shapes), not only as it loads.
    if (trace::ticks() % 300 == 0)
    {
        char when[64];
        snprintf(when, sizeof(when), "tick %u in %s (%u KB free)", unsigned(trace::ticks()), name.c_str(), FreeMemoryKb());
        memory::census_log(when);
    }
#endif
    // What holds input, when it changes (every 15 ticks: a string made).
    if (trace::ticks() % 15 == 0)
    {
        std::string input = mGame->input_state();
        if (input != mTraceInput)
        {
            PpgcLog("castle: input: %s", input.c_str());
            mTraceInput = input;
        }
    }
}

void CastleGame::Render(float screenWidth, float screenHeight)
{
    trace::drew();
    trace::at(trace::kMain, "render");
    if (mRenderer && mBoot != Boot::Running)
    {
        mRenderer->begin_frame(int(screenWidth), int(screenHeight), kStage, swf::Rgba{});
        RenderPrompt();
        trace::at(trace::kMain, "octave (after render)");
        return;
    }
    if (!mGame || !mRenderer)
    {
        return;
    }

    uint64_t start = NowUs();
    trace::at(trace::kMain, "begin frame");
    mRenderer->begin_frame(int(screenWidth), int(screenHeight), kStage, swf::Rgba{});
    try
    {
        trace::at(trace::kMain, "game render");
        mGame->render(*mRenderer);
    }
    catch (const std::bad_alloc&)
    {
        PpgcLog("castle: OUT OF MEMORY drawing, %u KB free", FreeMemoryKb());
        mStatus = "out of memory (drawing)";
    }
    // The stage copied off the GPU, for the flicker detector (the status line
    // and Octave's UI come after).
    render::gx_flicker_copy(int(screenWidth), int(screenHeight), mTickedSinceFrame);
    mTickedSinceFrame = false;
    mPerfRenderUs += NowUs() - start;
    mPerfFrames++;
    trace::at(trace::kMain, "octave (after render)");
}

void CastleGame::LogPerformance(float deltaTime)
{
    mPerfTime += deltaTime;
    if (mPerfTime < 2.0f)
    {
        return;
    }

    player::Player* movie = mGame->current();
    uint32_t shapeBytes = 0, textureBytes = 0, aramBytes = 0, putOff = 0;
    render::gx_memory(shapeBytes, textureBytes, aramBytes, putOff);
    char line[256];
    menu::BaseMenu* active = mGame->active_controller();
    char where[64];
    snprintf(where, sizeof(where), "%s%s", movie ? movie->name().c_str() : "-", mGame->quitting() ? " (quitting)" : "");
    if (active && active->current)
    {
        snprintf(where + strlen(where), sizeof(where) - strlen(where), " page %d", int(active->current_index));
    }
    // (its timeline's frame: one that stays put with nothing loading is stuck)
    if (movie && movie->root())
    {
        snprintf(where + strlen(where), sizeof(where) - strlen(where), " frame %d", movie->root()->current_frame());
    }
    snprintf(line, sizeof(line), "%s%s  %.1f ticks/s  tick %.1f ms (max %.1f)  draw %.1f ms  %u KB free (%u in one piece)  small %u KB  shapes %u KB  textures %u KB  aram %u KB  list waits %u  scratch over %u  clips %u  roots %u",
        where, render::gx_mask_mode() == 0 ? "" : render::gx_mask_mode() == 1 ? " [masks: equal]" : " [masks: off]",
        mPerfTicks / mPerfTime,
        mPerfTicks ? double(mPerfTickUs) / mPerfTicks / 1000.0 : 0.0,
        double(mPerfMaxTickUs) / 1000.0,
        mPerfFrames ? double(mPerfRenderUs) / mPerfFrames / 1000.0 : 0.0,
        FreeMemoryKb(), memory::largest_free_kb(), memory::small_kb(), shapeBytes / 1024, textureBytes / 1024, aramBytes / 1024,
        putOff, memory::scratch_overflows(), unsigned(player::live_clips()),
        unsigned(player::live_roots()));
    mStatus = line;
    PpgcLog("castle: perf %s  ticks %u frames %u", line, unsigned(trace::ticks()), unsigned(trace::frames()));
    // While a movie change or loadMovie is under way: what it's at (a level
    // that never came, a black screen, showed nothing else in the log).
    const std::string loading = mGame->loading_state();
    if (!loading.empty())
    {
        PpgcLog("castle: loading: %s", loading.c_str());
    }
    render::gx_mismatch_log();
    uint32_t mixed, voices, effectsKb, musicAhead;
    audio_gc::stats(mixed, voices, effectsKb, musicAhead);
    PpgcLog("castle: audio %u buffers mixed, %u voices, effects %u KB, music %u blocks ahead", unsigned(mixed),
        unsigned(voices), unsigned(effectsKb), unsigned(musicAhead));

    mPerfTime = 0.0f;
    mPerfTicks = 0;
    mPerfFrames = 0;
    mPerfTickUs = 0;
    mPerfRenderUs = 0;
    mPerfMaxTickUs = 0;
}
