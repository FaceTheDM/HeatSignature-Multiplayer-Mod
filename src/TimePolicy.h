#pragma once
#include "ModInterface.h"

// Shared time policy: both games run at the slower of the two players' desired time scales while both are in gameplay.
void TimePolicyInit(const HS_ModApi* api);
double TimePolicyLocalWanted();
bool TimePolicyLocalPaused();       // this player has the pause menu or the inventory open
int TimePolicyLocalPauseKind();     // 0 playing, 1 pause menu, 2 inventory (planning)
bool TimePolicyPausedByPartner();   // this game is held still by the partner's pause
void TimePolicyTick(CInstance* self);
// From coop.ini `time=` (shared by default).
void TimePolicyConfigure(bool shared);
// Implemented by the plugin core: the partner's desired time scale, or 0 when unknown.
double CoopRemoteTimeScaleWanted();
// True while the partner's slow-mo is slowing this game (shown on screen).
bool TimePolicyPartnerSlowing();
