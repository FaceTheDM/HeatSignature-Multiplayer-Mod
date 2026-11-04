#pragma once
// Shared crate choice: the opener chooses in the game's own menu; the partner's game stands still and shows the same
// rows with Suggest buttons; the suggestion is highlighted on both screens.
#include "ModInterface.h"
#include <cstddef>
#include <cstdint>

void CrateInit(const HS_ModApi* api, int* localPlayer);
void CrateOnMessage(uint8_t type, const uint8_t* data, size_t size);
// GUI pass: the opener's highlight and the partner's Suggest panel.
void CrateDraw(CInstance* self);
// The partner is choosing from a crate they opened (this game then stands still).
bool CrateRemoteChoosing();
