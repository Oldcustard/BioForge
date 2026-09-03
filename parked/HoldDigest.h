#pragma once

#include <string>

namespace BioForge::HoldDigest
{
    // A short reference sheet of the significant people and institutions in one
    // hold, written once by the LLM and reused for every bio generated there.
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
    // is the same for everyone in the hold, so it is generated once, cached on
    // disk, and handed to every bio generated there - the cost amortises away
    // over exactly the bulk work this mod exists to do.

    // The hold the player is currently in, e.g. "The Rift". Empty when it cannot
    // be resolved. Main thread only - walks the location tree.
    std::string CurrentHold();

    // The cached digest for a hold, or "" if none has been built yet.
    std::string Get(const std::string& a_hold);

    // True while a digest request is in flight.
    bool Building();

    // Build (or rebuild) the digest for a hold through the LLM and cache it.
    // Returns false if the request could not be started. Safe to call when one
    // is already running - it does nothing. Main thread only: it reads the bio
    // corpus to assemble the candidate list.
    bool Build(const std::string& a_hold, bool a_force = false);
}
