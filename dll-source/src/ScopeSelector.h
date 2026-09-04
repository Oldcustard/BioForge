#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RE { class Actor; }

namespace BioForge
{
    struct Candidate
    {
        std::uint32_t refFormID{};      // the placed reference - what bios key on
        std::uint32_t baseFormID{};
        std::string   name;
        std::string   race;
        std::string   sourcePlugin;     // plugin defining the base record
        std::string   bioTemplate;      // as SkyrimNet resolves it; "" means none
        // What a commit would write this bio as. Same as bioTemplate whenever
        // SkyrimNet has assigned one; where it has not, this is the derived
        // fallback, so the UI can always state the outcome instead of "(none)".
        std::string   wouldWriteAs;
        bool          bioFileExists{};
        // SkyrimNet resolves a UUID for this actor. When it does not, the
        // actor is invisible to every part of its API: no template name, and
        // no way to dispatch a generation - BuildContext bails and the job is
        // never queued. Measured at Nightgate: three of six gaps were silently
        // dropped from a batch for exactly this reason.
        bool          trackedBySkyrimNet{};
        bool          isUnique{};
        bool          isFollower{};   // travelling with the player: present by accident
        float         distance{};

        // A gap is an NPC SkyrimNet has no usable bio file for.
        [[nodiscard]] bool IsGap() const { return !bioFileExists; }

        // ...but a gap is only WORTH offering if SkyrimNet knows the actor.
        [[nodiscard]] bool CanGenerate() const { return trackedBySkyrimNet; }
    };

    // Enumerate actors around the player. Honours Config::Get() for radius,
    // cell restriction, unique-only and dead filtering. Sorted by distance.
    std::vector<Candidate> Scan();

    // Write the scan to the log, gaps first. This is the whole of phase 1.
    void LogGapReport(const std::vector<Candidate>& a_candidates);
}
