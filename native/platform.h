#ifndef TF_VIDEO_PLATFORM_H
#define TF_VIDEO_PLATFORM_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdatomic.h>
#include "core.h"
typedef struct { void *object, *reference; } TfShared;
typedef int (*TfGenerateAudio)(void *, float *, int);
typedef void (*TfReleaseShared)(TfShared *);
typedef void (*TfEndAudio)(void *);
void tf_log(const char *format, ...);
/* Backend 0 waiting, 1 FMOD CRTV video bus, 2 native Windows fallback, -1 unavailable. */
int tf_audio_prepare(void *studio_system);
int tf_audio_rate(void);
void tf_audio_generator(TfShared *owned, TfGenerateAudio generate, TfReleaseShared release, TfEndAudio end);
void tf_audio_control(float gain, int enabled, uint64_t heartbeat, unsigned fade_ms);
void tf_audio_flush(void);
void tf_audio_shutdown(void);
uint64_t tf_audio_frames(void);
int tf_discover(const wchar_t *videos, TfConfig *config);
void tf_save_auto_map(const wchar_t *videos,const TfConfig *config);
/* 1 valid, 0 absent, -1 unreadable/invalid. Does not write configuration. */
int tf_read_config_file(const wchar_t *path, TfConfig *config);
int tf_video_exists(const wchar_t *videos, const char *file, wchar_t *path, size_t count);
int tf_same_filename(const char *a, const char *b);
#endif
