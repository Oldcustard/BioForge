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
        // True when ANY content layer provides this stem - a plugin pack, the
        // shipped base, the player's overlay, or this playthrough's per-save
        // files. Detection goes through ContentLibrary::Index, not a stat on
        // one directory: Beta 25 spreads bios across layers.
        bool          bioFileExists{};
        // The winning bio is the engine's own evolving per-save draft
        // (.dynamic.prompt). Not a gap - regenerating over it would discard
        // characterisation this playthrough built up, and SkyrimNet renders
        // the dynamic copy ahead of any static one, so a fresh commit would
        // lose to it anyway. Surfaced as its own row state.
        bool          bioDynamic{};
        // SkyrimNet resolves a UUID for this actor. When it does not, the
        // actor is invisible to every part of its API: no template name, and
        // no way to dispatch a generation - BuildContext bails and the job is
        // never queued. Measured at Nightgate: three of six gaps were silently
        // dropped from a batch for exactly this reason.
        bool          trackedBySkyrimNet{};
        bool          isUnique{};
        bool          isFollower{};   // travelling with the player: present by accident
        float         distance{};   // from the PLAYER, for the table's sort
        // World position, so a bio's roster can be the people near THAT NPC
        // rather than the people near wherever the player happened to stand.
        float         posX{};
        float         posY{};
        float         posZ{};

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
