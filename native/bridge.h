/* Standard Lua file handles access two local Windows pipes. No Lua ABI imports,
 * background server, networking, polling files or disk-backed frame packets. */
#ifndef TF_BRIDGE_H
#define TF_BRIDGE_H
#include <windows.h>
#include <stdint.h>

enum { TF_BRIDGE_INPUT=1, TF_BRIDGE_MARKERS=2, TF_BRIDGE_RANGES=3,
       TF_BRIDGE_MAX=4120, TF_BRIDGE_BUFFER=16384 };
typedef struct { char magic[8]; uint32_t kind,size; } TfBridgeHeader;
typedef struct { HANDLE input,output; wchar_t input_name[160],output_name[160]; int fault; } TfBridge;

static void tf_bridge_close(TfBridge *bridge) {
    if (bridge->input && bridge->input!=INVALID_HANDLE_VALUE) CloseHandle(bridge->input);
    if (bridge->output && bridge->output!=INVALID_HANDLE_VALUE) CloseHandle(bridge->output);
    bridge->input=bridge->output=INVALID_HANDLE_VALUE;
}
static int tf_bridge_create(TfBridge *bridge) {
    static unsigned generation;
    tf_bridge_close(bridge); bridge->fault=0;
    swprintf(bridge->input_name,160,L"\\\\.\\pipe\\TFVideoTuner-%lu-%llu-%u-in",
        GetCurrentProcessId(),(unsigned long long)GetTickCount64(),++generation);
    swprintf(bridge->output_name,160,L"%ls-out",bridge->input_name);
    /* NOWAIT bounds every native operation on the game thread. Each Lua handle
     * uses only one direction; an input burst fits before the synchronous tick. */
    bridge->input=CreateNamedPipeW(bridge->input_name,PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,
        TF_BRIDGE_BUFFER,TF_BRIDGE_BUFFER,0,NULL);
    bridge->output=CreateNamedPipeW(bridge->output_name,PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,
        TF_BRIDGE_BUFFER,TF_BRIDGE_BUFFER,0,NULL);
    if (bridge->input==INVALID_HANDLE_VALUE || bridge->output==INVALID_HANDLE_VALUE) {
        tf_bridge_close(bridge); return 0;
    }
    return 1;
}
/* 1 = complete bounded message, 0 = no complete message, -1 = bad/broken pipe. */
static int tf_bridge_receive(TfBridge *bridge,uint32_t *kind,void *data,uint32_t *size) {
    TfBridgeHeader header; DWORD copied=0,available=0,received=0;
    if (bridge->fault || bridge->input==INVALID_HANDLE_VALUE) return -1;
    if (!PeekNamedPipe(bridge->input,&header,sizeof(header),&copied,&available,NULL)) return -1;
    if (copied<sizeof(header)) return 0;
    if (memcmp(header.magic,"TFVPIPE1",8) || !header.size || header.size>TF_BRIDGE_MAX ||
        header.kind<TF_BRIDGE_INPUT || header.kind>TF_BRIDGE_RANGES) { bridge->fault=1; return -1; }
    if (available<sizeof(header)+header.size) return 0;
    if (!ReadFile(bridge->input,&header,sizeof(header),&received,NULL) || received!=sizeof(header) ||
        !ReadFile(bridge->input,data,header.size,&received,NULL) || received!=header.size) return -1;
    *kind=header.kind; *size=header.size; return 1;
}
static int tf_bridge_reply(TfBridge *bridge,const void *data,DWORD size) {
    DWORD sent=0;
    return bridge->output!=INVALID_HANDLE_VALUE && WriteFile(bridge->output,data,size,&sent,NULL) && sent==size;
}
#endif
