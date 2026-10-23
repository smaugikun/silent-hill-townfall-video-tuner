#ifndef TF_VIDEO_PROFILE_H
#define TF_VIDEO_PROFILE_H
#include <stdint.h>
/* What tf_video.dll takes from the game. game_profile.c (run through rundll32 after a game update) looks each of these
 * up in the game's PDB, checks it, and writes Scripts/tf_video.profile; the DLL takes that file only for the exact game
 * it was written for (tf_video.c validate_image). Changing these lists changes the file: raise TF_PROFILE_VERSION. */
#define TF_PROFILE_VERSION 2

/* The engine functions the DLL calls: key, name in the PDB, and the signature it calls them with (result and
 * arguments, `this` aside, as game_profile.c's type_text writes them). The keys index FN() in tf_video.c. */
#define TF_FUNCTIONS(X) \
    X(MEDIA_OPEN, "UMediaPlayer::OpenFile", "base10:1(FString*)") \
    X(MEDIA_CLOSE, "UMediaPlayer::Close", "base1:0()") \
    X(MEDIA_RATE, "UMediaPlayer::SetRate", "base10:1(base8:4)") \
    X(MEDIA_LOOP, "UMediaPlayer::SetLooping", "base10:1(base10:1)") \
    X(MEDIA_TIME, "UMediaPlayer::GetTime", "FTimespan()") \
    X(MEDIA_DURATION, "UMediaPlayer::GetDuration", "FTimespan()") \
    X(MEDIA_PLAYING, "UMediaPlayer::IsPlaying", "base10:1()") \
    X(MEDIA_PREPARING, "UMediaPlayer::IsPreparing", "base10:1()") \
    X(MEDIA_SEEK, "FMediaPlayerFacade::Seek", "base10:1(FTimespan*)") \
    X(MEDIA_SEEKING, "FMediaPlayerFacade::IsSeeking", "base10:1()") \
    X(TEXTURE_UPDATE, "UMediaTexture::UpdateResource", "base1:0()") \
    X(TEXTURE_SET, "UMediaTexture::SetMediaPlayer", "base1:0(UMediaPlayer*)") \
    X(SOUND_SET, "UMediaSoundComponent::SetMediaPlayer", "base1:0(UMediaPlayer*)") \
    X(SOUND_UPDATE, "UMediaSoundComponent::UpdatePlayer", "base1:0()") \
    X(SOUND_CREATE, "UMediaSoundComponent::CreateSoundGenerator", \
      "TSharedPtr<ISoundGenerator,1>(FSoundGeneratorInitParams*)") \
    X(GEN_AUDIO, "FMediaSoundGenerator::OnGenerateAudio", "base6:4(base8:4*, base6:4)") \
    X(GEN_END, "FMediaSoundGenerator::OnEndGenerate", "base1:0()") \
    X(SHARED_RELEASE, "TSharedPtr<ISoundGenerator,1>::~TSharedPtr<ISoundGenerator,1>", "base1:0()") \
    X(FMOD_GET, "IFMODStudioModule::Get", "IFMODStudioModule*()") \
    X(FMOD_SYSTEM, "FFMODStudioModule::GetStudioSystem", "FMOD::Studio::System*(EFMODSystemContext::Type)") \
    X(FMOD_AVAILABLE, "IFMODStudioModule::IsAvailable", "base10:1()") \
    X(CANVAS_BEGIN, "UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget", "base1:0(UObject*, " \
      "UTextureRenderTarget2D*, UCanvas**, UE::Math::TVector2<double>*, FDrawToRenderTargetContext*)") \
    X(CANVAS_END, "UKismetRenderingLibrary::EndDrawCanvasToRenderTarget", \
      "base1:0(UObject*, FDrawToRenderTargetContext*)") \
    X(CANVAS_CLEAR, "UKismetRenderingLibrary::ClearRenderTarget2D", \
      "base1:0(UObject*, UTextureRenderTarget2D*, FLinearColor)") \
    X(CANVAS_DRAW, "UCanvas::K2_DrawTexture", "base1:0(UTexture*, UE::Math::TVector2<double>, " \
      "UE::Math::TVector2<double>, UE::Math::TVector2<double>, UE::Math::TVector2<double>, FLinearColor, EBlendMode, " \
      "base8:4, UE::Math::TVector2<double>)") \
    X(TARGET_INIT, "UTextureRenderTarget2D::InitCustomFormat", "base1:0(base7:4, base7:4, EPixelFormat, base10:1)") \
    X(SOURCE_RESOURCE, "UTexture::GetResource", "FTextureResource*()") \
    X(WEAK_GET, "FWeakObjectPtr::Get", "UObject*()") \
    X(WEAK_ASSIGN, "FWeakObjectPtr::operator=", "base1:0(FObjectPtr)") \
    X(DIAL_UPDATE, "UCRTVTuningDisplayWidget::UpdateDisplayedSignal", "base1:0(FSignalDisplayData*)") \
    X(DIAL_REMOVE, "UCRTVTuningDisplayWidget::RemoveDisplayedSignal", "base1:0(UActorComponent*)") \
    X(MEDIA_GET_RATE, "UMediaPlayer::GetRate", "base8:4()") \
    X(IMAGE_COLOR, "UImage::SetColorAndOpacity", "base1:0(FLinearColor)") \
    X(SLOT_POSITION, "UCanvasPanelSlot::SetPosition", "base1:0(UE::Math::TVector2<double>)") \
    X(QUICK_TUNE, "UHandheldRadioComponent::DoRadioQuickTune", "base1:0(base10:1)") \
    X(SET_FREQUENCY, "UHandheldRadioComponent::SetTunedFrequency_Internal", "base1:0(base8:4, base10:1, base10:1)") \
    X(RADIO_WIDGET, "UHandheldRadioComponent::GetCRTVWidget", "UCRTVWidget*()") \
    X(QUICK_TUNE_JUMP, "UCRTVWidget::QuickTuneJump", "base1:0()")

