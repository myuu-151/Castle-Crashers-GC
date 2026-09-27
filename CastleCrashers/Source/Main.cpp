#include <stdint.h>

#undef min
#undef max

#include "Engine.h"
#include "Log.h"
#include "Renderer.h"
#include "World.h"
#include "Nodes/Widgets/Text.h"

#include "CastleGame.h"
#include "StageWidget.h"

// Octave's packager generates these; embedded builds ("GameCube Embedded") fill them.
#if __has_include("../Generated/EmbeddedAssets.h")
#include "../Generated/EmbeddedAssets.h"
#include "../Generated/EmbeddedScripts.h"
#define CASTLE_HAS_GENERATED 1
#else
#define CASTLE_HAS_GENERATED 0
#endif

// No scene: CastleGame runs the game from CastleCrashers/Scripts/Data
// (packaged with the project and served from the ISO), drawn by one
// full-screen StageWidget.

// SD diagnostic log (Octave System_Dolphin.cpp; writes /octiso.log when the local logger is enabled).
void OctLog(const char* format, ...);

static CastleGame* sGame = nullptr;
static Text* sStatus = nullptr;  // the performance readout, over the stage

void OctPreInitialize(EngineConfig& config)
{
    GetEngineState()->mStandalone = true;

    if (config.mWindowWidth == 0)
        config.mWindowWidth = 1280;

    if (config.mWindowHeight == 0)
        config.mWindowHeight = 720;

#if CASTLE_HAS_GENERATED
    config.mEmbeddedAssetCount = gNumEmbeddedAssets;
    config.mEmbeddedAssets = gEmbeddedAssets;
    config.mEmbeddedScriptCount = gNumEmbeddedScripts;
    config.mEmbeddedScripts = gEmbeddedScripts;
    config.mEmbeddedConfig = gEmbeddedConfig_Data;
    config.mEmbeddedConfigSize = gEmbeddedConfig_Size;
#endif
}

void OctPostInitialize()
{
    // No engine console over the game (see FNAF1's Main.cpp).
    if (Renderer::Get() != nullptr)
    {
        Renderer::Get()->EnableConsole(false);
    }

    OctLog("castle: engine initialized, screen %dx%d", GetEngineState()->mWindowWidth, GetEngineState()->mWindowHeight);
    sGame = new CastleGame();

    if (!sGame->Initialize())
    {
        OctLog("castle: game initialization FAILED");
        LogError("castle: initialization failed");
    }

    glm::vec2 res = Renderer::Get()->GetScreenResolution();
    StageWidget* stage = GetWorld(0)->SpawnNode<StageWidget>();
    stage->SetName("Stage");
    stage->SetRect(0.0f, 0.0f, res.x, res.y);
    stage->SetGame(sGame);

    sStatus = stage->CreateChild<Text>("Status");
    sStatus->SetRect(8.0f, 4.0f, res.x - 16.0f, 20.0f);
    sStatus->SetTextSize(12.0f);
    sStatus->SetColor(glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
}

void OctPreUpdate()
{

}

void OctPostUpdate()
{
    if (sGame != nullptr)
    {
        sGame->Update(GetEngineState()->mGameDeltaTime);
        if (sStatus != nullptr)
        {
            sStatus->SetText(sGame->GetStatus());
        }
    }
}

void OctPreShutdown()
{
    delete sGame;
    sGame = nullptr;
}

void OctPostShutdown()
{

}
