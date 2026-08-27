#include "api/HostApi.h"
#include "game/BagFilter.h"
#include "game/Census.h"
#include "game/Costume.h"
#include "game/DeltaWatch.h"
#include "game/Ledger.h"
#include "game/WornLedger.h"
#include "game/DualRing.h"
#include "game/GoldCoins.h"
#include "game/Lotd.h"
#include "ui/Editor.h"
#include "ui/Fallback.h"
#include "ui/Lang.h"
#include "ui/Theme.h"
#include "ui/WinManager.h"
#include "ui/Loadout.h"
#include "ui/Grid.h"
#include "ui/LootBarter.h"
#include "ui/IconCache.h"
#include "ui/Sfx.h"
#include "ui/ItemPreview.h"
#include "ui/UIRoot.h"
#include "ui/Wheeler.h"

#include <cmath>
#include <cstdio>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
//  GridInventory - Mabinogi/Diablo-style tetris inventory (ImGui)
//
//  The UI lives in src/ui/ (GridInventoryMenu + UIRoot + Grid/Equip/Editor;
//  icons are captured off the engine's Inventory3DManager render - see
//  ui/ItemPreview). This file owns the game-side data and wiring: item
//  defs / categories / presets, the InventoryMenu intercept, raw input,
//  and the per-frame update hook.
// ============================================================================

namespace
{
    bool g_planBPendingOpen = false;   // open our menu once InventoryMenu fully closed
    // ⑫ — the vanilla menu we swallowed and still owe a close for; empty =
    // nothing owed. A NAME rather than a flag because three different screens
    // come through the same door (see MenuCloseEchoTick).
    std::string_view g_echoMenu{};
    bool g_pendingPartnerOpen = false; // open our grid once Container/BarterMenu fully closed (loot/barter)
    bool g_movementOff = false;        // we disabled the movement handler (text input)
    bool g_reopenAfterMsg = false;     // we stepped aside for a MessageBox (poison confirm)

    // ★★★HOP TO THE VANILLA INVENTORY AND BACK, WITHOUT LEAVING THE GAME.
    //
    // GridInventory_vanilla.txt already did this, but reaching it meant
    // alt-tabbing out to touch a file. A key makes the comparison immediate:
    // open ours, open the engine's, and the SAME log holds both -- which is
    // the only way to tell "our path is wrong" from "the engine does this
    // too" for a report about one particular item.
    //
    // Written from the input thread, read from the UI thread, hence atomic.
    std::atomic<bool> g_vanillaKey{ false };

    // ★★★THE KEY IS UNASSIGNED UNLESS SOMEONE ASSIGNS IT.
    //
    // Handing the inventory to the engine mid-session is a diagnostic, not a
    // feature: a player who hits it by accident is left wondering why their
    // inventory suddenly looks different. So the scancode lives in
    // GridInventory_ui.ini beside the other test switches --
    //
    //     !vanillakey = 87        (87 = 0x57 = F11)
    //
    // -- and ships as 0, which matches no key. The mechanism stays whole, so
    // it is also something a REPORTER can be handed: "add this line, press
    // F11, tell us whether vanilla does the same thing." That comparison is
    // what pinned the book bug to us rather than to the engine.
    //
    // Read from UIRoot rather than a file: this sits on the raw input path,
    // where a filesystem call would run on every key a player ever presses
    // (원칙 3).

    // raw RefHandle -> reference (ContainerMenu/BarterMenu return a raw handle)
    RE::TESObjectREFR* HandleToRef(RE::RefHandle a_handle)
    {
        if (a_handle == 0) return nullptr;
        // ★the NG line exposes this as a free function; it used to be reached
        // through RE::Offset, which no longer exists there
        RE::NiPointer<RE::TESObjectREFR> ptr;
        RE::LookupReferenceByHandle(a_handle, ptr);
        return ptr.get();
    }

    // Typing into a text field (rename / preset name) must not leak A/D/W/S
    // into the movement handler. Re-asserted per frame from the Update hook:
    // the engine re-enables handlers on its own (equip / player-3D rebuild).
    void SetMoveInput(bool a_enable)
    {
        auto* pc = RE::PlayerControls::GetSingleton();
        if (pc && pc->movementHandler) {
            pc->movementHandler->inputEventHandlingEnabled = a_enable;
        }
    }
    void LockpickReopenTick();      // defined below (lockpick auto-open fallback)
    void MenuCloseEchoTick();        // defined below (⑫ — the close nobody heard)

    // ---- PlayerCharacter::Update vtable hook (index 0xAD) ----
    struct UpdateHook
    {
        static void thunk(RE::PlayerCharacter* a_this, float a_delta)
        {
            func(a_this, a_delta);
            // text input owns the keyboard: block WASD from moving the player
            if (FUI::UIRoot::IsTextInputActive()) {
                SetMoveInput(false);
                g_movementOff = true;
            } else if (g_movementOff) {
                SetMoveInput(true);
                g_movementOff = false;
            }
            // apply capture defs + park the preview model BEFORE this frame
            // renders. While the menu is open (game paused) GridInventoryMenu::
            // AdvanceMovie drives Tick - this path covers unpaused frames.
            FUI::UIRoot::Tick();
            // ★Every tick, open or not: the CLOSE animation has to keep running
            // after the hotkey is already released, and it is what finally takes
            // the overlay menu down.
            FUI::Wheeler::Tick();
            LockpickReopenTick();      // lockpick auto-open fallback
            MenuCloseEchoTick();        // ⑫ — the close, when it is true
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
            // ★★★THE SLOT MOVES IN VR. CommonLibSSE splits it itself --
            // Actor.cpp: RelocateVirtual<&Actor::Update>(0x0AD, 0x0AF, ...) --
            // and this build ships with ENABLE_SKYRIM_VR, so SKSEVR loads us.
            // Writing the SE/AE index there replaced Resurrect (VR 0x0AD),
            // whose signature is nothing like ours, and left the real Update
            // unhooked: no Tick ever ran and the game died the first time
            // anything was resurrected. Silent on SE/AE, fatal on VR.
            func = vtbl.write_vfunc(REL::Module::IsVR() ? 0xAF : 0xAD, thunk);
        }
    };

    // ---- Capacity system (Mabinogi rule): no free cells -> no pickup ----
    // PlayerCharacter::PickUpObject vtable hook (0xCC on SE/AE, 0xCE on VR): the manual
    // world-pickup path. Scripted/quest AddItem is deliberately NOT blocked
    // (bouncing those would break quests); such items overflow into extra
    // grid rows instead.
    struct PickUpHook
    {
        static void thunk(RE::Actor* a_this, RE::TESObjectREFR* a_object, std::int32_t a_count,
                          bool a_arg3, bool a_playSound)
        {
            if (a_this && a_this->IsPlayerRef() && a_object) {
                // G2: a Coin_Sack world ref IS gold — consume it as ledger
                // gold instead of picking up the sack item
                if (FUI::GoldCoins::TryPickUpSack(a_object)) {
                    return;
                }
                if (auto* base = a_object->GetBaseObject();
                    base && !FUI::Grid::CanFitNewItem(base)) {
                    // ⓖ PROBE. A refusal used to leave no trace at all, so a
                    // report of "gold would not pick up" could not be told
                    // apart from "the torch next to it would not". Gold is
                    // supposed to be exempt (MaxAcceptUnits returns early on
                    // IsGold), and this line is what proves whether it was.
                    logger::info("[PICKUP] refused '{}' ({:08X}) type={} "
                                 "gold={} coin={} -- board full (PickUpObject)",
                        base->GetName(), base->GetFormID(),
                        static_cast<int>(base->GetFormType()),
                        base->IsGold(),
                        FUI::GoldCoins::IsCoinForm(base->GetFormID()));
                    FUI::Sfx::FailNote(FUI::Lang::T(FUI::Lang::Str::InventoryFull));
                    return;   // blocked: the reference stays in the world
                }
            }
            func(a_this, a_object, a_count, a_arg3, a_playSound);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
            // ★Same split as UpdateHook above -- CommonLibSSE's Actor.cpp has
            // RelocateVirtual<&Actor::PickUpObject>(0x0CC, 0x0CE, ...). On VR
            // the SE/AE index is OnArmorActorValueChanged.
            func = vtbl.write_vfunc(REL::Module::IsVR() ? 0xCE : 0xCC, thunk);
        }
    };

    void NotifyInventoryFull();   // defined below (throttled toast)

    // G2: Coin_Sack activation intercept at the TESObjectMISC::Activate slot —
    // one level ABOVE PickUpObject, so TrueHUD's Recent Loot (which wraps
    // PickUpObject outside our hook) never sees a "Coinpurse" pickup. The
    // sack converts straight into ledger gold.
    struct SackActivateHook
    {
        static bool thunk(RE::TESObjectMISC* a_this, RE::TESObjectREFR* a_targetRef,
                          RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3,
                          RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            if (a_activatorRef && a_activatorRef->IsPlayerRef()) {
                if (FUI::GoldCoins::TryPickUpSack(a_targetRef)) {
                    return true;   // consumed as gold — no item pickup happens
                }
                // capacity gate for plain MISC items, at the SAME pre-TrueHUD
                // level as CapacityActivateHook (the sack conversion above
                // must run first: gold ignores grid space)
                if (!FUI::Grid::CanFitNewItem(a_this)) {
                    // ⓖ probe: the same question at the MISC door, which is the
                    // one gold actually walks through (Gold001 is a MISC record)
                    logger::info("[PICKUP] refused '{}' ({:08X}) gold={} coin={} "
                                 "-- board full (MISC activate)",
                        a_this->GetName(), a_this->GetFormID(),
                        a_this->IsGold(),
                        FUI::GoldCoins::IsCoinForm(a_this->GetFormID()));
                    NotifyInventoryFull();
                    return false;   // blocked: the reference stays in the world
                }
            }
            return func(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_TESObjectMISC[0] };
            func = vtbl.write_vfunc(0x37, thunk);
        }
    };

