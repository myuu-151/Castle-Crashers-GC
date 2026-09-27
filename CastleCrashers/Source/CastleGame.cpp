#include "CastleGame.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "input/input.h"
#include "player/game.h"
#include "render/renderer.h"
#include "swf/types.h"
#include "text/fonts.h"

void OctLog(const char* format, ...);

// Where the packager puts CastleCrashers/Scripts/ inside the disc image; the
// data is the Castle-Crashers repository's assets/ (see tools/copy_data.py).
static const char* kDataRoot = "CastleCrashers/Scripts/Data";

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

CastleGame::CastleGame() = default;
CastleGame::~CastleGame() = default;

bool CastleGame::Initialize()
{
    OctLog("castle: %u KB free before loading", FreeMemoryKb());

    if (!text::load(kDataRoot, "en"))
    {
        OctLog("castle: fonts or strings missing in %s", kDataRoot);
    }

    mRenderer = std::make_unique<render::Renderer>();
    mRenderer->init();

    mGame = std::make_unique<player::Game>(std::filesystem::path(kDataRoot) / "swf");
    uint64_t start = NowUs();
    mGame->start("");
    OctLog("castle: started in %u ms, %u KB free",
        unsigned((NowUs() - start) / 1000), FreeMemoryKb());
    if (mGame->current() == nullptr)
    {
        mStatus = "no movie loaded: is CastleCrashers/Scripts/Data on the disc?";
        return false;
    }
    return true;
}

void CastleGame::ReadPads()
{
    // GameCube pads as XInput pads (engine/main.cpp read_gamepads): the face
    // buttons by position, Z as the right shoulder, the triggers analog.
    PADStatus status[PAD_CHANMAX];
    PAD_Read(status);

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
        mGame->input.pads[i] = r;
    }
}

void CastleGame::Update(float deltaTime)
{
    if (!mGame)
    {
        return;
    }

    mTickTime += deltaTime;
    if (mTickTime >= kTickSeconds)
    {
        mTickTime -= kTickSeconds;
        if (mTickTime > kTickSeconds)
        {
            mTickTime = 0.0f;  // slow down, don't skip
        }

        ReadPads();
        uint64_t start = NowUs();
        mGame->tick();
        uint64_t us = NowUs() - start;
        mPerfTickUs += us;
        mPerfMaxTickUs = std::max(mPerfMaxTickUs, us);
        mPerfTicks++;
    }

    LogPerformance(deltaTime);
}

void CastleGame::Render(float screenWidth, float screenHeight)
{
    if (!mGame || !mRenderer)
    {
        return;
    }

    uint64_t start = NowUs();
    mRenderer->begin_frame(int(screenWidth), int(screenHeight), kStage, swf::Rgba{});
    mGame->render(*mRenderer);
    mPerfRenderUs += NowUs() - start;
    mPerfFrames++;
}

void CastleGame::LogPerformance(float deltaTime)
{
    mPerfTime += deltaTime;
    if (mPerfTime < 2.0f)
    {
        return;
    }

    player::Player* movie = mGame->current();
    char line[256];
    snprintf(line, sizeof(line), "%s  %.1f ticks/s  tick %.1f ms (max %.1f)  draw %.1f ms  %u KB free",
        movie ? movie->name().c_str() : "-",
        mPerfTicks / mPerfTime,
        mPerfTicks ? double(mPerfTickUs) / mPerfTicks / 1000.0 : 0.0,
        double(mPerfMaxTickUs) / 1000.0,
        mPerfFrames ? double(mPerfRenderUs) / mPerfFrames / 1000.0 : 0.0,
        FreeMemoryKb());
    mStatus = line;
    OctLog("castle: perf %s", line);

    mPerfTime = 0.0f;
    mPerfTicks = 0;
    mPerfFrames = 0;
    mPerfTickUs = 0;
    mPerfRenderUs = 0;
    mPerfMaxTickUs = 0;
}