/* The member offsets and enum values the DLL uses, through PV(key) in tf_video.c (game_profile.c says where each
 * comes from). */
#define TF_VALUES(X) \
    X(SOUND_CHANNELS) X(MEDIA_PLAY_ON_OPEN) X(MEDIA_NATIVE_AUDIO) X(MEDIA_FACADE) X(TEXTURE_NEW_STYLE) \
    X(TEXTURE_DIMENSIONS) X(TEXTURE_ASPECT) X(RADIO_FINE) X(RADIO_ACTIVE) X(RADIO_STRENGTH) X(RADIO_STORY) \
    X(RADIO_DIAL) X(DIAL_CAPACITY) X(DIAL_NEEDLES) X(DIAL_PANEL) X(NEEDLE_IMAGE) X(NEEDLE_SLOT) X(NEEDLE_SOURCE) \
    X(SLOT_X) X(TEXTURE_RHI) X(RHI_EXTENT) X(RHI_FORMAT) X(SOUND_ACTIVE) X(SOUND_ACTIVE_MASK) X(PIXEL_BGRA) \
    X(PIXEL_RGBA) X(FMOD_RUNTIME) X(SOUND_STEREO) X(BLEND_OPAQUE) X(SIGNAL_DISCOVERED) X(RADIO_FREQUENCY)

enum TfFunction {
#define X(key, name, signature) key,
    TF_FUNCTIONS(X)
#undef X
    TF_FUNCTION_COUNT
};
enum TfValue {
#define X(key) TFV_##key,
    TF_VALUES(X)
#undef X
    TF_VALUE_COUNT
};
static const struct { const char *name, *signature; } tf_function_info[TF_FUNCTION_COUNT] = {
#define X(key, name, signature) {name, signature},
    TF_FUNCTIONS(X)
#undef X
};

/* Scripts/tf_video.profile, as written and read: little-endian, no padding. */
typedef struct {
    char magic[8];                    /* "TFVPROF1" */
    uint32_t version, size;           /* TF_PROFILE_VERSION, sizeof(TfProfile) */
    uint32_t timestamp, image_size;   /* the game executable's build stamp and loaded size */
    unsigned char identity[24];       /* its PDB's CodeView record: "RSDS", the PDB's GUID and age */
    struct { uint32_t rva; unsigned char code[16]; } functions[TF_FUNCTION_COUNT];  /* each one's first 16 bytes */
    uint32_t values[TF_VALUE_COUNT];
} TfProfile;
_Static_assert(sizeof(TfProfile) == 48 + 20*TF_FUNCTION_COUNT + 4*TF_VALUE_COUNT, "profile file layout");
#endif
