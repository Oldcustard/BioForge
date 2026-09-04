#include "pch.h"

#include "Placement.h"

#include <RE/B/BGSPackageDataLocation.h>
#include <RE/P/PackageLocation.h>
#include <RE/T/TESCustomPackageData.h>

namespace BioForge::Placement
{
    namespace
    {
        // A long package list is usually a quest NPC's scaffolding rather than
        // a routine, and the prompt pays for every line it is handed.
        constexpr std::size_t kMaxRoutineLines = 8;

        // What a package actually MAKES someone do.
        //
        // Not readable the obvious way. `GetObjectTypeName()` reports
        // `packData.packType`, which is `kPackage` (18) for anything built on a
        // template - and nearly every modern package is, so every line came out
        // reading "Package at Hall of Kyne". Following templateParent does not
        // help either: the template record reports `kPackageTemplate` (19).
        // The template's EDITOR ID would say "Sandbox" outright, but package
        // editor IDs are empty at runtime (see the note in CLAUDE.md).
        //
        // So the template is identified by FORM ID. These are all Skyrim.esm,
        // which is always load-order index 0, so the runtime IDs are the record
        // IDs unchanged and no lookup is needed. Verbs carry their own
        // preposition because "patrols at" is not English.
        struct TemplateVerb
        {
            std::uint32_t formID;
            const char*   verb;
        };

        // Daily-life templates only. This doubles as a WHITELIST: a package
        // built on anything else (UseWeapon, ForceGreet, Flee, Say, the dragon
        // and Civil War scaffolding) says nothing about where someone lives or
        // works, and a wasted line here is paid for by every bio generated.
        constexpr TemplateVerb kRoutineTemplates[] = {
            { 0x0001C254, "sandboxes at" },              // Sandbox
            { 0x00068B86, "sandboxes at" },              // SandboxMultiLocation
            { 0x0006434E, "sandboxes at" },              // SandboxLockOnArrival
            { 0x0006C872, "sandboxes at" },              // SandboxAndKeepEyeOn
            { 0x0005FAD4, "sandboxes and stands guard at" },   // SandboxAndGuard
            { 0x000604EE, "works at" },                  // SandboxWorking
            { 0x0006C873, "works at" },                  // SandboxWorkingMultiLocation
            { 0x00019717, "sleeps at" },                 // Sleep
            { 0x000DD745, "sleeps at" },                 // SleepAndGuard
            { 0x00019714, "eats at" },                   // Eat
            { 0x00019715, "sits at" },                   // Sit
            { 0x00016FAA, "travels to" },                // Travel
            { 0x00098EB2, "travels to" },                // TravelBackAndForth
            { 0x0009BD85, "travels to" },                // TravelAndGuard
            { 0x0009BD86, "travels to" },                // TravelBackAndForthAndGuard
            { 0x000F1A3F, "travels to" },                // TravelToRadius
            { 0x000C8EA5, "travels to" },                // TravelVariableSpeed
            { 0x000FD814, "travels to" },                // TravelAndUseIdle
            { 0x00017723, "patrols around" },            // Patrol
            { 0x0001722F, "patrols around" },            // PatrolStaticPathing
            { 0x0001CA00, "stands guard at" },           // GuardPatrol
            { 0x0001C9FF, "stands guard at" },           // GuardPost
            { 0x000283F0, "spends time at" },            // UseIdleMarker
            { 0x00082571, "stays at" },                  // StayAtCurrentLocation
            { 0x000503D0, "holds a post at" },           // HoldPosition
        };

        // Verb for this package, or "" when its template is not a daily-life
        // one. Templates can be built on templates, so walk a few levels.
        const char* VerbFor(RE::TESPackage* a_package)
        {
            auto* package = a_package;
            for (int hop = 0; package && hop < 4; ++hop) {
                for (const auto& known : kRoutineTemplates) {
                    if (package->GetFormID() == known.formID) {
                        return known.verb;
                    }
                }
                auto* custom = skyrim_cast<RE::TESCustomPackageData*>(package->data);
                package      = custom ? custom->templateParent : nullptr;
            }
            return nullptr;
        }

        // Location records are often named with their own scaffolding still
        // attached - "Riften Bathhouse Location" - which reads as a filename
        // rather than a place. Only the suffix goes; the name is otherwise the
        // author's own words and none of our business.
        std::string Tidy(std::string a_name)
        {
            constexpr std::string_view kSuffix = " Location"sv;
            if (a_name.size() > kSuffix.size() && a_name.ends_with(kSuffix)) {
                a_name.resize(a_name.size() - kSuffix.size());
            }
            return a_name;
        }

        // Name a package target we can actually put in a sentence. The target
        // is a plain TESForm whose shape depends on the location type, so this
        // only ever runs after locType has been checked.
        std::string NameOfForm(RE::TESForm* a_form)
        {
            if (!a_form) {
                return {};
            }

            // The cell is the common case and the useful one: a sandbox
            // package pointed at an interior resolves to the room's own name
            // ("Hall of Kyne"), which beats any editor ID we could have read.
            if (auto* cell = a_form->As<RE::TESObjectCELL>()) {
                if (const char* name = cell->GetFullName(); name && *name) {
                    return Tidy(name);
                }
            }
            if (auto* location = a_form->As<RE::BGSLocation>()) {
                if (const char* name = location->GetFullName(); name && *name) {
                    return Tidy(name);
                }
            }
            if (auto* ref = a_form->As<RE::TESObjectREFR>()) {
                // A reference target is only worth naming when it is a PERSON:
                // "travels to Hadring" says something, "eats at Bench" does
                // not. Package targets are overwhelmingly furniture and idle
                // markers, and their names are set dressing - a first cut
                // filtered on the word "marker" and still produced "eats at
                // Wooden Stool", "eats at Cooking Pot" and an archery trainer
                // written up as spending his days on the benches. What the
                // player would actually say about any of them is the ROOM.
                if (ref->As<RE::Actor>()) {
                    if (const char* name = ref->GetDisplayFullName(); name && *name) {
                        return Tidy(name);
                    }
                }
                if (auto* cell = ref->GetParentCell()) {
                    if (const char* name = cell->GetFullName(); name && *name) {
                        return Tidy(name);
                    }
                }
            }
            return {};
        }

