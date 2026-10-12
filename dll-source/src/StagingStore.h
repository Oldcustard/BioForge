#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "BlockRules.h"
#include "ContentLibrary.h"

namespace BioForge
{
    struct Candidate;

    namespace Staging
    {
        // The ten blocks a character bio is made of, in the order
        // dynamic_character_bio consumes them. The LLM is instructed to emit
        // each under a `### <name>` heading; ParseResponse maps those headings
        // onto these blocks.
        inline constexpr std::string_view kBlockNames[10] = {
            "summary",    "interject_summary", "background",   "personality",
            "appearance", "aspirations",       "relationships", "occupation",
            "skills",     "speech_style"
        };

        enum class State
        {
            Generating,
            Staged,
            Failed
        };

        struct Entry
        {
            std::uint32_t refFormID{};
            std::string   name;          // NPC display name
            std::string   fileName;      // "gudra_59A6.prompt" - target bio slot
            std::string   stagingDir;    // bundle dir under BioForge/staging/
            State         state{ State::Generating };
            std::string   note;          // failure reason / parse gaps / commit result
            bool          committing{};  // a commit HTTP call is in flight
            bool          committed{};
            bool          refined{};     // ties rewritten by the second pass
            // SeverActions bio blocks settled for this bio (facts plus the
            // flavour the model kept or chose). Applied on commit.
            std::vector<BlockRules::Choice> blocks;
            std::string                     blocksNote;   // apply outcome
            // A blocks review for an NPC who already HAS a bio: no bio.prompt
            // in the bundle, and committing applies the blocks and nothing
            // else - the existing bio is never touched.
            bool                            blocksOnly{};
        };

        // <Data>/SKSE/Plugins/BioForge/staging, resolved from this DLL's own
        // location. Reads and writes go through the game process's USVFS view
        // so everything stays inside the running game's world (files created
        // by an EXTERNAL process while the game runs are enumerated by
        // SkyrimNet's path scan but fail to open). Deliberately NOT under
        // SkyrimNet's content tree: staging bundles are audit trail, not
        // content, and Beta 25 rejects anything but real content there.
        std::filesystem::path StagingRoot();

        // Target bio file name for a candidate. Prefers SkyrimNet's own
        // resolution (it may disambiguate); otherwise derives the corpus
        // convention: lowercase, spaces -> '_', anything outside [a-z0-9_-]
        // dropped, then '_' + low 12 bits of the REFERENCE FormID as three
        // uppercase hex digits (torg_strong-arm_9A8 <- 0x680059A8).
        std::string BioFileName(const Candidate& a_candidate);

        // The extra `### bio_blocks` section the generate prompt asks for
        // first: the model's settlement of the open bio-block menus. It is
        // not a bio block - ParseResponse skips it - and is read separately
        // with ExtractSection(raw, kBioBlocksSection).
        inline constexpr std::string_view kBioBlocksSection = "bio_blocks";

        // Parse an LLM response into the ten blocks and render it as a
        // {% block %} character bio in the corpus format. Lines before the
        // first `###` heading are treated as preamble and dropped; ``` fences
        // are stripped. False when any block is missing or empty - `a_missing`
        // names which.
        bool ParseResponse(std::string_view a_raw, std::string& a_bioText,
                           std::vector<std::string>& a_missing);

        // --- lifecycle, callable from any thread ---

        // Register an in-flight generation (called when the LLM task is queued).
        // a_blocksOnly marks a blocks review rather than a bio.
        void Begin(std::uint32_t a_refFormID, std::string_view a_name,
                   std::string_view a_fileName, bool a_blocksOnly = false);

        // Complete an entry from the LLM worker thread. Pure file I/O - no
        // RE:: calls. Writes the staging bundle (bio.prompt on a clean parse,
        // plus response.raw.txt and harvest.json either way, so a bad response
        // can still be inspected and retried). Empty a_bioText marks the entry
        // Failed with the missing blocks named in a_missing.
        // a_blocks are the settled bio blocks, written to blocks.txt beside
        // the bio and carried on the entry until commit applies them.
        void RecordGenerated(std::uint32_t a_refFormID, std::string_view a_contextJson,
                             std::string_view a_rawResponse, const std::string& a_bioText,
                             const std::vector<std::string>& a_missing,
                             const std::vector<BlockRules::Choice>& a_blocks,
                             std::string_view a_blocksNote);

        // Complete a blocks review (an entry begun with a_blocksOnly). Pure
        // file I/O, like RecordGenerated: the reply goes to response.raw.txt,
        // the settled blocks to blocks.txt, and the entry is staged for the
        // player to read and commit. a_rawResponse may be empty when there
        // was nothing for a model to decide (facts only). a_existingBio is
        // the bio the review read, kept so the reader can show it beside the
        // verdicts. Dropped rolls stay in a_blocks (Verdict::Dropped) for the
        // reader; commit skips them.
        void RecordBlocks(std::uint32_t a_refFormID, std::string_view a_contextJson,
                          std::string_view a_rawResponse,
                          const std::vector<BlockRules::Choice>& a_blocks,
                          std::string_view a_blocksNote, std::string_view a_existingBio);

