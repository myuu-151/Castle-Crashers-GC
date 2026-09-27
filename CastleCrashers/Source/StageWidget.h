// A full-screen widget that draws the game's stage with GX when Octave
// renders its UI.
#pragma once

#include "Nodes/Widgets/Widget.h"

class CastleGame;

class StageWidget : public Widget
{
public:
    DECLARE_NODE(StageWidget, Widget);

    void SetGame(CastleGame* game) { mGame = game; }

    // A plain Widget has no draw data, so Octave would never call Render.
    virtual DrawData GetDrawData() override;
    virtual void Render() override;

private:
    CastleGame* mGame = nullptr;
};