        // The place one package sends them, or "" when it names none.
        //
        // TESPackage::data is a bare vtable in CommonLib; the concrete type
        // carrying the slot list is TESCustomPackageData, hence the RTTI cast
        // rather than a reinterpret. Slot 0 holds the location for every
        // package template that has one, but the list is walked rather than
        // indexed: a template that reorders its inputs would read garbage.
        std::string PlaceOf(RE::TESPackage* a_package)
        {
            auto* custom = skyrim_cast<RE::TESCustomPackageData*>(a_package->data);
            if (!custom || !custom->data.data) {
                return {};
            }

            for (std::uint16_t i = 0; i < custom->data.dataSize; ++i) {
                auto* slot = custom->data.data[i];
                if (!slot) {
                    continue;
                }
                auto* asLocation = skyrim_cast<RE::BGSPackageDataLocation*>(slot);
                if (!asLocation || !asLocation->pointer) {
                    continue;
                }

                const auto* target = asLocation->pointer;
                switch (target->locType.get()) {
                case RE::PackageLocation::Type::kInCell:
                case RE::PackageLocation::Type::kAtPackagelocation:
                case RE::PackageLocation::Type::kAlias_Location:
                    // data is a union - only safe to read as a form once the
                    // type above says that is what it holds.
                    return NameOfForm(target->data.object);

                case RE::PackageLocation::Type::kNearReference:
                case RE::PackageLocation::Type::kNearLinkedReference:
                case RE::PackageLocation::Type::kAlias_Reference:
                    if (const auto ref = target->data.refHandle.get()) {
                        return NameOfForm(ref.get());
                    }
                    return {};

                case RE::PackageLocation::Type::kNearEditorLocation:
                    // "Sleep at editor location" is the commonest home marker
                    // in the game. Phrased as a place rather than a clause so
                    // every verb above still reads as English in front of it.
                    return "the spot where the game placed them";

                default:
                    break;   // kObjectID / kObjectType name a thing, not a place
                }
            }
            return {};
        }

        void AppendPackage(RE::TESPackage* a_package, bool a_requireKnownVerb,
                           std::vector<std::string>& a_out)
        {
            if (!a_package || a_out.size() >= kMaxRoutineLines) {
                return;
            }

            const char* verb = VerbFor(a_package);
            if (!verb) {
                if (a_requireKnownVerb) {
                    return;
                }
                verb = "goes to";   // custom template: say the place, not the verb
            }

            const auto place = PlaceOf(a_package);
            if (place.empty()) {
                return;   // a package naming no place says nothing useful here
            }

            auto line = "- Routine: " + std::string{ verb } + " " + place;
            if (std::find(a_out.begin(), a_out.end(), line) == a_out.end()) {
                a_out.push_back(std::move(line));
            }
        }

        // Every package the base record carries, its own and its list's.
        std::vector<RE::TESPackage*> PackagesOf(RE::TESNPC* a_base)
        {
            std::vector<RE::TESPackage*> packages;
            for (auto* package : a_base->aiPackages.packages) {
                packages.push_back(package);
            }
            // NPCs driven by a package LIST rather than their own packages -
            // common for guards and generic townsfolk.
            if (a_base->defaultPackList) {
                a_base->defaultPackList->ForEachForm([&](RE::TESForm* a_form) {
                    if (auto* package = a_form ? a_form->As<RE::TESPackage>() : nullptr) {
                        packages.push_back(package);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            return packages;
        }
    }

    std::string RoutineOf(std::uint32_t a_refFormID)
    {
        std::string              home;
        std::vector<std::string> routine;

        if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refFormID)) {
            // Where the reference was PLACED, which for a static NPC is home.
            // This is the runtime editorLocation the engine maintains, not the
            // reference record's own location field - that one is usually
            // empty even for an NPC with a perfectly clear home.
            if (auto* editor = ref->GetEditorLocation()) {
                if (const char* name = editor->GetFullName(); name && *name) {
                    home = std::string{ "- Home, where the game placed them: " } + name;
                }
            }

            if (auto* actor = ref->As<RE::Actor>()) {
                if (auto* base = actor->GetActorBase()) {
                    const auto packages = PackagesOf(base);

                    // Daily-life packages first and on their own. Only if the
                    // NPC has none - every package built on a template we do
                    // not know - fall back to naming the places without a verb,
                    // so a mod's custom template still says something.
                    for (auto* package : packages) {
                        AppendPackage(package, true, routine);
                    }
                    if (routine.empty()) {
                        for (auto* package : packages) {
                            AppendPackage(package, false, routine);
                        }
                    }
                }
            }
        }

        if (home.empty() && routine.empty()) {
            // Say the absence out loud. A missing section reads as "no
            // information", and the model fills that from where they are
            // standing - which is the whole failure this exists to stop.
            return "- Nothing in this character's record says where they live or work.";
        }

        std::string out{ home };
        for (const auto& line : routine) {
            if (!out.empty()) {
                out += '\n';
            }
            out += line;
        }
        return out;
    }
}
