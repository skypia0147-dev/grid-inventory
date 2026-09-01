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

        // ★★★SOURCE (2) -- see the header. Local ids in kCurator, resolved the
        // same way as the sections: form id first, EditorID second.
        constexpr const char* kCurator = "DBM_RelicNotifications.esp";
        enum : std::size_t { kMaster = 0, kDisp, kFound, kNew, kListCount };
        constexpr struct { std::uint32_t id; const char* name; } kCuratorLists[] = {
            { 0x609634, "dbmMaster" },
            { 0x558287, "dbmDisp"   },
            { 0x558286, "dbmFound"  },
            { 0x558285, "dbmNew"    },
        };

        // ★These are static forms, not references -- they live as long as the
        // load order does, which is why they may be held (원칙 2 is about
        // Actor/TESObjectREFR). Cleared at the session boundary all the same.
        RE::BGSListForm* g_cur[kListCount] = {};
        std::size_t      g_curSizes[kListCount] = {
            static_cast<std::size_t>(-1), static_cast<std::size_t>(-1),
            static_cast<std::size_t>(-1), static_cast<std::size_t>(-1),
        };
        std::unordered_map<RE::FormID, bool> g_curator;  // form -> donated

        // Defensive about nesting the way AddItem is: this list is consolidated
        // by somebody else's script and we do not own its shape.
        void AddCuratorForm(RE::TESForm* a_form, bool a_donated, int a_depth)
        {
            if (!a_form) return;
            if (auto* list = a_form->As<RE::BGSListForm>()) {
                if (a_depth >= 4) return;
                for (auto* sub : list->forms) {
                    AddCuratorForm(sub, a_donated, a_depth + 1);
                }
                return;
            }
            // ★Master is walked first with false, then Disp with true, so this
            // must never demote: a form in both lists IS donated.
            auto& donated = g_curator[a_form->GetFormID()];
            donated = donated || a_donated;
        }

        void RefreshCurator()
        {
            if (!g_cur[kMaster] || !g_cur[kDisp]) return;

            // ★Sizes first. This runs on every inventory open, and rebuilding
            // a few thousand entries to learn that nothing moved is the kind of
            // work that only shows up on somebody else's machine.
            std::size_t now[kListCount] = {};
            bool        moved = false;
            for (std::size_t i = 0; i < kListCount; ++i) {
                now[i] = g_cur[i] ? g_cur[i]->forms.size() : 0;
                if (now[i] != g_curSizes[i]) moved = true;
            }
            if (!moved) return;
            std::copy(std::begin(now), std::end(now), std::begin(g_curSizes));

            g_curator.clear();
            for (auto* f : g_cur[kMaster]->forms) AddCuratorForm(f, false, 0);
            for (auto* f : g_cur[kDisp]->forms)   AddCuratorForm(f, true, 0);

            std::size_t donated = 0, novel = 0;
            for (const auto& [id, d] : g_curator) {
                if (d) ++donated;
                // ★The number this whole source exists for: forms the 47
                // sections never held. It is what an add-on brought, and it is
                // the one figure that says whether reading (2) was worth it.
                if (!g_index.contains(id)) ++novel;
            }
            // ★All four sizes, because the fourth is a free self-test: its own
            // MCM holds them to new + found + disp == master, so a line where
            // they do not add up says the lists were read mid-fill.
            logger::info("[LOTD] curator: {} relic(s), {} donated, {} beyond the "
                         "sections (master={} new={} found={} disp={})",
                         g_curator.size(), donated, novel,
                         now[kMaster], now[kNew], now[kFound], now[kDisp]);
        }
    }

    void Clear()
    {
        g_index.clear();
        g_lastShown = static_cast<std::size_t>(-1);
        g_curator.clear();
        for (auto*& p : g_cur) p = nullptr;
        for (auto& s : g_curSizes) s = static_cast<std::size_t>(-1);
    }

    std::size_t Size() { return g_index.size(); }

    std::size_t CuratorSize() { return g_curator.size(); }

    void Rebuild()
    {
        Clear();

        // ★LOTD absent -> one line, no noise. Asked of the LOAD ORDER, which
        // is the only witness that answers the question actually being asked.
        // It used to be a lookup of one section's EditorID, and that conflated
        // "LOTD is not here" with "EditorIDs are not here" -- two very
        // different situations reported as the same sentence.
        auto* dh   = RE::TESDataHandler::GetSingleton();
        auto* file = dh ? dh->LookupModByName(kPlugin) : nullptr;
        if (!file) {
            logger::info("[LOTD] not installed -- museum marks are off");
            return;
        }
        // ★And the case LookupModByName alone cannot tell apart -- see the
        // longer note at kCurator. Present on disk, unchecked in the load
        // order: an entry answers, every form behind it does not.
        if (file->GetCompileIndex() == 0xFF) {
            logger::warn("[LOTD] {} is installed but NOT ENABLED in the load "
                         "order -- museum marks are off", kPlugin);
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

        // ★Source (2), resolved once -- the lists themselves are empty on disk,
        // so WHAT they hold is read in Refresh(). Master and Disp are the two
        // that must answer; Found and New are carried for the log line only.
        auto* curFile = dh->LookupModByName(kCurator);
        // ★★LookupModByName answers for a file that EXISTS, not one that is
        // LOADED. A plugin left unchecked in the load order still has an entry
        // -- with compileIndex 0xFF and no forms behind it, so every lookup
        // returns null. Measured 2026-09-02: the first test run had exactly
        // that, and the warning below used to read "is loaded but did not
        // resolve", which sent the search into the form ids instead of the
        // load order. Say which one it is.
        if (curFile && curFile->GetCompileIndex() == 0xFF) {
            logger::warn("[LOTD] {} is installed but NOT ENABLED in the load "
                         "order -- add-on relics are off", kCurator);
        } else if (curFile) {
            for (std::size_t i = 0; i < kListCount; ++i) {
                g_cur[i] = dh->LookupForm<RE::BGSListForm>(kCuratorLists[i].id, kCurator);
                if (!g_cur[i]) {
                    g_cur[i] = RE::TESForm::LookupByEditorID<RE::BGSListForm>(
                        kCuratorLists[i].name);
                }
            }
            if (g_cur[kMaster] && g_cur[kDisp]) {
                logger::info("[LOTD] Curator's Companion present -- add-on relics "
                             "will be read from its moreHUD lists");
            } else {
                logger::warn("[LOTD] {} is loaded but dbmMaster/dbmDisp did not "
                             "resolve -- add-on relics are off", kCurator);
                for (auto*& p : g_cur) p = nullptr;
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
        // ★Runs after Refresh(), so source (2) has already answered if it can:
        // with the Curator's Companion present the marks are NOT off, and
        // saying so would send someone chasing an EditorID map that is fine.
        if (g_index.empty() && g_curator.empty()) {
            logger::warn("[LOTD] {} is loaded but no section resolved -- museum "
                         "marks are off", kPlugin);
        }
    }

    void Refresh()
    {
        // ★Source (2) does not depend on source (1) having found anything, so
        // it is swept even when no section resolved. It used to be one early
        // return for the whole function.
        RefreshCurator();

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
        // (1) first: a pedestal's enabled state is the donation itself.
        if (!g_index.empty()) {
            const auto it = g_index.find(a_base);
            if (it != g_index.end()) {
                return it->second.donated ? Status::kDonated : Status::kUndonated;
            }
        }
        // (2) second, and this is where a museum add-on's relics live -- the
        // sections above only ever held what ships in LOTD itself.
        if (!g_curator.empty()) {
            const auto it = g_curator.find(a_base);
            if (it != g_curator.end()) {
                return it->second ? Status::kDonated : Status::kUndonated;
            }
        }
        return Status::kNotRelic;
    }
}
