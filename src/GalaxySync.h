#pragma once
// Automatic galaxy sharing: when the CLIENT's galaxy differs from the HOST's, the CLIENT asks for it, the HOST sends its
// saved galaxy (and crew) files, and the CLIENT installs them as the galaxy slot "Galaxy Coop" (or "Galaxy Coop B" while
// "Galaxy Coop" is the one being played), selects it and asks the player to restart the game.
#include "ModInterface.h"
#include <cstddef>
#include <cstdint>

// Re-applies a galaxy selection made by the previous session (the game may rewrite its options on exit).
void GalaxySyncStartup();
void GalaxySyncTick(bool host, uint32_t localGalaxy, uint32_t remoteGalaxy, bool partnerFresh, bool sameBuild);
void GalaxySyncOnMessage(uint8_t type, const uint8_t* data, size_t size);
// Status line text while a transfer is pending or done ("" otherwise).
const char* GalaxySyncStatus();
// Asks the HOST for its galaxy now (CLIENT; console `coop getgalaxy`).
void GalaxySyncRequest();
// The partner restarted: a lost galaxy request may be made again.
void GalaxySyncReset();
