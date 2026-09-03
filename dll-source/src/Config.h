#pragma once

#include <cstdint>

namespace BioForge::Config
{
    struct Settings
    {
        // Radius scan, in game units. ~3000 comfortably covers an inn interior.
        float scanRadius = 3000.0f;

        // Restrict the scan to the player's current cell.
        bool cellOnly = true;

        // Only consider NPCs flagged Unique. Generic leveled actors share base
        // records and are served fine by the generic fallback template.
        bool uniqueOnly = true;

        bool includeDead = false;

        // How many bio generations may be in flight at once. A batch over a
        // busy cell is otherwise a burst of simultaneous LLM calls, which is
        // the fastest way to meet a provider's rate limit. Two keeps the queue
        // moving without looking like an attack.
        int maxConcurrent = 2;

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
    };

    // Reads SkyrimNet's config store for this plugin, falling back to the
    // defaults above for anything missing or unparseable. Safe before SkyrimNet
    // resolves - it just yields defaults.
    void Load();

    const Settings& Get();
}
