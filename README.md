# Ignition RTX Remix

An [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix) compatibility mod for **Ignition**
(Unique Development Studios, 1997), 3dfx version (`Ign_3dfx.exe`).

Ignition transforms and rasterizes everything itself and hands Glide finished screen-space
triangles, so Remix normally has no 3D scene to path trace. This mod is a `d3d9.dll` proxy that
hooks the game's renderer while geometry is still in object space. It submits the meshes to
Remix with real world, view and projection transforms and keeps the game's 2D HUD on top.

```
Ign_3dfx.exe -> glide2x.dll (nGlide) -> d3d9.dll (this mod) -> d3d9_remix.dll (RTX Remix) -> .trex\
```

## Features

- Object-space geometry injection with the game's textures, palettes and animated texture frames
- Sprites and coloured (untextured) triangles
- Smoothed vertex normals (Ignition has none) with a configurable crease angle
- Wheels lifted out of the road surface, which the original depth-bucket renderer hid
- Render rate unlocked from the 36 Hz logic tick (game speed unchanged)
- HUD and menus kept, world raster suppressed
- F4 debug overlay (ImGui)

## Requirements

- Ignition, 3dfx version (you must own the game; no game files are included)
- [nGlide](https://www.zeus-software.com/downloads/nglide) Glide wrapper
- [RTX Remix runtime](https://github.com/NVIDIAGameWorks/rtx-remix/releases) and an RTX GPU

## Installing

1. Download the latest release zip from the **Releases** page.
2. Install nGlide and make sure the game runs through it.
3. Copy the RTX Remix runtime into the game folder, then rename Remix's `d3d9.dll` to `d3d9_remix.dll`.
4. Copy `d3d9.dll`, `remix-comp-proxy.ini` and the `.trex` folder from the release zip into the game folder.
5. Start `Ign_3dfx.exe`. Settings are documented inside `remix-comp-proxy.ini`.

## Building

Requires Visual Studio 2022 with the C++ x86 toolset and a Windows 10/11 SDK.

```bat
build.bat
```

The output is `build\bin\release\d3d9.dll` plus `remix-comp-proxy.ini`. Use `build.bat debug` for a debug build.

## Project layout

```
src/comp/modules/ignition_inject.*   Ignition renderer hooks and Remix submission
src/comp/game/                       Ignition addresses and structures
src/comp/, src/shared/               remix-comp-proxy framework
deps/                                Vendored dependencies
assets/remix-comp-proxy.ini          Default configuration
```

## Credits

Built on the remix-comp-proxy framework by [xoxor4d](https://github.com/xoxor4d) and
[kim2091](https://github.com/kim2091), from the
[Vibe Reverse Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) toolkit.
Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT. See [LICENSE](LICENSE).