    // W2: equipping/unequipping frees/consumes grid cells (worn items leave the
    // board) — favorites/hotkey equips happen with our menu CLOSED, so the
    // rebuild path never sees them; recompute the capacity state instead.
    class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        static EquipSink* GetSingleton()
        {
            static EquipSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            // ★1.4/B0: this sink used to throw its delta away -- it read
            // IsPlayerRef() and nothing else. B0 measured what the event can
            // and cannot say (§8-2): uniqueID is always zero, so it never names
            // the unit -- but `equipped` is exactly the one bit we need.
            FUI::DeltaWatch::OnEquip(a_event);
            if (a_event && a_event->actor && a_event->actor->IsPlayerRef()) {
                // ★1.4/B3: an unequip puts a unit BACK on the board, and that
                // is the only direction without an optimistic path. Includes
                // the engine's own slot-conflict removals, which our equip code
                // never sees. Deferred to the main thread: these arrive on
                // whatever thread the engine was on.
                // ★B3 BODY: the partial update first -- it adds the returning
                // unit through the same walk and tile factory the rebuild
                // uses, scoped to one form, and declines (with a logged
                // reason) whenever it is not certain. Only a decline still
                // costs a full rebuild.
                if (!a_event->equipped) {
                    const RE::FormID fid = a_event->baseObject;
                    SKSE::GetTaskInterface()->AddTask([fid]() {
                        // B4-2 observation: the ledger hears every player
                        // unequip, including the engine's own slot-conflict
                        // removals -- the case our equip code never sees.
                        FUI::WornLedger::OnUnequip(fid);
                        // B4-4: a landed equip record's story ends at the
                        // unequip -- retire it before the partial reconcile
                        // reads the exclusions.
                        FUI::Grid::ReleaseLandedPendingEquip(fid);
                        if (!FUI::Grid::OnFormDelta(fid)) {
                            FUI::Grid::RequestRebuild();
                        }
                    });
                } else {
                    // ★The helmet that never showed (user report): while OUR
                    // menu holds the game paused, the engine's actor update --
                    // the pass that applies a finished equip to the biped 3D --
                    // does not run. ProcessPending forces one refresh two UI
                    // ticks after the request, but that is a GUESS about when
                    // the equip data has settled; when the engine applied
                    // late, the refresh redrew the old biped and the worn
                    // helmet stayed invisible until something else forced one
                    // (cycling loadout presets was the reported healer --
                    // Loadout.cpp forces the same refresh). THIS event is the
                    // engine saying the equip IS applied, the exact moment the
                    // frame count tried to approximate, so the refresh anchors
                    // here. Marshalled: equip events arrive on arbitrary
                    // threads (rule 4), and the menu/3D checks belong on the
                    // main thread anyway. Outside our menu the game is
                    // unpaused and refreshes itself -- skip.
                    const RE::FormID fid = a_event->baseObject;
                    SKSE::GetTaskInterface()->AddTask([fid]() {
                        // B4-2 observation: BEFORE the menu gate below -- the
                        // ledger listens whether our menu is open or not.
                        FUI::WornLedger::OnEquip(fid);
                        // B4-2c: same confirm, delivered to the equip queue --
                        // the worn-clock flips here now, not when our call
                        // returned.
                        FUI::Grid::NoteEquipLanded(fid);
                        // ★Ring session: a same-form SELF-SWAP fires OFF
                        // before ON, and the OFF-side reconcile ran while this
                        // request was still un-landed -- the displaced spare
                        // cancelled against the in-flight record ("nothing
                        // fresh") and stayed hidden until a sweep (user
                        // report: the doffed half appears late). Landed, the
                        // worn unit belongs to skipWorn, so the same one-form
                        // reconcile now sees the spare and draws it. Declines
                        // are IGNORED on this side -- the equip direction
                        // never needed a rebuild (B3, measured), and e.g. a
                        // torch's "still worn" decline must not start one.
                        // ★Still ignored -- but SAID, because it used not to
                        // be. The shared decline line claimed "full rebuild"
                        // for every caller, and this one does not rebuild, so
                        // a stale tile after an equip looked in the log like a
                        // tile a rebuild had already been past. Which of the
                        // two it is decides where to look next.
                        if (!FUI::Grid::OnFormDelta(fid)) {
                            // ★★ASK WHETHER A CLICK ALREADY DID IT.
                            //
                            // The decline used to be dropped outright, on the
                            // reasoning above -- true for an equip started by
                            // a grid click, which takes the tile off the board
                            // at the moment of the click. An equip from the
                            // QUICK WHEEL has no such click, and neither does
                            // a hotkey or a script: nothing removes the tile,
                            // and throwing the decline away left it standing
                            // until some unrelated rebuild wandered past.
                            // Measured -- a wheel equip, no removal, decline
                            // swallowed, and only luck cleaning up after.
                            if (FUI::Grid::ClaimOptimisticRemove(fid)) {
                                SKSE::log::info("[B3] equip-side decline ignored "
                                    "({:08X}) -- the click already took the tile", fid);
                            } else {
                                SKSE::log::info("[B3] equip-side decline escalated "
                                    "({:08X}) -- nothing removed the tile", fid);
                                FUI::Grid::RequestRebuild();
                            }
                        }
                        auto* ui = RE::UI::GetSingleton();
                        if (!ui || !ui->IsMenuOpen("GridInventoryMenu"sv)) return;
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (!player || !player->Is3DLoaded()) return;
                        if (auto* proc =
                                player->GetActorRuntimeData().currentProcess) {
                            proc->Update3DModel(player);
                        }
                    });
                }
                FUI::Grid::MarkCapacityDirty();
                // The active preset IS what the player is wearing, so anything
                // that changes the worn gear changes that tab.
                FUI::Loadout::MarkActiveStale();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void NotifyInventoryFull()
    {
        // take-all spams one container event per stack — throttle the toast
        static std::uint32_t s_last = 0;
        const std::uint32_t now = RE::GetDurationOfApplicationRunTime();
        if (now - s_last > 700) {
            s_last = now;
            FUI::Sfx::FailNote(FUI::Lang::T(FUI::Lang::Str::InventoryFull));
        }
    }

    // Capacity: harvesting (flora / food-bearing trees) — TESBoundObject::
    // Activate override slot 0x37; the produce item is checked BEFORE the
    // engine harvests, so nothing is consumed on a blocked attempt.
    template <class T>
    struct HarvestHook
    {
        static bool thunk(T* a_this, RE::TESObjectREFR* a_targetRef,
                          RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3,
                          RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            if (a_activatorRef && a_activatorRef->IsPlayerRef() &&
                a_this->produceItem && !FUI::Grid::CanFitNewItem(a_this->produceItem)) {
                // ⓖ probe: WHAT the plant would have produced. This gate is
                // where the vanilla coin purse was dying and it said nothing at
                // all -- the refusal reached the player as a toast and the log
                // as silence.
                logger::info("[PICKUP] refused harvest '{}' -> produce '{}' "
                             "({:08X}) type={} -- board full",
                    a_this->GetName(),
                    a_this->produceItem->GetName(),
                    a_this->produceItem->GetFormID(),
                    static_cast<int>(a_this->produceItem->GetFormType()));
                NotifyInventoryFull();
                return false;   // blocked: the plant stays harvestable
            }
            return func(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install(const REL::VariantID& a_vtbl0)
        {
            REL::Relocation<std::uintptr_t> vtbl{ a_vtbl0 };
            func = vtbl.write_vfunc(0x37, thunk);
        }
    };

    // Capacity gate at the ACTIVATE slot (0x37) — one level ABOVE
    // PickUpObject. TrueHUD's Recent Loot wraps PickUpObject OUTSIDE our
    // PickUpHook, so a pickup blocked down there still logged a phantom
    // "received" entry (user-reported: full inventory + E on a world item
    // spammed RECEIVED while the item stayed on the ground). Blocking at
    // this level, the pickup call never happens and TrueHUD stays silent —
    // same reasoning as SackActivateHook below. PickUpHook remains as the
    // backstop for paths that skip Activate (book-menu Take etc.).
    template <class T>
    struct CapacityActivateHook
    {
        static bool thunk(T* a_this, RE::TESObjectREFR* a_targetRef,
                          RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3,
                          RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            if (a_activatorRef && a_activatorRef->IsPlayerRef() &&
                !FUI::Grid::CanFitNewItem(a_this)) {
                // ⓖ probe: the last of the four gates to get a voice
                logger::info("[PICKUP] refused '{}' ({:08X}) type={} "
                             "-- board full (activate)",
                    a_this->GetName(), a_this->GetFormID(),
                    static_cast<int>(a_this->GetFormType()));
                NotifyInventoryFull();
                return false;   // blocked: the reference stays in the world
            }
            return func(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install(const REL::VariantID& a_vtbl0)
        {
            REL::Relocation<std::uintptr_t> vtbl{ a_vtbl0 };
            func = vtbl.write_vfunc(0x37, thunk);
        }
    };

    // Capacity: container take (loot / chests / pickpocket). The move has
    // already happened when the event fires, so this is a BOUNCE: put the
    // item back into the source. Scoped to an open ContainerMenu — scripted
    // quest handovers (no menu) must never bounce.
    // ★★★A RESPAWNED CONTAINER OWES NOBODY ANYTHING.
    //
    // The deposit ledger (LootBarter, ContLayout::deposits) remembers how many
    // of each form the player stored in a given container, so taking them back
    // is not theft. A cell reset empties that container and refills it from the
    // record -- the player's deposit is gone, but a ledger that outlived it
    // would hand those replacements over as "yours", which is a free steal on
    // every respawn.
    //
    // TESResetEvent is the engine saying exactly that happened, and it names
    // the ref. Without it the only alternative was to expire ledgers on a
    // guessed timer, and a guess is wrong in both directions.
    class ResetSink : public RE::BSTEventSink<RE::TESResetEvent>
    {
    public:
        static ResetSink* GetSingleton()
        {
            static ResetSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESResetEvent* a_event,
            RE::BSTEventSource<RE::TESResetEvent>*) override
        {
            if (a_event && a_event->object) {
                // ★Same reasoning as the pouch handlers in ContainerSink: this
                // erases from g_contLayouts, which LootBarter reads and writes
                // from the main thread with no lock. Read the id here (the
                // event is only valid for this call) and do the erase there.
                const RE::FormID id = a_event->object->GetFormID();
                SKSE::GetTaskInterface()->AddTask([id]() {
                    FUI::LootBarter::ForgetDeposits(id);
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class ContainerSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static ContainerSink* GetSingleton()
        {
            static ContainerSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // ★1.4/B2+B3: the LEDGER consumes the event first -- one request,
            // one event (rule 1). This lives in the SINK, not in DeltaWatch:
            // the ledger is wiring and the watch is observation, and when
            // Confirm sat behind "!delta" the default configuration starved
            // the ledger of every confirmation -- 100% of requests expired.
            // Thread-safe (the ledger locks); no-op unless "!ledger = 1".
            const char* req = nullptr;
            if (a_event->newContainer == 0x14 || a_event->oldContainer == 0x14) {
                const std::int32_t signedCount = a_event->newContainer == 0x14
                                                     ? a_event->itemCount
                                                     : -a_event->itemCount;
                req = FUI::Ledger::Confirm(a_event->baseObj, signedCount);
            }
            // ★1.4/B0 next, before any consumer below reacts -- the whole point
            // is to see the delta as it ARRIVES, in the order it arrived, and
            // to know where the existing consumers sit relative to it. A sink
            // of our own would be delivered in an order we do not control.
            // Observation only; returns immediately unless "!delta = 1".
            FUI::DeltaWatch::OnContainer(a_event, req);
            // W2: any change touching the player's inventory can flip the
            // capacity state (shop buys, scripted AddItem, drops, sells)
            if (a_event->newContainer == 0x14 || a_event->oldContainer == 0x14) {
                FUI::Grid::MarkCapacityDirty();
                // ★★1.2.1: AND THE TILES, not only the numbers. Capacity and
                // gold were re-derived here while the board itself was left
                // alone, so an item arriving from OUTSIDE the UI (console
                // AddItem, Modex, a script, a quest reward) moved the SPACE
                // figure and showed no tile -- the player had to close and
                // reopen. Our own take/sell/drop paths already ask for this,
                // and the flag coalesces per frame, so the extra request is
                // free.
                // ★1.4: the delta applier, at last. Every player-side container
                // delta first tries the per-form partial (the same walk and
                // tile factory the rebuild uses, scoped to one form); anything
                // unproven declines into the full rebuild, and a pending flag
                // short-circuits the partial -- a take-all burst coalesces to
                // one rebuild exactly as before. Menu closed, the partial
                // declines quietly and the flag is consumed by the capacity
                // gates (H1) or the next open, unchanged.
                {
                    const RE::FormID deltaForm = a_event->baseObj;
                    SKSE::GetTaskInterface()->AddTask([deltaForm]() {
                        if (!FUI::Grid::OnFormDelta(deltaForm)) {
                            FUI::Grid::RequestRebuild();
                        }
                    });
                }
                // G1: ledger (Gold001) or coin-form movement -> re-mirror.
                // The reconciler's own edits re-mark dirty and settle at a
                // zero diff next tick (also renormalises console-given coins).
                if (a_event->baseObj == 0x0000000F ||
                    FUI::GoldCoins::IsCoinForm(a_event->baseObj)) {
                    FUI::GoldCoins::MarkDirty();
                }
                // pouch leaving/returning: stored gold travels with it
                // (sale releases it instead — see OnPouchLeftPlayer)
                if (FUI::GoldCoins::IsPouch(a_event->baseObj)) {
                    // ★★★ONTO THE MAIN THREAD, LIKE EVERYTHING ELSE IN HERE.
                    //
                    // A container event arrives on whatever thread moved the
                    // item -- a Papyrus VM worker for a scripted AddItem, and
                    // this file already knows it (the Grid delta twenty lines
                    // up goes through AddTask for exactly that reason; Ledger
                    // takes a mutex; WornLedger marshals). These two were the
                    // only consumers left calling straight through, and they
                    // are not read-only: OnPouchLeftPlayer push_backs into
                    // g_pending and erases from g_pouchStored while Tick() is
                    // iterating that same vector on the main thread. GoldCoins
                    // has no lock of its own -- so give it the thread instead.
                    const bool left = a_event->oldContainer == 0x14;
                    const bool back = a_event->newContainer == 0x14;
                    if (left || back) {
                        const RE::FormID pf = a_event->baseObj;   // ★which pouch
                        SKSE::GetTaskInterface()->AddTask([left, pf]() {
                            if (left) FUI::GoldCoins::OnPouchLeftPlayer(pf);
                            else      FUI::GoldCoins::OnPouchReturned(pf);
                        });
                    }
                }
            }
            if (a_event->newContainer != 0x14 || a_event->oldContainer == 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (auto* ui = RE::UI::GetSingleton();
                !ui || !ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const RE::FormID     base = a_event->baseObj;
            const RE::FormID     src = a_event->oldContainer;
            const std::int32_t   count = a_event->itemCount;
            SKSE::GetTaskInterface()->AddTask([base, src, count]() {
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(base);
                auto* srcRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(src);
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!obj || !srcRef || !player || obj->IsGold()) return;
                if (!FUI::Grid::WouldOverflow(obj)) return;
                // GI36: deliberately NOT ResolveExitUnit. This item never became
                // a tile and we never gave it a star -- any hotkey on it belongs
                // to the container's own copy. Rule 58 is about stars WE own.
                player->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                    nullptr, srcRef);
                NotifyInventoryFull();
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void InitializeLog()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= "GridInventory.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S] [%l] %v");
    }

    // ---- Input sink ----
    class InputSink : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputSink* GetSingleton()
        {
            static InputSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
            RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* e = *a_event; e; e = e->next) {
                // ★The wheeler gets first refusal, on every device. It is a
                // gameplay overlay, so it has to see the hotkey before the
                // "only while our inventory is open" filters below throw the
                // event away — and it must see key-UP too, which the keyboard
                // branch further down drops.
                if (auto* ts = e->AsThumbstickEvent()) {
                    if (FUI::Wheeler::OnThumbstick(ts)) continue;
                } else if (auto* mm = e->AsMouseMoveEvent()) {
                    if (FUI::Wheeler::OnMouseMove(mm)) continue;
                } else if (auto* wb = e->AsButtonEvent()) {
                    if (FUI::Wheeler::OnButton(wb)) continue;
                }
                // ★★NOT muted here. Silencing the game's own input is done at
                // PlayerControls' and MenuControls' entry points instead (see
                // InputLock / MenuLock in Wheeler.cpp), because an event is one
                // object shared by the whole sink chain: blanking it in OUR
                // sink also blanks it for every listener that runs after us,
                // and which side of us the game sits on is not ours to decide.
                // Those hooks blank, call the original, and put the value back.
                if (FUI::Wheeler::IsOpen()) continue;

                // ★(1.5.x) a shelf-read book page is up: the player's ACTIVATE
                // key takes the book home, the world page's own grammar. Only
                // the atomic flag is written here (input thread); the take
                // itself runs on the render thread at the page-close edge.
                if (FUI::LootBarter::ShelfBookTakeArmed()) {
                    if (auto* bt = e->AsButtonEvent(); bt && bt->IsDown()) {
                        std::uint32_t want = 0;
                        if (auto* cm = RE::ControlMap::GetSingleton()) {
                            if (auto* ue = RE::UserEvents::GetSingleton()) {
                                want = cm->GetMappedKey(ue->activate,
                                                        e->GetDevice());
                            }
                        }
                        // fallback: the keyboard default (E = 18)
                        const bool hit =
                            want != 0xFF && want != 0
                                ? bt->GetIDCode() == want
                                : (e->GetDevice() ==
                                       RE::INPUT_DEVICE::kKeyboard &&
                                   bt->GetIDCode() == 18);
                        if (hit) {
                            FUI::LootBarter::FlagShelfBookTake();
                            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                                mq->AddMessage(RE::BookMenu::MENU_NAME,
                                               RE::UI_MESSAGE_TYPE::kHide,
                                               nullptr);
                            }
                            continue;
                        }
                    }
                }

                // A real mouse event hands the pointer back from the pad.
                // This is the ONLY reliable signal — see UIRoot::NoteMouseInput.
                if (e->GetDevice() == RE::INPUT_DEVICE::kMouse) {
                    if (auto* ui = RE::UI::GetSingleton();
                        ui && ui->IsMenuOpen("GridInventoryMenu"sv)) {
                        FUI::UIRoot::NoteMouseInput();
                    }
                    continue;
                }

                // ---- gamepad: drive the grid's own pointer -----------------
                // Only while our menu owns the screen, so nothing here can
                // touch normal gameplay input.
                if (e->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
                    auto* ui = RE::UI::GetSingleton();
                    if (!ui || !ui->IsMenuOpen("GridInventoryMenu"sv)) continue;
                    if (FUI::UIRoot::IsBookOpen()) continue;   // the book has input
                    if (auto* ts = e->AsThumbstickEvent()) {
                        FUI::UIRoot::NotePadStick(ts->IsRight(), ts->xValue, ts->yValue);
                        // let the engine's cursor move itself (see the header)
                        // ★LEFT stick only: the right stick is the SCROLL
                        // wheel, and feeding it here had the engine walking
                        // the pointer with it -- scroll and cursor moving on
                        // one stick (user report).
                        if (!ts->IsRight()) FUI::UIRoot::FeedEngineCursor(ts);
                    } else if (auto* gb = e->AsButtonEvent()) {
                        // held state, not the down EDGE: the UI needs press and
                        // release both (click-drag, the shift modifier)
                        FUI::UIRoot::NotePadButton(gb->GetIDCode(), gb->IsPressed());
                    }
                    continue;
                }

                auto* btn = e->AsButtonEvent();
                if (!btn || !btn->IsDown()) {
                    continue;
                }
                if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }
                // A book opened from our grid owns the keyboard until it is
                // dismissed — the Inventory key must not close us underneath it.
                // ★The console is the same story from the other direction: it
                // sits on top taking keystrokes, and an 'i' typed into a
                // command would otherwise close the menu behind it.
                if (FUI::UIRoot::IsBookOpen() || FUI::UIRoot::IsConsoleOpen()) {
                    continue;
                }
                // ★★F11 hands the next inventory to the engine, and F11 again
                // takes it back. Deliberately NOT gated on our menu being open:
                // the point is to arm the swap while nothing is open, then press
                // the inventory key and watch the OTHER screen do the same thing.
                // The state only reads at open time, so a mid-session press
                // never disturbs a screen already on show.
                if (const int vk = FUI::UIRoot::VanillaKey();
                    vk != 0 && btn->GetIDCode() == static_cast<std::uint32_t>(vk)) {
                    const bool on = !g_vanillaKey.load();
                    g_vanillaKey.store(on);
                    logger::warn("[INV] vanilla passthrough {} (F11)",
                                 on ? "ON -- the engine gets the next inventory"
                                    : "OFF -- the grid is back");
                    SKSE::GetTaskInterface()->AddUITask([on]() {
                        FUI::Sfx::Notify(on ? "Vanilla inventory (F11)"
                                            : "Grid inventory (F11)");
                    });
                    continue;
                }
                // The game's Inventory key closes our menu. This sink sits
                // UPSTREAM of input-context filtering, so it still sees the
                // raw key while kMenuMode swallows the user event.
                if (auto* ui = RE::UI::GetSingleton();
                    ui && ui->IsMenuOpen("GridInventoryMenu"sv)) {
                    auto* cm = RE::ControlMap::GetSingleton();
                    // NOTE: GetMappedKey returns 0xFF here (confirmed in
                    // the log) - fall back to the default I scancode.
                    auto scan = cm ? cm->GetMappedKey(
                        RE::UserEvents::GetSingleton()->inventory,
                        RE::INPUT_DEVICE::kKeyboard) : 0xFF;
                    if (scan == 0xFF || scan == 0xFFFFFFFF) scan = 0x17;   // default I
                    if (btn->GetIDCode() == scan) {
                        // input thread: defer state changes to the UI task
                        SKSE::GetTaskInterface()->AddUITask([]() {
                            if (FUI::UIRoot::IsTextInputActive()) {
                                return;   // typing 'i' into a text field
                            }
                            if (FUI::Grid::IsHolding()) {
                                FUI::Grid::CancelHold();
                            } else if (!FUI::UIRoot::CloseTopWindow()) {
                                // settings/EDIT close first; then the inventory
                                FUI::UIRoot::Close();
                            }
                        });
                    }
                    // ★(1.3.1) the game's MAGIC key hops out: close the grid
                    // and raise the vanilla MagicMenu, journal-style. The
                    // kItemMenu context never translates this key into a user
                    // event, so it is read raw here exactly like the
                    // Inventory key above (same 0xFF fallback story).
                    static const RE::BSFixedString s_magicEvent("Magic");
                    auto mscan = cm ? cm->GetMappedKey(s_magicEvent,
                        RE::INPUT_DEVICE::kKeyboard) : 0xFF;
                    if (mscan == 0xFF || mscan == 0xFFFFFFFF) mscan = 0x19;   // default P
                    if (btn->GetIDCode() == mscan) {
                        SKSE::GetTaskInterface()->AddUITask([]() {
                            if (FUI::UIRoot::IsTextInputActive()) {
                                return;   // typing 'p' into a text field
                            }
                            // plain inventory only: a loot/barter session has
                            // a partner to tear down, and vanilla refuses menu
                            // hopping out of those screens too
                            if (FUI::LootBarter::CurrentMode() !=
                                FUI::LootBarter::Mode::kNormal) {
                                return;
                            }
                            FUI::UIRoot::Close();
                            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                                q->AddMessage(RE::MagicMenu::MENU_NAME,
                                              RE::UI_MESSAGE_TYPE::kShow, nullptr);
                            }
                        });
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- Tetris footprint / orientation definitions ----
    // Logic layer works purely in GRID CELLS (design-independent): an item
    // occupies w x h cells; pixel size, gaps and art are the UI layer's business.
    // Phase 2 D2: the def struct + its ini serialization live in ui/ItemDef.h
    // (ONE struct shared by every module; field metatable drives parse/format)
    using ItemDef = FUI::ItemDef;
    std::unordered_map<std::string, ItemDef> g_itemDefs;   // user overrides

    constexpr const char* kDefsPath = "Data/SKSE/Plugins/GridInventory_items.ini";
    constexpr const char* kUniquePath = "Data/SKSE/Plugins/GridInventory_unique.ini";

    // Stable item key across load orders: "Plugin.esp|0xLocalID"
    //
    // A runtime-created form (player-brewed potion, player-enchanted weapon)
    // has NO source file, and GetLocalFormID() dereferences that file without
    // checking it -- so it must never be called for one. Such a form has no
    // local id anyway; its whole FormID is the identity.
    std::string FormKey(RE::TESForm* a_form)
    {
        auto* file = a_form->GetFile(0);
        std::string key = file ? std::string(file->GetFilename()) : "Dynamic";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "|0x%06X",
            file ? a_form->GetLocalFormID() : a_form->GetFormID());
        return key + buf;
    }

    RE::TESBoundObject* FormFromKey(const std::string& a_key)
    {
        const auto bar = a_key.find('|');
        if (bar == std::string::npos) return nullptr;
        std::uint32_t local = 0;
        try {
            local = static_cast<std::uint32_t>(
                std::stoul(a_key.substr(bar + 1), nullptr, 16));
        } catch (...) {
            return nullptr;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return nullptr;
        auto* form = dh->LookupForm(local, a_key.substr(0, bar));
        return form ? form->As<RE::TESBoundObject>() : nullptr;
    }

    // ---- model-level def sharing ----
    // Enchant variants / keys / notes / potions are hundreds of records over
    // ONE nif (measured: 10,711 records -> 2,143 unique models). Tuning the
    // base item should tune every sibling: items without their own override
    // fall back to the def of ANY overridden item sharing their model path.
    std::string ModelPathOf(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return {};
        const char* p = nullptr;
        // ★★THIS CAST TOOK THE GAME DOWN, and the shape of the failure says
        // what kind of pointer reached it: As<> survived (it reads the form
        // TYPE, a plain field) and skyrim_cast threw __non_rtti_object (it
        // reads the VTABLE). So the memory was readable and the vtable was not
        // a game vtable -- a pointer into something that is not a form.
        //
        // Where it comes from is still open: the icon path is fed a tile's
        // object, and a view holds INDICES into g_items rather than copies, so
        // a stale index is the obvious candidate (guarded separately in
        // Grid.cpp). Either way a bad icon is not worth a CTD, and the log line
        // below is what will identify the source next time instead of another
        // round of guessing.
        // ★The pointer is printed, not the name: asking a suspect object for
        // its name is the very dereference that just failed.
        try {
            if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
                // armor is NOT a TESModel — its GND model lives on the biped form
                p = armo->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
            } else if (const auto* mdl = skyrim_cast<RE::TESModel*>(a_obj)) {
                p = mdl->GetModel();
            }
        } catch (...) {
            static std::set<const void*> s_said;
            if (s_said.size() < 8 && s_said.insert(a_obj).second) {
                SKSE::log::error("[DEFS] model lookup refused a non-form pointer "
                                 "({}); icon skipped", static_cast<const void*>(a_obj));
            }
            return {};
        }
        if (!p || !*p) return {};
        std::string s(p);
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (c == '/') c = '\\';
        }
        if (s.rfind("meshes\\", 0) == 0) s.erase(0, 7);
        return s;
    }

    // ★★Is this BOOK record a NOTE (a loose page / letter) rather than a book?
    //
    // The record cannot answer. OBJ_BOOK carries a Type with a kNoteScroll
    // value, and it is 0 on every book in the load order — 1,135 of 1,135,
    // matching an earlier count of 1,134 of 1,134 on a different load order.
    // Skyrim never fills the field. (The library could not read it anyway:
    // kNoteScroll is -1 while the member is a uint8_t, so 0xFF != -1 and the
    // comparison is always false.) The mesh is the only signal left.
    //
    // ★Matched on the FILE NAME's start, not anywhere in the path. A bare
    // find("note") also hits 'notebook', 'denote', and any folder someone
    // named "notes" — a short substring against a whole path is how the armour
    // classifier once mis-sorted silently. Vanilla notes are
    // 'clutter\books\note0N.nif', so the file simply begins with it.
    // The nif's file name alone (no directories) — every mesh-based rule below
    // works on this, never on the whole path, for the reason in the note above.
    [[nodiscard]] std::string ModelFileOf(RE::TESBoundObject* a_obj)
    {
        std::string mp = ModelPathOf(a_obj);   // lowercased, backslashes
        const auto slash = mp.rfind('\\');
        return slash == std::string::npos ? mp : mp.substr(slash + 1);
    }

    [[nodiscard]] bool IsNoteMesh(RE::TESBoundObject* a_obj)
    {
        return ModelFileOf(a_obj).starts_with("note");
    }

    // ★★An INGOT and an ORE carry the exact same keyword.
    //
    // Vanilla ships one VendorItemOreIngot for both — measured across two load
    // orders (60 and 90 records), no second keyword separates them anywhere.
    // So the mesh again. 'ingot' is long enough to match anywhere in the file
    // name safely, which is what picks up 'madnessingot01' and 'dummyingot'
    // alongside the plain 'ingotEbony' family.
    //
    // ★Only the ingot side gets a rule. The ore side would need find("ore"),
    // and a three-letter needle hides inside score/store/forest — the same trap
    // that IsNoteMesh() sidesteps. Everything that is not an ingot simply stays
    // misc_ore, exactly as before.
    [[nodiscard]] bool IsIngotMesh(RE::TESBoundObject* a_obj)
    {
        return ModelFileOf(a_obj).find("ingot") != std::string::npos;
    }

    // ★A DRINK and a plate of food also share one keyword (VendorItemFood), so
    // this is best effort: a drink whose mesh is not named after any of these
    // simply stays "food", which is where it sat before this category existed.
    //
    // ★No "ale" and no "rum" — three-letter needles hide inside whale and
    // drumstick. Every word here is >=4 chars and was checked against the 242
    // distinct food meshes of the test load orders for false hits.
    [[nodiscard]] bool IsDrinkMesh(RE::TESBoundObject* a_obj)
    {
        static constexpr std::string_view kWords[] = {
            "wine", "mead", "flagon", "tankard", "liquor", "brandy", "grog",
            "goblet", "bottle", "drink", "milkjug", "waterskin", "skooma",
            "jagga", "matze", "shein", "sujamma",
        };
        const std::string file = ModelFileOf(a_obj);
        if (file.empty()) return false;
        for (const auto w : kWords) {
            if (file.find(w) != std::string::npos) return true;
        }
        return false;
    }

    std::unordered_map<std::string, ItemDef> g_modelDefs;   // derived, lazy
    bool g_modelDefsDirty = true;

    void RebuildModelDefs()
    {
        g_modelDefsDirty = false;
        g_modelDefs.clear();
        // deterministic first-wins: iterate overrides in sorted key order
        std::vector<const std::string*> keys;
        keys.reserve(g_itemDefs.size());
        for (const auto& kv : g_itemDefs) keys.push_back(&kv.first);
        std::sort(keys.begin(), keys.end(),
            [](const std::string* a, const std::string* b) { return *a < *b; });
        for (const auto* k : keys) {
            auto* obj = FormFromKey(*k);
            if (!obj) continue;
            auto mp = ModelPathOf(obj);
            if (!mp.empty()) g_modelDefs.emplace(std::move(mp), g_itemDefs[*k]);
        }
        logger::info("[DEFS] model-level defs: {}", g_modelDefs.size());
    }

    // ---- category presets (H7: data-driven so preset files can carry them) ----
    std::unordered_map<std::string, ItemDef> g_catDefs;

    void InitCategoryDefs()   // factory values (the user-tuned in-game presets)
    {
        g_catDefs.clear();
        auto put = [&](const char* k, int w, int h, float rx, float ry, float rz, float sc) {
            ItemDef d;
            d.w = w; d.h = h; d.rx = rx; d.ry = ry; d.rz = rz; d.scale = sc;
            g_catDefs[k] = d;
        };
        // 세분화 분류 (Docs/스카이림_아이템_분류_및_상징_아이템.md): each new
        // key seeds from its legacy parent's factory values so resolved defs
        // (and pak capture keys) stay identical until the user tunes them.
        put("weap_dagger",     1, 2, -90, 0, 0, 1.0f);
        put("weap_sword",      1, 3, -90, 0, 0, 1.0f);     // <- weap_1h
        put("weap_waraxe",     1, 3, -90, 0, 0, 1.0f);     // <- weap_1h
        put("weap_mace",       1, 3, -90, 0, 0, 1.0f);     // <- weap_1h
        put("weap_greatsword", 1, 4, -90, 0, 0, 1.0f);     // <- weap_2h
        put("weap_staff",      1, 4, -90, 0, 0, 1.0f);     // <- weap_2h
        put("weap_battleaxe",  2, 4, -90, 0, 15, 1.0f);
        put("weap_warhammer",  2, 4, -90, 0, 15, 1.0f);    // <- weap_battleaxe
        put("weap_bow",        2, 4, -90, 0, 0, 1.0f);
        put("weap_crossbow",   2, 3, -90, 0, 0, 1.0f);
        put("weap_other",      1, 1, -90, 0, 0, 1.0f);
        put("ammo_arrow",      2, 2, -90, 0, -135, 1.2f);  // <- ammo
        put("ammo_bolt",       2, 2, -90, 0, -135, 1.2f);  // <- ammo
        put("armor_ring",      1, 1, -90, 0, 0, 1.0f);
        put("armor_amulet",    1, 1, -90, 0, 0, 1.0f);
        put("armor_circlet",   2, 2, -90, 0, 0, 1.0f);     // <- armor_head
        put("armor_body_heavy", 2, 3, -90, 0, 180, 0.8f);  // <- armor_body
        put("armor_body_light", 2, 3, -90, 0, 180, 0.8f);  // <- armor_body
        put("armor_cloth",     2, 3, -90, 0, 180, 0.8f);   // <- armor_body
        put("armor_shield",    2, 3, -90, 180, 0, 0.9f);
        put("armor_hands",     2, 2, -90, 0, 180, 1.0f);
        put("armor_gloves",    2, 2, -90, 0, 180, 1.0f);   // <- armor_hands
        put("armor_feet",      2, 2, 0, 0, 165, 1.0f);
        put("armor_shoes",     2, 2, 0, 0, 165, 1.0f);     // <- armor_feet
        put("armor_head",      2, 2, -90, 0, 0, 1.0f);
        put("armor_hood",      2, 2, -90, 0, 0, 1.0f);     // <- armor_head
        put("armor_accessory", 2, 3, -90, 0, 180, 0.8f);   // <- armor_cloth
        put("book",            1, 2, -90, 0, 0, 1.0f);
        put("book_skill",      1, 2, -90, 0, 0, 1.0f);     // <- book
        put("book_spell",      1, 2, -90, 0, 0, 1.0f);     // <- book
        put("book_note",       1, 2, -90, 0, 0, 1.0f);     // <- book
        put("scroll",          2, 1, -90, 0, 0, 1.0f);
        put("potion",          1, 1, 0, 0, -90, 1.0f);
        put("poison",          1, 1, 0, 0, -90, 1.0f);     // <- potion
        put("food",            1, 1, 0, 0, -90, 1.0f);     // <- potion
        put("food_raw",        1, 1, 0, 0, -90, 1.0f);     // <- food
        put("food_drink",      1, 1, 0, 0, -90, 1.0f);     // <- food
        put("ingredient",      1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("soulgem",         1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("key",             1, 1, 0, 0, 90, 9.5f);
        put("misc_ore",        1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_ingot",      1, 1, -90, 0, 0, 1.0f);     // <- misc_ore
        put("misc_gem",        1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_hide",       1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_animalpart", 1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_tool",       1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_clutter",    1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc",            1, 1, -90, 0, 0, 1.0f);
    }

    const char* CategoryOf(RE::TESBoundObject* a_obj)
    {
        if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) {
            switch (weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandDagger: return "weap_dagger";
            case RE::WEAPON_TYPE::kOneHandSword:  return "weap_sword";
            case RE::WEAPON_TYPE::kOneHandAxe:    return "weap_waraxe";
            case RE::WEAPON_TYPE::kOneHandMace:   return "weap_mace";
            case RE::WEAPON_TYPE::kTwoHandSword:  return "weap_greatsword";
            case RE::WEAPON_TYPE::kStaff:         return "weap_staff";
            case RE::WEAPON_TYPE::kTwoHandAxe:
                // the engine folds warhammers into TwoHandAxe; the keyword
                // is the canonical discriminator (same rule in the tool)
                return weap->HasKeywordString("WeapTypeWarhammer")
                           ? "weap_warhammer" : "weap_battleaxe";
            case RE::WEAPON_TYPE::kBow:           return "weap_bow";
            case RE::WEAPON_TYPE::kCrossbow:      return "weap_crossbow";
            default:                              return "weap_other";
            }
        }
        if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            using S = RE::BGSBipedObjectForm::BipedObjectSlot;
            if (armo->HasPartOf(S::kAmulet))  return "armor_amulet";
            if (FUI::Grid::IsRing(armo))      return "armor_ring";
            // circlet = slot 42 WITHOUT a head/hair slot (helmets add slot 42
            // to their mask just to hide circlets — those stay armor_head)
            if (armo->HasPartOf(S::kCirclet) && !armo->HasPartOf(S::kHead) &&
                !armo->HasPartOf(S::kHair)) {
                return "armor_circlet";
            }
            if (armo->HasPartOf(S::kBody)) {
                switch (armo->GetArmorType()) {
                case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor:
                    return "armor_body_heavy";
                case RE::BGSBipedObjectForm::ArmorType::kClothing:
                    return "armor_cloth";
                default:
                    return "armor_body_light";
                }
            }
            if (armo->HasPartOf(S::kShield)) return "armor_shield";
            // clothing-type limb/head gear is a glove, a shoe, a hood — soft
            // things that hang differently from the plate they share a slot
            // with. Same discriminator the body already splits on.
            const bool cloth =
                armo->GetArmorType() == RE::BGSBipedObjectForm::ArmorType::kClothing;
            if (armo->HasPartOf(S::kHands)) return cloth ? "armor_gloves" : "armor_hands";
            if (armo->HasPartOf(S::kFeet))  return cloth ? "armor_shoes"  : "armor_feet";
            if (armo->HasPartOf(S::kHead) || armo->HasPartOf(S::kHair) ||
                armo->HasPartOf(S::kCirclet)) {
                return cloth ? "armor_hood" : "armor_head";
            }
            // B10: custom biped slots (capes 46, backpacks, accessories...)
            // used to be swallowed by the armor_head fallback (2x2 helmet
            // defaults), then by armor_cloth — but a cape is not a robe, and
            // this is 908 records on a heavy load order, so it owns a category.
            return "armor_accessory";
        }
        if (auto* book = a_obj->As<RE::TESObjectBOOK>()) {
            if (book->TeachesSpell()) return "book_spell";
            if (book->TeachesSkill()) return "book_skill";
            if (IsNoteMesh(a_obj))    return "book_note";
            return "book";
        }
        if (a_obj->Is(RE::FormType::Scroll)) return "scroll";
        if (auto* ammo = a_obj->As<RE::TESAmmo>()) {
            // ★NOT TESAmmo::IsBolt() -- it reads a member whose offset moves
            // between SE and AE, so on AE every bolt came back an arrow. The one
            // correct answer lives in Fallback (see Fallback::IsBoltAmmo).
            return FUI::Fallback::IsBoltAmmo(ammo) ? "ammo_bolt" : "ammo_arrow";
        }
        if (auto* alch = a_obj->As<RE::AlchemyItem>()) {
            if (alch->IsPoison()) return "poison";
            if (alch->IsFood()) {
                // ★mesh before keyword here, the opposite of everywhere else:
                // a category only decides a default SHAPE, and the bottle is
                // what makes a drink different. Vanilla milk carries
                // VendorItemFoodRaw yet comes in a jug — it wants the bottle.
                if (IsDrinkMesh(a_obj))                        return "food_drink";
                if (alch->HasKeywordString("VendorItemFoodRaw")) return "food_raw";
                return "food";
            }
            return "potion";
        }
        if (a_obj->Is(RE::FormType::Ingredient)) return "ingredient";
        if (a_obj->Is(RE::FormType::SoulGem))    return "soulgem";
        if (a_obj->Is(RE::FormType::KeyMaster))  return "key";
        if (auto* misc = a_obj->As<RE::TESObjectMISC>()) {
            if (misc->HasKeywordString("VendorItemOreIngot")) {
                return IsIngotMesh(a_obj) ? "misc_ingot" : "misc_ore";
            }
            if (misc->HasKeywordString("VendorItemGem"))        return "misc_gem";
            if (misc->HasKeywordString("VendorItemAnimalHide")) return "misc_hide";
            // narrowest first: a bone or a hammer also carries VendorItemClutter
            if (misc->HasKeywordString("VendorItemAnimalPart")) return "misc_animalpart";
            if (misc->HasKeywordString("VendorItemTool"))       return "misc_tool";
            if (misc->HasKeywordString("VendorItemClutter"))    return "misc_clutter";
        }
        return "misc";
    }

    ItemDef DefaultDef(RE::TESBoundObject* a_obj)
    {
        if (g_catDefs.empty()) InitCategoryDefs();
        const auto it = g_catDefs.find(CategoryOf(a_obj));
        return it != g_catDefs.end() ? it->second : ItemDef{};
    }


    // Upsert (or remove, when a_def==nullptr) one item's line in the override ini —
    // the in-game editor writes through this, so hand-edits elsewhere are preserved.
    void UpsertDefLine(const std::string& a_key, const ItemDef* a_def, const std::string& a_name)
    {
        std::vector<std::string> lines;
        {
            std::ifstream in(kDefsPath);
            std::string l;
            while (std::getline(in, l)) lines.push_back(l);
        }
        if (lines.empty()) {
            lines.push_back("; GridInventory item overrides (edited in-game via the EDIT mode)");
            lines.push_back("; key = w:, h:, rx:, ry:, rz:, scale:   or   shape:11|10|10 (rows of 1/0)");
        }
        bool done = false;
        for (auto it = lines.begin(); it != lines.end(); ++it) {
            const auto eq = it->find('=');
            if (eq == std::string::npos) continue;
            std::string k = it->substr(0, eq);
            k.erase(0, k.find_first_not_of(" \t"));
            k.erase(k.find_last_not_of(" \t") + 1);
            if (k != a_key) continue;
            if (a_def) {
                *it = FormatItemDef(a_key, *a_def);
            } else {
                // ★Take the "; Name" comment written directly above with it.
                // Erasing the entry alone leaves the comment behind, where it
                // then reads as the label of the NEXT, unrelated item — 211 of
                // those had piled up in the shipped file. Index >= 2 keeps the
                // two header comments safe.
                auto first = it;
                if (it != lines.begin()) {
                    const auto prev = std::prev(it);
                    if (std::distance(lines.begin(), prev) >= 2 &&
                        !prev->empty() && prev->front() == ';') {
                        first = prev;
                    }
                }
                lines.erase(first, std::next(it));
            }
            done = true;
            break;
        }
        if (!done && a_def) {
            if (!a_name.empty()) lines.push_back("; " + a_name);
            lines.push_back(FormatItemDef(a_key, *a_def));
        }
        if (std::ofstream out(kDefsPath, std::ios::trunc); out) {
            for (const auto& l : lines) out << l << "\n";
        }
    }

    ItemDef DefFor(RE::TESBoundObject* a_obj)
    {
        if (auto it = g_itemDefs.find(FormKey(a_obj)); it != g_itemDefs.end()) {
            return it->second;
        }
        // G1: Coin_Pouch is 2x2 (4 cells) out of the box; coins keep the 1x1
        // misc default. User ini overrides above still win (icon tuning).
        if (FUI::GoldCoins::IsPouch(a_obj->GetFormID())) {
            ItemDef d = DefaultDef(a_obj);
            d.w = 2;
            d.h = 2;
            return d;
        }
        // model-level fallback: an overridden sibling sharing this nif
        // (enchant variants etc.) donates its def before the category default
        if (g_modelDefsDirty) RebuildModelDefs();
        if (!g_modelDefs.empty()) {
            if (auto mp = ModelPathOf(a_obj); !mp.empty()) {
                if (auto it = g_modelDefs.find(mp); it != g_modelDefs.end()) {
                    return it->second;
                }
            }
        }
        return DefaultDef(a_obj);
    }

    // ★★WHICH ITEMS WEAR THE UNIQUE MARK, beyond what the record can say.
    // The built-in rule is "its enchantment has no base enchantment", i.e. one
    // the player can never learn -- that finds the Daedric artifacts and the
    // named uniques that carry a bespoke enchantment, and nothing else. A
    // unique that is unenchanted (the Longhammer, Valdr's Lucky Dagger),
    // scripted (Nettlebane, the Bloodskal Blade) or plainly disenchantable
    // (Grimsever, Okin, Eduj) is indistinguishable from ordinary gear in the
    // data, and a MOD's artifacts are invisible to any rule at all.
    // One form per line, in the same key the rest of this file uses:
    //     Skyrim.esm|0x01C492          ; on
    //     Skyrim.esm|0x01C492 = 0      ; off -- overrides the rule the other way
    void LoadUniqueDefs()
    {
        std::unordered_map<RE::FormID, bool> out;
        std::ifstream in(kUniquePath);
        std::string   line;
        while (in && std::getline(in, line)) {
            if (const auto c = line.find_first_of(";#"); c != std::string::npos) {
                line.erase(c);
            }
            std::string key = line;
            bool        on = true;
            if (const auto eq = key.find('='); eq != std::string::npos) {
                std::string val = key.substr(eq + 1);
                key.erase(eq);
                val.erase(0, val.find_first_not_of(" 	"));
                on = !val.empty() && val[0] != '0';
            }
            key.erase(0, key.find_first_not_of(" 	"));
            if (const auto e = key.find_last_not_of(" 	"); e != std::string::npos) {
                key.erase(e + 1);
            } else {
                continue;
            }
            if (key.empty()) continue;
            // ★Resolved through the data handler, so the load order may put the
            // plugin anywhere -- and a line naming a plugin the player does not
            // have is simply skipped rather than being an error.
            if (auto* obj = FormFromKey(key)) out[obj->GetFormID()] = on;
        }
        FUI::Grid::SetUniqueOverrides(std::move(out));
    }

    // User override file, one line per item (hot-reloaded on every inventory open):
    //   Skyrim.esm|0x0001397E = w:1, h:4, rx:-90, ry:0, rz:0, scale:1.0
    void LoadItemDefs()
    {
        g_itemDefs.clear();
        {
            std::ifstream in(kDefsPath);
            std::string line;
            while (in && std::getline(in, line)) {
                if (line.empty() || line[0] == ';' || line[0] == '#') continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                // shared metatable parser (ui/ItemDef.h) over factory defaults
                g_itemDefs[key] = ParseItemDef(line.substr(eq + 1), ItemDef{});
            }
        }
        // §RELEASE-B: the SHIPPED bags (Grid Inventory.esp Satchel 0x818 /
        // Knapsack 0x819) must act as bags out of the box, with no ini to
        // ship. Values mirror the author's Default preset. A user line parsed
        // above always wins; "Reset Default" erases the line and these seeds
        // return on the next launch -- i.e. THIS is their factory default.
        static constexpr std::pair<const char*, const char*> kShippedBagDefs[] = {
            { "Grid Inventory.esp|0x000818",   // Satchel
              "w:1, h:1, rx:90, ry:0, rz:180, scale:1.00, bag:1, bw:4, bh:4" },
            { "Grid Inventory.esp|0x000819",   // Knapsack
              "w:2, h:2, rx:0, ry:1, rz:90, scale:1.00, bag:1, bw:8, bh:4" },
            // ---- typed bags: the accept token is what makes them typed ----
            { "Grid Inventory.esp|0x00081A",   // Alchemy Pouch
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:8, bh:5, accept:alchemy" },
            { "Grid Inventory.esp|0x00081B",   // Ore Sack
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:5, accept:ore" },
            { "Grid Inventory.esp|0x00081C",   // Hide Roll
              "w:2, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:5, accept:hide" },
            { "Grid Inventory.esp|0x00081D",   // Potion Bag
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:5, accept:potion" },
            { "Grid Inventory.esp|0x00081E",   // Soul Gem Pouch
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:4, accept:soulgem" },
            { "Grid Inventory.esp|0x00081F",   // Key Pouch
              "w:2, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:5, bh:5, accept:key" },
            // ---- general purpose ----
            { "Grid Inventory.esp|0x000820",   // Small Leather Pouch
              "w:1, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:4, bh:4" },
            { "Grid Inventory.esp|0x000821",   // Leather Satchel
              "w:1, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:4, bh:5" },
            { "Grid Inventory.esp|0x000822",   // Belt Pouch
              "w:2, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:4" },
            { "Grid Inventory.esp|0x000823",   // Witching Pouch
              "w:1, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:4, bh:6" },
            { "Grid Inventory.esp|0x000824",   // Canvas Pack
              "w:3, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:8, bh:5" },
            { "Grid Inventory.esp|0x000825",   // Buckled Satchel
              "w:3, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:8, bh:6" },
            { "Grid Inventory.esp|0x000826",   // Backframe Pack
              "w:2, h:3, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:5, bh:10" },
            { "Grid Inventory.esp|0x000827",   // Adventure Satchel
              "w:3, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:9, bh:6" },
            { "Grid Inventory.esp|0x000828",   // Exploration Pack
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:10, bh:8" },
            { "Grid Inventory.esp|0x000829",   // Mysterious Bag
              "w:1, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:10, bh:14" },
        };
        for (const auto& [key, val] : kShippedBagDefs) {
            if (!g_itemDefs.contains(key)) {
                g_itemDefs[key] = ParseItemDef(val, ItemDef{});
            }
        }
        g_modelDefsDirty = true;

        // ★Typed bags: an accept token naming a filter that does not exist is
        // the quiet failure mode of this whole feature — the bag simply takes
        // nothing and looks like a routing bug. Name it at load, once.
        int bags = 0, typed = 0;
        for (const auto& [key, d] : g_itemDefs) {
            if (!d.bag) continue;
            ++bags;
            if (d.accept.empty()) continue;
            ++typed;
            bool known = false;
            for (int i = 0; i < FUI::BagFilter::Count(); ++i) {
                if (d.accept == FUI::BagFilter::Id(i)) { known = true; break; }
            }
            if (!known) {
                logger::warn("[DEFS] {}: accept:{} is not a known filter - "
                             "this bag will accept nothing", key, d.accept);
            }
        }
        logger::info("[DEFS] {} item overrides loaded ({} bags, {} typed)",
            g_itemDefs.size(), bags, typed);

        // ★Hand the merchant seeder the bag list derived from THESE defs. A
        // FormID table inside GoldCoins would be a second source of truth: add
        // a bag here and the shops would quietly never stock it.
        //
        // ★OUR esp only. Marking an item as a bag in EDIT says "I want to use
        // this as a bag", not "put this on a shopkeeper's shelf" — and putting
        // another mod's item into a vendor chest rewrites THAT mod's intended
        // acquisition. Measured on the author's load order: 16 foreign packs
        // had been designated, diluting the rotation pool to 28 so the 12
        // shipped general bags drew less than half the time (one merchant
        // rolled three foreign backpacks in a row).
        {
            constexpr std::string_view kOurs = "grid inventory.esp|";
            std::vector<FUI::GoldCoins::BagWare> wares;
            std::vector<RE::TESBoundObject*>     pouchWares;   // ★multi-pouch
            int foreign = 0;
            for (const auto& [key, d] : g_itemDefs) {
                if (!d.bag && d.pouchCap <= 0) continue;
                std::string lower = key.substr(0, (std::min)(key.size(), kOurs.size()));
                for (auto& c : lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (lower != kOurs) { ++foreign; continue; }
                auto* obj = FormFromKey(key);
                if (!obj) continue;
                if (d.bag) {
                    wares.push_back({ obj, d.accept });
                } else if (obj != FUI::GoldCoins::PouchForm()) {
                    // a def-declared pouch (the builtin 0x804 places itself)
                    pouchWares.push_back(obj);
                }
            }
            if (foreign > 0) {
                logger::info("[VENDOR] {} user-designated bag(s)/pouch(es) from "
                             "other plugins are NOT stocked (by design)", foreign);
            }
            FUI::GoldCoins::SetBagWares(std::move(wares));
            FUI::GoldCoins::SetPouchWares(std::move(pouchWares));
        }
    }

    // ---- category ini (H7) ----
    constexpr const char* kCatsPath = "Data/SKSE/Plugins/GridInventory_categories.ini";

    void SaveCategoryDefs()
    {
        std::ofstream out(kCatsPath, std::ios::trunc);
        if (!out) return;
        out << "; Category defaults for items without a per-item override\n";
        out << "; 개별 오버라이드가 없는 아이템에 적용되는 카테고리 기본값\n";
        // deterministic order (diff-able files, stable across sessions)
        const std::map<std::string, ItemDef> sorted(g_catDefs.begin(), g_catDefs.end());
        for (const auto& [name, d] : sorted) {
            out << FormatItemDef(name, d) << "\n";
        }
    }

    // ---- drawn-icon transforms (GI60) ----
    // ONE LINE PER ICON KEY, not per item. A drawing is shared by everything
    // that resolves to it (472 swords all draw wpn_sword), so how big it sits
    // and which way it points belongs to the picture. Written by IconStudio,
    // read here; an item def that names its own value still wins at draw time.
    constexpr const char* kFlatPath = "Data/SKSE/Plugins/GridInventory_flaticons.ini";

    void SaveFlatIconDefs()
    {
        std::ofstream out(kFlatPath, std::ios::trunc);
        if (!out) return;
        out << "; Drawn-icon transforms -- ONE LINE PER ICON, not per item.\n";
        out << "; 그림 아이콘 자체의 확대·회전·좌우 위치입니다.\n";
        out << "; 아이템에 개별 값이 있으면 그쪽이 우선합니다 (카테고리 기본값과 같은 규칙).\n";
        char buf[128];
        for (const auto& [key, x] : FUI::Fallback::Xforms()) {
            std::snprintf(buf, sizeof(buf), "%s = fscale:%.2f, frot:%.0f, fx:%.2f",
                key.c_str(), x.scale, x.rot, x.x);
            out << buf << "\n";
        }
    }

    void LoadFlatIconDefs()
    {
        FUI::Fallback::ClearXforms();
        std::ifstream in(kFlatPath);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            const auto b = key.find_first_not_of(" \t");
            const auto e = key.find_last_not_of(" \t\r");
            if (b == std::string::npos) continue;
            key = key.substr(b, e - b + 1);
            // reuse the item-def grammar so there is ONE parser for
            // "fscale:… , frot:… , fx:…" and it cannot drift
            const ItemDef d = ParseItemDef(line.substr(eq + 1), ItemDef{});
            FUI::Fallback::SetXform(key, { d.fscale, d.frot, d.fx });
        }
    }

    void LoadCategoryDefs()
    {
        InitCategoryDefs();
        std::ifstream in(kCatsPath);
        if (!in) return;

        // legacy migration: pre-split files carry the coarse keys — each one
        // seeds its finer children so resolved defs (and pak capture keys)
        // stay identical. Two-pass: children with their own explicit line
        // anywhere in the file must NOT be overwritten by the parent seed
        // (file order is not guaranteed, e.g. "poison" sorts before "potion").
        static const std::vector<std::pair<const char*, std::vector<const char*>>>
            kLegacySplit = {
                { "armor_jewelry",  { "armor_ring", "armor_amulet" } },
                { "weap_1h",        { "weap_sword", "weap_waraxe", "weap_mace" } },
                { "weap_2h",        { "weap_greatsword", "weap_staff" } },
                { "weap_battleaxe", { "weap_warhammer" } },
                { "ammo",           { "ammo_arrow", "ammo_bolt" } },
                { "armor_body",     { "armor_body_heavy", "armor_body_light", "armor_cloth" } },
                { "armor_head",     { "armor_circlet", "armor_hood" } },
                { "armor_hands",    { "armor_gloves" } },
                { "armor_feet",     { "armor_shoes" } },
                { "armor_cloth",    { "armor_accessory" } },
                { "book",           { "book_skill", "book_spell", "book_note" } },
                { "potion",         { "poison", "food" } },
                { "food",           { "food_raw", "food_drink" } },
                { "misc_ore",       { "misc_ingot" } },
                { "misc",           { "ingredient", "soulgem", "misc_ore", "misc_gem",
                                      "misc_hide", "misc_animalpart", "misc_tool",
                                      "misc_clutter" } },
            };

        std::vector<std::pair<std::string, std::string>> entries;   // key, values
        std::set<std::string> explicitKeys;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            entries.emplace_back(key, line.substr(eq + 1));
            explicitKeys.insert(key);
        }
        for (const auto& [key, values] : entries) {
            for (const auto& [parent, children] : kLegacySplit) {
                if (key != parent) continue;
                for (const char* child : children) {
                    if (explicitKeys.contains(child)) continue;
                    g_catDefs[child] = ParseItemDef(values, g_catDefs[child]);
                }
            }
            const auto it = g_catDefs.find(key);
            if (it != g_catDefs.end()) {
                it->second = ParseItemDef(values, it->second);
            }
        }
    }

    void RewriteItemDefsFile()
    {
        std::ofstream out(kDefsPath, std::ios::trunc);
        if (!out) return;
        out << "; GridInventory item overrides (edited in-game via the EDIT mode)\n";
        out << "; key = w:, h:, rx:, ry:, rz:, scale:   or   shape:11|10|10 (rows of 1/0)\n";
        // Phase 2: deterministic (sorted) order — the old unordered dump
        // reshuffled the whole file on every preset load — and the editor's
        // per-item name comments are regenerated by live form lookup instead
        // of being silently dropped.
        auto* dh = RE::TESDataHandler::GetSingleton();
        const std::map<std::string, ItemDef> sorted(g_itemDefs.begin(), g_itemDefs.end());
        for (const auto& [key, d] : sorted) {
            if (dh) {
                const auto bar = key.find('|');
                if (bar != std::string::npos && key.compare(bar + 1, 2, "0x") == 0) {
                    const auto localID = static_cast<RE::FormID>(
                        std::strtoul(key.c_str() + bar + 3, nullptr, 16));
                    if (auto* f = dh->LookupForm(localID, key.substr(0, bar))) {
                        if (const char* nm = f->GetName(); nm && *nm) {
                            out << "; " << nm << "\n";
                        }
                    }
                }
            }
            out << FormatItemDef(key, d) << "\n";
        }
    }

    // ---- Intercept the vanilla InventoryMenu: block it and open our UI instead ----
    // ---- Phase 3: menu open/close handling, one function per menu concern ----
    // Each returns true when the event is fully handled (stop processing).

    // Engine MessageBoxes (e.g. "apply poison to weapon?") render UNDER the
    // movie-less menu and can't be clicked through it — step aside while the
    // box is up, come back when it closes.
    bool HandleMessageBoxAside(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::MessageBoxMenu::MENU_NAME) return false;
        auto* ui = RE::UI::GetSingleton();
        if (a_event.opening && ui && ui->IsMenuOpen("GridInventoryMenu"sv)) {
            g_reopenAfterMsg = true;
            FUI::UIRoot::Close();
            logger::info("[INV] MessageBox opened -> stepping aside");
        } else if (!a_event.opening && g_reopenAfterMsg) {
            g_reopenAfterMsg = false;
            FUI::UIRoot::Open();
            logger::info("[INV] MessageBox closed -> back to the grid");
        }
        return true;
    }

    // While an ImGui text field owns the keyboard, hotkey menus must not open
    // over us — J still opened the Journal even with the user-event channel
    // swallowed (its open path is global). Intercept-and-close.
    bool HandleTextInputHotkeyBlock(const RE::MenuOpenCloseEvent& a_event)
    {
        if (!a_event.opening || !FUI::UIRoot::IsTextInputActive()) return false;
        if (a_event.menuName != RE::JournalMenu::MENU_NAME &&
            a_event.menuName != RE::TweenMenu::MENU_NAME &&
            a_event.menuName != RE::MapMenu::MENU_NAME &&
            a_event.menuName != RE::MagicMenu::MENU_NAME &&
            a_event.menuName != RE::StatsMenu::MENU_NAME &&
            a_event.menuName != RE::FavoritesMenu::MENU_NAME) {
            return false;
        }
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(a_event.menuName, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
        logger::info("[INV] blocked {} (text input active)", a_event.menuName.c_str());
        return true;
    }

    // ★★The vanilla favourites menu is closed on sight -- the quick wheel has
    // taken its place and answers to the same key. Without this, one press
    // opens both: the wheel reads the key from the input stream while the menu
    // opens through the engine's own path, and the two are not the same road.
    // ★No "swallow-then-open" dance like the inventory below needs. The wheel
    // is already up by the time this arrives; there is nothing to wait for.
    bool HandleFavoritesMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::FavoritesMenu::MENU_NAME) return false;
        // ★Measurement, not decoration. "The vanilla menu still does not
        // appear" has two very different causes -- we closed it, or the engine
        // never opened it -- and only the event itself can tell them apart. If
        // no line appears when the key is pressed, the press never became a
        // menu request and nothing on our side is holding it.
        logger::info("[FAV] FavoritesMenu {} (wheel={} yielding={})",
            a_event.opening ? "opening" : "closing",
            FUI::Wheeler::Enabled() ? 1 : 0,
            FUI::Wheeler::YieldingToVanilla() ? 1 : 0);
        // ★★★THE SWITCH LIVES HERE, not only on the wheel's own input. This is
        // the half that gives the vanilla screen back: with the wheel off it
        // opens exactly as it always did, hotkey binding and all. Gating only
        // the wheel would have left the menu suppressed with nothing put in
        // its place -- the favourites key would simply have stopped working.
        if (!FUI::Wheeler::Enabled()) return false;
        // ★...and the same stand-down the hotkey makes. In beast form the wheel
        // passes the key through, the engine opens its own menu -- and this
        // line used to shut it again a frame later, so the player saw NOTHING
        // open and stayed locked in the form (measured: the yield fired on
        // every press, no menu). The takeover has two halves and both have to
        // let go.
        if (FUI::Wheeler::YieldingToVanilla()) return false;
        if (!a_event.opening) return false;
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(RE::FavoritesMenu::MENU_NAME,
                RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
        return true;
    }

    // Vanilla InventoryMenu: swallow-then-open (our grid opens once the
    // vanilla menu finished closing).
    // ★★★ONE PLACE DECIDES WHETHER THE ENGINE KEEPS A SCREEN.
    //
    // Both switches -- the F11 key and the watch file -- used to be checked in
    // the inventory intercept alone. So F11 swapped your own bags and left the
    // MERCHANT's window as ours (reported), which is exactly the comparison the
    // key exists to make: a shop's stock is built by levelled lists, and the
    // only way to prove we hide none of it is to open the same shop both ways.
    //
    // Every screen we take over asks here now: inventory, container, barter.
    [[nodiscard]] bool VanillaPassthrough(const char* a_tag)
    {
        if (g_vanillaKey.load()) {
            logger::warn("[{}] vanilla passthrough is ON (F11) -- "
                         "the engine keeps this screen", a_tag);
            return true;
        }
        // ★ⓔⓖ WATCH: drop a file named GridInventory_vanilla.txt beside the
        // plugin and the vanilla screens open instead of ours, for as long as
        // it is there. It exists so the engine's own behaviour can be watched
        // from the same session that watches ours -- delete the file and the
        // grid comes back, no restart.
        std::error_code ec;
        if (std::filesystem::exists(
                "Data/SKSE/Plugins/GridInventory_vanilla.txt", ec)) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                logger::warn("[{}] GridInventory_vanilla.txt present -- "
                             "the vanilla screens are being left alone", a_tag);
            }
            return true;
        }
        return false;
    }

    bool HandleInventoryMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::InventoryMenu::MENU_NAME) return false;
        if (!a_event.opening && g_planBPendingOpen) {
            g_planBPendingOpen = false;
            FUI::UIRoot::Open();
            logger::info("[INV] InventoryMenu closed -> GridInventoryMenu opening");
            return true;
        }
        if (a_event.opening) {
            if (VanillaPassthrough("INV")) return false;
            // close the vanilla inventory that just opened
            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                mq->AddMessage(RE::InventoryMenu::MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide, nullptr);
                // launched from the TAB (Tween) menu: our instant hide robs
                // it of its normal close-on-select, so it lingers under the
                // grid and is still there after we close
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->IsMenuOpen(RE::TweenMenu::MENU_NAME)) {
                    mq->AddMessage(RE::TweenMenu::MENU_NAME,
                        RE::UI_MESSAGE_TYPE::kHide, nullptr);
                }
            }
            g_planBPendingOpen = true;
            g_echoMenu = RE::InventoryMenu::MENU_NAME;   // ⑫
            logger::info("[INV] intercepted InventoryMenu -> deferring GridInventoryMenu open");
        }
        return false;   // opening intercept falls through (matches old flow)
    }

    // ★★★⑫ THE CLOSE NOBODY HEARD.
    //
    // Measured, not guessed: TK Dodge's `aaaTKDodgeScript.pex` decompiles to
    //
    //     Event OnMenuOpen(String menuName)
    //         GotoState("Busy")            ; where OnKeyDown is an EMPTY function
    //     EndEvent
    //     Event OnMenuClose(String menuName)
    //         If !Utility.IsInMenuMode()   ; <-- the whole bug lives on this line
    //             GotoState("")
    //         EndIf
    //     EndEvent
    //
    // and the chain it meets on our side is:
    //
    //   1. the vanilla InventoryMenu opens        -> TK goes Busy
    //   2. we kHide it in that same event         -> a close is announced
    //   3. we open OUR menu inside that handler   -> kPausesGame, so menu mode
    //                                                is ON again
    //   4. Papyrus delivers the close (later)     -> IsInMenuMode() is true, so
    //                                                TK never leaves Busy
    //   5. our grid closes                        -> we are not a menu TK
    //                                                registered for: silence
    //
    // Dodge stays dead until some OTHER registered menu closes with nothing
    // open -- which is exactly the report: "press ESC and come back and it
    // works, open the inventory again and it stops."
    //
    // TK's guard is right (do not go idle while a menu is still up). What is
    // wrong is OUR story: we let the engine announce an inventory that opened,
    // then announce it closed a frame later while the inventory is in fact
    // still on screen, and we say nothing at all when it really ends. Every
    // Papyrus mod listening for "the inventory closed" hears it at the wrong
    // moment. This says it at the right one -- once, on the first unpaused
    // frame after our grid is gone, and only for an open the engine itself
    // announced.
    //
    // Mods that act on the early close will now see two; that is strictly
    // better than the one they see being a lie.
    //
    // ★★AND IT IS NOT ONLY THE INVENTORY. ContainerMenu and BarterMenu come
    // through the identical swallow-then-open door, and TK registers for both
    // (checked in its string table). The report named the inventory because
    // that is what the reporter tried; looting a chest breaks dodge exactly the
    // same way. Fixing one of three would have sent the bug straight back.
    //
    // FavoritesMenu is swallowed too and is NOT here -- deliberately. What
    // replaces it is the wheel, which is not kPausesGame (Wheeler.cpp), so menu
    // mode really has ended by the time that close is delivered and the guard
    // passes on its own.
    void MenuCloseEchoTick()
    {
        if (g_echoMenu.empty()) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        // This tick only runs unpaused (kPausesGame stops the Update hook), so
        // both tests are belt and braces -- and both are the condition the
        // listener is about to evaluate for itself. Waiting for BOTH is what
        // makes the hop-outs work: the magic key closes our grid and raises
        // MagicMenu in the same breath, and this simply keeps owing until that
        // screen is gone too.
        if (ui->IsMenuOpen("GridInventoryMenu"sv) || ui->GameIsPaused()) return;
        RE::MenuOpenCloseEvent e{};
        e.menuName = g_echoMenu;
        e.opening = false;
        g_echoMenu = {};
        // UI is three event sources at once; name the one we mean
        static_cast<RE::BSTEventSource<RE::MenuOpenCloseEvent>*>(ui)->SendEvent(&e);
        logger::info("[INV] session over -> announcing '{}' close", e.menuName.c_str());
    }

    // ---- lockpick auto-open fallback ----
    // Vanilla re-activates a container automatically after a successful pick;
    // in this load order that never happens (no ContainerMenu event at all —
    // diagnosed 2026-07-24, some mod suppresses it). Fallback: when the
    // lockpicking menu closes with the lock OPEN, re-activate the container
    // ourselves a few frames later — unless something else (vanilla path,
    // QuickLoot's widget, our own grid) claimed the moment first.
    RE::ObjectRefHandle g_pickTarget;        // captured at menu OPEN
    RE::ObjectRefHandle g_pickReopen;        // armed at menu CLOSE (unlocked)
    int                 g_pickReopenDelay = 0;

    bool HandleLockpickAutoReopen(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::LockpickingMenu::MENU_NAME) return false;
        // ★GetTargetReference hands back a smart pointer on the NG line
        if (a_event.opening) {
            const auto tp = RE::LockpickingMenu::GetTargetReference();
            auto* target = tp.get();
            g_pickTarget = target ? target->CreateRefHandle() : RE::ObjectRefHandle{};
            return false;   // observation only
        }
        const auto tp = RE::LockpickingMenu::GetTargetReference();
        auto* target = tp.get();
        if (!target) target = g_pickTarget.get().get();
        g_pickTarget = {};
        if (target && !target->IsLocked() && target->GetBaseObject() &&
            target->GetBaseObject()->Is(RE::FormType::Container)) {
            g_pickReopen = target->CreateRefHandle();
            g_pickReopenDelay = 10;   // ~0.15s head start for the native path
        }
        return false;   // never consumes the event
    }

    // called per unpaused frame from UpdateHook
    void LockpickReopenTick()
    {
        if (g_pickReopenDelay <= 0) return;
        if (--g_pickReopenDelay > 0) return;
        const auto handle = g_pickReopen;
        g_pickReopen = {};
        auto ref = handle.get();
        auto* ui = RE::UI::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!ref || !ui || !player) return;
        // yield when anything already claimed the moment
        if (ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) ||
            ui->IsMenuOpen("LootMenu") ||                    // QuickLoot widget
            ui->IsMenuOpen("GridInventoryMenu") ||
            ui->GameIsPaused()) {
            return;
        }
        logger::info("[LOOT] lockpick auto-open fallback -> activating container");
        ref->ActivateRef(player, 0, nullptr, 1, false);
    }

    // LOOT: ContainerMenu — every mode is intercepted now: kLoot chest/corpse,
    // kNPCMode companion, kSteal owned containers (F6a) and kPickpocket
    // living targets (F6b, vanilla roll via AttemptPickpocket). Same
    // swallow-then-open pattern.
    bool HandleContainerMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::ContainerMenu::MENU_NAME) return false;
        if (!a_event.opening && g_pendingPartnerOpen) {
            g_pendingPartnerOpen = false;
            FUI::UIRoot::Open();
            logger::info("[LOOT] ContainerMenu closed -> grid opening");
            return true;
        }
        if (a_event.opening) {
            if (VanillaPassthrough("LOOT")) return false;
            auto* ui = RE::UI::GetSingleton();
            auto  menu = ui ? ui->GetMenu<RE::ContainerMenu>() : nullptr;
            if (!menu) {   // unreadable mode: leave the vanilla menu
                logger::warn("[LOOT] ContainerMenu opening but the menu object "
                             "isn't registered yet — not intercepted");
                return false;
            }
            const auto cmode = menu->GetContainerMode();
            FUI::LootBarter::Mode gmode;
            switch (cmode) {
            case RE::ContainerMenu::ContainerMode::kLoot:
            case RE::ContainerMenu::ContainerMode::kNPCMode:
                gmode = FUI::LootBarter::Mode::kLoot;
                break;
            case RE::ContainerMenu::ContainerMode::kSteal:
                gmode = FUI::LootBarter::Mode::kSteal;
                break;
            case RE::ContainerMenu::ContainerMode::kPickpocket:
                gmode = FUI::LootBarter::Mode::kPickpocket;
                break;
            default:
                return false;   // unknown future mode: vanilla
            }
            auto* ref = HandleToRef(RE::ContainerMenu::GetTargetRefHandle());
            FUI::LootBarter::Enter(gmode, ref);
            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                mq->AddMessage(RE::ContainerMenu::MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            g_pendingPartnerOpen = true;
            g_echoMenu = RE::ContainerMenu::MENU_NAME;   // ⑫
            logger::info("[LOOT] intercepted ContainerMenu (mode {}) -> deferring grid",
                static_cast<int>(cmode));
        }
        return false;
    }

    // BARTER: BarterMenu (merchant).
    bool HandleBarterMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::BarterMenu::MENU_NAME) return false;
        if (!a_event.opening && g_pendingPartnerOpen) {
            g_pendingPartnerOpen = false;
            FUI::UIRoot::Open();
            logger::info("[BARTER] BarterMenu closed -> grid opening");
            return true;
        }
        if (a_event.opening) {
            if (VanillaPassthrough("BARTER")) return false;
            // BarterMenu::GetTargetRefHandle returns the PLAYER, not the
            // merchant — the merchant is the dialogue partner.
            RE::TESObjectREFR* ref = nullptr;
            if (auto* mtm = RE::MenuTopicManager::GetSingleton()) {
                if (auto sp = mtm->speaker.get()) ref = sp.get();
                // ★the dialogue may have closed a frame before the shop opened;
                // the engine keeps the partner here for exactly that window
                // ("the dialogue menu was closed but the NPC is still talking")
                if (!ref) {
                    if (auto sp = mtm->lastSpeaker.get()) ref = sp.get();
                }
            }
            // ★★★AND IF WE STILL DO NOT KNOW WHO THE MERCHANT IS, WE STAND
            // DOWN. This used to fall back to GetTargetRefHandle -- which the
            // comment above already says is the PLAYER -- so a shop we could
            // not identify was rendered with the player seated as the
            // merchant: your own inventory on both sides of the window.
            //
            // Reported against Faction Camps, and the shape fits: a camp opens
            // its shop from a script when you activate a tent, with no
            // conversation at all, so there is no speaker to find. Vanilla
            // merchants are always reached through dialogue, which is why this
            // never showed up in testing.
            //
            // Standing down is the honest answer -- we cannot draw a shelf for
            // a shop we cannot name. The vanilla barter screen opens instead
            // and the trade works; only our grid is missing.
            if (!ref || ref == RE::PlayerCharacter::GetSingleton()) {
                logger::warn("[BARTER] no merchant identified (a script-opened "
                             "shop?) -> leaving the vanilla menu up");
                return false;
            }
            FUI::LootBarter::Enter(FUI::LootBarter::Mode::kBarter, ref);
            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                mq->AddMessage(RE::BarterMenu::MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            g_pendingPartnerOpen = true;
            g_echoMenu = RE::BarterMenu::MENU_NAME;   // ⑫
            logger::info("[BARTER] intercepted BarterMenu -> deferring grid");
        }
        return false;
    }



    class InvMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static InvMenuSink* GetSingleton()
        {
            static InvMenuSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event) {
                // ★★WHILE TRANSFORMED, NAME EVERY MENU. "Q does nothing in
                // beast form" has three different causes that look identical
                // from outside: no menu was raised at all, the favourites menu
                // was raised and shut again, or the game raised something else
                // entirely (a beast form need not map that key the way a body
                // does). Only the engine's own event stream tells them apart,
                // so while the player is a beast we write down every menu that
                // opens or closes -- and the yield line from the wheel sits
                // right above it in the same log, at the same second.
                // Scoped to the transformation so ordinary play stays quiet.
                if (FUI::Wheeler::YieldingToVanilla()) {
                    logger::info("[BEAST] menu '{}' {}", a_event->menuName.c_str(),
                        a_event->opening ? "OPENING" : "closing");
                }
                // one handler per menu concern; true = event fully handled
                HandleLockpickAutoReopen(*a_event);   // observation only
                HandleMessageBoxAside(*a_event) ||
                    HandleTextInputHotkeyBlock(*a_event) ||
                    HandleFavoritesMenuIntercept(*a_event) ||
                    HandleInventoryMenuIntercept(*a_event) ||
                    HandleContainerMenuIntercept(*a_event) ||
                    HandleBarterMenuIntercept(*a_event);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Setup()
    {
        // ---- UI bootstrap ----
        FUI::UIRoot::RegisterMenu();
        FUI::Wheeler::RegisterMenu();
        FUI::UIRoot::TryInitD3D();   // renderer is live at kDataLoaded; retried in Open() if not
        // capture orientation = same resolution path as the old pipeline:
        // items ini override -> category preset (PLAN_B §2-G3)
        // D2: IconDef/GridDef are the SAME struct as ItemDef now — the old
        // field-by-field converters collapse to the resolver itself
        FUI::IconCache::GetSingleton()->SetDefResolver(
            [](RE::TESBoundObject* a_obj) -> FUI::IconDef { return DefFor(a_obj); });
        FUI::Grid::SetDefResolver(
            [](RE::TESBoundObject* a_obj) -> FUI::Grid::GridDef { return DefFor(a_obj); });
        // ★Multi-pouch: a pouch's capacity comes from its item def
        // ("pouchcap:N"), so a future pouch form is an ESP record plus one
        // ini line -- no code. The builtin 0x804 stays seeded at 10,000.
        // ★RAW MAP, NOT DefFor. DefFor's own fallback asks IsPouch (the
        // builtin 2x2 sizing), and IsPouch asks this resolver -- routing the
        // resolver back through DefFor closed that circle and the first
        // IsCoinForm on any un-ini'd item recursed to a stack overflow
        // (crash-2026-08-26-11-40-10). pouchcap only ever comes from an
        // explicit ini entry, so the raw map is the complete answer.
        FUI::GoldCoins::SetPouchDefResolver([](RE::FormID a_id) -> int {
            auto* f = RE::TESForm::LookupByID(a_id);
            auto* obj = f ? f->As<RE::TESBoundObject>() : nullptr;
            if (!obj) return 0;
            const auto it = g_itemDefs.find(FormKey(obj));
            return it != g_itemDefs.end() ? it->second.pouchCap : 0;
        });
        FUI::Grid::SetGameCallbacks(
            [](RE::TESBoundObject* a_obj, bool a_up) {   // vanilla per-item sounds (I2)
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    player->PlayPickUpSound(a_obj, a_up, false);
                }
            },
            [](RE::TESBoundObject* a_obj, int a_count,
               RE::ExtraDataList* a_xl) {   // C5/D1: world drop
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) return;
                int owned = 0;
                auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& o) { return &o == a_obj; });
                for (auto& [o2, d2] : inv) owned = d2.first;
                if (owned <= 0) return;
                // a_count > 0 = partial (D1: hover+R drops one); <= 0 = stack
                const int count = a_count > 0 ? (std::min)(a_count, owned) : owned;
                // RemoveItem(kDropping) = the vanilla drop path; DropObject
                // direct is a known CTD from task context
                player->PlayPickUpSound(a_obj, false, false);
                // 1.4/B0: the first round showed drops arriving as req=? simply
                // because nothing had registered them -- which inflates the
                // "external delta" share and makes the echo figure unreadable.
                // ★Slotless by design: this callback never learns the tile key.
                // A slotless confirmation consumes nobody's queued cell
                // (CommitSlotDrop is keyed), and the queue entry the drop DID
                // create expires into the rebuild sweep once its layout entry
                // is pruned.
                FUI::Ledger::Submit(a_obj->GetFormID(), -count, "drop");
                player->RemoveItem(a_obj, count, RE::ITEM_REMOVE_REASON::kDropping,
                    a_xl, nullptr);   // GI25: the named sub-stack
            });
        // ---- B-6: editor hooks (def storage / ini / presets live here) ----
        // D2: FullDef == ItemDef — the toFull/fromFull converters are gone;
        // only the editor's shape-bounds re-derivation survives.
        {
            FUI::Editor::Hooks hooks;
            hooks.getEffective = [](RE::TESBoundObject* o) { return DefFor(o); };
            hooks.getDefault = [](RE::TESBoundObject* o) { return DefaultDef(o); };
            hooks.hasOverride = [](RE::TESBoundObject* o) {
                return g_itemDefs.contains(FormKey(o));
            };
            hooks.setOverride = [](RE::TESBoundObject* o, const FUI::Editor::FullDef& f,
                                   bool a_persist) {
                const std::string key = FormKey(o);
                ItemDef d = f;
                DeriveShapeBounds(d);   // the editor may have repainted the mask
                g_itemDefs[key] = d;   // live: the resolvers see it immediately
                g_modelDefsDirty = true;
                if (a_persist) {
                    UpsertDefLine(key, &d, o->GetName() ? o->GetName() : "");
                }
            };
            hooks.resetOverride = [](RE::TESBoundObject* o) {
                const std::string key = FormKey(o);
                g_itemDefs.erase(key);
                g_modelDefsDirty = true;
                UpsertDefLine(key, nullptr, "");
            };
            hooks.saveAsCategory = [](RE::TESBoundObject* o, const FUI::Editor::FullDef& f) {
                ItemDef d = f;
                DeriveShapeBounds(d);
                d.bag = 0;   // bags are per-item, never a category trait
                d.accept.clear();   // ...and so is what a bag accepts
                g_catDefs[CategoryOf(o)] = d;
                SaveCategoryDefs();
            };
            hooks.categoryName = [](RE::TESBoundObject* o) { return std::string(CategoryOf(o)); };
            FUI::Editor::SetHooks(std::move(hooks));
        }

        // Per-item drawn icons are named after this exact string. Handing the
        // function over instead of letting Fallback spell it again keeps ONE
        // definition of "which item is this line about" — the item ini and the
        // PNG file name can then never disagree.
        FUI::Fallback::SetFormKeyResolver([](RE::TESForm* f) { return FormKey(f); });

        // GI47: the settings-window preset is the ONE share file -- it carries
        // the item/category defs alongside the style keys. The def storage
        // lives here, so WinManager takes it through hooks.
        FUI::WinManager::GetSingleton()->SetPresetDefsHooks(
            [](std::ostream& o) {
                o << "[categories]\n";
                const std::map<std::string, ItemDef> sc(g_catDefs.begin(), g_catDefs.end());
                for (const auto& [name, d] : sc) o << FormatItemDef(name, d) << "\n";
                // GI47: the WHOLE universe, not just the overrides. An item the
                // author left at default is a CHOICE ("the default look is
                // right"), and on import it must beat the reader's local tweak
                // of that same item -- so every playable item's EFFECTIVE def
                // travels. Items from mods only the reader has never appear
                // here, so their tweaks survive the import untouched.
                o << "[items]\n";
                std::map<std::string, ItemDef> si;
                auto sweepDefs = [&](const auto& a_arr) {
                    for (auto* form : a_arr) {
                        auto* obj = form ? form->template As<RE::TESBoundObject>() : nullptr;
                        if (!obj || !obj->GetPlayable()) continue;
                        const char* nm = obj->GetName();
                        if (!nm || !nm[0]) continue;
                        if (!obj->GetFile(0)) continue;   // runtime form: no stable key
                        si[FormKey(obj)] = FUI::Grid::ResolveDef(obj);
                    }
                };
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    sweepDefs(dh->GetFormArray<RE::TESObjectWEAP>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectARMO>());
                    sweepDefs(dh->GetFormArray<RE::TESAmmo>());
                    sweepDefs(dh->GetFormArray<RE::AlchemyItem>());
                    sweepDefs(dh->GetFormArray<RE::IngredientItem>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectBOOK>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectMISC>());
                    sweepDefs(dh->GetFormArray<RE::TESSoulGem>());
                    sweepDefs(dh->GetFormArray<RE::TESKey>());
                    sweepDefs(dh->GetFormArray<RE::ScrollItem>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectLIGH>());
                }
                for (const auto& [key, d] : si) o << FormatItemDef(key, d) << "\n";
                // GI60: drawn-icon transforms travel too — they are part of
                // how the author's inventory LOOKS, which is what a preset is.
                o << "[flat]\n";
                char fb[128];
                for (const auto& [key, x] : FUI::Fallback::Xforms()) {
                    std::snprintf(fb, sizeof(fb), "%s = fscale:%.2f, frot:%.0f, fx:%.2f",
                        key.c_str(), x.scale, x.rot, x.x);
                    o << fb << "\n";
                }
            },
            [](int a_section, const std::string& a_key, const std::string& a_val) {
                if (a_section == 4) {   // [flat]
                    const ItemDef d = ParseItemDef(a_val, ItemDef{});
                    FUI::Fallback::SetXform(a_key, { d.fscale, d.frot, d.fx });
                } else if (a_section == 1) {
                    if (const auto it = g_catDefs.find(a_key); it != g_catDefs.end()) {
                        it->second = ParseItemDef(a_val, it->second);
                    }
                } else {
                    // GI47: a preset line is that item's WHOLE def -- parsed
                    // over the factory default, never over the reader's tweak,
                    // so every shared-universe item becomes exactly the
                    // author's (untouched-by-author included).
                    g_itemDefs[a_key] = ParseItemDef(a_val, ItemDef{});
                }
            },
            []() {
                SaveCategoryDefs();
                RewriteItemDefsFile();
                SaveFlatIconDefs();
                g_modelDefsDirty = true;
                FUI::Grid::RequestRebuild();
                FUI::Grid::MarkCapacityDirty();
            });

