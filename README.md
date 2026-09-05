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

1. **Scan.** Finds nearby NPCs and reports which ones SkyrimNet has no bio for. What
   "nearby" means follows where you are standing: **indoors** it is the whole room you are
   in, however large, so a scan in the Blue Palace picks up everyone in it. **Outdoors** it
   crosses cell boundaries and sweeps the surrounding neighbourhood.

   Outdoors there is a limit worth knowing, and it is Skyrim's, not Bio Forge's: the game
   only keeps a small grid of cells loaded around you, so a scan can only ever see people
   inside it — roughly 17,000 units at the default `uGridsToLoad`. A big city is wider than
   that, so **no single scan covers a whole city**; the count also changes as you walk,
   because the loaded grid slides with you. To sweep a city, scan from two or three spots.
   The same report goes to `BioForge.log`:

   ```
   --- Bio Forge scan: 12 actor(s), 5 without a bio ---
     [GAP ] Phoenia              ref=680059A5 base=68000A40 Mara's Embrace.esp  race=BretonRace  dist=  312 bio=''
     [have] Uthgerd the Unbroken ref=00091918 base=0001A67F Skyrim.esm         race=NordRace    dist=  486 bio='uthgerd_the_unbroken_918'
   ```

   The scan clears itself once you leave the ground it describes, so the table never
   lists someone you have walked away from. An indoor scan clears when you leave the room;
   an outdoor sweep holds while you wander the settlement and only clears when you leave it,
   so walking across Whiterun to the people it found doesn't wipe the list.

2. **Generate**, one NPC at a time or the whole cell at once. Requests run a couple at
   a time so a busy inn doesn't turn into a burst of simultaneous LLM calls.

3. **Review.** Each result is staged, not written. Open one in the reader to see the
   bio as it would be committed, or flip to the raw model reply when a parse failed.
   Nothing touches your `prompts/characters/` folder until you press Commit.

4. **Refine.** When a batch finishes, any bio written while the people around it still
   had no profile gets its `relationships` section re-asked, now that those neighbours
   exist. Only that section, only for NPCs that actually gained a written neighbour, and
   a failure leaves the original untouched. Turn it off with `generate.refinePass`.

5. **Commit.** Writes the bio into `prompts/characters/`. SkyrimNet picks up the new file
   on its own, so the character speaks with their new personality without restarting the
   game. Any existing file is backed up first as `<name>.prompt.backup.<unixtime>`.

6. **Clear up.** **Discard** throws away a staged bio you don't want — it is the only
   copy, so that is the end of it. **Dismiss** takes a committed one off the review
   list and leaves the written bio exactly where it is. In bulk: **Commit all** writes
   every staged bio that is not committed yet, **Clear committed** drops the finished
   ones from the list, and **Discard all** empties it — that last one asks first, and
   tells you how many uncommitted bios are at stake. Nothing here can delete a bio you
   have committed.

A bio is written around where someone lives and works — read from their AI packages and
from where the game placed them — not from wherever the scan happened to catch them.
Someone found mid-errand in a tavern is written as who they are rather than as a regular,
and where the record genuinely says nothing, the prompt is told that too instead of being
left to guess from the room.

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

A settlement here means a city, town or village — or a standalone inn or farmstead out
on the road, which gets its own sheet rather than borrowing the hold's. Nightgate Inn is
about the people at Nightgate Inn, not about Dawnstar.

Run a batch in a town with no digest yet and Bio Forge builds one first, holding the
batch until it lands — the panel says so while it waits. Generating a single NPC never
spends that extra call; it uses a digest if one is already there. There is a **Build
digest** button for doing it deliberately, **View digest** to read the sheet you are about
to generate against, and **Rebuild** to replace one.

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

A committed bio goes live straight away — SkyrimNet loads a character template on demand
and watches it for changes, so there is no cache to rebuild and no restart to sit through.

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
| Exterior scan radius, game units | `scan.exteriorRadius` | `20000` |
| Unique NPCs only | `scan.uniqueOnly` | `true` |
| Include dead | `scan.includeDead` | `false` |
| Concurrent generations | `generate.maxConcurrent` | `4` |
| Refine ties after a batch | `generate.refinePass` | `true` |
| Use regional digests | `digest.enabled` | `true` |
| Build digest before a batch | `digest.autoBuild` | `true` |
| Bios harvested per digest | `digest.maxCandidates` | `120` |

Change a setting in SkyrimNet's panel and Bio Forge picks it up within a second — no
restart, nothing to press. A change that actually moves a value is written to
`BioForge.log` as `config changed:`. A batch already running keeps the settings it
started with.

`scan.uniqueOnly` defaults on because generic leveled actors share base records and
are correctly served by SkyrimNet's generic fallback. It also affects reporting: a
leveled actor's editor base is a template shell whose race and sex fields are
placeholders, so the race column is only trustworthy for unique NPCs.

The scan's **Template** column is SkyrimNet's assigned name for that character, which
exists whether or not the bio file does — so a row can read "missing" and still show a
name. Where SkyrimNet has assigned nothing, the column shows the name Bio Forge would
derive on commit instead, dimmed and marked with `*`. Either way the column tells you
what committing that row would write.

`generate.maxConcurrent` is the one to lower if your provider rate-limits you, and to
raise if you are running a local model.

## Known issues

**One scan can't cover a whole city.** Skyrim only keeps a small grid of cells loaded around
you — 5×5 at the default `uGridsToLoad` — and unloads everything else. Actors outside that
grid aren't in memory at all, so no scan can see them, whatever the radius is set to. The
practical ceiling is roughly 17,000 units, and a large city is wider than that: Solitude's
streets span seven cells against a five-cell window.

Two visible consequences, both normal:

- **The count changes as you walk.** The loaded grid slides with you, so each scan sees a
  different set. (If the list goes *empty* instead, that's different — that's the scan being
  dropped because you left the area.)
- **Raising the scan radius past ~20,000 does nothing.** It isn't the limit; the loaded grid
  is.

So a city is covered by scanning from **two or three positions**, not one. Each scan currently
replaces the previous one, so generate what you find before moving on. Note this also means
NPCs on opposite sides of a city are never in the same batch, and so won't be written knowing
about each other — the regional digest is what carries city-wide context instead.

A planned improvement is a sweep mode that accumulates results across several scan positions
into one list, so a whole city can be gathered and generated as a single batch.

**A wrong bio already in your corpus is trusted, and it spreads.** Bio Forge treats any NPC
SkyrimNet has a bio for as finished — it won't offer to regenerate them — and it reads those
same bios to tell *other* NPCs who their neighbours are. So a bad entry is both invisible and
contagious.

A real example from a 3,200-bio install: Riften's Dunmer market trader **Brand-Shei** is served
by a template named `brandish_DDC`, whose summary reads *"Brandish is a hostile Breton
destruction mage operating with the bandit group occupying Fort Neugrad."* No NPC called
Brandish exists in that load order at all — the name and the character were both invented,
most likely by whatever bulk-generated the corpus misreading "Brand-Shei" as the English word
"brandish". Every bio generated near Riften's market then inherited that false description in
its neighbour list.

Bio Forge didn't create the entry and can't currently tell it's wrong: it asks SkyrimNet which
template serves a reference and trusts the answer. So if a generated bio describes a neighbour
oddly, check that neighbour's own `.prompt` before blaming the new bio. Deleting the bad file
and regenerating fixes both it and everyone downstream.

A planned mitigation is to check that a bio actually names the character it claims to describe,
and to withhold it from other NPCs' neighbour lists when it doesn't.

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
