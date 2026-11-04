#pragma once
// What the partner sees of you beyond position: the weapon in your hands, your torso animation (idle/aim/shoot/swing),
// your shots (tracers) and gun sounds.
#include "ModInterface.h"
#include "Net.h"
#include <cstddef>
#include <cstdint>

void VisualsInit(const HS_ModApi* api, int* localPlayer, int* puppet);
void VisualsFill(PlayerState& s);
// Applies the partner's weapon and animation to the puppet; returns false when nothing could be applied.
void VisualsApplyToPuppet(CInstance* puppet, const PlayerState& remote);
void VisualsOnMessage(uint8_t type, const uint8_t* data, size_t size);
void VisualsDraw(CInstance* self);
// The torso animation last applied to the puppet ("" if none).
const char* VisualsPuppetTorso();
// The stand-in takes the game's down pose (torso and legs) while the partner is down.
void VisualsPuppetLookDead(CInstance* puppet);

// A bullet made here as a copy of a shot fired in the other game (it does no damage; its effects come from that game).
bool VisualsIsCopyBullet(int id);
