#pragma once
#include "ModInterface.h"
#include <string>

// Subscribes generation seed points; must be called from ModInit. Empty password disables seeding.
void SeedInit(const HS_ModApi* api, const char* password);
// Stable cross-process identity of an instance (object, name, rounded position, faction, station names).
std::string SeedIdentity(int id);
// Reseeds the RNG for `key` (with a per-key use counter).
void SeedNow(CInstance* self, const std::string& key);
// Dev diagnostic: probe file lists scripts at whose entry the RNG position is logged.
void SeedProbes(const char* path);
// The shared secret (the session's password) that makes both games generate alike; "" turns seeding off.
void SeedSetSecret(const char* password);

// The practice scenario this player is in ("" when not practising).
const char* SeedPracticeScenario();
// This player joined the partner's running practice (same practice, started after theirs).
bool SeedPracticeJoined();
