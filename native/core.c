#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

static int equal(const char *a, const char *b) {
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return *a == *b;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    return s;
}
static int number(const char *s, double *out) {
    char *end;
    *out = strtod(s, &end);
    return *s && !*end && isfinite(*out);
}
static int boolean(const char *s, int *out) {
    if (equal(s,"true") || equal(s,"1")) { *out = 1; return 1; }
    if (equal(s,"false") || equal(s,"0")) { *out = 0; return 1; }
    return 0;
}
int tf_safe_filename(const char *name) {
    size_t n = strlen(name);
    if (n < 5 || n >= TF_FILE_BYTES || (!equal(name+n-4, ".mp4") && !equal(name+n-4, ".mkv")) || strstr(name,"..")) return 0;
    for (const unsigned char *p=(const unsigned char *)name; *p; ++p)
        if (*p < 32 || strchr("/\\:*?\"<>|",*p)) return 0;
    return name[0] != '.' && name[n-1] != ' ';
}
/* The audio codecs in the game's Matroska codec table (Electra, UE 5.6). The reader leaves out any other audio track,
 * so a video whose audio is only E-AC-3, AC-3, DTS or TrueHD plays without sound. */
static const char *const MKV_AUDIO[] = {"A_AAC","A_AAC/MPEG4/LC","A_AAC/MPEG4/LC/SBR","A_OPUS","A_FLAC",
    "A_MPEG/L1","A_MPEG/L2","A_MPEG/L3"};
static int ebml_vint(const unsigned char *data, size_t end, size_t *at, int marker, uint64_t *out) {
    if (*at >= end) return 0;
    int length = 1;
    while (length <= 8 && !(data[*at] & (0x80 >> (length-1)))) ++length;
    if (length > (marker ? 4 : 8) || *at + length > end) return 0;
    uint64_t value = marker ? data[*at] : data[*at] & (0xFF >> length), ones = (1ull << (7*length)) - 1;
    for (int i = 1; i < length; ++i) value = value << 8 | data[*at+i];
    *at += length;
    *out = !marker && value == ones ? UINT64_MAX : value;
    return 1;
}
/* One element at `at` inside [at, end): its ID and where its data starts and ends. `whole`: all of it is in the
 * buffer (an unknown size, or one past the buffer, ends at `end`). */
