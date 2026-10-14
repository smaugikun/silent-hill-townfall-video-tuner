/* FMOD's documented C ABI, loaded only from the game's already loaded DLLs.
 * No hook on Studio::update: the verified game module hands us the runtime Studio system.
 * The media component is never registered with Unreal's audio device; one consumer owns its generator.
 */
#include "platform.h"
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define AUDIO_BLOCK 8192
typedef int FmResult;
typedef struct { void *instance; } FmState;
typedef FmResult (*FmRead)(FmState *,float *,float *,unsigned,int,int *);
typedef FmResult (*FmProcessCheck)(FmState *,int,unsigned,unsigned,int,int);
typedef struct {
    unsigned pluginsdkversion; char name[32]; unsigned version; int numinputbuffers,numoutputbuffers;
    void *create,*release,*reset; FmRead read; void *process,*setposition; int numparameters;
    void **paramdesc; void *setparameterfloat,*setparameterint,*setparameterbool,*setparameterdata;
    void *getparameterfloat,*getparameterint,*getparameterbool,*getparameterdata;
    FmProcessCheck shouldiprocess; void *userdata,*sys_register,*sys_deregister,*sys_mix;
} FmDescription;
_Static_assert(sizeof(FmDescription)==216,"FMOD 2 x64 DSP description");
static struct {
    FmResult (*get_core)(void *,void **);
    FmResult (*get_bus)(void *,const char *,void **);
    FmResult (*lock_bus)(void *),(*unlock_bus)(void *);
    FmResult (*get_group)(void *,void **);
    FmResult (*get_version)(void *,unsigned *,unsigned *);
    FmResult (*get_format)(void *,int *,int *,int *);
    FmResult (*create_dsp)(void *,const FmDescription *,void **);
    FmResult (*add_dsp)(void *,int,void *),(*remove_dsp)(void *,void *),(*release_dsp)(void *);
} fm;
static SRWLOCK generator_lock=SRWLOCK_INIT;
static TfShared generator;
static TfGenerateAudio generate_audio;
static TfReleaseShared release_shared;
static TfEndAudio end_audio;
static _Atomic float requested_gain;
static _Atomic int enabled;
static _Atomic int reset_gain;
static _Atomic unsigned ramp_samples=9600;
static _Atomic uint64_t heartbeat,frames_out;
static _Atomic int backend;
static int output_rate=48000,imports_ready;
static void *bus,*group,*dsp;
static int bus_locked,bus_shared;
static uint64_t first_try,last_try;
static const char *last_failure;
static int last_result;
static float current_gain; /* only the current backend's one audio thread touches it */
static HANDLE fallback_thread, fallback_event, fallback_stop;

