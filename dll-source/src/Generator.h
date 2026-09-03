#pragma once

#include <cstddef>
#include <vector>

namespace BioForge
{
    struct Candidate;

    namespace Generator
    {
        struct Progress
        {
            int inFlight{};
            int queued{};

            [[nodiscard]] bool Busy() const { return inFlight > 0 || queued > 0; }
        };

        // Queue one NPC for bio generation through SkyrimNet's configured LLM.
        // Returns false when no job could be built (SkyrimNet too old, no save
        // loaded, no UUID for that reference). Progress lands in the staging
        // store and is visible through Staging::Snapshot() from any thread.
        // a_roster is everyone else the scan found in the same place. It is
        // what lets a bio name real neighbours: PublicGetRelatedActors is built
        // on shared EVENT history and comes back empty for a cell the player
        // has only just walked into, so without this every relationships block
        // falls back to abstractions like "the customers".
        bool Generate(const Candidate& a_candidate,
                      const std::vector<Candidate>& a_roster = {});

        // Queue every candidate given, honouring the concurrency cap. Returns
        // how many were queued - anything skipped failed to build a job and is
        // already logged. Call from the main thread: jobs read game data as
        // they are built.
        std::size_t GenerateAll(const std::vector<Candidate>& a_candidates,
                                const std::vector<Candidate>& a_roster = {});

        // For the UI: how much work is outstanding.
        Progress GetProgress();

        // Drop everything not yet dispatched. In-flight requests cannot be
        // recalled - they will finish and stage normally.
        void CancelQueued();
    }
}
