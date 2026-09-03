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
        bool          bioFileExists{};
        bool          isUnique{};
        bool          isFollower{};   // travelling with the player: present by accident
        float         distance{};

        // A gap is an NPC SkyrimNet has no usable bio file for.
        [[nodiscard]] bool IsGap() const { return !bioFileExists; }
    };

    // Enumerate actors around the player. Honours Config::Get() for radius,
    // cell restriction, unique-only and dead filtering. Sorted by distance.
    std::vector<Candidate> Scan();

    // Write the scan to the log, gaps first. This is the whole of phase 1.
    void LogGapReport(const std::vector<Candidate>& a_candidates);
}
