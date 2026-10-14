#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int ordinary_directory(const wchar_t *path) {
    DWORD attr=GetFileAttributesW(path);
    return attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY) && !(attr&FILE_ATTRIBUTE_REPARSE_POINT);
}
static int ordinary_file_or_absent(const wchar_t *path) {
    DWORD attr=GetFileAttributesW(path);
    if (attr==INVALID_FILE_ATTRIBUTES) return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND;
    return !(attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
int tf_video_exists(const wchar_t *videos,const char *file,wchar_t *path,size_t count) {
    wchar_t name[768];
    if (!ordinary_directory(videos) || !tf_safe_filename(file) || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,file,-1,name,768)) return 0;
    if (swprintf(path,count,L"%ls\\%ls",videos,name)<0) return 0;
    DWORD attr=GetFileAttributesW(path);
    return attr!=INVALID_FILE_ATTRIBUTES && !(attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
int tf_read_config_file(const wchar_t *path,TfConfig *config) {
    if (!ordinary_file_or_absent(path)) { tf_log("Configuration is unsafe or inaccessible: %ls",path); return -1; }
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file==INVALID_HANDLE_VALUE) {
        DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?0:-1;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file,&size) || size.QuadPart<0 || size.QuadPart>262144) { CloseHandle(file); return -1; }
    char *text=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(SIZE_T)size.QuadPart+1); DWORD count=0;
    int ok=text && ReadFile(file,text,(DWORD)size.QuadPart,&count,NULL) && count==(DWORD)size.QuadPart && !memchr(text,0,count);
    CloseHandle(file);
    char error[256]={0};
    if (ok) ok=tf_parse_config(text,config,error,sizeof(error));
    if (!ok) tf_log("Configuration rejected: %ls: %s",path,*error?error:"read failed or contains NUL bytes");
    if (text) HeapFree(GetProcessHeap(),0,text);
    return ok?1:-1;
}
int tf_same_filename(const char *a,const char *b) {
    wchar_t wanted[768],existing[768];
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,a,-1,wanted,768) &&
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,b,-1,existing,768) &&
        CompareStringOrdinal(wanted,-1,existing,-1,TRUE)==CSTR_EQUAL;
}
static int configured_file(const TfConfig *config,const char *file) {
    for (int i=0;i<config->count;++i)
        if (tf_same_filename(file,config->stations[i].file)) return 1;
    return 0;
}
static int write_bytes(HANDLE file,const char *text) {
    DWORD length=(DWORD)strlen(text),written=0;
    return WriteFile(file,text,length,&written,NULL) && written==length;
}
static void save_map(const wchar_t *path,const TfConfig *config) {
    wchar_t temporary[2048];
    if (swprintf(temporary,2048,L"%ls.tmp",path)<0 || !ordinary_file_or_absent(path) || !ordinary_file_or_absent(temporary)) {
        tf_log("Cannot save automatic frequency map: unsafe path"); return;
    }
    HANDLE file=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file==INVALID_HANDLE_VALUE) { tf_log("Cannot save automatic frequency map (Windows error %lu)",GetLastError()); return; }
    char line[2048];
    int ok=write_bytes(file,"; Generated automatically. Assignments stay stable while clear of game tuning ranges.\r\n"
        "; Put optional manual overrides in the mod's stations.ini.\r\n"
        "; Game signals and fine-tuning still have priority.\r\n[General]\r\nautoAssign=false\r\n");
    snprintf(line,sizeof(line),"tolerance=%.17g\r\nhysteresis=%.17g\r\nfadeMs=%u\r\n",config->tolerance,config->hysteresis,config->fade_ms);
    ok=ok && write_bytes(file,line);
    for (int i=0;i<config->count && ok;++i) {
        const TfStation *station=&config->stations[i];
        if (!station->automatic || !station->enabled) continue;
        snprintf(line,sizeof(line),"\r\n[%s]\r\nfile=\"%s\"\r\nfrequency=%.6f\r\nloop=true\r\nvolume=1.0\r\n",station->name,station->file,station->frequency);
        ok=write_bytes(file,line);
    }
    CloseHandle(file);
    /* This regenerable map needs atomic replacement, not a forced disk flush. */
    if (ok && MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING))
        tf_log("Automatic frequencies saved to Videos/stations-auto.ini");
    else { DeleteFileW(temporary); tf_log("Cannot save automatic frequency map; assignments are available in the log"); }
}
void tf_save_auto_map(const wchar_t *videos,const TfConfig *config) {
    wchar_t path[2048];
    if (config->auto_assign && swprintf(path,2048,L"%ls\\stations-auto.ini",videos)>=0) save_map(path,config);
}
/* An MKV whose audio the game can't read plays without sound, and nothing else shows why: said at discovery. The
 * tracks come before the first cluster, well inside the file's first 64 KB. */
