# Bio Forge

Writes SkyrimNet character bios for mod-added NPCs that don't have one, using the
characterisation their own mod already authored — their dialogue tree, factions,
outfit, class and location.

SkyrimNet drives NPC roleplay from per-character `.prompt` bios. Any NPC without one
falls back to a generic template, so a carefully written mod character speaks with
none of their own personality. A typical heavily-modded load order has thousands of
uncovered NPCs, and no prebuilt bio pack can anticipate an arbitrary mod list — so
Bio Forge fills the gaps in *your* load order, on your machine.

**Bio Forge never handles an API key.** Generation runs through SkyrimNet's own
`SendCustomPromptToLLM`, so it uses whatever LLM you already have configured. You
choose the model for the `bioforge_generate` variant in SkyrimNet's settings.

## Status

**Phase 1 — scan only.** The plugin loads, binds SkyrimNet's public API, and reports
which nearby NPCs lack a bio. It does not generate or write anything yet.

Open SKSE Menu Framework's mod control panel (default **`x`**, set in
`SKSEMenuFramework.ini`), go to **Bio Forge / Scan**, and press *Scan for missing
bios*. Results render as a table, and the same report goes to `BioForge.log`:

```
--- Bio Forge scan: 12 actor(s), 5 without a bio ---
  [GAP ] Phoenia              ref=680059A5 base=68000A40 Mara's Embrace.esp  race=BretonRace  dist=  312 bio=''
  [have] Uthgerd the Unbroken ref=00091918 base=0001A67F Skyrim.esm         race=NordRace    dist=  486 bio='uthgerd_the_unbroken_918'
```

## Requirements

- [SkyrimNet](https://goncalo22.github.io/SkyrimNet-GamePlugin/) with public API **v8+**
  (v8 introduced `SendCustomPromptToLLM`; the scan itself needs only v3)
- SKSE, Address Library for SKSE Plugins
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) v3 —
  the entire UI. Bio Forge deliberately registers **no hotkey of its own**: SMF already
  has one global toggle, and a per-mod binding would be one more thing to collide with.
  (SkyrimNet's `Hotkey.yaml` binds F10 to `toggleContinuousMode`, and it numbers keys by
  Virtual-Key code while CommonLibSSE reports DirectInput scan codes — so two hotkey
  settings sitting side by side would not even agree on what "F10" means.)

## Install

Install as a normal mod (MO2 / Vortex). The archive is already Data-shaped:

```
SKSE/Plugins/BioForge.dll
SKSE/Plugins/SkyrimNet/config/plugins/BioForge/manifest.yaml
```

## Configuration

Settings live in the manifest and appear in SkyrimNet's own in-game settings UI
under **BioForge**. The DLL reads them through `PublicGetPluginConfigValue`, falling
back to compiled-in defaults for anything missing — so a missing or malformed
manifest degrades to defaults rather than failing.

| Setting | Path | Default |
|---|---|---|
| Scan radius, game units | `scan.radius` | `3000` |
| Current cell only | `scan.cellOnly` | `true` |
| Unique NPCs only | `scan.uniqueOnly` | `true` |
| Include dead | `scan.includeDead` | `false` |

`scan.uniqueOnly` defaults on because generic leveled actors share base records and
are correctly served by SkyrimNet's generic fallback. It also affects reporting: a
leveled actor's editor base is a template shell whose race and sex fields are
placeholders, so the race column is only trustworthy for unique NPCs.

## Building

Needs CMake, a C++23 MSVC toolchain, and vcpkg.

```sh
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The installable mod folder is staged at `build/BioForge/`.

To build against a CommonLibSSE-NG checkout you already have rather than cloning
another 500 MB, pass `-DBIOFORGE_COMMONLIB_DIR=<path>`.

The triplet is pinned to `x64-windows-static-md` in `CMakeLists.txt`, and both
halves matter: static libs keep `fmt.dll` / `spdlog.dll` out of `SKSE/Plugins`
where they could collide with another plugin's copy, while the dynamic CRT is
*required* by SkyrimNet's public API, which passes `std::string` and
`std::function` across the DLL boundary.

## Layout

```
CMakeLists.txt
dll-source/include/SkyrimNet_PublicAPI.h   vendored from SkyrimNet, unmodified
dll-source/src/main.cpp                    SKSE entry, API binding, version gate
dll-source/src/UI.{h,cpp}                  SKSE Menu Framework page
dll-source/src/SkyrimNetAPI.{h,cpp}        wrapper - the only TU including the vendor header
dll-source/src/Config.{h,cpp}              settings via SkyrimNet's config store
dll-source/src/ScopeSelector.{h,cpp}       actor enumeration + gap detection
mod-root/                                  files shipped verbatim into the mod
```

Two vendored headers, both resolving their DLLs at runtime via `GetProcAddress`, so
neither SkyrimNet nor SMF is a load-time dependency — a missing one logs and degrades
instead of failing to load:

- `SkyrimNet_PublicAPI.h` *defines* its function pointers at namespace scope, so
  including it in more than one translation unit is a link error. `SkyrimNetAPI.cpp`
  is the single place it appears; everything else goes through `SkyrimNetAPI.h`.
- `SKSEMenuFramework.h` (from [SKSE-Menu-Framework-3-Example](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example))
  is ~519 KB of header-only ImGui bindings, included only by `UI.cpp`. It calls
  `std::filesystem` without including it, so `pch.h` must come first.
