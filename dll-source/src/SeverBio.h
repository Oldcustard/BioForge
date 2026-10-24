#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// SeverActions' Bio Blocks, through its public Papyrus API (v4).
//
// SeverActions keeps a library of short bio snippets ("blocks") and renders
// the ones applied to an NPC into that NPC's SkyrimNet bio at render time.
// Who carries which block is stored per save by SeverActions; applying one
// never touches a bio file. BioForge only ever applies KEYED blocks - the
// ones SeverActions ships (severause.severactions:*) and any other mod's -
// because the player's own blocks carry no key and no mod can apply them.
//
// The API is Papyrus-only: Global Native functions on SeverActionsNativeExt2.
// Every call is a VM dispatch with a result callback, so NOTHING here is
// synchronous. Results land in this module's caches, which the UI and the
// rule picker read; each call waits about a frame on SeverActions' side.
//
// The API guide's one hard rule, kept here: apply ONCE, on the user's own
// action (a commit, of a bio or of a blocks review) - never on load, and never in
// reaction to SeverActions' change event.
namespace BioForge::SeverBio
{
    enum class Status
    {
        Unknown,    // not probed yet (no save loaded)
        Probing,
        Absent,     // SeverActions.esp not loaded, or its script is missing
        TooOld,     // BioApi_Version() < 4
        Ready
    };

    // One block a mod can apply, from BioApi_ListBlocks.
    struct Block
    {
        std::string key;
        std::string tab;
        std::string title;
        std::string content;    // BioApi_GetContent; filled in after the list
        bool        intimate{};
    };

    // One block an NPC's bio shows, from BioApi_ListApplied.
    struct Applied
    {
        bool        direct{};   // false: a faction rule grants it
        std::string tab;
        std::string title;
        std::string key;        // "" for the player's own block
    };

    // Ask SeverActions what it offers. Checks the plugin is loaded, then
    // dispatches BioApi_Version and, on v4+, refreshes the catalog. Call
    // after a save loads (the VM is running then); safe to call again.
    void Probe();

    Status      GetStatus();
    int         Version();
    const char* StatusText();

    // The library of applicable blocks, empty until the first ListBlocks
    // lands. CatalogVersion() moves every time it is replaced, so a cached
    // plan can tell it is stale.
    std::vector<Block>          Catalog();
    std::optional<Block>        FindBlock(const std::string& a_key);
    bool                        CatalogReady();
    std::uint32_t               CatalogVersion();
    void                        RefreshCatalog();

    // What an NPC carries. Empty optional until a ListApplied for them has
    // landed. AppliedVersion() moves with every update, for cache checks.
    void                                RefreshApplied(std::uint32_t a_refFormID);
    std::optional<std::vector<Applied>> AppliedFor(std::uint32_t a_refFormID);
    std::uint32_t                       AppliedVersion();

    // A block to apply, and the tabs that count as "this group is already
    // covered" - so a religion block the player chose is never joined by a
    // second one from BioForge.
    struct ApplyItem
    {
        std::string              key;
        std::vector<std::string> tabs;
    };

    struct ApplyResult
    {
        int                      applied{};
        int                      alreadyHad{};
        std::vector<std::string> skipped;   // their group was already covered
        std::vector<std::string> failed;
        std::string              error;     // whole call failed (NPC not loaded...)

        [[nodiscard]] std::string Describe() const;
    };

    using ApplyDone = std::function<void(const ApplyResult&)>;

    // Apply blocks to one NPC. Reads what the NPC carries first, at apply
    // time, so a block the player added since the plan was made is honoured.
    // Callable from any thread; the work is marshalled onto the game thread
    // and the VM. a_done runs on a VM thread and must not touch game data.
    void Apply(std::uint32_t a_refFormID, std::vector<ApplyItem> a_items, ApplyDone a_done);
}
