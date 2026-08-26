#include "ui/Equip.h"
#include "game/Ledger.h"

#include <SKSE/SKSE.h>
#include "ui/Fallback.h"
#include "game/Costume.h"
#include "game/DualRing.h"
#include "ui/Grid.h"
#include "ui/Loadout.h"
#include "ui/LootBarter.h"
#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>
#include <imgui_internal.h>   // RenderCheckMark (the costume tab marker)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Equipment doll, v9 mockup: vertical accessory strip (circlet + acc x4) +
// 3-column doll [earring/head/necklace, weapon/body/shield (tall), gaunt/
// boots/ammo, ringR/accM/ringL]. Every slot is 2x2 inventory cells; the
// middle row is 2x4. Empty slots show game-icons silhouettes (CC BY 3.0 —
// Lorc, Delapouite), rasterised white and tinted by the active skin.
// Equip/unequip logic ported from main.cpp's PrismaUI-era listeners.

namespace FUI::Equip
{
    namespace
    {
        // ---- accessory drawer state --------------------------------------
        // How many worn accessories the doll's five cells could not take.
        // Written by CollectEquipment, read by the tab and the panel.
        int   g_drawerCount = 0;
        bool  g_drawerOpen  = false;
        // 0 = shut, 1 = fully out. Rolled by DeltaTime so opening and closing
        // ride the same curve.
        float g_drawerSlide = 0.0f;

        struct SlotDef
        {
            const char* id;
            const char* icon;   // silhouette file: slot_<icon>.fic
        };

        // ★FOUR, not five. The strip's last cell is no longer a slot — it is
        // the drawer's button (see DrawDrawerCell). Trading one accessory cell
        // for the drawer is not a loss: whatever would have sat there is one
        // of the things the drawer holds anyway, and a control that lives in
        // the column it opens beats one bolted to the window frame.
        constexpr SlotDef kStrip[4] = {
            { "circlet", "circlet" }, { "acc1", "acc" }, { "acc2", "acc" },
            { "acc3", "acc" },
        };
        constexpr SlotDef kDoll[4][3] = {
            { { "earring", "earring" }, { "head", "head" },   { "necklace", "necklace" } },
            { { "weapon", "weapon" },   { "body", "body" },   { "shieldL", "shield" } },
            { { "gaunt", "gaunt" },     { "boots", "boots" }, { "ammo", "ammo" } },
            { { "ringR", "ring" },      { "accM", "acc" },    { "ringL", "ring" } },
        };

        struct EquipEntry
        {
            RE::TESBoundObject* obj = nullptr;
            int                 count = 1;
            std::uint8_t        glow = 0;   // rarity glow bits (Grid::GlowBits)
            // WHICH unit this slot is wearing. The doll used to hold a bare form
            // and re-resolve "the first worn list of that form" on every use, so
            // with a copy in each hand the two slots pointed at ONE list: the
            // right slot showed the left item's stats, lifting one unequipped the
            // other, and a swap could not tell them apart at all.
            std::uint16_t       uid = 0;
            std::uint16_t       sig = 0;
            int                 hand = 0;   // 0 none, 1 right, 2 left
        };

        // equip/unequip actions deferred to the game-update hook (Tick):
        // running them inside the render pass left the player's 3D stale
        // until the menu closed.
        struct PendingAction
        {
            RE::FormID    id = 0;
            std::string   slotId;
            bool          unequip = false;
            // GI1/D4: which sub-stack. 0/-1 = let the engine choose (the old
            // behaviour, still correct when no instance is in hand).
            std::uint16_t uid = 0;
            int           xlIdx = -1;
            // The content signature identifies units the engine gave no uid --
            // tempered and player-enchanted gear, i.e. exactly the copies a
            // player cares about equipping. Without it a drop onto a doll slot
            // fell back to uid 0 / xlIdx -1 and wore an arbitrary copy.
            std::uint16_t sig = 0;
            // Which TILE the item left. Rule 13 forgets exactly that cell -- the
            // one the player acted on -- rather than guessing among siblings.
            std::string   srcKey;
            // GI53: which hand the slot wears (0 none, 1 right, 2 left). An
            // unequip without it matched "the first worn list of this form",
            // so right-clicking the right slot could strip the LEFT copy when
            // both hands wore identical units.
            int           hand = 0;
            // How many units this action moves. 1 for everything worn a copy at
            // a time; a whole tile for ammo. (Appended last so the aggregate
            // initialisers elsewhere stay valid.)
            int           count = 1;
            // What the SOURCE TILE held at click time (0 = unknown/legacy).
            // Rule 13 asks "did this tile empty", and the form-wide count
            // cannot answer that when the form is split across tiles: draining
            // a pack potion with a second copy in the potion bag read cnt==1
            // and kept the emptied cell, whose stale layout claim then made
            // the reconciler eat the BAG's copy. (Appended last, same deal.)
            int           tileCount = 0;
        };
        std::vector<PendingAction> g_pending;
        int                        g_rebuildLag = 0;   // rebuild AFTER the queued task applied

        // L2 loadout tab UI state — shared between the tab strip (sets these) and
        // the confirm windows drawn at TOP LEVEL (like Settings). Rendering the
        // confirm inside the equip panel's child window crashed; a top-level
        // window is the proven pattern.
        bool  g_buyOpen = false;    // "+" purchase confirm window open
        int   g_delTarget = -1;     // preset index pending delete confirm (>=1)
        // ★How many accessory cells the doll took before the drawer starts.
        // Written by CollectEquipment from the array it actually used, read by
        // the drawer to build the same ids -- a literal in both places is how
        // the two drift and a whole cell's contents go missing.
        int   g_drawerFirstIdx = 4;
        // ★Which side the panel comes out of, decided by the panel and read by
        // the BUTTON, which sits in a child window and cannot see the main
        // window's rect to work it out. One frame stale at worst, and only
        // while a window drag is crossing the screen edge.
        bool  g_drawerToRight = false;

        float g_slotsTop = 0.0f;    // GI77: measured tab-strip height
        // ★★THE SAME ROW, IN SCREEN SPACE -- and it has to be measured
        // separately, not derived. g_slotsTop is relative to the window Draw()
        // ran in, and Draw() runs inside the "fab_left" CHILD, so adding it to
        // the MAIN window's origin lands a child-height too high. The drawer
        // sits outside both windows and can only speak screen coordinates, so
        // it gets the row's absolute y and no arithmetic to get wrong.
        float g_slotsTopScreen = 0.0f;

        // silhouettes: loaded once, kept for the process lifetime
        std::unordered_map<std::string, IconCache::Icon> g_silhouettes;

        // ★★TWO SETS, chosen by the skin. The ink skins get brush-drawn
        // silhouettes ("_ink"); everything else gets the flat ones. Both are
        // white + alpha and Equip tints them with a single colour, so the wash
        // sheet's TONE survives in the alpha channel -- that is the whole
        // reason a painted set can work here at all.
        // ★The suffix is part of the CACHE KEY, not just the path. Keying on
        // the slot name alone would serve whichever set was asked for first
        // for the rest of the session, and the skin can change at any time.
        const IconCache::Icon* Silhouette(const char* a_name)
        {
            std::string key = a_name;
            if (Theme::InkChrome()) key += "_ink";
            const auto it = g_silhouettes.find(key);
            if (it != g_silhouettes.end()) {
                return it->second.srv ? &it->second : nullptr;
            }
            IconCache::Icon icon;
            const std::string path =
                "Data/SKSE/Plugins/GridInventory_slots/slot_" + key + ".fic";
            IconCache::LoadFicTexture(path, icon);   // leaves srv null on failure
            // ★A missing "_ink" file falls back to the plain one rather than
            // leaving the slot blank: a set that is incomplete should look
            // unstyled, never empty.
            if (!icon.srv && key != a_name) {
                IconCache::LoadFicTexture(
                    std::string("Data/SKSE/Plugins/GridInventory_slots/slot_") + a_name + ".fic",
                    icon);
            }
            g_silhouettes[key] = icon;
            return icon.srv ? &g_silhouettes[key] : nullptr;
        }

        // ★The lowest biped slot the item occupies. See Equip.h for why this,
        // and not arrival order, decides which accessory cell it gets.
        // Bit N is slot 30+N -- the same reading the tooltip already does.
        int PrimarySlotOf(RE::TESBoundObject* a_obj)
        {
            constexpr int kNoSlot = 9999;   // sorts last
            const auto* biped = a_obj ? a_obj->As<RE::BGSBipedObjectForm>() : nullptr;
            if (!biped) return kNoSlot;
            const auto mask = static_cast<std::uint32_t>(biped->GetSlotMask().get());
            if (mask == 0) return kNoSlot;
            for (int bit = 0; bit < 32; ++bit) {
                if (mask & (1u << bit)) return 30 + bit;
            }
            return kNoSlot;
        }

        const char* SlotForArmor(RE::TESObjectARMO* a_armo)
        {
            using S = RE::BGSBipedObjectForm::BipedObjectSlot;
            // ★Ring test before any slot test: Grid::IsRing asks the item what
            // it IS, which outranks a guess made from the slot it happens to
            // sit on. A mod ring parked on kEars or kCirclet would otherwise
            // be filed as an earring and take that slot from a real one.
            if (Grid::IsRing(a_armo))           return "ringR";
            if (a_armo->HasPartOf(S::kBody))    return "body";
            if (a_armo->HasPartOf(S::kHead) || a_armo->HasPartOf(S::kHair)) return "head";
            if (a_armo->HasPartOf(S::kHands))   return "gaunt";
            if (a_armo->HasPartOf(S::kFeet))    return "boots";
            if (a_armo->HasPartOf(S::kShield))  return "shieldL";
            if (a_armo->HasPartOf(S::kAmulet))  return "necklace";
            if (a_armo->HasPartOf(S::kRing))    return "ringR";
            if (a_armo->HasPartOf(S::kCirclet)) return "circlet";
            if (a_armo->HasPartOf(S::kEars))    return "earring";
            return nullptr;
        }

        // Does this item belong in the slot it was dropped on?
        //
        // Without this the drop TARGET was ignored entirely: EquipItem only ever
        // read the slot id to spot "shieldL" for a one-hander, so a helmet dropped
        // on the boots slot still went on the head -- and the swap logic then
        // pulled the BOOTS off, handed them to the cursor, and they re-equipped
        // themselves the moment the player tried to put them down.
        bool SlotAccepts(RE::TESBoundObject* a_obj, const std::string& a_slotId)
        {
            if (!a_obj || a_slotId.empty()) return true;   // no target = engine picks
            if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
                const char* home = SlotForArmor(armo);
                if (!home) return a_slotId.rfind("acc", 0) == 0;   // odd armour -> accessory
                if (a_slotId == home) return true;
                // rings occupy either hand; the doll splits them into two slots
                if (std::string_view(home) == "ringR") {
                    // ★★The SECOND ring slot has rules of its own -- the first
                    // slot must be filled, and the two rings may not share an
                    // effect -- and they live in DualRing so that the drop
                    // target and the act that follows cannot disagree. A slot
                    // that accepts a drag and then does nothing is worse than
                    // one that refuses it.
                    if (a_slotId == "ringL") {
                        return DualRing::CanWear(armo) == DualRing::Verdict::kOk;
                    }
                    // ★...and past the second they SPILL into the accessory
                    // pool, so those are legal targets too. Refusing them made
                    // a spilled ring undroppable onto the very slot the doll
                    // had just chosen for it.
                    return a_slotId == "ringR" || a_slotId.rfind("acc", 0) == 0;
                }
                return false;
            }
            if (a_obj->Is(RE::FormType::Weapon) || a_obj->Is(RE::FormType::Light)) {
                // weapons take either hand; shieldL doubles as the left hand
                return a_slotId == "weapon" || a_slotId == "shieldL";
            }
            if (a_obj->Is(RE::FormType::Ammo)) return a_slotId == "ammo";
            // potions / scrolls / spell tomes are USED, not worn: any slot is a
            // drop target for them and the engine decides what that means
            return true;
        }

        // Everything currently equipped, mapped to UI slots (D2).
        void CollectEquipment(std::unordered_map<std::string, EquipEntry>& a_out)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;

            // ★★★THE SIXTH ACCESSORY USED TO BE THROWN AWAY.
            //
            // This was `accPool[5]` and `if (!s) return;` -- an item the player
            // is visibly wearing, dropped with nothing on screen and nothing in
            // the log to say so. Outfit mods equip eight to fifteen accessories
            // routinely (scarves, bags, belts, glasses, cloaks, piercings all
            // sit on biped slots 44..60, which SlotForArmor cannot name), so
            // the cap was not an edge case.
            //
            // Accessories are now COLLECTED rather than assigned on sight, so
            // there is a complete list to sort before any cell is handed out.
            // The doll's five keep their names; the rest become acc6, acc7...
            // and the drawer shows them. SlotAccepts tests rfind("acc", 0) == 0,
            // which already accepts every one of those.
            struct PendingAcc
            {
                RE::TESBoundObject* obj;
                int                 count;
                std::uint8_t        glow;
                RE::ExtraDataList*  xl;
                int                 hand;
                int                 slotNo;   // PrimarySlot -- decides the cell
            };
            std::vector<PendingAcc> accs;

            auto add = [&](const char* slot, RE::TESBoundObject* obj, int count,
                           std::uint8_t glow = 0, RE::ExtraDataList* xl = nullptr,
                           int hand = 0) {
                if (!obj) return;
                if (!slot) {
                    // an accessory: park it, place it once they are all in
                    accs.push_back({ obj, count, glow, xl, hand, PrimarySlotOf(obj) });
                    return;
                }
                std::uint16_t uid = 0;
                if (xl) {
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
                }
                a_out[slot] = { obj, count, glow, uid, Grid::InstanceSigOf(xl), hand };
            };

