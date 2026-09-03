#pragma once

#include <string>

namespace BioForge::PromptReload
{
    // POST /prompts?api=reload against SkyrimNet's loopback web server so a
    // freshly committed bio resolves without a restart. The port comes from
    // <SkyrimNet>/config/WebServer.yaml (default 8080). Returns "" on success,
    // otherwise a short human-readable reason. Pure networking + file read -
    // safe from any thread, including the LLM worker.
    std::string Request();
}