        // Fail an entry (queue refusal, LLM error, no UUID...). a_reason is the
        // error string, possibly SkyrimNet's own text.
        void RecordFailed(std::uint32_t a_refFormID, std::string_view a_reason);

        // Thread-safe copy of all entries, oldest first, for the UI.
        std::vector<Entry> Snapshot();

        // Delete every staged bundle. Staging is per-session scratch: a bio that
        // was not committed is not in use by anything, and the review list it
        // belonged to died with the process. Called once as the game loads,
        // rather than on exit - Skyrim has no reliable shutdown hook, and file
        // I/O from DllMain during teardown is not worth the risk. Clearing on
        // the way in gives the same guarantee and leaves the last session's
        // bundles readable in the meantime, which is useful when a generation
        // went wrong and you want to see the prompt that produced it.
        // Committed bios live in SkyrimNet's content library and are never
        // touched.
        void ClearStaged();

        // Commit a staged bio into SkyrimNet's content library (the player's
        // overlay layer), through SkyrimNet's own HTTP API so its ContentStore
        // rescans and the bio goes live in seconds. Asynchronous: the HTTP
        // call runs on a worker thread - Commit itself never blocks, and the
        // outcome lands in the entry's committed/note fields by the next
        // Snapshot. Returns false only when there is nothing staged to commit;
        // a transport failure is reported through the note.
        //
        // When the commit lands, the entry's bio blocks are applied through
        // SeverActions (if blocks are enabled) - the one moment BioForge
        // applies blocks to a new bio, because the API asks for exactly that:
        // once, on the player's own action. A blocksOnly entry skips the bio
        // write and only applies its blocks.
        bool Commit(const Entry& a_entry);

        // Pull the body of one `{% block %}` out of .prompt text. Empty when
        // there is no such block.
        std::string ExtractBlock(std::string_view a_promptText, std::string_view a_blockName);

        // Shorthand for the block the roster and the digest harvest both want.
        std::string ExtractSummary(std::string_view a_promptText);

        // Pull one `### name` section out of a raw LLM reply, ending at the
        // next heading or the end of the text. For the refine pass, which asks
        // for a single block rather than all ten.
        std::string ExtractSection(std::string_view a_raw, std::string_view a_sectionName);

        // One line describing who this actor already is, for the roster handed
        // to a neighbour's generation: the first sentence of their summary
        // block. Checks THIS SESSION'S staging bundle first (a batch-mate
        // generated minutes ago is the freshest truth), then the committed
        // corpus. Empty when nobody has written them yet.
        //
        // This matters more than it looks. Without it the roster is bare names
        // and the model invents what each neighbour is like - which is how a
        // paying guest became a housemate and a girl from Ivarstead became a
        // resident, both of whom already had bios saying otherwise.
        //
        // The content index is passed in rather than built here: BioSummary
        // runs once per roster line, and one Build() per line would re-sweep
        // the whole library per batch-mate.
        std::string BioSummary(const Candidate&                        a_candidate,
                               const ContentLibrary::Index& a_committed);

        // The staged bio as written, for the review panel. Empty if the bundle
        // has no bio (a failed parse) or cannot be read.
        std::string ReadStagedBio(const Entry& a_entry);

        // The raw LLM response, for when a parse failed and the reason matters.
        std::string ReadRawResponse(const Entry& a_entry);

        // A blocks review's copy of the bio it was checked against.
        std::string ReadExistingBio(const Entry& a_entry);

        // Read a staged bio's own text, by reference rather than by Entry.
        // Empty when nothing is staged for it.
        std::string StagedBioFor(std::uint32_t a_refFormID);

        // Replace one block in an already-staged bio with a rewritten body and
        // save it. This is the refine pass landing: pass 1 wrote the bio when
        // some neighbours had no profile yet, and pass 2 rewrites just the
        // block that depended on them. Keeps the raw reply beside the original
        // as refine.raw.txt, so a bad refine is still inspectable.
        //
        // False when the entry is gone, the block is absent from the reply, or
        // the file cannot be written - a_note says which. The staged bio is
        // left untouched on any failure: a thin relationships block beats a
        // destroyed one.
        bool ApplyRefinedBlock(std::uint32_t a_refFormID, std::string_view a_blockName,
                               std::string_view a_rawResponse, std::string& a_note);

        // Reject a staged bio: delete its bundle and drop it from the review
        // list. Does NOT touch an already-committed bio in the content
        // library - undoing a commit means deleting the file in SkyrimNet's
        // dashboard.
        void Discard(const Entry& a_entry);
    }
}
