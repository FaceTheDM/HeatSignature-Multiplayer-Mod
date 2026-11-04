#include "ModInterface.h"
#include <cstdio>
static int lines=0,subscriptions=0;
static void log(const char* prefix,const char* msg){++lines;printf("[%s] %s\n",prefix,msg);}
static void subscribe(const char*,HS_HookPostCallback,void*){++subscriptions;}
int main(int argc,char** argv){
 if(argc!=2)return 2;
 HMODULE dll=LoadLibraryA(argv[1]);if(!dll){printf("LoadLibrary failed %lu\n",GetLastError());return 1;}
 auto version=(HS_ModApiVersionFn)GetProcAddress(dll,"ModApiVersion");
 auto init=(HS_ModInitFn)GetProcAddress(dll,"ModInit");
 if(!version||!init||version()!=HS_API_VERSION)return 3;
 HS_ModApi api{};api.Log=log;api.SubscribeHookPost=subscribe;
 init(&api);
 if(lines!=2||subscriptions!=0)return 4;
 printf("PASS: x86 DLL loads, ABI=%u, exports resolve, unknown executable rejected before any hook\n",version());
 FreeLibrary(dll);return 0;
}
