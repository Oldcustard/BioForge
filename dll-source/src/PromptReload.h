#pragma once

#include <string>

namespace BioForge::PromptReload
{
    // POST /prompts?api=reload against SkyrimNet's loopback web server so a
    // freshly committed bio resolves without a restart. The port comes from
    // <SkyrimNet>/config/WebServer.yaml (default 8080). Returns "" on success,
    // otherwise a short human-readable reason. Pure networking + file read -
    // safe from any thread, including the LLM worker.
    //
    // BLOCKS until SkyrimNet has rescanned its whole prompt tree. That is
    // seconds on a large corpus - measured at 2.4-3.3s across a 3,200-bio
    // install, once at 10.7s - so it must NOT be called from the UI thread.
    // Use Schedule() from anything the player is waiting on.
    std::string Request();

    // Ask for a reload without waiting for one. Returns immediately, runs
    // Request() on a background thread, and COALESCES a burst into a single
    // reload: committing six bios should rescan the prompt tree once, not six
    // times. The outcome is logged rather than returned, because by the time
    // it is known the caller is long gone.
    void Schedule();
}
