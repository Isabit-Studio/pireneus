# UWP (Universal Windows Platform)

SDL3 removed its WinRT backend, so the engine has its own UWP backend, in C++/WinRT, in
`src/platforms/uwp/`:

- `uwp_platform.cpp`: `CoreApplication` and `CoreWindow`, window, keyboard, mouse, paths and dialogs;
- `uwp_audio.cpp`: audio output through XAudio2, fed by SDL_mixer.

SDL is still used for what doesn't depend on a platform layer (surfaces, files, audio conversion). It
is compiled with its desktop subsystems turned off. Rendering is DirectX 12 only
(`CreateSwapChainForCoreWindow`).

## Building

In a shell with MSVC loaded (`vcvars64.bat`), in a separate build folder:

```bash
cmake -S . -B build-uwp -G Ninja -DCMAKE_BUILD_TYPE=Release -DPIRENEUS_UWP=ON
```

```bash
cmake --build build-uwp
```

The executable is linked with `/APPCONTAINER /ENTRY:wWinMainCRTStartup`, against a static CRT, in
C++20.

The package layout is written to `build-uwp/uwp_layout`. It holds:

- the executable;
- `AppxManifest.xml`, generated from `cmake/uwp/AppxManifest.xml.in`;
- the logos;
- the shaders and resources.

## Installing and running

Turn on Windows' "Developer Mode", then run from PowerShell:

```bash
Add-AppxPackage -Register build-uwp\uwp_layout\AppxManifest.xml
```

The "Pireneus" app then appears in the Start menu. To uninstall it:

```bash
Get-AppxPackage Pireneus.Examples | Remove-AppxPackage
```

To distribute the package:

1. Build it with `makeappx pack`.
2. Sign it. The `PIRENEUS_UWP_PUBLISHER` CMake option must be the certificate's subject.

## Specifics

- **Log**: standard output goes to `log.txt`, in the app's `LocalState` folder
  (`%LOCALAPPDATA%\Packages\Pireneus.Examples_<id>\LocalState\`).
- **Projects**: there is no folder picker, so projects are created in `LocalState\Projects`.
- **Deleting**: deleting an asset is permanent (there is no recycle bin).
- **Key labels**: `input_key_label` returns US keyboard labels.
- **Mouse**: mouse input comes from the `CoreWindow`'s `Pointer*` events.

## Known limitations

The binary still imports desktop APIs (`USER32`, `WINMM`, `IMM32`...) through SDL's code. It runs in
the sandbox, but it hasn't been through Microsoft Store certification (the Windows App Certification
Kit), which will probably fail on that point.
