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
//  - FLAVOUR is a seeded weighted roll per group, for variety. For a NEW bio
//    the roll is only the DEFAULT: the open groups go to the generate prompt
//    as numbered menus, and the model may override a default when the
//    evidence it is already reading points elsewhere (Dibella dialogue makes
//    the religion Dibella). For an EXISTING bio there is no LLM call: the
//    rules' `evidence` words are matched against the bio text instead.
//
// Everything that is not Pick() is pure string work with no RE:: calls, so
// Resolve() can run on the LLM completion worker.
namespace BioForge::BlockRules
{
    // A block chosen for one NPC.
    struct Choice
    {
        std::string              key;
        std::string              title;
        std::string              group;
        std::vector<std::string> tabs;     // the group's tabs: "already covered" test
        std::string              source;   // "fact: ...", "rolled", "evidence: ...", "chosen"
        bool                     carried{};// already on the NPC, not to be applied
    };

    // One open group offered to the model.
    struct Menu
    {
        std::string              group;
        std::vector<std::string> tabs;
        std::vector<std::string> keys;     // 1-based in the prompt
        std::vector<std::string> titles;
        int                      defaultIndex{ -1 };   // into keys; -1 = none
    };

    struct Plan
    {
        bool                rulesLoaded{};
        bool                catalogReady{};
        bool                appliedKnown{};  // the NPC's current blocks were read
        std::vector<Choice> carried;         // on the NPC already (any provider)
        std::vector<Choice> facts;
        std::vector<Choice> flavour;         // rolled (or evidence-led) picks
        std::vector<Menu>   menus;           // generation only: open groups
        int                 maxFlavour{};

        // What would be applied: facts + flavour, carried excluded.
        [[nodiscard]] std::vector<Choice> ToApply() const;
        [[nodiscard]] bool                Empty() const { return facts.empty() && flavour.empty(); }
    };

    // Read the rules file and resolve its form references. Call at
    // kDataLoaded; lines that do not parse are logged and skipped.
    void Load();
    std::size_t GroupCount();

    // Build a plan. MAIN THREAD ONLY: reads factions, race, voice, skills.
    // a_bioText is the NPC's existing bio (add mode) for `evidence` matching;
    // pass "" for a generation, where the model weighs the evidence itself.
    // a_forGeneration fills `menus`.
    Plan Pick(RE::Actor* a_actor, std::string_view a_bioText, bool a_forGeneration);

    // Rolls are seeded from the reference FormID plus this salt, so the same
    // NPC gets the same flavour until the player asks for another roll.
    std::uint32_t Salt(std::uint32_t a_refFormID);
    void          Reroll(std::uint32_t a_refFormID);

    // Context text for the generate prompt. Both may be "".
    std::string FixedForPrompt(const Plan& a_plan);
    std::string MenusForPrompt(const Plan& a_plan);

    // Settle the open menus from the model's `### bio_blocks` section. A
    // missing, malformed or off-menu answer keeps that group's default, and
    // the flavour cap is enforced on the result. Thread-safe, no RE:: calls.
    // a_note says what was overridden or fell back, for the log.
    std::vector<Choice> Resolve(const Plan& a_plan, std::string_view a_section,
                                std::string& a_note);

    // Short display form of a block title: "Religion: Talos Worshiper" ->
    // "Talos Worshiper".
    std::string ShortTitle(std::string_view a_title);
}
