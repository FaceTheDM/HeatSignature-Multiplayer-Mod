#pragma once
// Placing something aboard a ship after the mod changed its ship-local position (Fore/Starboard).
//
// The game keeps everyone aboard in ship-local coordinates and derives their room x/y once per frame, in End Step, in one
// chain: the ship integrates and rebuilds its anchor, then each person is placed from that anchor. Its aim, hit and
// sight code (in Step) compares room x/y of the player, the cursor and other people, relying on all of them belonging
// to the same moment (the previous End Step).
//
// Calling the game's UpdatePosition on a person from the mod's tick (Step) broke that: it makes the person's ship
// integrate early, so the ship and that person are a frame of ship travel ahead (about 200 units on a fast ship) of
// everyone the game has not placed yet, the player included. On the partner's screen melee aim and lock-on "led" in the
// ship's direction of travel; in the owner's game the stand-in was a frame ahead of the guards.
//
// PlaceAboard derives room x/y from the ship's CURRENT anchor and axes, without integrating anything and without
// marking the person as updated, so the game's own End Step chain still runs for it. Things with no ship to stand on
// fall back to UpdatePosition.
#include "Console.h"
#include "GmArgs.h"
#include <cmath>

inline void PlaceAboard(const HS_ModApi* api, CInstance* inst) {
    if(!inst) return;
    auto num=[&](int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); };
    int id=(int)inst->id, parent=(int)num(id,"Parent");
    CInstance* ship=parent>=100000 && parent!=id ? api->ResolveCInstance(parent) : nullptr;
    double ax=ship?num(parent,"AnchorX"):NAN, ay=ship?num(parent,"AnchorY"):NAN;
    double fux=ship?num(parent,"ForeUnitX"):NAN, fuy=ship?num(parent,"ForeUnitY"):NAN, sux=ship?num(parent,"StarboardUnitX"):NAN, suy=ship?num(parent,"StarboardUnitY"):NAN;
    double f=num(id,"Fore"), s=num(id,"Starboard");
    if(std::isnan(ax+ay+fux+fuy+sux+suy+f+s)) {
        GmArgs none; RValue r{}; r.type=5; api->CallScript("gml_Script_UpdatePosition",inst,inst,&r,none.Count(),none.Build());
        return;
    }
    double x=ax+f*fux+s*sux, y=ay+f*fuy+s*suy;
    RValue v{}; v.type=0;
    v.real=x; api->SetVarByName(id,"xReal",0x80000000,&v);
    v.real=y; api->SetVarByName(id,"yReal",0x80000000,&v);
    GmArgs sx; sx.AddReal(id); sx.AddStr(api,"x"); sx.AddReal(x); Engine(api,"variable_instance_set",inst,sx.Count(),sx.Build());
    GmArgs sy; sy.AddReal(id); sy.AddStr(api,"y"); sy.AddReal(y); Engine(api,"variable_instance_set",inst,sy.Count(),sy.Build());
}
