#pragma once

#include <cstdint>

// THE SECOND RING -- WORN FOR REAL.
//
// The engine wears exactly one. BipedAnim is `BIPOBJECT objects[kTotal]` --
// one item per slot -- and kRing is a single bit, so two rings claiming that
// bit means the engine takes the first one off. Moving the slot does not help:
// probed three ways (both masks moved, ARMO only, and claiming NO slot at all)
// the engine took the first ring off every time.
//
// ★So the contest is ENDED rather than worked around: the ring already on the
// body gives up its kRing bit, and the incoming one takes the slot with
// nothing to displace. Both are then engine-worn in every sense the rest of
// this plugin asks about -- ExtraWorn, effects, tooltips, the doll walk, the
// board's worn accounting -- and none of them needs a special case.
//
// ★★THIS REPLACES A CARRIER, and the replacement is why the 1.5.x bug cannot
// come back. That version equipped an invisible costume anchor and copied the
// ring's `formEnchanting` onto it. A PLAYER-ENCHANTED ring keeps its
// enchantment on the INSTANCE (ExtraEnchantment), not on the record, so there
// was nothing to copy: no effects applied at all, and the item showed its
// unenchanted value (user report, 1.5.0 -- "Enchantment disappear after
// equipping", value 4362 -> 500). Wearing the real item cannot have that bug.
// The technique is Jewelry Limiter's (dylbill, Nexus 22098), which reaches the
// same end from Papyrus with RemoveSlotFromMask.
//
// ★★★STILL NOT DRAWN, and now for a smaller and more honest reason than the
// one this file used to give. An ARMO with no biped slot has nowhere for
// BipedAnim to put it, so the ring that gave up its bit is invisible; a ring's
// mesh also carries slot 36 inside the NIF, which the first ring holds.
// The old note called that settled after two routes (inside the equipment
// system, and a hand-loaded model through BSModelDB, which comes back
// `skin=NO`). ★A THIRD ROUTE EXISTS and Left Hand Rings SKSE takes it: deep-copy
// the part the engine ALREADY skinned, rewrite `NiSkinInstance::bones` onto
// another finger, and force the partition on with
// `BSDismemberSkinInstance::UpdateDismemberPartion(slot, true)`. That is a
// piece of work of its own and is not attempted here -- but it is not
// impossible, and this comment should stop saying that it is.
//
// ★★★★NOTHING IS REMEMBERED ACROSS A LOAD, deliberately. The engine re-reads
// ARMO records from the plugin every load -- measured, and the reason the old
// carrier had to re-lend its enchantment there -- so the bits we took are
// already back by the time a save opens. Rather than serialise a debt the load
// has cancelled, the invariant is re-derived from the body: see Tick.
namespace FUI::DualRing
{
    // ---- no rules ---------------------------------------------------------
    // ★★★THERE ARE NO REFUSALS. A ring dropped on either cell goes on.
    //
    // Two used to live here -- "not another unit of a worn form" and "not the
    // same magic effect twice" -- and both are gone for the same reason: each
    // sat on ONE road (the second cell's drop) while the right-hand cell, a
    // click and the wheel let the very same pair through. ★A rule kept on half
    // the roads is worse than no rule: it annoys the player who meets it and
    // does nothing about the player who does not.
    //
    // The effect rule also had a technical reason once, and lost it. Under the
    // carrier ONE stand-in wore a borrowed enchantment, so a duplicate really
    // was a strange state. Both rings are engine-worn now and the game applies
    // both effects perfectly well (measured) -- whether that is desirable is a
    // BALANCE question, and vanilla never had to answer it because vanilla
    // wears one ring.
    //
    // ★★And removing it took a whole class of bugs with it: the effect test was
    // the only reason the drop GATE had to work out which UNIT was coming, and
    // a plain unit carries no uid and no list index to be named by. It named
    // the wrong one -- refusing a plain ring for the enchantment of an
    // enchanted sibling, and only in one left/right order (measured).

