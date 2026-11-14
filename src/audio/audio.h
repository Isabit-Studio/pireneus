#ifndef _AUDIO_H_
#define _AUDIO_H_

#ifdef __cplusplus
extern "C" {
#endif

// Music and sounds, through SDL_mixer : WAV, AIFF, MP3, FLAC and Ogg Vorbis.

typedef struct AudioClip AudioClip;   // a loaded sound
typedef struct AudioVoice AudioVoice; // a sound being played

typedef enum
{
    AUDIO_BUS_MUSIC,
    AUDIO_BUS_SOUND,
    AUDIO_BUS_COUNT
} AudioBus;

int audio_init(void);  // 0 if no audio device could be opened ; every other call is then a no-op
void audio_shutdown(void);

void audio_set_volumes(float master, float music, float sound);

// `path` is absolute. `stream` : decoded while playing rather than up front, for long music.
AudioClip* audio_load(const char* path, int stream);
void audio_destroy_clip(AudioClip* clip);
float audio_clip_duration(AudioClip* clip); // seconds

AudioVoice* audio_play(AudioClip* clip, AudioBus bus, float volume, int loop);
void audio_set_voice_position(AudioVoice* voice, float x, float y, float z); // relative to the listener, for 3D sound
void audio_stop(AudioVoice* voice);   // and releases it
void audio_stop_all(void);            // releases every voice
int audio_is_playing(AudioVoice* voice);

#ifdef __cplusplus
}
#endif

#endif // _AUDIO_H_