            // ...and hand out the accessory cells, in slot-number order, once
            // the walk below has finished. Called at the end of this function.
            auto placeAccessories = [&]() {
                std::stable_sort(accs.begin(), accs.end(),
                    [](const PendingAcc& x, const PendingAcc& y) {
                        return x.slotNo < y.slotNo;
                    });
                // The doll's own cells, then the drawer's.
                // ★acc4 is GONE from this list — the strip's last cell became
                // the drawer button. Anything landing there would have been
                // invisible, which is the exact failure the drawer exists to
                // fix. The count below follows the array, so the two can never
                // disagree about where the doll stops.
                static const char* kDollAcc[] = { "acc1", "acc2", "acc3", "accM" };
                for (std::size_t i = 0; i < accs.size(); ++i) {
                    const auto& a = accs[i];
                    const std::string id =
                        (i < std::size(kDollAcc)) ? std::string(kDollAcc[i])
                                                  : ("acc" + std::to_string(i + 1));
                    std::uint16_t uid = 0;
                    if (a.xl) {
                        if (const auto* xu = a.xl->GetByType<RE::ExtraUniqueID>()) {
                            uid = xu->uniqueID;
                        }
                    }
                    a_out[id] = { a.obj, a.count, a.glow, uid,
                                  Grid::InstanceSigOf(a.xl), a.hand };
                }
                g_drawerCount = static_cast<int>(
                    accs.size() > std::size(kDollAcc) ? accs.size() - std::size(kDollAcc) : 0);
                g_drawerFirstIdx = static_cast<int>(std::size(kDollAcc));
            };

            // equipped SPELLS have no world model (garbage capture) and are
            // not doll items — the hands simply stay empty for them
            auto* right = player->GetEquippedObject(false);
            auto* left = player->GetEquippedObject(true);
            if (right && right->Is(RE::FormType::Spell)) right = nullptr;
            if (left && left->Is(RE::FormType::Spell)) left = nullptr;
            auto* rightObj = right ? right->As<RE::TESBoundObject>() : nullptr;
            // 33-B: `left != right` was a FORM comparison, so wielding two copies
            // of the same weapon reported the left hand as empty. The case it was
            // really guarding is a TWO-HANDER, which the engine reports in both
            // hands -- test for that instead of for form identity.
            bool twoHanded = false;
            if (right == left) {
                if (auto* w = right ? right->As<RE::TESObjectWEAP>() : nullptr) {
                    using WT = RE::WEAPON_TYPE;
                    const auto t = w->GetWeaponType();
                    twoHanded = t == WT::kTwoHandSword || t == WT::kTwoHandAxe ||
                                t == WT::kBow || t == WT::kCrossbow || t == WT::kStaff;
                } else {
                    twoHanded = true;   // not a weapon and reported in both hands
                }
            }
            auto* leftObj = (left && !twoHanded) ? left->As<RE::TESBoundObject>() : nullptr;
            if (rightObj || leftObj) {
                // entry-aware glow (player-crafted enchants live on the entry)
                std::uint8_t rGlow = Grid::GlowBits(rightObj, nullptr, nullptr);
                std::uint8_t lGlow = Grid::GlowBits(leftObj, nullptr, nullptr);
                RE::ExtraDataList* rXl = nullptr;
                RE::ExtraDataList* lXl = nullptr;
                auto inv = player->GetInventory([&](RE::TESBoundObject& o) {
                    return &o == rightObj || &o == leftObj;
                });
                for (auto& [obj, data] : inv) {
                    // GI1/D2: the doll shows the WORN unit — and with the same form
                    // in both hands that means the list for THIS hand, not the first
                    // worn list of the form.
                    if (obj == rightObj) {
                        rXl = Grid::WornExtraOf(data.second.get(), 1);
                        rGlow = Grid::GlowBits(obj, data.second.get(), rXl);
                    }
                    if (obj == leftObj) {
                        // GI54: a SHIELD is armour -- the engine wears it with
                        // ExtraWorn (hand-free), never ExtraWornLeft. Reading
                        // hand 2 missed its list (sig/uid/temper-ring blank)
                        // and recording hand 2 poisoned every hand-aware worn
                        // match downstream (the spare-shield blink).
                        const int lh = leftObj->Is(RE::FormType::Armor) ? 0 : 2;
                        lXl = Grid::WornExtraOf(data.second.get(), lh);
                        lGlow = Grid::GlowBits(obj, data.second.get(), lXl);
                    }
                }
                if (rightObj) add("weapon", rightObj, 1, rGlow, rXl, 1);
                if (leftObj) {
                    add("shieldL", leftObj, 1, lGlow, lXl,
                        leftObj->Is(RE::FormType::Armor) ? 0 : 2);
                }
            }