        // Load the def inis NOW (kDataLoaded) — not only on first menu open.
        // The post-load capacity compute runs BEFORE any menu: with g_itemDefs
        // empty it lost every user override (bag flags above all), counted bag
        // CONTENTS as main-board occupants and reported a false overload
        // ("slow until the inventory is opened", log-verified).
        // ★GI71: BEFORE any settings read. "!lang" is stored as an id, so the
        // pack list has to exist for that id to resolve to anything — load them
        // afterwards and a user on a pack silently reverts to English once,
        // then saves that revert back over their choice.
        FUI::Lang::LoadPacks();
        // ★Typed bags: BEFORE LoadItemDefs. That loader validates every bag's
        // accept token against this list, so an empty list would report each
        // typed bag as naming an unknown filter — the loudest possible version
        // of the exact false alarm the check exists to prevent. Keyword lookup
        // needs the game data, which kDataLoaded guarantees.
        FUI::BagFilter::SetCategoryResolver(
            [](RE::TESBoundObject* o) { return std::string(CategoryOf(o)); });
        FUI::BagFilter::Load();

        LoadCategoryDefs();
        LoadItemDefs();
        LoadUniqueDefs();
        LoadFlatIconDefs();

        // ★★★AND THE UI INI, HERE -- not when a window first asks for its
        // place. WinManager loads it lazily from ApplyNext, so until the
        // INVENTORY had been opened once nothing in that file was in effect;
        // and the wheel does not use ApplyNext at all, because it is a
        // full-screen overlay with no managed window.
        //
        // ★★That made the quick wheel come up in the wrong skin AND with
        // every icon a category drawing, on a machine whose pak was complete:
        // `!caplight` is part of every cache KEY, so a wheel drawn before the
        // ini was read hashed its lookups against the default lamp (0,0) and
        // missed a pak captured at the player's own angle -- ALL of it, every
        // time, until a bag was opened. Which is why the file's own comment
        // ("loaded BEFORE any icon is asked for") read as true and was not:
        // it describes the order INSIDE Load, and Load itself came late.
        //
        // ★A settings file is read once, at load, before anything can ask a
        // question it answers. Wheeler::LoadSettings already had to reach
        // past this for `!wheelon` alone (see its comment); that is the same
        // bug reported once and fixed one key at a time.
        FUI::WinManager::GetSingleton()->Load();

