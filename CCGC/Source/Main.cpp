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

// No scene: CastleGame runs the game from CCGC/Scripts/Data
// (packaged with the project and served from the ISO), drawn by one
// full-screen StageWidget.

// SD diagnostic log (Octave System_Dolphin.cpp; writes /octiso.log when the local logger is enabled).
#include "trace_gc.h"

static CastleGame* sGame = nullptr;
static Text* sStatus = nullptr;  // the performance readout, over the stage

#if OCT_GDB
#include <debug.h>
#include <ogc/usbgecko.h>
#elif OCT_GECKO_LOG
bool OctGeckoLogEnable();  // Octave (System_Dolphin.cpp): the log over the USB Gecko
#endif

void OctPreInitialize(EngineConfig& config)
{
#if OCT_GDB
    // Debug (GDB): libogc's debug stub on the USB Gecko (slot A, else B); waits here for GDB (DolphinWorks'
    // Start GDB; the screen stays black till then). "continue" in GDB runs the game.
    DEBUG_Init(GDBSTUB_DEVICE_USB, usb_isgeckoalive(EXI_CHANNEL_0) ? EXI_CHANNEL_0 : EXI_CHANNEL_1);
    _break();
#elif OCT_GECKO_LOG
    OctGeckoLogEnable();  // built with GECKOLOG=1: the log, live over the USB Gecko
#endif
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

    PpgcLog("castle: engine initialized, screen %dx%d", GetEngineState()->mWindowWidth, GetEngineState()->mWindowHeight);
    sGame = new CastleGame();

    if (!sGame->Initialize())
    {
        PpgcLog("castle: game initialization FAILED");
        LogError("castle: initialization failed");
    }

    glm::vec2 res = Renderer::Get()->GetScreenResolution();
    StageWidget* stage = GetWorld(0)->SpawnNode<StageWidget>();
    stage->SetName("Stage");
    stage->SetRect(0.0f, 0.0f, res.x, res.y);
    stage->SetGame(sGame);

    // The performance readout, only in test builds (DIAG, REPLAY, AUTOPRESS):
    // a disc for playing shows the game alone (the same figures go to the
    // SD card's log every two seconds, in a build with it).
#if defined(PPGC_DIAG) || defined(CASTLE_REPLAY)
    sStatus = stage->CreateChild<Text>("Status");
    sStatus->SetRect(8.0f, 4.0f, res.x - 16.0f, 20.0f);
    sStatus->SetTextSize(12.0f);
    sStatus->SetColor(glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
#endif
}

void OctPreUpdate()
{

}

void OctPostUpdate()
{
    if (sGame != nullptr)
    {
        sGame->Update(GetEngineState()->mGameDeltaTime);
        // Quit (the title menu's) leaves the game, back to the loader.
        if (sGame->IsQuitting())
        {
            Quit();
        }
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
