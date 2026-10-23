#include "../native/platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <wchar.h>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static int sound_warnings; /* "No sound from" lines: discovery's MKV audio check */
void tf_log(const char *format,...) { if (!strncmp(format,"No sound from",13)) ++sound_warnings; }
void tf_audio_test_pull(float *,unsigned);
void tf_audio_test_read(float *,float *,unsigned,int,int *);
static const char *ini="[General]\ntolerance=.01\nhysteresis=.005\nfadeMs=200\n[One]\nfile=one.mp4\nfrequency=.15\nloop=true\nvolume=.75\n[Two]\nfile=two.mp4\nfrequency=.75\nloop=false\n";
static TfConfig parse(const char *s,int expected) {
    TfConfig cfg; char error[256];
    CHECK(tf_parse_config(s,&cfg,error,sizeof(error))==expected);
    CHECK(expected?!*error:!!*error); return cfg;
}
static void configuration_tests(void) {
    TfConfig c=parse(ini,1);
    CHECK(c.count==2 && c.stations[0].loop && !c.stations[1].loop);
    CHECK(c.tolerance==.01 && c.hysteresis==.005 && c.fade_ms==200);
    CHECK(c.stations[0].volume==.75);
    CHECK(c.auto_assign);
    CHECK(c.output_width==640 && c.output_height==480);
    CHECK(c.warm_decoder_ms==3000);
    CHECK(parse("[General]\nwarmDecoderMs=0\n",1).warm_decoder_ms==0);
    CHECK(parse("[General]\nwarmDecoderMs=10000\n",1).warm_decoder_ms==10000);
    parse("[General]\nwarmDecoderMs=-1\n",0); parse("[General]\nwarmDecoderMs=10001\n",0);
    parse("[General]\nwarmDecoderMs=0.5\n",0);
    parse("[General]\nwarmDecoderMs=1\nwarmDecoderMs=2\n",0);
    TfConfig small=parse("[General]\noutputWidth=320\noutputHeight=240\n",1);
    CHECK(small.output_width==320 && small.output_height==240);
    parse("[General]\noutputWidth=0\n",0); parse("[General]\noutputHeight=2049\n",0);
    parse("[General]\noutputWidth=640\noutputWidth=320\n",0);
    parse("[General]\noutputHeight=480.5\n",0);
    TfConfig manual=parse("[General]\nautoAssign=false\n",1); CHECK(!manual.auto_assign);
    parse("\xef\xbb\xbf[General]\r\n[Station]\r\nfile=\"my video.mp4\" ; name\r\nfrequency=0\r\nvolume=2\r\nloop=0\r\n",1);
    parse("[Station]\nfile=prenos_\xc5\xa1.mp4\nfrequency=1\n",1);
    parse("[Station]\nfile=prenos.MKV\nfrequency=.4\n",1);
    const char *bad[]={"file=a.mp4\n", "[Station]\nfile=a.mp4\n", "[Station]\nfrequency=.2\n",
      "[Station]\nfile=a.png\nfrequency=.2\n", "[Station]\nfile=../x.mp4\nfrequency=.2\n",
      "[Station]\nfile=C:/x.mp4\nfrequency=.2\n", "[Station]\nfile=x.mp4\nfrequency=nan\n",
      "[Station]\nfile=x.mp4\nfrequency=inf\n", "[Station]\nfile=x.mp4\nfrequency=1.001\n",
      "[Station]\nfile=x.mp4\nfrequency=-.1\n", "[Station]\nfile=x.mp4\nfrequency=.2bad\n",
      "[Station]\nfile=x.mp4\nfrequency=.2\nloop=perhaps\n", "[Station]\nfile=x.mp4\nfrequency=.2\nvolume=-1\n",
      "[Station]\nfile=x.mp4\nfrequency=.2\nvolume=2.01\n", "[General]\ntolerance=0\n",
      "[General]\nhysteresis=-1\n", "[General]\nfadeMs=200.5\n", "[General]\nfadeMs=2001\n",
      "[General]\nunknown=true\n", "[General]\nautoAssign=perhaps\n", "[General]\nautoAssign=true\nautoAssign=false\n",
      "[Station]\nfile=x.mp4\nfrequency=.2\nfrequency=.2\n",
      "[General]\ntolerance=.01\ntolerance=.02\n", "[One]\nfile=x.mp4\nfrequency=.2\n[one]\nfile=y.mp4\nfrequency=.4\n",
      "[One]\nfile=x.mp4\nfrequency=.2\n[Two]\nfile=y.mp4\nfrequency=.2\n",
      "[One]\nfile=x.mp4\nfrequency=.2\n[Two]\nfile=y.mp4\nfrequency=.225\n"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) parse(bad[i],0);
    CHECK(!tf_safe_filename("\\server\\a.mp4") && !tf_safe_filename("x.mp4:stream") && !tf_safe_filename("a\n.mp4"));
    CHECK(tf_safe_filename("A.MP4") && tf_safe_filename("my file.mp4") && tf_safe_filename("video.MKV"));
    CHECK(tf_choose_station(&c,-1,.150,0)==0);
    CHECK(tf_choose_station(&c,0,.164,0)==0);
    CHECK(tf_choose_station(&c,-1,.164,0)==0); /* entry and return use the same playable band */
    CHECK(tf_choose_station(&c,0,.166,0)==-1);
    CHECK(tf_choose_station(&c,0,.75,0)==1);
    CHECK(tf_choose_station(&c,0,.15,1)==-1);
    CHECK(tf_choose_station(&c,0,NAN,0)==-1 && tf_choose_station(&c,0,INFINITY,0)==-1);
    c.stations[0].enabled=0; CHECK(tf_choose_station(&c,0,.15,0)==-1);
    /* Quick tuning: the nearest enabled station that way, unless the game's own jump landed nearer. */
    TfConfig jumps=parse("[A]\nfile=a.mp4\nfrequency=.2\n[B]\nfile=b.mp4\nfrequency=.85\n[C]\nfile=c.mp4\nfrequency=.7\n",1);
    jumps.stations[2].enabled=0;
    CHECK(tf_quick_tune(&jumps,.5,.5,1)==.85);            /* no game signal that way; the disabled .7 is skipped */
    CHECK(tf_quick_tune(&jumps,.5,.6,1)==-1);             /* the game's signal is nearer */
    CHECK(tf_quick_tune(&jumps,.5,.95,1)==.85);           /* the station is nearer */
    CHECK(tf_quick_tune(&jumps,.5,.1,0)==.2 && tf_quick_tune(&jumps,.5,.3,0)==-1);
    CHECK(tf_quick_tune(&jumps,.1,.1,0)==-1);             /* nothing further down */
    CHECK(tf_quick_tune(&jumps,.85,.85,1)==-1);           /* on a station already: the next one, here none */
    TfConfig dial=parse("[General]\ntolerance=.01\n[Video]\nfile=video.mp4\nfrequency=.080\n",1);
    CHECK(tf_choose_station(&dial,-1,(double)(float).070,0)==0);
    CHECK(tf_choose_station(&dial,-1,(double)(float).090,0)==0);
    CHECK(tf_choose_station(&dial,0,(double)(float).065,0)==0);
    CHECK(tf_choose_station(&dial,0,(double)(float).095,0)==0);
    CHECK(tf_choose_station(&dial,-1,(double)(float).095,0)==0);
    CHECK(tf_choose_station(&dial,0,(double)(float).100,0)==-1);
    CHECK(tf_choose_station(&dial,-1,.095001,0)==-1); /* rounding allowance does not accept a real dial step */
    parse("[General]\ntolerance=.000001\nhysteresis=0\n[A]\nfile=a.mp4\nfrequency=.08\n[B]\nfile=b.mp4\nfrequency=.0800021\n",0);
}
static void automatic_station_tests(void) {
    TfConfig cfg=parse("[General]\nfrequencySide=full\n",1);
    CHECK(tf_add_auto_station(&cfg,"first.mp4",-1)==0);
    CHECK(cfg.stations[0].automatic && cfg.stations[0].loop && cfg.stations[0].volume==1);
    CHECK(tf_add_auto_station(&cfg,"second.mkv",.55)==1 && cfg.stations[1].frequency==.55);
    CHECK(tf_add_auto_station(&cfg,"FIRST.MP4",-1)==-1 && cfg.count==2);
    CHECK(tf_add_auto_station(&cfg,"../escape.mkv",-1)==-1);
    CHECK(tf_add_auto_station(&cfg,"third.mp4",.55)==2 && cfg.stations[2].frequency!=.55);
    for (int i=0;i<cfg.count;++i) for (int j=0;j<i;++j)
        CHECK(fabs(cfg.stations[i].frequency-cfg.stations[j].frequency)>2*(cfg.tolerance+cfg.hysteresis));
    cfg.auto_assign=0; CHECK(tf_add_auto_station(&cfg,"fourth.mkv",-1)==-1);
    cfg=parse("[Auto_1]\nfile=pinned.mp4\nfrequency=.05\n",1);
    CHECK(tf_add_auto_station(&cfg,"other.mkv",.05)==1);
    CHECK(strcmp(cfg.stations[1].name,"Auto_1") && cfg.stations[1].frequency>.08);
    cfg=parse("[General]\ntolerance=.1\nhysteresis=.1\n[A]\nfile=a.mp4\nfrequency=0\n[B]\nfile=b.mkv\nfrequency=.5\n[C]\nfile=c.mp4\nfrequency=1\n",1);
    CHECK(tf_add_auto_station(&cfg,"no-room.mp4",-1)==-1 && cfg.count==3);
    cfg=parse("[General]\ntolerance=.000001\nhysteresis=0\n",1);
    for (int i=0;i<TF_MAX_STATIONS;++i) {
        char name[64]; snprintf(name,sizeof(name),"station-%d.mkv",i);
        CHECK(tf_add_auto_station(&cfg,name,-1)==i);
    }
    CHECK(tf_add_auto_station(&cfg,"one-too-many.mp4",-1)==-1 && cfg.count==TF_MAX_STATIONS);
    cfg=parse("[General]\nfrequencySide=right\ntolerance=.01\n",1);
    CHECK(tf_add_auto_station(&cfg,"right1.mp4",.05)==0 && cfg.stations[0].frequency==.984);
    CHECK(tf_add_auto_station(&cfg,"right2.mkv",.085)==1 && cfg.stations[1].frequency==.953);
    CHECK(tf_add_auto_station(&cfg,"right3.mkv",.185)==2 && cfg.stations[2].frequency==.922);
    CHECK(tf_add_auto_station(&cfg,"keep.mkv",.81)==3 && cfg.stations[3].frequency==.81);
    cfg=parse("[General]\nfrequencySide=left\ntolerance=.01\n",1);
    CHECK(tf_add_auto_station(&cfg,"left.mp4",.95)==0 && cfg.stations[0].frequency==.016);
    cfg=parse("[General]\nfrequencySide=both\ntolerance=.000001\nhysteresis=0\n",1);
    CHECK(tf_add_auto_station(&cfg,"left_saved.mp4",.10)==0 && cfg.stations[0].frequency==.10);
    parse("[General]\nfrequencySide=middle\n",0);
    cfg=parse("[General]\nfrequencySide=auto\ntolerance=.01\n[Pin]\nfile=pinned.mp4\nfrequency=.15\n",1);
    CHECK(tf_add_auto_station(&cfg,"first.mp4",.95)==1);
    CHECK(tf_add_auto_station(&cfg,"second.mkv",.915)==2);
    CHECK(tf_add_auto_station(&cfg,"third.mkv",.88)==3);
    TfRange game[]={{.10,.20},{.70,.95}};
    CHECK(tf_reassign_automatic(&cfg,game,2,1)==3);
    CHECK(cfg.stations[0].frequency==.15 && cfg.stations[0].enabled);
    CHECK(cfg.stations[1].frequency==.016 && cfg.stations[2].frequency==.047 && cfg.stations[3].frequency==.078);
    CHECK(tf_choose_station(&cfg,-1,0,0)==-1 && tf_choose_station(&cfg,-1,1,0)==-1);
    CHECK(tf_reassign_automatic(&cfg,game,2,1)==0); /* stable group, no per-tick movement */
    for (int i=1;i<cfg.count;++i) for (int j=0;j<2;++j)
        CHECK(cfg.stations[i].frequency+cfg.tolerance+cfg.hysteresis<game[j].low ||
              cfg.stations[i].frequency-cfg.tolerance-cfg.hysteresis>game[j].high);
    CHECK(tf_reassign_automatic(&cfg,NULL,0,0)==3 && cfg.stations[0].enabled && !cfg.stations[1].enabled);
    CHECK(tf_reassign_automatic(&cfg,game,2,1)==3 && cfg.stations[1].enabled);
    TfRange full={0,1};
    CHECK(tf_reassign_automatic(&cfg,&full,1,1)==3 && !cfg.stations[1].enabled && !cfg.stations[2].enabled && !cfg.stations[3].enabled);
    CHECK(tf_reassign_automatic(&cfg,game,2,1)==3);
    TfRange bad={NAN,1}; CHECK(tf_reassign_automatic(&cfg,&bad,1,1)==-1);
    CHECK(tf_reassign_automatic(&cfg,NULL,TF_MAX_RANGES+1,1)==-1);
}
typedef struct { int opens,closes,playing,preparing,failed,reject,rate_fail,index; float rate; int64_t pos,duration; } Fake;
static int fake_open(void *ctx,int index) { Fake *f=ctx; ++f->opens; f->index=index; f->pos=0; f->preparing=1; return !f->reject; }
static void fake_close(void *ctx) { Fake *f=ctx; ++f->closes; f->playing=f->preparing=0; }
static int fake_rate(void *ctx,float rate) { Fake *f=ctx; f->rate=rate; f->playing=rate!=0; return !f->rate_fail; }
static int fake_preparing(void *ctx) { return ((Fake *)ctx)->preparing; }
static int fake_playing(void *ctx) { return ((Fake *)ctx)->playing; }
static int fake_failed(void *ctx) { return ((Fake *)ctx)->failed; }
static int64_t fake_position(void *ctx) { return ((Fake *)ctx)->pos; }
static int64_t fake_duration(void *ctx) { return ((Fake *)ctx)->duration; }
static void controller_tests(void) {
    TfConfig cfg=parse(ini,1); Fake f={.duration=100000000};
    TfBackend b={&f,fake_open,fake_close,fake_rate,fake_preparing,fake_playing,fake_failed,fake_position,fake_duration};
    TfController c; tf_controller_init(&c);
#define STEP(freq,act,priority,pause,epoch,now) tf_controller_step(&c,&cfg,&b,freq,act,priority,pause,epoch,now)
    STEP(.15,1,0,0,1,0); CHECK(c.phase==TF_OPENING && f.opens==1 && !c.display);
    f.preparing=0; STEP(.15,1,0,0,1,20); CHECK(c.phase==TF_PLAYING && c.display && c.audible);
    for (int i=0;i<1000;++i) STEP(.15+(i%3)*.004,1,0,0,1,21+i);
    CHECK(f.opens==1 && f.closes==0);
    STEP(.15,1,0,1,1,1100); CHECK(c.phase==TF_PAUSED && !c.audible && f.rate==0 && c.display);
    STEP(.15,1,0,0,1,1120); CHECK(c.phase==TF_PLAYING && f.rate==1);
    STEP(.75,1,0,0,1,1140); CHECK(c.phase==TF_FADING && !c.display && f.opens==1);
    STEP(.15,1,0,0,1,1200); CHECK(c.phase==TF_PLAYING && c.display && c.audible && f.opens==1 && !f.closes);
    cfg.fade_ms=100; STEP(.75,1,0,0,1,1201); CHECK(c.phase==TF_FADING);
    cfg.fade_ms=200;
    STEP(.75,1,0,0,1,1340); CHECK(f.opens==2 && f.index==1 && c.phase==TF_OPENING && f.closes==1);
    f.preparing=0; STEP(.75,1,0,0,1,1360); CHECK(c.phase==TF_PLAYING);
    f.pos=f.duration; f.playing=0; STEP(.75,1,0,0,1,1500); CHECK(c.phase==TF_ENDED && !c.display && !c.audible);
    for(int i=0;i<100;++i) STEP(.75,1,0,0,1,1501+i);
    CHECK(f.opens==2);
    STEP(.45,1,0,0,1,1800); CHECK(c.phase==TF_IDLE);
    STEP(.15,1,0,0,1,1820); f.preparing=0; STEP(.15,1,0,0,1,1840);
    f.pos=f.duration-100000; STEP(.15,1,0,0,1,1860); f.pos=1000; STEP(.15,1,0,0,1,1880);
    CHECK(c.phase==TF_PLAYING && f.opens==3);
    STEP(.15,1,1,0,1,1900); CHECK(c.phase==TF_IDLE && !c.display && !c.audible);
    STEP(.15,1,0,0,1,1920); f.preparing=0; STEP(.15,1,0,0,1,1940);
    STEP(.15,0,0,0,1,1960); CHECK(c.phase==TF_IDLE && !c.audible);
    STEP(.15,1,0,0,1,1980); f.preparing=0; STEP(.15,1,0,0,1,2000);
    STEP(.15,1,0,0,2,2020); CHECK(c.phase==TF_OPENING && f.closes==5 && f.opens==6);
    STEP(.15,1,1,0,2,2040);
    f.reject=1; STEP(.15,1,0,0,2,2060); CHECK(c.phase==TF_ERROR && !c.display && f.closes==7);
    int opens=f.opens;
    for (int i=0;i<100;++i) STEP(.15,1,0,0,2,2100+i);
    CHECK(f.opens==opens);
    f.reject=0; STEP(.4,1,0,0,2,2300); STEP(.15,1,0,0,2,2320);
    f.failed=1; STEP(.15,1,0,0,2,2340); CHECK(c.phase==TF_ERROR && !c.display);
    f.failed=0; STEP(.4,1,0,0,2,2360); STEP(.15,1,0,0,2,2380);
    STEP(.15,1,0,0,2,17381); CHECK(c.phase==TF_ERROR); /* corrupt/unsupported async open timeout */
    STEP(.4,1,0,0,2,17400); STEP(.15,1,0,0,2,17420); f.preparing=0;
    STEP(.15,1,0,0,2,17440); STEP(.4,1,0,0,2,17460); CHECK(c.phase==TF_FADING);
    STEP(.4,0,0,0,2,17470); CHECK(c.phase==TF_IDLE && !c.audible); /* close cancels a fade immediately */
    cfg.fade_ms=0; STEP(.15,1,0,0,2,18000); f.preparing=0; STEP(.15,1,0,0,2,18020);
    STEP(.75,1,0,0,2,18040); CHECK(c.phase==TF_OPENING && c.station==1);
    cfg=parse("[General]\ntolerance=.01\n[Video]\nfile=video.mp4\nfrequency=.080\n",1);
    tf_controller_init(&c); memset(&f,0,sizeof(f)); f.duration=100000000;
    STEP((double)(float).080,1,0,0,3,20000); f.preparing=0;
    STEP((double)(float).080,1,0,0,3,20020); f.pos=50000000;
    STEP((double)(float).100,1,0,0,3,20040); CHECK(c.phase==TF_FADING);
    STEP((double)(float).095,1,0,0,3,20060);
    CHECK(c.phase==TF_PLAYING && c.display && f.opens==1 && !f.closes && f.pos==50000000);
    STEP((double)(float).100,1,0,0,3,20100);
    STEP((double)(float).100,1,0,0,3,20300); CHECK(c.phase==TF_IDLE && f.closes==1);
    STEP((double)(float).095,1,0,0,3,20320); CHECK(c.phase==TF_OPENING && f.opens==2);
#undef STEP
}
static void geometry_tests(void) {
    TfRect r=tf_fit(16.0/9.0,640,512); CHECK(fabs(r.width-640)<.001 && fabs(r.height-360)<.001 && fabs(r.y-76)<.001);
    r=tf_fit(.5,640,512); CHECK(r.width==256 && r.x==192 && r.height==512);
    r=tf_fit(NAN,640,512); CHECK(r.width==640 && r.height==512);
    float in[4]={.1,.2,.3,.4},stereo[4]={.5,.6,.7,.8},out[4]={0},gain=1;
    tf_mix(out,in,2,2,stereo,2,&gain,1,0);
    CHECK(fabsf(out[0]-.6f)<.00001 && fabsf(out[3]-1.2f)<.00001); /* input remains in the mix */
    gain=0; tf_mix(out,NULL,0,2,stereo,2,&gain,1,100); CHECK(gain==.02f && out[0]>0 && out[0]<.01);
}
static void write_data(const wchar_t *path,const void *content,size_t size) {
    HANDLE h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(h!=INVALID_HANDLE_VALUE); DWORD n; CHECK(WriteFile(h,content,(DWORD)size,&n,NULL)); CloseHandle(h);
}
static void write_file(const wchar_t *path,const char *content) { write_data(path,content,strlen(content)); }
/* Just enough Matroska for tf_mkv_audio: an EBML element with a one- or two-byte size. */
static size_t ebml(unsigned char *out,uint32_t id,const void *data,size_t n) {
    size_t at=0;
    for (int shift=24;shift>=0;shift-=8) if (id>>shift || at) out[at++]=(unsigned char)(id>>shift);
    if (n<127) out[at++]=(unsigned char)(0x80|n); else { out[at++]=(unsigned char)(0x40|n>>8); out[at++]=(unsigned char)n; }
    memcpy(out+at,data,n); return at+n;
}
/* A file's head: the EBML header, then a Segment (of unknown size if `unknown`) with Info and one track per codec
 * ("V_..." video, else audio). */
