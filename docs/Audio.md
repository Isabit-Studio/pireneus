# Audio

Code: `src/audio/audio.{h,c}`, on [SDL_mixer 3](https://github.com/libsdl-org/SDL_mixer) with its
built-in decoders only, so no external library is needed:

- WAV and AIFF;
- MP3 and FLAC (dr_libs);
- Ogg Vorbis (stb_vorbis).

```c
audio_init();                                        // 0 with no device : every other call is then a no-op
audio_set_volumes(master, music, sound);

AudioClip* clip = audio_load(path, stream);          // stream : decoded on the fly (music)
AudioVoice* voice = audio_play(clip, bus, volume, loop);
audio_set_voice_position(voice, x, y, z);            // relative to the listener : 3D sound
audio_stop(voice);

audio_shutdown();
```

- **Buses**: **Music** and **Sound**, under a master volume, set from the project's volumes.
- **3D voices** are positioned relative to the listener (the in-game camera).
- **In a scene**, an audio source is an object component. It has a file, a volume, looping, autoplay,
  the music or sound bus, and 3D spatialization.

On UWP there is no SDL audio device. `MIX_CreateMixer` and `MIX_Generate` produce the samples, and
`uwp_audio.cpp` sends them to XAudio2.
