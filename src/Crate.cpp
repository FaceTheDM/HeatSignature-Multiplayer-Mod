// Shared crate choice. When a player opens a crate, the game shows them the "found" menu: the item next to their
// inventory, one Replace/Keep/Send-to-Stash row per slot. Only the opener gets the item.
//  - The opener's game sends the rows it draws (their labels) while the menu is open.
//  - The partner's game stands still (like a pause) and shows the same rows with a "Suggest" button each; a click sends
//    that row to the opener.
//  - The suggested row is highlighted on both screens; the opener still makes the choice in the game's own menu.
// Rows are taken from the game's own drawing (DrawInventoryListBox "Cursor Tag" boxes and the texts beside them), so the
// highlight sits exactly on the row the opener sees, whatever the menu's scale.
#include "Crate.h"
#include "Console.h"
#include "GmArgs.h"
#include "Net.h"
#include "Snapshot.h"
#include "Wire.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static const HS_ModApi* api;
static int* player;
static const double OBJ_LOOTBOX=66, OBJ_LOOTBOXLID=67;
static const DWORD SEND_MS=250, MENU_GONE_MS=400, REMOTE_STALE_MS=1500;
bool ObjectsBoxOpenedByPartner(int box);
bool ObjectsPartnerOpenedABoxJustNow();
int ObjectsMakePreviewItem(const char* name,Reader& r);
const char* CoopRemoteName();

static bool live(int id){ return id>0 && api->ResolveCInstance(id); }
static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static RValue engine(const char* fn,CInstance* self,GmArgs& a){ return Engine(api,fn,self,a.Count(),a.Build()); }
static double arg(int argc,RValue** argv,int i){ return i<argc&&argv&&argv[i]?Number(*argv[i]):0; }
static std::string argStr(int argc,RValue** argv,int i){ return i<argc&&argv&&argv[i]&&GetRValueKind(*argv[i])==RValueKind::String&&argv[i]->str&&argv[i]->str->text?argv[i]->str->text:""; }

// ---- The opener's side ----
struct Row { double x,y,w,h; std::string label; };
static bool opening;                 // this player opened a crate and its menu is (about to be) shown
static DWORD openedAt, lastRowsAt, lastSent;
static std::vector<Row> rows, frameRows;      // last complete frame's rows; rows being collected this frame
static std::map<int,std::string> textAt;      // texts drawn in the list this frame, by y
static std::vector<double> slotYs;            // the list's left-column boxes this frame
static int suggestedHere=-1;
static double suggestedY=-1e9;                 // that row's line in the list
static bool drawingOwn;                       // our own text drawn into the list (not one of the game's labels)                  // the row the partner suggested (opener's side)

