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
    };

    // Reads SkyrimNet's config store for this plugin, falling back to the
    // defaults above for anything missing or unparseable. Safe before SkyrimNet
    // resolves - it just yields defaults.
    void Load();

    const Settings& Get();
}
