#include "Lotd.h"

namespace FUI::Lotd
{
    namespace
    {
        // The 47 sections whose slot list and item list agree on size, taken
        // from LegacyoftheDragonborn.esm itself rather than from a wiki. Two of
        // them are named DBM_LIST... instead of DBM_Section... -- LOTD's own
        // naming, not a typo here.
        constexpr const char* kSections[] = {
            "DBM_SectionLibraryUpperFloor",
            "DBM_SectionNSFossils",
            "DBM_SectionLibraryLowerFloorLeft",
            "DBM_SectionLibraryLowerFloorRight",
            "DBM_SectionNSGemstone",
            "DBM_SectionHOOMainFloor",
            "DBM_SectionHOHUpperGallery",
            "DBM_SectionDBHallAchievements",
            "DBM_SectionHOHJewelry",
            "DBM_SectionNSShells",
            "DBM_SectionDaedricGallery",
            "DBM_SectionHOSDisplays",
            "DBM_SectionArmoryExtraDisplays",
            "DBM_SectionLibraryMaps",
            "DBM_SectionLibraryRareBooks",
            "DBM_SectionHOLEUpperRing",
            "DBM_SectionGuildhouse",
            "DBM_SectionHOHGroundFloorLeft",
            "DBM_SectionHOHGroundFloorRight",
            "DBM_SectionHOHReceptionHall",
            "DBM_SectionNaturalScienceAnimals",
            "DBM_SectionHOHCultureandArts",
            "DBM_SectionHOHMasksAndClaws",
            "DBM_SectionArmoryGuardArmorDisplay",
            "DBM_SectionArmoryFalmer",
            "DBM_SectionHOLEMainFloor",
            "DBM_SectionArmoryThaneWeapons",
            "DBM_SectionArmoryAncientNord",
            "DBM_SectionArmorySteel",
            "DBM_SectionArmoryDragon",
            "DBM_SectionArmoryElven",
            "DBM_SectionArmoryDawnguard",
            "DBM_SectionArmoryStalhrim",
            "DBM_SectionStoreRoomReserveVintages",
            "DBM_SectionArmoryDwarven",
            "DBM_SectionArmoryIron",
            "DBM_SectionArmoryDaedric",
            "DBM_SectionArmoryEbony",
            "DBM_SectionArmoryGlass",
            "DBM_SectionArmoryNordic",
            "DBM_SectionArmoryOrcish",
            "DBM_SectionArmorySnowElf",
            "DBM_SectionArmoryForsworn",
            "DBM_SectionArmoryBlades",
            "DBM_LISTQuestDisplayDawnguard",
            "DBM_LISTQuestDisplayCivilWar",
            "DBM_SectionToolStorage",
        };

        struct Entry
        {
            RE::ObjectRefHandle slot;
            bool                donated = false;
        };

        std::unordered_map<RE::FormID, Entry> g_index;
        std::size_t                           g_lastShown = static_cast<std::size_t>(-1);

        // ★A nested FormList means ONE pedestal that accepts several variants
        // (77 of them). Every variant maps to the same slot, so donating any of
        // them lights that slot for all of them -- which is exactly what the
        // museum does.
        void AddItem(RE::TESForm* a_item, const RE::ObjectRefHandle& a_slot,
                     int a_depth, std::size_t& a_nested)
        {
            if (!a_item) return;
            if (auto* list = a_item->As<RE::BGSListForm>()) {
                // depth guard: a self-referencing list would otherwise recurse
                // until the stack gives out, and we do not own this data
                if (a_depth >= 4) return;
                ++a_nested;
                for (auto* sub : list->forms) {
                    AddItem(sub, a_slot, a_depth + 1, a_nested);
                }
                return;
            }
            // ★FIRST SLOT WINS, deliberately. A handful of forms appear under
            // more than one pedestal; keeping the first is a stable rule, and
            // the alternative -- holding every slot per form so that "any one
            // filled" counts -- costs a vector per entry to settle a case the
            // player is unlikely to meet. Revisit only if a report names it.
            g_index.try_emplace(a_item->GetFormID(), Entry{ a_slot, false });
        }
    }

    void Clear()
    {
        g_index.clear();
        g_lastShown = static_cast<std::size_t>(-1);
    }

    std::size_t Size() { return g_index.size(); }

    void Rebuild()
    {
        Clear();

        // LOTD absent -> one line, no noise. Its master is the surest witness:
        // every section above lives in it.
        if (!RE::TESForm::LookupByEditorID("DBM_SectionHOHMasksAndClaws")) {
            logger::info("[LOTD] not installed -- museum marks are off");
            return;
        }

        std::size_t sections = 0, slots = 0, nested = 0;
        std::size_t missing = 0, mismatched = 0, notRefr = 0;

        for (const char* name : kSections) {
            auto* slotList = RE::TESForm::LookupByEditorID<RE::BGSListForm>(name);
            auto* itemList =
                RE::TESForm::LookupByEditorID<RE::BGSListForm>(std::string(name) + "Items");
            // A section missing entirely is not an error -- it means an LOTD
            // add-on the user does not have.
            if (!slotList || !itemList) { ++missing; continue; }

            const std::size_t n = slotList->forms.size();
            if (n != itemList->forms.size()) {
                logger::warn("[LOTD] {}: slots={} items={} -- sizes disagree, skipped",
                             name, n, itemList->forms.size());
                ++mismatched;
                continue;
            }

            ++sections;
            for (std::size_t i = 0; i < n; ++i) {
                auto* form = slotList->forms[i];
                auto* ref  = form ? form->As<RE::TESObjectREFR>() : nullptr;
                if (!ref) { ++notRefr; continue; }
                AddItem(itemList->forms[i], ref->CreateRefHandle(), 0, nested);
                ++slots;
            }
        }

        Refresh();

        logger::info("[LOTD] indexed {} form(s) from {} section(s), {} slot(s), "
                     "{} nested list(s)",
                     g_index.size(), sections, slots, nested);
        if (missing || mismatched || notRefr) {
            logger::info("[LOTD] skipped: {} section(s) absent, {} size mismatch, "
                         "{} non-reference slot(s)", missing, mismatched, notRefr);
        }
    }

    void Refresh()
    {
        if (g_index.empty()) return;

        std::size_t shown = 0;
        for (auto& [id, e] : g_index) {
            // ★The handle, resolved here and nowhere else. A raw pointer kept
            // across a load would outlive the reference it names (원칙 2).
            const auto ref = e.slot.get();
            e.donated = ref && !ref->IsDisabled();
            if (e.donated) ++shown;
        }

        // ★Only when it MOVES. This runs on every inventory open, and a line
        // per open would bury everything else in the log.
        if (shown != g_lastShown) {
            logger::info("[LOTD] donated: {} / {}", shown, g_index.size());
            g_lastShown = shown;
        }
    }

    Status Of(RE::FormID a_base)
    {
        if (g_index.empty()) return Status::kNotRelic;
        const auto it = g_index.find(a_base);
        if (it == g_index.end()) return Status::kNotRelic;
        return it->second.donated ? Status::kDonated : Status::kUndonated;
    }
}
