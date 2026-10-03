# 📜 BioForge

Writes SkyrimNet character bios for mod-added NPCs that don't have one, built from what
their own mod already gives them: dialogue, factions, outfit, class and location.

Without a bio, an NPC falls back to SkyrimNet's generic template and speaks with no
personality of their own. Heavily modded load orders have thousands of these, and no
prebuilt bio pack can cover *your* mod list. BioForge fills the gaps on your machine.

- 🔑 **No API key.** It uses the LLM you already set up in SkyrimNet, through SkyrimNet's
  own `CharacterProfileGeneration` profile, so there's no second model to configure
  (unless you want one - see `llm.useOwnVariant` below).
- 👀 **Nothing is written without your say-so.** Every bio is staged for review first.
- ⚡ **Live immediately.** A committed bio takes effect without a restart.

## 🤔 Why not SkyrimNet's own bio generator?

SkyrimNet's dashboard can already generate a bio, one character at a time: you pick an
NPC, generate, save. That's fine for a follower or two. It doesn't scale to a load order
with thousands of uncovered NPCs. **BioForge is the bulk tool.**

- 🗺️ **It finds the gaps for you.** Stand somewhere and scan. You don't have to know
  which NPCs are missing a bio or look them up one by one.
- 📦 **Whole places at once.** Generate an inn, a street or a village in one press, with
  several requests running at a time.
- 🤝 **Bios that know each other.** A batch is written together: each NPC sees who else
  lives there, relationships are re-checked once the whole batch exists, and a
  per-settlement digest says who runs the town. One-at-a-time bios can't know about
  neighbours that don't have bios yet.
- 📋 **One review queue.** Read, commit or discard a whole batch from one list.
- 🧩 **It works alongside SkyrimNet.** It uses the same LLM setup and saves into the same
  layer as your dashboard edits, and it leaves NPCs with a dynamic bio alone.

## 🧭 How it works

Open SKSE Menu Framework's panel (SMF's own toggle key, `ToggleKey` in
`SKSEMenuFramework.ini`) and go to **BioForge / Scan**.

1. 🔍 **Scan** — lists nearby NPCs and flags the ones with no bio.
   - **Indoors:** the whole interior you're in.
   - **Outdoors:** the surrounding area, across cell borders.
2. ✍️ **Generate** — one NPC, or everyone the scan found. Requests run a few at a time.
3. 📖 **Review** — read each bio before it goes anywhere, or view the raw model reply if
   parsing failed.
4. 🔗 **Refine** — after a batch, relationships are re-checked now that everyone's
   neighbours exist too. Only that section changes. Turn it off with `generate.refinePass`.
5. ✅ **Commit** — saves the bio into your SkyrimNet overlay (the layer that holds your
   own dashboard edits) through SkyrimNet's content API.