            if (auto* ammo = EquippedAmmo(player)) {
                // ★★The count on this slot is what is ON THE BACK, not how many
                // of that arrow the player owns. data.first is the whole stock —
                // the quiver plus every tile still in the pack — so the doll
                // claimed 200 while the grid showed the other 100 as its own
                // tile, and the two together counted 300 arrows that did not
                // exist. Harmless while the number was only printed; the moment
                // the unequip and the carry started USING it, clicking a
                // hundred-arrow quiver lifted two hundred.
                // Worn units are the lists the engine marked, exactly as the
                // rebuild counts them (Grid.cpp: wornUnits).
                int          worn = 0;
                int          stock = 0;
                std::uint8_t g = Grid::GlowBits(ammo, nullptr, nullptr);
                RE::ExtraDataList* ammoXl = nullptr;   // ★the quiver's own list
                auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& o) { return &o == ammo; });
                for (auto& [obj, data] : inv) {
                    stock = data.first;
                    auto* entry = data.second.get();
                    if (entry && entry->extraLists) {
                        for (auto* xl : *entry->extraLists) {
                            if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                       xl->HasType<RE::ExtraWornLeft>())) {
                                worn += (std::max)(1, xl->GetCount());
                            }
                        }
                    }
                    ammoXl = Grid::WornExtraOf(entry);
                    g = Grid::GlowBits(obj, entry, ammoXl);
                }
                // worn but unlisted: the engine is wearing the lot
                if (worn <= 0) worn = (std::max)(1, stock);
                add("ammo", ammo, worn, g, ammoXl);
            }

            auto inv = player->GetInventory(
                [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Armor); });
            for (auto& [obj, data] : inv) {
                auto& [count, entry] = data;
                if (count <= 0 || !entry || !entry->IsWorn()) continue;
                // ★The costume anchor is worn but is not gear -- it is the
                // placeholder that gives a bare slot an appearance list. Showing
                // it on the doll would advertise a helmet the player never put on.
                if (Costume::IsAnchor(obj)) continue;
                auto* armo = obj->As<RE::TESObjectARMO>();
                if (!armo) continue;
                const char* slot = SlotForArmor(armo);
                if (slot && std::string_view(slot) == "shieldL") {
                    // B9: the left-hand path normally reports the shield —
                    // but if it didn't (mod edge cases / spell in left hand
                    // data), a WORN shield must still show on the doll
                    if (!a_out.contains("shieldL")) {
                        auto* wxl = Grid::WornExtraOf(entry.get());
                        add("shieldL", obj, 1, Grid::GlowBits(obj, entry.get(), wxl), wxl);
                    }
                    continue;
                }
                // ★No form-level skip for the second ring here. The carried
                // ring is in the PACK, not worn, so this IsWorn loop never
                // meets it -- the old `Second() == armo continue` was a FORM
                // comparison that could only ever fire when a SIBLING unit of
                // that form was engine-worn, and then it hid exactly the ring
                // that deserved the first slot: a plain same-form pair showed
                // one ring on the doll while both were worn (user report).
                if (slot && std::string_view(slot) == "ringR") {
                    // ★The doll has two ring slots but the body has one kRing
                    // bit, so the second is only ever reached by a mod that
                    // equips more than one ring at a time. Fill them in order
                    // and SPILL the remainder into the accessory pool: a_out[s]
                    // is an overwrite, so a third ring used to replace the
                    // second and vanish off the doll leaving no trace at all.
                    // ★Asked of a_out, not remembered in a flag -- the map IS
                    // the answer to "is that slot taken", and a flag was a
                    // second copy of it that could only ever be wrong.
                    // ★ringL belongs to the carrier while one is active -- an
                    // engine-worn spill must not take the slot the hand
                    // placement below will overwrite.
                    if (!a_out.contains("ringR"))      slot = "ringR";
                    else if (!a_out.contains("ringL") && !DualRing::Second()) slot = "ringL";
                    else                               slot = nullptr;
                }
                // ★★★HAND THE LIST OVER, like the weapon slots always did.
                //
                // Without it `add` writes uid 0 / sig 0, and both markers are
                // asked as pool questions: IsPoolStolen(obj, 0, 0) compares
                // InstanceSig(list) == 0, while InstanceSig MIXES OWNERSHIP AND
                // TEMPER IN. A stolen unit therefore hashes to something other
                // than 0 and could never match -- stolen armour showed no mark
                // on the doll at all, ever -- and a tempered or enchanted piece
                // lost its favourite star the moment it went on the body. Only
                // plain, untempered armour behaved. It reads from outside as
                // "weapons show their star, armour doesn't", which is exactly
                // the split between the two call shapes.
                auto* wxl = Grid::WornExtraOf(entry.get());
                add(slot, obj, 1, Grid::GlowBits(obj, entry.get(), wxl), wxl);
            }

            // ★The second ring, placed after the loop. The engine is not
            // wearing it -- the carrier is -- so nothing above could have
            // found it, and the doll would have shown an empty slot while the
            // effect was plainly active.
            if (auto* second = DualRing::Second()) {
                add("ringL", second, 1, Grid::GlowBits(second, nullptr, nullptr));
            }

            // ★Every accessory is in hand now, so the cells can be handed out
            // in slot order. Nothing above may place one directly -- that was
            // the arrival-order assignment this replaces.
            placeAccessories();

            // ---- self-check: THE DOLL SHOWS WHAT THE BODY WEARS ---------------
            // Everything else in this log describes the BOARD. Nothing described
            // the doll, so "the same weapon in both hands" -- where the whole
            // question is whether the left slot resolves to its own unit --
            // produced no evidence at all and had to be judged by eye.
            //
            // Logged only when the doll CHANGES, so this stays quiet.
            {
                std::vector<std::pair<std::string, const EquipEntry*>> rows;
                rows.reserve(a_out.size());
                for (const auto& [k, v] : a_out) rows.push_back({ k, &v });
                std::sort(rows.begin(), rows.end(),
                          [](const auto& x, const auto& y) { return x.first < y.first; });
                std::string line;
                for (const auto& [k, v] : rows) {
                    line += std::format("{}='{}'(u{:04X}/s{:04X}/h{}) ", k,
                        v->obj ? v->obj->GetName() : "?", v->uid, v->sig, v->hand);
                }
                static std::string s_prev;
                if (line != s_prev) {
                    s_prev = line;
                    SKSE::log::info("[DOLL] {}", line.empty() ? "(empty)" : line);
                    // The engine is the authority: every worn unit of a form must
                    // have exactly one doll slot. Fewer slots than worn units is
                    // the dual-wield failure (one hand shows nothing); more is a
                    // unit drawn twice.
                    std::map<RE::TESBoundObject*, int> shown;
                    for (const auto& [k, v] : rows) if (v->obj) ++shown[v->obj];
                    for (const auto& [obj, n] : shown) {
                        // ★★AMMO IS A STACK IN ONE SLOT, and this check does not
                        // apply to it. Fifty arrows are worn as a single quiver
                        // and the doll shows exactly one slot for them, so
                        // counting worn UNITS reports a mismatch that is not one
                        // (measured: "'Steel Arrow' body wears 2 but doll shows
                        // 1" while the doll was entirely correct). The failure
                        // this check exists to catch -- one hand of a dual wield
                        // drawing nothing -- is about weapons.
                        if (obj->Is(RE::FormType::Ammo)) continue;
                        // ★The SECOND RING is never engine-worn by design --
                        // the carrier wears its effect (B4-4) -- so its slot
                        // legitimately shows a unit the body list lacks.
                        // Counting it cried "body wears 0 but doll shows 1"
                        // five times in one test session, all false.
                        if (DualRing::Second() == obj) continue;
                        auto* e = Grid::LiveEntryOf(player, obj);
                        int wornUnits = 0;
                        if (e && e->extraLists) {
                            for (auto* xl : *e->extraLists) {
                                if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                           xl->HasType<RE::ExtraWornLeft>())) {
                                    wornUnits += (std::max)(1, xl->GetCount());
                                }
                            }
                        }
                        if (wornUnits != n) {
                            SKSE::log::warn("[DOLL] MISMATCH '{}' body wears {} but doll "
                                            "shows {} slot(s)", obj->GetName(), wornUnits, n);
                        }
                    }
                }
            }
        }

        // ★★THE CELL FACE, SHARED. Every doll slot and the drawer's button all
        // have to be the same object at rest — the button only stops looking
        // like a slot when the cursor is on it. Four skin branches copied into
        // a second place is exactly how that promise breaks, one skin at a
        // time, and nobody notices until someone reports the odd one out.
        //
        // frame (v9: border only; filled slots get the skin's filled
        // accent + faint fill)
        // ★engravedCells: every slot wears the SAME heavy frame, worn or
        // not, and "worn" is said by the darker ground alone. A bright rim
        // on the worn ones turned the doll into a scatter of highlights —
        // the reference marks occupancy with ground, never with rim.
        void PaintCellFace(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1,
                           const char* a_id, bool a_worn)
        {
            const auto& sk = Theme::S();
            // Half-pixel inset — see the note at the DrawSlot call site.
            const ImVec2 e0(p0.x + 0.5f, p0.y + 0.5f);
            const ImVec2 e1(p1.x - 0.5f, p1.y - 0.5f);

            if (Theme::InkChrome()) {
                // ★★THE WASH LIVES HERE AND NOWHERE ELSE. The board draws its
                // occupied ground as a flat rect and that split is deliberate:
                // a doll slot is one cell standing alone, where an irregular
                // blob reads; a board item can span 1x4, and a blob either
                // smears when stretched to that or breaks into four stamps.
                // See OccupiedGround() for why the two were unified in the
                // first place -- this is a designed divergence, not the drift
                // that comment warns about.
                // ★Keyed by the slot's id, so a slot keeps its own blob for as
                // long as it exists and its neighbours never match it.
                if (a_worn) {
                    unsigned int key = 2166136261u;
                    for (const char* p = a_id; p && *p; ++p) {
                        key = (key ^ static_cast<unsigned char>(*p)) * 16777619u;
                    }
                    Theme::InkWash(dl, p0, p1, Theme::OccupiedGround(), key);
                }
                // ★A box of four marks, not a rectangle -- and the SAME box
                // whether or not something is worn. The reference marks a worn
                // slot with the wash alone; adding a second signal to the
                // border turned the doll into a scatter of highlights, which
                // is the note the engraved branch below already carries.
                // ★★The SAME assembly the window uses, at slot size. Four
                // separate strokes were the problem: a section of a brush is
                // cut square at both ends, so a box of four met at four square
                // notches -- and no amount of overshoot fixes a cut, it only
                // makes it a longer cut. The corner mark is the piece that is
                // not square, and it is what the reference has there.
                const float th = Theme::InkHeavyPx();
                Theme::InkFrame(dl,
                    ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                    ImVec2(p1.x - 1.0f, p1.y - 1.0f),
                    Theme::Col(sk.ink, 0.80f), Theme::InkCornerFor(th));
            } else if (sk.engravedCells) {
                // ★A doll slot IS a grid cell — same face, same carved shadow,
                // no border and no drop shadow. It used to be a bordered card
                // floating on the panel while the board next to it was tiles,
                // so the two halves of one window spoke different languages.
                // Occupancy is said by the ground, exactly as on the board.
                dl->AddRectFilled(p0, p1, Theme::Col(sk.cellBg), sk.rounding);
                // ★Theme::OccupiedGround(), not sk.shade — the doll was the one
                // place still reading the raw token, so it would have kept the
                // old near-black ground after the board moved off it.
                if (a_worn) dl->AddRectFilled(p0, p1, Theme::OccupiedGround(), sk.rounding);
                const ImU32 inner = Theme::Col(sk.cellGroove, sk.cellGroove.w * 0.85f);
                dl->AddLine(ImVec2(p0.x, p0.y + 0.5f), ImVec2(p1.x, p0.y + 0.5f), inner);
                dl->AddLine(ImVec2(p0.x + 0.5f, p0.y), ImVec2(p0.x + 0.5f, p1.y), inner);
            } else if (a_worn) {
                // ★A worn slot is an occupied cell — same ground the grid gives
                // an item, so "there is something here" reads the same way in
                // both places. A 5% accent wash said nothing on a light panel.
                // ★★Nor on a TRANSLUCENT one, and there for a sharper reason:
                // the mark is 0.05*(filled - panel), and the panel climbs with
                // whatever is behind the window until it meets the filled
                // colour — measured +3.5 on snow, which is nothing. Skins that
                // let the world through take the grid's ground, which is tuned
                // across the whole range of backgrounds. Opaque skins keep the
                // wash: they have no background to lose to.
                dl->AddRectFilled(p0, p1,
                    sk.lightPanel || sk.translucent
                        ? Theme::OccupiedGround()
                        : Theme::Col(sk.filled, 0.05f), sk.rounding);
                if (sk.cornerFade) {   // v10.4: equipped highlight = corner fade
                    Theme::CornerFade(dl, e0, e1, Theme::Col(sk.filled, 0.70f));
                } else {
                    dl->AddRect(e0, e1, Theme::Col(sk.filled, 0.8f), sk.rounding);
                }
            } else {
                dl->AddRect(e0, e1, Theme::Acc(sk.cornerFade ? 0.14f : 0.28f), sk.rounding);
            }
        }

        // ★The tint an empty slot gives its silhouette. The drawer button's
        // mark takes the same one so it weighs exactly what the diamonds
        // beside it weigh — a hint, not a signal.
        [[nodiscard]] ImU32 CellHintCol()
        {
            const auto& sk = Theme::S();
            return Theme::InkChrome() ? Theme::Col(sk.ink, 0.65f)
                 : sk.lightPanel      ? Theme::Col(sk.ink, 0.50f)
                                      : Theme::Acc(0.35f);
        }

        // ★★★ONE WALK A FRAME, SHARED BY THE PANEL AND THE DRAWER.
        //
        // CollectEquipment runs GetInventory three times (weapons, ammo,
        // armour) and GetInventory DEEP-COPIES every matching entry: a fresh
        // InventoryEntryData plus a new BSSimpleList of extra lists, per item.
        // Draw() and DrawDrawer() each asked for their own, so a character with
        // a full wardrobe paid for six to eight full walks of the pack every
        // frame the equipment panel was up.
        //
        // Safe to hold for the frame because nothing in the draw pass changes
        // the inventory -- every click here QUEUES its mutation for the game
        // thread, which is this plugin's rule everywhere. The per-click queries
        // (WornObjectAt and friends) deliberately keep walking fresh: they run
        // either side of real mutations, where a cached answer would be a lie.
        const std::unordered_map<std::string, EquipEntry>& EquipmentThisFrame()
        {
            static std::unordered_map<std::string, EquipEntry> s_eq;
            static int s_frame = -1;
            if (const int now = ImGui::GetFrameCount(); s_frame != now) {
                s_frame = now;
                s_eq.clear();
                CollectEquipment(s_eq);
            }
            return s_eq;
        }

        void DrawSlot(const SlotDef& a_slot, float a_w, float a_h,
                      const std::unordered_map<std::string, EquipEntry>& a_eq)
        {
            const auto& sk = Theme::S();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 p1(p0.x + a_w, p0.y + a_h);

            const auto it = a_eq.find(a_slot.id);
            const EquipEntry* eq = it != a_eq.end() ? &it->second : nullptr;

            // ★★THE BORDER RECT IS HALF A PIXEL IN, and that is not a nicety.
            // ImGui strokes a rect ON its path, so a 1px border straddles the
            // edge: half of it lies OUTSIDE p0..p1. The doll's last column ends
            // exactly on the panel's clip boundary -- measured in game, slot
            // 1394.0 and clip 1394.0 -- so that outer half was clipped away and
            // the right edge of every slot in that column drew at half weight.
            // ★It was never an ink-skin bug. Every skin draws its slot border
            // with AddRect, so every skin had it; the ink one only made it
            // obvious by drawing a border thick enough to notice.
            // The FILL still uses p0..p1: a cell's ground is the whole cell.
            const ImVec2 e0(p0.x + 0.5f, p0.y + 0.5f);
            const ImVec2 e1(p1.x - 0.5f, p1.y - 0.5f);

            PaintCellFace(dl, p0, p1, a_slot.id, eq != nullptr);

            if (eq) {
                // GI51: same rule as the grid — a worn item is drawable now,
                // the 3D capture only raises the quality when it lands.
                const IconCache::Icon* eqIcon = IconCache::GetSingleton()->Get(eq->obj);
                bool eqFallback = false;
                if (!eqIcon) {
                    IconCache::GetSingleton()->QueueCapture(eq->obj);
                    eqIcon = Fallback::Get(eq->obj);
                    eqFallback = eqIcon != nullptr;
                }
                if (const auto* icon = eqIcon) {
                    // alpha-trimmed sprite. Rings capture close-up and looked
                    // oversized — half size. Everything else CONTAIN-fits the
                    // slot BOX (the old min-side square fit left tall sprites
                    // small inside the tall body/weapon slots).
                    const bool ringSlot = std::string_view(a_slot.id) == "ringR" ||
                                          std::string_view(a_slot.id) == "ringL";
                    float dw, dh;
                    if (ringSlot) {
                        const float target = (std::min)(a_w, a_h) * 0.46f;
                        const float ms = static_cast<float>((std::max)(icon->w, icon->h));
                        dw = icon->w / ms * target;
                        dh = icon->h / ms * target;
                    } else {
                        float s = (std::min)(a_w * 0.92f / icon->w,
                                             a_h * 0.92f / icon->h);
                        // CONTAIN-fitting alone BLOWS UP small items: a dagger
                        // draws 1x2 cells in the grid but the weapon slot box is
                        // far bigger, so the fit scaled it past life size. Cap at
                        // the size the grid would give it — same rule as there
                        // (long axis = footprint long axis * 0.95 * def.scale) —
                        // so a slot can shrink an item to fit but never enlarge it.
                        const auto def = Grid::ResolveDef(eq->obj);
                        // Cap at the size the GRID would give it — and the grid
                        // sizes the two kinds differently: a trimmed capture by
                        // its long axis, a square category icon contained in the
                        // short one. Using the capture rule for both is what
                        // made a category icon balloon past its cell.
                        const float gridCap = eqFallback
                            ? static_cast<float>((std::min)(def.w, def.h)) *
                                  Grid::CellPx() * 0.85f
                            : static_cast<float>((std::max)(def.w, def.h)) *
                                  Grid::CellPx() * 0.95f * def.scale;
                        const float longPx = (std::max)(icon->w, icon->h) * s;
                        if (gridCap > 0.0f && longPx > gridCap) s *= gridCap / longPx;
                        dw = icon->w * s;
                        dh = icon->h * s;
                    }
                    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                    const auto  fdef = Grid::ResolveDef(eq->obj);
                    const float eqRot = eqFallback ? fdef.frot : 0.0f;
                    const float eqOff = eqFallback ? fdef.fx * Grid::CellPx() : 0.0f;
                    // rarity glow + status rings UNDER the icon -- the shared
                    // renderer (GI49: this was a THIRD hand copy of the halo
                    // switch; unmasked, it read poison/temper status bits as
                    // "both rarities" red and never drew the rings)
                    // ★★1.4.4: THE `if (eq->glow)` GUARD IS GONE, and it was
                    // hiding the museum mark on every worn item that has no
                    // rarity of its own. Reported as "weapons show it, armour
                    // does not" -- which was a coincidence of that character's
                    // gear, not a difference between the two: the worn weapon
                    // happened to be enchanted and the armour did not.
                    // The guard was never needed. DrawGlow is one call through
                    // to DrawRarityWedge, which decides for itself whether
                    // there is anything to draw -- and the other two boards
                    // (partner window, stored bag) have always called it
                    // unguarded for exactly that reason.
                    Grid::DrawGlow(dl, eq->obj, eq->glow,
                        ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                        ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f),
                        p0, p1);
                    // ★1.0.5: the same shadow the grid gives its tiles, so a
                    // pale item does not vanish here either
                    Grid::DrawItemShadow(dl, icon->srv,
                                         ImVec2(c.x + eqOff, c.y), dw, dh, eqRot);
                    UIRoot::DrawItemIconRot(dl, icon->srv,
                        ImVec2(c.x + eqOff, c.y), ImVec2(dw, dh), eqRot);
                }
                // GI30: this slot wears the pool's favourite -- draw the mark
                // here so it does not read as "the star disappeared".
                // ★★1.0.5: through the SHARED tray, at the grid's size. It used
                // to be a hand-rolled diamond at its own 6.5px radius "because
                // the doll slot is 2x2", which made the same flag a different
                // object depending on the window — and it collided with the
                // poison droplet DrawGlow was putting in the same corner.
                Grid::DrawMarkerTray(dl, p0, p1,
                                     Grid::IsPoolStarWorn(eq->obj, eq->uid, eq->sig),
                                     Grid::IsPoolStolen(eq->obj, eq->uid, eq->sig),
                                     (eq->glow & 0x4) != 0);     // poison
                if (eq->count > 1) {   // eqqty (ammo stack)
                    // ★★The SAME badge the grid draws, not a second one that
                    // happens to say the same number. This slot used to place
                    // it top-RIGHT in sk.hi with a single drop shadow, while a
                    // quiver one window over sat top-LEFT in Val() with a full
                    // ring — so equipping arrows moved the count across the
                    // tile and changed its colour. A stack is a stack wherever
                    // it is shown, and one function owning that is what keeps
                    // the two from drifting again (the marker tray below is
                    // shared for exactly this reason).
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", eq->count);
                    Grid::DrawCountBadge(dl, p0, buf);
                }
            } else if (const auto* sil = Silhouette(a_slot.icon)) {
                // white silhouette tinted by the skin accent (46% of the SLOT
                // unit, not the tall height — v9 keeps tall-slot icons modest)
                const float unit = SlotPx();
                const float target = unit * 0.46f;
                // ★★Sized by the GEOMETRIC MEAN, not the long side. Fitting the
                // long side makes apparent size depend on shape: measured over
                // the twelve sprites, a near-square sword filled 0.94 of its
                // square while a wide shallow circlet filled 0.48 -- half the
                // area at the same nominal size, and the eye reads area. Half
                // the set looked small and no number in the file said why.
                // sqrt(w*h) holds AREA constant instead, so a long thin shape
                // grows along its length and a squat one along its width.
                // ★Costs nothing on a padded square sprite (w == h makes this
                // the old formula exactly), so older art still draws as it did.
                const float ms = std::sqrt(static_cast<float>(sil->w) *
                                           static_cast<float>(sil->h));
                float dw = sil->w / ms * target;
                float dh = sil->h / ms * target;
                // ...but an extreme aspect must not run out of its slot.
                const float cap = unit * 0.72f;
                if (const float over = (std::max)(dw, dh) / cap; over > 1.0f) {
                    dw /= over;
                    dh /= over;
                }
                const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                // ★The silhouette art is white. Tinting it with the ACCENT is
                // right over a dark panel and wrong over a light one — navy at
                // 35% on blue is barely a shape. A light panel keeps it white
                // and just holds it back with alpha, so it stays a hint rather
                // than competing with the item that will sit there.
                const auto& sk2 = Theme::S();
                // ★★The ink set is PAINTED, so it is thinner than the flat one
                // at the same size: measured over the twelve slots, its alpha
                // sums to 0.766 of the silhouettes' (brush tone is partial
                // coverage, the flat art is solid). At a shared 0.50 the two
                // sets are the same SHAPE and the same SIZE and still do not
                // weigh the same on the panel. 0.50 / 0.766 restores that.
                const ImU32 silCol =
                    Theme::InkChrome() ? Theme::Col(sk2.ink, 0.65f)
                  : sk2.lightPanel    ? Theme::Col(sk2.ink, 0.50f)
                                      : Theme::Acc(0.35f);
                dl->AddImage(reinterpret_cast<ImTextureID>(sil->srv),
                    ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                    ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f),
                    ImVec2(0, 0), ImVec2(1, 1), silCol);
            }

            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton((std::string("##eq_") + a_slot.id).c_str(),
                ImVec2(a_w, a_h));

            if (!Grid::IsHolding()) {
                if (eq && ImGui::IsItemHovered()) {
                    // I1: name + stats. D1: the doll shows the WORN unit.
                    // kWorn resolves "the first worn list of this form"; with a
                    // copy in each hand that is the wrong one for one of the two
                    // slots. Hand the tooltip the list this slot actually wears.
                    Grid::DrawItemTooltip(eq->obj, eq->count, -1, -1, false, nullptr,
                                          Grid::ExtraScope::kWorn,
                                          eq->uid, -1, eq->sig, eq->hand,
                                          Grid::TileContext{ {}, false, false, false, true });
                    // (1.3.1) T = recharge the WORN unit -- while equipped the
                    // charge lives in this hand's AV, and OpenRecharge knows.
                    if (ImGui::IsKeyPressed(ImGuiKey_T, false) &&
                        !ImGui::GetIO().WantTextInput) {
                        Grid::OpenRecharge(eq->obj, eq->uid, eq->sig, true, eq->hand);
                    }
                    // ★F stars it, the same key and the same queue the board
                    // uses. The doll and the drawer are where a player looks at
                    // what they are actually wearing, and until now the only
                    // way to star a worn item was to take it off first. Named
                    // by uid+sig: a worn unit owns no cell to point at.
                    // (no coin guard: a doll slot never holds one -- coins and
                    // pouches are pack tiles and have no biped slot to sit in)
                    if (ImGui::IsKeyPressed(ImGuiKey_F, false) &&
                        !ImGui::GetIO().WantTextInput) {
                        Grid::ToggleFavoriteUnit(eq->obj, eq->uid, eq->sig,
                                                 eq->hand);
                        Sfx::Favorite();
                        // ★S1: no rebuild -- the doll's own star reads
                        // IsPoolStarWorn live every frame, and the grid
                        // tiles' stars refresh in place when the engine
                        // applies the toggle (ProcessFavorites). Measured
                        // as the #4 rebuild source of the gate-1 session
                        // (8 of 85) doing nothing the refresh does not.
                    }
                    // C: the same 3D view the grid offers. Vanilla files Item
                    // Zoom under the kItemMenu context, which every item screen
                    // shares -- turning a piece over is expected wherever an
                    // item is shown, and the doll is the one place you can see
                    // what you are actually wearing.
                    if (!UIRoot::MouseInOverlay() &&
                        ImGui::IsKeyPressed(ImGuiKey_C, false) &&
                        !ImGui::GetIO().WantTextInput) {
                        UIRoot::OpenInspect(eq->obj, Grid::DefKeyOf(eq->obj));
                    }
                }
                // ★★The second ring has NO worn copy for the unequip queue to
                // find: the engine is wearing a carrier, not the ring. Both
                // click paths have to route through DualRing instead -- the
                // right-click one because the unequip would do nothing, and the
                // left-click one because the carry would lift the item while it
                // stayed on the doll, reading as a duplicate.
                const bool secondRing = eq && std::string_view(a_slot.id) == "ringL" &&
                                        DualRing::Second() == eq->obj;

                if (eq && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {   // D5
                    if (secondRing) {
                        SKSE::log::info("[ACT] rclick-unequip second ring '{}'",
                                        eq->obj->GetName());
                        DualRing::RequestTakeOff();
                    } else {
                        SKSE::log::info("[ACT] rclick-unequip '{}' slot '{}' hand={}",
                                        eq->obj->GetName(), a_slot.id, eq->hand);
                        g_pending.push_back({ eq->obj->GetFormID(), "", true,
                                              eq->uid, -1, eq->sig, {}, eq->hand,
                                              EquipCountFor(eq->obj, eq->count) });
                    }
                }
                if (eq && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    // v9.2: left-click PICKS the equipped item up — unequip
                    // (deferred to Tick) + start carrying it right away
                    // The slot already knows WHICH unit it is wearing -- take it
                    // from there instead of re-resolving "the first worn list of
                    // this form", which answered the other hand's item whenever
                    // the same form was worn twice.
                    if (secondRing) {
                        DualRing::RequestTakeOff();
                    } else {
                        g_pending.push_back({ eq->obj->GetFormID(), "", true,
                                              eq->uid, -1, eq->sig, {}, eq->hand,
                                              EquipCountFor(eq->obj, eq->count) });
                    }
                    // ★The carry must match what the unequip above actually
                    // takes off. Queuing the whole quiver but lifting ONE arrow
                    // sent the other ninety-nine straight to the pack the
                    // instant the click landed.
                    // ★★The second ring lifts to the cursor like every other
                    // slot -- but its carry is marked fromCarrier, because its
                    // unit was never ENGINE-worn and the worn-backed
                    // accounting must not let it claim the FIRST ring's list
                    // (same plain form leaked that unit onto the board).
                    Grid::BeginCarry(eq->obj, eq->uid, eq->sig, eq->hand,
                                     /*swappedOut=*/false,
                                     EquipCountFor(eq->obj, eq->count),
                                     /*fromCarrier=*/secondRing);
                }
            } else if (ImGui::IsItemHovered()) {
                // C6: carried item over a slot — highlight; click = equip try
                // ★Same half-pixel rule as the slot border: a 2px stroke on
                // the path puts 1px outside, and on the last column that 1px
                // is past the clip.
                dl->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                            ImVec2(p1.x - 1.0f, p1.y - 1.0f),
                            Theme::Col(sk.hi, 0.8f), sk.rounding, 0, 2.0f);
                Grid::NotifySlotDropTarget(a_slot.id);
            }
        }
    }

    bool IsWearOrConsume(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return false;
        // ★★NO BOOK IS "USED" HERE, tomes included. A tome used to count,
        // because equipping one does teach its spell -- but that is the raw
        // learn with nothing in front of it, and the checks a player expects
        // (and mods add) live on READING the book instead. Every book now
        // takes the RequestBookRead branch and the engine settles it.
        return a_obj->Is(RE::FormType::Weapon) || a_obj->Is(RE::FormType::Armor) ||
               a_obj->Is(RE::FormType::Ammo) || a_obj->Is(RE::FormType::Light) ||
               a_obj->Is(RE::FormType::AlchemyItem) || a_obj->Is(RE::FormType::Scroll) ||
               // ★Vanilla EATS an ingredient on click (that is how effects are
               // discovered). The tooltip already promised "use" on these and
               // the gate refused it — the bar said one thing, the click did
               // another.
               a_obj->Is(RE::FormType::Ingredient);
    }

    int EquipCountFor(RE::TESBoundObject* a_obj, int a_tileCount)
    {
        return (a_obj && a_obj->Is(RE::FormType::Ammo))
                   ? (std::max)(1, a_tileCount)
                   : 1;
    }

    RE::TESAmmo* EquippedAmmo(RE::Actor* a_actor)
    {
        if (!a_actor) return nullptr;
        auto quiver = a_actor->GetInventory([](RE::TESBoundObject& o) {
            return o.Is(RE::FormType::Ammo);
        });
        for (auto& [obj, data] : quiver) {
            if (data.first <= 0) continue;
            if (auto* e = data.second.get(); e && e->IsWorn()) {
                return obj->As<RE::TESAmmo>();
            }
        }
        return nullptr;
    }

    bool UseItem(RE::TESBoundObject* a_obj, std::uint16_t a_uid, int a_xlIdx,
                 std::uint16_t a_sig, const std::string& a_srcKey, int a_tileCount)
    {
        if (!a_obj) return false;
        // No slot, no type test. ProcessPending hands it to the engine exactly
        // as a vanilla inventory click would; a form the engine cannot use
        // simply does nothing, which is also what vanilla does.
        // ★The type is LOGGED: when a mod's click-me item still does nothing,
        // this line is what says which record type to go look at next.
        SKSE::log::info("[USE] '{}' formType={}", a_obj->GetName(),
                        static_cast<int>(a_obj->GetFormType()));
        // ★1.4/B0. Round 2 turned up the step's first MISMATCH here: applying a
        // poison logged ONE use and produced TWO -1 container events, while the
        // stack fell by one. Registering the consumable kinds -- and only those,
        // so a use that consumes nothing does not sit in the queue expiring --
        // is what tells the next round whether the surplus event is ours or the
        // engine's.
        // ★Ingredients (eaten on click) joined the list with the consume-release
        // fix: their confirmation is what frees the suppression entry, and an
        // unregistered consume left it to the applied-erase fallback -- one
        // rebuild late.
        //
        // ★★★AND BOOKS LEFT IT AGAIN. They were here for the spell tome, which
        // is consumed on learn -- but the tome no longer comes through this
        // door at all. Reading moved to TESObjectBOOK::Read (ProcessBookRead),
        // and the tome's spending is done explicitly there with its own
        // NotePendingRemove; measured in the same log:
        //
        //   'Spell Tome: Ash Rune'  Read=true  Use=false   <- never reaches here
        //   'Line and Lure'         Read=false Use=true    <- only plain books do
        //
        // So every reservation this line still made was for a book that is NOT
        // consumed, and each one sat in the queue until it timed out and forced
        // a rebuild nobody asked for:
        //
        //   [LEDGER] ★expired: 000F86FE -1 'use' -- 180 frames, never confirmed
        //   [GRID] 'The Book of the Dragonborn' -1 (use) was never confirmed
        //
        // A mod's book that DOES vanish on use now falls back to the
        // applied-erase path -- one rebuild late, which is the cheaper side of
        // this trade by far: that book is rare, and reading one is not.
        if (Ledger::Enabled() &&
            (a_obj->As<RE::AlchemyItem>() || a_obj->As<RE::ScrollItem>() ||
             a_obj->As<RE::IngredientItem>())) {
            Ledger::Submit(a_obj->GetFormID(),
                -EquipCountFor(a_obj, a_tileCount), "use", a_uid, a_sig);
        }
        g_pending.push_back({ a_obj->GetFormID(), "", false, a_uid, a_xlIdx,
                              a_sig, a_srcKey, 0,
                              EquipCountFor(a_obj, a_tileCount), a_tileCount });
        return true;
    }

    bool UnequipItem(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                     std::uint16_t a_sig, int a_hand, int a_count)
    {
        if (!a_obj) return false;
        SKSE::log::info("[ACT] unequip '{}' hand={}", a_obj->GetName(), a_hand);
        g_pending.push_back({ a_obj->GetFormID(), "", true, a_uid, -1, a_sig, {},
                              a_hand, EquipCountFor(a_obj, a_count) });
        return true;
    }

    bool RingWantsSecondSlot(RE::TESBoundObject* a_obj)
    {
        auto* ringIn = a_obj ? a_obj->As<RE::TESObjectARMO>() : nullptr;
        if (!ringIn || !Grid::IsRing(ringIn) || DualRing::IsCarrier(ringIn)) {
            return false;
        }
        // ★The routing, unchanged (user spec): a second ring JOINS the first
        // when it brings something the first does not; where both slots are
        // taken the duplicate names its own victim, and everything else trades
        // with the first as it always did. Reading the board BEFORE anyone
        // clears it is the whole reason this runs where it does -- the
        // conflict pass downstream strips everything on kRing, so asking after
        // it would always answer "no first ring".
        auto* firstObj = WornObjectAt("ringR");
        auto* first = firstObj ? firstObj->As<RE::TESObjectARMO>() : nullptr;
        if (first && !Grid::IsRing(first)) first = nullptr;
        auto* second = DualRing::Second();
        bool  toSecond = false;
        if (first && !second) {
            toSecond = !DualRing::SharesEffect(first, ringIn);
        } else if (first && second) {
            toSecond = DualRing::SharesEffect(second, ringIn) &&
                       !DualRing::SharesEffect(first, ringIn);
        }
        // ★[RING] every routed click, decision and state -- the "all my rings
        // ended up worn" report needs the router's own words, not a
        // reconstruction.
        SKSE::log::info("[RING] route '{}': first='{}' second='{}' "
                        "toSecond={} carrierCarry={}",
            ringIn->GetName(),
            first ? first->GetName() : "-",
            second ? second->GetName() : "-",
            toSecond ? 1 : 0, Grid::CarrierCarryActive() ? 1 : 0);
        return toSecond;
    }

    void RequestWear(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                     std::uint16_t a_sig, int a_hand, int a_count, bool a_second)
    {
        if (!a_obj) return;
        std::string slot;
        if (a_second) {
            slot = "ringL";   // the commit's ringL branch IS the carrier route
        } else if (a_hand == 2 && a_obj->Is(RE::FormType::Weapon)) {
            slot = "shieldL";   // a left-hand lift goes back to the left hand
        }
        SKSE::log::info("[ACT] re-wear '{}' (cancelled carry) slot '{}' hand={}",
            a_obj->GetName(), slot.empty() ? std::string("-") : slot, a_hand);
        g_pending.push_back({ a_obj->GetFormID(), slot, false, a_uid, -1,
                              a_sig, {}, a_hand,
                              EquipCountFor(a_obj, a_count), a_count });
    }

    bool EquipItem(RE::TESBoundObject* a_obj, const std::string& a_slotId,
                   std::uint16_t a_uid, int a_xlIdx, std::uint16_t a_sig,
                   const std::string& a_srcKey, int a_tileCount)
    {
        if (!a_obj) return false;

        // D3 type gate (sync — callers key refresh off the verdict).
        // ★This gate is about the DOLL: what a slot will accept. The
        // right-click "use" goes through UseItem and is deliberately ungated —
        // see the header.
        if (!IsWearOrConsume(a_obj)) return false;

        if (!SlotAccepts(a_obj, a_slotId)) return false;   // wrong slot: reject the drop

        g_pending.push_back({ a_obj->GetFormID(), a_slotId, false, a_uid, a_xlIdx,
                              a_sig, a_srcKey, 0,
                              EquipCountFor(a_obj, a_tileCount), a_tileCount });
        return true;
    }

    // GI7: the doll's own collection lives in an anonymous namespace, so
    // nothing outside could ask "what is worn in this slot". Extensions and the
    // equip path both need it. Re-resolved on every call -- an ExtraDataList*
    // must never outlive the frame it was fetched in.
    RE::TESBoundObject* WornObjectAt(const std::string& a_slotId)
    {
        std::unordered_map<std::string, EquipEntry> eq;
        CollectEquipment(eq);
        const auto it = eq.find(a_slotId);
        return it == eq.end() ? nullptr : it->second.obj;
    }

    int WornCountAt(const std::string& a_slotId)
    {
        std::unordered_map<std::string, EquipEntry> eq;
        CollectEquipment(eq);
        const auto it = eq.find(a_slotId);
        return it == eq.end() ? 0 : (std::max)(1, it->second.count);
    }

    RE::ExtraDataList* WornExtraAt(const std::string& a_slotId)
    {
        std::unordered_map<std::string, EquipEntry> eq;
        CollectEquipment(eq);
        const auto it = eq.find(a_slotId);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (it == eq.end() || !it->second.obj || !player) return nullptr;
        return Grid::WornExtraMatching(Grid::LiveEntryOf(player, it->second.obj),
                                       it->second.uid, it->second.sig, it->second.hand);
    }

    namespace
    {
        std::uint16_t UidOfList(RE::ExtraDataList* a_xl)
        {
            if (!a_xl) return 0;
            const auto* xu = a_xl->GetByType<RE::ExtraUniqueID>();
            return xu ? xu->uniqueID : 0;
        }
    }

    void ProcessPending()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!player || !em) { g_pending.clear(); return; }

        // late pass: rebuild + FORCE the biped 3D refresh once the equip data
        // has settled — while paused the engine never runs the actor update
        // that would apply it (the "only visible after closing" root cause)
        // ★1.4/B3-c: this used to rebuild the WHOLE board here as well, and it
        // did not need to. Measured with !rbdrop over a full session -- 28
        // suppressed calls, no doll mismatch, no error, nothing visibly wrong.
        // It was the third of THREE full rebuilds per equip. The 3D refresh
        // below is what this late pass was actually for; the rebuild was along
        // for the ride.
        if (g_rebuildLag > 0 && --g_rebuildLag == 0) {
            if (auto* proc = player->GetActorRuntimeData().currentProcess) {
                proc->Update3DModel(player);
            }
        }
        if (g_pending.empty()) return;

        for (const auto& act : g_pending) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(act.id);
            if (!obj) continue;

            if (act.unequip) {
                // D4: unequip the WORN sub-stack explicitly -- with a null list
                // the engine picks, and it does not have to pick the one on the
                // body when spares of the same form sit in the pack.
                // GI53: name the HAND too (worn-unit identity needs it), and
                // hand the engine the left-hand slot so identical copies in
                // both hands cannot resolve to the wrong side.
                auto* wornList = Grid::WornExtraMatching(Grid::LiveEntryOf(player, obj),
                                                         act.uid, act.sig, act.hand);
                const RE::BGSEquipSlot* unSlot = act.hand == 2
                    ? RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x13F43)
                    : nullptr;
                // ★The same quantity rule as equipping: a quiver comes off
                // whole. Taking one arrow back per click would leave the rest
                // worn with no tile to click.
                em->UnequipObject(player, obj, wornList, act.count, unSlot,
                    false, false, true, true);
                SKSE::log::info("[EQUIP] unequip {} x{}", obj->GetName(), act.count);
                continue;
            }

            // Spell tome: right-click = LEARN (a BookMenu can't open inside
            // our movie-less UI, so teach directly and consume the tome —
            // same net result as vanilla reading)
            if (auto* book = obj->As<RE::TESObjectBOOK>(); book && book->TeachesSpell()) {
                auto* spell = book->GetSpell();
                if (spell && !player->HasSpell(spell)) {
                    player->AddSpell(spell);
                    FUI::Sfx::Notify(spell->GetName(), "UISpellLearned");
                    // GI36: name the copy being consumed instead of letting the
                    // engine pick, and let rule 58 take its star with it.
                    auto* bxl = Grid::ExtraForInstance(
                        Grid::LiveEntryOf(player, book), act.uid, act.xlIdx);
                    const int starred =
                        (bxl && bxl->HasType<RE::ExtraHotkey>()) ? 1 : 0;
                    player->RemoveItem(book, 1, RE::ITEM_REMOVE_REASON::kRemove,
                        Grid::ResolveExitUnit(book, act.uid, act.sig, 1, starred,
                                              act.xlIdx),
                        nullptr);
                    Grid::RequestRebuild();
                    SKSE::log::info("[EQUIP] learned spell '{}'", spell->GetName());
                }
                continue;
            }

            // ★★RING ROUTER for SLOTLESS equips (right-click, wheel). The
            // engine always swaps the first slot, but two slots exist -- so a
            // second ring used to displace the first instead of joining it.
            // The routing (user spec):
            //   first slot worn, second free, no effect collision with the
            //     first  ->  the new ring joins on the SECOND slot;
            //   both worn  ->  the DUPLICATE names the victim: a ring sharing
            //     the second ring's effect trades with the SECOND (Wear's own
            //     "one at a time" TakeOff does the trade), anything else
            //     trades with the first as before;
            //   any refusal (no carrier, no free biped slot, effect collision
            //     with the first)  ->  fall through to the engine's first-slot
            //     swap, which the stand-down guard below keeps duplication-
            //     free. Targeted drops (act.slotId set) keep their aim.
            // ★★BEFORE the conflict pass below, which is exactly where the
            // first version died: that pass strips everything on kRing before
            // equipping, so by the time the router asked "what is on the first
            // slot" the answer was already nothing, and every enchanted ring
            // fell through to the plain engine swap. Only a same-form pair
            // ever routed -- its worn sibling survived on the pass's sameUnit
            // exemption. The router must read the board BEFORE anyone clears
            // it. (Wear ignores its list argument, so resolving srcList later
            // costs this call nothing.)
            if (act.slotId.empty()) {
                if (auto* ringIn = obj->As<RE::TESObjectARMO>();
                    ringIn && Grid::IsRing(ringIn) && !DualRing::IsCarrier(ringIn)) {
                    // ★O-1b: the decision itself moved to RingWantsSecondSlot
                    // so a click can aim with it. What stays here is the
                    // FALLBACK for callers that have no board to aim from --
                    // the wheel, which equips slotless while our menu is shut.
                    const bool toSecond = RingWantsSecondSlot(ringIn);
                    if (toSecond && DualRing::Wear(ringIn, nullptr)) {
                        Grid::ForgetTile(act.srcKey);   // rule 13, same as the drop path
                        Grid::NoteFormSeen(obj);
                        // B4-4: one tile's exit, not a repaint (see the ringL
                        // branch below -- same window, same reason)
                        Grid::DropTileDisplay(act.srcKey, obj);
                        SKSE::log::info("[EQUIP] '{}' -> second ring slot (routed)",
                            obj->GetName());
                        continue;
                    }
                }
            }

            // Same-slot conflict resolution BY HAND: while paused the engine's
            // own queued conflict pass is unreliable (stacked body armour) —
            // unequip everything sharing a biped slot with the incoming piece.
            //
            // ★★...but a ring aimed at the SECOND slot displaces NOTHING. It
            // never goes on the body at all -- a carrier stands in for it --
            // and its mask still says kRing, so this pass would strip the ring
            // already worn. Then DualRing refuses for want of a first ring and
            // the player is left wearing NEITHER: "slot conflict: unequip"
            // followed by "refused: kNoFirstRing" in the log.
            if (auto* armo = obj->As<RE::TESObjectARMO>(); armo && act.slotId != "ringL") {
                const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask().get());
                auto worn = player->GetInventory(
                    [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Armor); });
                for (auto& [o2, d2] : worn) {
                    if (d2.first <= 0 || !d2.second || !d2.second->IsWorn()) continue;
                    // Skipping the whole FORM meant a tempered helmet dropped on
                    // a slot wearing a PLAIN copy of the same helmet unequipped
                    // nothing -- the incoming piece had nowhere to go and the
                    // swap silently did not happen. Only the very unit being
                    // equipped is exempt, and units differ by signature.
                    if (o2 == obj) {
                        auto* w2 = Grid::WornExtraOf(d2.second.get());
                        const std::uint16_t s2 = Grid::InstanceSigOf(w2);
                        const bool sameUnit = (act.sig == s2) &&
                                              (act.uid == 0 || act.uid == UidOfList(w2));
                        if (sameUnit) continue;
                    }
                    auto* a2 = o2->As<RE::TESObjectARMO>();
                    if (!a2) continue;
                    // ★★NEVER the costume anchors. They are ARMO and they hold
                    // biped slots, so this pass takes them for the player's own
                    // gear and strips them -- and an anchor stripped is a
                    // costume piece with nothing to hang on, which is the bare
                    // chest after equipping something over a costume. They are
                    // not the player's property (see Costume.h): the costume
                    // system raises and drops them, and the next Apply already
                    // removes any that stopped being needed.
                    if (Costume::IsAnchor(o2)) continue;
                    if (static_cast<std::uint32_t>(a2->GetSlotMask().get()) & mask) {
                        em->UnequipObject(player, o2,
                            Grid::WornExtraOf(Grid::LiveEntryOf(player, o2)), 1, nullptr,
                            false, false, false, true);
                        SKSE::log::info("[EQUIP] slot conflict: unequip {}", o2->GetName());
                    }
                }
            }

            // ★★AMMO DOES NOT DISPLACE ITSELF. Equip a second tile of the SAME
            // arrow and the engine just marks that list worn as well — both
            // tiles are then on the back, and a worn unit is off the board, so
            // the quiver reads as VANISHED. (At the old count of 1 it went one
            // arrow per click: "반복하면 계속 사라진다" was this, one at a time.)
            // Nothing was ever lost — it was all equipped.
            // So take the worn quiver off FIRST: one tileful on the back at a
            // time, like every other slot. Done by hand for the same reason the
            // armour block above is: while paused, the engine's own conflict
            // pass is unreliable.
            // ★Before srcList is resolved, never after — unequipping rewrites
            // the entry's lists, and a pointer taken across that is a pointer
            // to something else.
            if (obj->Is(RE::FormType::Ammo)) {
                std::vector<RE::ExtraDataList*> wornNow;
                if (auto* entry = Grid::LiveEntryOf(player, obj);
                    entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (xl && xl->HasType<RE::ExtraWorn>()) wornNow.push_back(xl);
                    }
                }
                for (auto* xl : wornNow) {
                    em->UnequipObject(player, obj, xl, (std::max)(1, xl->GetCount()),
                                      nullptr, false, false, false, true);
                    SKSE::log::info("[EQUIP] quiver swap: unequip {} x{}",
                                    obj->GetName(), (std::max)(1, xl->GetCount()));
                }
            }

            // D4: a one-hander (or staff) dropped on the shield slot = left hand
            const RE::BGSEquipSlot* slot = nullptr;
            if (act.slotId == "shieldL") {
                if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    using WT = RE::WEAPON_TYPE;
                    const auto wt = weap->GetWeaponType();
                    if (wt == WT::kOneHandSword || wt == WT::kOneHandDagger ||
                        wt == WT::kOneHandAxe || wt == WT::kOneHandMace || wt == WT::kStaff) {
                        slot = RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x13F43);
                    }
                }
            }

            // D4: equip THIS copy. Resolving late (here, not at request time)
            // is deliberate -- ExtraDataList* must never be cached across
            // frames, the engine reallocates and frees them.
            // Identity first, POSITION last. xlIdx is a list index captured a
            // frame or more before this runs, and the engine reorders lists --
            // a stale index still resolves to a REAL list, just the wrong one,
            // so trying it first meant a carried tempered dagger re-equipped as
            // the plain copy and the signature fallback never even ran.
            RE::ExtraDataList* srcList = nullptr;
            if (act.uid != 0 || act.sig != 0) {
                srcList = Grid::ExtraForPool(Grid::LiveEntryOf(player, obj),
                                             act.uid, act.sig);
                // GI53: the named unit vanished between the click and this
                // tick (a queued sale/transfer raced us). A stale xlIdx still
                // resolves to a REAL list -- the wrong one -- so refuse rather
                // than equip an arbitrary sibling (PoolChoice rule 61).
                if (!srcList) {
                    SKSE::log::info("[EQUIP] named unit gone -- equip skipped");
                    continue;
                }
            } else {
                srcList = Grid::ExtraForInstance(Grid::LiveEntryOf(player, obj),
                                                 act.uid, act.xlIdx);
            }
            // ★The SECOND ring slot is reached by DROPPING on it, never by a
            // plain click -- that is the rule the feature is built on, so the
            // slot id has to survive this far. A ring aimed at the second slot
            // does not go through the engine at all: a carrier stands in for
            // it (DualRing), because the engine wears exactly one ring however
            // the biped slots are arranged.
            if (act.slotId == "ringL") {
                if (auto* ringArmo = obj->As<RE::TESObjectARMO>()) {
                    if (DualRing::Wear(ringArmo, srcList)) {
                        // ★Rule 13 reaches here too: putting a ring ON forgets
                        // the cell it came from. This path returns early and so
                        // skipped it, and taking the ring off later put it back
                        // in its old tile instead of the first free one --
                        // unlike every other piece of gear in the inventory.
                        Grid::ForgetTile(act.srcKey);
                        // ★B4-4: one tile's exit, not a repaint -- the tail
                        // rebuild here ran in the middle of the ring2 exclusion
                        // handoff (the deferred blink's window). Both callers
                        // have already taken the tile off the display by now
                        // (the lift for a drag, WholeUse for a click), so this
                        // is expected to be a no-op -- O-6 removes it once the
                        // log has confirmed that.
                        Grid::DropTileDisplay(act.srcKey, obj);
                        continue;
                    }
                    // ★★A REFUSED Wear must NOT eat the ring. This branch used
                    // to drop the tile and `continue` whatever Wear answered,
                    // so a ring the carrier would not take (same effect, no
                    // free biped slot) was neither worn NOR shown -- gone until
                    // the pending-equip TTL put it back two seconds later. The
                    // slotless router has always fallen through to the engine's
                    // own first-slot swap on a refusal; this is the same
                    // answer, and O-1b needs it because the click now aims
                    // HERE instead of at that router.
                    SKSE::log::info("[EQUIP] second ring slot refused '{}' -- "
                                    "falling through to the first", obj->GetName());
                }
            }
            // ★★...and the reverse. Equipping a ring NORMALLY while the carrier
            // stands in for one used to make the carrier let go whenever the
            // FORMS matched -- but form identity is not the rule (user spec):
            // a plain pair of one form is two legal rings, and two slots exist
            // to wear them. The carrier lets go only when it must:
            //   - the player owns a single unit of that form, so this equip is
            //     the very ring the carrier holds moving to the first slot,
            //     and leaving the carrier up would show it worn twice; or
            //   - the incoming ring shares a base effect with the carried one
            //     (the duplication the whole feature exists to prevent --
            //     same-form enchanted pairs land here too, since one form is
            //     one enchantment).
            if (auto* second = DualRing::Second(); second) {
                auto* ringIn = obj->As<RE::TESObjectARMO>();
                bool  letGo = false;
                if (second == obj) {
                    int owned = 0;
                    {
                        auto inv2 = player->GetInventory(
                            [&](RE::TESBoundObject& o) { return &o == obj; });
                        for (auto& [o2, d2] : inv2) owned = d2.first;
                    }
                    letGo = owned <= 1 ||
                            (ringIn && DualRing::WouldDuplicate(ringIn));
                } else if (ringIn && Grid::IsRing(ringIn) &&
                           DualRing::WouldDuplicate(ringIn)) {
                    letGo = true;
                }
                if (letGo) {
                    SKSE::log::info("[EQUIP] '{}' takes the first ring slot -- "
                                    "the carrier lets go", obj->GetName());
                    DualRing::TakeOff();
                }
            }
            // Rule 13 below needs to know whether the engine actually TOOK
            // units (a consumable drunk) or left them in the pack (a scripted
            // click-me item, a spell tome already known). Only a before/after
            // read can tell -- the tile test alone cannot.
            int before = 0;
            {
                auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& o) { return &o == obj; });
                for (auto& [o2, d2] : inv) before = d2.first;
            }
            em->EquipObject(player, obj, srcList, act.count, slot,
                            false, false, true, true);

            // Rule 13: equipping forgets the cell, exactly like selling or
            // storing. A stack that stays visible keeps its tile -- only the
            // unit that actually left is forgotten, and for a stack the tile is
            // still occupied by the rest.
            // The old guard counted the whole FORM, so owning a second copy
            // skipped the forget entirely -- which is exactly the case rule 13
            // is about. What matters is whether THIS TILE still has anything in
            // it: gear holds one unit per tile and is now empty, while a stack
            // tile still shows the rest and keeps its cell.
            // Gear holds one unit per tile, so its tile is now empty. A
            // STACKABLE item (torch, scroll, arrows) also empties its tile when
            // it was the only one -- the old cap-only test let those keep their
            // cell and they walked straight back to it on unequip.
            // ★...but ONLY for something that actually left. A scripted item
            // poked through UseItem is still sitting in the pack, and forgetting
            // its cell would move it to the first free slot on every single
            // click.
            int cnt = 0;
            {
                auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& o) { return &o == obj; });
                for (auto& [o2, d2] : inv) cnt = d2.first;
            }
            // ★Ammo always empties its tile: the whole tileful went on the back,
            // so that cell is free whatever the rest of the quiver still holds.
            // The count test below cannot see this — it asks about the FORM.
            // ★★And the test is TYPE-SENSITIVE, because `cnt` is read AFTER the
            // engine call. A WORN item stays in the inventory, so "cnt <= 1"
            // really asks "was this the only one" -- right for a single torch.
            // A CONSUMED item already left, so at two-going-on-one the post-use
            // count reads 1 and the old test forgot the cell of a tile that
            // still held an apple: drinking a middle-of-board stack down to
            // its last unit teleported that unit to the front gap (user
            // report; present since rule 13 -- 1.3.x ships it too).
            // ★★★"Nothing left of the FORM" (cnt <= 0) over-corrected for that:
            // it is form-wide, and rule 13 is about a TILE. With the form split
            // across tiles -- one potion in the pack, one in the potion bag --
            // draining the pack tile read cnt==1 and KEPT the emptied cell.
            // That stale layout claim then made the reconciler pay the deficit
            // from the other tile: the player drank the pack's potion and the
            // BAG's disappeared (user report). The exact question is "did the
            // units the engine took empty the clicked tile" -- act.tileCount
            // (what the tile held at click time) against act.count (what this
            // action moves), gated on the engine actually having taken them so
            // a scripted item that stays in the pack keeps its cell.
            const bool consumedType = obj->Is(RE::FormType::AlchemyItem) ||
                                      obj->Is(RE::FormType::Ingredient) ||
                                      obj->Is(RE::FormType::Book);
            const bool emptied = obj->Is(RE::FormType::Ammo) ||
                                 (consumedType
                                      ? (cnt < before &&
                                         (act.tileCount > 0
                                              ? act.count >= act.tileCount
                                              : cnt <= 0))
                                      : Grid::StackCap(obj) <= 1 || cnt <= 1);
            // ★P2/3-1: a BAG is the exception rule 13 now needs. The rule is
            // "leaving the board forgets the cell", and a worn bag never left
            // -- so forgetting its cell moved it to the first free space the
            // moment it was put on (user report). Everything else that gets
            // equipped genuinely does leave, and keeps the rule.
            if (IsWearOrConsume(obj) && emptied && !Grid::IsBagForm(obj)) {
                Grid::ForgetTile(act.srcKey);
            }
            // ★Equipping is the strongest "I have seen this" there is -- the
            // player picked it out and put it on. The NEW wash must not be
            // waiting for them when they take it off again.
            Grid::NoteFormSeen(obj);
            SKSE::log::info("[EQUIP] {}{}", obj->GetName(), slot ? " (left hand)" : "");
        }
        g_pending.clear();
        // Gear changed -> the costume has different armour to borrow from.
        // Marked, not applied: one outfit change lands as many equip calls and
        // the rebuild behind this is a whole-actor one.
        Costume::MarkDirty();
        Grid::MarkEquipsApplied();   // suppression has done its job
        // ★★1.4/B3: no rebuild here any more, and the reason is measured
        // (!rbdrop, §8-8). Equipping does not need one -- NotePendingEquip
        // already took the unit off the board when the player committed. Only
        // UNequipping did, because putting a unit back is work nobody does.
        //
        // So the rebuild moved to where an unequip is actually known: the
        // engine's own TESEquipEvent with equipped == false. That also covers
        // the case this code could never see -- the engine taking something off
        // by itself to resolve a slot conflict, which is most equips in
        // practice.
        g_rebuildLag = 2;
    }

    // L2: custom loadout tab strip — one tab per loadout, clipped horizontal
    // wheel-scroll, trailing "+" purchase button, double-click rename, right-click
    // delete. Replaces ImGui's rigid tab bar so all four interactions fit.
    void DrawLoadoutTabs()
    {
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        static int   s_renaming = -1;
        static bool  s_renameFocus = false;
        static char  s_renameBuf[64] = {};

        const float tabH = ImGui::GetFrameHeight();
        const float gap = 4.0f * S;

        // R3: the strip is a CLIPPED child of the panel width; the mouse wheel
        // scrolls it horizontally and the active tab auto-scrolls into view.
        // (The old suspicion that child+SetScrollX caused the "+" CTD was
        // wrong — the crash was Actor::GetGoldAmount.) The "+" stays OUTSIDE
        // the scroll region so it is always reachable; hidden at the cap.
        const float plusW = Loadout::AtCap() ? 0.0f : tabH + gap;
        const float stripW = PanelW() - plusW;
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        ImGui::BeginChild("##lt_strip", ImVec2(stripW, tabH), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            ImGui::SetScrollX(ImGui::GetScrollX() -
                ImGui::GetIO().MouseWheel * 60.0f * S);
        }
        static int s_lastActive = -1;
        const bool activeChanged = s_lastActive != Loadout::Active();

        for (int i = 0; i < Loadout::Count(); ++i) {
            if (i > 0) ImGui::SameLine(0, gap);

            if (s_renaming == i) {
                ImGui::SetNextItemWidth(120.0f * S);
                if (s_renameFocus) { ImGui::SetKeyboardFocusHere(); s_renameFocus = false; }
                const bool enter = ImGui::InputText("##rn", s_renameBuf, sizeof(s_renameBuf),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                if (enter || ImGui::IsItemDeactivated()) {
                    if (s_renameBuf[0]) Loadout::SetName(i, s_renameBuf);
                    s_renaming = -1;
                }
                continue;
            }

            const bool active = (i == Loadout::Active());
            if (active && activeChanged) {
                ImGui::SetScrollHereX(0.5f);   // keep the active tab visible
            }

            // ---- COSTUME marker (mockup B) ---------------------------------
            // The checked tab's outfit is worn as an APPEARANCE -- its stats
            // never apply, and the gear really equipped keeps all of its.
            //
            // ★The marker lives INSIDE the tab, before the name. An ImGui
            // Checkbox was tried first and read as a separate control: it
            // carries its own frame background, so the strip showed a box beside
            // a box instead of one tab with a mark in it. Drawn by hand here for
            // that reason -- there is no way to take the frame off a Checkbox.
            //
            // ★Disabled on the ACTIVE tab: dressing as the set already on the
            // body changes nothing. The marker is still drawn (dimmer) so the
            // tab does not change width when it becomes active.
            const bool hasMark = (i >= 1);   // EQUIP (0) is the base set
            const bool canMark = hasMark && Costume::CanBeTab(i);
            const bool marked = hasMark && Costume::IsTab(i);

            // Mockup B sizes, taken from the glyph rather than the font size:
            // a Tabler `square` at 15px draws its box across ~0.75 of the em, so
            // against 13px label text the box is ~11px -> 0.86 of the font size.
            const float mkSz = std::floor(ImGui::GetFontSize() * 0.86f);
            const float mkGap = 6.0f * S;          // mockup B: 6px
            const ImVec2 fpad = ImGui::GetStyle().FramePadding;
            const char* nm = Loadout::Name(i);
            const ImVec2 nmSz = ImGui::CalcTextSize(nm);
            const float lead = hasMark ? mkSz + mkGap : 0.0f;
            const ImVec2 tabPos = ImGui::GetCursorScreenPos();
            // ★A loadout tab is a BUTTON, and it now says on/off the same way
            // every other button does. It used to have its own two colours —
            // sel .30 for on (the only cyan face on screen) and acc .10 for off
            // (all but invisible) — so the two tabs read as a different kind of
            // control from the four beside them.
            ImGui::PushStyleColor(ImGuiCol_Button,
                sk.lightPanel ? (active ? Theme::BtnOn() : IM_COL32(0, 0, 0, 0))
                              : Theme::Col(active ? sk.sel : sk.acc, active ? 0.30f : 0.10f));
            // ★The hover wash is the skin's INK at low alpha, not white. White
            // was right only because the one light-panelled skin had white ink;
            // on a pale sheet with dark ink it lightens an already-light panel
            // and the hover all but disappears (+7 luma). Ink at 0.10 is the
            // same pixel value for SIMPLE (26/255) and the correct direction
            // for parchment, with no new token to keep in sync.
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                sk.lightPanel && !active ? Theme::Col(sk.ink, 0.10f)
                                         : Theme::Col(sk.sel, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Col(sk.sel, 0.55f));
            const ImVec4 inkVec =
                sk.lightPanel
                    ? (active ? Theme::BtnOnInkVec()
                              : ImGui::GetStyleColorVec4(ImGuiCol_Text))
                    : (active ? Theme::ValVec() : sk.ink);
            ImGui::PushStyleColor(ImGuiCol_Text, inkVec);
            // ★Label-less: the name is drawn by hand below, beside the marker.
            // Letting the button draw it would centre it over the marker's space.
            const std::string lbl = "##lt" + std::to_string(i);
            const bool hitTab = Sfx::Button(
                lbl.c_str(), ImVec2(fpad.x * 2.0f + lead + nmSz.x, tabH));
            ImGui::PopStyleColor(4);

            auto* tdl = ImGui::GetWindowDrawList();
            tdl->AddText(ImVec2(tabPos.x + fpad.x + lead,
                                tabPos.y + (tabH - nmSz.y) * 0.5f),
                         ImGui::GetColorU32(inkVec), nm);

            bool onMark = false;
            if (hasMark) {
                const ImVec2 m0(tabPos.x + fpad.x, tabPos.y + (tabH - mkSz) * 0.5f);
                const ImVec2 m1(m0.x + mkSz, m0.y + mkSz);
                // Same ink as the name at three strengths. ★0.63 is not a
                // guess: mockup B drew the unchecked mark #8b8d8e on a #1e2021
                // tab in #cacbcc text, and 30 + (202-30)a = 139 solves to 0.63.
                // The 0.45 tried first was visibly fainter than the mockup.
                const float a = canMark ? (marked ? 1.0f : 0.63f) : 0.30f;
                const ImU32 mc = ImGui::GetColorU32(
                    ImVec4(inkVec.x, inkVec.y, inkVec.z, inkVec.w * a));
                tdl->AddRect(m0, m1, mc, 2.0f * S, 0, (std::max)(1.0f, S));
                if (marked) {
                    ImGui::RenderCheckMark(tdl,
                        ImVec2(m0.x + mkSz * 0.17f, m0.y + mkSz * 0.17f),
                        mc, mkSz * 0.66f);
                }
                // ★One button, two targets, split by where the press landed.
                // An overlapping InvisibleButton would become the "last item"
                // and the next SameLine would measure from it instead of the tab.
                onMark = ImGui::GetIO().MousePos.x < m1.x + mkGap * 0.5f;
            }

            if (hitTab) {
                const bool pressedMark = hasMark && canMark &&
                    ImGui::GetIO().MouseClickedPos[0].x < tabPos.x + fpad.x + lead;
                if (pressedMark) Costume::SetTab(marked ? -1 : i);
                else if (!active) Loadout::RequestSwitch(i);
            }
            if (hasMark && onMark && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Lang::T(canMark ? Lang::Str::CostumeHint
                                                       : Lang::Str::CostumeWornHint));
            }

            // ★Rename and delete belong to the NAME, not the marker. Without
            // this the marker inherited both: double-clicking it opened the
            // rename field it had just toggled the costume with, and a
            // right-click there asked to delete the tab.
            if (i >= 1 && !onMark && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                s_renaming = i;
                s_renameFocus = true;
                std::snprintf(s_renameBuf, sizeof(s_renameBuf), "%s", Loadout::Name(i));
            }
            if (i >= 1 && !onMark && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                g_delTarget = i;
                g_buyOpen = false;
                Sfx::SelectOn();
            }
        }

        if (activeChanged) s_lastActive = Loadout::Active();
        const float scrollX = ImGui::GetScrollX();
        const float scrollMax = ImGui::GetScrollMaxX();
        ImGui::EndChild();

        // edge fades hint that more tabs are hidden beyond the clip
        if (scrollMax > 0.0f) {
            auto* dl = ImGui::GetWindowDrawList();
            const float fw = 16.0f * S;
            // ★A black fade is a shadow on a light panel, and it lands right
            // beside the "+" button — so it read as that button having a
            // strange drop shadow rather than as "the strip scrolls".
            const ImU32 fadeCol = sk.lightPanel ? IM_COL32(0, 0, 0, 45)
                                                : IM_COL32(0, 0, 0, 170);
            if (scrollX > 1.0f) {
                dl->AddRectFilledMultiColor(stripPos,
                    ImVec2(stripPos.x + fw, stripPos.y + tabH),
                    fadeCol, IM_COL32(0, 0, 0, 0),
                    IM_COL32(0, 0, 0, 0), fadeCol);
            }
            if (scrollX < scrollMax - 1.0f) {
                dl->AddRectFilledMultiColor(
                    ImVec2(stripPos.x + stripW - fw, stripPos.y),
                    ImVec2(stripPos.x + stripW, stripPos.y + tabH),
                    IM_COL32(0, 0, 0, 0), fadeCol,
                    fadeCol, IM_COL32(0, 0, 0, 0));
            }
        }

        if (!Loadout::AtCap()) {
            ImGui::SameLine(0, gap);
            // ('+' used to need centring by hand here; Sfx::Button ink-centres
            // every label now, so it is just a button again)
            if (Sfx::Button("+##addlt", ImVec2(tabH, tabH))) {
                g_buyOpen = true;
                g_delTarget = -1;
            }
        }
        // The confirm UI is drawn by DrawLoadoutWindows() at TOP LEVEL (not here
        // inside the equip child) — see UIRoot::Render.
    }

    // L2: buy / delete confirm windows — SAME construction as the settings
    // window (WinManager::ApplyNext fixed size + TitleBar + managed flags),
    // drawn at top level from UIRoot::Render. NOTE: the long "+"-click CTD was
    // never these windows — it was Loadout::PlayerGold -> Actor::GetGoldAmount
    // dereferencing a garbage BGSDefaultObjectManager slot (symbolized crash
    // chain). PlayerGold now returns Grid's cached gold, which is render-safe.
    bool IsPopupOpen() { return g_buyOpen || g_delTarget >= 1; }

    bool CloseTopPopup()
    {
        if (g_buyOpen) {
            g_buyOpen = false;
            return true;
        }
        if (g_delTarget >= 1) {
            g_delTarget = -1;
            return true;
        }
        return false;
    }

    void DrawLoadoutWindows()
    {
        if (!g_buyOpen && g_delTarget < 1) return;

        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;
        // every content line is centred on the window width (the TitleBar's
        // left-anchored content origin left all the slack on the right side)
        auto center = [](float a_itemW) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_itemW) * 0.5f));
        };

        if (g_buyOpen) {
            const int cost = Loadout::NextCost();
            const int gold = Loadout::PlayerGold();   // Grid cache — render-safe
            const bool afford = gold >= cost;

            char costLine[96];
            std::snprintf(costLine, sizeof(costLine), "%s: %dG",
                Lang::T(Lang::Str::CostLabel), cost);
            char goldLine[96];
            std::snprintf(goldLine, sizeof(goldLine), "%s (%dG)",
                Lang::T(Lang::Str::NotEnoughGold), gold);

            const float contentW = (std::max)({ btnRow, 200.0f * S,
                ImGui::CalcTextSize(costLine).x,
                afford ? 0.0f : ImGui::CalcTextSize(goldLine).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::BuyPresetTab)).x });
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            const float sp = ImGui::GetStyle().ItemSpacing.y;
            // left margin 12S (TitleBar's content origin) + matching 18S right
            const ImVec2 size(
                contentW + 30.0f * S + 2.0f * insX,
                barH + 8.0f * S + lineH + (afford ? 0.0f : lineH) +
                    6.0f * S + sp + ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
            if (wm->BeginConfirmPopup("ltbuy", "##fablerim_ltbuy",
                    Lang::T(Lang::Str::BuyPresetTab), size)) {
                g_buyOpen = false;
                Sfx::SelectOff();
            }

            center(ImGui::CalcTextSize(costLine).x);
            ImGui::TextColored(sk.ink, "%s", costLine);
            if (!afford) {
                center(ImGui::CalcTextSize(goldLine).x);
                ImGui::TextColored(ImVec4(0.80f, 0.32f, 0.28f, 1.0f), "%s", goldLine);
            }
            ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
            center(afford ? btnRow : btnW);
            // GI51: Enter/Space confirm (ESC closes via Equip::CloseTopPopup)
            // GI52: never while a text field (loadout rename) owns the keyboard
            const bool keyOk = !ImGui::GetIO().WantTextInput &&
                               (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_Space, false));
            if (afford) {
                if (Sfx::Button(Lang::T(Lang::Str::Confirm), ImVec2(btnW, 0)) || keyOk) {
                    Loadout::RequestPurchase();
                    g_buyOpen = false;
                }
                ImGui::SameLine(0.0f, 8.0f * S);
            }
            if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
                g_buyOpen = false;
            }
            ImGui::End();
        }

        if (g_delTarget >= 1) {
            const char* question = Lang::T(Lang::Str::DeletePresetConfirm);
            const float contentW = (std::max)({ btnRow, 200.0f * S,
                ImGui::CalcTextSize(question).x });
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            const float sp = ImGui::GetStyle().ItemSpacing.y;
            const ImVec2 size(
                contentW + 30.0f * S + 2.0f * insX,
                barH + 8.0f * S + lineH + 6.0f * S + sp +
                    ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
            if (wm->BeginConfirmPopup("ltdel", "##fablerim_ltdel",
                    Lang::T(Lang::Str::DeleteLabel), size)) {
                g_delTarget = -1;
                Sfx::SelectOff();
            }

            center(ImGui::CalcTextSize(question).x);
            ImGui::TextColored(sk.ink, "%s", question);
            ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
            center(btnRow);
            // GI51: Enter/Space confirm (ESC closes via Equip::CloseTopPopup)
            // GI52: never while a text field (loadout rename) owns the keyboard
            if (Sfx::Button(Lang::T(Lang::Str::DeleteLabel), ImVec2(btnW, 0)) ||
                (!ImGui::GetIO().WantTextInput &&
                 (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                  ImGui::IsKeyPressed(ImGuiKey_Space, false)))) {
                Loadout::RequestRemove(g_delTarget);
                g_delTarget = -1;
            }
            ImGui::SameLine(0.0f, 8.0f * S);
            if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
                g_delTarget = -1;
            }
            ImGui::End();
        }
    }

    // ★★ONE ANSWER, SHARED. SlotForArmor already decides where an item lands on
    // the doll and SlotAccepts already files anything it does not recognise as an
    // accessory; a tooltip that worked it out again would eventually disagree
    // with the slot the item actually goes into.
    // ★Every set bit is listed, not just the first: modded armour routinely
    // covers body+hands+feet at once, and "Body" alone would be a lie about what
    // the piece takes off you.
    std::string SlotLabel(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return {};

        // ★★Weapons have no biped slot worth printing -- the hand is not the
        // fact, the KIND is. Same switch the drawn-icon picker uses
        // (Fallback::KeyFor), keyword test included: the engine files both the
        // battleaxe and the warhammer as kTwoHandAxe, so the type alone would
        // call every warhammer a battleaxe.
        if (const auto* weap = a_obj->As<RE::TESObjectWEAP>()) {
            using WT = RE::WEAPON_TYPE;
            switch (weap->GetWeaponType()) {
            case WT::kOneHandDagger: return Lang::T(Lang::Str::WeapDagger);
            case WT::kOneHandSword:  return Lang::T(Lang::Str::WeapSword);
            case WT::kOneHandAxe:    return Lang::T(Lang::Str::WeapWarAxe);
            case WT::kOneHandMace:   return Lang::T(Lang::Str::WeapMace);
            case WT::kTwoHandSword:  return Lang::T(Lang::Str::WeapGreatsword);
            case WT::kTwoHandAxe:
                return Lang::T(weap->HasKeywordString("WeapTypeWarhammer")
                                   ? Lang::Str::WeapWarhammer
                                   : Lang::Str::WeapBattleaxe);
            case WT::kBow:           return Lang::T(Lang::Str::WeapBow);
            case WT::kCrossbow:      return Lang::T(Lang::Str::WeapCrossbow);
            case WT::kStaff:         return Lang::T(Lang::Str::WeapStaff);
            default:                 return Lang::T(Lang::Str::WeapSword);
            }
        }

        const auto* biped = a_obj->As<RE::BGSBipedObjectForm>();
        if (!biped) return {};
        const auto mask = static_cast<std::uint32_t>(biped->GetSlotMask().get());
        if (mask == 0) return {};

        // bit N is slot 30+N (BGSBipedObjectForm::BipedObjectSlot)
        auto named = [](int bit) -> Lang::Str {
            switch (bit) {
            case 0: case 1:  return Lang::Str::SlotHead;      // head / hair
            case 2:          return Lang::Str::SlotBody;
            case 3:          return Lang::Str::SlotHands;
            case 5:          return Lang::Str::SlotAmulet;
            case 6:          return Lang::Str::SlotRing;
            case 7:          return Lang::Str::SlotFeet;
            case 9:          return Lang::Str::SlotShield;
            case 12:         return Lang::Str::SlotCirclet;
            case 13:         return Lang::Str::SlotEars;
            default:         return Lang::Str::Count_;         // not one of ours
            }
        };

        std::string out;
        std::vector<int> extra;                  // slot numbers, in order
        std::vector<Lang::Str> seen;             // head+hair must not print twice
        for (int bit = 0; bit < 32; ++bit) {
            if (!(mask & (1u << bit))) continue;
            const Lang::Str id = named(bit);
            if (id == Lang::Str::Count_) { extra.push_back(30 + bit); continue; }
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
            seen.push_back(id);
            if (!out.empty()) out += " · ";
            out += Lang::T(id);
        }
        if (!extra.empty()) {
            if (!out.empty()) out += " · ";
            out += Lang::T(Lang::Str::SlotAccessory);
            out += " (";
            for (size_t i = 0; i < extra.size(); ++i) {
                if (i) out += " · ";
                out += std::to_string(extra[i]);
            }
            out += ")";
        }
        return out;
    }

    float SlotsTopOffset() { return g_slotsTop; }
    float SlotsTopScreen() { return g_slotsTopScreen; }

    // ---- accessory drawer -------------------------------------------------

    int PrimarySlot(RE::TESBoundObject* a_obj) { return PrimarySlotOf(a_obj); }

    int  DrawerCount() { return g_drawerCount; }
    bool DrawerOpen()  { return g_drawerOpen; }

    void SetDrawerOpen(bool a_open) { g_drawerOpen = a_open; }

    namespace
    {
        // ★★ONE SOURCE FOR THE DRAWER'S MEASUREMENTS, so the button in the
        // strip and the panel outside the window can never disagree about
        // which side it comes out of or how far it has travelled.
        //
        // ★bodyX/bodyR are the drawer BOX — where it is right now, most of it
        // usually still inside the cabinet. panelX/shownW are the window we
        // open onto it, which stops dead at the main window's edge. Keeping
        // the two apart is what makes this a drawer rather than an accordion:
        // cells are placed against the BOX and travel with it, while the
        // window merely reveals more of them.
        struct DrawerGeom
        {
            float slot, gap, pad;
            float panelW, panelH, shownW;
            float bodyX, bodyR;      // the box: left edge, right edge
            float panelX, rowTop, top;
            int   cols;
            bool  toRight;
        };

        // ★FIVE ROWS, ALWAYS. The strip beside the doll is five cells tall and
        // the drawer continues it, so a drawer holding two accessories is one
        // column with three empty cells -- which are drop targets, not waste.
        constexpr int kDrawerRows = 5;

        DrawerGeom DrawerGeomOf(const ImVec2& a_mainPos, const ImVec2& a_mainSize,
                                float a_rowTop)
        {
            DrawerGeom g{};
            const float S = Theme::Scale();
            g.slot = SlotPx();
            g.gap  = GapPx();
            // ★3px was the plan's number and it proved too mean in game -- the
            // cells sat right on the frame. 6 still reads as trim, and gives
            // the border somewhere to be. Deliberately far tighter than a bag
            // window: a title bar and generous padding are what would make
            // this read as a second window instead of an extension.
            g.pad  = 6.0f * S;

            const int n = (std::max)(0, g_drawerCount);
            g.cols   = (std::max)(1, (n + kDrawerRows - 1) / kDrawerRows);
            g.panelW = g.cols * g.slot + (g.cols - 1) * g.gap + 2 * g.pad;
            g.panelH = kDrawerRows * g.slot + (kDrawerRows - 1) * g.gap + 2 * g.pad;

            // Left by default; right when the screen runs out.
            g.toRight = (a_mainPos.x - g.panelW) < 0.0f;
            g.shownW  = g.panelW * g_drawerSlide;

            // ★★THE FIRST CELL LINES UP WITH THE DOLL'S FIRST SLOT ROW, not
            // the panel's frame. The drawer is an extension of the equipment
            // column, not a window parked beside it, so the two grids have to
            // start on one line -- and they did not: the panel's own top
            // padding lifted its cells a notch above the doll's (reported,
            // with a rule drawn across both to show it). Pull the FRAME up by
            // that padding rather than dropping the padding, which every other
            // edge still wants.
            g.rowTop = a_rowTop;
            g.top    = a_rowTop - g.pad;

            // ★★THE BOX SLIDES; THE OPENING DOES NOT GROW. The drawer body
            // starts fully inside the cabinet (its outer edge flush with the
            // window) and travels a full panel-width out. The window we open
            // onto it always ends exactly at the main window's edge, so the
            // part still inside is CLIPPED rather than drawn -- there is no
            // arrangement of alpha where it could show through the inventory.
            //
            // The old version moved the opening instead and pinned the cells
            // to it, which grew a column at a time from the inside: an
            // accordion, not a drawer.
            const float travel = g.panelW * g_drawerSlide;
            if (g.toRight) {
                g.bodyX  = a_mainPos.x + a_mainSize.x - g.panelW + travel;
                g.panelX = a_mainPos.x + a_mainSize.x;
            } else {
                g.bodyX  = a_mainPos.x - travel;
                g.panelX = g.bodyX;
            }
            g.bodyR = g.bodyX + g.panelW;

            g_drawerToRight = g.toRight;
            return g;
        }

        // ★★THE STRIP'S LAST CELL, AND IT IS NOT A SLOT. It sits among four
        // that are, so it has to say so in its own shape or players will try
        // to drop things on it: sunk ground instead of the slot's raised one,
        // and a solid mark where a slot would hold an icon.
        void DrawDrawerCell(float a_size)
        {
            const auto& sk = Theme::S();
            auto* dl = ImGui::GetWindowDrawList();

            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 p1(p0.x + a_size, p0.y + a_size);

            const bool clicked = ImGui::InvisibleButton("##gi_accdrawer_btn",
                                                        ImVec2(a_size, a_size));
            const bool hov = ImGui::IsItemHovered();

            // ★Sound on the EDGE, not on the state. IsItemHovered is true every
            // frame the cursor rests here, and playing on that fires the blip
            // dozens of times a second.
            static bool s_wasHov = false;
            if (hov && !s_wasHov) Sfx::Focus();
            s_wasHov = hov;

            if (clicked) {
                g_drawerOpen = !g_drawerOpen;
                // Opening is a confirm, closing is a dismiss -- the same pair
                // every other panel in this UI uses.
                if (g_drawerOpen) Sfx::SelectOn(); else Sfx::SelectOff();
            }

            // ★★AT REST IT IS A SLOT, to the pixel. The first cut gave it a
            // sunk ground and a darker rim of its own, and the result did not
            // read as "one of these is a button" — it read as a broken cell
            // (reported: "평상시에 색상부터 차이가 심하게 난다"). The cell face
            // comes from the same function every slot uses, so the difference
            // is carried entirely by what happens under the cursor.
            PaintCellFace(dl, p0, p1, "accdrawer", /*worn=*/false);

            // ★Pressing DROPS the highlight and releasing brings it back, so
            // the button dips under the finger. Without it a click had a sound
            // and no picture, which reads as a missed press on a slow frame.
            if (hov && !ImGui::IsItemActive()) {
                // ★Same half-pixel inset as every slot border -- a 2px stroke
                // on the path puts 1px outside, and on an edge cell that 1px
                // is past the clip.
                // The highlight the slots use when they accept something, so
                // the button answers the cursor the way its neighbours do.
                dl->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                            ImVec2(p1.x - 1.0f, p1.y - 1.0f),
                            Theme::Col(sk.hi, 0.8f), sk.rounding, 0, 2.0f);
            }

            // The mark: a solid triangle pointing where the drawer will go.
            // ★Sized and tinted like the silhouettes beside it — those are the
            // marks a resting cell wears, and this one is no louder.
            const float cx = (p0.x + p1.x) * 0.5f;
            const float cy = (p0.y + p1.y) * 0.5f;
            const float tw = a_size * 0.115f;   // half-width
            const float th = a_size * 0.165f;   // half-height
            const bool  pointLeft = g_drawerToRight ? g_drawerOpen : !g_drawerOpen;
            const float dx = pointLeft ? tw : -tw;
            const ImVec2 apex(cx - dx, cy);
            const ImVec2 top (cx + dx, cy - th);
            const ImVec2 bot (cx + dx, cy + th);

            // ★★★WINDING DECIDES WHETHER THE AA WORKS AT ALL, and this is why
            // the two states looked different (reported: the closed arrow was
            // clean, the open one stepped). ImGui's anti-aliased FILL assumes
            // CLOCKWISE points -- it walks the edge and lays the feather on
            // one nominated side. Mirroring the arrow by negating dx also
            // reverses the winding, so on one of the two states the feather
            // was laid on the INSIDE and the outer edge was left raw.
            // Flipping the order back means both states are drawn the same
            // way and both get the same edge.
            const ImVec2 v1 = pointLeft ? top : bot;
            const ImVec2 v2 = pointLeft ? bot : top;

            const ImU32 mark = CellHintCol();
            dl->AddTriangleFilled(apex, v1, v2, mark);
            // ★And stroke the same path. Even correctly wound, the fill's
            // feather is a single pixel; the LINE rasteriser is the better one
            // here (AntiAliasedLinesUseTex is on), so the outline in the same
            // colour hands it the edge.
            dl->AddTriangle(apex, v1, v2, mark, 1.5f);
        }
    }

    void DrawDrawer(const ImVec2& a_mainPos, const ImVec2& a_mainSize,
                    float a_rowTop)
    {
        if (!DrawerAvailable()) { g_drawerSlide = 0.0f; return; }

        // ---- the slide -----------------------------------------------------
        // One curve for both directions, so pressing the tab mid-open simply
        // reverses it instead of snapping.
        {
            const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 20.0f);
            const float target = g_drawerOpen ? 1.0f : 0.0f;
            constexpr float kSpeed = 7.0f;   // ~0.14s end to end
            if (g_drawerSlide < target) {
                g_drawerSlide = (std::min)(target, g_drawerSlide + dt * kSpeed);
            } else if (g_drawerSlide > target) {
                g_drawerSlide = (std::max)(target, g_drawerSlide - dt * kSpeed);
            }
        }

        const float S   = Theme::Scale();
        const auto  g   = DrawerGeomOf(a_mainPos, a_mainSize, a_rowTop);
        const float S2  = g.slot;
        const float gap = g.gap;
        const float pad = g.pad;

        // ---- the panel -----------------------------------------------------
        // Only while it is actually out: a zero-width window still draws its
        // border, which would leave a hairline glued to the main window.
        if (g.shownW <= 1.0f) return;

        ImGui::SetNextWindowPos(ImVec2(g.panelX, g.top), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(g.shownW, g.panelH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        // ★★SetNextWindowSize IS NOT THE LAST WORD -- WindowMinSize (32x32 by
        // default) clamps it afterwards. So the last 32px of the slide made a
        // window WIDER than the opening it was meant to be, and its right edge
        // pushed past the inventory's left edge: the drawer showed through the
        // panel exactly while closing, which is when nothing should be left of
        // it at all (reported). The clamp is a guard for windows a user
        // resizes; this one is driven frame by frame.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
        ImGui::Begin("##gi_accdrawer", nullptr, kManagedWinFlags);

        // ★The skin paints its own sheet, and it does that inside TitleBar --
        // which this panel deliberately has none of. Without asking for the
        // ground explicitly, an ink or torn skin left the cells floating over
        // the world with nothing behind them (reported). 0/0 = no title strip.
        // ★Painted over the whole BOX, not the visible slice: the sheet has to
        // travel with the drawer or the grain slides under the cells while
        // they move. The window clips whatever is still inside.
        // ★Fenced to the visible slice. PaintGround overrides the window clip
        // on purpose (chrome must reach the edge), so without a fence the
        // sheet for the whole box painted straight over the inventory.
        WinManager::GetSingleton()->PaintGround(
            ImGui::GetWindowDrawList(), ImVec2(g.bodyX, g.top),
            ImVec2(g.bodyR, g.top + g.panelH), "accdrawer", 0.0f, 0.0f,
            ImVec2(g.panelX, g.top - g.panelH),
            ImVec2(g.panelX + g.shownW, g.top + 2.0f * g.panelH));
        const auto& eq = EquipmentThisFrame();

        // ★Cells are nailed to the BOX. Column 0 is the innermost, so it is
        // the last to clear the cabinet -- pull the drawer and its far end
        // appears first, exactly like a real one. (Placing them against the
        // visible slice instead is what made the old version unfold.)
        // ★★COLUMN 0 IS THE INNER ONE, WHICHEVER WAY THE BOX TRAVELS. Laying
        // out from bodyR unconditionally worked only while the drawer opened
        // LEFT, where bodyR is the edge against the window; docked right that
        // same edge is the far one, so the first accessory jumped to the
        // outermost column and the slide revealed the columns in reverse. Same
        // worn set, mirrored layout, decided by nothing but where the window
        // happened to sit.
        for (int c = 0; c < g.cols; ++c) {
            const float cx = g.toRight
                ? (g.bodyX + pad + c * (S2 + gap))
                : (g.bodyR - pad - S2 - c * (S2 + gap));
            for (int r = 0; r < kDrawerRows; ++r) {
                const int idx = c * kDrawerRows + r;
                // ★The drawer's nth cell continues the doll's — and where the
                // doll stops is whatever CollectEquipment actually used, not a
                // number typed here twice.
                const std::string id =
                    "acc" + std::to_string(idx + g_drawerFirstIdx + 1);
                const SlotDef def{ id.c_str(), "acc" };
                // ★From the ROW, not from the frame -- g.top already carries
                // the padding offset, and adding it back here is what put the
                // grid a notch above the doll's.
                ImGui::SetCursorScreenPos(
                    ImVec2(cx, g.rowTop + r * (S2 + gap)));
                DrawSlot(def, S2, S2, eq);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);   // WindowMinSize + WindowPadding
    }


    bool DrawerAvailable()
    {
        // The doll is not on screen during loot/barter -- the partner window
        // has that side -- so a drawer hanging off it would belong to
        // something the player cannot see. kNormal is the only mode that
        // shows the doll, so it is the only one that gets a drawer.
        return LootBarter::CurrentMode() == LootBarter::Mode::kNormal;
    }

    void OnMenuClosed()
    {
        g_buyOpen = false;
        g_delTarget = -1;
    }

    void Draw()
    {
        DrawLoadoutTabs();   // L2: loadout tab strip (switch / + buy / rename / delete / wheel)

        const auto& eq = EquipmentThisFrame();

        const float S2 = SlotPx();
        const float gap = GapPx();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        // GI77: the item grid pads itself down to this so both columns begin on
        // one line. Read from the cursor the tabs left behind, not computed —
        // frame height and item spacing are style values, not ours.
        g_slotsTop = start.y - ImGui::GetWindowPos().y;
        g_slotsTopScreen = start.y;

        // vertical strip: circlet + acc x3, then the drawer button
        // (total height == doll height)
        for (int i = 0; i < 4; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + i * (S2 + gap)));
            DrawSlot(kStrip[i], S2, S2, eq);
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 4 * (S2 + gap)));
        DrawDrawerCell(S2);

        // doll: 3 columns, rows [2u, tall, 2u, 2u]
        const float rowY[4] = {
            start.y,
            start.y + S2 + gap,
            start.y + S2 + gap + TallPx() + gap,
            start.y + S2 + gap + TallPx() + gap + S2 + gap,
        };
        for (int r = 0; r < 4; ++r) {
            const float h = (r == 1) ? TallPx() : S2;
            for (int c = 0; c < 3; ++c) {
                ImGui::SetCursorScreenPos(
                    ImVec2(start.x + S2 + gap + c * (S2 + gap), rowY[r]));
                DrawSlot(kDoll[r][c], S2, h, eq);
            }
        }

        // reserve extent for the layout cursor
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(PanelW(), 3 * S2 + TallPx() + 3 * gap));
    }
}
