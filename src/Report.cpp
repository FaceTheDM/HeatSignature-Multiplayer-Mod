// Session logs and bug reports: see Report.h.
#include "Report.h"
#include "Console.h"
#include "GmArgs.h"
#include "Net.h"
#include "Wire.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void CoopRequestStatus();   // CoopProbe: log the status line on the next frame

static const HS_ModApi* api;
static std::string base, sessionPath, lastFolder;
static FILE* session;
static const int KEEP_SESSIONS=5;
static const size_t PARTNER_LOG_MAX=200*1024;   // the end of the partner's log is what matters

static std::string stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    char b[32]; snprintf(b,sizeof(b),"%04d%02d%02d-%02d%02d%02d",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    return b;
}
static std::vector<std::string> listSessions(const std::string& dir) {
    std::vector<std::string> out; WIN32_FIND_DATAA f;
    HANDLE h=FindFirstFileA((dir+"session-*.log").c_str(),&f);
    if(h==INVALID_HANDLE_VALUE) return out;
    do out.push_back(f.cFileName); while(FindNextFileA(h,&f));
    FindClose(h); std::sort(out.begin(),out.end());   // names sort by time
    return out;
}
void ReportInit(const HS_ModApi* a,const char* baseDir) {
    api=a; base=baseDir;
    std::string dir=base+"coop-logs\\"; CreateDirectoryA(dir.c_str(),nullptr);
    std::vector<std::string> old=listSessions(dir);
    for(size_t i=0;i+KEEP_SESSIONS-1<old.size();i++) DeleteFileA((dir+old[i]).c_str());   // room for this one
    sessionPath=dir+"session-"+stamp()+".log";
    session=fopen(sessionPath.c_str(),"w");
}
void ReportLogLine(const char* line) {
    if(!session) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(session,"%02d:%02d:%02d.%03d %s\n",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,line);
    fflush(session);
}

void ReportRecord(const char* line) {
    if(!session) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(session,"%02d:%02d:%02d.%03d REC %s\n",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,line);
    fflush(session);
}

// The game window as a .bmp, through Windows (GameMaker's screen_save only writes inside its own save area, so it
// could not put the picture in the report folder).
static bool saveWindow(const std::string& path) {
    HWND w=api->GetGameWindow?api->GetGameWindow():nullptr; if(!w) return false;
    RECT r; if(!GetClientRect(w,&r)) return false;
    int width=r.right-r.left, height=r.bottom-r.top; if(width<=0 || height<=0) return false;
    HDC screen=GetDC(w); HDC mem=CreateCompatibleDC(screen); HBITMAP bmp=CreateCompatibleBitmap(screen,width,height);
    HGDIOBJ old=SelectObject(mem,bmp);
    bool ok=PrintWindow(w,mem,3)!=0;   // PW_CLIENTONLY | PW_RENDERFULLCONTENT (DirectX content)
    BITMAPINFOHEADER bi{}; bi.biSize=sizeof(bi); bi.biWidth=width; bi.biHeight=height; bi.biPlanes=1; bi.biBitCount=24; bi.biCompression=BI_RGB;
    int stride=(width*3+3)&~3; std::vector<uint8_t> pixels((size_t)stride*height);
    SelectObject(mem,old);
    if(ok) ok=GetDIBits(mem,bmp,0,height,pixels.data(),(BITMAPINFO*)&bi,DIB_RGB_COLORS)==height;
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(w,screen);
    if(!ok) return false;
    BITMAPFILEHEADER bf{}; bf.bfType=0x4D42; bf.bfOffBits=sizeof(bf)+sizeof(bi); bf.bfSize=bf.bfOffBits+(DWORD)pixels.size();
    FILE* f=fopen(path.c_str(),"wb"); if(!f) return false;
    fwrite(&bf,sizeof(bf),1,f); fwrite(&bi,sizeof(bi),1,f); fwrite(pixels.data(),1,pixels.size(),f); fclose(f);
    return true;
}

static std::string readTail(const std::string& path,size_t max) {
    std::string out; FILE* f=fopen(path.c_str(),"rb"); if(!f) return out;
    fseek(f,0,SEEK_END); long size=ftell(f); long from=size>(long)max?size-(long)max:0;
    fseek(f,from,SEEK_SET); out.resize((size_t)(size-from));
    size_t got=fread(&out[0],1,out.size(),f); out.resize(got); fclose(f);
    return out;
}
static void writeFile(const std::string& path,const std::string& text) { FILE* f=fopen(path.c_str(),"wb"); if(f) { fwrite(text.data(),1,text.size(),f); fclose(f); } }

// A report is written a moment after it is asked for, so the status line requested with it is in the log.
static bool pending; static DWORD pendingAt; static std::string pendingNote, pendingStamp; static bool pendingFromPartner;
static void begin(const std::string& id,const std::string& note,bool fromPartner) {
    pending=true; pendingAt=GetTickCount(); pendingNote=note; pendingStamp=id; pendingFromPartner=fromPartner;
    CoopRequestStatus();
}
void ReportSave(const char* note) {
    std::string id=stamp();
    begin(id,note?note:"",false);
    // The partner's game makes the same report and sends its log back.
    std::string msg=id; NetSendReliable(MSG_REPORT_REQUEST,msg.data(),msg.size());
}
void ReportTick(CInstance* self) {
    if(!pending || GetTickCount()-pendingAt<300 || !self) return;
    pending=false;
    std::string dir=base+"coop-reports\\"; CreateDirectoryA(dir.c_str(),nullptr);
    dir+=pendingStamp+"\\"; CreateDirectoryA(dir.c_str(),nullptr);
    writeFile(dir+"my-log.txt",readTail(sessionPath,8*1024*1024));
    std::string state="Bug report "+pendingStamp+(pendingFromPartner?" (asked for by the partner)":"")+"\r\n"
        +"Build: "+std::to_string(HS_COOP_BUILD)+"\r\nNote: "+pendingNote+"\r\n"
        +"Session log: "+sessionPath+"\r\nThe current state is the last 'status' line in my-log.txt.\r\n";
    writeFile(dir+"report.txt",state);
    if(!saveWindow(dir+"screenshot.bmp")) CoopLog("Report: could not take a screenshot");
    lastFolder=dir;
    CoopLog("Report: saved to %s",dir.c_str());
}
void ReportOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    if(type==MSG_REPORT_REQUEST) {
        std::string id((const char*)data,size);
        if(id.empty() || id.size()>32 || id.find_first_not_of("0123456789-")!=std::string::npos) return;
        begin(id,"(the partner pressed report)",true);
        std::string log=readTail(sessionPath,PARTNER_LOG_MAX);
        std::string msg; msg.push_back((char)id.size()); msg+=id; msg+=log;
        NetSendReliable(MSG_REPORT_LOG,msg.data(),msg.size());
        CoopLog("Report: the partner asked for a bug report; sent them this log (%u bytes)",(unsigned)log.size());
    } else if(type==MSG_REPORT_LOG && size>1) {
        size_t n=data[0]; if(1+n>size) return;
        std::string id((const char*)data+1,n);
        if(id.find_first_not_of("0123456789-")!=std::string::npos) return;
        std::string dir=base+"coop-reports\\"; CreateDirectoryA(dir.c_str(),nullptr);
        dir+=id+"\\"; CreateDirectoryA(dir.c_str(),nullptr);
        writeFile(dir+"partner-log.txt",std::string((const char*)data+1+n,size-1-n));
        CoopLog("Report: the partner's log arrived (%u bytes)",(unsigned)(size-1-n));
    }
}
const char* ReportLastFolder() { return lastFolder.c_str(); }
