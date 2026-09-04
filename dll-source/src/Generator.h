#pragma once

#include <cstddef>
#include <string>
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

            // Candidates held back until the region digest they were promised
            // arrives. They are not jobs yet: job assembly reads game data, so
            // it has to wait for Tick() on the UI thread.
            int pending{};

            std::string pendingRegion;

            // Bios staged with a neighbour still unwritten, waiting for the
            // batch to settle so their relationships block can be re-asked.
            // Not jobs yet and not in the queue - RunRefinePass turns them into
            // jobs only once nothing is in flight - so without this the panel
            // went quiet while a whole second pass was still to come.
            int awaitingRevision{};

            // Pass-two jobs actually running or queued. Distinct from
            // awaitingRevision, which is the ones not yet turned into jobs.
            int revising{};

            [[nodiscard]] bool Busy() const
            {
                return inFlight > 0 || queued > 0 || pending > 0 || awaitingRevision > 0;
            }
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
        //
        // Returns 0 having queued nothing when the batch is DEFERRED: no region
        // digest is cached for this settlement and digest.autoBuild is on, so
        // the candidates are held and Tick() queues them once it arrives.
        // GetProgress().pending reports that wait.
        std::size_t GenerateAll(const std::vector<Candidate>& a_candidates,
                                const std::vector<Candidate>& a_roster = {});

        // For the UI: how much work is outstanding.
        Progress GetProgress();

        // Drop everything not yet dispatched, a batch waiting on a digest
        // included. In-flight requests cannot be recalled - they will finish
        // and stage normally.
        void CancelQueued();

        // Release a batch that was waiting on its region digest. Cheap when
        // nothing is pending; call it once per frame from the UI, which is the
        // only main-thread pump this plugin has. The consequence is that a
        // deferred batch only advances while the panel is open - acceptable,
        // since that is where you press the button and watch it run.
        void Tick();
    }
}