static void preShowInventoryMenu(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    // A crate (and then its lid) shows the found menu as it opens.
    if(!self || (self->type!=(uint32_t)OBJ_LOOTBOX && self->type!=(uint32_t)OBJ_LOOTBOXLID)) return;
    bool partners=self->type==(uint32_t)OBJ_LOOTBOX?ObjectsBoxOpenedByPartner((int)self->id):ObjectsPartnerOpenedABoxJustNow();
    if(partners) {
        // The partner opened this crate: what is inside is theirs to choose; this player gets no menu and no item.
        api->RequestBypass(); return;
    }
    if(!opening) { opening=true; openedAt=GetTickCount(); suggestedHere=-1; CoopLog("Crate: you opened a crate; your partner can suggest what to take"); }
}
// Every list box the menu draws; the "Cursor Tag" boxes are the choice rows (Replace / Keep / Send to Stash).
static void postDrawListBox(const char*,CInstance* self,CInstance* other,RValue*,int argc,RValue** argv,void*) {
    if(!opening || argc<5) return;
    // The left column: inventory slots (filled or empty) and Stash.
    { std::string k=argStr(argc,argv,4); if(arg(argc,argv,0)<1 && (k=="Inventory" || k=="Cursor Item")) slotYs.push_back(arg(argc,argv,1)); }
    // The row under the opener's mouse is drawn "Black" instead of "Cursor Tag"; both are in the tag column.
    std::string kind=argStr(argc,argv,4);
    if(kind!="Cursor Tag" && !(kind=="Black" && arg(argc,argv,0)>=300)) return;
    for(const Row& r:frameRows) if(std::fabs(r.y-arg(argc,argv,1))<1) return;   // one row per line (hover redraws it)
    frameRows.push_back({arg(argc,argv,0),arg(argc,argv,1),arg(argc,argv,2),arg(argc,argv,3),""});
    // The partner's suggestion, written into that row by the game's own list drawing (so it lines up exactly).
    if(suggestedHere>=0 && std::fabs(frameRows.back().y-suggestedY)<1 && self) {
        const Row& r=frameRows.back();
        // The partner's first name keeps the note on one line.
        const char* who=CoopRemoteName(); std::string first=who&&*who?who:"Partner"; first=first.substr(0,first.find(' '));
        char t[96]; snprintf(t,sizeof(t),"<< %s's pick",first.c_str());
        GmArgs a; a.AddStr(api,t); a.AddReal(r.x+r.w*0.42); a.AddReal(r.y); a.AddReal(0x33ccff);
        RValue out{}; drawingOwn=true; api->CallScript("gml_Script_DrawInventoryListText",self,other,&out,a.Count(),a.Build()); drawingOwn=false;
    }
}
static void postDrawListText(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!opening || argc<3 || drawingOwn) return;
    textAt[(int)std::lround(arg(argc,argv,2))]=argStr(argc,argv,0);
}
static void preDrawInventoryList(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { frameRows.clear(); textAt.clear(); slotYs.clear(); }

// The row's meaning, from the slot drawn on its left (an item name, an empty slot, or "Stash").
static std::string labelFor(double y) {
    auto it=textAt.find((int)std::lround(y));
    if(it==textAt.end() || it->second.empty()) return "Keep (empty slot)";
    if(it->second=="Stash") return "Send to Stash";
    return "Replace "+it->second;
}

// ---- The partner's side ----
static bool remoteActive; static DWORD remoteAt;
static std::string remoteFound;
static std::vector<std::string> remoteRows;
static int suggestedThere=-1;   // the row this player suggested
static int previewItem;         // a hidden copy of the partner's found item, for the game's details panel
static int sentItem;            // the opener's found item last described to the partner
static double panelX,panelY,panelW,rowH,firstRowY; static int hoverRow=-1;

bool CrateRemoteChoosing() { return remoteActive && GetTickCount()-remoteAt<REMOTE_STALE_MS; }

void CrateOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    Reader r(data,size);
    if(type==MSG_CRATE_MENU) {
        uint8_t active=r.get<uint8_t>(); std::string found=r.str(); uint8_t n=r.get<uint8_t>();
        std::vector<std::string> labels; for(int i=0;i<n && r.ok;i++) labels.push_back(r.str());
        int8_t suggestion=r.get<int8_t>();
        if(!r.ok) return;
        if(active && !remoteActive) { suggestedThere=-1; previewItem=0; CoopLog("Crate: partner opened a crate (%s); waiting for their choice",found.c_str()); }
        if(!active && remoteActive) CoopLog("Crate: partner made their choice");
        remoteActive=active!=0; remoteAt=GetTickCount(); remoteFound=found; remoteRows=labels; suggestedThere=suggestion;
    } else if(type==MSG_CRATE_ITEM) {
        std::string name=r.str(); if(!r.ok) return;
        int id=ObjectsMakePreviewItem(name.c_str(),r);
        if(id) { previewItem=id; CoopLog("Crate: partner's found item (%s) shown with its details",name.c_str()); }
    } else if(type==MSG_CRATE_SUGGEST) {
        int8_t row=r.get<int8_t>(); if(!r.ok || !opening) return;
        suggestedHere=row;
        CoopLog("Crate: partner suggests row %d (%s)",row,row>=0&&row<(int)rows.size()?rows[row].label.c_str():"?");
    }
}

