#include "game/Costume.h"

#include "api/HostApi.h"
#include "ui/Loadout.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace FUI::Costume
{
    namespace
    {
        using BO = RE::BIPED_OBJECTS::BIPED_OBJECT;
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

        // Editor slots 0..31 are the ones an ARMO can occupy; 32+ are weapons.
        constexpr std::uint32_t kSlots = BO::kEditorTotal;

        int  g_tab = -1;          // checked tab, -1 = none
        int  g_appliedTab = -2;   // what the body is currently wearing (-2 = unknown)
        int  g_toldApi = -2;      // what the outside world was last told (-2 = nothing yet)

        // ★★★HOW A REBUILD IS SEEN. Not the biped pointer -- the engine reuses
        // that (measured: the same pointer over 11s), so a rebuild is invisible
        // there. `BIPOBJECT::addon` is not: it names the ARMA actually hanging
        // in that slot right now. We dress a slot, note what ended up in it,
        // and if the engine later puts something else there it has rebuilt the
        // body from the restored lists -- which is the bald head.
        // ★A pointer kept for COMPARISON only, never dereferenced.
        int         g_watchSlot = -1;
        const void* g_watchAddon = nullptr;

        // Post-load re-dress schedule -- the belt to the watch's braces. The
        // watch catches a rebuild in one frame; this covers the case where a
        // rebuild leaves the same addon in place and the watch sees nothing.
        constexpr int kReapplyTimes = 3;
        constexpr int kReapplyFirst = 30;    // ~0.5s after the load lands
        constexpr int kReapplyEvery = 90;    // ~1.5s between the rest
        int g_reapplyLeft = 0;
        int g_reapplyAt = 0;
        bool g_dirty = false;

        // ---- the anchor (PLAN_COSTUME §5) ----------------------------------
        // A costume piece reaches the body by borrowing a WORN item's addon
        // list, so a slot with nothing in it cannot be dressed. The anchor is
        // the answer: put something in the slot purely so there is a list.
        //
        // ★It is our own record, not a borrowed helmet: 0 rating, 0 weight,
        // 0 value, no keywords, no model. Stage 1 proved the plumbing with a
        // real helmet out of the player's pack, which worked but applied that
        // helmet's stats -- exactly what this feature promises not to do.
        //
        // ★★EIGHT carriers, one addon. A costume can leave several slots bare at
        // once (helmet AND amulet AND ring AND cape), and one carrier cannot
        // serve them: armorAddons is overwritten per-ARMO, so two pieces sharing
        // a carrier would have to share an appearance. The ARMA is shared freely
        // -- it is never rendered, only replaced.
        // ★24, not 8. Eight was a guess at how many slots a costume leaves
        // bare, and a real set overran it: a 17-piece outfit needed 11 and the
        // last three simply did not appear -- silently, apart from one warning
        // in the log.
        // ★★★32, not 24 -- and "24 is every slot a costume can cover" was
        // simply wrong when it was written. BipedObjectSlot spans 32 bits and
        // CoversSlot excludes only the shield, so 31 pieces can each want a
        // slot of their own; 24 truncated seven of them.
        // What makes that reachable rather than theoretical is the cost model:
        // an anchor is spent per distinct donor ARMA, NOT per slot. A suit of
        // armour covering body+forearms+hands costs ONE. Jewellery is the
        // opposite -- one ARMA, one slot -- so a multi-ring mod, which gives
        // every ring a custom slot of its own, spends one anchor per ring.
        // Eight rings on top of the 11 that 17-piece outfit needed is 19, and a
        // cloak, a backpack and a lantern take it past 24.
        // At 32 the array can no longer be the shortage: Apply() can build at
        // most 31 groups, so the warning below is now unreachable by counting
        // (it stays as the guard against that ever ceasing to be true).
        // ★★...which is exactly why the COSTUME stops at 31: the 32nd record
        // (0x84A) was the second ring's carrier. The costume could not want it
        // -- `need` is `j < groups.size()` and groups tops out at 31 -- but its
        // loop would still DROP one it found held, so the count was lowered
        // rather than shared. The reservation is free: nothing is taken from
        // the costume.
        // ★★★AND IT STAYS RESERVED AFTER 1.6.0, though nothing uses it. The
        // carrier's ESP record is kept (deleting it would break the references
        // in existing saves), and an old save can still be WEARING one until
        // SweepRetiredCarrier runs. Promoting 0x84A to an ordinary anchor
        // would put this system in a fight with that leftover over the same
        // form -- for a 32nd group Apply can never build anyway.
        // ★They cost nothing to carry: no model, no keywords, 0/0/0, and only
        // the ones a costume actually needs are ever equipped.
        constexpr std::uint32_t kAnchorFirst = 0x82B;
        constexpr int           kAnchorCount = 31;   // 32nd: the retired carrier
        // ★★...but ALL 32 are ours to hide. IsAnchor is what keeps these off
        // the grid, the doll, the capacity count and every transfer, and the
        // carrier needs exactly the same treatment -- it is no more the
        // player's property than an anchor is. Counting only 31 here would
        // have put a nameless 0/0/0 helmet in the player's bag, and that is
        // still true of a leftover one an old save is carrying.
        constexpr int           kAnchorTotal = 32;
        constexpr const char*   kPlugin = "Grid Inventory.esp";

        int  g_anchorTries = 0;        // give up rather than loop forever
        bool g_anchorGaveUp = false;   // ...but say so exactly once

        // Rebuilds are spaced a few frames apart. ★Not the fix for anything --
        // the invisibility had a different cause (see Apply) and this did not
        // prevent it. Kept because a rebuild per frame is wasted work when the
        // player is clicking quickly, and 8 frames is below notice.
        std::uint64_t           g_frame = 0;
        std::uint64_t           g_lastRebuild = 0;
        constexpr std::uint64_t kRebuildGap = 8;     // ~0.13s at 60fps


        [[nodiscard]] RE::TESObjectARMO* AnchorForm(int a_i)
        {
            // Not a function-local static initialiser: a miss must be
            // retryable. This can be reached before the data handler has the
            // plugin, and caching a null there would disable the feature for
            // the session with no way back.
            static std::array<RE::TESObjectARMO*, kAnchorTotal> cached{};
            if (a_i < 0 || a_i >= kAnchorTotal) return nullptr;
            if (!cached[a_i]) {
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    cached[a_i] = dh->LookupForm<RE::TESObjectARMO>(
                        kAnchorFirst + a_i, kPlugin);
                }
            }
            return cached[a_i];
        }

        [[nodiscard]] int AnchorIndexOf(const RE::TESForm* a_form)
        {
            if (!a_form) return -1;
            // ★kAnchorTotal, not kAnchorCount: this answers "is this ours to
            // hide", which covers the retired second-ring carrier too.
            for (int i = 0; i < kAnchorTotal; ++i) {
                auto* f = AnchorForm(i);
                if (f && f->GetFormID() == a_form->GetFormID()) return i;
            }
            return -1;
        }

        [[nodiscard]] Slot SlotOf(std::uint32_t a_i)
        {
            return static_cast<Slot>(1u << a_i);
        }

        // One armour's addon list, saved so the shared form can be put back.
        struct Backup
        {
            RE::TESObjectARMO*              armo = nullptr;
            std::vector<RE::TESObjectARMA*> saved;
        };

        // A form whose slot mask was changed for the length of one rebuild --
        // a costume ARMA widened to reach its carrier, or a carrier narrowed to
        // stop claiming slots the costume does not use. Both are the same
        // operation on the same base class, and both must be put back.
        struct MaskEdit
        {
            RE::BGSBipedObjectForm*          form = nullptr;
            RE::BIPED_MODEL::BipedObjectSlot saved{};
        };

        // ★Restores every borrowed list however the scope is left. armorAddons
        // lives on the shared form -- an early return or a throw between the
        // swap and the rebuild would leave every actor wearing that armour
        // dressed as the player's costume until the game is restarted.
        struct RestoreAll
        {
            std::vector<Backup>&   list;
            std::vector<MaskEdit>& edits;
            ~RestoreAll()
            {
                for (auto& b : list) {
                    if (!b.armo) continue;
                    // Element-wise, never a resize: the array belongs to the
                    // game's allocator and the size is identical either way.
                    for (std::uint32_t i = 0; i < b.armo->armorAddons.size() &&
                                              i < b.saved.size(); ++i) {
                        b.armo->armorAddons[i] = b.saved[i];
                    }
                }
                // ★★Assign the saved value back, never AddSlotToMask /
                // RemoveSlotFromMask. Those helpers set or clear a bit
                // unconditionally, so "undo what I did" also strips a slot the
                // form legitimately owned -- the bug that once dropped worn
                // armour into an accessory slot.
                for (auto& e : edits) {
                    if (e.form) e.form->bipedModelData.bipedObjectSlots = e.saved;
                }
            }
        };
    }

    // ---- state -----------------------------------------------------------

    int  Tab() { return g_tab; }
    bool IsTab(int a_index) { return a_index >= 0 && a_index == g_tab; }

    bool CanBeTab(int a_index)
    {
        // ★Tab 0 is EQUIP, not a preset -- it is the base set, and it has no
        // stored list of its own to read an outfit out of. The checkbox must
        // not appear on it at all.
        if (a_index < 1 || a_index >= Loadout::Count()) return false;
        // Dressing as the set you are already wearing changes nothing, and the
        // active tab's stored list is a stale snapshot anyway (Loadout::FormsOf).
        return a_index != Loadout::Active();
    }

    void SetTab(int a_index)
    {
        const int want = (a_index >= 0 && CanBeTab(a_index)) ? a_index : -1;
        if (want == g_tab) return;
        g_tab = want;   // one at a time: assigning IS clearing the previous
        MarkDirty();
        SKSE::log::info("[COSTUME] tab -> {}", g_tab);
    }

    void OnTabRemoved(int a_index)
    {
        if (g_tab < 0) return;
        if (g_tab == a_index) {
            g_tab = -1;
            MarkDirty();
            SKSE::log::info("[COSTUME] costume tab was deleted -- costume cleared");
        } else if (g_tab > a_index) {
            g_tab -= 1;   // the tabs above it shifted down
            SKSE::log::info("[COSTUME] costume tab index shifted to {}", g_tab);
        }
    }

    // ---- coverage ---------------------------------------------------------

    bool CoversSlot(Slot a_slot)
    {
        // ★SHIELD is the only exclusion that can be expressed here. It is a
        // real ARMO slot, but it is held gear: a shield that does not match the
        // one you are blocking with reads as the same mistake a weapon costume
        // would.
        //
        // ★The QUIVER needs no rule. BipedObjectSlot only reaches 1<<31 (kFX01)
        // — biped objects 0..31 — so the quiver (41) has no bit at all and no
        // armour can claim it. Its look comes from the equipped ammo, which
        // this system never touches. (Writing the check anyway shifted 1u by 41
        // and was undefined behaviour, caught by the compiler.)
        return a_slot != Slot::kShield;
    }

    bool CoversArmor(RE::TESObjectARMO* a_armor)
    {
        if (!a_armor) return false;
        for (std::uint32_t i = 0; i < kSlots; ++i) {
            const auto s = SlotOf(i);
            if (a_armor->HasPartOf(s) && CoversSlot(s)) return true;
        }
        return false;
    }

    // ---- applying ---------------------------------------------------------

    void MarkDirty() { g_dirty = true; }

    void NoteGameLoaded()
    {
        // ★SAY IT AGAIN AFTER A LOAD, even when the tab happens to match what
        // the last session ended on. A listener's own state did not survive the
        // load either, so "unchanged" is the wrong thing to tell it -- the next
        // tick re-announces whatever this save holds.
        g_toldApi = -2;
        g_reapplyLeft = kReapplyTimes;
        g_reapplyAt = g_frame + kReapplyFirst;
        SKSE::log::info("[COSTUME] load: {} re-dress passes scheduled", kReapplyTimes);
    }

    namespace
    {
        // ★★ASK AGAIN UNTIL IT CAN BE ANSWERED. The sweep runs from a task off
        // kPostLoadGame, and the 3D guard below can send it away empty-handed
        // on a body the engine has not finished building. A one-shot sweep
        // that missed would leave the carrier on for the whole session -- worn,
        // hidden by IsAnchor, and so impossible to take off by hand -- so a
        // miss stays ARMED and Tick asks again on the next frame that has a
        // body. Set false only once the world could actually be read.
        bool g_sweepCarrier = false;

        void SweepCarrierNow()
        {
            // ★Anchor 32 -- the index the costume deliberately never reaches.
            // Resolved through the same cache, so no second lookup path can
            // drift from it. See kAnchorCount / kAnchorTotal above.
            auto* carrier = AnchorForm(kAnchorTotal - 1);
            auto* p       = RE::PlayerCharacter::GetSingleton();
            if (!carrier || !p || !p->Is3DLoaded()) return;   // stays armed
            g_sweepCarrier = false;

            // ★Ask the INVENTORY, not a saved flag. The cosave record that
            // named the ring is discarded on load, and it was never the
            // authority anyway: whether the carrier is on the body is a fact
            // about the world, and the world can change behind our back.
            bool worn = false;
            int  held = 0;
            for (const auto& [obj, data] : p->GetInventory(
                     [&](RE::TESBoundObject& o) { return &o == carrier; })) {
                held = data.first;
                worn = data.second && data.second->IsWorn();
                (void)obj;
            }
            if (held <= 0) return;

            if (worn) {
                if (auto* em = RE::ActorEquipManager::GetSingleton()) {
                    em->UnequipObject(p, carrier, nullptr, 1, nullptr,
                                      false, false, false, true);
                }
            }
            // Count high enough to sweep the duplicates a crossed save could
            // leave -- the retired code carried the same allowance.
            p->RemoveItem(carrier, 99, RE::ITEM_REMOVE_REASON::kRemove,
                          nullptr, nullptr);
            SKSE::log::info("[COSTUME] retired second-ring carrier removed "
                            "({} held, worn={}) -- 1.6.0 migration",
                            held, worn ? 1 : 0);
        }
    }

    void SweepRetiredCarrier()
    {
        g_sweepCarrier = true;
        SweepCarrierNow();   // usually done right here; Tick covers the rest
    }

    namespace
    {
        // The costume addon a WORN armour will lend its list to, by the one rule
        // both the anchor planner and the dressing loop must agree on: the
        // lowest slot the armour claims that the costume has a piece for.
        // ★They used to be two copies of this walk. When they disagree the
        // failure is invisible -- an anchor gets raised for a piece another
        // carrier is already showing, and the same helmet renders twice.
        [[nodiscard]] RE::TESObjectARMA* DonorFor(
            RE::TESObjectARMO* a_armo,
            const std::array<RE::TESObjectARMA*, kSlots>& a_donor,
            std::uint32_t* a_covered = nullptr, std::uint32_t* a_at = nullptr)
        {
            std::uint32_t      covered = 0;
            RE::TESObjectARMA* want = nullptr;
            for (std::uint32_t i = 0; i < kSlots; ++i) {
                const auto s = SlotOf(i);
                if (!a_armo->HasPartOf(s) || !CoversSlot(s)) continue;
                covered |= 1u << i;
                if (!want && a_donor[i]) {
                    want = a_donor[i];
                    if (a_at) *a_at = i;
                }
            }
            if (a_covered) *a_covered = covered;
            return want;
        }

        // Put the anchor into a_slots and wear it. The mask is ours to set --
        // no other actor wears this form, so unlike a costume ARMA there is
        // nothing to restore afterwards.
        void RaiseAnchor(RE::PlayerCharacter* a_p, int a_i, RE::TESObjectARMO* a_anchor,
                         std::uint32_t a_slots, bool a_held, bool a_worn)
        {
            auto* em = RE::ActorEquipManager::GetSingleton();
            if (!em) return;
            // Moving to different slots: take it off first, but keep it in the
            // pack. Re-equipping over itself with a changed mask leaves the biped
            // holding the old slot.
            if (a_worn) {
                em->UnequipObject(a_p, a_anchor, nullptr, 1, nullptr,
                                  false, false, false, true);
            }
            a_anchor->bipedModelData.bipedObjectSlots = static_cast<Slot>(a_slots);
            if (!a_held) a_p->AddObjectToContainer(a_anchor, nullptr, 1, nullptr);
            em->EquipObject(a_p, a_anchor, nullptr, 1, nullptr, false, false, false, true);
            SKSE::log::info("[COSTUME] anchor {} raised on slots 0x{:08X}", a_i, a_slots);
        }

        // ★Take it out of the inventory, not just off the body. It is not the
        // player's item; leaving it in the pack would put a phantom in every
        // save made with the costume off.
        // ★a_why is not decoration. Four different situations drop the anchor and
        // three of them are indistinguishable from a bug in the log otherwise --
        // "the costume changed its mind" reads exactly like "the anchor fell off".
        void DropAnchor(RE::PlayerCharacter* a_p, int a_i, RE::TESObjectARMO* a_anchor,
                        const std::string& a_why)
        {
            if (!a_anchor) return;
            if (auto* em = RE::ActorEquipManager::GetSingleton()) {
                em->UnequipObject(a_p, a_anchor, nullptr, 1, nullptr,
                                  false, false, false, true);
            }
            // count high enough to sweep duplicates a crossed save could leave
            a_p->RemoveItem(a_anchor, 99, RE::ITEM_REMOVE_REASON::kRemove,
                            nullptr, nullptr);
            SKSE::log::info("[COSTUME] anchor {} dropped -- {}", a_i, a_why);
        }
    }

    bool IsAnchor(const RE::TESForm* a_form)
    {
        return AnchorIndexOf(a_form) >= 0;
    }

    void Apply()
    {
        g_dirty = false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        if (!player->Is3DLoaded()) return;   // nothing to dress yet
        auto* race = player->GetRace();
        if (!race) return;
        // ★★NEVER DRESS A TRANSFORMED PLAYER. A costume hangs its pieces on
        // anchors worn in biped slots, and a werewolf or a vampire lord is a
        // body those slots do not belong to -- dressing it is how a beast ends
        // up wearing a cuirass, or loses the parts of itself that share a slot.
        //
        // ★And this is NOT prevented by the inventory being unavailable in
        // beast form, which is the reassuring answer and the wrong one. Nothing
        // here is driven by the menu: Tick runs on the main-thread hook every
        // frame, open or closed, and the watch below asks whether the body was
        // rebuilt from under us. TRANSFORMING IS EXACTLY THAT REBUILD -- it is
        // the surest way there is to trip the very watchdog that calls this.
        //
        // The request is kept, not dropped: the same watch fires again when the
        // form ends and the real body comes back, and by then this gate opens.
        static bool s_beastNoted = false;
        if (!race->GetPlayable()) {
            if (!s_beastNoted) {
                s_beastNoted = true;
                // ★★A TRANSFORMATION TAKES THE COSTUME OFF. Holding it back
                // and putting it on again afterwards was the first answer, and
                // it bought a race we do not need to run: the form ends, the
                // engine is still rebuilding the body out of the addon lists it
                // took apart for the beast, and an anchor dressed into that
                // half-built body ends up worn, holding its slot, with no model
                // in it -- the bald head (user report: costume still ticked,
                // appearance plain and bald, cured by unticking it). Timing it
                // right means guessing when the actor has settled, which is the
                // guess §NoteGameLoaded already has to make once per load.
                //
                // Clearing is the simpler contract and the safer one: a costume
                // is a thing you put ON, and turning into a beast is exactly
                // the kind of event that should end it. The tick goes away, the
                // player sees why, and putting it back on is one click on a
                // body that is finished being built.
                if (g_tab >= 0) {
                    SKSE::log::info("[COSTUME] transformed into '{}' -- costume "
                                    "cleared (put it back on after the form ends)",
                        race->GetName() ? race->GetName() : "?");
                    g_tab = -1;
                }
            }
            // ★The BODY still has to be put right, and not while it is a
            // beast's: keep the request standing so the pass below runs the
            // moment there is a real body again. The anchors come back worn
            // when the engine restores what the player had on, and an anchor
            // worn with no costume behind it is the same bald head from the
            // other direction -- undressing is what takes it off.
            g_dirty = true;
            return;
        }
        if (s_beastNoted) {
            s_beastNoted = false;
            // The form ended. Same belt as a load: the body is still being
            // assembled, so make the plain state stick over several passes
            // rather than betting on one, and forget what we think it is
            // wearing so each pass actually runs.
            g_reapplyLeft = kReapplyTimes;
            g_reapplyAt = g_frame + kReapplyFirst;
            g_appliedTab = -2;
            SKSE::log::info("[COSTUME] form ended -- {} passes scheduled to put "
                            "the body back to plain", kReapplyTimes);
        }
        auto& rt = player->GetActorRuntimeData();
        auto* biped = rt.biped.get();
        if (!biped) return;

        // ★Switching INTO the checked tab clears it. The rule "the active tab
        // cannot be the costume" was only enforced one way before: you could
        // not tick the active tab, but ticking one and then activating it left
        // a checked box that could not be unticked (its own checkbox is
        // disabled). Clearing here makes the rule true from both directions,
        // and it loses nothing -- dressing as the set you are wearing shows
        // exactly what you would see anyway.
        if (g_tab >= 0 && g_tab == Loadout::Active()) {
            SKSE::log::info("[COSTUME] tab {} became the worn set -- costume cleared", g_tab);
            g_tab = -1;
        }

        const bool wanted = g_tab >= 0 && CanBeTab(g_tab);
        if (!wanted && g_appliedTab == -1) return;   // already plain, nothing to do

        // ★Space the rebuilds out. Equipping an anchor also makes the engine
        // rebuild, so this gate is at the top rather than around DoReset3D --
        // both kinds of work have to stay outside each other's window.
        if (g_frame - g_lastRebuild < kRebuildGap) {
            g_dirty = true;   // try again once the last rebuild has settled
            return;
        }

        // ---- what the costume wants in each slot --------------------------
        std::array<RE::TESObjectARMA*, kSlots> donor{};
        // ★The costume ARMO's own mask, kept per slot. It answers "which slots
        // does this piece actually use", which is what decides whether a carrier
        // may keep claiming the rest -- a hood claiming Hair keeps the engine
        // from drawing hair even when the costume only wanted the circlet.
        std::array<std::uint32_t, kSlots>      donorMask{};
        if (wanted) {
            for (const auto id : Loadout::FormsOf(g_tab)) {
                auto* form = RE::TESForm::LookupByID(id);
                auto* armo = form ? form->As<RE::TESObjectARMO>() : nullptr;
                if (!armo) {
                    // ★DIAG: a tab holds weapons and ammo too; those are simply
                    // not costume material. Named so "my helmet did nothing"
                    // can be told from "my helmet was never in the list".
                    SKSE::log::info("[COSTUME]   src 0x{:08X} {} -- not armour, skipped",
                        id, form ? form->GetName() : "(missing)");
                    continue;
                }
                // ★★Ask the ARMO for its addon ONCE, then register it for every
                // slot the ARMO claims. Looking it up per slot with
                // GetArmorAddonByMask fails whenever the ARMA's own mask is
                // narrower than the ARMO's: an Iron Helmet is Hair+Circlet as an
                // armour, but its addon only claims Hair, so asking for the
                // Circlet slot returned nothing and a worn crown got no costume.
                auto* arma = armo->GetArmorAddon(race);
                if (!arma) {
                    SKSE::log::info("[COSTUME]   src '{}' has no addon for this race",
                        armo->GetName());
                    continue;
                }
                std::string slots;
                for (std::uint32_t i = 0; i < kSlots; ++i) {
                    const auto s = SlotOf(i);
                    if (!armo->HasPartOf(s)) continue;
                    slots += std::format("{} ", i);
                    if (donor[i] || !CoversSlot(s)) continue;
                    donor[i] = arma;
                    donorMask[i] = static_cast<std::uint32_t>(armo->GetSlotMask().get());
                }
                // ★Log BOTH masks. They disagree far more often than they look
                // like they should, and every "the costume did nothing" bug so
                // far has been readable in the gap between these two numbers.
                SKSE::log::info("[COSTUME]   src '{}' covers slots: {} (armo 0x{:08X}, "
                                "addon 0x{:08X})",
                    armo->GetName(), slots.empty() ? "(none)" : slots,
                    static_cast<std::uint32_t>(armo->GetSlotMask().get()),
                    static_cast<std::uint32_t>(arma->GetSlotMask().get()));
            }
        }
        auto* skin = player->GetSkin();

        // ---- what the player is actually wearing ---------------------------
        // ★★★Read the INVENTORY, never the biped. The biped is last frame's
        // render, and while a menu is open the engine does not refresh it at
        // all -- an item equipped this tick simply never appears there. It also
        // lies about multi-slot gear: an armour's ARMO sits in the one slot that
        // owns the 3D and the others hold its ARMA as a marker, so a crown whose
        // primary slot is 55 looks like nothing is on the circlet slot.
        //
        // Both defects had the same visible shape -- "the costume did nothing to
        // my head" -- and both disappear here: IsWorn() is true the instant the
        // equip lands, and GetSlotMask() names every slot the item claims.
        struct AnchorState
        {
            RE::TESObjectARMO* form = nullptr;
            bool               held = false;
            bool               worn = false;
            std::uint32_t      mask = 0;
        };
        std::array<AnchorState, kAnchorCount> anc{};
        for (int j = 0; j < kAnchorCount; ++j) anc[j].form = AnchorForm(j);

        std::vector<RE::TESObjectARMO*> worn;          // anchors belong here
        std::uint32_t                   wornMask = 0;  // ...but not in here
        for (const auto& [obj, data] : player->GetInventory(
                 [](RE::TESBoundObject& o) { return o.IsArmor(); })) {
            if (data.first <= 0 || !data.second) continue;
            auto* a = obj->As<RE::TESObjectARMO>();
            // ★The SKIN is worn like gear but it is the body, not an outfit.
            // Borrowing its addon list would rewrite the player's own skin form.
            if (!a || a == skin) continue;
            // ★★Ask the inventory, keep no state. Whether an anchor is held or
            // worn is a fact about the world, and the world can change behind
            // this system's back -- a save made with the costume on, loaded with
            // it off, would strand remembered state. Observing instead means the
            // stale case repairs itself on the next tick.
            const int ai = AnchorIndexOf(a);
            if (ai >= 0) {
                // ★★AnchorIndexOf answers for 32 records but anc[] holds 31 --
                // index 31 is the retired second-ring carrier, which this
                // system must neither track nor touch. It still `continue`s, so
                // a leftover one stays out of wornMask exactly like an anchor:
                // the costume must not treat it as gear occupying a slot.
                if (ai < kAnchorCount) {
                    anc[ai].held = true;
                    anc[ai].mask = static_cast<std::uint32_t>(a->GetSlotMask().get());
                    if (data.second->IsWorn()) { anc[ai].worn = true; worn.push_back(a); }
                }
                continue;
            }
            if (!data.second->IsWorn()) continue;
            worn.push_back(a);
            // ★wornMask answers "would an anchor displace something?", so the
            // anchors themselves must not count -- they would look like rivals.
            wornMask |= static_cast<std::uint32_t>(a->GetSlotMask().get());
        }

        // ---- anchors: fill the slots the costume wants and nothing occupies --
        // ★Equipping and dressing must not happen in the same tick. Equipping
        // fires the events that mark this dirty in the first place, and doing
        // 3D work on a body the engine is still rebuilding is how you get a
        // half-built actor. So this phase only changes equip state and returns;
        // the NEXT tick sees the settled result and dresses it.
        {
            // Which costume pieces already have a carrier. ★Computed with the
            // very function the dressing loop uses -- a piece that a worn armour
            // will show must not ALSO get an anchor, or it renders twice.
            std::vector<RE::TESObjectARMA*> covered;
            if (wanted) {
                for (auto* a : worn) {
                    if (AnchorIndexOf(a) >= 0) continue;   // planning them now
                    if (auto* d = DonorFor(a, donor)) covered.push_back(d);
                }
            }

            // One group per costume piece that has nowhere to go: the slots it
            // wants, minus anything real gear already claims. Grouping by ADDON
            // rather than by slot is what keeps a Hair+Circlet helmet on one
            // anchor instead of two.
            std::vector<std::pair<RE::TESObjectARMA*, std::uint32_t>> groups;
            if (wanted) {
                for (std::uint32_t i = 0; i < kSlots; ++i) {
                    auto* d = donor[i];
                    if (!d) continue;
                    if (wornMask & (1u << i)) continue;   // real gear is there
                    if (std::find(covered.begin(), covered.end(), d) != covered.end())
                        continue;                          // already on the body
                    auto it = std::find_if(groups.begin(), groups.end(),
                        [&](const auto& g) { return g.first == d; });
                    if (it == groups.end()) groups.emplace_back(d, 1u << i);
                    else it->second |= 1u << i;
                }
            }

            if (static_cast<int>(groups.size()) > kAnchorCount) {
                // ★Never truncate in silence. Say exactly what will not appear.
                SKSE::log::warn("[COSTUME] {} costume pieces need an anchor but only {} "
                                "exist -- the last {} will not show",
                    groups.size(), kAnchorCount,
                    groups.size() - static_cast<std::size_t>(kAnchorCount));
                groups.resize(kAnchorCount);
            }

            bool changed = false;
            for (int j = 0; j < kAnchorCount; ++j) {
                const bool need = j < static_cast<int>(groups.size());
                const std::uint32_t want = need ? groups[j].second : 0u;
                // Already exactly right: leave it alone. Re-raising an anchor
                // that is doing its job costs a rebuild and a visible blink.
                if (need && anc[j].worn && anc[j].mask == want) continue;

                if (!need) {
                    if (anc[j].held) {
                        DropAnchor(player, j, anc[j].form,
                            !wanted        ? std::string("the costume is off")
                            : groups.empty() ? std::string("nothing needs an anchor")
                                           : std::string("fewer pieces need one now"));
                        changed = true;
                    }
                    continue;
                }
                if (!anc[j].form) {
                    SKSE::log::warn("[COSTUME] need an anchor for slots 0x{:08X} but form "
                                    "{} is missing -- is Grid Inventory.esp loaded?",
                        want, j);
                    continue;
                }
                if (g_anchorTries >= 3) {
                    if (!g_anchorGaveUp) {
                        // ★Say it. Retries followed by a silently bare slot is
                        // precisely the shape of failure that cost days here.
                        SKSE::log::warn("[COSTUME] anchors raised {}x but never became "
                                        "worn -- dressing without them, those slots stay "
                                        "bare", g_anchorTries);
                        g_anchorGaveUp = true;
                    }
                    continue;
                }
                RaiseAnchor(player, j, anc[j].form, want, anc[j].held, anc[j].worn);
                changed = true;
            }
            if (changed) {
                ++g_anchorTries;
                g_dirty = true;
                return;   // they land next tick; dress them then
            }
            g_anchorTries = 0;
            g_anchorGaveUp = false;
        }

        // ---- borrow the worn armour's appearance lists ---------------------
        std::vector<Backup>        backups;
        std::vector<MaskEdit>      edits;
        RestoreAll restore{ backups, edits };

        // Change a form's claimed slots for this rebuild only. First save wins:
        // the same form can be touched twice, and restoring the second (already
        // edited) value would make the change permanent.
        const auto reclaim = [&edits](RE::BGSBipedObjectForm* a_form,
                                      std::uint32_t a_mask) {
            if (std::none_of(edits.begin(), edits.end(),
                             [&](const MaskEdit& e) { return e.form == a_form; })) {
                edits.push_back({ a_form, a_form->GetSlotMask().get() });
            }
            a_form->bipedModelData.bipedObjectSlots = static_cast<Slot>(a_mask);
        };
        // ★Every slot the costume is going to fill, worked out BEFORE the loop.
        // It has to be complete on the first iteration: the carrier that gets
        // the skin may well be processed before the carrier wearing the chest,
        // and a set built up as we go would fence the skin against a costume
        // that had not been laid down yet.
        std::uint32_t dressed = 0;
        for (std::uint32_t i = 0; i < kSlots; ++i) {
            if (donor[i]) dressed |= 1u << i;
        }
        if (wanted) {
            for (auto* armo : worn) {
                if (armo->armorAddons.size() == 0) continue;

                // ★★Match against EVERY slot the armour claims, not just the
                // one the biped filed it under. A crown that is Circlet+55 gets
                // filed under 55, and looking there alone never reached the
                // costume's helmet on slot 12 -- the accessory was hidden and
                // nothing replaced it.
                std::uint32_t      covered = 0;   // slots we are allowed to touch
                std::uint32_t      at = 0;
                RE::TESObjectARMA* want = DonorFor(armo, donor, &covered, &at);
                if (!covered) continue;   // a shield, or nothing we dress
                const bool fromCostume = want != nullptr;

                // ★An empty costume slot becomes SKIN, not nothing. Blanking
                // the list would take the limb with it -- an armour's addon
                // carries that part of the body, which is why culling gloves
                // removed the hands.
                if (!want && skin) {
                    for (std::uint32_t i = 0; i < kSlots; ++i) {
                        if (!(covered & (1u << i))) continue;
                        want = skin->GetArmorAddonByMask(race, SlotOf(i));
                        if (want) { at = i; break; }
                    }
                    // ★★★AND FENCE IT IN. A body replacer's skin addon is ONE
                    // mesh for the whole body, and it claims every slot that
                    // body covers -- on UBE that is body + forearms + hands
                    // together. Lend it to a bare HANDS carrier and the engine
                    // puts it on all three, painting bare skin straight over
                    // the costume's chest. Borrowing it to fill one empty slot
                    // emptied two full ones.
                    // ★It is the SKIN that gets narrowed, not the costume that
                    // gets moved: the costume's claim is the correct one, and
                    // the skin is only here to keep a limb from vanishing.
                    // Restored with every other mask edit by RestoreAll.
                    if (want) {
                        const auto sm = static_cast<std::uint32_t>(want->GetSlotMask().get());
                        if (sm & dressed) {
                            reclaim(want, sm & ~dressed);
                            SKSE::log::info("[COSTUME]     skin addon fenced: 0x{:08X} -> "
                                            "0x{:08X} (costume owns 0x{:08X})",
                                sm, sm & ~dressed, dressed);
                        }
                    }
                }

                // ★★...but skin only exists for slots the BODY occupies. A cape,
                // a circlet or a piece of jewellery has no bare-skin equivalent,
                // so there is nothing to swap in -- the claim is withdrawn
                // instead, and the engine builds nothing there.
                SKSE::log::info("[COSTUME]   worn '{}' (slots 0x{:08X}) <- {}",
                    armo->GetName(), covered,
                    fromCostume ? std::format("costume @{}", at)
                                : (want ? std::format("skin @{}", at)
                                        : std::string("claim withdrawn (no skin here)")));
                const auto full = static_cast<std::uint32_t>(armo->GetSlotMask().get());
                if (!want) {
                    // ★★Withdraw the claim, do not hide the node. A culled slot
                    // is still OCCUPIED, and an occupied Hair slot stops the
                    // engine drawing hair at all. Dropping the claim frees it, so
                    // whatever the body grows there comes back.
                    // (full & ~covered keeps slots we never touch, e.g. a shield.)
                    reclaim(armo, full & ~covered);
                    continue;
                }

                // ★★★A carrier must stop claiming slots the costume piece does
                // not use. Swapping the appearance leaves the CLAIM behind: a
                // hood is Hair+Circlet, a circlet-only costume piece needs just
                // Circlet, and the leftover Hair claim kept the player bald --
                // the crown rendered correctly and the hair never came back.
                if (fromCostume) {
                    const std::uint32_t keep = covered & donorMask[at];
                    if (keep && keep != covered) {
                        SKSE::log::info("[COSTUME]     carrier drops slots 0x{:08X} the "
                                        "costume piece does not use", covered & ~keep);
                        reclaim(armo, (full & ~covered) | keep);
                        covered = keep;
                    }
                }

                // ★★★Owning the addon list is not enough -- the ADDON must also
                // claim a slot the carrier claims, because the engine only
                // builds a part where the two masks meet. An Iron Helmet is
                // Hair+Circlet as an ARMO but its ARMA claims Hair alone, so
                // lending it to a Circlet-only crown produced nothing at all:
                // the swap "succeeded", the log said redressed, and the head
                // stayed empty. Widen the addon to the carrier for this call.
                const auto armaMask = static_cast<std::uint32_t>(want->GetSlotMask().get());
                if ((armaMask & covered) == 0) {
                    reclaim(want, armaMask | covered);
                    SKSE::log::info("[COSTUME]     addon slots 0x{:08X} widened to 0x{:08X} "
                                    "to reach the carrier", armaMask, armaMask | covered);
                }
                // ★★TRIED AND REVERTED: widening the CARRIER to the addon's
                // mask when the two overlap without matching. The reasoning was
                // sound -- the engine only builds where the carrier claims, so
                // an addon spanning body+53 lent to a body-only carrier should
                // come out half-built -- and it fired exactly where predicted
                // (`carrier widened by 0x00800000` on the piece in question).
                // The bare chest did not change. So the missing part is not a
                // claim the carrier failed to make, and the cost of keeping it
                // was real: 0x00800154 on another piece meant one carrier
                // claiming five slots, which is five other garments it can take
                // the place of.

                Backup b;
                b.armo = armo;
                b.saved.reserve(armo->armorAddons.size());
                for (std::uint32_t k = 0; k < armo->armorAddons.size(); ++k) {
                    b.saved.push_back(armo->armorAddons[k]);
                    armo->armorAddons[k] = want;
                }
                backups.push_back(std::move(b));
            }
        }

        // ★★★Rebuild the way the ENGINE does when gear changes: flag what needs
        // redoing, THEN ask for the update. Both halves are required.
        //
        // `Update3DModel` was tried alone first, did nothing, and was written off
        // as "the engine ignores our swap" -- so DoReset3D was used instead, and
        // it worked. But Update3DModel had done nothing because NOTHING WAS
        // FLAGGED; it had no opinion about the addon swap at all. DoReset3D is
        // the heavier, different path, and repeating it is what left the player
        // invisible: every measure stayed healthy (nodes, culling, parent, fade,
        // mesh list) and only a real equip change -- which goes through the
        // flagged path -- brought the body back. That was the clue: the engine's
        // own route is complete and ours was not.
        //
        // ★It is also much faster, which is the tell that DoReset3D was doing far
        // more than this feature needs.
        auto* proc = player->GetActorRuntimeData().currentProcess;
        if (proc) {
            proc->Set3DUpdateFlag(static_cast<RE::RESET_3D_FLAGS>(
                static_cast<std::uint32_t>(RE::RESET_3D_FLAGS::kModel) |
                static_cast<std::uint32_t>(RE::RESET_3D_FLAGS::kSkin)));
            proc->Update3DModel(player);
        } else {
            // No process (never seen for the player with 3D loaded) -- the old
            // route rather than no rebuild at all.
            SKSE::log::warn("[COSTUME] no AIProcess -- falling back to DoReset3D");
            player->DoReset3D(false);
        }


        // ★★★NO NODE CULLING HERE. There used to be a `SetAppCulled(true)` pass
        // over the slots with nothing to show, and it was a one-way door: nothing
        // ever cleared it. DoReset3D does not always allocate fresh nodes (the
        // same pointer survived 11s in an earlier measurement), so a cull set on
        // one pass could outlive the reason for it, and switching the costume off
        // never unset it -- toggling repeatedly made the player turn INVISIBLE,
        // and only closing the menu (a rebuild from outside this system) undid it.
        //
        // Withdrawing the CLAIM replaced it. That is better in both directions:
        // the engine builds nothing there, so there is nothing to hide, and the
        // change is restored with every other mask edit by RestoreAll.

        g_lastRebuild = g_frame;
        g_appliedTab = wanted ? g_tab : -1;

        // ★Sample AFTER the rebuild -- what is in the slot now is the thing a
        // later rebuild would replace. Sampling before would record the body we
        // just discarded and the watch would fire on our own work, forever.
        // ★A slot the costume actually DRESSED. An untouched slot holds the
        // same addon before and after, so watching one would never notice
        // anything, and watching an empty one would compare null to null.
        g_watchSlot = -1;
        g_watchAddon = nullptr;
        if (wanted) {
            auto* bp = player->GetActorRuntimeData().biped.get();
            if (bp) {
                for (std::uint32_t i = 0; i < kSlots; ++i) {
                    if (!donor[i] || !bp->objects[i].addon) continue;
                    g_watchSlot = static_cast<int>(i);
                    g_watchAddon = bp->objects[i].addon;
                    break;
                }
            }
        }
        SKSE::log::info("[COSTUME] applied tab {}: {} redressed, {} mask edit(s) "
                        "(watching slot {})",
            g_appliedTab, backups.size(), edits.size(), g_watchSlot);

        // ★★When a costume piece does not appear and every step above still
        // reports success, the question has stopped being "did we ask
        // correctly" and become "what did the answer turn out to be". Compare
        // what was lent against BipedAnim::objects[i].addon and print
        // objects[i].part->GetModel(); a MISMATCH names the addon that won the
        // slot instead, and the mesh path says whose it is. That is what found
        // the body replacer's skin painting over the chest.
    }

    // ★★★ONE PLACE WATCHES, INSTEAD OF EVERY PLACE ANNOUNCING.
    //
    // g_tab IS the costume state, and four separate paths move it: SetTab,
    // OnTabRemoved, LoadRecord (a save that had one) and RevertGame. Putting a
    // broadcast at each of those would be four chances to forget one -- and a
    // listener that missed a single transition is silently wrong from then on,
    // with nothing to tell it so.
    //
    // So the transition is DETECTED here rather than reported from four places.
    // One integer compared per tick costs nothing, and the piece list is only
    // built on the frames where the answer actually changed.
    void AnnounceIfMoved()
    {
        if (g_tab == g_toldApi) return;
        g_toldApi = g_tab;
        if (g_tab < 0) {
            HostApi::BroadcastCostume(-1, nullptr, 0);
            return;
        }
        // ★★ONLY WHAT THE COSTUME ACTUALLY WEARS. A loadout tab holds the whole
        // kit, weapons included -- but a costume is appearance and it leaves
        // weapons, the shield and the quiver alone by design. Sending the raw
        // tab would name pieces that never reach the body, and a listener has
        // no way to tell which of them count. CoversArmor is the same question
        // the dressing code asks, so the answer cannot drift from it.
        //
        // Built here and not held: the ABI lends this list for the length of
        // the call, and HostApi copies it into its own buffer (원칙 2).
        const std::vector<RE::FormID> all = FUI::Loadout::FormsOf(g_tab);
        std::vector<RE::FormID> worn;
        worn.reserve(all.size());
        for (const RE::FormID id : all) {
            auto* armo = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
            if (armo && CoversArmor(armo)) worn.push_back(id);
        }
        HostApi::BroadcastCostume(g_tab, worn.data(),
                                  static_cast<std::uint32_t>(worn.size()));
    }

    void Tick()
    {
        ++g_frame;   // counted every tick, not only on the ones that rebuild
        AnnounceIfMoved();   // ★cheap: one int compare unless the state moved
        // ★1.6.0 migration retry. Armed by SweepRetiredCarrier and cleared the
        // first frame the body can be read -- so a load that arrived ahead of
        // the actor still gets its carrier taken off. One bool test otherwise.
        if (g_sweepCarrier) SweepCarrierNow();
        // ★★★AFTER A LOAD, DRESS MORE THAN ONCE.
        //
        // A load finishes, the costume is applied, the log says so -- and the
        // player is bald. The engine is still working on the actor: it builds
        // the body again from the addon lists, which by then have been put back
        // (they must be -- they live on the shared form). The anchors stay worn,
        // claiming their slots with no model to put there, and that IS the bald
        // head. Nothing here is wrong except the timing.
        //
        // ★There is no signal to watch for. The obvious one -- "did the biped
        // pointer change" -- does not work: the engine reuses it (measured at
        // 11s on the same pointer), so a rebuild is invisible from outside.
        // Since the end of the load sequence cannot be observed, cover it:
        // re-dress on a schedule until the actor has settled. Cheap, because
        // Update3DModel is the engine's own light path, and it stops on its own.
        if (g_reapplyLeft > 0 && g_frame >= g_reapplyAt) {
            --g_reapplyLeft;
            g_reapplyAt = g_frame + kReapplyEvery;
            g_appliedTab = -2;   // "already applied" is exactly the wrong answer
            g_dirty = true;
        }

        // ★The watch. One slot is enough -- a rebuild remakes the whole body,
        // so whichever slot we sampled tells the same story as all of them.
        if (g_watchSlot >= 0 && !g_dirty) {
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* bp = (p && p->Is3DLoaded()) ? p->GetActorRuntimeData().biped.get() : nullptr;
            if (bp && bp->objects[g_watchSlot].addon != g_watchAddon) {
                SKSE::log::info("[COSTUME] slot {} was rebuilt from under us -- redressing",
                    g_watchSlot);
                g_appliedTab = -2;
                g_dirty = true;
            }
        }
        // ★Every tick, not a window around the rebuild. The bug does not appear
        // where the work happens, so watching only there proved nothing.
        if (!g_dirty) return;
        Apply();
    }

    // ---- persistence ------------------------------------------------------

    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kRecordType, kVersion)) return;
        a_intfc->WriteRecordData(static_cast<std::int32_t>(g_tab));
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t)
    {
        std::int32_t tab = -1;
        if (!a_intfc->ReadRecordData(tab)) return;
        // Trusting the file here would point the costume at a tab that may no
        // longer exist; Apply re-checks through CanBeTab anyway, and MarkDirty
        // makes the first tick dress the body.
        g_tab = tab;
        g_appliedTab = -2;
        MarkDirty();
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        // ★Only bookkeeping is cleared here -- unequipping against a dying actor
        // would be reaching into a torn-down world. Nothing is lost: whether the
        // anchor is held or worn is read back out of the inventory, so a save
        // crossed with the costume on is cleaned up by the first Apply.
        g_anchorTries = 0;
        g_anchorGaveUp = false;
        g_lastRebuild = 0;   // a load is not something to space against
        g_tab = -1;
        g_appliedTab = -2;   // unknown: the next Apply must run even if -1
        g_dirty = false;
        // ★The schedule belongs to the save being left. NoteGameLoaded opens a
        // fresh one for the save coming in. The watch goes with it: its slot
        // and addon describe a body that is being torn down.
        g_reapplyLeft = 0;
        g_watchSlot = -1;
        g_watchAddon = nullptr;
    }
}