static size_t mkv_head(unsigned char *out,const char *const *codecs,int count,int unknown) {
    unsigned char tracks[2048],entry[256],fields[256],body[4096]; size_t t=0,b=0,at;
    for (int i=0;i<count;++i) {
        unsigned char type=codecs[i][0]=='V'?1:2;
        size_t f=ebml(fields,0xD7,(unsigned char[]){(unsigned char)(i+1)},1);
        f+=ebml(fields+f,0x83,&type,1); f+=ebml(fields+f,0x86,codecs[i],strlen(codecs[i]));
        size_t e=ebml(entry,0xAE,fields,f); memcpy(tracks+t,entry,e); t+=e;
    }
    b=ebml(body,0x1549A966,"\x2A\xD7\xB1\x83\x0F\x42\x40",7); /* Info: TimestampScale */
    b+=ebml(body+b,0x1654AE6B,tracks,t);
    at=ebml(out,0x1A45DFA3,"\x42\x82\x88matroska",11);
    if (!unknown) return at+ebml(out+at,0x18538067,body,b);
    memcpy(out+at,"\x18\x53\x80\x67\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF",12); memcpy(out+at+12,body,b);
    return at+12+b;
}
static void mkv_audio_tests(void) {
    unsigned char head[8192]; char codec[64]; size_t n;
    const char *eac3[]={"V_MPEGH/ISO/HEVC","A_EAC3"},*aac[]={"V_MPEG4/ISO/AVC","A_AAC"},*both[]={"V_MPEGH/ISO/HEVC","A_EAC3","A_AAC"};
    const char *video[]={"V_VP9"},*dts[]={"A_DTS","A_TRUEHD"},*opus[]={"V_VP9","A_OPUS"};
    n=mkv_head(head,eac3,2,0); CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==0 && !strcmp(codec,"A_EAC3"));
    n=mkv_head(head,eac3,2,1); CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==0); /* a live-written segment */
    n=mkv_head(head,aac,2,0); CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==1);
    n=mkv_head(head,opus,2,0); CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==1);
    n=mkv_head(head,both,3,0); CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==1); /* the game takes the AAC track */
    n=mkv_head(head,dts,2,0); CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==0 && !strcmp(codec,"A_DTS"));
    n=mkv_head(head,video,1,0); CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==-1); /* no sound to miss */
    /* Cut off inside the audio track's codec: that track is unknown, never reported as unsupported. */
    n=mkv_head(head,eac3,2,0); CHECK(tf_mkv_audio(head,n-3,codec,sizeof(codec))==-1);
    unsigned char fields[32],entry[64],tracks[64],two=2; /* a NUL-padded CodecID, as EBML allows */
    size_t f=ebml(fields,0x83,&two,1); f+=ebml(fields+f,0x86,"A_AAC\0",6);
    size_t e=ebml(entry,0xAE,fields,f),t=ebml(tracks,0x1654AE6B,entry,e); n=ebml(head,0x18538067,tracks,t);
    CHECK(tf_mkv_audio(head,n,codec,sizeof(codec))==1);
    CHECK(tf_mkv_audio(head,0,codec,sizeof(codec))==-1 && tf_mkv_audio((const unsigned char *)"not a video",11,codec,sizeof(codec))==-1);
    /* A cluster before any tracks: nothing to tell from the head. */
    unsigned char clustered[64]; size_t c=ebml(clustered,0x1A45DFA3,"\x42\x82\x88matroska",11);
    memcpy(clustered+c,"\x18\x53\x80\x67\x88\x1F\x43\xB6\x75\x83\xE7\x81\x00",13);
    CHECK(tf_mkv_audio(clustered,c+13,codec,sizeof(codec))==-1);
}
static void discovery_tests(void) {
    /* Only creates and deletes explicitly named files below the test executable's build directory. */
    wchar_t exe[2048],folder[2048],a[2048],other[2048],path[2048],mkv[2048],added[2048],map[2048];
    CHECK(GetModuleFileNameW(NULL,exe,2048)>0); wchar_t *end=wcsrchr(exe,L'\\'); CHECK(end!=NULL); *end=0;
    swprintf(folder,2048,L"%ls\\test-videos",exe); CreateDirectoryW(folder,NULL);
    swprintf(a,2048,L"%ls\\one.mp4",folder); swprintf(other,2048,L"%ls\\ignore.txt",folder);
    swprintf(mkv,2048,L"%ls\\zz;video.MKV",folder); swprintf(added,2048,L"%ls\\a-added.mp4",folder);
    swprintf(map,2048,L"%ls\\stations-auto.ini",folder);
    write_file(a,"invalid mp4 bytes"); write_file(other,"not a video");
    TfConfig cfg=parse(ini,1);
    CHECK(tf_discover(folder,&cfg)==1 && cfg.stations[0].enabled && !cfg.stations[1].enabled);
    CHECK(tf_video_exists(folder,"one.mp4",path,2048)); CHECK(!tf_video_exists(folder,"../one.mp4",path,2048));
    CHECK(!tf_video_exists(folder,"missing.mp4",path,2048));
    write_file(mkv,"mock MKV, discovery does not decode it");
    cfg=parse(ini,1); CHECK(tf_discover(folder,&cfg)==2 && cfg.count==3);
    CHECK(GetFileAttributesW(map)==INVALID_FILE_ATTRIBUTES); /* discovery does not force a map write */
    tf_save_auto_map(folder,&cfg);
    CHECK(!strcmp(cfg.stations[2].file,"zz;video.MKV") && cfg.stations[2].automatic);
    double saved_frequency=cfg.stations[2].frequency;
    TfConfig saved; CHECK(tf_read_config_file(map,&saved)==1 && saved.count==1);
    CHECK(!strcmp(saved.stations[0].file,"zz;video.MKV") && saved.stations[0].frequency==saved_frequency);
    write_file(added,"mock MP4 sorts before existing video");
    cfg=parse(ini,1); CHECK(tf_discover(folder,&cfg)==3 && cfg.count==4);
    CHECK(!strcmp(cfg.stations[2].file,"zz;video.MKV") && cfg.stations[2].frequency==saved_frequency);
    CHECK(!strcmp(cfg.stations[3].file,"a-added.mp4"));
    cfg=parse("[General]\nautoAssign=false\n",1); CHECK(tf_discover(folder,&cfg)==3 && cfg.count==0);
    cfg=parse("",1); CHECK(tf_discover(folder,&cfg)==3 && cfg.count==3);
    tf_save_auto_map(folder,&cfg);
    CHECK(tf_read_config_file(map,&saved)==1 && saved.count==3);
    CHECK(DeleteFileW(mkv)); cfg=parse("",1); CHECK(tf_discover(folder,&cfg)==2 && cfg.count==2);
    tf_save_auto_map(folder,&cfg);
    CHECK(tf_read_config_file(map,&saved)==1 && saved.count==2);
    write_file(map,"not valid configuration"); cfg=parse("",1); CHECK(tf_discover(folder,&cfg)==2 && cfg.count==2);
    tf_save_auto_map(folder,&cfg);
    CHECK(tf_read_config_file(map,&saved)==1); /* corrupt cache is rebuilt */
    /* An MKV with only E-AC-3 audio is reported once per discovery; the mock MKV and MP4s aren't. */
    unsigned char head[1024]; const char *eac3[]={"V_MPEGH/ISO/HEVC","A_EAC3"}; wchar_t silent[2048];
    swprintf(silent,2048,L"%ls\\silent.mkv",folder); write_data(silent,head,mkv_head(head,eac3,2,0));
    sound_warnings=0; cfg=parse("",1); CHECK(tf_discover(folder,&cfg)==3 && sound_warnings==1);
    CHECK(DeleteFileW(silent));
    CHECK(DeleteFileW(a) && DeleteFileW(other) && DeleteFileW(added) && DeleteFileW(map)); CHECK(RemoveDirectoryW(folder));
}
typedef struct { _Atomic int generating,ended,released,calls; } FakeGenerator;
static _Atomic int lifetime_errors,run_audio;
static int generate(void *object,float *out,int n) {
    FakeGenerator *g=object;
    if (atomic_load(&g->released)) atomic_fetch_add(&lifetime_errors,1);
    atomic_store(&g->generating,1);
    for (int i=0;i<n;++i) out[i]=.25f;
    atomic_fetch_add(&g->calls,1); atomic_store(&g->generating,0); return n;
}
static void release(TfShared *owned) {
    FakeGenerator *g=owned->object;
    if (atomic_load(&g->generating)) atomic_fetch_add(&lifetime_errors,1);
    atomic_store(&g->released,1); owned->object=owned->reference=NULL;
}
static void end_generate(void *object) { FakeGenerator *g=object; if (atomic_load(&g->generating)) atomic_fetch_add(&lifetime_errors,1); atomic_fetch_add(&g->ended,1); }
static DWORD WINAPI audio_racer(void *ctx) {
    (void)ctx; float out[128];
    while (atomic_load(&run_audio)) tf_audio_test_pull(out,64);
    return 0;
}
static void audio_tests(void) {
    FakeGenerator g={0}; TfShared owned={&g,&g};
    tf_audio_generator(&owned,generate,release,end_generate); CHECK(!owned.object);
    float stereo[128]; tf_audio_control(1,1,GetTickCount64(),0); tf_audio_test_pull(stereo,64); CHECK(stereo[0]==.25f);
    tf_audio_control(1,1,GetTickCount64()-1000,0); tf_audio_test_pull(stereo,64); CHECK(stereo[0]==0);
    tf_audio_control(1,0,GetTickCount64(),0); tf_audio_test_pull(stereo,64); CHECK(stereo[0]==0);
    tf_audio_control(1,1,GetTickCount64(),0);
    float input[128],out[128]; for(int i=0;i<128;++i) input[i]=.5f;
    int channels=2; tf_audio_test_read(input,out,64,2,&channels); CHECK(out[0]==.75f && channels==2);
    tf_audio_flush(); CHECK(atomic_load(&g.ended)==1);
    TfShared empty={0}; tf_audio_generator(&empty,NULL,NULL,NULL); CHECK(atomic_load(&g.released)==1);
    FakeGenerator objects[1000]={0}; atomic_store(&run_audio,1);
    HANDLE thread=CreateThread(NULL,0,audio_racer,NULL,0,NULL); CHECK(thread!=NULL);
    for(int i=0;i<1000;++i) {
        owned=(TfShared){&objects[i],&objects[i]}; tf_audio_generator(&owned,generate,release,end_generate);
        tf_audio_control(1,1,GetTickCount64(),0);
        if (i%10==0) Sleep(1);
    }
    atomic_store(&run_audio,0); CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CloseHandle(thread);
    tf_audio_shutdown(); CHECK(!atomic_load(&lifetime_errors));
    for(int i=0;i<1000;++i) CHECK(atomic_load(&objects[i].released)==1);
}
int main(void) {
    configuration_tests(); automatic_station_tests(); controller_tests(); geometry_tests(); mkv_audio_tests(); discovery_tests(); audio_tests();
    printf("PASS %d native checks: config, discovery, tuning, lifecycle, fitting, audio, thread/resource ownership\n",checks);
    return 0;
}
