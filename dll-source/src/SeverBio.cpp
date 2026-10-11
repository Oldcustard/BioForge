#include "pch.h"

#include "SeverBio.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace BioForge::SeverBio
{
    namespace
    {
        constexpr auto kPlugin = "SeverActions.esp"sv;
        constexpr auto kScript = "SeverActionsNativeExt2"sv;

        // ListBlocks and GetContent are v4; ListApplied is v2.
        constexpr int kNeededVersion = 4;

        std::mutex                                       g_mutex;
        Status                                           g_status{ Status::Unknown };
        int                                              g_version{};
        std::vector<Block>                               g_catalog;
        bool                                             g_catalogReady{};
        std::uint32_t                                    g_catalogVersion{};
        std::map<std::uint32_t, std::vector<Applied>>    g_applied;
        std::uint32_t                                    g_appliedVersion{};

        // The VM hands a call's return value to one of these, on a VM thread.
        class Callback final : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            explicit Callback(std::function<void(const RE::BSScript::Variable&)> a_fn) :
                m_fn(std::move(a_fn))
            {}

            void operator()(RE::BSScript::Variable a_result) override
            {
                if (m_fn) {
                    m_fn(a_result);
                }
            }

            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::function<void(const RE::BSScript::Variable&)> m_fn;
        };

        // One static call. False when the VM would not take it - the script
        // is not loaded, which on an install without SeverActions is the
        // expected answer rather than an error.
        template <class... Args>
        bool Call(std::string_view a_fn, std::function<void(const RE::BSScript::Variable&)> a_onResult,
                  Args&&... a_args)
        {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                return false;
            }
            // By value: FunctionArguments is only defined for plain types,
            // and an lvalue argument would deduce a reference.
            auto* args = RE::MakeFunctionArguments(std::decay_t<Args>(std::forward<Args>(a_args))...);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{ new Callback(
                std::move(a_onResult)) };
            return vm->DispatchStaticCall(RE::BSFixedString{ kScript }, RE::BSFixedString{ a_fn },
                                          args, cb);
        }

        std::vector<std::string> Strings(const RE::BSScript::Variable& a_var)
        {
            std::vector<std::string> out;
            if (!a_var.IsArray() || a_var.IsNoneArray()) {
                return out;
            }
            const auto array = a_var.GetArray();
            if (!array) {
                return out;
            }
            out.reserve(array->size());
            for (const auto& element : *array) {
                if (element.IsString()) {
                    out.emplace_back(element.GetString());
                }
            }
            return out;
        }

        // Records are TAB-separated, and Papyrus' own Split drops empty
        // fields - which is why the key, the one field that may be empty,
        // comes last. Keep the empty fields here; nothing needs dropping.
        std::vector<std::string> SplitTabs(std::string_view a_record)
        {
            std::vector<std::string> out;
            std::size_t              start = 0;
            for (;;) {
                const auto tab = a_record.find('\t', start);
                out.emplace_back(a_record.substr(start, tab == std::string_view::npos
                                                            ? std::string_view::npos
                                                            : tab - start));
                if (tab == std::string_view::npos) {
                    break;
                }
                start = tab + 1;
            }
            return out;
        }

        std::string Lower(std::string_view a_s)
        {
            std::string out{ a_s };
            for (auto& ch : out) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return out;
        }

        // Keys are folded to lower case by SeverActions, but any string it
        // hands back may come back re-cased through the string pool.
        bool SameKey(std::string_view a_lhs, std::string_view a_rhs)
        {
            return Lower(a_lhs) == Lower(a_rhs);
        }

        void SetStatus(Status a_status, int a_version)
        {
            std::lock_guard lock{ g_mutex };
            g_status  = a_status;
            g_version = a_version;
        }

        void ParseApplied(std::uint32_t a_refFormID, const std::vector<std::string>& a_records)
        {
            std::vector<Applied> carried;
            for (const auto& record : a_records) {
                const auto f = SplitTabs(record);
                if (f.size() < 3) {
                    continue;
                }
                Applied a;
                a.direct = f[0] == "direct";
                a.tab    = f[1];
                a.title  = f[2];
                a.key    = f.size() > 3 ? Lower(f[3]) : std::string{};
                carried.push_back(std::move(a));
            }

            std::lock_guard lock{ g_mutex };
            g_applied[a_refFormID] = std::move(carried);
            ++g_appliedVersion;
        }
    }

    std::string ApplyResult::Describe() const
    {
        if (!error.empty()) {
            return "blocks not applied: " + error;
        }
        std::string out = std::to_string(applied) + " block(s) applied";
        if (alreadyHad > 0) {
            out += ", " + std::to_string(alreadyHad) + " already there";
        }
        if (!skipped.empty()) {
            out += ", " + std::to_string(skipped.size()) + " skipped (player's own choice there)";
        }
        if (!failed.empty()) {
            out += ", " + std::to_string(failed.size()) + " refused";
        }
        return out;
    }

    void Probe()
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data || (!data->LookupModByName(kPlugin) && !data->LookupLoadedLightModByName(kPlugin))) {
            SetStatus(Status::Absent, 0);
            logs::info("bio blocks: SeverActions.esp not loaded - blocks disabled"sv);
            return;
        }

        SetStatus(Status::Probing, 0);
        const bool sent = Call("BioApi_Version"sv, [](const RE::BSScript::Variable& a_result) {
            const int version = a_result.IsInt() ? a_result.GetSInt() : 0;
            if (version < kNeededVersion) {
                SetStatus(version <= 0 ? Status::Absent : Status::TooOld, version);
                logs::warn("bio blocks: SeverActions Bio Blocks API v{} - BioForge needs v{}"sv,
                           version, kNeededVersion);
                return;
            }
            SetStatus(Status::Ready, version);
            logs::info("bio blocks: SeverActions Bio Blocks API v{}"sv, version);
            RefreshCatalog();
        });
        if (!sent) {
            SetStatus(Status::Absent, 0);
            logs::warn("bio blocks: {} is not available to the VM"sv, kScript);
        }
    }

    Status GetStatus()
    {
        std::lock_guard lock{ g_mutex };
        return g_status;
    }

    int Version()
    {
        std::lock_guard lock{ g_mutex };
        return g_version;
    }

    const char* StatusText()
    {
        switch (GetStatus()) {
        case Status::Unknown: return "not checked yet (load a save)";
        case Status::Probing: return "checking...";
        case Status::Absent:  return "SeverActions not installed";
        case Status::TooOld:  return "SeverActions too old (needs Bio Blocks API v4)";
        case Status::Ready:   return "ready";
        }
        return "?";
    }

    std::vector<Block> Catalog()
    {
        std::lock_guard lock{ g_mutex };
        return g_catalog;
    }

    std::optional<Block> FindBlock(const std::string& a_key)
    {
        std::lock_guard lock{ g_mutex };
        for (const auto& b : g_catalog) {
            if (SameKey(b.key, a_key)) {
                return b;
            }
        }
        return std::nullopt;
    }

    bool CatalogReady()
    {
        std::lock_guard lock{ g_mutex };
        return g_catalogReady;
    }

    std::uint32_t CatalogVersion()
    {
        std::lock_guard lock{ g_mutex };
        return g_catalogVersion;
    }

    void RefreshCatalog()
    {
        if (GetStatus() != Status::Ready) {
            return;
        }

        Call(
            "BioApi_ListBlocks"sv,
            [](const RE::BSScript::Variable& a_result) {
                std::vector<Block> blocks;
                for (const auto& record : Strings(a_result)) {
                    const auto f = SplitTabs(record);
                    if (f.size() < 4 || f[3].empty()) {
                        continue;
                    }
                    Block b;
                    b.intimate = f[0] == "intimate";
                    b.tab      = f[1];
                    b.title    = f[2];
                    b.key      = Lower(f[3]);
                    blocks.push_back(std::move(b));
                }

                const auto count = blocks.size();
                {
                    std::lock_guard lock{ g_mutex };
                    g_catalog      = std::move(blocks);
                    g_catalogReady = true;
                    ++g_catalogVersion;
                }
                logs::info("bio blocks: {} applicable block(s) in the library"sv, count);

                // The text, one call per block, all dispatched at once. The
                // catalog is usable on titles alone while these land; the
                // text only sharpens what the bio prompt is told.
                for (std::size_t i = 0; i < count; ++i) {
                    std::string key;
                    {
                        std::lock_guard lock{ g_mutex };
                        if (i >= g_catalog.size()) {
                            break;
                        }
                        key = g_catalog[i].key;
                    }
                    Call(
                        "BioApi_GetContent"sv,
                        [key](const RE::BSScript::Variable& a_text) {
                            if (!a_text.IsString()) {
                                return;
                            }
                            std::lock_guard lock{ g_mutex };
                            for (auto& b : g_catalog) {
                                if (b.key == key) {
                                    b.content = std::string{ a_text.GetString() };
                                    break;
                                }
                            }
                        },
                        key);
                }
            },
            true);
    }

    void RefreshApplied(std::uint32_t a_refFormID)
    {
        if (GetStatus() != Status::Ready) {
            return;
        }
        // Resolving the actor and packing it reads game data, so it happens
        // on the game thread, whichever thread asked.
        SKSE::GetTaskInterface()->AddTask([a_refFormID]() {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_refFormID);
            if (!actor) {
                return;
            }
            Call(
                "BioApi_ListApplied"sv,
                [a_refFormID](const RE::BSScript::Variable& a_result) {
                    ParseApplied(a_refFormID, Strings(a_result));
                },
                actor);
        });
    }

    std::optional<std::vector<Applied>> AppliedFor(std::uint32_t a_refFormID)
    {
        std::lock_guard lock{ g_mutex };
        const auto      it = g_applied.find(a_refFormID);
        if (it == g_applied.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::uint32_t AppliedVersion()
    {
        std::lock_guard lock{ g_mutex };
        return g_appliedVersion;
    }

    void Apply(std::uint32_t a_refFormID, std::vector<ApplyItem> a_items, ApplyDone a_done)
    {
        if (GetStatus() != Status::Ready) {
            ApplyResult r;
            r.error = StatusText();
            if (a_done) {
                a_done(r);
            }
            return;
        }
        if (a_items.empty()) {
            if (a_done) {
                a_done(ApplyResult{});
            }
            return;
        }

        SKSE::GetTaskInterface()->AddTask([a_refFormID, items = std::move(a_items),
                                           done = std::move(a_done)]() mutable {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_refFormID);
            if (!actor) {
                ApplyResult r;
                r.error = "the NPC is not loaded - go near them and add blocks again";
                if (done) {
                    done(r);
                }
                return;
            }

            // Held by pointer: if the dispatch below is refused, the lambda
            // that would have owned it never runs and the caller still has
            // to hear about it.
            auto finish = std::make_shared<ApplyDone>(std::move(done));

            // Step one: what they carry NOW, so a choice the player made
            // since the plan was built stands.
            const bool sent = Call(
                "BioApi_ListApplied"sv,
                [a_refFormID, items = std::move(items),
                 finish](const RE::BSScript::Variable& a_list) {
                    const auto records = Strings(a_list);
                    ParseApplied(a_refFormID, records);

                    std::vector<Applied> carried;
                    if (auto known = AppliedFor(a_refFormID)) {
                        carried = std::move(*known);
                    }

                    auto result = std::make_shared<ApplyResult>();
                    std::vector<std::string> toApply;
                    for (const auto& item : items) {
                        const bool have = std::any_of(carried.begin(), carried.end(),
                                                      [&](const Applied& a) {
                                                          return SameKey(a.key, item.key);
                                                      });
                        if (have) {
                            ++result->alreadyHad;
                            continue;
                        }
                        // Covered by a block that is NOT one of ours in
                        // this batch: the player (or a faction rule) has
                        // already decided this group.
                        const bool covered = std::any_of(
                            carried.begin(), carried.end(), [&](const Applied& a) {
                                return std::any_of(item.tabs.begin(), item.tabs.end(),
                                                   [&](const std::string& t) {
                                                       return Lower(t) == Lower(a.tab);
                                                   });
                            });
                        if (covered) {
                            result->skipped.push_back(item.key);
                            continue;
                        }
                        toApply.push_back(item.key);
                    }

                    if (toApply.empty()) {
                        if (*finish) {
                            (*finish)(*result);
                        }
                        return;
                    }

                    auto remaining = std::make_shared<std::atomic<int>>(
                        static_cast<int>(toApply.size()));
                    auto resultMutex = std::make_shared<std::mutex>();

                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_refFormID);
                    for (const auto& key : toApply) {
                        const auto onApplied = [=](const RE::BSScript::Variable& a_ok) {
                            {
                                std::lock_guard lock{ *resultMutex };
                                if (a_ok.IsBool() && a_ok.GetBool()) {
                                    ++result->applied;
                                } else {
                                    result->failed.push_back(key);
                                }
                            }
                            if (remaining->fetch_sub(1) == 1) {
                                logs::info("bio blocks: {:08X} - {}"sv, a_refFormID,
                                           result->Describe());
                                RefreshApplied(a_refFormID);
                                if (*finish) {
                                    (*finish)(*result);
                                }
                            }
                        };
                        if (!actor || !Call("BioApi_Apply"sv, onApplied, actor, key)) {
                            RE::BSScript::Variable failed;
                            failed.SetBool(false);
                            onApplied(failed);
                        }
                    }
                },
                actor);

            if (!sent) {
                ApplyResult r;
                r.error = "SeverActions' script is not available";
                if (*finish) {
                    (*finish)(r);
                }
            }
        });
    }
}
