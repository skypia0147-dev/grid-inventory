#pragma once

// 1.4 / B4-2 -- the WORN LEDGER.
//
// The board holds only unworn units, and re-reads who is worn from the
// engine's ExtraWorn on every derivation -- which is why half the exclusion
// ladder exists (PLAN_B4_DEMOLITION §2). Before that arithmetic can flip to
// "the board knows", the ledger has to prove it can stay in step with the
// engine on requests and events alone.
//
// ★B4-2b: ENTRIES, not counts. The first observation round ran count-level
// and scored 30/30 -- but the takeOne ladder's worn questions ("has my equip
// landed", "is this doll carry still on the body") turn on WINDOWS where the
// engine flag count and the ledger count are equal while meaning different
// things. Only identity plus a lifecycle can answer those, so the ledger now
// carries what the REQUEST knew (uid / sig / hand -- rule 2: the request is
// the only moment that knows the unit) through the states the takeOne clock
// machinery currently juggles by matching worn lists:
//
//     pending  -- our equip request is out; the engine has not applied it
//     worn     -- the engine confirmed (TESEquipEvent), or wore it unasked
//                 (slot-conflict removal's counterpart, loadout, script)
//
// An unequip event retires one worn entry. (A `doffing` state -- our unequip
// request in flight -- joins in B4-2c, where the doll-carry question needs
// it; nothing consumes the ledger until then.)
//
// One entry = one WORN LIST (a quiver equips as one list however many arrows
// ride in it, and the event fires once) -- the unit both sides agree on.
// Rebaselined wholesale at every load (rule 3). Every menu open and close
// audits entry counts against a fresh ExtraWorn walk AND reports lifecycle
// residue (a pending that never landed), bending to the engine on mismatch
// -- the engine stays the authority until B4-2c flips consumption.
namespace FUI::WornLedger
{
    // Our own equip request, at the moment it is queued -- the one moment
    // that knows the unit (rule 2). a_units: what the action moves (a
    // tileful for ammo); data, not the list count.
    void NotePending(RE::FormID a_form, std::uint16_t a_uid, std::uint16_t a_sig,
                     int a_hand, int a_units);

    // ★CancelPending is RETIRED. It existed for one caller and one shape: the
    // ring CARRIER stood in for a second-slot ring, so the ring's own equip
    // event never came and its pending would sit until the stale sweep. Both
    // rings are engine-worn now and both equip events arrive, so a ring's
    // pending retires the way every other pending does -- and a function that
    // silently drops the oldest pending of a form is not something to leave
    // lying around for the next caller to find and misuse.

    // ★B4-2c: our UNEQUIP is out -- a doll lift starts one the moment the
    // carry begins. The matching worn entry turns `doffing`: still on the
    // body as far as the engine is concerned, already spoken for as far as
    // the board is. The unequip event retires doffing entries first.
    void NoteDoffing(RE::FormID a_form, int a_hand);

    // Any doffing entry of this form still open? THE doll-carry clock: true
    // means the lift's unequip has not landed, so the carried unit is still
    // engine-worn (never in the board's set); false means it landed and the
    // carry must come out of the set like any other unit. Replaces the
    // pendingEquip-scan swap clock, which could only reason about the
    // same-form-swap shape and answered the rest by assumption.
    [[nodiscard]] bool Doffing(RE::FormID a_form);

    // Both arrive already marshalled to the main thread (the sink AddTasks;
    // equip events land on arbitrary threads -- rule 4).
    void OnEquip(RE::FormID a_form);
    void OnUnequip(RE::FormID a_form);

    // Load replaces the inventory wholesale: rebuild from the engine, once.
    void Rebaseline(const char* a_why);

    // Compare worn-entry counts per form against the engine's ExtraWorn walk,
    // report lifecycle residue, and log the verdict. On mismatch the ledger
    // bends to the engine -- each divergence must be counted from a clean
    // baseline or one miss would echo through every later audit.
    void Audit(const char* a_when);
}
