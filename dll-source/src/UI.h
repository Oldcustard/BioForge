#pragma once

namespace BioForge::UI
{
    // Registers BioForge's page under SKSE Menu Framework's mod control panel.
    // No-op (with a log line) when SMF is not installed. Safe to call from
    // SKSEPlugin_Load: SMF ships a preload marker, so its DLL is already in the
    // process before ordinary SKSE plugins load.
    void Register();
}
