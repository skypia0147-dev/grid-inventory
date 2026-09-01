#include "game/DualRing.h"

#include "game/Costume.h"
#include "ui/Grid.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace FUI::DualRing
{
    namespace
    {
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

        // ★The forms we took kRing from, and owe it back to. Session state
        // only: a load restores every record from the plugin, so a debt
        // written into the cosave would already be paid by the time it was
        // read. Tick re-derives instead (see the header).
        std::vector<RE::FormID> g_stripped;

        // ★Where the player PUT the second ring -- the left cell's tenant, by
        // form. Placement, not physics: see NoteSecondCell in the header for
        // why this cannot be read off the slot bit. 0 = nobody placed one,
        // and the doll falls back to the bit.
        RE::FormID    g_leftRing = 0;
        std::uint16_t g_leftSig  = 0;

        // The sweep walks the inventory, so it is not a per-frame job. It runs
        // when something asked it to and, while any bit is out, on a slow
        // heartbeat -- the world can change with no equip event we see.
        bool          g_dirty = false;
        std::uint64_t g_frame = 0;
        std::uint64_t g_nextSweep = 0;
        constexpr std::uint64_t kSweepGap = 30;   // ~0.5s at 60fps
        // A frame is 16.7ms at 60fps. Anything at this scale is a
        // visible hitch and wants to be in the log by name.
        constexpr double        kSweepWarnMs = 2.0;

        [[nodiscard]] const char* NameOf(RE::TESForm* a_f)
        {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return (n && *n) ? n : "<unnamed>";
        }

        [[nodiscard]] bool HoldsRingBit(const RE::TESObjectARMO* a_armo)
        {
            return a_armo &&
                   (static_cast<std::uint32_t>(a_armo->GetSlotMask().get()) &
                    static_cast<std::uint32_t>(Slot::kRing)) != 0;
        }

        // ★★★MAY THIS RING GIVE UP ITS BIT AND STILL BE A RING?
        //
        // Grid::IsRing answers on the kRing bit OR the ClothingRing keyword.
        // Take the bit from a ring that has ONLY the bit and it stops being a
        // ring to every reader in this plugin at once: the doll files it as
        // odd armour, the board's rules change under it, and -- the part that
        // does not heal -- WornRings can no longer see it, so the sweep never
        // finds it to hand the bit BACK. The ring would be stranded slotless
        // for the rest of the save.
        // ★Vanilla rings all carry the keyword, so this refuses nothing an
        // ordinary game can produce; it is the mod ring parked on a custom
        // slot with no keyword that must be left alone.
        [[nodiscard]] bool MayGiveUpRingBit(const RE::TESObjectARMO* a_armo)
        {
            constexpr RE::FormID kClothingRing = 0x0010CD09;   // Skyrim.esm
            return a_armo && a_armo->HasKeywordID(kClothingRing);
        }

        void TakeRingBit(RE::TESObjectARMO* a_armo)
        {
            if (!a_armo) return;
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask().get());
            a_armo->bipedModelData.bipedObjectSlots =
                static_cast<Slot>(mask & ~static_cast<std::uint32_t>(Slot::kRing));
            if (std::find(g_stripped.begin(), g_stripped.end(), a_armo->GetFormID()) ==
                g_stripped.end()) {
                g_stripped.push_back(a_armo->GetFormID());
            }
            SKSE::log::info("[DUALRING] '{}' gave up its ring slot", NameOf(a_armo));
        }

        // ★Only ever to a form WE took it from. A modded ring can be authored
        // with no kRing bit at all -- it sits on a custom slot and answers for
        // itself through the ClothingRing keyword -- and handing that one a bit
        // it never had would move it onto the finger its author kept clear.
        void GiveRingBitBack(RE::FormID a_id)
        {
            auto* f = RE::TESForm::LookupByID(a_id);
            auto* armo = f ? f->As<RE::TESObjectARMO>() : nullptr;
            if (!armo) return;
            const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask().get());
            armo->bipedModelData.bipedObjectSlots =
                static_cast<Slot>(mask | static_cast<std::uint32_t>(Slot::kRing));
            SKSE::log::info("[DUALRING] '{}' has its ring slot back", NameOf(armo));
        }

        // One worn ring unit: the form and the list that names THIS unit.
        struct WornRing
        {
            RE::TESObjectARMO*  armo = nullptr;
            RE::ExtraDataList*  xl   = nullptr;
        };

        // ★Every ring the body is wearing, in FormID order so the choice of
        // which one keeps kRing is the same answer twice running. An arbitrary
        // order would move the visible ring from one finger to the other on
        // every load for no reason the player could see.
        [[nodiscard]] std::vector<WornRing> WornRings(RE::PlayerCharacter* a_p)
        {
            std::vector<WornRing> out;
            if (!a_p) return out;
            for (const auto& [obj, data] : a_p->GetInventory(
                     [](RE::TESBoundObject& o) { return o.IsArmor(); })) {
                if (data.first <= 0 || !data.second) continue;
                auto* armo = obj->As<RE::TESObjectARMO>();
                if (!armo || Costume::IsAnchor(armo) || !Grid::IsRing(armo)) continue;
                if (!data.second->extraLists) continue;
                for (auto* xl : *data.second->extraLists) {
                    if (!xl || !(xl->HasType<RE::ExtraWorn>() ||
                                 xl->HasType<RE::ExtraWornLeft>())) {
                        continue;
                    }
                    out.push_back({ armo, xl });
                }
            }
            std::sort(out.begin(), out.end(), [](const WornRing& a, const WornRing& b) {
                return a.armo->GetFormID() < b.armo->GetFormID();
            });
            return out;
        }

        // ★★★ONE WALK A FRAME, SHARED -- the same bargain the doll already
        // makes. GetInventory DEEP-COPIES every matching entry, which is why
        // CollectEquipment was rebuilt around a single shared walk; asking it
        // again per question quietly undid that. A single ring swap asked four
        // times (the drop gate, MakeRoom, the cap, AimAt) plus the sweep's own,
        // all in one frame, over every piece of armour the player owns.
        // ★Only MEMBERSHIP is cached. Slot bits are read live off the ARMO, so
        // a mask this pass changes is visible to the next reader immediately --
        // which is exactly what MakeRoom and AimAt rely on.
        std::vector<WornRing> g_wornCache;
        std::uint64_t         g_wornFrame = ~0ull;

        [[nodiscard]] const std::vector<WornRing>& WornRingsCached(RE::PlayerCharacter* a_p)
        {
            if (g_wornFrame != g_frame) {
                g_wornCache = WornRings(a_p);
                g_wornFrame = g_frame;
            }
            return g_wornCache;
        }

        // Membership changed under us -- the next reader must walk again.
        void ForgetWorn() { g_wornFrame = ~0ull; }

        // ★★THE INVARIANT, restored from the body:
        //
        //     IF ANY RING IS WORN, EXACTLY ONE OF THEM HOLDS kRing.
        //
        // ★"At most one" is what this pass enforced first, and it was half a
        // rule. Take the visible ring off a pair and the survivor is left with
        // no bit at all -- one ring on the body, wearing no slot, INVISIBLE
        // for the rest of the save with nothing to notice it. Both directions
        // have to be walked, and then the whole bookkeeping falls out of one
        // question asked of the body: no state to keep in step, and every
        // stale case repairs itself on the next sweep.
        // Returns false when the body could not be read -- the caller stays
        // armed and asks again rather than dropping the request on the floor.
        bool Enforce()
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p || !p->Is3DLoaded()) return false;

            const auto& worn = WornRingsCached(p);

            // ★★★COUNTED OVER FORMS, NOT UNITS. The slot bit lives on the ARMO,
            // so two plain rings of one form share ONE bit. Counting units made
            // this pass see two holders where there was one, strip the shared
            // form, and then see NO holder on the next sweep and hand it
            // straight back -- a give/take oscillation twice a second, which
            // the player sees as a ring blinking in and out. The invariant is
            // about forms because the thing it governs is.
            std::vector<RE::TESObjectARMO*> forms;
            for (const auto& w : worn) {
                if (std::find(forms.begin(), forms.end(), w.armo) == forms.end()) {
                    forms.push_back(w.armo);
                }
            }

            // ★The left cell's tenant is forgotten the moment it comes off, so
            // the next ring to land there is not shown in a cell it never
            // took. Same discipline as the bits: observed, not remembered.
            if (g_leftRing != 0 &&
                std::none_of(forms.begin(), forms.end(),
                    [](const RE::TESObjectARMO* a) { return a->GetFormID() == g_leftRing; })) {
                g_leftRing = 0;
                g_leftSig  = 0;
            }

            // 1. Hand back every bit whose form has left the body entirely.
            for (auto it = g_stripped.begin(); it != g_stripped.end();) {
                const bool stillWorn = std::any_of(forms.begin(), forms.end(),
                    [&](const RE::TESObjectARMO* a) { return a->GetFormID() == *it; });
                if (stillWorn) { ++it; continue; }
                GiveRingBitBack(*it);
                it = g_stripped.erase(it);
            }
            if (forms.empty()) return true;

            const auto holders = std::count_if(forms.begin(), forms.end(),
                [](const RE::TESObjectARMO* a) { return HoldsRingBit(a); });

            // 2. Nobody holds it: the ring that did has come off. Give the bit
            // back to a survivor -- ★one WE took it from, never a ring authored
            // without it, which would move it onto a finger its author kept
            // clear.
            if (holders == 0) {
                for (auto* armo : forms) {
                    // ★★NEVER TO A FORM WEARING TWO UNITS. They share the one
                    // bit, so handing it over arms the engine against BOTH --
                    // the next equip single-ends and takes a ring the player
                    // never asked to remove. A doubled form stays slotless,
                    // which is the only state it can hold honestly.
                    const auto units = std::count_if(worn.begin(), worn.end(),
                        [&](const WornRing& w) { return w.armo == armo; });
                    if (units > 1) continue;
                    const auto id = armo->GetFormID();
                    const auto it = std::find(g_stripped.begin(), g_stripped.end(), id);
                    if (it == g_stripped.end()) continue;
                    GiveRingBitBack(id);
                    g_stripped.erase(it);
                    return true;
                }
                return true;
            }

            // 3. More than one holds it -- a load put the pair back into
            // contest. The FIRST keeps the slot; the rest give it up.
            if (holders <= 1) return true;
            bool kept = false;
            for (auto* armo : forms) {
                if (!HoldsRingBit(armo)) continue;
                if (!kept) { kept = true; continue; }
                // ★A ring that may not give its bit up KEEPS it, and then two
                // hold kRing after all. That is still the better answer: the
                // alternative strands it (see MayGiveUpRingBit), and the engine
                // only re-reads the contest at the next equip.
                if (MayGiveUpRingBit(armo)) TakeRingBit(armo);
            }
            return true;
        }
    }

    bool HoldsRingSlot(const RE::TESObjectARMO* a_armo)
    {
        return HoldsRingBit(a_armo);
    }

    void NoteSecondCell(const RE::TESObjectARMO* a_ring, std::uint16_t a_sig)
    {
        g_leftRing = a_ring ? a_ring->GetFormID() : 0;
        g_leftSig  = a_ring ? a_sig : 0;
    }

    bool IsSecondCell(const RE::TESObjectARMO* a_armo, std::uint16_t a_sig)
    {
        return g_leftRing != 0 && a_armo &&
               a_armo->GetFormID() == g_leftRing && a_sig == g_leftSig;
    }

    bool HasSecondCell() { return g_leftRing != 0; }

    bool MakeRoom()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return false;
        const auto& worn = WornRingsCached(p);
        if (worn.empty()) return false;
        // ★★NOT WHEN BOTH SLOTS ARE ALREADY FULL. Room is made by taking the
        // bit off the ring that has it; do that with a pair on the body and
        // NOTHING holds kRing, so the incoming equip displaces nothing and the
        // player ends up wearing three. With a pair the right answer is the
        // engine's own: leave the bit alone and let the ordinary conflict pass
        // trade the visible ring for the new one.
        if (worn.size() >= 2) return false;
        bool made = false;
        for (const auto& w : worn) {
            if (!HoldsRingBit(w.armo) || !MayGiveUpRingBit(w.armo)) continue;
            TakeRingBit(w.armo);
            made = true;
        }
        if (!made) return false;   // nothing could give a bit up -- ordinary swap
        g_dirty = true;   // the sweep gives the bits back when the rings leave
        return true;
    }

    void PrepareForEquip(RE::TESObjectARMO* a_incoming, std::uint16_t a_sig,
                         RE::TESObjectARMO* a_aimed, bool a_secondCell)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !a_incoming) return;

        // 1. WHO MUST LEAVE. Only ever past the cap, and the ring the player
        //    pointed at goes first -- a drop on a cell names its own victim.
        //    Otherwise the visible one, which is what an ordinary equip has
        //    always meant.
        std::vector<WornRing> victims;
        {
            const auto& worn = WornRingsCached(p);
            int over = static_cast<int>(worn.size()) - 1;   // room for one more
            if (over > 0 && a_aimed) {
                for (const auto& w : worn) {
                    if (w.armo != a_aimed || over <= 0) continue;
                    victims.push_back(w);
                    --over;
                }
            }
            for (const auto& w : worn) {
                if (over <= 0) break;
                if (std::any_of(victims.begin(), victims.end(),
                        [&](const WornRing& v) { return v.xl == w.xl; })) {
                    continue;
                }
                if (!HoldsRingBit(w.armo)) continue;   // prefer the visible one
                victims.push_back(w);
                --over;
            }
            for (const auto& w : worn) {
                if (over <= 0) break;
                if (std::any_of(victims.begin(), victims.end(),
                        [&](const WornRing& v) { return v.xl == w.xl; })) {
                    continue;
                }
                victims.push_back(w);
                --over;
            }
        }

        // 2. TAKE THEM OFF OURSELVES. ★Never the engine's conflict pass: it
        //    removes every worn ring whose mask OVERLAPS the incoming one's,
        //    and the slot bit is a FORM fact -- so it cannot be steered to one
        //    ring, and three attempts to steer it produced three different
        //    failures (a phantom on the cursor, a pair both coming off, a
        //    third ring going on). The caller skips that pass for rings.
        if (auto* em = RE::ActorEquipManager::GetSingleton()) {
            for (const auto& v : victims) {
                em->UnequipObject(p, v.armo, v.xl, 1, nullptr,
                                  false, false, false, true);
                SKSE::log::info("[DUALRING] '{}' comes off to make room",
                                NameOf(v.armo));
            }
        }
        if (!victims.empty()) ForgetWorn();

        // 3. WHOEVER STAYS GIVES UP THE SLOT, so the engine has nothing to
        //    single-end when the incoming ring goes on.
        bool sharesWithSurvivor = false;
        for (const auto& w : WornRingsCached(p)) {
            if (w.armo == a_incoming) sharesWithSurvivor = true;
            if (!HoldsRingBit(w.armo) || !MayGiveUpRingBit(w.armo)) continue;
            TakeRingBit(w.armo);
        }

        // 4. ...and the incoming ring takes it, so something is drawn. ★Not
        //    when a survivor shares its FORM: one ARMO, one bit, and handing
        //    it over would arm the engine against the very ring we just kept.
        if (!sharesWithSurvivor) {
            const auto id = a_incoming->GetFormID();
            const auto it = std::find(g_stripped.begin(), g_stripped.end(), id);
            if (it != g_stripped.end()) {
                GiveRingBitBack(id);
                g_stripped.erase(it);
            }
        }

        // 5. Where the player put it. Placement, not physics -- see the header.
        if (a_secondCell) NoteSecondCell(a_incoming, a_sig);
        else if (IsSecondCell(a_incoming, a_sig)) NoteSecondCell(nullptr, 0);

        g_dirty = true;
    }

    void Tick()
    {
        ++g_frame;
        // ★Cheap when there is nothing to do, which is nearly always: no bit
        // is out and nobody asked. The heartbeat only runs while we owe one.
        if (!g_dirty && g_stripped.empty()) return;
        if (!g_dirty && g_frame < g_nextSweep) return;
        g_nextSweep = g_frame + kSweepGap;
        // ★★MEASURED, NOT ASSUMED. A reporter saw the game stall for a few
        // seconds and asked whether this was us -- and the log could not
        // answer, because it records events and not the time between them.
        // Idle looks exactly like frozen from outside. So the sweep times
        // itself, and anything worth noticing says so with its own numbers.
        // Costs one clock read on the frames that sweep at all.
        const auto t0 = std::chrono::steady_clock::now();
        // ★Cleared only when the sweep actually READ the body. A load reaches
        // here before the actor has 3D, and a request dropped there would
        // leave a loaded pair both holding kRing -- which the next equip's
        // conflict pass would answer by taking BOTH rings off.
        const bool ran = Enforce();
        if (ran) g_dirty = false;
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
        if (ms > kSweepWarnMs) {
            SKSE::log::warn("[DUALRING] sweep took {:.1f}ms ({} worn ring(s), "
                            "{} slot(s) out) -- this is us", ms,
                            g_wornCache.size(), g_stripped.size());
        }
    }

    void RevertGame()
    {
        // ★The records are about to be re-read anyway; this is for the case
        // where they are not -- a new game in the same session. Cheap either
        // way, and it leaves no form of ours edited behind a save boundary.
        for (const auto id : g_stripped) GiveRingBitBack(id);
        g_stripped.clear();
        g_leftRing = 0;
        // ★★ARMED, not cleared -- this runs before every load, and the save
        // about to open may have TWO rings on. The engine hands back both slot
        // bits with the records, so the pair arrives in contest with nothing
        // owed and nothing to notice it: the first sweep after the body exists
        // is what separates them again. Costs one walk on a new game.
        g_dirty = true;
    }
}