        FUI::UIRoot::SetVisibilityCallbacks(
            []() {   // menu shown
                // ★Hot-reload BY TIMESTAMP. These four parses ran on every
                // open -- ~5,000 override lines re-read to produce the same
                // tables -- because hot reload was implemented as "always
                // reload". The reload's PURPOSE is picking up edits, and an
                // edit is visible in the file's write time, so unchanged
                // files now skip the parse (measured ~20-30ms per open).
                // The editor's own saves bump the timestamp like any external
                // edit, so nothing about the reload story changes.
                namespace fs = std::filesystem;
                static const char* kWatched[] = { kCatsPath, kDefsPath,
                                                  kUniquePath, kFlatPath };
                static fs::file_time_type s_seen[4]{};
                static bool s_first = true;
                bool changed = s_first;
                s_first = false;
                for (int i = 0; i < 4; ++i) {
                    std::error_code ec;
                    const auto t = fs::last_write_time(kWatched[i], ec);
                    // a missing file reads as epoch -- still a comparable
                    // value, so deleting or restoring an ini counts as a change
                    if (t != s_seen[i]) {
                        s_seen[i] = t;
                        changed = true;
                    }
                }
                if (changed) {
                    LoadCategoryDefs();   // hot-reload category defaults (H7)
                    LoadItemDefs();       // hot-reload user overrides (same as legacy path)
                    LoadUniqueDefs();     // ...and the unique declarations beside them
                    LoadFlatIconDefs();   // hot-reload IconStudio's drawn-icon edits
                }
                // typed bags phase 0: classify what the player is carrying and
                // write the tally out. ONCE per session — this is an
                // observation, not a feature, and it must not cost anything on
                // every open. Nothing is routed or moved.
                static bool s_bagDumped = false;
                if (!s_bagDumped) {
                    s_bagDumped = true;
                    FUI::BagFilter::DumpFormDatabase();
                    FUI::BagFilter::DumpPlayerInventory();
                }
                // NOTE (A3, 2026-07-13): the attack handler is NOT disabled any
                // more. kPausesGame + the kInventory menu context already keep
                // clicks from the gameplay layer, and toggling
                // inputEventHandlingEnabled mid-hold corrupted the held-input
                // bookkeeping — closing after an in-menu right-click fired a
                // POWER ATTACK (the legacy PrismaUI-era block was for a
                // context-less overlay and no longer applies).
            },
            []() {   // menu hidden
            });

        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(InputSink::GetSingleton());
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(InvMenuSink::GetSingleton());
        }
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            // capacity: container-take bounce (menu-scoped, see ContainerSink)
            holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
            holder->AddEventSink<RE::TESResetEvent>(ResetSink::GetSingleton());
            // W2: worn state changes the board occupancy
            holder->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
        }
        logger::info("[SETUP] ready (ImGui inventory)");
    }

    // Player 3D is rebuilt across save load / new game: every cached node pointer
    // becomes stale (rule 4-3 #2). Drop them all and hide the UI if it was open.
    void ResetSession()
    {
        // restore ONLY if we disabled it (never touch input state during load otherwise)
        if (g_movementOff) {
            SetMoveInput(true);
            g_movementOff = false;
        }
        g_planBPendingOpen = false;
        g_reopenAfterMsg = false;
        // ★★★A DEBT OWED TO A SAVE THAT IS GONE. g_echoMenu names a vanilla
        // menu whose close we still have to announce; left set across a load,
        // MenuCloseEchoTick fires it on the FIRST unpaused frame of the new
        // session, and every Papyrus mod listening for that menu (TK Dodge and
        // friends) is told the previous game's container just closed.
        g_echoMenu = {};
        g_pendingPartnerOpen = false;
        // ★Same shape, shorter fuse: a lockpick handle armed at close will
        // ActivateRef ten frames into whatever game is loaded next.
        g_pickReopen = {};
        g_pickReopenDelay = 0;
        // NOTE: Loadout reset moved to the serialization REVERT callback (L3) —
        // kPostLoadGame arrives AFTER the cosave load and would wipe the tabs.
        FUI::UIRoot::Close();   // hide across load/new game
    }

    // Registered with sender == "SKSE", so every `type` here really is a
    // lifecycle value. GI10's ABI messages arrive on a SEPARATE listener
    // (HostApi::Install) precisely so that stays true.
    void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            FUI::HostApi::Broadcast();   // GI10: announce the host to providers
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            Setup();
            FUI::GoldCoins::InitForms();   // G1: resolve Grid Inventory.esp
            // ★B3-a: close the loop the ledger opened. Registered once, here,
            // where the forms are already resolved.
            // ★A confirmation commits ITS OWN cell and no other: the slot key
            // rides the request (Ledger.h), so a slotless drop or use can
            // never pop a pending store's key -- the count-based version did
            // exactly that whenever two paths moved the same form.
            FUI::Ledger::SetOnExpire([](const FUI::Ledger::Expired& a_e) {
                FUI::Grid::OnRequestExpired(a_e.form, a_e.delta, a_e.who, a_e.slot);
            });
            FUI::Ledger::SetOnConfirm([](const FUI::Ledger::Expired& a_e) {
                if (a_e.delta < 0) FUI::Grid::CommitSlotDrop(a_e.form, a_e.slot);
                // ★A confirmed consume releases its suppression entry NOW --
                // see ReleaseAppliedPendingEquip. Without this the entry
                // overlapped the dropped engine count for one rebuild and the
                // stack was subtracted twice ("one drink removed two", and the
                // last unit lost its cell to the front gap).
                if (a_e.delta < 0 && a_e.who && std::strcmp(a_e.who, "use") == 0) {
                    FUI::Grid::ReleaseAppliedPendingEquip(a_e.form);
                }
            });
            break;
        case SKSE::MessagingInterface::kNewGame:
            ResetSession();
            FUI::DeltaWatch::Reset("new game");
            FUI::Census::Reset("new game");
            FUI::Ledger::Reset("new game");
            SKSE::GetTaskInterface()->AddTask([]() {
                FUI::WornLedger::Rebaseline("new game");
            });
            // no cosave load callback fires on new game — start with an empty
            // grid layout instead of migrating the legacy ini (old saves only)
            FUI::Grid::MarkLayoutFresh();
            // ⓛ probe: the museum index, once the forms are real
            SKSE::GetTaskInterface()->AddTask([]() { FUI::Lotd::Rebuild(); });
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ResetSession();
            // ★Before, not after: the engine swaps the inventory during the
            // load and any event that crosses it belongs to neither side.
            FUI::DeltaWatch::Reset("load");
            FUI::Census::Reset("load");
            FUI::Ledger::Reset("load");
            // ★The museum handles name THIS game's references. Dropped before
            // the swap, rebuilt after it (kPostLoadGame).
            FUI::Lotd::Clear();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            ResetSession();
            // B4-2: a load is a discontinuity (rule 3) -- the worn ledger
            // rebuilds from the engine once, here, where the new inventory is
            // real. Deferred a task so the walk runs after the engine settles.
            SKSE::GetTaskInterface()->AddTask([]() {
                FUI::WornLedger::Rebaseline("load");
            });
            // ★The costume has to be put on again -- more than once. See
            // Costume::NoteGameLoaded: the engine rebuilds the actor for a
            // while after this message, and every rebuild undoes it.
            FUI::Costume::NoteGameLoaded();
            // ★The equip survived the save; the LOAN did not -- the engine
            // re-read the carrier from the plugin. Re-lend before the player
            // can notice a second ring that stopped working.
            FUI::DualRing::OnLoad();
            // ⓛ probe: the museum index. Deferred like the rest -- the display
            // references have to exist before their state means anything.
            SKSE::GetTaskInterface()->AddTask([]() { FUI::Lotd::Rebuild(); });
            // ★Icon warm-up: hand the icon cache the forms the player is
            // carrying so their pak sprites go resident BEFORE the first
            // open. The cache itself paces the work (grace period + a couple
            // of reads per tick) -- see IconCache::QueueWarm.
            SKSE::GetTaskInterface()->AddTask([]() {
                auto* p = RE::PlayerCharacter::GetSingleton();
                if (!p) return;
                std::vector<RE::FormID> forms;
                for (auto& [obj, pair] : p->GetInventory()) {
                    if (obj && pair.first > 0) forms.push_back(obj->GetFormID());
                }
                FUI::IconCache::GetSingleton()->QueueWarm(std::move(forms));
            });
            break;
        }
    }

    // ---- SKSE cosave: one record loop, dispatched by type ----
    void SaveCallback(SKSE::SerializationInterface* a_intfc)
    {
        FUI::Loadout::SaveGame(a_intfc);
        FUI::Grid::SaveGame(a_intfc);
        FUI::GoldCoins::SaveGame(a_intfc);
        FUI::Costume::SaveGame(a_intfc);
        FUI::DualRing::SaveGame(a_intfc);
        FUI::LootBarter::SaveGame(a_intfc);   // F7: container spot memory (GCLY)
        FUI::Wheeler::SaveGame(a_intfc);      // quick-wheel slot order (GWHL)
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc)
    {
        std::uint32_t type = 0, version = 0, length = 0;
        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type == FUI::Loadout::kRecordType) {
                FUI::Loadout::LoadRecord(a_intfc, version);
            } else if (type == FUI::Grid::kRecordType) {
                FUI::Grid::LoadRecord(a_intfc, version);
            } else if (type == FUI::GoldCoins::kRecordType) {
                FUI::GoldCoins::LoadRecord(a_intfc, version);
            } else if (type == FUI::Costume::kRecordType) {
                FUI::Costume::LoadRecord(a_intfc, version);
            } else if (type == FUI::DualRing::kRecordType) {
                FUI::DualRing::LoadRecord(a_intfc, version);
            } else if (type == FUI::Wheeler::kRecordType) {
                FUI::Wheeler::LoadRecord(a_intfc, version);
            } else if (type == FUI::LootBarter::kContRecordType) {
                FUI::LootBarter::LoadRecord(a_intfc, version);   // F7 (GCLY)
            } else {
                // P2: unknown records are skipped by SKSE automatically, but
                // silently — log them so a future-type/corruption case is
                // diagnosable instead of invisible
                logger::warn("[COSAVE] unknown record type {:08X} v{} ({} bytes) skipped",
                    type, version, length);
            }
        }
        FUI::Grid::RequestRebuild();   // reserved gear + placements just changed
    }

    void RevertCallback(SKSE::SerializationInterface* a_intfc)
    {
        FUI::Loadout::RevertGame(a_intfc);
        FUI::Costume::RevertGame(a_intfc);
        FUI::DualRing::RevertGame(a_intfc);
        FUI::Wheeler::RevertGame(a_intfc);
        FUI::Grid::RevertGame(a_intfc);
        FUI::GoldCoins::RevertGame(a_intfc);
        FUI::LootBarter::RevertGame();   // F7: container spot memory
        FUI::IconCache::GetSingleton()->OnRevert();   // drop work queued
                                                      // against dying forms
    }
}