static void sendMenu(bool active) {
    Writer w; w.put<uint8_t>(active?1:0);
    std::string found; int cursor=*player>0?(int)getReal(*player,"CursorItem"):0;
    if(live(cursor)) found=getString(cursor,"Name");
    w.str(found);
    size_t n=active?(rows.size()<16?rows.size():16):0; w.put<uint8_t>((uint8_t)n);
    for(size_t i=0;i<n;i++) w.str(rows[i].label);
    w.put<int8_t>((int8_t)suggestedHere);
    NetSendUnreliable(MSG_CRATE_MENU,w.bytes.data(),w.bytes.size());
}

// Called from the GUI draw (after the game drew its own list this frame).
void CrateDraw(CInstance* self) {
    DWORD now=GetTickCount();
    if(opening) {
        // This frame's rows are complete once the list was drawn.
        if(!frameRows.empty()) {
            // Choices are the tags beside an inventory slot or Stash; a tag beside nothing follows the opener's mouse.
            rows.clear();
            for(const Row& r:frameRows) { bool slot=false; for(double y:slotYs) if(std::fabs(y-r.y)<1) slot=true; if(slot) rows.push_back(r); }
            for(Row& r:rows) r.label=labelFor(r.y); lastRowsAt=now;
            suggestedY=suggestedHere>=0 && suggestedHere<(int)rows.size()?rows[suggestedHere].y:-1e9;
        }
        bool menuGone=now-lastRowsAt>MENU_GONE_MS && now-openedAt>MENU_GONE_MS;
        if(menuGone) { opening=false; rows.clear(); suggestedHere=-1; sentItem=0; sendMenu(false); CoopLog("Crate: crate menu closed"); return; }
        if(now-lastSent>=SEND_MS) { lastSent=now; sendMenu(true); }
        // The found item itself (once per item), so the partner sees its full description and traits.
        int cursor=(int)getReal(*player,"CursorItem");
        if(live(cursor) && cursor!=sentItem) {
            sentItem=cursor;
            CInstance* ci=api->ResolveCInstance(cursor);
            Writer w; w.str(ci->GetObjectName(api)); SnapshotCapture(cursor,w);
            NetSendReliable(MSG_CRATE_ITEM,w.bytes.data(),w.bytes.size());
        }
    }
    if(!CrateRemoteChoosing()) { hoverRow=-1; return; }
    // The partner's choice, shown here with Suggest buttons.
    GmArgs none;
    double gw=Number(Engine(api,"display_get_gui_width",self,0,none.Build())), gh=Number(Engine(api,"display_get_gui_height",self,0,none.Build()));
    if(!(gw>0)) gw=1920; if(!(gh>0)) gh=1080;
    rowH=40; panelW=gw*0.42<560?560:gw*0.42; if(panelW>gw-40) panelW=gw-40;
    double panelH=90+rowH*remoteRows.size()+20;
    panelX=gw*0.04; panelY=(gh-panelH)/2; firstRowY=panelY+90;
    auto rect=[&](double x0,double y0,double x1,double y1,double colour,double alpha,bool outline){
        GmArgs c; c.AddReal(colour); engine("draw_set_colour",self,c);
        GmArgs a; a.AddReal(alpha); engine("draw_set_alpha",self,a);
        GmArgs b; b.AddReal(x0); b.AddReal(y0); b.AddReal(x1); b.AddReal(y1); b.AddReal(outline?1:0); engine("draw_rectangle",self,b);
        GmArgs one; one.AddReal(1); engine("draw_set_alpha",self,one);
    };
    auto text=[&](double x,double y,const std::string& s,double colour){
        GmArgs c; c.AddReal(colour); engine("draw_set_colour",self,c);
        GmArgs t; t.AddReal(x); t.AddReal(y); t.AddStr(api,s.c_str()); engine("draw_text",self,t);
    };
    GmArgs l0; l0.AddReal(0); engine("draw_set_halign",self,l0);
    rect(panelX,panelY,panelX+panelW,panelY+panelH,0x000000,0.85,false);
    const char* who=CoopRemoteName(); std::string name=who&&*who?who:"Your partner";
    text(panelX+20,panelY+14,name+" opened a crate"+(remoteFound.empty()?"":": "+remoteFound),0x66ddff);
    text(panelX+20,panelY+48,"They choose. Click a row to suggest it.",0xcccccc);
    GmArgs mx; mx.AddReal(0); double mouseX=Number(engine("device_mouse_x_to_gui",self,mx));
    GmArgs my; my.AddReal(0); double mouseY=Number(engine("device_mouse_y_to_gui",self,my));
    hoverRow=-1;
    for(size_t i=0;i<remoteRows.size();i++) {
        double y0=firstRowY+rowH*i, y1=y0+rowH-4;
        bool hover=mouseX>=panelX+10 && mouseX<=panelX+panelW-10 && mouseY>=y0 && mouseY<=y1;
        if(hover) hoverRow=(int)i;
        bool chosen=(int)i==suggestedThere;
        rect(panelX+10,y0,panelX+panelW-10,y1,chosen?0x885500:(hover?0x444444:0x222222),0.95,false);
        if(chosen) for(int k=0;k<2;k++) rect(panelX+10+k,y0+k,panelX+panelW-10-k,y1-k,0x33ccff,1,true);
        text(panelX+24,y0+8,remoteRows[i],0xffffff);
        GmArgs r; r.AddReal(2); engine("draw_set_halign",self,r);
        text(panelX+panelW-24,y0+8,chosen?"Suggested":"Suggest",chosen?0x33ccff:0x88ff88);
        engine("draw_set_halign",self,l0);
    }
    GmArgs w; w.AddReal(0xffffff); engine("draw_set_colour",self,w);
    // The found item's details, drawn by the game's own panel (it describes the player's CursorItem).
    if(live(previewItem) && *player>0 && live(*player)) {
        // DrawDescriptionAndTraitsForItem(item, x, y, 0): the game's own description and trait box, here beside the panel.
        double dx=panelX+panelW+16;
        { double tc=getReal(previewItem,"TextColour"); GmArgs c; c.AddReal(tc==tc?tc:0xffffff); engine("draw_set_colour",self,c);
          GmArgs nt; nt.AddReal(dx); nt.AddReal(panelY+8); nt.AddStr(api,getString(previewItem,"Name").c_str()); engine("draw_text",self,nt);
          GmArgs wc; wc.AddReal(0xffffff); engine("draw_set_colour",self,wc); }
        GmArgs d; d.AddReal(previewItem); d.AddReal(dx); d.AddReal(panelY+40); d.AddReal(0);
        RValue out{}; api->CallScript("gml_Script_DrawDescriptionAndTraitsForItem",api->ResolveCInstance(*player),api->ResolveCInstance(*player),&out,d.Count(),d.Build());
    }
}

