#pragma once

#include <cstdint>

namespace BioForge::Config
{
    struct Settings
    {
        // Scan scope follows where the player is standing, because the right
        // scope IS the interior/exterior distinction - there is no useful case
        // for a cell-only scan outdoors or a cross-cell scan inside a room, so
        // it was a toggle the user only ever got wrong.
        //
        // Indoors the scan is the whole current cell (enumerated directly, so a
        // large hall like the Blue Palace is covered with no radius to tune).
        // Outdoors it crosses cells, and THEN the radius applies.
        //
        // The real bound is neither this number nor the city's size: it is
        // uGridsToLoad. The engine keeps a uGrids x uGrids cell window around
        // the player (5x5 by default, 20480 units across) and unloads
        // everything else, in EVERY worldspace - the walled cities included.
        // Actors outside that window are not in memory at all, and
        // ForEachReferenceInRange can only return what is loaded. So at the
        // default uGrids the farthest an actor can POSSIBLY be is ~17400 units
        // (the window's corner-to-corner diagonal), no matter what this says.
        //
        // Measured, after two wrong theories: a Riften sweep at 40000 topped
        // out at 8429 and Solitude at 9959 - neither was radius-limited. The
        // tell was that scan counts change as the player walks, because the
        // window slides and swaps which actors exist.
        //
        // 20000 therefore covers everything the engine can load, with margin;
        // anything larger is dead weight. Raise it only alongside uGridsToLoad
        // (7 wants ~23000, 9 wants ~29000).
        float exteriorScanRadius = 20000.0f;

        // Only consider NPCs flagged Unique. Generic leveled actors share base
        // records and are served fine by the generic fallback template.
        bool uniqueOnly = true;

        bool includeDead = false;

        // How many bio generations may be in flight at once. A batch over a
        // busy cell is otherwise a burst of simultaneous LLM calls, which is
        // the fastest way to meet a provider's rate limit. Four measured well
        // on a real batch - four bios staged inside five seconds with nothing
        // refused - and it is still short of a burst. Lower it on a strict
        // endpoint; the clamp allows up to 8.
        int maxConcurrent = 4;

        // Hand every bio a short reference sheet of who matters in the
        // settlement it is being written in. Off means bios are written from
        // the NPC's own evidence alone.
        bool digestEnabled = true;

        // Build that sheet automatically before a BATCH when none is cached.
        // Only the batch: a single generation should not silently spend an
        // extra LLM call, and it is the batch the cost amortises over anyway.
        bool digestAutoBuild = true;

        // How many installed bios to offer the digest as candidates. This is
        // the one genuinely large block in that prompt, so it is capped.
        int digestMaxCandidates = 120;

        // After a batch, re-ask for the relationships block of any bio whose
        // neighbours were still unwritten when it was composed. One small extra
        // call per affected NPC, and only for those that actually gained a
        // known neighbour.
        bool refinePass = true;

        // Send BioForge's calls on its own `bioforge` variant (declared in
        // settings/BioForge.yaml, configured in SkyrimNet's LLM settings)
        // instead of SkyrimNet's CharacterProfileGeneration. Off by default:
        // a fresh variant inherits the DIALOGUE defaults - a flash model on a
        // 4k cap, tight for a ten-block bio - until the player configures it.
        bool ownVariant = false;

        // So Refresh() can tell whether anything actually moved.
        bool operator==(const Settings&) const = default;
    };

    // Reads SkyrimNet's config store for this plugin, falling back to the
    // defaults above for anything missing or unparseable. Safe before SkyrimNet
    // resolves - it just yields defaults. Logs what it read.
    void Load();

    // Re-read, and adopt the values if they changed.
    //
    // Settings used to be read only at kDataLoaded, so changing one in
    // SkyrimNet's own panel wrote its settings.yaml and reached nothing until a
    // full restart - a save reload does not re-fire the event. Nothing said so,
    // and the setting simply appeared not to work; it cost two experiments in
    // one evening before the cause was spotted.
    //
    // Cheap enough to call every frame: the actual read is throttled inside,
    // and a read that changes nothing is silent. MAIN THREAD ONLY, and never
    // while a generation is in flight - Pump reads maxConcurrent from a
    // completion callback on a worker thread.
    void Refresh();

    const Settings& Get();

    // The SkyrimNet LLM variant every BioForge call goes out on - bios, the
    // refine pass and the region digest alike, so one switch moves them all.
    const char* LlmVariant();
}
