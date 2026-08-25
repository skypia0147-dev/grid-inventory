#include "Lotd.h"

namespace FUI::Lotd
{
    namespace
    {
        // ★★★RESOLVED BY FORM ID, NOT BY EDITOR ID, and that is not a
        // preference -- it is the difference between the feature working and
        // the feature lying about why it does not.
        //
        // LookupByEditorID reads the engine's runtime EditorID map, and base
        // Skyrim does not populate that map for FormLists. powerofthree's
        // Tweaks does, which is the only reason this ever worked here: the
        // test setup ships it. Without it every lookup below returned null,
        // Rebuild took its early exit, and the log said "not installed" about
        // a mod the player had plainly installed.
        //
        // The local ids are read out of LegacyoftheDragonborn.esm itself (47
        // pairs, every DBM_ FormList carrying LOTD's own index 0x06, no
        // overrides). They are safe to hard-code for the reason that usually
        // makes hard-coded ids unsafe: these are DISPLAY REFERENCES whose
        // enabled state lives in every player's save, so LOTD cannot renumber
        // them without breaking every collection ever made.
        //
        // ★The EditorID stays as a per-section FALLBACK. It costs one lookup
        // when the id already answered, and it is what would carry a renumber
        // we did not foresee -- or a section an LOTD add-on brings with it.
        struct Section
        {
            std::uint32_t slots;   // local FormID in kPlugin
            std::uint32_t items;
            const char*   name;    // EditorID: the fallback, and the log's word
        };
        constexpr const char* kPlugin = "LegacyoftheDragonborn.esm";
        constexpr Section kSections[] = {
            { 0x15294B, 0x03621D, "DBM_LISTQuestDisplayCivilWar" },
            { 0x15294C, 0x03621E, "DBM_LISTQuestDisplayDawnguard" },
            { 0x6F0124, 0x161CE6, "DBM_SectionArmoryAncientNord" },
            { 0x6F0123, 0x161CE7, "DBM_SectionArmoryBlades" },
            { 0x6C22C0, 0x161CE8, "DBM_SectionArmoryDaedric" },
            { 0x6C22C4, 0x161CE9, "DBM_SectionArmoryDawnguard" },
            { 0x6C22C1, 0x161CEA, "DBM_SectionArmoryDragon" },
            { 0x6C22BD, 0x161CEB, "DBM_SectionArmoryDwarven" },
            { 0x6C22BF, 0x161CEC, "DBM_SectionArmoryEbony" },
            { 0x6C22BA, 0x161CED, "DBM_SectionArmoryElven" },
            { 0x1F4A6E, 0x161CEE, "DBM_SectionArmoryExtraDisplays" },
            { 0x6F0121, 0x161CEF, "DBM_SectionArmoryFalmer" },
            { 0x6F0122, 0x161CF0, "DBM_SectionArmoryForsworn" },
            { 0x6C22BB, 0x161CF1, "DBM_SectionArmoryGlass" },
            { 0x03B4C9, 0x08F99E, "DBM_SectionArmoryGuardArmorDisplay" },
            { 0x6C22B7, 0x161CF2, "DBM_SectionArmoryIron" },
            { 0x6C22B9, 0x161CF3, "DBM_SectionArmoryNordic" },
            { 0x6C22BC, 0x161CF4, "DBM_SectionArmoryOrcish" },
            { 0x161CE5, 0x161CF5, "DBM_SectionArmorySnowElf" },
            { 0x6C22BE, 0x161CF6, "DBM_SectionArmoryStalhrim" },
            { 0x6C22B8, 0x161CF7, "DBM_SectionArmorySteel" },
            { 0x161CE0, 0x161CE1, "DBM_SectionArmoryThaneWeapons" },
            { 0x152947, 0x152948, "DBM_SectionDBHallAchievements" },
            { 0x14849F, 0x1484A0, "DBM_SectionDaedricGallery" },
            { 0x066429, 0x06642A, "DBM_SectionGuildhouse" },
            { 0x161D31, 0x161D32, "DBM_SectionHOHCultureandArts" },
            { 0x1387AC, 0x1387AD, "DBM_SectionHOHGroundFloorLeft" },
            { 0x14CEF1, 0x14CEF2, "DBM_SectionHOHGroundFloorRight" },
            { 0x13D914, 0x13D915, "DBM_SectionHOHJewelry" },
            { 0x1387AA, 0x1387AB, "DBM_SectionHOHMasksAndClaws" },
            { 0x13D916, 0x13D917, "DBM_SectionHOHReceptionHall" },
            { 0x13D910, 0x13D911, "DBM_SectionHOHUpperGallery" },
            { 0x14D7DA, 0x14D7DB, "DBM_SectionHOLEMainFloor" },
            { 0x138798, 0x1387A5, "DBM_SectionHOLEUpperRing" },
            { 0x152943, 0x152944, "DBM_SectionHOOMainFloor" },
            { 0x15CBC3, 0x15CBC4, "DBM_SectionHOSDisplays" },
            { 0x1387A0, 0x1387A1, "DBM_SectionLibraryLowerFloorLeft" },
            { 0x13879A, 0x13879B, "DBM_SectionLibraryLowerFloorRight" },
            { 0x1387A3, 0x1387A4, "DBM_SectionLibraryMaps" },
            { 0x13879E, 0x13879F, "DBM_SectionLibraryRareBooks" },
            { 0x13879C, 0x13879D, "DBM_SectionLibraryUpperFloor" },
            { 0x0F0AC7, 0x0F0AC8, "DBM_SectionNSFossils" },
            { 0x415117, 0x415118, "DBM_SectionNSGemstone" },
            { 0x33D88B, 0x33D88C, "DBM_SectionNSShells" },
            { 0x15CBC1, 0x15CBC2, "DBM_SectionNaturalScienceAnimals" },
            { 0x0AE79C, 0x0AE79D, "DBM_SectionStoreRoomReserveVintages" },
            { 0x099DA3, 0x099DA4, "DBM_SectionToolStorage" },
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

        // ★LOTD absent -> one line, no noise. Asked of the LOAD ORDER, which
        // is the only witness that answers the question actually being asked.
        // It used to be a lookup of one section's EditorID, and that conflated
        // "LOTD is not here" with "EditorIDs are not here" -- two very
        // different situations reported as the same sentence.
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName(kPlugin)) {
            logger::info("[LOTD] not installed -- museum marks are off");
            return;
        }

