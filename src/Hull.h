#pragma once
// Ship damage both players share: module losses and ship splits are decided by the game that flies the ship.
#include "ModInterface.h"
#include <cstddef>
#include <cstdint>

void HullInit(const HS_ModApi* api);
void HullOnMessage(uint8_t type, const uint8_t* data, size_t size);
void HullTick();
