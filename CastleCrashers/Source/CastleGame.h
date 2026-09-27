// Castle Crashers on the GameCube: runs the engine's player::Game at its 30
// ticks per second, feeds it the pads, and draws it through StageWidget.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

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

private:
    void ReadPads();
    void LogPerformance(float deltaTime);

    std::unique_ptr<player::Game> mGame;
    std::unique_ptr<render::Renderer> mRenderer;

    float mTickTime = 0.0f;  // time owed to the next tick
    std::string mStatus;

    // Performance, every 2 s: ticks run, and the time spent in them and in
    // drawing.
    float mPerfTime = 0.0f;
    uint32_t mPerfTicks = 0;
    uint32_t mPerfFrames = 0;
    uint64_t mPerfTickUs = 0;
    uint64_t mPerfRenderUs = 0;
    uint64_t mPerfMaxTickUs = 0;
};
