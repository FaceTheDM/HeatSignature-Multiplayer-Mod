#pragma once
// Heat Signature keeps the camera near the origin by shifting every object's xReal/yReal (MoveEverythingToKeepCameraAtZero),
// so world coordinates differ between processes. Positions exchanged between games are expressed relative to one station
// both games share (the one whose name sorts first), which moves with the shift in each process. (Not the oGalaxy
// instance: it is not shifted with the world; see Frame.cpp.)
#include "ModInterface.h"
#include <cstdint>

void FrameInit(const HS_ModApi* api);
// Reference position in this process; false if the galaxy's stations are not available yet.
bool FrameRef(double& x, double& y, CInstance* hint=nullptr);
// Galaxy identity (0 until computed); `stationShip` is a station ship the player is aboard.
uint32_t FrameGalaxyFingerprint(CInstance* self, int stationShip);
