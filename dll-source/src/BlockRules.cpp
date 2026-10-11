#include "pch.h"

#include "BlockRules.h"

#include "Config.h"
#include "ContentLibrary.h"
#include "SeverBio.h"

#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <random>

namespace BioForge::BlockRules
{
    namespace
    {
        enum class CondKind
        {
            Faction,   // faction(Skyrim.esm|0B3292)
            Keyword,   // keyword(Vampire)       - on the NPC record or its race
            Race,      // race(Nord)             - race editor ID prefix
            Voice,     // voice(Skyrim.esm|013AD4)
            Child,
            Adult,
            Female,
            Male,
            Skill      // skill(Restoration>=50)
        };

        struct Cond
        {
            CondKind         kind{};
            RE::TESForm*     form{};
            std::string      text;       // keyword / race prefix
            RE::ActorValue   av{ RE::ActorValue::kNone };
            float            threshold{};
            std::string      label;      // as written, for the source note
        };

        struct Entry
        {
            std::string              key;
            bool                     fact{};
            int                      weight{ 1 };
            std::vector<Cond>        when;
            std::vector<Cond>        unless;
            std::vector<Cond>        rarely;     // any holds: a tenth of the weight
        };

        struct Group
        {
            std::string              name;
            std::vector<std::string> tabs;
            int                      chance{};
            bool                     intimate{};
            std::vector<Cond>        unless;
            std::vector<Entry>       entries;
        };

        std::vector<Group> g_groups;
        bool               g_loaded{};

        std::mutex                            g_saltMutex;
        std::map<std::uint32_t, std::uint32_t> g_salt;