// Clicks on the Suggest panel are read before the player's step, and then cleared so the game does not also act on them.
static void prePlayerStep(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || *player<=0 || (int)self->id!=*player || !CrateRemoteChoosing()) return;
    GmArgs b; b.AddReal(1);   // mb_left
    if(!Number(engine("mouse_check_button_pressed",self,b))) return;
    engine("mouse_clear",self,b);
    if(hoverRow<0) return;
    suggestedThere=hoverRow;
    Writer w; w.put<int8_t>((int8_t)hoverRow); NetSendReliable(MSG_CRATE_SUGGEST,w.bytes.data(),w.bytes.size());
    CoopLog("Crate: you suggested %s",hoverRow<(int)remoteRows.size()?remoteRows[hoverRow].c_str():"?");
}

void CrateInit(const HS_ModApi* a,int* localPlayer) {
    api=a; player=localPlayer;
    api->SubscribeHook("gml_Script_ShowInventoryMenu",preShowInventoryMenu,nullptr);
    api->SubscribeHookPost("gml_Script_DrawInventoryListBox",postDrawListBox,nullptr);
    api->SubscribeHookPost("gml_Script_DrawInventoryListText",postDrawListText,nullptr);
    api->SubscribeHook("gml_Script_DrawInventoryList",preDrawInventoryList,nullptr);
    api->SubscribeHook("gml_Script_UpdateState",prePlayerStep,nullptr);
}
