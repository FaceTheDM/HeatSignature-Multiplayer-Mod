#pragma once
// Copies an instance's variables between processes. Variable ids come from the same executable, so they match;
// instance references do not, so they are never copied.
#include "ModInterface.h"
#include "Wire.h"

void SnapshotInit(const HS_ModApi* api);
// Writes every defined real/string variable and real/string array element of `id`.
void SnapshotCapture(int id, Writer& w);
// Applies a snapshot written by SnapshotCapture onto `id`; returns the number of values written.
// keepLocalRefs: instance references (legs, handles, parent) keep their local values instead of being re-linked.
int SnapshotApply(int id, Reader& r, bool keepLocalRefs=false);