static void pull(float *stereo,unsigned frames) {
    memset(stereo,0,frames*2*sizeof(float));
    if (!atomic_load(&enabled) || GetTickCount64()-atomic_load(&heartbeat)>250) return;
    if (!TryAcquireSRWLockShared(&generator_lock)) return;
    if (generator.object && generate_audio) {
        int n=generate_audio(generator.object,stereo,(int)frames*2);
        if (n<0 || n>(int)frames*2) memset(stereo,0,frames*2*sizeof(float));
        else if (n<(int)frames*2) memset(stereo+n,0,((int)frames*2-n)*sizeof(float));
        atomic_fetch_add(&frames_out,frames);
    }
    ReleaseSRWLockShared(&generator_lock);
}
static float target_gain(void) {
    return atomic_load(&enabled) && GetTickCount64()-atomic_load(&heartbeat)<=250?atomic_load(&requested_gain):0;
}
static FmResult read_audio(FmState *state,float *in,float *out,unsigned length,int inchannels,int *outchannels) {
    (void)state;
    int channels=*outchannels>0?*outchannels:inchannels;
    if (channels<=0 || channels>32 || inchannels<0 || inchannels>32) return 0;
    float stereo[AUDIO_BLOCK*2];
    unsigned offset=0;
    while (offset<length) {
        unsigned n=length-offset; if (n>AUDIO_BLOCK) n=AUDIO_BLOCK;
        pull(stereo,n);
        if (atomic_exchange(&reset_gain,0)) current_gain=0;
        tf_mix(out+(size_t)offset*channels,in?in+(size_t)offset*inchannels:NULL,inchannels,channels,stereo,n,
               &current_gain,target_gain(),atomic_load(&ramp_samples));
        offset+=n;
    }
    *outchannels=channels; return 0;
}
static FmResult always_process(FmState *state,int idle,unsigned length,unsigned mask,int channels,int mode) {
    (void)state; (void)idle; (void)length; (void)mask; (void)channels; (void)mode; return 0;
}
static const FmDescription description={
    .pluginsdkversion=110,.name="TF Video PCM",.version=1,.numinputbuffers=1,.numoutputbuffers=1,
    .read=read_audio,.shouldiprocess=always_process,
};
static int imports(void) {
    HMODULE core=GetModuleHandleW(L"fmod.dll"),studio=GetModuleHandleW(L"fmodstudio.dll");
    if (!core || !studio) return 0;
    struct { HMODULE module; const char *name; void **out; } list[]={
        {studio,"FMOD_Studio_System_GetCoreSystem",(void **)&fm.get_core},
        {studio,"FMOD_Studio_System_GetBus",(void **)&fm.get_bus},
        {studio,"FMOD_Studio_Bus_LockChannelGroup",(void **)&fm.lock_bus},
        {studio,"FMOD_Studio_Bus_UnlockChannelGroup",(void **)&fm.unlock_bus},
        {studio,"FMOD_Studio_Bus_GetChannelGroup",(void **)&fm.get_group},
        {core,"FMOD_System_GetVersion",(void **)&fm.get_version},
        {core,"FMOD_System_GetSoftwareFormat",(void **)&fm.get_format},
        {core,"FMOD_System_CreateDSP",(void **)&fm.create_dsp},
        {core,"FMOD_ChannelGroup_AddDSP",(void **)&fm.add_dsp},
        {core,"FMOD_ChannelGroup_RemoveDSP",(void **)&fm.remove_dsp},
        {core,"FMOD_DSP_Release",(void **)&fm.release_dsp},
    };
    for (size_t i=0;i<sizeof(list)/sizeof(list[0]);++i) {
        *list[i].out=(void *)GetProcAddress(list[i].module,list[i].name);
        if (!*list[i].out) { tf_log("FMOD export missing: %s",list[i].name); return 0; }
    }
    return 1;
}
static void detach(void) {
    if (dsp && group && fm.remove_dsp) fm.remove_dsp(group,dsp);
    if (dsp && fm.release_dsp) fm.release_dsp(dsp);
    if (bus_locked && fm.unlock_bus) fm.unlock_bus(bus);
    dsp=group=bus=NULL; bus_locked=bus_shared=0;
}
#define WAVE_FRAMES 960
#define WAVE_BUFFERS 4
static void audio_failure(const char *stage,int result) {
    if (stage!=last_failure || result!=last_result) {
        tf_log("FMOD pending: %s (%d)",stage,result);
        last_failure=stage; last_result=result;
    }
}
static DWORD WINAPI windows_audio(void *unused) {
    (void)unused;
    /* Audio consumption drives Electra's clock. Give this thread Windows' audio
     * scheduling and 80 ms of bounded device buffering to survive game-frame stalls. */
    HMODULE avrt=LoadLibraryExW(L"avrt.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    HANDLE (WINAPI *set_audio_task)(LPCWSTR,LPDWORD)=avrt?(void *)GetProcAddress(avrt,"AvSetMmThreadCharacteristicsW"):NULL;
    BOOL (WINAPI *revert_audio_task)(HANDLE)=avrt?(void *)GetProcAddress(avrt,"AvRevertMmThreadCharacteristics"):NULL;
    DWORD task=0; HANDLE scheduling=set_audio_task && revert_audio_task?set_audio_task(L"Audio",&task):NULL;
    if (!scheduling) SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
    WAVEFORMATEX format={.wFormatTag=WAVE_FORMAT_PCM,.nChannels=2,.nSamplesPerSec=48000,
        .nAvgBytesPerSec=192000,.nBlockAlign=4,.wBitsPerSample=16};
    HWAVEOUT device=NULL;
    MMRESULT result=waveOutOpen(&device,WAVE_MAPPER,&format,(DWORD_PTR)fallback_event,0,CALLBACK_EVENT);
    if (result!=MMSYSERR_NOERROR) {
        tf_log("Windows audio unavailable (%u)",result); backend=-1;
        if (scheduling) revert_audio_task(scheduling);
        if (avrt) FreeLibrary(avrt);
        return 0;
    }
    WAVEHDR headers[WAVE_BUFFERS]={0}; int16_t samples[WAVE_BUFFERS][WAVE_FRAMES*2];
    int prepared[WAVE_BUFFERS]={0};
    for (int i=0;i<WAVE_BUFFERS;++i) {
        headers[i].lpData=(LPSTR)samples[i]; headers[i].dwBufferLength=sizeof(samples[i]);
        if (waveOutPrepareHeader(device,&headers[i],sizeof(WAVEHDR))!=MMSYSERR_NOERROR) goto cleanup;
        prepared[i]=1; headers[i].dwFlags|=WHDR_DONE;
    }
    HANDLE events[2]={fallback_stop,fallback_event};
    int was_enabled=0,next=0;
    tf_log("Windows audio: %s scheduling, %u ms bounded PCM buffer",scheduling?"MMCSS Audio":"above-normal",WAVE_FRAMES*WAVE_BUFFERS/48);
    while (WaitForSingleObject(fallback_stop,0)!=WAIT_OBJECT_0) {
        int is_enabled=atomic_load(&enabled);
        if (was_enabled!=is_enabled) { waveOutReset(device); next=0; }
        was_enabled=is_enabled;
        for (int filled=0;filled<WAVE_BUFFERS && (headers[next].dwFlags&WHDR_DONE);++filled) {
            int i=next;
            float stereo[WAVE_FRAMES*2],mixed[WAVE_FRAMES*2];
            pull(stereo,WAVE_FRAMES);
            if (atomic_exchange(&reset_gain,0)) current_gain=0;
            tf_mix(mixed,NULL,0,2,stereo,WAVE_FRAMES,&current_gain,target_gain(),atomic_load(&ramp_samples));
            for (unsigned s=0;s<WAVE_FRAMES*2;++s) samples[i][s]=(int16_t)(fmaxf(-1,fminf(1,mixed[s]))*32767);
            if (waveOutWrite(device,&headers[i],sizeof(WAVEHDR))!=MMSYSERR_NOERROR) goto cleanup;
            next=(next+1)%WAVE_BUFFERS;
        }
        if (WaitForMultipleObjects(2,events,FALSE,30)==WAIT_OBJECT_0) break;
    }
cleanup:
    waveOutReset(device);
    for (int i=0;i<WAVE_BUFFERS;++i) if (prepared[i]) waveOutUnprepareHeader(device,&headers[i],sizeof(WAVEHDR));
    waveOutClose(device);
    if (scheduling) revert_audio_task(scheduling);
    if (avrt) FreeLibrary(avrt);
    return 0;
}
int tf_audio_prepare(void *studio) {
    if (backend==1 && bus_shared) {
        void *current=NULL;
        if (fm.get_group(bus,&current) || current!=group) {
            atomic_store(&enabled,0); detach(); backend=0; first_try=last_try=0;
            tf_log("Shared FMOD channel group changed; reacquiring audio route");
        }
    }
    if (backend) return backend;
    uint64_t now=GetTickCount64();
    if (!first_try) first_try=now;
    if (now-last_try<200) return 0;
    last_try=now;
    if (!imports_ready) imports_ready=imports();
    if (imports_ready && studio) {
        void *core=NULL; unsigned version=0,build=0; int mode=0,channels=0;
        int result=fm.get_core(studio,&core);
        if (result || !core) audio_failure("core system",result);
        else if ((result=fm.get_version(core,&version,&build)) || (version>>16)!=2) audio_failure("core version",result?result:(int)version);
        else if ((result=fm.get_format(core,&output_rate,&mode,&channels)) || output_rate<8000 || output_rate>192000) audio_failure("software format",result?result:output_rate);
        else {
            if (!bus) {
                result=fm.get_bus(studio,"bus:/Cinematics/Sequences/CRTV_Videos",&bus);
                if (result || !bus) { audio_failure("CRTV_Videos bus lookup",result); bus=NULL; }
                else {
                    result=fm.lock_bus(bus);
                    if (!result) bus_locked=1;
                    /* FMOD 2.03 FMOD_ERR_ALREADY_LOCKED=78: another owner
                     * already holds this bus. Borrow the group, never unlock it. */
                    else if (result==78) { bus_shared=1; tf_log("Using existing CRTV_Videos bus lock; external owner retains it"); }
                    else { audio_failure("CRTV_Videos bus lock",result); bus=NULL; }
                }
            }
            if ((bus_locked || bus_shared) && ((result=fm.get_group(bus,&group)) || !group)) audio_failure("CRTV_Videos channel group",result);
            else if (bus_locked || bus_shared) {
                if (!fm.create_dsp(core,&description,&dsp) && dsp && !fm.add_dsp(group,-3,dsp)) {
                    backend=1; tf_log("FMOD initialized: CRTV_Videos bus, %d Hz, DSP before effects",output_rate);
                    return backend;
                }
                tf_log("FMOD DSP creation/attachment failed; using Windows audio");
                detach(); first_try=now-2001;
            }
        }
    } else audio_failure(imports_ready?"game Studio system unavailable":"game FMOD DLLs unavailable",0);
    if (now-first_try<2000) return 0;
    detach(); output_rate=48000;
    fallback_stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    fallback_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    backend=2;
    if (fallback_stop && fallback_event) fallback_thread=CreateThread(NULL,0,windows_audio,NULL,0,NULL);
    if (!fallback_thread) backend=-1;
    tf_log("Audio backend %d: Windows PCM fallback",backend);
    return backend;
}
int tf_audio_rate(void) { return output_rate; }
void tf_audio_generator(TfShared *owned,TfGenerateAudio gen,TfReleaseShared release,TfEndAudio end) {
    atomic_store(&enabled,0);
    AcquireSRWLockExclusive(&generator_lock);
    if (generator.object && end_audio) end_audio(generator.object);
    if (generator.reference && release_shared) release_shared(&generator);
    generator=*owned; memset(owned,0,sizeof(*owned));
    generate_audio=gen; release_shared=release; end_audio=end;
    atomic_store(&reset_gain,1);
    ReleaseSRWLockExclusive(&generator_lock);
}
void tf_audio_control(float gain,int on,uint64_t beat,unsigned fade_ms) {
    atomic_store(&heartbeat,beat);
    atomic_store(&requested_gain,gain);
    atomic_store(&ramp_samples,(unsigned)((uint64_t)output_rate*fade_ms/1000));
    int was=atomic_exchange(&enabled,on);
    if (was!=on && fallback_event) SetEvent(fallback_event);
}
void tf_audio_flush(void) {
    atomic_store(&enabled,0);
    if (fallback_event) SetEvent(fallback_event);
    AcquireSRWLockExclusive(&generator_lock);
    if (generator.object && end_audio) end_audio(generator.object);
    ReleaseSRWLockExclusive(&generator_lock);
}
void tf_audio_shutdown(void) {
    atomic_store(&enabled,0);
    if (fallback_stop) SetEvent(fallback_stop);
    if (fallback_thread) { WaitForSingleObject(fallback_thread,INFINITE); CloseHandle(fallback_thread); fallback_thread=NULL; }
    if (fallback_stop) { CloseHandle(fallback_stop); fallback_stop=NULL; }
    if (fallback_event) { CloseHandle(fallback_event); fallback_event=NULL; }
    detach();
    TfShared empty={0}; tf_audio_generator(&empty,NULL,NULL,NULL);
    backend=0; first_try=last_try=0; last_failure=NULL; last_result=0;
    tf_log("Audio resources released");
}
uint64_t tf_audio_frames(void) { return atomic_load(&frames_out); }
#ifdef TF_AUDIO_TEST
/* Test-only interface. Never exported by the installed DLL. */
void tf_audio_test_pull(float *out,unsigned frames) { pull(out,frames); }
void tf_audio_test_read(float *in,float *out,unsigned n,int ic,int *oc) { read_audio(NULL,in,out,n,ic,oc); }
#endif