    // ---- which ring is which ----------------------------------------------
    // ★★DOES THIS RING HOLD THE ENGINE'S RING SLOT -- is it the one you can
    // SEE on the hand? This is what the doll's two ring cells mean:
    //
    //     ringR = the ring holding kRing (drawn on the hand)
    //     ringL = the one that gave the slot up
    //
    // ★They used to mean nothing physical at all. The doll filled ringR with
    // whichever worn ring the inventory walk reached first -- hash order -- and
    // the slot bit went to the lowest FormID, so "the visible ring" and "the
    // right-hand cell" were independent answers that happened to agree.
    // A drop then aimed at one ring and the engine displaced the other
    // (measured 2026-09-01: the cursor took a ring that was still worn, and
    // the board needed a full rebuild to recover).
    [[nodiscard]] bool HoldsRingSlot(const RE::TESObjectARMO* a_armo);

    // ★★★...BUT THE CELL IS NOT THE BIT, and tying them together was wrong in
    // the other direction. The bit moves for mechanical reasons -- the equip
    // takes it off whatever stays so the incoming ring can join -- so the
    // NEWEST ring always ended up holding it, and "cell = holder" then put
    // every new ring on the RIGHT and slid the old one LEFT. A ring dropped on
    // the left cell appeared on the right, and the pair looked like it had
    // swapped places for no reason the player did (user report).
    // ★So the cell is remembered as PLACEMENT -- where the player put it --
    // and the bit is left free to do its job underneath. They answer different
    // questions and neither has to follow the other.
    // ★★Keyed by form AND content signature, so a plain ring and an enchanted
    // one of the same kind keep their own cells. Two units that match on both
    // are interchangeable to every reader here, and either may take the seat.
    // ★★★WRITING it is private -- PrepareForEquip records the placement as
    // part of the one act, and that is the only moment anything knows where
    // the player put a ring. The setter was public while a caller still had
    // to sequence the three functions by hand; leaving it public now would
    // invite exactly the protocol that consolidation removed.
    // Does this ring unit belong in the LEFT cell? False also means "no
    // opinion" -- the doll then falls back to the slot bit, which is right for
    // a pair the player never placed by hand.
    [[nodiscard]] bool IsSecondCell(const RE::TESObjectARMO* a_armo, std::uint16_t a_sig);
    // Has the player placed ANYTHING in the left cell? ★The doll needs this
    // separately: its walk is in inventory order, so an unplaced ring can
    // reach the cell first and take the seat the placed one was promised.
    [[nodiscard]] bool HasSecondCell();

    // ---- the one act -------------------------------------------------------
    // ★★★A RING IS ABOUT TO BE EQUIPPED. LEAVE THE BODY READY FOR IT.
    //
    //   a_incoming   the ring going on
    //   a_sig        its content signature (names the UNIT)
    //   a_aimed      the UNIT the player pointed at -- the cell's occupant, as
    //                the worn list that names it. null for a click or the
    //                wheel, which point at nothing
    //   a_secondCell the drop named the LEFT cell
    //
    // Takes off whatever must leave, hands the slot bit round so the engine
    // single-ends nothing, and records the placement. ★The caller must SKIP
    // its conflict pass for rings afterwards -- everything is already arranged,
    // and that pass would undo it.
    //
    // ★★★★a_aimed IS A UNIT AND WAS A FORM, and the difference is a bug the
    // player could see. The cell's occupant arrived here as `occ->As<ARMO>()`
    // -- so with two units of ONE form on the body, "the ring in this cell"
    // matched whichever came first in INVENTORY order. The wrong one came off,
    // the board handed the cursor the one it thought it had displaced, and
    // that ring was still on the finger: a phantom the player could only drop
    // into nothing, while the other ring went quietly into the pack (user
    // report). ★Two units alike in uid AND signature stay interchangeable --
    // removing either is the same act to every reader here, including the
    // player -- so naming the unit by pool is exact where exactness exists and
    // arbitrary only where the difference does not exist either.
    //
    // ★★THIS WAS THREE FUNCTIONS AND A PROTOCOL. MakeRoom / TakeOneOff /
    // NoteSecondCell had to be called in the right order, by every road a ring
    // can arrive on, and the caller had to know which road it was on. The order
    // was got wrong four times in one day -- a click path that skipped the cap,
    // a drop path that skipped it too, a missing second MakeRoom, and a swap
    // that removed a ring the caller was also removing. A caller that must know
    // a sequence is a caller that will get the sequence wrong.
    //
    // ★★★And the engine cannot be asked to help. Its conflict pass removes
    // every worn ring whose mask OVERLAPS the incoming one's, the slot bit is a
    // FORM fact rather than a unit one, and so the pass can be aimed at "all"
    // or "none" but never at "that one". Three attempts to steer it produced
    // three separate failures -- a phantom ring on the cursor, a pair both
    // coming off, a third ring going on (all measured 2026-09-01). The removal
    // is ours, explicitly, and the pass is kept away from rings entirely.
    void PrepareForEquip(RE::TESObjectARMO* a_incoming, std::uint16_t a_sig,
                         RE::ExtraDataList* a_aimed, bool a_secondCell);

