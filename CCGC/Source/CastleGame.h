// Castle Crashers on the GameCube: runs the engine's player::Game at its 30
// ticks per second, feeds it the pads, and draws it through StageWidget.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MemoryCard.h"

namespace player { class Game; }
namespace render { class Renderer; }

class CastleGame
{
public:
    CastleGame();
    ~CastleGame();

    bool Initialize();
    void Update(float deltaTime);

    // Called by StageWidget while Octave renders its UI.
    void Render(float screenWidth, float screenHeight);

    // The last performance line, for the on-screen readout.
    const std::string& GetStatus() const { return mStatus; }

    // The memory card: whether progress is saved to it, and saving or
    // loading now (the Save / Load page).
    bool IsSaving() const { return mSaving; }
    bool SaveNow();
    bool LoadNow();
    player::Game* GetGame() { return mGame.get(); }
    // The title menu's Quit was chosen.
    bool IsQuitting() const;

private:
    // Before the game starts: slot A is checked, and asked about if there
    // is no save on it.
    enum class Boot { Check, Prompt, Running };
    void UpdateBoot();
    void StartGame();
    void RenderPrompt();

    void ReadPads();
    void LogPerformance(float deltaTime);

    std::unique_ptr<player::Game> mGame;
    std::unique_ptr<render::Renderer> mRenderer;

    Boot mBoot = Boot::Check;
    card::Status mCard;
    std::vector<std::string> mPrompt;  // the question's lines
    bool mSaving = false;               // progress goes to the card
    bool mCreateSave = false;           // a new save is written once the game starts
    std::vector<uint8_t> mSaveBytes;    // the save read at boot

    float mTickTime = 0.0f;  // time owed to the next tick
    std::string mStatus;

    // Performance, every 2 s: ticks run, and the time spent in them and in
    // drawing.
    float mPerfTime = 0.0f;
    uint32_t mPerfTicks = 0;
    uint32_t mPerfFrames = 0;

    // Tracing (trace_gc.h): the movie and menu page last logged.
    void TraceChanges();
    std::string mTraceMovie;
    bool mTickedSinceFrame = true;  // (the flicker detector: renderer_gx.cpp)
    uint16_t mComboHeld = 0;        // pad 1's buttons, for L + R + D-pad up
    int mTracePage = -2;
    // What holds input (the engine's Game::input_state), as last logged: to
    // catch a screen the game never leaves.
    std::string mTraceInput;
    uint64_t mPerfTickUs = 0;
    uint64_t mPerfRenderUs = 0;
    uint64_t mPerfMaxTickUs = 0;
};
