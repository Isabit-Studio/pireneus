// Audio output for UWP, where SDL has no audio device : an XAudio2 voice pulls interleaved float
// samples from a callback (in practice SDL_mixer's MIX_Generate, see src/audio/audio.c).

#ifndef _UWP_AUDIO_H_
#define _UWP_AUDIO_H_

#ifdef __cplusplus
extern "C" {
#endif

// Called from an audio thread, to fill `buffer` with `size` bytes of interleaved 32-bit float samples.
typedef void (*UwpAudioGenerate)(void* user_data, float* buffer, int size);

int uwp_audio_start(UwpAudioGenerate generate, void* user_data, int* out_rate, int* out_channels);
void uwp_audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif // _UWP_AUDIO_H_
