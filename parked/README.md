# Parked: regional digest for relationship seeding

Paused deliberately, not abandoned. The idea: an LLM decides which *other* NPCs a
person would plausibly know, because the mechanical signals (proper nouns in their
dialogue, shared factions, world knowledge) are all empty for a typical mod-added
NPC — Torg Strong-Arm's entire dialogue is "Need something?" and his only faction is
the hold itself. Deciding who a Riften barkeep deals with is a judgement about those
other people, not a lookup.

Doing that per NPC is unaffordable at load-order scale, so the answer is computed
once for an area and reused by everyone in it.

## Why it is parked

The first cut keyed on the **hold** ("The Rift"), which is too coarse. A Riften
barkeep and a farmhand in Shor's Stone are both "The Rift" but share almost nobody.
Narrow it to town/settlement before resuming.

The location tree already supports this: `GetCurrentLocation()` walks
`parentLoc` from interior -> city -> hold, so stopping at the city rather than
walking to the top is a small change to `CurrentHold()`. Riften interiors resolve
through a city location on the way up. Worth checking what mod-added settlements
resolve to; some parent straight to a hold or to nothing.

## What is here

- `HoldDigest.{h,cpp}` — hold detection, bio-corpus candidate harvest, LLM call,
  disk cache under `SKSE/Plugins/BioForge/holds/`. NOT in CMakeLists.
- `bioforge_hold_digest.prompt` — the digest prompt: 6-10 bullets of who holds
  power/trade locally and why an ordinary worker would care.

Known bug: `std::toupper` in `NameFromStem` fails to compile (C2589) — a Windows
header defines `toupper` as a macro, so `std::toupper(` expands wrongly. Use
`(std::toupper)(...)` or `::toupper`.

## Design notes worth keeping

- Candidate source is the user's OWN installed bios (3,207 here): the `summary`
  block of each is a ready-made description, and it reflects their actual load
  order including mod NPCs. 279 mention Riften; 122 mention Maven.
- Nobody else's bio mentions Torg — reciprocity does not exist in the corpus, and
  one-directional "knows of" is already the norm. Seed public/institutional ties
  only ("buys from", "pays his cut to", "keeps clear of"), never mutual bonds.
- Validate names against the load order after generation, so the model cannot
  invent locals this install does not have.
- Watch famous-name gravity: hand a bio Maven Black-Briar and she will crowd out
  the ordinary person the profile is about.
