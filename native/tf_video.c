/* Standalone Lua-loaded native UE4SS mod. All engine calls are build-gated and made on
 * the game thread. Electra owns decoding, timestamp queues, frame dropping and GPU uploads.
 * Our audio threads own a separate strong native generator reference, never a game UObject.
 */
#include "platform.h"
#include "profile.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include "bridge.h"

enum { ACTIVE=1,PRIORITY=2,PAUSED=4,OBJECTS=8,BOUND=16,SHUTDOWN=128,MEDIA_FAILED=256 };
typedef struct {
    char magic[8]; uint64_t sequence,player,texture,sound,target,world,radio;
    double frequency; uint32_t flags,epoch,width,height; uint64_t original;
} Input;
typedef struct {
    char magic[8]; uint64_t sequence; uint32_t phase; int32_t station;
    uint32_t bind,reserved; uint64_t draws; int64_t position; uint32_t flags,error;
} Output;
_Static_assert(sizeof(Input)==96 && sizeof(Output)==56,"Lua packet ABI");
typedef struct { wchar_t *data; int32_t count,capacity; } FString;
/* FVector2D and FLinearColor go to the engine by pointer (by value in its signatures), and its code loads them with
 * aligned SSE moves (UImage::SetColorAndOpacity: movaps [rdx]): 16-byte alignment, wherever they are kept. */
typedef struct { _Alignas(16) double x; double y; } Vec2;
typedef struct { _Alignas(16) float r; float g,b,a; } Color;
_Static_assert(sizeof(Vec2)==16 && _Alignof(Vec2)==16 && sizeof(Color)==16 && _Alignof(Color)==16,"engine vector ABI");
/* The stations' marks on the game's dial (marker_style), and on Townfall Companion's phone dial (publish_marks). */
static const Color MARK_COLOUR={1,0,1,1};
typedef struct { int32_t index,serial; } WeakObject;
typedef struct { WeakObject source; unsigned char enemy,suppress,pad[2]; uint32_t state; float frequency; } SignalDisplay;
typedef struct { char magic[8]; uint32_t count,reserved; uint64_t sources[TF_MAX_STATIONS]; } MarkerPacket;
_Static_assert(sizeof(SignalDisplay)==20 && sizeof(MarkerPacket)==528,"PDB and marker packet ABI");
typedef struct {
    uint32_t device,padding; uint64_t component; float rate; int output_frames,channels,callback_frames;
    uint64_t instance; unsigned char preview,pad[7]; FString graph; float start; uint32_t trailing;
} GeneratorParams;
_Static_assert(sizeof(FString)==16 && sizeof(GeneratorParams)==72,"PDB-verified engine ABI");
typedef unsigned char (*BoolFn)(void *);
typedef void (*VoidFn)(void *);
typedef void (*SetFn)(void *,void *);
typedef void *(*TimeFn)(void *,int64_t *); /* MSVC FTimespan nontrivial return: hidden result after this. */
typedef void *(*CreateGeneratorFn)(void *,TfShared *,const GeneratorParams *);
typedef void (*CanvasBeginFn)(void *,void *,void **,Vec2 *,void **);
typedef void (*CanvasEndFn)(void *,void **);
typedef void (*CanvasClearFn)(void *,void *,const Color *);
typedef void (*CanvasDrawFn)(void *,void *,const Vec2 *,const Vec2 *,const Vec2 *,const Vec2 *,const Color *,int,float,const Vec2 *);
static unsigned char *image;
/* The game's engine functions and layouts, from Scripts/tf_video.profile (profile.h), for the exact game running. */
static TfProfile tf_profile;
#define PV(key) (tf_profile.values[TFV_##key])
#ifdef TF_VIDEO_TEST
static void *tf_test_functions[TF_FUNCTION_COUNT];
#define FN(which,type) ((type)tf_test_functions[which])
#else
#define FN(which,type) ((type)(image+tf_profile.functions[which].rva))
#endif
static HMODULE module;
static wchar_t folder[2048],videos[2048],log_path[2048],profile_path[2048];
static HANDLE log_file=INVALID_HANDLE_VALUE;
static TfBridge bridge={.input=INVALID_HANDLE_VALUE,.output=INVALID_HANDLE_VALUE};
static SRWLOCK log_lock=SRWLOCK_INIT;
static TfConfig config;
static TfController controller;
static Input incoming;
static int initialized,valid_profile,objects_initialized,audio_initialized,audio_backend;
static int config_ok;             /* stations.ini read, or absent: valid_profile needs it and a fitting profile */
static HANDLE profile_helper;     /* rundll32 running game_profile.c, while it reads the game's PDB */
static DWORD thread_id;
static uint32_t error_code;
static uint64_t draws;
static void *player,*texture,*sound,*target;
static int64_t last_draw_time=-1;
static float last_aspect;
static unsigned target_width,target_height;
static int source_width,source_height;
static unsigned target_format;
static int opened_station=-1,resume_submitted;
static int cached_station=-1,force_release;
static uint64_t cache_until;
static int64_t bookmarks[TF_MAX_STATIONS],resume_target;
static MarkerPacket marker_packet;
static int marker_packet_ready;
/* The game's tuning ranges, and its waypoints' range (the stations take it unless stations.ini sets a tolerance). */
typedef struct { char magic[8]; uint32_t epoch,count,valid,revision; double waypoint_tolerance; } RangeHeader;
_Static_assert(sizeof(RangeHeader)==32,"Lua ranges packet ABI");
static RangeHeader range_header;
static TfRange range_values[TF_MAX_RANGES];
static int range_packet_ready;
static uint32_t range_epoch,range_revision;
static int range_seen;
static WeakObject marker_sources[TF_MAX_STATIONS],marker_widget;
static int markers_loaded,marker_capacity,marker_extended,marker_count;
static uint64_t marker_refresh,timing_at,timing_draws,timing_frames;
static int64_t timing_position;
static int timing_station=-1;

