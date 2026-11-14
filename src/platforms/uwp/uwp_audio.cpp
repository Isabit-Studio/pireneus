// Audio output on UWP : SDL's WASAPI driver needs the desktop's device enumerator, which an AppContainer
// doesn't have. XAudio2 is what UWP apps get, fed from SDL_mixer by a small thread.

#include <xaudio2.h>

#include <SDL3/SDL.h>

#include "platforms/uwp/uwp_audio.h"

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BLOCK_FRAMES 1024
#define BLOCK_COUNT 3

static IXAudio2* engine;
static IXAudio2MasteringVoice* master;
static IXAudio2SourceVoice* source;
static UwpAudioGenerate generate_callback;
static void* generate_user_data;
static SDL_Thread* thread;
static SDL_AtomicInt running;
static float blocks[BLOCK_COUNT][BLOCK_FRAMES * CHANNELS];

static int audio_thread(void*)
{
    int next = 0;

    while (SDL_GetAtomicInt(&running))
    {
        XAUDIO2_VOICE_STATE state;
        source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);

        if (state.BuffersQueued >= BLOCK_COUNT)
        {
            SDL_Delay(2);
            continue;
        }

        generate_callback(generate_user_data, blocks[next], (int)sizeof(blocks[next]));

        XAUDIO2_BUFFER buffer = {};
        buffer.AudioBytes = sizeof(blocks[next]);
        buffer.pAudioData = (const BYTE*)blocks[next];
        source->SubmitSourceBuffer(&buffer);

        next = (next + 1) % BLOCK_COUNT;
    }

    return 0;
}

extern "C" int uwp_audio_start(UwpAudioGenerate generate, void* user_data, int* out_rate, int* out_channels)
{
    if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        return 0;
    }

    if (FAILED(engine->CreateMasteringVoice(&master)))
    {
        engine->Release();
        engine = NULL;
        return 0;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = CHANNELS;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = 32;
    format.nBlockAlign = CHANNELS * sizeof(float);
    format.nAvgBytesPerSec = SAMPLE_RATE * format.nBlockAlign;

    if (FAILED(engine->CreateSourceVoice(&source, &format)))
    {
        master->DestroyVoice();
        engine->Release();
        engine = NULL;
        return 0;
    }

    generate_callback = generate;
    generate_user_data = user_data;
    *out_rate = SAMPLE_RATE;
    *out_channels = CHANNELS;

    source->Start();

    SDL_SetAtomicInt(&running, 1);
    thread = SDL_CreateThread(audio_thread, "audio", NULL);

    return 1;
}

extern "C" void uwp_audio_stop(void)
{
    if (!engine)
    {
        return;
    }

    SDL_SetAtomicInt(&running, 0);
    SDL_WaitThread(thread, NULL);

    source->DestroyVoice();
    master->DestroyVoice();
    engine->Release();
    engine = NULL;
}
