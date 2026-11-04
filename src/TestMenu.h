#pragma once
// Test menu (F10, console "test <name>"): shortcuts so co-op features can be tried without travelling.
#include "ModInterface.h"
#include "Net.h"
#include <cstddef>
#include <cstdint>

void TestMenuInit(const HS_ModApi* api, int* localPlayer);
void TestMenuTick(CInstance* self, const PlayerState& remote, bool remoteFresh);
void TestMenuDraw(CInstance* self);
void TestMenuCommand(const char* name);
void TestMenuOnMessage(uint8_t type, const uint8_t* data, size_t size);
