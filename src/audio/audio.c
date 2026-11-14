// Audio through SDL_mixer 3 : clips, voices (2D, or 3D relative to the listener), and Music / Sound
// buses under a master volume. On UWP, the mixer renders into XAudio2 (uwp_audio.h).

#include "audio/audio.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <stdio.h>

#include "debug.h"

#ifdef PIRENEUS_UWP
#include "platforms/uwp/uwp_audio.h"
#endif

#define MAX_VOICES 64

struct AudioClip
{
    MIX_Audio* audio;
};

struct AudioVoice
{
    MIX_Track* track;
    AudioBus bus;
    float volume;
    int used;
};

static MIX_Mixer* mixer;
static AudioVoice voices[MAX_VOICES];
static float bus_volumes[AUDIO_BUS_COUNT] = { 1.0f, 1.0f };
static float master_volume = 1.0f;

#ifdef PIRENEUS_UWP
static void generate_audio(void* user_data, float* buffer, int size)
{
    (void)user_data;

    int written = MIX_Generate(mixer, buffer, size);

    if (written < size)
    {
        SDL_memset((char*)buffer + SDL_max(0, written), 0, (size_t)(size - SDL_max(0, written)));
    }
}
#endif

int audio_init(void)
{
    if (mixer)
    {
        return 1;
    }

#ifdef PIRENEUS_UWP
    // No SDL audio device here : the mixer generates samples for XAudio2 (see uwp_audio.cpp).
    if (!MIX_Init())
    {
        printf("Could not initialize SDL_mixer: %s\n", SDL_GetError());
        return 0;
    }

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, 48000 };
    mixer = MIX_CreateMixer(&spec);

    if (!mixer || !uwp_audio_start(generate_audio, NULL, &spec.freq, &spec.channels))
    {
        printf("Could not start XAudio2.\n");

        if (mixer)
        {
            MIX_DestroyMixer(mixer);
            mixer = NULL;
        }

        MIX_Quit();
        return 0;
    }
#else
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        printf("Could not initialize audio: %s\n", SDL_GetError());
        return 0;
    }

    if (!MIX_Init())
    {
        printf("Could not initialize SDL_mixer: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }

    mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);

    if (!mixer)
    {
        printf("Could not open an audio device: %s\n", SDL_GetError());
        MIX_Quit();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
#endif

    DEBUG_LOG("[audio] mixer ready\n");

    return 1;
}

void audio_shutdown(void)
{
    if (!mixer)
    {
        return;
    }

    audio_stop_all();

#ifdef PIRENEUS_UWP
    uwp_audio_stop();
#endif

    MIX_DestroyMixer(mixer);
    mixer = NULL;
    MIX_Quit();

#ifndef PIRENEUS_UWP
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
}

static void apply_voice_gain(AudioVoice* voice)
{
    MIX_SetTrackGain(voice->track, voice->volume * bus_volumes[voice->bus] * master_volume);
}

void audio_set_volumes(float master, float music, float sound)
{
    master_volume = master;
    bus_volumes[AUDIO_BUS_MUSIC] = music;
    bus_volumes[AUDIO_BUS_SOUND] = sound;

    for (int i = 0; i < MAX_VOICES; i++)
    {
        if (voices[i].used)
        {
            apply_voice_gain(&voices[i]);
        }
    }
}

AudioClip* audio_load(const char* path, int stream)
{
    if (!mixer)
    {
        return NULL;
    }

    MIX_Audio* audio = MIX_LoadAudio(mixer, path, !stream);

    if (!audio)
    {
        printf("Could not load audio '%s': %s\n", path, SDL_GetError());
        return NULL;
    }

    AudioClip* clip = SDL_malloc(sizeof(AudioClip));
    clip->audio = audio;

    return clip;
}

void audio_destroy_clip(AudioClip* clip)
{
    if (clip)
    {
        MIX_DestroyAudio(clip->audio);
        SDL_free(clip);
    }
}

float audio_clip_duration(AudioClip* clip)
{
    if (!clip)
    {
        return 0.0f;
    }

    Sint64 frames = MIX_GetAudioDuration(clip->audio);
    return frames > 0 ? (float)MIX_AudioFramesToMS(clip->audio, frames) / 1000.0f : 0.0f;
}

// Voices whose sound ended stay allocated until they're reused here.
static AudioVoice* find_free_voice(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
    {
        if (voices[i].used && !MIX_TrackPlaying(voices[i].track) && !MIX_TrackPaused(voices[i].track))
        {
            MIX_DestroyTrack(voices[i].track);
            SDL_zero(voices[i]);
        }

        if (!voices[i].used)
        {
            return &voices[i];
        }
    }

    return NULL;
}

AudioVoice* audio_play(AudioClip* clip, AudioBus bus, float volume, int loop)
{
    if (!mixer || !clip)
    {
        return NULL;
    }

    AudioVoice* voice = find_free_voice();

    if (!voice)
    {
        return NULL;
    }

    voice->track = MIX_CreateTrack(mixer);

    if (!voice->track)
    {
        return NULL;
    }

    voice->used = 1;
    voice->bus = bus;
    voice->volume = volume;

    MIX_SetTrackAudio(voice->track, clip->audio);
    apply_voice_gain(voice);

    SDL_PropertiesID options = SDL_CreateProperties();
    SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);
    MIX_PlayTrack(voice->track, options);
    SDL_DestroyProperties(options);

    return voice;
}

void audio_set_voice_position(AudioVoice* voice, float x, float y, float z)
{
    if (voice && voice->used)
    {
        MIX_Point3D point = { x, y, z };
        MIX_SetTrack3DPosition(voice->track, &point);
    }
}

void audio_stop(AudioVoice* voice)
{
    if (voice && voice->used)
    {
        MIX_StopTrack(voice->track, 0);
        MIX_DestroyTrack(voice->track);
        SDL_zerop(voice);
    }
}

void audio_stop_all(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
    {
        audio_stop(&voices[i]);
    }
}

int audio_is_playing(AudioVoice* voice)
{
    return voice && voice->used && MIX_TrackPlaying(voice->track);
}
