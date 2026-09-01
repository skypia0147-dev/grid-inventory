#pragma once

#include <cstdint>

namespace SKSE
{
    class SerializationInterface;
}

// COSTUME — appearance-only outfits (PLAN_COSTUME.md).
//
// Stats keep coming from what is really equipped; the body shows the set held
// by whichever loadout tab is CHECKED. Nothing is equipped or unequipped for
// this: the equipped armour keeps its rating, keywords, perks, enchantment and
// tempering because it never stops being the equipped armour. Only its
// appearance list is borrowed for the length of one rebuild.
//
//   1. back up each worn armour's TESObjectARMO::armorAddons
//   2. overwrite them with the costume piece's addon for that slot
//      (or the SKIN's addon when the costume leaves the slot empty)
//   3. Actor::DoReset3D  -- synchronous; the engine builds the 3D from the
//      list it can see right now
//   4. put every list back
//
// ★Step 4 is not optional and not deferrable. armorAddons lives on the shared
// form: every actor wearing that armour reads the same array, so the window in
// which it is wrong has to close inside the same call. It does, because
// DoReset3D is synchronous (measured).
//
// A costume piece's model reaches the body by borrowing the equipped item's
// addon list, so a slot with nothing equipped has no list to borrow. Empty slots
// are filled by ANCHORS (below) rather than left bare.

namespace FUI::Costume
{
    // ---- which tab supplies the appearance -----------------------------
    // -1 = no costume (normal appearance). Only one tab at a time; setting a
    // new one clears the old.
    [[nodiscard]] int  Tab();
    [[nodiscard]] bool IsTab(int a_index);
    void               SetTab(int a_index);   // -1 clears

    // False for the ACTIVE loadout tab: dressing as the set you are already
    // wearing changes nothing, so the UI greys its checkbox instead of offering
    // a control that does nothing.
    [[nodiscard]] bool CanBeTab(int a_index);

    // The loadout system calls this when a tab is deleted, so a costume that
    // pointed at it clears instead of dangling.
    void OnTabRemoved(int a_index);

    // ---- what the costume covers ---------------------------------------
    // Weapons, SHIELD and QUIVER are excluded by design. The equipment doll
    // greys those slots and shows the really-equipped item there, so ask here
    // rather than repeating the slot list at the call site.
    [[nodiscard]] bool CoversSlot(RE::BGSBipedObjectForm::BipedObjectSlot a_slot);
    [[nodiscard]] bool CoversArmor(RE::TESObjectARMO* a_armor);

    // ---- the anchors -----------------------------------------------------
    // A costume piece reaches the body by borrowing a WORN item's addon list,
    // so a slot the player left empty has no list and stays bare. An anchor
    // fills that gap: a placeholder ARMO (Grid Inventory.esp 0x82B..0x84A) with
    // 0 rating, 0 weight, 0 value, NO keywords and NO model, equipped into the
    // empty slot purely so there is a list to borrow.
    //
    // ★Eight carriers, one shared addon. Several slots can be bare at once
    // (helmet AND amulet AND ring AND cape) and one carrier cannot serve them:
    // armorAddons is overwritten per-ARMO, so two pieces sharing a carrier would
    // have to share an appearance. The ARMA is shared freely -- it is never
    // rendered, only replaced. Anchors are assigned per costume PIECE, not per
    // slot, so a Hair+Circlet helmet takes one and not two.
    //
    // ★Keyword-free is the whole safety argument. Every vanilla condition that
    // asks "is this actor wearing armour" goes through WornHasKeyword against
    // ArmorHeavy / ArmorLight / ArmorClothing (verified by scanning every CTDA
    // in Skyrim.esm) -- carrying none of them makes the anchor invisible to
    // unarmoured builds, Mage Armor perks and the like.
    //
    // ★It is not the player's property. Hide it everywhere the player's own
    // items are shown: grid, doll, capacity, tooltips, transfers.
    [[nodiscard]] bool IsAnchor(const RE::TESForm* a_form);

    // ---- applying -------------------------------------------------------
    // Rebuild the player's appearance from the current state. Cheap to call
    // when nothing changed (it compares against what it last applied); the
    // rebuild itself is not, which is why callers mark instead of applying.
    void Apply();

    // Something that can change the answer happened -- gear equipped or
    // removed, a tab switched, the costume tab re-checked, a cell loaded, a
    // transformation ended. Coalesced into one Apply on the next tick.
    void MarkDirty();
    void Tick();   // game thread; performs a pending Apply

    // ★A save has just finished loading. The costume is re-applied several
    // times over the next few seconds rather than once: the engine keeps
    // rebuilding the actor after a load, each rebuild reads the addon lists we
    // have already put back, and the result is the plain body -- with the
    // anchors still worn, which is a bald head. There is no event for "the
    // engine is done", and the obvious probe does not work either (it reuses
    // the biped pointer, so a rebuild cannot be seen from outside), so the
    // window is covered instead of watched.
    void NoteGameLoaded();

    // ★★1.6.0 MIGRATION: take off the retired second-ring carrier.
    //
    // Anchor 32 (0x84A) was the second ring's stand-in until 1.6.0 -- it wore
    // the ring's enchantment while the ring itself stayed in the pack. The
    // feature is gone, but THE EQUIP SURVIVES IN OLD SAVES, and nothing owns
    // it any more. Left alone it is worse than untidy: the carrier is a clone
    // of the anchors and its record carries a circlet's whole wardrobe (BOD2
    // on the HAIR and circlet slots, a circlet ARMA on its armature). The
    // strip and the slot rewrite that made it harmless lived in the retired
    // code and ran on every load, because THE ENGINE RE-READS THE RECORD FROM
    // THE PLUGIN each time -- so from the first load after the update the
    // carrier is worn wearing its authored circlet again: a bald head under an
    // invisible helmet. And IsAnchor hides it from the grid, the doll and
    // every transfer, so the player cannot take it off.
    //
    // Runs once per load, from a task (it equips, so not inside a render
    // pass). A save that never had a carrier pays one inventory walk.
    // ★The form mutations themselves need no undoing: they were never
    // serialised -- the same plugin re-read that puts the circlet back is what
    // reverts them.
    void SweepRetiredCarrier();

    // ---- persistence ----------------------------------------------------
    // Which tab is checked is per-save state. Its own record rather than a
    // field on the loadout one: the two systems version independently, and a
    // costume that failed to load must not take the tabs down with it.
    inline constexpr std::uint32_t kRecordType = 'GCST';
    inline constexpr std::uint32_t kVersion = 1;

    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame(SKSE::SerializationInterface* a_intfc);
}
