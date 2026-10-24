#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RE { class Actor; }

// Which SeverActions bio blocks an NPC should carry, from
// SKSE/Plugins/BioForge/block_rules.ini.
//
// Two kinds, kept apart on purpose:
//  - FACTS follow from game data (a Vigilant worships Stendarr). They are
//    fixed before anything else happens and the LLM is never offered them as
//    a choice.
//  - FLAVOUR is a seeded weighted roll per group, for variety. The roll comes
//    FIRST, and a model then reviews it against the evidence: it keeps each
//    rolled trait unless the evidence contradicts it (replace or drop), and
//    may add one in a group that did not roll only when the evidence names
//    it outright (Dibella dialogue makes the religion Dibella). For a new bio
//    the review rides the generate call; for an existing bio it is a small
//    call of its own (Generator::ReviewBlocks) reading that bio.
//
// Everything that is not Pick() is pure string work with no RE:: calls, so
// Resolve() can run on the LLM completion worker.
namespace BioForge::BlockRules
{
    // What the review made of a block, for the reader.
    enum class Verdict
    {
        Fact,      // follows from game data, never reviewed
        Rolled,    // rolled, and kept (or not yet reviewed)
        Changed,   // the model replaced the roll with this one
        Added,     // the model added it from named evidence
        Dropped    // the model dropped this roll - NOT applied
    };

    // A block chosen for one NPC.
    struct Choice
    {
        std::string              key;
        std::string              title;
        std::string              group;
        std::vector<std::string> tabs;     // the group's tabs: "already covered" test
        std::string              source;   // one line for logs and blocks.txt
        bool                     carried{};// already on the NPC, not to be applied
        Verdict                  verdict{ Verdict::Rolled };
        std::string              reason;   // the fact's condition, or the model's words
        std::string              was;      // Changed: the roll it replaced

        [[nodiscard]] bool Applies() const { return !carried && verdict != Verdict::Dropped; }
    };

    // One open group offered to the model: its roll to review when it has
    // one, otherwise options it may add from when the evidence names one.
    struct Menu
    {
        std::string              group;
        std::vector<std::string> tabs;
        std::vector<std::string> keys;     // 1-based in the prompt
        std::vector<std::string> titles;
        int                      defaultIndex{ -1 };   // the roll, into keys; -1 = none
    };

    struct Plan
    {
        bool                rulesLoaded{};
        bool                catalogReady{};
        bool                appliedKnown{};  // the NPC's current blocks were read
        std::vector<Choice> carried;         // on the NPC already (any provider)
        std::vector<Choice> facts;
        std::vector<Choice> flavour;         // the rolls, before any review
        std::vector<Menu>   menus;           // every open group, for the review

        // What would be applied: facts + flavour, carried excluded.
        [[nodiscard]] std::vector<Choice> ToApply() const;
        [[nodiscard]] bool                Empty() const { return facts.empty() && flavour.empty(); }
    };

    // Read the rules file and resolve its form references. Call at
    // kDataLoaded; lines that do not parse are logged and skipped.
    void Load();
    std::size_t GroupCount();

    // Build a plan. MAIN THREAD ONLY: reads factions, race, voice, skills.
    Plan Pick(RE::Actor* a_actor);

    // Rolls are seeded from the reference FormID plus this salt, so the same
    // NPC gets the same flavour until the player asks for another roll.
    std::uint32_t Salt(std::uint32_t a_refFormID);
    void          Reroll(std::uint32_t a_refFormID);

    // Context text for the generate and blocks prompts. Both may be "".
    // FixedForPrompt: carried blocks and facts, with their text.
    // ReviewForPrompt: the rolled traits to keep/replace/drop, and the
    // groups that did not roll, to add to only on named evidence.
    std::string FixedForPrompt(const Plan& a_plan);
    std::string ReviewForPrompt(const Plan& a_plan);

    // Settle the review from the model's `### bio_blocks` section:
    // `group: keep | drop | N` for a rolled group, `group: N` to add. A
    // missing or malformed answer keeps the roll (adds nothing). Thread-safe,
    // no RE:: calls. a_note says what changed or fell back, for the log.
    std::vector<Choice> Resolve(const Plan& a_plan, std::string_view a_section,
                                std::string& a_note);

    // Short display form of a block title: "Religion: Talos Worshiper" ->
    // "Talos Worshiper".
    std::string ShortTitle(std::string_view a_title);
}
