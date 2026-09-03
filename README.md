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
`SendCustomPromptToLLM`, so it uses whatever LLM you already have configured — and it
dispatches on SkyrimNet's existing `CharacterProfileGeneration` variant, so there is no
second model to set up.

## How it works

Open SKSE Menu Framework's mod control panel (default **`x`**, set in
`SKSEMenuFramework.ini`) and go to **Bio Forge / Scan**.

1. **Scan.** Finds nearby NPCs and reports which ones SkyrimNet has no bio for. The
   same report goes to `BioForge.log`:

   ```
   --- Bio Forge scan: 12 actor(s), 5 without a bio ---
     [GAP ] Phoenia              ref=680059A5 base=68000A40 Mara's Embrace.esp  race=BretonRace  dist=  312 bio=''
     [have] Uthgerd the Unbroken ref=00091918 base=0001A67F Skyrim.esm         race=NordRace    dist=  486 bio='uthgerd_the_unbroken_918'
   ```

2. **Generate**, one NPC at a time or the whole cell at once. Requests run a couple at
   a time so a busy inn doesn't turn into a burst of simultaneous LLM calls.

3. **Review.** Each result is staged, not written. Open one in the reader to see the
   bio as it would be committed, or flip to the raw model reply when a parse failed.
   Nothing touches your `prompts/characters/` folder until you press Commit.

4. **Refine.** When a batch finishes, any bio written while the people around it still
   had no profile gets its `relationships` section re-asked, now that those neighbours
   exist. Only that section, only for NPCs that actually gained a written neighbour, and
   a failure leaves the original untouched. Turn it off with `generate.refinePass`.

5. **Commit.** Writes the bio and asks SkyrimNet to reload its prompt cache, so the
   character speaks with their new personality without restarting the game. Any
   existing file is backed up first as `<name>.prompt.backup.<unixtime>`.

Staged work lives in `SKSE/Plugins/SkyrimNet/prompts/bioforge_staging/<name>/` and
holds three files — `harvest.json` (what the model was asked), `response.raw.txt`
(what it answered) and `bio.prompt` (what would be committed). It is scratch: it is
cleared when the game loads, and committed bios are never touched by that.

## The regional digest

The evidence that would connect an NPC to the wider world — proper nouns in their
dialogue, shared factions, world knowledge — is empty for most mod-added NPCs. Torg
Strong-Arm's entire dialogue is "Need something?" and his only faction is the hold, so
without help every `relationships` block comes out as "the customers" and "the locals".

Deciding *which* locals a person would know is a judgement about those locals, not a
lookup. So Bio Forge writes a short reference sheet for the settlement — who holds
power, who owns the trade, who takes a cut — and hands it to every bio generated there.
It is written **once per settlement** and cached on disk, including across sessions, so
it costs one extra generation per town rather than one per NPC.

The candidates it chooses from are **your own installed bios**, so the sheet reflects
your actual load order, mod-added characters included. Bios naming the settlement come
first; if too few do, it tops up from the surrounding hold.

Run a batch in a town with no digest yet and Bio Forge builds one first, holding the
batch until it lands — the panel says so while it waits. Generating a single NPC never
spends that extra call; it uses a digest if one is already there. There is a **Build
digest** button for doing it deliberately, and **Rebuild** to replace one.

Sheets are cached at `SKSE/Plugins/BioForge/regions/<Settlement>.txt`. They are plain
text — read them, and delete one if you don't like it.

## Requirements

- [SkyrimNet](https://goncalo22.github.io/SkyrimNet-GamePlugin/) with public API **v8+**
  (v8 introduced `SendCustomPromptToLLM`; the scan itself needs only v3, and world
  knowledge in the harvest needs v9)
- SKSE, Address Library for SKSE Plugins
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) v3 —
  the entire UI. Bio Forge deliberately registers **no hotkey of its own**: SMF already
  has one global toggle, and a per-mod binding would be one more thing to collide with.
  (SkyrimNet's `Hotkey.yaml` binds F10 to `toggleContinuousMode`, and it numbers keys by
  Virtual-Key code while CommonLibSSE reports DirectInput scan codes — so two hotkey
  settings sitting side by side would not even agree on what "F10" means.)

SkyrimNet's loopback web server is used to reload the prompt cache after a commit. If
it is off, the commit still succeeds and says so — the bio just needs a restart to
become live.

## Install

Install as a normal mod (MO2 / Vortex). The archive is already Data-shaped:

```
SKSE/Plugins/BioForge.dll
SKSE/Plugins/SkyrimNet/config/plugins/BioForge/manifest.yaml
SKSE/Plugins/SkyrimNet/prompts/bioforge_generate.prompt
SKSE/Plugins/SkyrimNet/prompts/bioforge_region_digest.prompt
SKSE/Plugins/SkyrimNet/prompts/bioforge_refine_ties.prompt
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
| Concurrent generations | `generate.maxConcurrent` | `2` |
| Refine ties after a batch | `generate.refinePass` | `true` |
| Use regional digests | `digest.enabled` | `true` |
| Build digest before a batch | `digest.autoBuild` | `true` |
| Bios harvested per digest | `digest.maxCandidates` | `120` |

`scan.uniqueOnly` defaults on because generic leveled actors share base records and
are correctly served by SkyrimNet's generic fallback. It also affects reporting: a
leveled actor's editor base is a template shell whose race and sex fields are
placeholders, so the race column is only trustworthy for unique NPCs.

`generate.maxConcurrent` is the one to lower if your provider rate-limits you, and to
raise if you are running a local model.

## Building

Needs CMake, a C++23 MSVC toolchain, and vcpkg.

```sh
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The installable mod folder is staged at `build/BioForge/` — the DLL plus everything
under `mod-root/`, so it is complete and can be pointed at directly by MO2.

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
dll-source/src/Generator.{h,cpp}           context assembly, queue, dispatch
dll-source/src/RegionDigest.{h,cpp}        per-settlement reference sheet
dll-source/src/StagingStore.{h,cpp}        response parsing, staging bundle, commit
dll-source/src/PromptReload.{h,cpp}        loopback prompt-cache reload
dll-source/src/Json.h                      context escaping; there is no JSON library
mod-root/                                  files shipped verbatim into the mod
tools/render_probe.py                      prompt iteration against the live game
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

The two `.prompt` files under `mod-root/` carry the entire data harvest on purpose, so
prompts can be tuned without rebuilding the DLL. Their headers document the renderer
quirks that govern any edit to them — read those before changing a template.
