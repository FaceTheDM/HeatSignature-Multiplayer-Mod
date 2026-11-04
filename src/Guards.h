#pragma once
#include "ModInterface.h"
#include <cstddef>
#include <cstdint>
#include <string>

// Guard replication on ships both players share: the HOST simulates guards and streams their state; the CLIENT's copies
// follow it. Hits are applied by the HOST, and HOST guards can target the remote player through its puppet.
void GuardsInit(const HS_ModApi* api, int* localPlayer, int* puppet);
void GuardsOnMessage(uint8_t type, const uint8_t* data, size_t size);
void GuardsTick(CInstance* self, bool inGameplay);
const char* GuardsStatus();
void GuardsDebugDump(int ship);
// True while an authority guard is updating with the remote player's puppet as `Player`.
bool GuardsTargetingPuppet();
// The ship both players are aboard (0 if none) and whether this game simulates its guards.
int GuardsSharedShip();
bool GuardsIsAuthority();
// Cross-process guard keys (generation position) on the shared ship.
const char* GuardsKeyOfGuard(int id);
int GuardsGuardByKey(const char* key);
const char* GuardsTestDeliver(const char* key);
// JSON array describing the guards aboard a ship (developer builds only).
std::string GuardsSnapshotJson(int ship);
// A guard this game shows as a copy of the guards' game's (it does not act on its own).
bool GuardsIsMirroredGuard(int id);
// The guards' game's guard dropped this item: the copy here gives up its own copy of it.
void GuardsCopyGivesUp(const char* guardKey,const char* objectName);
// This game rebuilt the shared ship the partner plays on (practice restarted): it follows the partner's guards.
void GuardsFollowRebuiltShip();
// The partner rebuilt the shared ship its game ran the guards of: this game runs them from now on.
void GuardsTakeOver(const char* ship);
// This guard was put down because the guards' game said so (its drops come from there).
bool GuardsDownByAuthority(int id);