        std::size_t sections = 0, slots = 0, nested = 0;
        std::size_t missing = 0, mismatched = 0, notRefr = 0, byName = 0;

        // ★Id first, EditorID second. The fallback is what would carry a
        // renumber, and it is also why byName is counted: if it ever starts
        // answering, the table has drifted and the log says so before anyone
        // has to guess.
        const auto resolve = [&](std::uint32_t a_local, std::string_view a_edid) {
            if (auto* f = dh->LookupForm<RE::BGSListForm>(a_local, kPlugin)) return f;
            auto* f = RE::TESForm::LookupByEditorID<RE::BGSListForm>(a_edid);
            if (f) ++byName;
            return f;
        };

        for (const auto& sec : kSections) {
            auto* slotList = resolve(sec.slots, sec.name);
            auto* itemList = resolve(sec.items, std::string(sec.name) + "Items");
            // A section missing entirely is not an error -- it means an LOTD
            // add-on the user does not have.
            if (!slotList || !itemList) { ++missing; continue; }

            const std::size_t n = slotList->forms.size();
            if (n != itemList->forms.size()) {
                logger::warn("[LOTD] {}: slots={} items={} -- sizes disagree, skipped",
                             sec.name, n, itemList->forms.size());
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
        // ★Only when it happens, and it should never happen: the ids answered
        // for every section, so this line means the table has drifted from the
        // plugin and the EditorID fallback carried it. Silence here is the
        // healthy state.
        if (byName) {
            logger::warn("[LOTD] {} list(s) resolved by EditorID -- the form-id "
                         "table has drifted from {}", byName, kPlugin);
        }
        // ★★And the case the old sentinel could not tell apart. LOTD is loaded
        // (checked above) but nothing resolved: on base Skyrim that is the
        // EditorID map missing, which is what powerofthree's Tweaks supplies.
        // Said plainly, because the alternative is a player told their museum
        // mod is not installed.
        if (g_index.empty()) {
            logger::warn("[LOTD] {} is loaded but no section resolved -- museum "
                         "marks are off", kPlugin);
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
