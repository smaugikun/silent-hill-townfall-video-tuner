/* Optional test of the real game's FMOD binaries/bus, in a separate process with
 * NOSOUND output. Its PCM source is a mock; this does not test Unreal decoding. */
#include "../native/platform.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "../native/audio.c"
void tf_log(const char *format,...) { va_list args; va_start(args,format); vprintf(format,args); va_end(args); putchar('\n'); }
static int mock_generate(void *ctx,float *out,int n) { (void)ctx; for(int i=0;i<n;++i) out[i]=.2f; return n; }
static void mock_release(TfShared *ptr) { ptr->object=ptr->reference=NULL; }
static void mock_end(void *ptr) { (void)ptr; }
static _Atomic unsigned received_blocks;
static _Atomic int heard_pcm;
static FmResult capture(FmState *s,float *in,float *out,unsigned n,int ic,int *oc) {
    (void)s;
    if (in && ic>0) {
        for (unsigned i=0;i<n;++i) for(int ch=0;ch<*oc;++ch) out[(size_t)i**oc+ch]=ch<ic?in[(size_t)i*ic+ch]:0;
        for (unsigned i=0;i<n*(unsigned)ic;++i) if (fabsf(in[i])>.001f) atomic_store(&heard_pcm,1);
        atomic_fetch_add(&received_blocks,1);
    }
    return 0;
}
#define LOAD(module,name,type) ((type)(void *)GetProcAddress(module,name))
#define OK(call) do { int result=(call); if(result) { fprintf(stderr,"FMOD error %d: %s\n",result,#call); return 1; } } while(0)
int main(int argc,char **argv) {
    if (argc!=3) { fprintf(stderr,"usage: fmod_integration.exe <FMOD DLL folder> <bank folder>\n"); return 2; }
    char path[2048]; snprintf(path,sizeof(path),"%s/fmod.dll",argv[1]); HMODULE corelib=LoadLibraryA(path);
    snprintf(path,sizeof(path),"%s/fmodstudio.dll",argv[1]); HMODULE studio=LoadLibraryA(path);
    if (!corelib || !studio) { fprintf(stderr,"FMOD load failed %lu\n",GetLastError()); return 1; }
    void *sys=NULL,*core=NULL,*master=NULL,*strings=NULL;
    int (*create)(void **,unsigned)=LOAD(studio,"FMOD_Studio_System_Create",int (*)(void **,unsigned));
    int (*getcore)(void *,void **)=LOAD(studio,"FMOD_Studio_System_GetCoreSystem",int (*)(void *,void **));
    int (*setoutput)(void *,int)=LOAD(corelib,"FMOD_System_SetOutput",int (*)(void *,int));
    int (*init)(void *,int,unsigned,unsigned,void *)=LOAD(studio,"FMOD_Studio_System_Initialize",int (*)(void *,int,unsigned,unsigned,void *));
    int (*update)(void *)=LOAD(studio,"FMOD_Studio_System_Update",int (*)(void *));
    int (*shutdown)(void *)=LOAD(studio,"FMOD_Studio_System_Release",int (*)(void *));
    int (*loadbank)(void *,const char *,unsigned,void **)=LOAD(studio,"FMOD_Studio_System_LoadBankFile",int (*)(void *,const char *,unsigned,void **));
    if (!create || !getcore || !setoutput || !init || !update || !shutdown || !loadbank) return 1;
    OK(create(&sys,0x00020300)); OK(getcore(sys,&core)); OK(setoutput(core,2)); /* game binary verified 2.03; NOSOUND */
    OK(init(sys,64,0,0,NULL));
    snprintf(path,sizeof(path),"%s/Master.bank",argv[2]); OK(loadbank(sys,path,0,&master));
    snprintf(path,sizeof(path),"%s/Master.strings.bank",argv[2]); OK(loadbank(sys,path,0,&strings));
    TfShared source={(void *)1,(void *)1}; tf_audio_generator(&source,mock_generate,mock_release,mock_end);
    int route=0;
    for(int i=0;i<200 && route==0;++i) { OK(update(sys)); route=tf_audio_prepare(sys); Sleep(5); }
    if(route!=1) { fprintf(stderr,"Expected FMOD route, got %d\n",route); tf_audio_shutdown(); shutdown(sys); return 1; }
    void *tap=NULL;
    FmDescription tap_desc={.pluginsdkversion=110,.name="TF capture test",.version=1,.numinputbuffers=1,.numoutputbuffers=1,.read=capture};
    OK(fm.create_dsp(core,&tap_desc,&tap)); OK(fm.add_dsp(group,-1,tap));
    for(int i=0;i<100;++i) { tf_audio_control(1,1,GetTickCount64(),0); OK(update(sys)); Sleep(5); }
    uint64_t frames=tf_audio_frames();
    OK(fm.remove_dsp(group,tap)); OK(fm.release_dsp(tap));
    tf_audio_shutdown();
    void *external_bus=NULL;
    OK(fm.get_bus(sys,"bus:/Cinematics/Sequences/CRTV_Videos",&external_bus)); OK(fm.lock_bus(external_bus));
    OK(update(sys));
    int (*flush)(void *)=LOAD(studio,"FMOD_Studio_System_FlushCommands",int (*)(void *));
    if (!flush) return 1;
    OK(flush(sys));
    int already_locked=fm.lock_bus(external_bus);
    if (already_locked!=78) { fprintf(stderr,"Expected actual FMOD already-locked value 78, got %d\n",already_locked); return 1; }
    source=(TfShared){(void *)1,(void *)1}; tf_audio_generator(&source,mock_generate,mock_release,mock_end);
    route=tf_audio_prepare(sys);
    if (route!=1 || !bus_shared || bus_locked) { fprintf(stderr,"Prelocked bus was not shared safely\n"); return 1; }
    uint64_t shared_before=tf_audio_frames();
    for(int i=0;i<20;++i) { tf_audio_control(1,1,GetTickCount64(),0); OK(update(sys)); Sleep(5); }
    if (tf_audio_frames()==shared_before) return 1;
    tf_audio_shutdown();
    if (fm.lock_bus(external_bus)!=78) { fprintf(stderr,"Shutdown released the external bus lock\n"); return 1; }
    OK(fm.unlock_bus(external_bus)); OK(shutdown(sys));
    puts("PASS prelocked CRTV bus sharing, PCM generation and external lock preservation");
    printf("FMOD PCM frames=%llu capture blocks=%u audible PCM=%d\n",(unsigned long long)frames,
        atomic_load(&received_blocks),atomic_load(&heard_pcm));
    FreeLibrary(studio); FreeLibrary(corelib);
    if (!frames || !atomic_load(&received_blocks) || !atomic_load(&heard_pcm)) return 1;
    puts("PASS real FMOD CRTV bus and head capture (mock PCM source, NOSOUND output)"); return 0;
}
