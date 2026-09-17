# Ignition RTX Remix
<img width="1920" height="1200" alt="trex_Screenshot_2026 07 28_-_11 55 40 81" src="https://github.com/user-attachments/assets/0cdbdf90-d828-45de-a9fe-5c2c82a40b37" />
<img width="1920" height="1200" alt="trex_Screenshot_2026 07 28_-_11 56 51 28" src="https://github.com/user-attachments/assets/fd6366f0-4645-402b-bb8f-41c47a961069" />
<img width="1920" height="1200" alt="trex_Screenshot_2026 07 28_-_11 58 21 09" src="https://github.com/user-attachments/assets/1feb3fc0-072e-4163-9a6c-38f60a1ab799" />
<img width="1920" height="1200" alt="trex_Screenshot_2026 07 28_-_12 01 08 80" src="https://github.com/user-attachments/assets/3da82abb-a8ca-4038-8797-4b5569a0f497" />

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

- Ignition (tested with the GOG version; you must own the game, no game files are included)
- [RTX Remix runtime](https://github.com/NVIDIAGameWorks/rtx-remix/releases). I recommend
  [Remix Plus](https://github.com/RemixProjGroup/dxvk-remix/releases) for the Numos sky implementation
- [nGlide](https://www.zeus-software.com/downloads/nglide) 3dfx Glide wrapper (not included, install it from the official site)
- An RTX GPU

## Installing

1. Install the RTX Remix runtime to the game folder.
2. Rename the RTX Remix `d3d9.dll` to `d3d9_remix.dll`.
3. Extract the 3dfx patch to the game folder: [ign_3dfx2.zip](http://web.archive.org/web/19981203075024/http://www.uds.se:80/ignition/ign_3dfx2.zip)
4. Install nGlide: download it from the [official site](https://www.zeus-software.com/downloads/nglide), run the nGlide setup and click Install.
5. Extract the mod archive from the **Releases** page to the Ignition folder.
6. Go to the `BALTAZAR\DATA` folder.
7. Make backup copies of `DEFAULT2.PSQ` and `TEST2.PFM` and rename the files `DEFAULT.PSQ` and `TEST.PFM` respectively.
8. Set graphics options via the nGlide Configurator (Start menu > nGlide > nGlide Configurator).
9. Set options for increased refresh rate (default 2x speed) or smoothed normals (default on) in `remix-comp-proxy.ini`.
10. Start the game via `Ign_3dfx.exe`.

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