6. 🧹 **Clear up**
   - **Discard** deletes a staged bio (it's the only copy).
   - **Dismiss** hides a committed one from the list; the bio itself stays.
   - **Commit all**, **Clear committed** and **Discard all** do the same in bulk.
     **Discard all** asks first.
   - Nothing in the panel can delete a bio you've committed.

**Good to know**

- 🌀 A row marked **dynamic** is not a gap. SkyrimNet is already evolving a bio for that
  NPC in this playthrough, and a new one would lose to it anyway.
- 🏠 Bios are written around where someone *lives and works* (read from their AI
  packages), not wherever the scan caught them.
- 🗂️ Staged bios live in `SKSE/Plugins/BioForge/staging/` and are cleared when the game
  loads. Committed bios are never touched by that.

## 🏘️ Regional digest

Most mod NPCs have almost nothing tying them to the wider world, so their relationships
come out as "the customers" and "the locals". To fix that, BioForge writes a short
reference sheet per settlement (who holds power, who runs the trade) and hands it to
every bio generated there.

- 🏰 **One per settlement**, whether that's a city, a village or a lone roadside inn.
- 💾 **Cached on disk** at `SKSE/Plugins/BioForge/regions/<Settlement>.txt`: plain text
  you can read, edit or delete.
- 📚 **Built from your own installed bios**, so it reflects your actual load order.
- 💰 **One extra LLM call per town**, and only when you generate a batch. A single
  **Generate** never builds one, but uses one if it's already there.
- 🛠️ Panel buttons: **Build digest**, **View digest**, **Rebuild**.

## 📦 Requirements

- [SkyrimNet](https://goncalo22.github.io/SkyrimNet-GamePlugin/) **Beta 25+** (API v10+)
- SKSE and Address Library for SKSE Plugins
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) v3,
  which provides the whole UI (BioForge adds no hotkey of its own)

> ⬆️ **Upgrading from Beta 24?** Old committed bios in
> `SKSE/Plugins/SkyrimNet/prompts/characters/` are no longer read. Use SkyrimNet's
> **Plugins → Import Old Content** to bring them across.

## 💿 Install

Install like any mod (MO2 / Vortex). It contains the DLL plus a SkyrimNet content layer
at `SKSE/Plugins/SkyrimNet/external/oldcustard.bioforge/`, which shows up on SkyrimNet's
**Installed Plugins** page with an **External** badge.

## ⚙️ Settings

Found under **BioForge** in SkyrimNet's in-game settings. Changes apply within a second,
with no restart.

| Setting | Path | Default |
|---|---|---|
| Exterior scan radius (units) | `scan.exteriorRadius` | `20000` |
| Unique NPCs only | `scan.uniqueOnly` | `true` |
| Include dead | `scan.includeDead` | `false` |
| Concurrent generations | `generate.maxConcurrent` | `4` |
| Refine ties after a batch | `generate.refinePass` | `true` |
| Use BioForge's own LLM | `llm.useOwnVariant` | `false` |
| Use regional digests | `digest.enabled` | `true` |
| Build digest before a batch | `digest.autoBuild` | `true` |
| Bios harvested per digest | `digest.maxCandidates` | `120` |

- 🐢 Lower `generate.maxConcurrent` if your provider rate-limits you. Raise it for a local
  model.
- 🧠 **A separate model for BioForge:** set up the `bioforge` model in SkyrimNet's LLM
  settings, *then* turn on `llm.useOwnVariant`. Raise its max tokens to about 10,000 first:
  until configured it inherits the dialogue defaults (4,096 tokens), which cut bios short.
  Off, BioForge uses your Character Profile Generation model.
- 👤 `scan.uniqueOnly` skips generic leveled NPCs (bandits, guards), which SkyrimNet's
  generic template already handles well. Creatures (horses, dogs, anything whose race
  isn't a person's) are never scanned.
- 🏷️ The **Template** column shows the file name a commit would write. A dimmed name
  with `*` is one BioForge derived because SkyrimNet hasn't assigned one.

## ⚠️ Known issues

**One scan can't cover a whole city.** Skyrim only keeps a small grid of cells loaded
(about 17,000 units across), and a scan can't see anyone outside it.

- 📍 Scan big cities from **two or three spots**, and generate before moving on (each scan
  replaces the last).
- 🚶 The count changes as you walk. That's normal. An *empty* list means you left the
  area.
- 📏 Raising the radius past ~20,000 does nothing.
- 🔜 Planned: a sweep mode that gathers several scans into one batch.

**A wrong bio already in your load order gets trusted and spreads.** BioForge treats any
existing bio as correct, and uses it to tell neighbours who lives nearby.

- 🧪 Real case: Riften's trader **Brand-Shei** was served by a bulk-generated bio for an
  invented bandit mage named "Brandish", and every nearby bio inherited the error.
- 🩹 If a new bio describes a neighbour oddly, check that neighbour's own `.prompt`.
  Deleting the bad file and regenerating fixes it everywhere.
- 🔜 Planned: a check that a bio actually names the character it claims to describe.

## 🛠️ Building

Needs CMake, a C++23 MSVC toolchain and vcpkg.

```sh
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

- 📁 Output is a complete mod folder at `build/BioForge/` that you can point MO2 at.
- ♻️ Already have CommonLibSSE-NG checked out? Pass `-DBIOFORGE_COMMONLIB_DIR=<path>`.
- ✏️ Prompts live in `mod-root/.../external/oldcustard.bioforge/prompts/` and can be
  tuned without a rebuild. Read their header comments first, and bump the layer's
  `manifest.json` version when they change.

```
dll-source/src/
  main.cpp            SKSE entry, API binding, version gate
  UI                  SKSE Menu Framework page
  ScopeSelector       actor scan + gap detection
  ContentLibrary      bio lookup across SkyrimNet's content layers
  Generator           context assembly, queue, dispatch
  RegionDigest        per-settlement reference sheet
  StagingStore        response parsing, staging, commit
  SkyrimNetAPI / Web  SkyrimNet's C API and its loopback content API
  Config              settings from SkyrimNet's config store
dll-source/include/   vendored SkyrimNet and SMF headers
mod-root/             shipped verbatim into the mod
tools/                prompt and ranking probes
```

## 📄 License

[GPL-3.0](LICENSE). The vendored headers in `dll-source/include/` keep their upstream
terms (SkyrimNet developer kit, SKSE Menu Framework).