void tf_log(const char *format,...) {
    char line[2048];
    int n=snprintf(line,sizeof(line),"[TF-VIDEO] %llu ",(unsigned long long)GetTickCount64());
    va_list args; va_start(args,format); vsnprintf(line+n,sizeof(line)-n-2,format,args); va_end(args);
    size_t length=strlen(line); line[length++]='\n'; line[length]=0;
    OutputDebugStringA(line);
    AcquireSRWLockExclusive(&log_lock);
    if (log_file!=INVALID_HANDLE_VALUE) { DWORD written; WriteFile(log_file,line,(DWORD)length,&written,NULL); }
    ReleaseSRWLockExclusive(&log_lock);
}
static int read_memory(const void *address,void *out,size_t size) {
    SIZE_T n=0;
    return address && ReadProcessMemory(GetCurrentProcess(),address,out,size,&n) && n==size;
}
static int validate_image(unsigned char *base) {
    if (!base) return 0;
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!read_memory(base,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x10000 ||
        !read_memory(base+dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.TimeDateStamp!=tf_profile.timestamp ||
        nt.OptionalHeader.SizeOfImage!=tf_profile.image_size || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    const uint32_t size=tf_profile.image_size;
    IMAGE_DATA_DIRECTORY dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (size<0x1000 || dir.VirtualAddress>size || dir.Size>size-dir.VirtualAddress) return 0;
    int identity=0;
    for (DWORD at=0;at+sizeof(IMAGE_DEBUG_DIRECTORY)<=dir.Size;at+=sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY entry; unsigned char record[24];
        if (!read_memory(base+dir.VirtualAddress+at,&entry,sizeof(entry))) return 0;
        if (entry.Type==IMAGE_DEBUG_TYPE_CODEVIEW && entry.SizeOfData>=24 && entry.AddressOfRawData<=size-24 &&
            read_memory(base+entry.AddressOfRawData,record,sizeof(record)) && !memcmp(record,tf_profile.identity,sizeof(record))) identity=1;
    }
    if (!identity) return 0;
    for (size_t i=0;i<TF_FUNCTION_COUNT;++i) {
        unsigned char code[16];
        if (tf_profile.functions[i].rva>size-16 || !read_memory(base+tf_profile.functions[i].rva,code,16) ||
            memcmp(code,tf_profile.functions[i].code,16)) { tf_log("Game function fingerprint differs: %s",tf_function_info[i].name); return 0; }
    }
    return 1;
}
/* Scripts/tf_video.profile into tf_profile, if it is a whole profile of this layout (not yet checked against the game). */
static int profile_read(const wchar_t *path) {
    TfProfile read={0}; DWORD count=0;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file==INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER length;
    int ok=GetFileSizeEx(file,&length) && length.QuadPart==(LONGLONG)sizeof(read) &&
        ReadFile(file,&read,sizeof(read),&count,NULL) && count==sizeof(read) &&
        !memcmp(read.magic,"TFVPROF1",8) && read.version==TF_PROFILE_VERSION && read.size==sizeof(read);
    CloseHandle(file);
    if (ok) tf_profile=read;
    return ok;
}
static int packet_read(void) {
    int got_input=0;
    for (int i=0;i<16;++i) {
        unsigned char bytes[TF_BRIDGE_MAX]; uint32_t kind=0,size=0;
        int result=tf_bridge_receive(&bridge,&kind,bytes,&size);
        if (result<0) { error_code=6; return 0; }
        if (!result) return got_input;
        if (kind==TF_BRIDGE_INPUT && size==sizeof(Input) && !memcmp(bytes,"TFVIDEO1",8)) {
            memcpy(&incoming,bytes,sizeof(incoming)); got_input=1;
        } else if (kind==TF_BRIDGE_MARKERS && size==sizeof(MarkerPacket) && !memcmp(bytes,"TFVMARK1",8)) {
            memcpy(&marker_packet,bytes,sizeof(marker_packet)); marker_packet_ready=1; markers_loaded=0;
        } else if (kind==TF_BRIDGE_RANGES && size>=sizeof(RangeHeader)) {
            RangeHeader header; memcpy(&header,bytes,sizeof(header));
            if (memcmp(header.magic,"TFVRANG2",8) || header.count>TF_MAX_RANGES || header.valid>1 ||
                size!=sizeof(header)+header.count*sizeof(TfRange)) { error_code=6; return 0; }
            range_header=header;
            memcpy(range_values,bytes+sizeof(header),header.count*sizeof(TfRange)); range_packet_ready=1;
        } else { error_code=6; return 0; }
    }
    error_code=6; return 0;
}
static void packet_write(void) {
    int64_t position=0;
    if (valid_profile && player && objects_initialized && (!thread_id || thread_id==GetCurrentThreadId())) FN(MEDIA_TIME,TimeFn)(player,&position);
    Output out={.sequence=incoming.sequence,.phase=controller.phase,.station=controller.station,
        .bind=controller.display,.reserved=(uint32_t)config.count,.draws=draws,.position=position,
        .flags=(valid_profile?1u:0u)|((uint32_t)(audio_backend&3)<<8),.error=error_code};
    memcpy(out.magic,"TFVSTAT1",8);
    tf_bridge_reply(&bridge,&out,sizeof(out));
}
static int load_config(void) {
    wchar_t path[2048]; swprintf(path,2048,L"%ls\\stations.ini",folder);
    int result=tf_read_config_file(path,&config);
    if (result<0) return 0;
    if (!result) {
        char error[256];
        tf_parse_config("",&config,error,sizeof(error));
        tf_log("stations.ini absent; automatically assigning video frequencies with defaults");
    }
    tf_discover(videos,&config);
    return 1;
}
static void reset_audio_generator(void) {
    if (!sound || !audio_backend || audio_backend<0) return;
    GeneratorParams params={.rate=(float)tf_audio_rate(),.channels=2,.output_frames=512,.callback_frames=512};
    TfShared owned={0};
    FN(SOUND_CREATE,CreateGeneratorFn)(sound,&owned,&params);
    tf_audio_generator(&owned,FN(GEN_AUDIO,TfGenerateAudio),FN(SHARED_RELEASE,TfReleaseShared),FN(GEN_END,TfEndAudio));
    audio_initialized=1;
    tf_log("Native media audio generator initialized (%d Hz stereo)",tf_audio_rate());
}
static void *studio_system(void) {
    if (!GetModuleHandleW(L"fmodstudio.dll") || !FN(FMOD_AVAILABLE,unsigned char (*)(void))()) return NULL;
    void *mod=FN(FMOD_GET,void *(*)(void))();
    return mod?FN(FMOD_SYSTEM,void *(*)(void *,int))(mod,PV(FMOD_RUNTIME)):NULL;
}
static void release_decoder(void) {
    if (player && objects_initialized) { FN(MEDIA_CLOSE,VoidFn)(player); FN(SOUND_UPDATE,VoidFn)(sound); }
    cached_station=-1; cache_until=0;
}
static int open_station(void *unused,int index) {
    (void)unused;
    wchar_t path[2048]; TfStation *station=&config.stations[index];
    if (cached_station>=0 && GetTickCount64()<cache_until &&
        tf_same_filename(station->file,config.stations[cached_station].file)) {
        cached_station=-1; cache_until=0; opened_station=index;
        resume_target=0; resume_submitted=0; incoming.flags&=~MEDIA_FAILED; error_code=0;
        last_draw_time=-1; last_aspect=0;
        if (audio_backend>0) reset_audio_generator();
        tf_log("Reusing paused decoder: %s",station->file);
        return 1;
    }
    if (cached_station>=0) release_decoder();
    if (!tf_video_exists(videos,station->file,path,2048)) { tf_log("Video missing: %s",station->file); return 0; }
    FString filename={path,(int32_t)wcslen(path)+1,(int32_t)wcslen(path)+1};
    incoming.flags&=~MEDIA_FAILED;
    last_draw_time=-1; last_aspect=0;
    tf_audio_flush();
    FN(MEDIA_LOOP,unsigned char (*)(void *,unsigned char))(player,(unsigned char)station->loop);
    int accepted=FN(MEDIA_OPEN,unsigned char (*)(void *,const FString *))(player,&filename);
    if (accepted) { opened_station=index; resume_target=bookmarks[index]; resume_submitted=0; error_code=0; }
    /* OnEndGenerate removes the generator's queue reference. A fresh generator on each
     * successful open reconnects it even when the MediaPlayer facade itself is reused. */
    if (accepted && audio_backend>0) reset_audio_generator();
    tf_log("%s [%s] %.6f: Videos/%s",accepted?"Opening":"Open rejected",station->name,station->frequency,station->file);
    return accepted;
}
static void close_station(void *unused) {
    (void)unused;
    int previous=opened_station;
    int keep=!force_release && !error_code && previous>=0 && !resume_target && config.warm_decoder_ms &&
        incoming.epoch==controller.epoch && !(incoming.flags&(PRIORITY|MEDIA_FAILED)) &&
        (controller.phase==TF_PAUSED || FN(MEDIA_PLAYING,BoolFn)(player)) &&
        (controller.phase==TF_PLAYING || controller.phase==TF_PAUSED || controller.phase==TF_FADING);
    if (player && objects_initialized && opened_station>=0 && !resume_target) {
        int64_t saved=0;
        FN(MEDIA_TIME,TimeFn)(player,&saved);
        if (saved>=0) {
            for (int i=0;i<config.count;++i)
                if (tf_same_filename(config.stations[i].file,config.stations[opened_station].file)) bookmarks[i]=saved;
            tf_log("Remembered %.3f seconds: %s",(double)saved/10000000.0,config.stations[opened_station].file);
        }
    }
    opened_station=-1; resume_target=0; resume_submitted=0;
    tf_audio_flush();
    if (keep && FN(MEDIA_RATE,unsigned char (*)(void *,float))(player,0)) {
        cached_station=previous; cache_until=GetTickCount64()+config.warm_decoder_ms;
        tf_log("Paused decoder retained for up to %u ms: %s",config.warm_decoder_ms,config.stations[previous].file);
    } else release_decoder();
    last_draw_time=-1;
    tf_log("Playback stopped; original CRTV texture requested");
}
static int set_rate(void *unused,float rate) { (void)unused; return FN(MEDIA_RATE,unsigned char (*)(void *,float))(player,rate); }
static int preparing(void *unused) {
    (void)unused;
    if (FN(MEDIA_PREPARING,BoolFn)(player)) return 1;
    if (resume_target>0) {
        void *facade=NULL;
        if (!read_memory((char *)player+PV(MEDIA_FACADE),&facade,sizeof(facade)) || !facade) return 1;
        if (!resume_submitted) {
            int64_t length=0; FN(MEDIA_DURATION,TimeFn)(player,&length);
            if (length<=0) return 1;
            if (resume_target>=length) resume_target=config.stations[opened_station].loop?resume_target%length:length-1;
            if (!resume_target) return 0;
            if (!FN(MEDIA_SEEK,unsigned char (*)(void *,const int64_t *))(facade,&resume_target)) return 1;
            resume_submitted=1;
        }
        /* Use the engine's pending-seek state rather than time equality.
         * GetTime can already be past the bookmark; an exact-time window can
         * otherwise leave a completed seek stuck until the open timeout. */
        if (FN(MEDIA_SEEKING,BoolFn)(facade)) return 1;
        int64_t current=0; FN(MEDIA_TIME,TimeFn)(player,&current);
        if (current<=0) return 1;
        tf_log("Resumed bookmark %.3f seconds at %.3f seconds: %s",(double)resume_target/10000000.0,(double)current/10000000.0,config.stations[opened_station].file);
        resume_target=0;
    }
    return 0;
}
static int playing(void *unused) { (void)unused; return FN(MEDIA_PLAYING,BoolFn)(player); }
static int failed(void *unused) { (void)unused; return !!(incoming.flags&MEDIA_FAILED); }
static int64_t position(void *unused) { (void)unused; int64_t t=0; FN(MEDIA_TIME,TimeFn)(player,&t); return t; }
static int64_t duration(void *unused) { (void)unused; int64_t t=0; FN(MEDIA_DURATION,TimeFn)(player,&t); return t; }
static const TfBackend media={NULL,open_station,close_station,set_rate,preparing,playing,failed,position,duration};

static int initialize_objects(void) {
    if (!(incoming.flags&OBJECTS) || !incoming.player || !incoming.texture || !incoming.sound || !incoming.target) return 0;
    if (objects_initialized) {
        /* Rooted objects are reused for the entire process. Never accept replacement addresses during a session. */
        return incoming.player==(uintptr_t)player && incoming.texture==(uintptr_t)texture &&
               incoming.sound==(uintptr_t)sound && incoming.target==(uintptr_t)target;
    }
    player=(void *)(uintptr_t)incoming.player; texture=(void *)(uintptr_t)incoming.texture;
    sound=(void *)(uintptr_t)incoming.sound; target=(void *)(uintptr_t)incoming.target;
    *(unsigned char *)((char *)player+PV(MEDIA_PLAY_ON_OPEN))=0;
    *(unsigned char *)((char *)player+PV(MEDIA_NATIVE_AUDIO))=0;
    *(unsigned char *)((char *)texture+PV(TEXTURE_NEW_STYLE))=1;
    *(int *)((char *)sound+PV(SOUND_CHANNELS))=PV(SOUND_STEREO);
    /* The unregistered mod-owned component needs its active flag for UpdatePlayer's sample sink.
     * Start/Activate is deliberately not called: that would create a second Unreal audio consumer. */
    *(unsigned char *)((char *)sound+PV(SOUND_ACTIVE))|=PV(SOUND_ACTIVE_MASK);
    FN(TEXTURE_SET,SetFn)(texture,player);
    FN(TEXTURE_UPDATE,VoidFn)(texture);
    FN(SOUND_SET,SetFn)(sound,player);
    objects_initialized=1;
    tf_log("Runtime MediaPlayer, MediaTexture and unregistered MediaSoundComponent acquired");
    return 1;
}
static void *weak_get(const WeakObject *weak) { return FN(WEAK_GET,void *(*)(const WeakObject *))(weak); }
static void markers_clear(void) {
    void *widget=weak_get(&marker_widget);
    if (widget) {
        for (int i=0;i<config.count;++i) {
            void *source=weak_get(&marker_sources[i]);
            if (source) FN(DIAL_REMOVE,SetFn)(widget,source);
        }
        int capacity=0;
        if (read_memory((char *)widget+PV(DIAL_CAPACITY),&capacity,4) && capacity==marker_extended)
            memcpy((char *)widget+PV(DIAL_CAPACITY),&marker_capacity,4);
    }
    memset(&marker_widget,0,sizeof(marker_widget));
    marker_capacity=marker_extended=0; marker_refresh=0;
}
static int markers_load(void) {
    if (!marker_packet_ready) return 0;
    MarkerPacket packet=marker_packet;
    if (packet.count!=(uint32_t)config.count || packet.count>TF_MAX_STATIONS) return 0;
    for (unsigned i=0;i<packet.count;++i) {
        void *source=(void *)(uintptr_t)packet.sources[i];
        if (!source || source==sound || source==player || source==texture || source==target) return 0;
        for (unsigned j=0;j<i;++j) if (packet.sources[i]==packet.sources[j]) return 0;
        /* FObjectPtr is an 8-byte value passed by address in this MSVC ABI.
         * The exact implementation allocates/validates the engine's object serial. */
        FN(WEAK_ASSIGN,void (*)(WeakObject *,void **))(&marker_sources[i],&source);
        if (weak_get(&marker_sources[i])!=source) return 0;
    }
    markers_loaded=1; return 1;
}
static void marker_style(void *widget,const WeakObject *source) {
    struct { void *data; int32_t count,capacity; } needles;
    if (!read_memory((char *)widget+PV(DIAL_NEEDLES),&needles,sizeof(needles)) ||
        !needles.data || needles.count<0 || needles.count>4096 || needles.count>needles.capacity) return;
    for (int i=0;i<needles.count;++i) {
        unsigned char entry[28];
        if (!read_memory((char *)needles.data+(size_t)i*sizeof(entry),entry,sizeof(entry))) return;
        if (memcmp(entry+PV(NEEDLE_SOURCE),source,sizeof(*source))) continue;
        WeakObject image_key,slot_key;
        memcpy(&image_key,entry+PV(NEEDLE_IMAGE),8); memcpy(&slot_key,entry+PV(NEEDLE_SLOT),8);
        void *marker_image=weak_get(&image_key),*slot=weak_get(&slot_key);
        if (marker_image) FN(IMAGE_COLOR,void (*)(void *,const Color *))(marker_image,&MARK_COLOUR);
        float x=0;
        if (slot && read_memory((char *)slot+PV(SLOT_X),&x,4) && isfinite(x)) {
            /* Use the game's original marker size and baseline. Frequency assignment
             * separates custom stations at the dial end, without a clipped extra row. */
            Vec2 location={x,0};
            FN(SLOT_POSITION,void (*)(void *,const Vec2 *))(slot,&location);
        }
        return;
    }
}
static void markers_update(int active) {
    if (!active || !incoming.radio) { markers_clear(); return; }
    if (!markers_loaded && !markers_load()) return;
    WeakObject current={0};
    if (!read_memory((char *)(uintptr_t)incoming.radio+PV(RADIO_DIAL),&current,sizeof(current))) { markers_clear(); return; }
    void *widget=weak_get(&current);
    if (!widget) { markers_clear(); return; }
    if (memcmp(&current,&marker_widget,sizeof(current))) {
        markers_clear();
        /* Widget initialization can lag behind raising the radio. Wait until its
         * canvas exists, and only extend presentation capacity for our own keys. */
        WeakObject panel={0};
        if (!read_memory((char *)widget+PV(DIAL_PANEL),&panel,sizeof(panel)) || !weak_get(&panel) ||
            !read_memory((char *)widget+PV(DIAL_CAPACITY),&marker_capacity,4) || marker_capacity<0 || marker_capacity>4096) return;
        marker_widget=current; marker_count=0;
        for (int i=0;i<config.count;++i) if (config.stations[i].enabled) ++marker_count;
        marker_extended=marker_capacity+marker_count;
        memcpy((char *)widget+PV(DIAL_CAPACITY),&marker_extended,4);
        tf_log("Dial markers attached: %d available video stations",marker_count);
    }
    uint64_t now=GetTickCount64();
    if (marker_refresh && now-marker_refresh<250) return;
    marker_refresh=now;
    for (int i=0;i<config.count;++i) {
        if (!config.stations[i].enabled) continue;
        SignalDisplay display={.source=marker_sources[i],.suppress=1,.state=PV(SIGNAL_DISCOVERED),.frequency=(float)config.stations[i].frequency};
        FN(DIAL_UPDATE,void (*)(void *,const SignalDisplay *))(widget,&display);
        marker_style(widget,&display.source);
    }
}
/* The stations as marks for Townfall Companion's phone dial, whole, whenever they change: %TEMP%\townfall-companion-
 * dial-marks.txt, a line "TFMARKS1 <count>", then "<frequency> <r> <g> <b>" a mark (linear RGB 0..1). None while
 * playback is off (no game profile yet, or none for this game). */
static void publish_marks(void) {
    wchar_t path[MAX_PATH+64];
    DWORD n=GetTempPathW(MAX_PATH,path);
    if (!n || n>=MAX_PATH) return;
    wcscat(path,L"townfall-companion-dial-marks.txt");
    char text[4096];
    int count=0;
    for (int i=0;i<config.count;++i) count+=valid_profile && config.stations[i].enabled;
    int used=snprintf(text,sizeof(text),"TFMARKS1 %d\n",count);
    for (int i=0;i<config.count && valid_profile && used>0 && used<(int)sizeof(text);++i) {
        if (!config.stations[i].enabled) continue;
        used+=snprintf(text+used,sizeof(text)-(size_t)used,"%.6f %.3f %.3f %.3f\n",config.stations[i].frequency,
            MARK_COLOUR.r,MARK_COLOUR.g,MARK_COLOUR.b);
    }
    if (used<=0 || used>=(int)sizeof(text)) return;
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return;
    DWORD written=0;
    WriteFile(file,text,(DWORD)used,&written,NULL);
    CloseHandle(file);
}
static void ranges_update(void) {
    RangeHeader header=range_header;
    int ok=range_packet_ready && header.epoch==incoming.epoch;
    if (range_seen && range_epoch==incoming.epoch && range_revision==(ok?header.revision:0)) return;
    /* Unless stations.ini sets a tolerance, a station reaches as far as the game's own waypoints (its videos and
     * transmissions): tolerance plus hysteresis is that range. */
    double reach=ok && header.valid ? header.waypoint_tolerance : 0;
    if (!config.tolerance_set && reach>config.hysteresis && reach<=0.1 && fabs(reach-config.hysteresis-config.tolerance)>1e-9) {
        config.tolerance=reach-config.hysteresis;
        tf_log("Stations tune like the game's CRTV signals: within %.4f of their frequency",reach);
    }
    int changed=tf_reassign_automatic(&config,ok?range_values:NULL,ok?(int)header.count:0,ok && header.valid);
    if (changed<0) changed=tf_reassign_automatic(&config,NULL,0,0);
    range_seen=1; range_epoch=incoming.epoch; range_revision=ok?header.revision:0;
    if (changed) {
        /* File indices stay stable, so bookmarks remain attached to the same file.
         * Release current playback before moving presentation-only markers. */
        force_release=1;
        if (opened_station>=0) close_station(NULL);
        else if (cached_station>=0) release_decoder();
        force_release=0;
        tf_controller_init(&controller); markers_clear(); marker_refresh=0;
        for (int i=0;i<config.count;++i) if (config.stations[i].automatic)
            tf_log("Automatic station %s: %.6f %s",config.stations[i].file,config.stations[i].frequency,
                config.stations[i].enabled?"(clear of game tuning ranges)":"(waiting for free tuning space)");
        if (ok && header.valid) tf_save_auto_map(videos,&config);
        publish_marks();
    }
}
static void log_timing(void) {
    uint64_t now=GetTickCount64(),frames=tf_audio_frames();
    int64_t current=0;
    if (controller.phase!=TF_PLAYING || !controller.display) { timing_at=0; return; }
    FN(MEDIA_TIME,TimeFn)(player,&current);
    if (!timing_at || timing_station!=controller.station || current<timing_position) {
        timing_at=now; timing_position=current; timing_station=controller.station; timing_frames=frames; timing_draws=draws; return;
    }
    if (now-timing_at<5000) return;
    double seconds=(double)(now-timing_at)/1000.0;
    tf_log("Playback timing: station=%d wall=%.3f media=%.3f rate=%.3f audio=%.3f draws=%.1f/s",
        controller.station,seconds,(double)(current-timing_position)/10000000.0,
        FN(MEDIA_GET_RATE,float (*)(void *))(player),(double)(frames-timing_frames)/tf_audio_rate(),(double)(draws-timing_draws)/seconds);
    timing_at=now; timing_position=current; timing_frames=frames; timing_draws=draws;
}
static int draw_frame(void) {
    if (!incoming.world || !target || !texture) return 0;
    int dimensions[2]={0}; float aspect=0;
    read_memory((char *)texture+PV(TEXTURE_DIMENSIONS),dimensions,sizeof(dimensions));
    read_memory((char *)texture+PV(TEXTURE_ASPECT),&aspect,sizeof(aspect));
    if (dimensions[0]<=2 || dimensions[1]<=2 || dimensions[0]>16384 || dimensions[1]>16384) return 0;
    if (!(aspect>0) || !isfinite(aspect)) aspect=(float)dimensions[0]/dimensions[1];
    int64_t time=position(NULL);
    unsigned w=incoming.width,h=incoming.height;
    if (!incoming.original) return 0;
    /* Read the original UV-space dimensions/format, then use a smaller private
     * output while retaining normalized placement in the screen material. */
    void *resource=FN(SOURCE_RESOURCE,void *(*)(void *))((void *)(uintptr_t)incoming.original);
    void *rhi=NULL; unsigned char format=0; int source_extent[2]={0};
    if (!resource || !read_memory((char *)resource+PV(TEXTURE_RHI),&rhi,sizeof(rhi)) || !rhi ||
        !read_memory((char *)rhi+PV(RHI_FORMAT),&format,1) ||
        !read_memory((char *)rhi+PV(RHI_EXTENT),source_extent,sizeof(source_extent))) return 0;
    if (source_extent[0]<64 || source_extent[0]>2048 || source_extent[1]<64 || source_extent[1]>2048 ||
        (format!=PV(PIXEL_BGRA) && format!=PV(PIXEL_RGBA))) return 0;
    w=config.output_width; h=config.output_height;
    if (source_width!=source_extent[0] || source_height!=source_extent[1]) {
        source_width=source_extent[0]; source_height=source_extent[1]; last_draw_time=-1;
    }
    if (w!=target_width || h!=target_height || format!=target_format) {
        FN(TARGET_INIT,void (*)(void *,unsigned,unsigned,int,unsigned char))(target,w,h,format,0);
        target_width=w; target_height=h; target_format=format; last_draw_time=-1;
        tf_log("Reusable CRTV output target %ux%u pixelFormat=%u (%.2f MiB color buffer)",w,h,format,(double)w*h*4/1048576.0);
    }
    if (time==last_draw_time && aspect==last_aspect) return 1;
    void *world=(void *)(uintptr_t)incoming.world,*canvas=NULL,*context=NULL;
    Color black={0,0,0,1},white={1,1,1,1};
    if (last_draw_time<0 || aspect!=last_aspect) FN(CANVAS_CLEAR,CanvasClearFn)(world,target,&black);
    Vec2 size={0};
    FN(CANVAS_BEGIN,CanvasBeginFn)(world,target,&canvas,&size,&context);
    if (!canvas) { if (context) FN(CANVAS_END,CanvasEndFn)(world,&context); return 0; }
    /* Preserve placement in the game's original normalized screen UVs even when
     * the private output has a different aspect ratio (e.g. 640x480 vs 2048x2048). */
    TfRect rect=tf_fit(aspect,source_extent[0],source_extent[1]);
    rect.x*=size.x/source_extent[0]; rect.width*=size.x/source_extent[0];
    rect.y*=size.y/source_extent[1]; rect.height*=size.y/source_extent[1];
    Vec2 origin={rect.x,rect.y},extent={rect.width,rect.height},uv0={0,0},uv1={1,1},pivot={0.5,0.5};
    FN(CANVAS_DRAW,CanvasDrawFn)(canvas,texture,&origin,&extent,&uv0,&uv1,&white,PV(BLEND_OPAQUE),0,&pivot);
    FN(CANVAS_END,CanvasEndFn)(world,&context);
    last_draw_time=time; last_aspect=aspect; ++draws; return 1;
}

static int bridge_setup(void) {
    if (!tf_bridge_create(&bridge)) { error_code=6; tf_log("Cannot create local memory bridge"); return 0; }
    wchar_t path[2048]; swprintf(path,2048,L"%ls\\video-bridge.ini",folder);
    char names[400];
    int n=snprintf(names,sizeof(names),"%ls\n%ls\n",bridge.input_name,bridge.output_name);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD written=0;
    int ok=file!=INVALID_HANDLE_VALUE && n>0 && (size_t)n<sizeof(names) && WriteFile(file,names,(DWORD)n,&written,NULL) && written==(DWORD)n;
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok) { tf_bridge_close(&bridge); error_code=6; return 0; }
    marker_packet_ready=range_packet_ready=range_seen=0;
    return 1;
}
/* Quick tuning (Q/E, D-pad, the phone) only knows the game's signals: after its jump, the needle goes to a video station
 * instead when one is nearer that way, set the way the game sets it (SetTunedFrequency_Internal, then the CRTV's
 * jump). Installed once a fitting profile is there; the game thread calls it, as it calls the ticks. */
#ifndef TF_VIDEO_TEST
#include "MinHook.h"
static void (*quick_tune_original)(void *,unsigned char);
static void quick_tune_detour(void *radio,unsigned char up) {
    float before=0,after=0;
    int known=valid_profile && read_memory((char *)radio+PV(RADIO_FREQUENCY),&before,4);
    quick_tune_original(radio,up);
    if (!known || !valid_profile || !read_memory((char *)radio+PV(RADIO_FREQUENCY),&after,4)) return;
    double station=tf_quick_tune(&config,before,after,up);
    if (station<0) return;
    FN(SET_FREQUENCY,void (*)(void *,float,unsigned char,unsigned char))(radio,(float)station,1,0);
    void *widget=FN(RADIO_WIDGET,void *(*)(void *))(radio);
    if (widget) FN(QUICK_TUNE_JUMP,VoidFn)(widget);
}
static void quick_tune_hook(void) {
    static int installed;
    if (installed || !valid_profile) return;
    installed=1;
    void *quick_tune=image+tf_profile.functions[QUICK_TUNE].rva;
    MH_STATUS ready=MH_Initialize();
    if ((ready==MH_OK || ready==MH_ERROR_ALREADY_INITIALIZED) &&
        MH_CreateHook(quick_tune,(void *)quick_tune_detour,(void **)&quick_tune_original)==MH_OK && MH_EnableHook(quick_tune)==MH_OK)
        tf_log("Quick tuning stops at the video stations too");
    else tf_log("Quick tuning can't stop at the video stations: its hook didn't install");
}
#else
static void quick_tune_hook(void) {}
#endif
/* The profile written for this game, checked against the running game. */
static int profile_fits(void) { return profile_read(profile_path) && validate_image(image); }
/* No fitting profile (a game update, or a first start without one): this DLL reads the game's PDB in a process of
 * its own, through Windows' rundll32.exe (game_profile.c), at low priority, once per game run, and that process logs
 * its outcome itself; profile_poll takes the profile up when it has finished. */
static void profile_helper_start(void) {
    wchar_t game[2048],dll[2048],system[MAX_PATH],rundll[MAX_PATH+16],command[8192];
    STARTUPINFOW startup={.cb=sizeof(startup)}; PROCESS_INFORMATION started;
    UINT length=GetSystemDirectoryW(system,MAX_PATH);
    if (!GetModuleFileNameW((HMODULE)image,game,2048) || !GetModuleFileNameW(module,dll,2048) || !length || length>=MAX_PATH ||
        swprintf(rundll,MAX_PATH+16,L"%ls\\rundll32.exe",system)<0 ||
        swprintf(command,8192,L"\"%ls\" \"%ls\",tf_video_profile \"%ls\" \"%ls\" --log \"%ls\"",rundll,dll,game,profile_path,log_path)<0) return;
    if (!CreateProcessW(rundll,command,NULL,NULL,FALSE,BELOW_NORMAL_PRIORITY_CLASS|CREATE_NO_WINDOW,NULL,NULL,&startup,&started)) {
        tf_log("Cannot start reading the game's symbols (Windows error %lu): no engine calls will be made",GetLastError());
        return;
    }
    CloseHandle(started.hThread); profile_helper=started.hProcess;
    tf_log("No game profile for this game version yet: reading the game's symbols (a few seconds)");
}
static void profile_poll(void) {
    if (!profile_helper || WaitForSingleObject(profile_helper,0)!=WAIT_OBJECT_0) return;
    DWORD code=1; GetExitCodeProcess(profile_helper,&code); CloseHandle(profile_helper); profile_helper=NULL;
    if (code || !profile_fits()) {
        if (!code) tf_log("The new game profile doesn't fit the running game: no engine calls will be made");
        error_code=config_ok?1:2; return;
    }
    tf_log("Game profile ready: the stations are on");
    if (!(valid_profile=config_ok)) return;
    error_code=0; tf_reassign_automatic(&config,NULL,0,0); publish_marks(); quick_tune_hook();
}
__declspec(dllexport) int tf_video_info(void *lua_state) { (void)lua_state; profile_poll(); packet_write(); return 0; }
__declspec(dllexport) int tf_video_init(void *lua_state) {
    (void)lua_state;
    if (initialized) { bridge_setup(); return 0; }
    uint64_t began=GetTickCount64();
    initialized=1;
    DWORD n=GetModuleFileNameW(module,folder,2048); wchar_t *end=wcsrchr(folder,L'\\');
    if (!n || n>=1900 || !end) return 0;
    *end=0; end=wcsrchr(folder,L'\\'); if (!end) return 0; *end=0;
    swprintf(videos,2048,L"%ls\\Videos",folder);
    swprintf(profile_path,2048,L"%ls\\Scripts\\tf_video.profile",folder);
    /* This game's log, and the one before it (what led to a crash survives the restart after it). */
    wchar_t previous[2048];
    swprintf(log_path,2048,L"%ls\\tf-video.log",folder); swprintf(previous,2048,L"%ls\\tf-video.previous.log",folder);
    MoveFileExW(log_path,previous,MOVEFILE_REPLACE_EXISTING);
    log_file=CreateFileW(log_path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    tf_controller_init(&controller);
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&tf_video_init,&pinned);
    tf_log("DLL initialization: native Unreal Electra playback, checked against this game's profile");
    image=(unsigned char *)GetModuleHandleW(L"Townfall-Win64-Shipping.exe");
    int fits=profile_fits();
    config_ok=load_config();
    if (!fits && image) profile_helper_start();
    valid_profile=fits && config_ok;
    error_code=!config_ok?2:!fits?(profile_helper?7:1):0;
    if (valid_profile) { tf_reassign_automatic(&config,NULL,0,0); quick_tune_hook(); }
    publish_marks();
    bridge_setup();
    tf_log("Native setup completed in %llu ms; media files open only when tuned",
        (unsigned long long)(GetTickCount64()-began));
    return 0;
}
__declspec(dllexport) int tf_video_tick(void *lua_state) {
    (void)lua_state;
    if (!initialized) return 0;
    if (!thread_id) thread_id=GetCurrentThreadId();
    if (thread_id!=GetCurrentThreadId()) { error_code=3; packet_write(); return 0; }
    int good=packet_read();
    if (cached_station>=0 && (GetTickCount64()>=cache_until || incoming.epoch!=controller.epoch || !good)) {
        release_decoder(); tf_log("Paused decoder cache released");
    }
    if (!good || !valid_profile) {
        if (valid_profile) markers_clear();
        if (valid_profile && objects_initialized) tf_controller_step(&controller,&config,&media,0,0,1,0,controller.epoch,GetTickCount64());
        controller.display=0; tf_audio_control(0,0,GetTickCount64(),0); packet_write(); return 0;
    }
    if (incoming.flags&SHUTDOWN) {
        markers_clear(); markers_loaded=0;
        force_release=1; if (objects_initialized) close_station(NULL); force_release=0;
        tf_controller_init(&controller); tf_audio_shutdown(); audio_initialized=audio_backend=0;
        tf_log("Decoder and audio cleanup complete (rooted media objects remain reusable until game exit)");
        packet_write(); return 0;
    }
    if (!initialize_objects()) {
        markers_clear();
        if (objects_initialized) tf_controller_step(&controller,&config,&media,0,0,1,0,controller.epoch,GetTickCount64());
        tf_audio_control(0,0,GetTickCount64(),0); controller.display=0; error_code=4; packet_write(); return 0;
    }
    error_code=0;
    int priority=!!(incoming.flags&PRIORITY),active=!!(incoming.flags&ACTIVE);
    if (incoming.radio) {
        unsigned char fine=1,radio_active=0,story=1; float strength=1;
        const char *radio=(const char *)(uintptr_t)incoming.radio;
        if (!read_memory(radio+PV(RADIO_FINE),&fine,1) || !read_memory(radio+PV(RADIO_ACTIVE),&radio_active,1) ||
            !read_memory(radio+PV(RADIO_STRENGTH),&strength,4) || !read_memory(radio+PV(RADIO_STORY),&story,1)) priority=1;
        if (fine || story || !isfinite(strength) || strength>0) priority=1;
        active=active && radio_active;
    } else active=0;
    /* Markers are immediately available on the dial, independent of decoding,
     * audio acquisition and game signal priority. Playback still yields below. */
    ranges_update();
    markers_update(active);
    if (incoming.epoch!=controller.epoch && audio_initialized) {
        tf_audio_shutdown(); audio_initialized=audio_backend=0;
    }
    /* Prepare the single audio consumer before opening media so the opening seconds are not lost. */
    if (active && !priority && tf_choose_station(&config,controller.station,incoming.frequency,0)>=0) {
        audio_backend=tf_audio_prepare(studio_system());
        if (!audio_backend) active=0;
    }
    TfPhase before=controller.phase; int previous_station=controller.station;
    tf_controller_step(&controller,&config,&media,incoming.frequency,active,priority,!!(incoming.flags&PAUSED),incoming.epoch,GetTickCount64());
    if (before!=controller.phase || previous_station!=controller.station) tf_log("Frequency %.6f station=%d phase=%d gamePriority=%d",
        incoming.frequency,controller.station,controller.phase,priority);
    if (controller.phase==TF_ERROR) error_code=5;
    if (controller.station>=0 && controller.phase!=TF_ERROR && controller.phase!=TF_ENDED) {
        FN(SOUND_UPDATE,VoidFn)(sound);
    }
    if (controller.display && !draw_frame()) controller.display=0;
    int audio_allowed=controller.audible && ((controller.display && (incoming.flags&BOUND)) || controller.phase==TF_FADING);
    tf_audio_control(controller.gain,audio_allowed,GetTickCount64(),config.fade_ms);
    log_timing();
    packet_write(); return 0;
}
__declspec(dllexport) int tf_video_shutdown(void *lua_state) {
    (void)lua_state;
    if (initialized && (!thread_id || thread_id==GetCurrentThreadId())) {
        if (valid_profile) { markers_clear(); markers_loaded=0; }
        force_release=1; if (valid_profile && objects_initialized) close_station(NULL); force_release=0;
        tf_controller_init(&controller); tf_audio_shutdown(); audio_initialized=audio_backend=0;
        range_seen=0;
    }
    return 0;
}
/* Diagnostics also used by the binary tests. No UE4SS/Lua dependency. */
__declspec(dllexport) int tf_video_validate(void *mapped_image,const wchar_t *profile) {
    return profile_read(profile) && validate_image(mapped_image);
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)reserved;
    if (reason==DLL_PROCESS_ATTACH) { module=instance; DisableThreadLibraryCalls(instance); }
    /* Never wait, call Unreal/FM0D, or destroy thread resources under the loader lock.
     * Explicit shutdown runs on the game thread; Windows reclaims process-exit resources. */
    return TRUE;
}
