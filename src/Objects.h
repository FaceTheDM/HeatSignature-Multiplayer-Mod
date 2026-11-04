#pragma once
// Ship objects both players share (doors, loot, money, barrels, windows, airlocks, terminals, sentries, the mission
// objective): keyed by generation position, their state changes and disappearances are exchanged in both directions.
// Explosions from unkeyed sources (grenades, mines) are replayed in the partner's world; guard sounds are forwarded.
#include "ModInterface.h"
#include <cstddef>
#include <cstdint>

void ObjectsInit(const HS_ModApi* api, int* localPlayer, int* puppet);
// Called every frame with the ship both players are aboard (0 if none).
void ObjectsTick(CInstance* self, int sharedShip);
void ObjectsOnMessage(uint8_t type, const uint8_t* data, size_t size);
// Key of a ship object recorded at generation (empty if unkeyed).
const char* ObjectsKeyOf(int id);
// The local instance of a shared object on `ship` by its key (0 if none).
int ObjectsIdOfKey(int ship, const char* key);
// True shortly after an explosion: its damage happens in both games, so hits are not forwarded.
bool ObjectsExplosionWindow();
// Test helper: holds the door nearest to the player open for `seconds`; returns what it did.
const char* ObjectsTestHoldNearestDoor(double seconds);
// True shortly after an explosion close to this instance (its hit happened in both games).
bool ObjectsExplosionNear(int id);
// The game running the shared ship sends every object's state (the partner (re)connected).
void ObjectsResync();
// An item this game just created aboard the shared ship (a guard's loot drop): placed in the partner's game too, once it
// has come to rest.
void ObjectsShareNewItem(int item);
