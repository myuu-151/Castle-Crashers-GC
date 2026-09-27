#include "StageWidget.h"

#include "CastleGame.h"
#include "Engine.h"
#include "Graphics/GX/GxTypes.h"
#include "Graphics/GX/GxUtils.h"

DEFINE_NODE(StageWidget, Widget);

extern GxContext gGxContext;  // Graphics_GX.cpp

DrawData StageWidget::GetDrawData()
{
    DrawData data = {};
    data.mNode = this;
    return data;
}

void StageWidget::Render()
{
    Widget::Render();

    if (mGame == nullptr)
    {
        return;
    }

    // The stage fills the framebuffer, squeezed to it (anamorphic 16:9; Octave's
    // UI coordinates are a virtual resolution over the whole of it).
    GXRModeObj* rmode = &GetEngineState()->mSystem.mGxRmode;
    mGame->Render(float(rmode->fbWidth), float(rmode->efbHeight));

    // Back to Octave's UI state (viewport, scissor, matrices, vertex format)
    // for anything drawn after the stage.
    GX_SetViewport(0.0f, 0.0f, float(rmode->fbWidth), float(rmode->efbHeight), 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetColorUpdate(GX_TRUE);
    // The masks' depth and alpha tests (renderer_gx.cpp) off again.
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    PrepareUiRendering();

    // Octave's SetupLightingChannels remembers the channels it last set and
    // skips setting them again; the stage changed them, so set them back to
    // what it remembers (the UI's, as SetupLightingChannels programs them).
    const LightingState& ui = gGxContext.mLighting;
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR1A1, false, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_COLOR0A0, ui.mEnabled, GX_SRC_REG, ui.mMaterialSrc, ui.mLightMask, ui.mDiffuseFunc,
        ui.mAttenuationFunc);
}