    // ---- taking one off ----------------------------------------------------
    // ★★★TAKE ONE RING OFF AND LEAVE THE OTHERS' MAGIC ALONE.
    //
    // ★THE ENGINE DISPELS BY ENCHANTMENT, NOT BY UNIT. An ActiveEffect
    // remembers `spell` (the EnchantmentItem) and `source` (the item FORM), and
    // neither of those names a unit -- so two identical enchanted rings raise
    // two effects that are indistinguishable, and MagicTarget::DispelEffect
    // takes a spell and a caster. Unequipping ONE of them therefore dispels
    // BOTH: the ring still on the finger keeps its tooltip, its value and its
    // place on the doll, and quietly stops doing anything (user report).
    // Vanilla never had to answer this, because vanilla wears one ring.
    //
    // ★★There is no counterpart to Actor::DispelWornItemEnchantments to undo it
    // with -- the engine can clear worn enchantments but has no "apply them
    // again" -- so the survivor is simply PUT BACK ON. The equip is what
    // establishes the effect in the first place, and re-establishing it is all
    // this does.
    //
    // ★★★Deliberately NOT the sweep's job, which is where every other repair in
    // this file lives. The sweep OBSERVES the body, and "this ring's effect is
    // missing" is not a fact about the body: an effect can be legitimately
    // absent (dispelled, resisted, its conditions false), and a sweep reading
    // that as damage would re-equip rings nobody touched, forever. The
    // collateral has one cause and it is known exactly -- so it is repaired
    // where it is caused.
    //
    // ★★★★Every road a ring LEAVES on must come through here, for the same
    // reason every road it arrives on comes through PrepareForEquip: a rule
    // kept on half the roads is worse than no rule.
    void RemoveWornUnit(RE::TESObjectARMO* a_armo, RE::ExtraDataList* a_xl);

    // ---- lifecycle --------------------------------------------------------
    // ★Per game-update tick, and it OBSERVES rather than remembers: the world
    // changes behind this system's back (a ring sold, dropped, taken by a
    // script, or a save loaded with two already on). One invariant, restored
    // from what the body is actually wearing --
    //
    //     IF ANY RING IS WORN, EXACTLY ONE OF THEM HOLDS kRing.
    //
    // -- which covers every direction at once: a bit is handed back when its
    // ring leaves, a pair a load put back into contest is separated again,
    // and a survivor left slotless by the removal of the visible ring gets
    // the slot back instead of turning invisible for the rest of the save.
    // ★Not a per-frame walk: it sleeps until something asks, and then on a
    // slow heartbeat only while a bit is out.
    void Tick();
    // Hand every bit back, then ask for a fresh sweep. New game / pre-load.
    void RevertGame();
}
