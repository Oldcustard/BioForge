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
        // Outdoors it crosses cells, and THEN the radius is the scope - a town
        // sweep must reach the WHOLE settlement from wherever the player is
        // standing, so the figure is a diameter, not a radius.
        //
        // The bound is the city's CELL FOOTPRINT, not where its NPCs were
        // placed. Measured from the record data, SolitudeWorld's city cluster
        // is cells X[-3,3] Y[-3,1] - 7x5 cells, 28672 x 20480 units, a
        // corner-to-corner diagonal of ~35200 - and Whiterun, Windhelm and
        // Riften are all larger. Placement positions are much tighter (vanilla
        // Solitude's cast sits inside ~2800 units) but that is IRRELEVANT: NPCs
        // walk their routines, so any of them can be anywhere navmeshed in the
        // worldspace at scan time. Reasoning from editor positions here was a
        // real mistake - it is the wandering that this whole feature exists to
        // catch.
        //
        // Hence the max: 40000 covers the largest city from any corner. It
        // costs nothing. Outdoors in Tamriel, ForEachReferenceInRange only
        // returns LOADED actors and uGridsToLoad caps that regardless of the
        // number here; inside a city (all five are flagged SmallWorld, so
        // fully loaded) every actor is already in memory and a bigger radius
        // merely stops DISCARDING them.
        float exteriorScanRadius = 40000.0f;

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
}