static int ebml_element(const unsigned char *data, size_t end, size_t *at, uint64_t *id, size_t *stop, int *whole) {
    uint64_t size;
    if (!ebml_vint(data,end,at,1,id) || !ebml_vint(data,end,at,0,&size)) return 0;
    *whole = size <= end - *at;
    *stop = *whole ? *at + (size_t)size : end;
    return 1;
}
int tf_mkv_audio(const unsigned char *data, size_t size, char *codec, size_t codec_size) {
    size_t at = 0, stop;
    uint64_t id;
    int audio = 0, whole;
    if (codec_size) *codec = 0;
    while (ebml_element(data,size,&at,&id,&stop,&whole)) {
        if (id == 0x18538067) { size = stop; continue; }                 /* Segment: into it */
        if (id == 0x1F43B675) break;                                    /* Cluster: the tracks came before */
        if (id != 0x1654AE6B) { if (!whole) break; at = stop; continue; }
        for (size_t track_end, tracks_end = stop; at < tracks_end && ebml_element(data,tracks_end,&at,&id,&track_end,&whole); at = track_end) {
            if (!whole) break;                                          /* cut off by the buffer: unknown */
            if (id != 0xAE) continue;                                   /* TrackEntry */
            uint64_t type = 0, field;
            char name[64] = "";
            for (size_t field_end; at < track_end && ebml_element(data,track_end,&at,&field,&field_end,&whole); at = field_end) {
                size_t n = field_end - at;
                if (field == 0x83 && n && n <= 8) for (type = 0; at < field_end; ++at) type = type << 8 | data[at];
                else if (field == 0x86 && n < sizeof(name)) { memcpy(name,data+at,n); name[n] = 0; }
            }
            if (type != 2) continue;
            for (size_t i = 0; i < sizeof(MKV_AUDIO)/sizeof(*MKV_AUDIO); ++i) if (!strcmp(name,MKV_AUDIO[i])) return 1;
            if (!audio++ && codec_size) snprintf(codec,codec_size,"%s",*name ? name : "unknown");
        }
        break;
    }
    return audio ? 0 : -1;
}
int tf_parse_config(const char *text, TfConfig *config, char *error, size_t size) {
    memset(config,0,sizeof(*config));
    config->tolerance=0.010; config->hysteresis=0.005; config->fade_ms=200; config->auto_assign=1;
    config->output_width=640; config->output_height=480;
    config->warm_decoder_ms=3000;
    config->frequency_side=0; /* keep the group at a free end of the game's dial */
    unsigned masks[TF_MAX_STATIONS]={0}, general_mask=0, line_no=0;
    int section=-2;
    if (!text) { snprintf(error,size,"configuration is missing"); return 0; }
    if (!strncmp(text,"\xEF\xBB\xBF",3)) text+=3;
    while (*text) {
        char line[2048]; size_t n=0;
        ++line_no;
        while (*text && *text!='\n') {
            if (n+1 >= sizeof(line)) goto invalid;
            line[n++]=*text++;
        }
        if (*text) ++text;
        line[n]=0;
        char *s=trim(line);
        if (!*s || *s==';' || *s=='#') continue;
        if (*s=='[') {
            char *end=strchr(s,']');
            if (!end || *trim(end+1)) goto invalid;
            *end=0; s=trim(s+1);
            if (equal(s,"General")) { section=-1; continue; }
            if (!*s || strlen(s)>=sizeof(config->stations[0].name) || config->count==TF_MAX_STATIONS) goto invalid;
            for (int i=0;i<config->count;++i) if (equal(s,config->stations[i].name)) goto invalid;
            section=config->count++;
            TfStation *station=&config->stations[section];
            strcpy(station->name,s); station->frequency=-1; station->volume=1; station->loop=1; station->enabled=1;
            continue;
        }
        char *sep=strchr(s,'=');
        if (!sep || section==-2) goto invalid;
        *sep=0; char *key=trim(s), *value=trim(sep+1);
        /* Semicolon comments are accepted outside a quoted filename. */
        if (*value=='"') {
            char *end=strchr(value+1,'"');
            if (!end || (*trim(end+1) && *trim(end+1)!=';')) goto invalid;
            *end=0; ++value;
        } else { char *comment=strchr(value,';'); if (comment) *comment=0; value=trim(value); }
        double num=0; unsigned bit=0;
        if (section==-1) {
            if (equal(key,"tolerance")) {
                bit=1; if (!number(value,&num) || num<0.000001 || num>0.1) goto invalid;
                config->tolerance=num;
            } else if (equal(key,"hysteresis")) {
                bit=2; if (!number(value,&num) || num<0 || num>0.1) goto invalid;
                config->hysteresis=num;
            } else if (equal(key,"fadeMs")) {
                bit=4; if (!number(value,&num) || num<0 || num>2000 || floor(num)!=num) goto invalid;
                config->fade_ms=(unsigned)num;
            } else if (equal(key,"autoAssign")) {
                bit=8; if (!boolean(value,&config->auto_assign)) goto invalid;
            } else if (equal(key,"outputWidth")) {
                bit=16; if (!number(value,&num) || num<160 || num>2048 || floor(num)!=num) goto invalid;
                config->output_width=(unsigned)num;
            } else if (equal(key,"outputHeight")) {
                bit=32; if (!number(value,&num) || num<120 || num>2048 || floor(num)!=num) goto invalid;
                config->output_height=(unsigned)num;
            } else if (equal(key,"frequencySide")) {
                bit=64;
                if (equal(value,"auto")) config->frequency_side=0;
                else if (equal(value,"left")) config->frequency_side=1;
                else if (equal(value,"right")) config->frequency_side=2;
                else if (equal(value,"both")) config->frequency_side=3;
                else if (equal(value,"full")) config->frequency_side=4;
                else goto invalid;
            } else if (equal(key,"warmDecoderMs")) {
                bit=128; if (!number(value,&num) || num<0 || num>10000 || floor(num)!=num) goto invalid;
                config->warm_decoder_ms=(unsigned)num;
            } else goto invalid;
            if (general_mask&bit) goto invalid;
            general_mask|=bit;
        } else {
            TfStation *station=&config->stations[section];
            if (equal(key,"file")) {
                bit=1; if (!tf_safe_filename(value)) goto invalid;
                strcpy(station->file,value);
            } else if (equal(key,"frequency")) {
                bit=2; if (!number(value,&num) || num<0 || num>1) goto invalid;
                station->frequency=num;
            } else if (equal(key,"loop")) {
                bit=4; if (!boolean(value,&station->loop)) goto invalid;
            } else if (equal(key,"volume")) {
                bit=8; if (!number(value,&num) || num<0 || num>2) goto invalid;
                station->volume=num;
            } else goto invalid;
            if (masks[section]&bit) goto invalid;
            masks[section]|=bit;
        }
    }
    for (int i=0;i<config->count;++i) {
        if ((masks[i]&3)!=3) { snprintf(error,size,"[%s] requires file and frequency",config->stations[i].name); return 0; }
        for (int j=0;j<i;++j)
            if (fabs(config->stations[i].frequency-config->stations[j].frequency)<=2*(config->tolerance+config->hysteresis+TF_DIAL_EPSILON)) {
                snprintf(error,size,"[%s] overlaps [%s], including hysteresis",config->stations[i].name,config->stations[j].name);
                return 0;
            }
    }
    config->tolerance_set=(general_mask&1)!=0; /* else the stations take the game's own range (tf_video.c) */
    error[0]=0; return 1;
invalid:
    snprintf(error,size,"invalid, unknown, or duplicate setting at line %u",line_no); return 0;
}
static int frequency_available(const TfConfig *config,double frequency) {
    if (!isfinite(frequency) || frequency<0 || frequency>1) return 0;
    double reach=config->tolerance+config->hysteresis+TF_DIAL_EPSILON;
    if (frequency-reach<=0 || frequency+reach>=1) return 0; /* automatic bands leave both dial end stops free */
    for (int i=0;i<config->range_count;++i)
        if (frequency+reach>=config->ranges[i].low-1e-12 && frequency-reach<=config->ranges[i].high+1e-12) return 0;
    for (int i=0;i<config->count;++i)
        if (fabs(frequency-config->stations[i].frequency)<=2*reach) return 0;
    return 1;
}
static int frequency_in_band(const TfConfig *config,double frequency) {
    int side=config->frequency_side?config->frequency_side:3;
    return config->frequency_side==4 ||
        ((side&1) && frequency>=.01-1e-12 && frequency<=.20+1e-12) ||
        ((side&2) && frequency>=.80-1e-12 && frequency<=.99+1e-12);
}
int tf_add_auto_station(TfConfig *config,const char *file,double preferred_frequency) {
    if (!config->auto_assign || !tf_safe_filename(file) || config->count>=TF_MAX_STATIONS) return -1;
    for (int i=0;i<config->count;++i) if (equal(file,config->stations[i].file)) return -1;
    double frequency=-1;
    if (frequency_in_band(config,preferred_frequency) && frequency_available(config,preferred_frequency)) frequency=preferred_frequency;
    else {
        /* A small fixed grid avoids floating-point drift and leaves the end stops free.
         * Skip both entry and hysteresis ranges, including those of manual stations. */
        int step=1;
        for (int at=0;at<=980;at+=step) {
            int milli=config->frequency_side==1 || config->frequency_side==4?10+at:990-at;
            double candidate=(double)milli/1000.0;
            if (frequency_in_band(config,candidate) && frequency_available(config,candidate)) { frequency=candidate; break; }
        }
    }
    if (frequency<0) return -1;
    int index=config->count;
    TfStation *station=&config->stations[index];
    memset(station,0,sizeof(*station));
    for (int number=1;number<=TF_MAX_STATIONS+1;++number) {
        snprintf(station->name,sizeof(station->name),"Auto_%d",number);
        int duplicate=0;
        for (int i=0;i<index;++i) if (equal(station->name,config->stations[i].name)) duplicate=1;
        if (!duplicate) break;
    }
    strcpy(station->file,file); station->frequency=frequency; station->volume=1;
    station->loop=station->enabled=station->automatic=1;
    ++config->count;
    return index;
}
int tf_reassign_automatic(TfConfig *config,const TfRange *ranges,int count,int valid) {
    if (count<0 || count>TF_MAX_RANGES || (count && !ranges)) return -1;
    for (int i=0;i<count;++i)
        if (!isfinite(ranges[i].low) || !isfinite(ranges[i].high) || ranges[i].low>ranges[i].high) return -1;
    config->range_count=count;
    if (count) memcpy(config->ranges,ranges,(size_t)count*sizeof(*ranges));
    int indices[TF_MAX_STATIONS],automatic=0,changes=0;
    for (int i=0;i<config->count;++i) if (config->stations[i].automatic) indices[automatic++]=i;
    if (!valid) {
        for (int i=0;i<automatic;++i) {
            TfStation *station=&config->stations[indices[i]];
            changes+=!!station->enabled; station->enabled=0;
        }
        return changes;
    }
    int sides[2]={config->frequency_side,config->frequency_side},trials=1;
    if (!config->frequency_side) {
        int left=automatic>0;
        for (int i=0;i<automatic;++i) if (config->stations[indices[i]].frequency>.20) left=0;
        sides[0]=left?1:2; sides[1]=left?2:1; trials=2;
    }
    double best[TF_MAX_STATIONS],candidate[TF_MAX_STATIONS];
    int best_count=-1;
    TfConfig scratch;
    for (int trial=0;trial<trials;++trial) {
        scratch=*config; scratch.count=0; scratch.frequency_side=sides[trial];
        for (int i=0;i<config->count;++i)
            if (!config->stations[i].automatic) scratch.stations[scratch.count++]=config->stations[i];
        int assigned=0;
        for (int i=0;i<automatic;++i) {
            TfStation *station=&config->stations[indices[i]];
            int added=tf_add_auto_station(&scratch,station->file,station->frequency);
            candidate[i]=added<0?-1:scratch.stations[added].frequency;
            assigned+=added>=0;
        }
        if (assigned>best_count) { memcpy(best,candidate,(size_t)automatic*sizeof(*best)); best_count=assigned; }
        if (assigned==automatic) break;
    }
    for (int i=0;i<automatic;++i) {
        TfStation *station=&config->stations[indices[i]];
        int enabled=best[i]>=0;
        changes+=(station->enabled!=enabled) || (enabled && station->frequency!=best[i]);
        station->enabled=enabled;
        if (enabled) station->frequency=best[i];
    }
    return changes;
}
double tf_quick_tune(const TfConfig *config, double before, double after, int up) {
    double direction=up?1:-1, best=-1;
    for (int i=0;i<config->count;++i) {
        const TfStation *station=&config->stations[i];
        double step=(station->frequency-before)*direction;
        if (station->enabled && step>1e-6 && (best<0 || step<(best-before)*direction)) best=station->frequency;
    }
    if (best<0) return -1;
    double game=(after-before)*direction;
    return game>1e-6 && game<=(best-before)*direction ? -1 : best;  /* the game's signal comes first */
}
int tf_choose_station(const TfConfig *config, int current, double frequency, int priority) {
    if (priority || !isfinite(frequency) || frequency<0 || frequency>1) return -1;
    if (current>=0 && current<config->count && config->stations[current].enabled &&
        fabs(frequency-config->stations[current].frequency)<=config->tolerance+config->hysteresis+TF_DIAL_EPSILON) return current;
    int best=-1; double distance=config->tolerance+config->hysteresis+TF_DIAL_EPSILON;
    for (int i=0;i<config->count;++i) {
        double d=fabs(frequency-config->stations[i].frequency);
        if (config->stations[i].enabled && d<=distance) { best=i; distance=d; }
    }
    return best;
}
void tf_controller_init(TfController *c) { memset(c,0,sizeof(*c)); c->station=c->pending=-1; }
static void stop(TfController *c,const TfBackend *b) {
    if (c->phase!=TF_IDLE && c->phase!=TF_ENDED && c->phase!=TF_ERROR) b->close(b->context);
    c->phase=TF_IDLE; c->station=-1; c->pending=-1; c->display=c->audible=0; c->gain=0;
}
void tf_controller_step(TfController *c,const TfConfig *cfg,const TfBackend *b,double frequency,
                        int active,int priority,int paused,uint32_t epoch,uint64_t now) {
    if (epoch!=c->epoch) { stop(c,b); c->epoch=epoch; }
    if (!active || priority) { stop(c,b); return; }
    int wanted=tf_choose_station(cfg,c->phase==TF_FADING?-1:c->station,frequency,0);
    if (c->phase==TF_FADING) {
        c->pending=wanted;
        if (wanted==c->station && !paused) {
            /* A quick step back keeps this decoder/time; do not finish closing it
             * and seek/reopen a file that is still playing. */
            c->phase=TF_PLAYING; c->pending=-1;
        } else {
            c->display=0;
            if (paused) { stop(c,b); return; }
            if (now<c->fade_until) {
                c->gain=(float)cfg->stations[c->station].volume*(float)(c->fade_until-now)/(float)(c->fade_until-c->fade_began);
                c->audible=1; return;
            }
            stop(c,b);
        }
    }
    if (wanted!=c->station) {
        if (c->phase==TF_PLAYING && !paused && cfg->fade_ms) {
            c->phase=TF_FADING; c->pending=wanted; c->fade_began=now; c->fade_until=now+cfg->fade_ms;
            c->display=0; return;
        }
        stop(c,b);
        c->station=wanted;
        if (wanted<0) return;
        c->opened_at=now;
        if (b->open(b->context,wanted)) c->phase=TF_OPENING;
        else { b->close(b->context); c->phase=TF_ERROR; }
    }
    if (c->station<0 || c->phase==TF_ERROR || c->phase==TF_ENDED) return;
    if (b->failed(b->context) || (c->phase==TF_OPENING && now-c->opened_at>15000)) {
        b->close(b->context); c->phase=TF_ERROR; c->display=c->audible=0; return;
    }
    if (c->phase==TF_OPENING) {
        if (b->preparing(b->context)) return;
        if (!b->playing(b->context)) { if (!b->rate(b->context,1)) return; }
        c->phase=TF_PLAYING;
    }
    if (paused && c->phase==TF_PLAYING) {
        if (!b->rate(b->context,0)) { stop(c,b); return; }
        c->phase=TF_PAUSED;
    } else if (!paused && c->phase==TF_PAUSED) {
        if (!b->rate(b->context,1)) { stop(c,b); return; }
        c->phase=TF_PLAYING;
    }
    if (c->phase==TF_PLAYING && !b->playing(b->context) && !b->preparing(b->context)) {
        int64_t duration=b->duration(b->context), position=b->position(b->context);
        if (duration>0 && position>=duration-500000 && !cfg->stations[c->station].loop) {
            b->close(b->context); c->phase=TF_ENDED; c->display=c->audible=0; return;
        }
        /* Looping is handled by the engine; a stopped/broken player must not remain bound. */
        if (now-c->opened_at>15000) { b->close(b->context); c->phase=TF_ERROR; c->display=c->audible=0; return; }
    }
    c->display=1; c->audible=c->phase==TF_PLAYING;
    c->gain=c->audible?(float)cfg->stations[c->station].volume:0;
}
TfRect tf_fit(double aspect,double w,double h) {
    TfRect r={0,0,w,h};
    if (!(aspect>0) || !isfinite(aspect) || !(w>0) || !(h>0)) return r;
    if (w/h>aspect) { r.width=h*aspect; r.x=(w-r.width)*0.5; }
    else { r.height=w/aspect; r.y=(h-r.height)*0.5; }
    return r;
}
void tf_mix(float *out,const float *in,int ic,int oc,const float *stereo,unsigned frames,
            float *gain,float target,unsigned ramp_samples) {
    if (oc<=0 || oc>32 || ic<0 || ic>32) return;
    float step=ramp_samples?1.0f/(float)ramp_samples:1;
    for (unsigned f=0;f<frames;++f) {
        if (*gain<target) *gain=fminf(target,*gain+step);
        else if (*gain>target) *gain=fmaxf(target,*gain-step);
        for (int ch=0;ch<oc;++ch) {
            float base=in && ch<ic?in[(size_t)f*ic+ch]:0;
            float add=ch==0?stereo[f*2]:(ch==1?stereo[f*2+1]:0);
            if (oc==1) add=(stereo[f*2]+stereo[f*2+1])*0.5f;
            out[(size_t)f*oc+ch]=base+add*(*gain);
        }
    }
}
