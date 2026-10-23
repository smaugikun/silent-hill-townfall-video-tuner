/* Run the actual DLL dispatcher/render adapter with native engine mocks.
 * This does not run Electra or a GPU. Audio lifetime semantics match the inspected
 * OnEndGenerate implementation: ending generation removes its sample queue. */
#define TF_VIDEO_TEST
#include "../native/platform.h"
static int mock_audio_prepare(void *system) { (void)system; return 1; }
#define tf_audio_prepare mock_audio_prepare
#include "../native/tf_video.c"
#undef tf_audio_prepare
#include <stdlib.h>

static int checks,opens,closes,created,ended,released,draw_calls,rate_value;
static int64_t mock_time;
static int mock_preparing,mock_playing;
static int seeks,seek_delayed,seek_rejected,mock_seeking;
static int64_t last_seek;
static void *facade_marker=(void *)2;
static unsigned char player_bytes[1024],texture_bytes[1024],sound_bytes[3000],target_bytes[1024],radio_bytes[3000];
static unsigned char original_bytes[1024],resource_bytes[1024],rhi_bytes[1024];
static unsigned expected_format;
static double expected_y=71.25,expected_height=337.5;
static void *canvas_marker=(void *)1;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void *weak_objects[80];
static int weak_count,dial_updates,dial_removes;
static unsigned char dial_bytes[1000],panel_bytes[100],marker_bytes[2][3000];
static SignalDisplay displayed[2];
static unsigned char needle_entries[3][28],image_bytes[2][100],slot_bytes[2][200];
static int styled,moved;
static HANDLE request_client=INVALID_HANDLE_VALUE,response_client=INVALID_HANDLE_VALUE;
static void send_packet(unsigned kind,const void *bytes,unsigned size) {
    TfBridgeHeader header={.kind=kind,.size=size}; memcpy(header.magic,"TFVPIPE1",8);
    DWORD sent=0;
    CHECK(WriteFile(request_client,&header,sizeof(header),&sent,NULL) && sent==sizeof(header));
    CHECK(WriteFile(request_client,bytes,size,&sent,NULL) && sent==size);
}
static void *mock_weak_get(const WeakObject *weak) {
    return weak->serial==1 && weak->index>0 && weak->index<=weak_count?weak_objects[weak->index]:NULL;
}
static void mock_weak_assign(WeakObject *weak,void **source) {
    for (int i=1;i<=weak_count;++i) if (weak_objects[i]==*source) { *weak=(WeakObject){i,1}; return; }
    weak_objects[++weak_count]=*source; *weak=(WeakObject){weak_count,1};
}
static void mock_dial_update(void *widget,const SignalDisplay *display) {
    CHECK(widget==dial_bytes && display->state==PV(SIGNAL_DISCOVERED) && display->suppress && !display->enemy);
    void *source=mock_weak_get(&display->source);
    CHECK(source==marker_bytes[0] || source==marker_bytes[1]);
    int index=source==marker_bytes[1];
    displayed[index]=*display; ++dial_updates;
    WeakObject key; void *pointer=image_bytes[index]; mock_weak_assign(&key,&pointer); memcpy(needle_entries[index]+PV(NEEDLE_IMAGE),&key,8);
    pointer=slot_bytes[index]; mock_weak_assign(&key,&pointer); memcpy(needle_entries[index]+PV(NEEDLE_SLOT),&key,8);
    memcpy(needle_entries[index]+PV(NEEDLE_SOURCE),&display->source,8);
    float geometry[]={display->frequency*640-320,0,15,80};
    memcpy(slot_bytes[index]+PV(SLOT_X),geometry,sizeof(geometry));
}
/* The engine loads FVector2D/FLinearColor arguments with movaps: a misaligned one crashes the game. */
#define ALIGNED(p) (((uintptr_t)(p)&15)==0)
static void mock_image_color(void *image_object,const Color *color) {
    CHECK(ALIGNED(color));
    CHECK((image_object==image_bytes[0] || image_object==image_bytes[1]) && color->r==1 && color->g==0 && color->b==1 && color->a==1); ++styled;
}
static void mock_slot_position(void *slot,const Vec2 *value) {
    CHECK(ALIGNED(value));
    float x; memcpy(&x,(char *)slot+PV(SLOT_X),4);
    CHECK((slot==slot_bytes[0] || slot==slot_bytes[1]) && value->x==x && value->y==0); ++moved;
}
static void mock_dial_remove(void *widget,void *source) {
    CHECK(widget==dial_bytes && (source==marker_bytes[0] || source==marker_bytes[1])); ++dial_removes;
}
static float mock_get_rate(void *p) { CHECK(p==player_bytes); return (float)rate_value; }
typedef struct { int connected; } MockGenerator;
static MockGenerator *live_generator;
void tf_audio_test_pull(float *,unsigned);
static unsigned char mock_open(void *p,const FString *path) {
    CHECK(p==player_bytes && path->count>1 && path->count==path->capacity);
    ++opens; mock_time=0; mock_preparing=1; mock_seeking=0; return 1;
}
static void mock_close(void *p) { CHECK(p==player_bytes); ++closes; mock_playing=mock_preparing=mock_seeking=0; }
static unsigned char mock_rate(void *p,float rate) { CHECK(p==player_bytes); rate_value=(int)rate; mock_playing=rate!=0; return 1; }
static unsigned char mock_loop(void *p,unsigned char loop) { CHECK(p==player_bytes && loop<=1); return 1; }
static void *mock_position(void *p,int64_t *out) { CHECK(p==player_bytes); *out=mock_time; return out; }
static void *mock_duration(void *p,int64_t *out) { CHECK(p==player_bytes); *out=100000000; return out; }
static unsigned char mock_is_playing(void *p) { CHECK(p==player_bytes); return (unsigned char)mock_playing; }
static unsigned char mock_is_preparing(void *p) { CHECK(p==player_bytes); return (unsigned char)mock_preparing; }
static unsigned char mock_seek(void *facade,const int64_t *time) {
    CHECK(facade==facade_marker && *time>=0); ++seeks; last_seek=*time;
    if (seek_rejected) return 0;
    mock_seeking=seek_delayed;
    if (!seek_delayed) mock_time=*time;
    return 1;
}
static unsigned char mock_is_seeking(void *facade) { CHECK(facade==facade_marker); return (unsigned char)mock_seeking; }
static void mock_set_player(void *obj,void *p) { CHECK((obj==texture_bytes || obj==sound_bytes) && p==player_bytes); }
static void mock_texture_update(void *obj) { CHECK(obj==texture_bytes); }
static void mock_sound_update(void *obj) { CHECK(obj==sound_bytes); }
static void *mock_create_generator(void *obj,TfShared *out,const GeneratorParams *params) {
    CHECK(obj==sound_bytes && params->rate==48000 && params->channels==2);
    MockGenerator *g=calloc(1,sizeof(*g)); CHECK(g!=NULL); g->connected=1;
    live_generator=g; *out=(TfShared){g,g}; ++created; return out;
}
static int mock_generate_audio(void *obj,float *out,int n) {
    MockGenerator *g=obj; for(int i=0;i<n;++i) out[i]=g->connected?.25f:0; return n;
}
static void mock_end_audio(void *obj) { ((MockGenerator *)obj)->connected=0; ++ended; }
static void mock_release_shared(TfShared *out) { free(out->object); out->object=out->reference=NULL; ++released; }
static void mock_target_init(void *obj,unsigned w,unsigned h,int pixel,unsigned char linear) {
    CHECK(obj==target_bytes && w==640 && h==480 && pixel==(int)expected_format && linear==0);
}
static void *mock_resource(void *obj) { CHECK(obj==original_bytes); return resource_bytes; }
static void mock_begin(void *w,void *rt,void **canvas,Vec2 *size,void **context) {
    CHECK(w==canvas_marker && rt==target_bytes && ALIGNED(size)); *canvas=canvas_marker; *size=(Vec2){640,480}; *context=rt;
}
static void mock_canvas_end(void *w,void **context) { CHECK(w==canvas_marker && *context==target_bytes); }
static void mock_clear(void *w,void *rt,const Color *color) { CHECK(w==canvas_marker && rt==target_bytes && ALIGNED(color) && color->r==0); }
static void mock_draw(void *canvas,void *tex,const Vec2 *origin,const Vec2 *extent,const Vec2 *uv0,
                      const Vec2 *uv1,const Color *color,int blend,float rotation,const Vec2 *pivot) {
    CHECK(ALIGNED(origin) && ALIGNED(extent) && ALIGNED(uv0) && ALIGNED(uv1) && ALIGNED(color) && ALIGNED(pivot));
    CHECK(canvas==canvas_marker && tex==texture_bytes && uv0->x==0 && uv1->x==1 && color->r==1 &&
        (uint32_t)blend==PV(BLEND_OPAQUE) && rotation==0 && pivot->x==.5);
    CHECK(fabs(origin->y-expected_y)<.01 && extent->x==640 && fabs(extent->y-expected_height)<.01); ++draw_calls;
}
static void functions(void) {
#define SET(index,function) tf_test_functions[index]=(void *)(function)
    SET(MEDIA_OPEN,mock_open); SET(MEDIA_CLOSE,mock_close); SET(MEDIA_RATE,mock_rate); SET(MEDIA_LOOP,mock_loop);
    SET(MEDIA_TIME,mock_position); SET(MEDIA_DURATION,mock_duration); SET(MEDIA_PLAYING,mock_is_playing); SET(MEDIA_PREPARING,mock_is_preparing);
    SET(MEDIA_SEEK,mock_seek);
    SET(MEDIA_SEEKING,mock_is_seeking);
    SET(TEXTURE_UPDATE,mock_texture_update); SET(TEXTURE_SET,mock_set_player); SET(SOUND_SET,mock_set_player); SET(SOUND_UPDATE,mock_sound_update);
    SET(SOUND_CREATE,mock_create_generator); SET(GEN_AUDIO,mock_generate_audio); SET(GEN_END,mock_end_audio); SET(SHARED_RELEASE,mock_release_shared);
    SET(TARGET_INIT,mock_target_init); SET(CANVAS_BEGIN,mock_begin); SET(CANVAS_END,mock_canvas_end); SET(CANVAS_CLEAR,mock_clear); SET(CANVAS_DRAW,mock_draw);
    SET(SOURCE_RESOURCE,mock_resource);
    SET(WEAK_GET,mock_weak_get); SET(WEAK_ASSIGN,mock_weak_assign);
    SET(DIAL_UPDATE,mock_dial_update); SET(DIAL_REMOVE,mock_dial_remove); SET(MEDIA_GET_RATE,mock_get_rate);
    SET(IMAGE_COLOR,mock_image_color); SET(SLOT_POSITION,mock_slot_position);
#undef SET
}
static Output step(unsigned flags,double frequency,unsigned epoch) {
    static uint64_t sequence;
    Input in={.sequence=++sequence,.player=(uintptr_t)player_bytes,.texture=(uintptr_t)texture_bytes,.sound=(uintptr_t)sound_bytes,
        .target=(uintptr_t)target_bytes,.world=(uintptr_t)canvas_marker,.radio=(uintptr_t)radio_bytes,.frequency=frequency,
        .flags=flags|OBJECTS,.epoch=epoch,.width=640,.height=512,.original=(uintptr_t)original_bytes};
    memcpy(in.magic,"TFVIDEO1",8);
    send_packet(TF_BRIDGE_INPUT,&in,sizeof(in));
    tf_video_tick(NULL);
    Output out; DWORD n;
    CHECK(ReadFile(response_client,&out,sizeof(out),&n,NULL) && n==sizeof(out));
    CHECK(out.sequence==sequence && !memcmp(out.magic,"TFVSTAT1",8)); return out;
}
static void resume_tests(void) {
    char error[256];
    CHECK(tf_parse_config("[General]\ntolerance=.01\nfadeMs=0\nwarmDecoderMs=0\n[A]\nfile=test.mp4\nfrequency=.15\n[B]\nfile=second.mkv\nfrequency=.75\n",&config,error,sizeof(error)));
    memset(bookmarks,0,sizeof(bookmarks)); tf_controller_init(&controller);
    int previous_seeks=seeks;
    step(ACTIVE,.15,3); mock_preparing=0; mock_time=50000000;
    Output out=step(ACTIVE,.15,3); CHECK(out.bind && out.error==0); /* a successful reopen clears the previous station's error */
    step(0,.15,3); CHECK(bookmarks[0]==50000000 && bookmarks[1]==0);
    step(ACTIVE,.75,3); mock_preparing=0; mock_time=20000000;
    step(ACTIVE,.75,3); CHECK(seeks==previous_seeks); /* first view of a different file starts normally */
    step(ACTIVE,.15,3); CHECK(bookmarks[1]==20000000);
    mock_preparing=0; seek_delayed=1;
    out=step(ACTIVE,.15,3); CHECK(out.phase==TF_OPENING && !out.bind && seeks==previous_seeks+1 && last_seek==50000000);
    out=step(ACTIVE,.15,3); CHECK(out.phase==TF_OPENING && !out.bind && seeks==previous_seeks+1);
    mock_time=last_seek;
    out=step(ACTIVE,.15,3); CHECK(out.phase==TF_OPENING && !out.bind); /* time alone does not complete a seek */
    mock_time=last_seek+2500000; seek_delayed=mock_seeking=0;
    out=step(ACTIVE,.15,3); CHECK(out.phase==TF_PLAYING && out.bind && mock_time==52500000); /* completed seek has passed the old 100 ms window */
    int before=opens;
    step(ACTIVE|PAUSED,.15,3); step(ACTIVE,.15,3);
    CHECK(opens==before && seeks==previous_seeks+1);
    mock_time=60000000; step(ACTIVE|PRIORITY,.15,3); CHECK(bookmarks[0]==60000000);
    step(ACTIVE,.75,3); mock_preparing=0; seek_rejected=1;
    out=step(ACTIVE,.75,3); CHECK(out.phase==TF_OPENING && !out.bind);
    step(0,.75,3); CHECK(bookmarks[1]==20000000); /* interrupted resume must not replace the bookmark with zero */
    seek_rejected=0; step(ACTIVE,.75,3); mock_preparing=0;
    out=step(ACTIVE,.75,3); CHECK(out.bind && mock_time==20000000 && bookmarks[0]==60000000);
    step(0,.75,3);
    step(ACTIVE,.15,4); mock_preparing=0;
    out=step(ACTIVE,.15,4); CHECK(out.bind && mock_time==60000000); /* survives a level epoch */
    tf_video_shutdown(NULL);
}
static void marker_tests(void) {
    WeakObject widget={0},panel={0}; void *pointer=dial_bytes;
    mock_weak_assign(&widget,&pointer); pointer=panel_bytes; mock_weak_assign(&panel,&pointer);
    memcpy(radio_bytes+PV(RADIO_DIAL),&widget,8); memcpy(dial_bytes+PV(DIAL_PANEL),&panel,8);
    int capacity=12; memcpy(dial_bytes+PV(DIAL_CAPACITY),&capacity,4);
    MarkerPacket packet={.count=2,.sources={(uintptr_t)marker_bytes[0],(uintptr_t)marker_bytes[1]}};
    memcpy(packet.magic,"TFVMARK1",8);
    send_packet(TF_BRIDGE_MARKERS,&packet,sizeof(packet)); CHECK(!packet_read() && marker_packet_ready);
    incoming.radio=(uintptr_t)radio_bytes;
    struct { void *data; int32_t count,capacity; } array={needle_entries,3,3};
    memcpy(dial_bytes+PV(DIAL_NEEDLES),&array,sizeof(array));
    unsigned char unchanged_radio[sizeof(radio_bytes)]; memcpy(unchanged_radio,radio_bytes,sizeof(radio_bytes));
    markers_update(1); CHECK(markers_loaded && dial_updates==2 && marker_count==2 && styled==2 && moved==2);
    for (int i=0;i<2;++i) {
        float geometry[4]; memcpy(geometry,slot_bytes[i]+PV(SLOT_X),sizeof(geometry));
        CHECK(geometry[1]==0 && geometry[2]==15 && geometry[3]==80); /* normal dial geometry stays intact */
    }
    memcpy(&capacity,dial_bytes+PV(DIAL_CAPACITY),4); CHECK(capacity==14);
    CHECK(fabs(displayed[0].frequency-.15)<.00001 && fabs(displayed[1].frequency-.75)<.00001);
    markers_update(1); CHECK(dial_updates==2); /* unchanged dial is throttled */
    incoming.flags|=PRIORITY; markers_update(1); CHECK(dial_removes==0); /* stations stay visible during game priority */
    markers_update(0); CHECK(dial_removes==2);
    memcpy(&capacity,dial_bytes+PV(DIAL_CAPACITY),4); CHECK(capacity==12);
    config.stations[1].enabled=0; markers_update(1); CHECK(marker_count==1 && dial_updates==3);
    memcpy(&capacity,dial_bytes+PV(DIAL_CAPACITY),4); CHECK(capacity==13);
    markers_update(0); CHECK(dial_removes==4);
    config.stations[1].enabled=1; strcpy(config.stations[1].file,"missing.mkv");
    markers_update(1); CHECK(marker_count==2 && dial_updates==5); /* marker refresh does no file probing */
    markers_clear(); strcpy(config.stations[1].file,"test.mp4");
    markers_update(1); CHECK(marker_count==2 && dial_updates==7);
    capacity=24; memcpy(dial_bytes+PV(DIAL_CAPACITY),&capacity,4); markers_clear();
    memcpy(&capacity,dial_bytes+PV(DIAL_CAPACITY),4); CHECK(capacity==24); /* another UI writer retains ownership */
    markers_update(1); weak_objects[widget.index]=NULL; markers_clear(); /* destroyed widget must never be called */
    CHECK(dial_removes==8 && !marker_widget.serial);
    CHECK(!memcmp(unchanged_radio,radio_bytes,sizeof(radio_bytes))); /* no radio/signal mutation */
    memset(radio_bytes+PV(RADIO_DIAL),0,8); markers_loaded=0;
}
/* The marks file publish_marks wrote (into the fixture: main points the temp folder there), as text. */
static int read_marks(char *out,size_t size) {
    wchar_t path[2048]; swprintf(path,2048,L"%ls\\townfall-companion-dial-marks.txt",folder);
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if (f==INVALID_HANDLE_VALUE) return 0;
    DWORD n=0; int ok=ReadFile(f,out,(DWORD)size-1,&n,NULL); CloseHandle(f); out[n]=0;
    return ok;
}
static void reservation_packet_tests(void) {
    char error[256],marks[256];
    CHECK(tf_parse_config("[General]\ntolerance=.01\nfadeMs=0\nwarmDecoderMs=0\n[A]\nfile=test.mp4\nfrequency=.95\n[B]\nfile=second.mkv\nfrequency=.915\n",&config,error,sizeof(error)));
    config.stations[0].automatic=config.stations[1].automatic=1;
    struct { char magic[8]; uint32_t epoch,count,valid,revision; TfRange range; } packet={.epoch=7,.count=1,.valid=1,.revision=1,.range={.10,.98}};
    memcpy(packet.magic,"TFVRANG1",8);
    send_packet(TF_BRIDGE_RANGES,&packet,sizeof(packet)); CHECK(!packet_read() && range_packet_ready);
    incoming.epoch=7; range_seen=0; ranges_update();
    CHECK(config.stations[0].enabled && config.stations[0].frequency==.016 && config.stations[1].frequency==.047);
    CHECK(config.range_count==1 && range_seen);
    /* Townfall Companion's phone dial gets the stations' marks, in the dial's colour. */
    CHECK(read_marks(marks,sizeof(marks)) &&
          !strcmp(marks,"TFMARKS1 2\n0.016000 1.000 0.000 1.000\n0.047000 1.000 0.000 1.000\n"));
    double frequency=config.stations[0].frequency; ranges_update(); CHECK(config.stations[0].frequency==frequency);
    /* Stale world data cannot authorize the automatic stations in a new world. */
    incoming.epoch=8; ranges_update();
    CHECK(!config.stations[0].enabled && !config.stations[1].enabled);
    CHECK(read_marks(marks,sizeof(marks)) && !strcmp(marks,"TFMARKS1 0\n")); /* and none while they wait */
    wchar_t map[2048]; swprintf(map,2048,L"%ls\\stations-auto.ini",videos); CHECK(DeleteFileW(map));
    swprintf(map,2048,L"%ls\\townfall-companion-dial-marks.txt",folder); CHECK(DeleteFileW(map));
    range_packet_ready=range_seen=0;
}
static void warm_decoder_tests(void) {
    char error[256];
    CHECK(tf_parse_config("[General]\ntolerance=.01\nfadeMs=0\n[A]\nfile=test.mp4\nfrequency=.15\n[B]\nfile=second.mkv\nfrequency=.75\n",&config,error,sizeof(error)));
    tf_controller_init(&controller); memset(bookmarks,0,sizeof(bookmarks));
    step(ACTIVE,.15,10); mock_preparing=0; mock_time=40000000; step(ACTIVE|BOUND,.15,10);
    int before=closes,opened=opens,sought=seeks;
    Output out=step(0,.15,10);
    CHECK(!out.bind && cached_station==0 && rate_value==0 && closes==before && bookmarks[0]==40000000);
    out=step(ACTIVE,.15,10);
    CHECK(out.bind && cached_station==-1 && opens==opened && seeks==sought && mock_time==40000000 && rate_value==1);
    step(0,.15,10); cache_until=GetTickCount64()-1;
    step(0,.15,10); CHECK(cached_station==-1 && closes==before+1);
    step(ACTIVE,.15,10); mock_preparing=0; out=step(ACTIVE,.15,10);
    CHECK(out.bind && opens==opened+1 && seeks==sought+1 && mock_time==40000000);
    step(0,.15,10); opened=opens; before=closes;
    step(ACTIVE,.75,10); CHECK(opens==opened+1 && closes==before+1 && cached_station==-1);
    mock_preparing=0; mock_time=20000000; step(ACTIVE,.75,10); step(0,.75,10);
    CHECK(cached_station==1); before=closes;
    step(0,.75,11); CHECK(cached_station==-1 && closes==before+1);
    step(ACTIVE,.15,11); mock_preparing=0; step(ACTIVE,.15,11); step(0,.15,11);
    CHECK(cached_station==0); tf_video_shutdown(NULL); CHECK(cached_station==-1);
}
static void bridge_fault_tests(void) {
    TfBridgeHeader bad={.kind=TF_BRIDGE_INPUT,.size=TF_BRIDGE_MAX+1}; memcpy(bad.magic,"TFVPIPE1",8);
    DWORD sent; CHECK(WriteFile(request_client,&bad,sizeof(bad),&sent,NULL));
    uint32_t kind,size; unsigned char data[TF_BRIDGE_MAX];
    CHECK(tf_bridge_receive(&bridge,&kind,data,&size)==-1 && bridge.fault);
    /* Reload recreates the transport rather than retaining a poisoned stream. */
    CloseHandle(request_client); CloseHandle(response_client);
    CHECK(tf_bridge_create(&bridge));
    request_client=CreateFileW(bridge.input_name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    response_client=CreateFileW(bridge.output_name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    CHECK(request_client!=INVALID_HANDLE_VALUE && response_client!=INVALID_HANDLE_VALUE && !bridge.fault);
    CHECK(tf_bridge_receive(&bridge,&kind,data,&size)==0); /* an empty input never blocks */
}
/* The mocks' layouts: the real game's profile values (tests/expected_profile.json), which fit the mock objects above. */
static void test_profile(void) {
    static const struct { int key; uint32_t value; } values[]={
        {TFV_SOUND_CHANNELS,2208},{TFV_MEDIA_PLAY_ON_OPEN,249},{TFV_MEDIA_NATIVE_AUDIO,248},{TFV_MEDIA_FACADE,336},
        {TFV_TEXTURE_NEW_STYLE,341},{TFV_TEXTURE_DIMENSIONS,432},{TFV_TEXTURE_ASPECT,344},{TFV_RADIO_FINE,1880},
        {TFV_RADIO_ACTIVE,1136},{TFV_RADIO_STRENGTH,1388},{TFV_RADIO_STORY,2044},{TFV_RADIO_DIAL,1488},
        {TFV_DIAL_CAPACITY,880},{TFV_DIAL_NEEDLES,920},{TFV_DIAL_PANEL,896},{TFV_NEEDLE_IMAGE,0},{TFV_NEEDLE_SLOT,8},
        {TFV_NEEDLE_SOURCE,16},{TFV_SLOT_X,56},{TFV_TEXTURE_RHI,16},{TFV_RHI_EXTENT,68},{TFV_RHI_FORMAT,83},
        {TFV_SOUND_ACTIVE,150},{TFV_SOUND_ACTIVE_MASK,16},{TFV_PIXEL_BGRA,2},{TFV_PIXEL_RGBA,37},{TFV_FMOD_RUNTIME,1},
        {TFV_SOUND_STEREO,1},{TFV_BLEND_OPAQUE,0},{TFV_SIGNAL_DISCOVERED,2}};
    CHECK(sizeof(values)/sizeof(*values)==TF_VALUE_COUNT);
    for (size_t i=0;i<TF_VALUE_COUNT;++i) tf_profile.values[values[i].key]=values[i].value;
}
/* profile_read takes only a whole profile of this layout, and leaves the one it has otherwise. */
static void profile_file_tests(void) {
    wchar_t path[2048]; swprintf(path,2048,L"%ls\\tf_video.profile",folder);
    TfProfile written; memset(&written,0,sizeof(written));
    memcpy(written.magic,"TFVPROF1",8); written.version=TF_PROFILE_VERSION; written.size=sizeof(written);
    written.timestamp=1234; written.values[TFV_SLOT_X]=99;
    TfProfile kept=tf_profile;
    HANDLE f; DWORD n;
    f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(f!=INVALID_HANDLE_VALUE && WriteFile(f,&written,sizeof(written)-1,&n,NULL)); CloseHandle(f);
    CHECK(!profile_read(path) && !memcmp(&tf_profile,&kept,sizeof(kept)));        /* cut off */
    written.version=TF_PROFILE_VERSION+1;
    f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(f!=INVALID_HANDLE_VALUE && WriteFile(f,&written,sizeof(written),&n,NULL)); CloseHandle(f);
    CHECK(!profile_read(path) && !memcmp(&tf_profile,&kept,sizeof(kept)));        /* another layout */
    written.version=TF_PROFILE_VERSION;
    f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(f!=INVALID_HANDLE_VALUE && WriteFile(f,&written,sizeof(written),&n,NULL)); CloseHandle(f);
    CHECK(profile_read(path) && tf_profile.timestamp==1234 && PV(SLOT_X)==99);
    CHECK(!validate_image((unsigned char *)player_bytes));                       /* not the game it was written for */
    CHECK(DeleteFileW(path) && !profile_read(path));
    tf_profile=kept;
}
int main(void) {
    test_profile();
    wchar_t exe[2048],video[2048],second[2048]; CHECK(GetModuleFileNameW(NULL,exe,2048)>0);
    wchar_t *end=wcsrchr(exe,L'\\'); CHECK(end!=NULL); *end=0;
    swprintf(folder,2048,L"%ls\\driver-fixture",exe); CHECK(CreateDirectoryW(folder,NULL) || GetLastError()==ERROR_ALREADY_EXISTS);
    profile_file_tests();
    /* publish_marks writes into the temp folder: the fixture's here, never the real one. */
    CHECK(SetEnvironmentVariableW(L"TMP",folder) && SetEnvironmentVariableW(L"TEMP",folder));
    swprintf(videos,2048,L"%ls\\Videos",folder); CHECK(CreateDirectoryW(videos,NULL) || GetLastError()==ERROR_ALREADY_EXISTS);
    CHECK(tf_bridge_create(&bridge));
    request_client=CreateFileW(bridge.input_name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    response_client=CreateFileW(bridge.output_name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    CHECK(request_client!=INVALID_HANDLE_VALUE && response_client!=INVALID_HANDLE_VALUE);
    swprintf(video,2048,L"%ls\\test.mp4",videos);
    swprintf(second,2048,L"%ls\\second.mkv",videos);
    HANDLE f=CreateFileW(video,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); CHECK(f!=INVALID_HANDLE_VALUE); CloseHandle(f);
    f=CreateFileW(second,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); CHECK(f!=INVALID_HANDLE_VALUE); CloseHandle(f);
    char error[256]; CHECK(tf_parse_config("[General]\ntolerance=.01\nfadeMs=0\nwarmDecoderMs=0\n[A]\nfile=test.mp4\nfrequency=.15\n[B]\nfile=test.mp4\nfrequency=.75\n",&config,error,sizeof(error)));
    initialized=valid_profile=1; tf_controller_init(&controller); functions();
    memcpy(player_bytes+PV(MEDIA_FACADE),&facade_marker,sizeof(facade_marker));
    radio_bytes[PV(RADIO_ACTIVE)]=1;
    int dims[]={1280,720}; float aspect=16.0f/9.0f;
    memcpy(texture_bytes+PV(TEXTURE_DIMENSIONS),dims,sizeof(dims)); memcpy(texture_bytes+PV(TEXTURE_ASPECT),&aspect,4);
    void *rhi=rhi_bytes; int original_extent[]={640,512}; expected_format=PV(PIXEL_BGRA);
    memcpy(resource_bytes+PV(TEXTURE_RHI),&rhi,sizeof(rhi));
    memcpy(rhi_bytes+PV(RHI_EXTENT),original_extent,sizeof(original_extent)); rhi_bytes[PV(RHI_FORMAT)]=(unsigned char)expected_format;
    Output out=step(ACTIVE,.15,1); CHECK(out.phase==TF_OPENING && created==1 && opens==1);
    mock_preparing=0; mock_time=100000; out=step(ACTIVE,.15,1); CHECK(out.bind && draw_calls==1);
    out=step(ACTIVE|BOUND,.15,1);
    float pcm[64]; tf_audio_test_pull(pcm,32); CHECK(pcm[0]==.25f);
    for(int i=0;i<50;++i) step(ACTIVE|BOUND,.15+(i%2)*.003,1);
    CHECK(opens==1 && draw_calls==1); /* same PTS does not redraw; dial jitter does not reopen */
    expected_format=PV(PIXEL_RGBA); rhi_bytes[PV(RHI_FORMAT)]=(unsigned char)expected_format;
    out=step(ACTIVE|BOUND,.15,1); CHECK(out.bind && target_format==expected_format && draw_calls==2);
    int square_extent[]={2048,2048}; expected_y=105; expected_height=270;
    memcpy(rhi_bytes+PV(RHI_EXTENT),square_extent,sizeof(square_extent));
    out=step(ACTIVE|BOUND,.15,1); CHECK(out.bind && target_width==640 && target_height==480 && draw_calls==3);
    expected_y=71.25; expected_height=337.5; memcpy(rhi_bytes+PV(RHI_EXTENT),original_extent,sizeof(original_extent));
    out=step(ACTIVE|BOUND,.15,1); CHECK(out.bind && draw_calls==4); /* paused/same-PTS UV changes redraw */
    out=step(ACTIVE|BOUND|PAUSED,.15,1); CHECK(out.phase==TF_PAUSED && rate_value==0);
    tf_audio_test_pull(pcm,32); CHECK(pcm[0]==0);
    step(ACTIVE|BOUND,.15,1); CHECK(rate_value==1);
    out=step(ACTIVE|BOUND,.75,1); CHECK(out.phase==TF_OPENING && opens==2 && created==2 && released==1);
    CHECK(live_generator->connected); /* regression: OnEndGenerate must not leave subsequent stations silent */
    mock_preparing=0; mock_time=200000; step(ACTIVE|BOUND,.75,1);
    tf_audio_test_pull(pcm,32); CHECK(pcm[0]==.25f);
    radio_bytes[PV(RADIO_FINE)]=1; out=step(ACTIVE|BOUND,.75,1); CHECK(!out.bind && out.phase==TF_IDLE);
    tf_audio_test_pull(pcm,32); CHECK(pcm[0]==0);
    radio_bytes[PV(RADIO_FINE)]=0; radio_bytes[PV(RADIO_STORY)]=1;
    out=step(ACTIVE,.15,1); CHECK(out.phase==TF_IDLE);
    radio_bytes[PV(RADIO_STORY)]=0; float signal=.1f; memcpy(radio_bytes+PV(RADIO_STRENGTH),&signal,4);
    out=step(ACTIVE,.15,1); CHECK(out.phase==TF_IDLE);
    signal=0; memcpy(radio_bytes+PV(RADIO_STRENGTH),&signal,4);
    step(ACTIVE,.15,1); mock_preparing=0; step(ACTIVE|BOUND,.15,1);
    out=step(0,.15,1); CHECK(out.phase==TF_IDLE && !out.bind);
    step(ACTIVE,.15,1); mock_preparing=0; step(ACTIVE|BOUND,.15,1);
    out=step(ACTIVE|MEDIA_FAILED,.15,1); CHECK(out.phase==TF_ERROR && !out.bind);
    int before=opens; step(ACTIVE|MEDIA_FAILED,.15,1); CHECK(opens==before);
    out=step(0,.15,2); CHECK(out.phase==TF_IDLE && audio_initialized==0);
    tf_video_shutdown(NULL); CHECK(released==created && ended>=created);
    marker_tests(); resume_tests(); reservation_packet_tests(); warm_decoder_tests(); bridge_fault_tests();
    CHECK(released==created && ended>=created);
    CloseHandle(request_client); CloseHandle(response_client); tf_bridge_close(&bridge);
    CHECK(DeleteFileW(video) && DeleteFileW(second)); CHECK(RemoveDirectoryW(videos)); CHECK(RemoveDirectoryW(folder));
    printf("PASS %d actual native driver checks with engine mocks (not an in-game decode test)\n",checks); return 0;
}
