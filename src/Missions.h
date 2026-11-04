#pragma once
#include "ModInterface.h"
#include <cstddef>
#include <cstdint>

// Shared missions: each player keeps their own active mission. When either accepts one, its ship is generated in the
// partner's world too (seeded generation makes it identical), and completing it pays the partner a share.
void MissionsInit(const HS_ModApi* api, int* localPlayer);
void MissionsOnMessage(uint8_t type, const uint8_t* data, size_t size);
// Called every frame on the game thread; performs queued generation.
void MissionsTick(CInstance* self);
// Finds a local ship/station matching a remote parent by name and galaxy position (0 if none).
int MissionsFindShip(const char* name, double x, double y);
const char* MissionsStatus();
// The most recent partner mission ship still alive here (0 if none), with its owner and mission type.
int MissionsPartnerShip(const char** owner, const char** type);
// Sends this player's live mission ships again (the partner restarted).
void MissionsResend();
// A partner mission that just finished (its ship name, "" if none) - a player still aboard follows the partner.
const char* MissionsFinishedShip();
void MissionsClearFinished();
void MissionsTestDone();
// The partner's mission just ended: the text to show (nullptr when none); `failed` says how it ended.
const char* MissionsBanner(bool* failed);
