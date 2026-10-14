#ifndef TF_VIDEO_CORE_H
#define TF_VIDEO_CORE_H
#include <stdint.h>
#include <stddef.h>
#define TF_MAX_STATIONS 64
#define TF_FILE_BYTES 768
#define TF_MAX_RANGES 256
/* The game exposes normalized dial positions as float32, while INI values are doubles. */
#define TF_DIAL_EPSILON 0.0000001
typedef struct { double low,high; } TfRange;
typedef struct { char name[64], file[TF_FILE_BYTES]; double frequency, volume; int loop, enabled, automatic; } TfStation;
typedef struct { double tolerance, hysteresis; unsigned fade_ms,output_width,output_height,warm_decoder_ms; int count, auto_assign,frequency_side,range_count; TfRange ranges[TF_MAX_RANGES]; TfStation stations[TF_MAX_STATIONS]; } TfConfig;
int tf_parse_config(const char *text, TfConfig *config, char *error, size_t size);
int tf_add_auto_station(TfConfig *config, const char *file, double preferred_frequency);
int tf_reassign_automatic(TfConfig *config, const TfRange *ranges, int count, int valid);
int tf_choose_station(const TfConfig *config, int current, double frequency, int priority);
int tf_safe_filename(const char *name);
/* A Matroska file's first bytes: 1 if one of its audio tracks has a codec the game plays, 0 if it has audio tracks
 * but none the game plays (`codec`: the first one's CodecID), -1 if these bytes don't tell. */
int tf_mkv_audio(const unsigned char *data, size_t size, char *codec, size_t codec_size);
typedef enum { TF_IDLE, TF_OPENING, TF_PLAYING, TF_PAUSED, TF_FADING, TF_ENDED, TF_ERROR } TfPhase;
typedef struct {
    void *context;
    int (*open)(void *, int);
    void (*close)(void *);
    int (*rate)(void *, float);
    int (*preparing)(void *);
    int (*playing)(void *);
    int (*failed)(void *);
    int64_t (*position)(void *);
    int64_t (*duration)(void *);
} TfBackend;
typedef struct {
    TfPhase phase;
    int station, pending, display, audible;
    uint32_t epoch;
    uint64_t opened_at, fade_until, fade_began;
    float gain;
} TfController;
void tf_controller_init(TfController *controller);
void tf_controller_step(TfController *controller, const TfConfig *config, const TfBackend *backend,
                        double frequency, int active, int priority, int paused, uint32_t epoch, uint64_t now_ms);
typedef struct { double x, y, width, height; } TfRect;
TfRect tf_fit(double source_aspect, double width, double height);
/* Stereo addition preserves the game's input and never allocates in an audio callback. */
void tf_mix(float *output, const float *input, int input_channels, int output_channels,
            const float *stereo, unsigned frames, float *gain, float target, unsigned ramp_samples);
#endif