SKSEPluginInfo(
    .Version              = { 1, 5, 0, 0 },
    .Name                 = "GridInventory",
    .Author               = "Smooth",
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    SKSE::Init(a_skse);
    // ★Say so in the log itself. A diagnostic build is otherwise
    // indistinguishable from the release one, and a report is worth much less
    // when nobody can tell which binary produced it.
    if (FUI::Grid::PoolTrace()) {
        SKSE::log::info("=== DIAGNOSTIC BUILD: pool/take tracing is ON by default ===");
    }

    // no trampoline: every hook here is a vtable swap (write_vfunc)
    UpdateHook::Install();
    PickUpHook::Install();                                        // capacity: world pickup
    HarvestHook<RE::TESFlora>::Install(RE::VTABLE_TESFlora[0]);   // capacity: plants
    HarvestHook<RE::TESObjectTREE>::Install(RE::VTABLE_TESObjectTREE[0]);   // capacity: trees
    SackActivateHook::Install();   // G2: coin sack -> gold, silent to loot HUDs
    // capacity gate at the Activate slot for every direct-pickup form type —
    // pre-TrueHUD, so a blocked pickup can't log a phantom "received"
    // (MISC is covered inside SackActivateHook; books keep the menu flow)
    CapacityActivateHook<RE::TESObjectWEAP>::Install(RE::VTABLE_TESObjectWEAP[0]);
    CapacityActivateHook<RE::TESObjectARMO>::Install(RE::VTABLE_TESObjectARMO[0]);
    CapacityActivateHook<RE::TESAmmo>::Install(RE::VTABLE_TESAmmo[0]);
    CapacityActivateHook<RE::AlchemyItem>::Install(RE::VTABLE_AlchemyItem[0]);
    CapacityActivateHook<RE::IngredientItem>::Install(RE::VTABLE_IngredientItem[0]);
    CapacityActivateHook<RE::TESSoulGem>::Install(RE::VTABLE_TESSoulGem[0]);
    CapacityActivateHook<RE::TESKey>::Install(RE::VTABLE_TESKey[0]);
    CapacityActivateHook<RE::ScrollItem>::Install(RE::VTABLE_ScrollItem[0]);
    CapacityActivateHook<RE::TESObjectLIGH>::Install(RE::VTABLE_TESObjectLIGH[0]);

    // Lifecycle: sender == "SKSE" (the 1.0 path, unchanged). GI10's ABI messages
    // come in on a separate listener so a stray "type 4" from an unrelated
    // plugin can never be mistaken for kPostLoadGame here.
    SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
    FUI::HostApi::Install();

    // L3: loadout tabs + grid layout persist in the SKSE cosave (per-save,
    // load-order safe). The global layout ini stays as a legacy fallback only.
    if (auto* ser = SKSE::GetSerializationInterface()) {
        ser->SetUniqueID('FBIV');
        ser->SetSaveCallback(SaveCallback);
        ser->SetLoadCallback(LoadCallback);
        ser->SetRevertCallback(RevertCallback);
    }
    return true;
}
