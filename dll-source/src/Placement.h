#pragma once

#include <cstdint>
#include <string>

namespace BioForge
{
    // Where an NPC BELONGS, as opposed to where the scan happened to find them.
    //
    // The scan finds people where they are standing, and that was the only
    // place evidence a bio ever got - one line reading "Currently at: Vilemyr
    // Inn". So a warden of the Hall of Kyne, caught mid-visit to the inn, was
    // written up as a guest boarding there. Her own record said otherwise the
    // whole time: a sandbox package whose location target IS the Hall of Kyne.
    // Nothing was reading packages.
    namespace Placement
    {
        // Bullet lines describing the NPC's home and daily routine.
        //
        // MAIN THREAD ONLY - reads the reference, its base record and its AI
        // packages, so it belongs with the rest of job assembly and nowhere
        // near a completion callback.
        //
        // Never empty. When the game data genuinely says nothing, the last
        // line says so: a silent gap reads as "no information" and invites the
        // model to invent, which is the same reasoning behind the prompt's
        // "No employment markers" line and the roster's NO PROFILE YET.
        std::string RoutineOf(std::uint32_t a_refFormID);
    }
}