static void check_mkv_sound(const wchar_t *path,const char *name) {
    enum { HEAD=65536 };
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file==INVALID_HANDLE_VALUE) return;
    unsigned char *head=HeapAlloc(GetProcessHeap(),0,HEAD); DWORD count=0; char codec[64];
    if (head && ReadFile(file,head,HEAD,&count,NULL) && !tf_mkv_audio(head,count,codec,sizeof(codec)))
        tf_log("No sound from Videos/%s: its audio is %s; the game's player reads MKV sound only in AAC, Opus, FLAC or MP3",name,codec);
    if (head) HeapFree(GetProcessHeap(),0,head);
    CloseHandle(file);
}
int tf_discover(const wchar_t *videos,TfConfig *config) {
    wchar_t pattern[2048],path[2048],map[2048];
    for (int i=0;i<config->count;++i) config->stations[i].enabled=tf_video_exists(videos,config->stations[i].file,path,2048);
    if (!ordinary_directory(videos) || swprintf(pattern,2048,L"%ls\\*",videos)<0 ||
        swprintf(map,2048,L"%ls\\stations-auto.ini",videos)<0) { tf_log("Videos folder missing or unsafe"); return 0; }
    /* Reuse prior assignments before considering new filenames, so adding/removing
     * unrelated videos does not move a surviving station to another frequency. */
    if (config->auto_assign) {
        TfConfig *saved=HeapAlloc(GetProcessHeap(),0,sizeof(*saved));
        if (saved) {
            if (tf_read_config_file(map,saved)==1) {
                for (int i=0;i<saved->count;++i) {
                    TfStation *station=&saved->stations[i];
                    if (!configured_file(config,station->file) && tf_video_exists(videos,station->file,path,2048))
                        tf_add_auto_station(config,station->file,station->frequency);
                }
            }
            HeapFree(GetProcessHeap(),0,saved);
        }
    }
    typedef char VideoName[TF_FILE_BYTES];
    VideoName *names=HeapAlloc(GetProcessHeap(),0,sizeof(VideoName)*TF_MAX_STATIONS);
    int candidates=0,count=0,unassigned=0;
    WIN32_FIND_DATAW found;
    HANDLE search=FindFirstFileW(pattern,&found);
    if (search!=INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            char name[TF_FILE_BYTES];
            if (!WideCharToMultiByte(CP_UTF8,0,found.cFileName,-1,name,sizeof(name),NULL,NULL) || !tf_safe_filename(name)) continue;
            ++count; tf_log("Discovered Videos/%s",name);
            if (!_stricmp(name+strlen(name)-4,".mkv") && swprintf(path,2048,L"%ls\\%ls",videos,found.cFileName)>=0)
                check_mkv_sound(path,name);
            if (!config->auto_assign || configured_file(config,name)) continue;
            ++unassigned;
            if (!names) continue;
            /* Keep at most 64 new candidates in deterministic filename order. */
            int at=0; while (at<candidates && strcmp(names[at],name)<0) ++at;
            if (at>=TF_MAX_STATIONS) continue;
            if (candidates<TF_MAX_STATIONS) ++candidates;
            for (int i=candidates-1;i>at;--i) strcpy(names[i],names[i-1]);
            strcpy(names[at],name);
        } while (FindNextFileW(search,&found));
        FindClose(search);
    }
    for (int i=0;i<candidates;++i)
        if (!configured_file(config,names[i]) && tf_add_auto_station(config,names[i],-1)>=0) --unassigned;
    if (names) HeapFree(GetProcessHeap(),0,names);
    for (int i=0;i<config->count;++i) {
        TfStation *station=&config->stations[i];
        tf_log("Station [%s] frequency %.6f loop=%d volume=%.2f file=%s%s%s",station->name,
            station->frequency,station->loop,station->volume,station->file,station->automatic?" (automatic)":"",
            station->enabled?"":" (missing or unsafe; disabled)");
    }
    if (unassigned) tf_log("%d videos unassigned: station limit or no nonoverlapping frequency space",unassigned);
    /* Save only after current-world ranges validate the assignments. */
    return count;
}
