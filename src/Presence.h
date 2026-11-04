#pragma once
// Finding and helping each other anywhere: HUD arrows to the partner and to the partner's mission ship, the partner's
// danger timer (drifting in space, bleeding out), a co-op menu (F8) to join the partner or invite them, and pod rescue.
#include "ModInterface.h"
#include "Net.h"
#include <cstddef>
#include <cstdint>

void PresenceInit(const HS_ModApi* api, int* localPlayer);
// Fills the presence fields of the local state packet.
void PresenceFill(PlayerState& s);
// Per frame (game thread, step): menu input, invites, rescue detection.
void PresenceTick(CInstance* self, const PlayerState& remote, bool remoteFresh);
// GUI pass: arrows, danger timer, menu and prompts.
void PresenceDraw(CInstance* self, const PlayerState& remote, bool remoteFresh);
void PresenceOnMessage(uint8_t type, const uint8_t* data, size_t size);
// Console: "join", "invite", "accept", "menu".
void PresenceCommand(const char* what);
// The player's ship no longer exists: takes them to their home station at once. False if that was not possible.
bool PresenceRescueToHome();
// The ship this player rides on docked at a station in the partner's game: this player goes into that station too.
bool PresenceArriveAtStation(const char* stationName,const char* shipName);

// This player rides in the partner's pod (caught drifting): they do not steer.
bool PresenceRiding();
