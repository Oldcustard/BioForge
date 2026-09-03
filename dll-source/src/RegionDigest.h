#pragma once

#include <string>

namespace BioForge::RegionDigest
{
    // A short reference sheet of the significant people and institutions in one
    // settlement, written once by the LLM and reused for every bio generated
    // there.
    //
    // Why this exists: the mechanical signals that connect an NPC to the wider
    // world - proper nouns in their dialogue, shared factions, world knowledge -
    // are all empty for a typical mod-added NPC. Torg Strong-Arm's entire
    // dialogue is "Need something?" and his only faction is the hold itself, yet
    // a Riften barkeep obviously deals with the Black-Briars, the rival inns and
    // the Thieves Guild. Deciding WHICH locals a person would know is a
    // judgement about those locals, not a lookup, so an LLM has to make it.
    //
    // Doing that per NPC would be unaffordable at load-order scale. The answer
    // is much the same for everyone in one settlement, so it is generated once,
    // cached on disk, and handed to every bio generated there - the cost
    // amortises away over exactly the bulk work this mod exists to do.

    struct Region
    {
        // The digest key: the settlement, not the hold. An earlier cut keyed on
        // the hold and it was far too coarse - a Riften barkeep and a Shor's
        // Stone farmhand are both "The Rift" and share almost nobody.
        std::string name;

        // The hold containing it, or "" when it is the hold itself (or cannot
        // be resolved). Only used to widen a thin candidate harvest: a hamlet
        // has few bios naming it, and its hold's cast is the next best thing.
        std::string hold;

        [[nodiscard]] bool Valid() const { return !name.empty(); }
    };

    // Where the player is standing, narrowed to the settlement. Walks the
    // location tree, so it reads game data: call it from the UI thread only,
    // alongside the rest of job assembly.
    Region Current();

    // The cached digest for a region name, or "" if none has been built yet.
    // Checks memory first, then last session's file on disk. Any thread.
    std::string Get(const std::string& a_region);

    // True while a digest request is in flight. Only one runs at a time.
    bool Building();

    // Build (or rebuild) the digest for a region through the LLM and cache it,
    // in memory and on disk. Returns false if the request could not be started.
    // Does nothing when one is already running, or when a digest already exists
    // and a_force is false. Reads the installed bio corpus, so: UI thread only.
    bool Build(const Region& a_region, bool a_force = false);
}