        std::string_view Trim(std::string_view a_s)
        {
            while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.front()))) {
                a_s.remove_prefix(1);
            }
            while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.back()))) {
                a_s.remove_suffix(1);
            }
            return a_s;
        }

        std::string Lower(std::string_view a_s)
        {
            std::string out{ a_s };
            for (auto& ch : out) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return out;
        }

        std::vector<std::string> SplitWords(std::string_view a_s)
        {
            std::vector<std::string> out;
            std::size_t              pos = 0;
            while (pos < a_s.size()) {
                while (pos < a_s.size() && std::isspace(static_cast<unsigned char>(a_s[pos]))) {
                    ++pos;
                }
                const auto start = pos;
                while (pos < a_s.size() && !std::isspace(static_cast<unsigned char>(a_s[pos]))) {
                    ++pos;
                }
                if (pos > start) {
                    out.emplace_back(a_s.substr(start, pos - start));
                }
            }
            return out;
        }

        std::vector<std::string> SplitList(std::string_view a_s, char a_sep)
        {
            std::vector<std::string> out;
            std::size_t              start = 0;
            for (;;) {
                const auto at   = a_s.find(a_sep, start);
                const auto item = Trim(a_s.substr(start, at == std::string_view::npos
                                                              ? std::string_view::npos
                                                              : at - start));
                if (!item.empty()) {
                    out.emplace_back(item);
                }
                if (at == std::string_view::npos) {
                    break;
                }
                start = at + 1;
            }
            return out;
        }

        // "Skyrim.esm|0B3292" -> the form, or nullptr (logged by the caller).
        RE::TESForm* ResolveForm(std::string_view a_ref)
        {
            const auto bar = a_ref.find('|');
            if (bar == std::string_view::npos) {
                return nullptr;
            }
            const auto plugin = Trim(a_ref.substr(0, bar));
            const auto hex    = std::string{ Trim(a_ref.substr(bar + 1)) };
            RE::FormID local{};
            try {
                local = static_cast<RE::FormID>(std::stoul(hex, nullptr, 16));
            } catch (...) {
                return nullptr;
            }
            auto* data = RE::TESDataHandler::GetSingleton();
            return data ? data->LookupForm(local, plugin) : nullptr;
        }

        bool ParseCond(std::string_view a_token, Cond& a_cond, std::string& a_error)
        {
            a_cond       = {};
            a_cond.label = a_token;

            const auto open  = a_token.find('(');
            const auto close = a_token.rfind(')');
            const auto name  = Lower(open == std::string_view::npos ? a_token
                                                                    : a_token.substr(0, open));
            std::string arg;
            if (open != std::string_view::npos) {
                if (close == std::string_view::npos || close < open) {
                    a_error = "unclosed '(' in " + std::string{ a_token };
                    return false;
                }
                arg = std::string{ Trim(a_token.substr(open + 1, close - open - 1)) };
            }

            if (name == "child") { a_cond.kind = CondKind::Child; return true; }
            if (name == "adult") { a_cond.kind = CondKind::Adult; return true; }
            if (name == "female") { a_cond.kind = CondKind::Female; return true; }
            if (name == "male") { a_cond.kind = CondKind::Male; return true; }

            if (arg.empty()) {
                a_error = "missing argument in " + std::string{ a_token };
                return false;
            }

            if (name == "faction" || name == "voice") {
                a_cond.kind = name == "faction" ? CondKind::Faction : CondKind::Voice;
                a_cond.form = ResolveForm(arg);
                if (!a_cond.form) {
                    // A form from a plugin that is not loaded is not an error
                    // worth refusing the line for: the condition just never
                    // holds. Logged once so a typo is still findable.
                    logs::info("block rules: {} resolves to nothing in this load order"sv,
                               a_token);
                    return true;
                }
                const bool rightType =
                    name == "faction" ? a_cond.form->Is(RE::FormType::Faction)
                                      : a_cond.form->Is(RE::FormType::VoiceType);
                if (!rightType) {
                    a_error = std::string{ a_token } + " is not a " + name;
                    return false;
                }
                // "faction Vigilants of Stendarr" reads better in the panel
                // than a FormID; SE keeps no editor IDs for most forms.
                if (const char* display = a_cond.form->GetName(); display && *display) {
                    a_cond.label = name + " " + display;
                }
                return true;
            }
            if (name == "keyword") {
                a_cond.kind = CondKind::Keyword;
                a_cond.text = arg;
                return true;
            }
            if (name == "race") {
                a_cond.kind = CondKind::Race;
                a_cond.text = Lower(arg);
                return true;
            }
            if (name == "skill") {
                const auto ge = arg.find(">=");
                if (ge == std::string::npos) {
                    a_error = "skill() wants Name>=Value: " + std::string{ a_token };
                    return false;
                }
                const auto skill = std::string{ Trim(std::string_view{ arg }.substr(0, ge)) };
                a_cond.kind      = CondKind::Skill;
                a_cond.av        = RE::ActorValueList::LookupActorValueByName(skill.c_str());
                if (a_cond.av == RE::ActorValue::kNone) {
                    a_error = "unknown skill in " + std::string{ a_token };
                    return false;
                }
                try {
                    a_cond.threshold = std::stof(arg.substr(ge + 2));
                } catch (...) {
                    a_error = "bad number in " + std::string{ a_token };
                    return false;
                }
                return true;
            }

            a_error = "unknown condition " + std::string{ a_token };
            return false;
        }

        bool ParseConds(const std::vector<std::string>& a_tokens, std::vector<Cond>& a_out,
                        std::string& a_error)
        {
            for (const auto& token : a_tokens) {
                Cond c;
                if (!ParseCond(token, c, a_error)) {
                    return false;
                }
                a_out.push_back(std::move(c));
            }
            return true;
        }

        // <key> [weight N] [when c c ...] [unless c c ...] [rarely c c ...]
        bool ParseEntry(std::string_view a_value, bool a_fact, Entry& a_entry, std::string& a_error)
        {
            a_entry      = {};
            a_entry.fact = a_fact;

            const auto tokens = SplitWords(a_value);
            if (tokens.empty()) {
                a_error = "no block key";
                return false;
            }
            a_entry.key = Lower(tokens[0]);
            if (a_entry.key.find(':') == std::string::npos) {
                a_error = "'" + tokens[0] + "' is not a block key (<author>.<mod>:<local>)";
                return false;
            }

            enum { None, When, Unless, Rarely } mode = None;
            std::vector<std::string> when, unless, rarely;
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                const auto word = Lower(tokens[i]);
                if (a_fact && (word == "weight" || word == "rarely")) {
                    a_error = "a fact always applies, so it takes no '" + word + "'";
                    return false;
                }
                if (word == "weight" && i + 1 < tokens.size()) {
                    try {
                        a_entry.weight = (std::max)(0, std::stoi(tokens[++i]));
                    } catch (...) {
                        a_error = "bad weight";
                        return false;
                    }
                    mode = None;
                } else if (word == "when") {
                    mode = When;
                } else if (word == "unless") {
                    mode = Unless;
                } else if (word == "rarely") {
                    mode = Rarely;
                } else if (mode == When) {
                    when.push_back(tokens[i]);
                } else if (mode == Unless) {
                    unless.push_back(tokens[i]);
                } else if (mode == Rarely) {
                    rarely.push_back(tokens[i]);
                } else {
                    a_error = "unexpected '" + tokens[i] + "'";
                    return false;
                }
            }
            return ParseConds(when, a_entry.when, a_error) &&
                   ParseConds(unless, a_entry.unless, a_error) &&
                   ParseConds(rarely, a_entry.rarely, a_error);
        }

        std::filesystem::path RulesPath()
        {
            return ContentLibrary::SkyrimNetDir().parent_path() / "BioForge" / "block_rules.ini";
        }

        bool Holds(const Cond& a_cond, RE::Actor* a_actor)
        {
            auto* base = a_actor->GetActorBase();
            auto* race = a_actor->GetRace();
            switch (a_cond.kind) {
            case CondKind::Faction:
                return a_cond.form &&
                       a_actor->IsInFaction(a_cond.form->As<RE::TESFaction>());
            case CondKind::Keyword:
                return (base && base->HasKeywordString(a_cond.text)) ||
                       (race && race->HasKeywordString(a_cond.text));
            case CondKind::Race:
                return race && Lower(race->GetFormEditorID()).starts_with(a_cond.text);
            case CondKind::Voice:
                return a_cond.form && base && base->voiceType == a_cond.form;
            case CondKind::Child:
                return a_actor->IsChild();
            case CondKind::Adult:
                return !a_actor->IsChild();
            case CondKind::Female:
                return base && base->GetSex() == RE::SEX::kFemale;
            case CondKind::Male:
                return base && base->GetSex() == RE::SEX::kMale;
            case CondKind::Skill:
                if (auto* owner = a_actor->AsActorValueOwner()) {
                    return owner->GetBaseActorValue(a_cond.av) >= a_cond.threshold;
                }
                return false;
            }
            return false;
        }

        bool AllHold(const std::vector<Cond>& a_conds, RE::Actor* a_actor)
        {
            return std::all_of(a_conds.begin(), a_conds.end(),
                               [&](const Cond& c) { return Holds(c, a_actor); });
        }

        bool AnyHolds(const std::vector<Cond>& a_conds, RE::Actor* a_actor)
        {
            return std::any_of(a_conds.begin(), a_conds.end(),
                               [&](const Cond& c) { return Holds(c, a_actor); });
        }

        std::string Labels(const std::vector<Cond>& a_conds)
        {
            std::string out;
            for (const auto& c : a_conds) {
                out += (out.empty() ? "" : " ");
                out += c.label;
            }
            return out;
        }

        // FNV-1a: a stable seed per (NPC, reroll count, group), so one
        // group's roll never shifts because another group was added.
        std::uint32_t Seed(std::uint32_t a_ref, std::uint32_t a_salt, std::string_view a_group)
        {
            std::uint32_t h     = 2166136261u;
            const auto    mix = [&](std::uint8_t a_byte) {
                h ^= a_byte;
                h *= 16777619u;
            };
            for (int i = 0; i < 4; ++i) {
                mix(static_cast<std::uint8_t>(a_ref >> (8 * i)));
            }
            for (int i = 0; i < 4; ++i) {
                mix(static_cast<std::uint8_t>(a_salt >> (8 * i)));
            }
            for (const char ch : a_group) {
                mix(static_cast<std::uint8_t>(ch));
            }
            return h;
        }

        bool TabMatches(const std::vector<std::string>& a_tabs, std::string_view a_tab)
        {
            const auto tab = Lower(a_tab);
            return std::any_of(a_tabs.begin(), a_tabs.end(),
                               [&](const std::string& t) { return Lower(t) == tab; });
        }

        std::string Flatten(std::string_view a_text, std::size_t a_max)
        {
            std::string out;
            bool        space = false;
            for (const char ch : a_text) {
                if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
                    space = !out.empty();
                    continue;
                }
                if (ch == '*') {
                    continue;   // markdown emphasis in block text
                }
                if (space) {
                    out += ' ';
                    space = false;
                }
                out += ch;
                if (out.size() >= a_max) {
                    out += "...";
                    break;
                }
            }
            return out;
        }
    }

    std::vector<Choice> Plan::ToApply() const
    {
        std::vector<Choice> out = facts;
        out.insert(out.end(), flavour.begin(), flavour.end());
        return out;
    }

    void Load()
    {
        g_groups.clear();
        g_loaded = false;

        const auto    path = RulesPath();
        std::ifstream in{ path, std::ios::binary };
        if (!in) {
            logs::warn("block rules: {} not found - bio blocks disabled"sv,
                       ContentLibrary::Utf8Of(path));
            return;
        }

        Group*      current = nullptr;
        std::string line;
        int         lineNo   = 0;
        int         rejected = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            // Comments: whole-line ; or #, and an inline " ;".
            if (const auto at = line.find(" ;"); at != std::string::npos) {
                line.resize(at);
            }
            const auto text = Trim(line);
            if (text.empty() || text.front() == ';' || text.front() == '#') {
                continue;
            }

            if (text.front() == '[') {
                const auto close = text.find(']');
                if (close == std::string_view::npos) {
                    logs::warn("block rules: line {}: unclosed group header"sv, lineNo);
                    ++rejected;
                    current = nullptr;
                    continue;
                }
                g_groups.push_back(Group{});
                current       = &g_groups.back();
                current->name = Lower(Trim(text.substr(1, close - 1)));
                continue;
            }

            const auto eq = text.find('=');
            if (!current || eq == std::string_view::npos) {
                logs::warn("block rules: line {}: not inside a [group] or no '='"sv, lineNo);
                ++rejected;
                continue;
            }
            const auto name  = Lower(Trim(text.substr(0, eq)));
            const auto value = Trim(text.substr(eq + 1));

            std::string error;
            if (name == "tabs") {
                current->tabs = SplitList(value, ',');
            } else if (name == "chance") {
                try {
                    current->chance = std::clamp(std::stoi(std::string{ value }), 0, 100);
                } catch (...) {
                    error = "bad chance";
                }
            } else if (name == "intimate") {
                current->intimate = Lower(value) == "true";
            } else if (name == "unless") {
                ParseConds(SplitWords(value), current->unless, error);
            } else if (name == "fact" || name == "flavour" || name == "flavor") {
                Entry entry;
                if (ParseEntry(value, name == "fact", entry, error)) {
                    current->entries.push_back(std::move(entry));
                }
            } else {
                error = "unknown setting '" + name + "'";
            }

            if (!error.empty()) {
                logs::warn("block rules: line {}: {} - skipped"sv, lineNo, error);
                ++rejected;
            }
        }

        std::size_t entries = 0;
        for (const auto& g : g_groups) {
            entries += g.entries.size();
        }
        g_loaded = true;
        logs::info("block rules: {} group(s), {} rule(s){}"sv, g_groups.size(), entries,
                   rejected ? std::format(", {} line(s) rejected", rejected) : std::string{});
    }

    std::size_t GroupCount() { return g_groups.size(); }

    std::uint32_t Salt(std::uint32_t a_refFormID)
    {
        std::lock_guard lock{ g_saltMutex };
        const auto      it = g_salt.find(a_refFormID);
        return it == g_salt.end() ? 0u : it->second;
    }

    void Reroll(std::uint32_t a_refFormID)
    {
        std::lock_guard lock{ g_saltMutex };
        ++g_salt[a_refFormID];
    }

    Plan Pick(RE::Actor* a_actor)
    {
        Plan plan;
        const auto& cfg  = Config::Get();
        plan.maxFlavour  = cfg.blocksMaxFlavour;
        plan.rulesLoaded = g_loaded;
        if (!a_actor || !g_loaded || !cfg.blocksEnabled ||
            SeverBio::GetStatus() != SeverBio::Status::Ready) {
            return plan;
        }

        const auto catalog = SeverBio::Catalog();
        plan.catalogReady  = SeverBio::CatalogReady();
        if (!plan.catalogReady) {
            return plan;
        }
        const auto findBlock = [&](const std::string& a_key) -> const SeverBio::Block* {
            for (const auto& b : catalog) {
                if (b.key == a_key) {
                    return &b;
                }
            }
            return nullptr;
        };

        const auto ref   = a_actor->GetFormID();
        const auto salt  = Salt(ref);
        const bool child = a_actor->IsChild();

        // What they carry already, from any provider. Shown to the model, and
        // a group they already have a block in is never added to.
        std::vector<SeverBio::Applied> carried;
        if (auto known = SeverBio::AppliedFor(ref)) {
            plan.appliedKnown = true;
            carried           = std::move(*known);
        }
        for (const auto& a : carried) {
            Choice c;
            c.key     = a.key;
            c.title   = a.title;
            c.tabs    = { a.tab };
            c.source  = a.direct ? "carried" : "faction rule";
            c.carried = true;
            plan.carried.push_back(std::move(c));
        }

        struct Rolled
        {
            std::size_t   group{};
            std::uint32_t priority{};
            int           index{ -1 };     // into the group's candidates
            std::string   source;
        };
        // One menu slot per key: entries naming the same block (a leaning by
        // race AND by faction) add their weights.
        struct Candidate
        {
            std::string key;
            int         weight{};
        };
        std::vector<Rolled>                    rolls;
        std::vector<std::vector<Candidate>>    candidates(g_groups.size());
        std::vector<std::vector<std::string>>  groupTabs(g_groups.size());
        std::vector<int>                       menuIndex(g_groups.size(), -1);

        for (std::size_t gi = 0; gi < g_groups.size(); ++gi) {
            const auto& g = g_groups[gi];

            // The tabs that mean "this group is decided": the rule file's,
            // plus wherever the group's blocks actually live now (the player
            // can move a block to another tab).
            auto tabs = g.tabs;
            bool intimate = g.intimate;
            for (const auto& e : g.entries) {
                if (const auto* b = findBlock(e.key)) {
                    if (!TabMatches(tabs, b->tab)) {
                        tabs.push_back(b->tab);
                    }
                    intimate = intimate || b->intimate;
                }
            }
            groupTabs[gi] = tabs;

            // Intimate content is opt-in, and never for a child - SeverActions
            // leaves such blocks out of a child's bio anyway.
            if (intimate && (!cfg.blocksIntimate || child)) {
                continue;
            }
            if (AnyHolds(g.unless, a_actor)) {
                continue;
            }
            const bool covered = std::any_of(carried.begin(), carried.end(),
                                             [&](const SeverBio::Applied& a) {
                                                 return TabMatches(tabs, a.tab);
                                             });
            if (covered) {
                continue;
            }

            const auto usable = [&](const Entry& e) {
                const auto* b = findBlock(e.key);
                if (!b) {
                    return false;   // the player removed the pack or the block
                }
                if (b->intimate && (!cfg.blocksIntimate || child)) {
                    return false;
                }
                return AllHold(e.when, a_actor) && !AnyHolds(e.unless, a_actor);
            };

            // Facts first: one fills the group, and the model never sees it
            // as a choice.
            const Entry* fact = nullptr;
            for (const auto& e : g.entries) {
                if (e.fact && usable(e)) {
                    fact = &e;
                    break;
                }
            }
            if (fact) {
                const auto* b = findBlock(fact->key);
                Choice      c;
                c.key    = fact->key;
                c.title  = b->title;
                c.group  = g.name;
                c.tabs   = tabs;
                c.source = "fact: " + (fact->when.empty() ? std::string{ "always" }
                                                           : Labels(fact->when));
                plan.facts.push_back(std::move(c));
                continue;
            }

            for (const auto& e : g.entries) {
                if (e.fact || !usable(e)) {
                    continue;
                }
                // `rarely` keeps an unlikely pairing reachable - the odd
                // cowardly guard - at a tenth of the odds, instead of ruling
                // it out.
                const int weight = AnyHolds(e.rarely, a_actor) && e.weight > 0
                                       ? (std::max)(1, e.weight / 10)
                                       : e.weight;
                auto&     slots  = candidates[gi];
                auto      it     = std::find_if(slots.begin(), slots.end(),
                                                [&](const Candidate& c) { return c.key == e.key; });
                if (it == slots.end()) {
                    slots.push_back({ e.key, weight });
                } else {
                    it->weight += weight;
                }
            }
            if (candidates[gi].empty()) {
                continue;
            }

            std::mt19937 rng{ Seed(ref, salt, g.name) };
            Rolled       r;
            r.group    = gi;
            r.priority = rng();

            // Weight 0 marks an option that is never rolled but is still on
            // the menu, for the model to add when the evidence names it.
            std::vector<int> weights;
            for (const auto& c : candidates[gi]) {
                weights.push_back(c.weight);
            }
            const int  chance = intimate ? cfg.blocksIntimateChance : g.chance;
            const bool any    = std::any_of(weights.begin(), weights.end(),
                                            [](int w) { return w > 0; });
            if (any && std::uniform_int_distribution<int>{ 0, 99 }(rng) < chance) {
                std::discrete_distribution<int> pick{ weights.begin(), weights.end() };
                r.index  = pick(rng);
                r.source = "rolled";
            }
            rolls.push_back(std::move(r));

            // Every open group is a menu: a rolled one for the model to keep,
            // replace or drop, the rest for it to add to when the evidence
            // names an option outright.
            Menu m;
            m.group = g.name;
            m.tabs  = tabs;
            for (const auto& c : candidates[gi]) {
                m.keys.push_back(c.key);
                m.titles.push_back(findBlock(c.key)->title);
            }
            menuIndex[gi] = static_cast<int>(plan.menus.size());
            plan.menus.push_back(std::move(m));
        }

        // The flavour cap. Groups compete in a seeded order rather than file
        // order, so the last groups in the file are not always the ones to
        // lose out.
        std::sort(rolls.begin(), rolls.end(), [](const Rolled& a, const Rolled& b) {
            return a.priority < b.priority;
        });
        int kept = 0;
        for (auto& r : rolls) {
            if (r.index < 0) {
                continue;
            }
            if (kept >= plan.maxFlavour) {
                r.index = -1;
                continue;
            }
            ++kept;

            const auto& g     = g_groups[r.group];
            const auto& entry = candidates[r.group][r.index];
            Choice      c;
            c.key    = entry.key;
            c.title  = findBlock(entry.key)->title;
            c.group  = g.name;
            c.tabs   = groupTabs[r.group];
            c.source = r.source;
            plan.flavour.push_back(std::move(c));

            if (menuIndex[r.group] >= 0) {
                plan.menus[menuIndex[r.group]].defaultIndex = r.index;
            }
        }
        return plan;
    }

    std::string ShortTitle(std::string_view a_title)
    {
        const auto colon = a_title.rfind(':');
        if (colon == std::string_view::npos) {
            return std::string{ Trim(a_title) };
        }
        const auto tail = Trim(a_title.substr(colon + 1));
        return tail.empty() ? std::string{ Trim(a_title) } : std::string{ tail };
    }

    std::string FixedForPrompt(const Plan& a_plan)
    {
        std::string out;
        const auto  add = [&](const Choice& c) {
            out += "- " + c.title;
            if (!c.key.empty()) {
                if (auto block = SeverBio::FindBlock(c.key); block && !block->content.empty()) {
                    out += ": " + Flatten(block->content, 220);
                }
            }
            out += '\n';
        };
        for (const auto& c : a_plan.carried) {
            add(c);
        }
        for (const auto& c : a_plan.facts) {
            add(c);
        }
        if (!out.empty()) {
            out.pop_back();
        }
        return out;
    }


    std::string ReviewForPrompt(const Plan& a_plan)
    {
        if (a_plan.menus.empty() || a_plan.maxFlavour <= 0) {
            return {};
        }

        const auto options = [](const Menu& m) {
            std::string out;
            for (std::size_t i = 0; i < m.keys.size(); ++i) {
                out += std::format("{}{}. {}", i ? "; " : "", i + 1, ShortTitle(m.titles[i]));
            }
            return out;
        };

        std::string rolled;
        std::string open;
        for (const auto& m : a_plan.menus) {
            if (m.defaultIndex >= 0) {
                const auto& key = m.keys[m.defaultIndex];
                rolled += std::format("- {}: rolled {}. {}", m.group, m.defaultIndex + 1,
                                      ShortTitle(m.titles[m.defaultIndex]));
                if (auto block = SeverBio::FindBlock(key); block && !block->content.empty()) {
                    rolled += " - " + Flatten(block->content, 200);
                }
                rolled += "\n  options: " + options(m) + "\n";
            } else {
                open += std::format("- {}: {}\n", m.group, options(m));
            }
        }

        std::string out = std::format(
            "At most {} of these traits in all (the traits shown alongside do not count).\n",
            a_plan.maxFlavour);
        if (!rolled.empty()) {
            out += "\nRolled for this person - keep each unless the evidence contradicts it:\n";
            out += rolled;
        }
        if (!open.empty()) {
            out += "\nNot rolled - add one only when the evidence names it outright:\n";
            out += open;
        }
        out.pop_back();
        return out;
    }

    std::vector<Choice> Resolve(const Plan& a_plan, std::string_view a_section, std::string& a_note)
    {
        a_note.clear();

        // "religion: keep" / "- Religion: 3 (prays to Dibella)" / "phobias: drop".
        struct Answer
        {
            std::string value;    // as written, reason stripped
            std::string reason;   // the bracketed part, if any
        };
        std::map<std::string, Answer> answers;
        std::size_t                   pos = 0;
        while (pos <= a_section.size()) {
            const auto end  = a_section.find('\n', pos);
            auto       line = Trim(a_section.substr(pos, end == std::string_view::npos
                                                             ? std::string_view::npos
                                                             : end - pos));
            pos = end == std::string_view::npos ? a_section.size() + 1 : end + 1;
            while (!line.empty() && (line.front() == '-' || line.front() == '*')) {
                line = Trim(line.substr(1));
            }
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            Answer a;
            auto   value = Trim(line.substr(colon + 1));
            if (const auto paren = value.find('('); paren != std::string_view::npos) {
                auto reason = Trim(value.substr(paren + 1));
                if (!reason.empty() && reason.back() == ')') {
                    reason.remove_suffix(1);
                }
                a.reason = std::string{ Trim(reason) };
                value    = Trim(value.substr(0, paren));
            }
            while (!value.empty() && (value.back() == '.' || value.back() == ',')) {
                value.remove_suffix(1);
            }
            a.value = std::string{ value };
            answers[Lower(Trim(line.substr(0, colon)))] = std::move(a);
        }

        const auto addNote = [&](const std::string& a_text) {
            a_note += (a_note.empty() ? "" : "; ");
            a_note += a_text;
        };

        enum class How
        {
            Kept,
            Replaced,
            Added
        };
        struct Settled
        {
            const Menu* menu{};
            int         index{ -1 };
            How         how{ How::Kept };
            std::string reason;
        };
        std::vector<Settled> settled;

        for (const auto& m : a_plan.menus) {
            const bool rolled = m.defaultIndex >= 0;
            const auto it     = answers.find(m.group);
            if (it == answers.end()) {
                if (rolled) {
                    settled.push_back({ &m, m.defaultIndex, How::Kept, {} });
                }
                continue;
            }

            const auto answer = Lower(it->second.value);
            int        index  = -2;   // -2: not understood
            if (answer == "keep" || answer == "kept") {
                index = m.defaultIndex;
            } else if (answer == "none" || answer == "drop" || answer == "remove" ||
                       answer == "-" || answer.empty()) {
                index = -1;
            } else if (std::all_of(answer.begin(), answer.end(),
                                   [](unsigned char ch) { return std::isdigit(ch); })) {
                const int n = std::atoi(answer.c_str());
                if (n >= 1 && n <= static_cast<int>(m.keys.size())) {
                    index = n - 1;
                }
            } else {
                for (std::size_t i = 0; i < m.keys.size(); ++i) {
                    if (answer == m.keys[i] || answer == Lower(m.titles[i]) ||
                        answer == Lower(ShortTitle(m.titles[i]))) {
                        index = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (index == -2) {
                addNote(m.group + ": '" + it->second.value + "' is not an option" +
                        (rolled ? ", kept the roll" : ", nothing added"));
                index = m.defaultIndex;
            }
            if (index < 0) {
                if (rolled) {
                    addNote(m.group + ": dropped " + ShortTitle(m.titles[m.defaultIndex]) +
                            (it->second.reason.empty() ? "" : " (" + it->second.reason + ")"));
                }
                continue;
            }
            const How how = !rolled ? How::Added : index == m.defaultIndex ? How::Kept : How::Replaced;
            settled.push_back({ &m, index, how, it->second.reason });
        }
        if (answers.empty() && std::any_of(a_plan.menus.begin(), a_plan.menus.end(),
                                           [](const Menu& m) { return m.defaultIndex >= 0; })) {
            addNote("no bio_blocks answer, kept the rolls");
        }

        // The cap holds whatever the model did. The rolled groups were already
        // within it, so kept and replaced traits come first and an addition
        // only fits where a roll was dropped or never landed.
        std::stable_sort(settled.begin(), settled.end(), [](const Settled& a, const Settled& b) {
            return a.how != How::Added && b.how == How::Added;
        });

        std::vector<Choice> out;
        for (const auto& f : a_plan.facts) {
            out.push_back(f);
        }
        int kept = 0;
        for (const auto& s : settled) {
            const auto title = ShortTitle(s.menu->titles[s.index]);
            if (kept >= a_plan.maxFlavour) {
                addNote(s.menu->group + ": " + title + " over the cap of " +
                        std::to_string(a_plan.maxFlavour) + ", left out");
                continue;
            }
            ++kept;
            const auto why = s.reason.empty() ? std::string{ "the evidence" } : s.reason;
            Choice     c;
            c.key   = s.menu->keys[s.index];
            c.title = s.menu->titles[s.index];
            c.group = s.menu->group;
            c.tabs  = s.menu->tabs;
            switch (s.how) {
            case How::Kept:
                c.source = "rolled";
                break;
            case How::Replaced:
                c.source = "replaced the roll: " + why;
                addNote(s.menu->group + ": " +
                        ShortTitle(s.menu->titles[s.menu->defaultIndex]) + " -> " + title);
                break;
            case How::Added:
                c.source = "added: " + why;
                addNote(s.menu->group + ": added " + title);
                break;
            }
            out.push_back(std::move(c));
        }
        return out;
    }
}
