# Pireneus

Pireneus is **Isabit Studio**'s 2D and 3D game engine, written in
[Orthodox C++](https://bkaradzic.github.io/posts/orthodoxc++/): C for the engine, simple C++ for the
tools.

These pages use the **GitHub Wiki** format. You can read them in the repository (`docs/`), or publish
them as they are to the project's wiki (see [Publishing this wiki](#publishing-this-wiki)).

## Where to start

- [Building and running](Getting-Started): requirements, Win32 and UWP builds, command line options.
- [Architecture](Architecture): repository layout, engine modules, conventions.
- [Examples](Examples): what each example of the menu shows.
- [Demo scenes](Demo-Scenes): real content to test the engine on, such as the Bistro.

## The engine

- [Rendering](Rendering): the RHI, 2D rendering and pixel perfect, the bindless GPU layer.
- [3D rendering](3D-Rendering): Forward+ and Deferred+, and everything they share.
- [Baked lighting](Baked-Lighting): lightmaps, probe volume, occlusion culling, reflection probes.
- [Shaders (Slang)](Shaders)
- [Platforms and input](Platforms-and-Input): SDL3, window, scancode-based keyboard, paths.
- [UWP](UWP): the Universal Windows Platform backend.
- [Projects, assets and scenes](Projects-Assets-and-Scenes)
- [Physics and play mode](Physics-and-Play-Mode)
- [Audio](Audio)
- [Dear ImGui](Dear-ImGui): the UI integration.

## The editor

- [Editor](Editor): layout, camera, selection, gizmos, undo, play mode.
- [Level editing](Level-Editing): Hammer-style (Source 2) whiteboxing and greyboxing.

## Contributing

- [Code style](Code-Style)
- Licenses:
  - the engine's source code is released under the MIT license (`LICENSE`);
  - the Isabit Studio name and logos are not: they are the intellectual property of Isabit Studio,
    all rights reserved;
  - third-party libraries are listed in `third-parties.md`, at the root of the repository.

## Publishing this wiki

A GitHub wiki is a separate Git repository (`<repository>.wiki.git`).

1. Copy the contents of `docs/` to the root of that wiki repository.
2. Push it.

`Home.md` becomes the home page, `_Sidebar.md` the sidebar and `_Footer.md` the footer.

Links between pages use the page name without an extension (`[Architecture](Architecture)`), as the
wiki expects. As a result, they only resolve once the pages are published to the wiki.
