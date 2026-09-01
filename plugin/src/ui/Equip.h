#pragma once

#include "ui/Grid.h"

#include <string>

namespace FUI::Equip
{
    // B-5 (PLAN_B §2-D): equipment doll. Slot strip (circlet + 4 accessories)
    // over a 3x4 doll; worn items render their cached icons; right-click
    // unequips; the carried item can be dropped on a slot to equip (C6/D4).

    // Equip an item with the vanilla type gate (WEAP/ARMO/AMMO/LIGHT/ALCH/
    // SCRL). a_slotId == "shieldL" routes one-handers/staves to the LEFT hand
    // (BGSEquipSlot 0x13F43). Returns true when the gate accepted the item.
    // GI1/D4: a_uid/a_xlIdx name WHICH copy to equip. Every ActorEquipManager
    // call used to pass a null ExtraDataList, which lets the ENGINE choose the
    // sub-stack -- so equipping the enchanted one of three identical swords was
    // a coin flip. nullptr/0/-1 keeps the old "engine picks" behaviour for
    // callers that genuinely have no instance in hand.
    // a_tileCount: what the tile being acted on holds. Only AMMO uses it (see
    // EquipCountFor); every other form is equipped one at a time whatever the
    // tile holds.
    bool EquipItem(RE::TESBoundObject* a_obj, const std::string& a_slotId,
                   std::uint16_t a_uid, int a_xlIdx, std::uint16_t a_sig,
                   const std::string& a_srcKey = {}, int a_tileCount = 1);

    // ★★A quiver is not equipped one arrow at a time. Vanilla puts the whole
    // lot on your back, and here the TILE owns the count (G4) -- so equipping a
    // tile equips what that tile holds, and 200 arrows across two tiles means
    // clicking one leaves the other in the pack. Taking a single arrow off a
    // stack was the old behaviour and it was wrong twice over: it did not match
    // vanilla, and the leftover split lists are what made repeated clicks eat
    // the stack.
    // Everything else stays at one: a tile of ten potions is ten drinks.
    [[nodiscard]] int EquipCountFor(RE::TESBoundObject* a_obj, int a_tileCount);

    // ★★★TWO AMMO RULES USED TO LIVE HERE AND BOTH ARE GONE (1.6.1).
    //
    //   AmmoMergeRoom      how much room is left on the back
    //   NormaliseWornAmmo  take the over-cap surplus back off, every delta
    //
    // They existed to keep the engine's worn count at a number the board could
    // draw. Nothing reads that number now: the doll shows min(total, cap) and
    // the board shows the rest, so forty worn and two hundred and forty worn
    // are the same picture. The first was deciding something invisible; the
    // second was a tug of war the engine undid on every following delta.
    //
    // What went with them: an unequip of the whole quiver on every "replace"
    // click, and a 3D model reset on every arrow retrieved at the cap.
    // Full story and measurements: PLAN_AMMO_TOTALS.md.

    // ★★USING is not WEARING, and only the first has a type gate that makes
    // sense. EquipItem's whitelist answers "will a doll slot take this?" —
    // asking it "does clicking this do anything?" made us answer ON BEHALF OF
    // THE ENGINE, and we are not entitled to: a mod can build a click-me item
    // out of whatever record type suits it (AddItemMenu's cube), its script
    // reacts to the equip, and a whitelist can never know about it. The
    // vanilla inventory does not pre-judge either — it hands the item over and
    // lets the engine decide. So does this.
    // Same queue, same tick, no slot: the engine picks what the click means.
    // ★★THE QUIVER, WITHOUT ASKING A VTABLE. Actor::GetCurrentAmmo is a
    // virtual and the two CommonLib lines disagree about its slot -- 0x9E in
    // the tree we used to build against, 0x9F in the one that reads 1.7.99's
    // address library. An off-by-one there does not fail; it calls the
    // neighbour and answers with whatever that returns, which is how equipped
    // arrows silently vanished from the doll. Equipped ammo is the ammo the
    // engine has marked worn, and that needs no vtable.
    [[nodiscard]] RE::TESAmmo* EquippedAmmo(RE::Actor* a_actor);

    bool UseItem(RE::TESBoundObject* a_obj, std::uint16_t a_uid, int a_xlIdx,
                 std::uint16_t a_sig, const std::string& a_srcKey = {},
                 int a_tileCount = 1);

    // Take a worn item off. ★Same queue as everything else, so it is safe from
    // the render pass; the doll has always had this on right-click, but only
    // through its own slot widget. Surfaces without a doll -- the quick wheel --
    // need it as a call.
    // a_hand: 0 none, 1 right, 2 left. It matters when both hands wear
    // identical units; 0 lets the engine pick, which is right for armour.
    bool UnequipItem(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                     std::uint16_t a_sig, int a_hand = 0, int a_count = 1);

    // ★Ring session: a cancelled carry returns to the slot it was lifted
    // from (origin rule). Queues through the same pending pipeline as every
    // equip -- conflict pass and srcList resolve both apply. a_hand==2 sends a
    // one-hander back to the left hand.
    void RequestWear(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                     std::uint16_t a_sig, int a_hand, int a_count);

    // Does using this take the unit OFF the board — worn, drunk, eaten, learnt?
    // The board bookkeeping (vacate the cell, forget the tile, hint the drain)
    // must run for those and NOT for a scripted item that is only being poked:
    // forgetting the cell of something that never left makes it jump to the
    // first free slot on every click.
    [[nodiscard]] bool IsWearOrConsume(RE::TESBoundObject* a_obj);

    // The object / sub-stack worn in a doll slot ("weapon", "shieldL", "head"...).
    // Needed by anything that must act on the WORN copy rather than the form.
    [[nodiscard]] RE::TESBoundObject* WornObjectAt(const std::string& a_slotId);
    [[nodiscard]] RE::ExtraDataList*  WornExtraAt(const std::string& a_slotId);
    // ★How many units the slot wears. NOT derivable from WornExtraAt: ammo can
    // be worn with no ExtraDataList at all, and reading GetCount() off a null
    // list quietly answered "1" for a hundred-arrow quiver. The slot already
    // knows the number (CollectEquipment counts it) — ask the slot.
    [[nodiscard]] int WornCountAt(const std::string& a_slotId);

    // Equip/unequip requests queue here and execute in the game-update hook
    // (UIRoot::Tick) — calling ActorEquipManager inside the UI RENDER pass
    // defers the 3D refresh until the menu closes (paused game).
    void ProcessPending();

    // Draw the doll panel at the current cursor pos (inside the main window).
    void Draw();

    // L2: loadout buy/delete confirm windows — call at TOP LEVEL (like the
    // settings window), NOT inside the equip panel child.
    void DrawLoadoutWindows();

    // GI53: drop popup state (buy/delete asks) when the menu closes — a
    // confirm left open used to reappear on the next open.
    void OnMenuClosed();
    // true while EITHER loadout popup is up. One layer to the close-order
    // stack; CloseTopPopup keeps its own buy-then-delete order inside.
    [[nodiscard]] bool IsPopupOpen();
    bool CloseTopPopup();   // I/ESC layering: close an open buy/delete popup

    // v9 spec: every slot = 2x2 inventory cells; weapon/body/shield = 2x4 (+5px)
    // ★kDollTrim: the doll is measured in cells like everything else, but it —
    // not the grid — sets how tall the left column is, and the stats panel
    // below it is TYPE, so it does not shrink with the cells. Taking 5% off
    // the slots (and halving the gaps) buys back the room the stats panel
    // needs at small cell sizes, without touching a single font size.
    inline constexpr float kDollTrim = 0.95f;
    [[nodiscard]] inline float SlotPx() { return Grid::CellPx() * 2.0f * kDollTrim; }
    [[nodiscard]] inline float GapPx() { return 2.5f * Theme::Scale(); }
    // ★A tall slot must equal TWO short ones plus the gap between them — that
    // is what keeps the weapon/body column level with the accessory column
    // beside it. The old literal +5 happened to equal the gap, so halving the
    // gap silently broke the identity and the two columns drifted apart by
    // exactly the difference. Say it as the identity instead of a number.
    [[nodiscard]] inline float TallPx() { return 2.0f * SlotPx() + GapPx(); }
    // vertical strip (circlet + acc x4) + doll 3 columns
    [[nodiscard]] inline float PanelW() { return 4 * SlotPx() + 3 * GapPx(); }
    // tabs row + doll rows [2u, 2x4, 2u, 2u]
    [[nodiscard]] inline float PanelH()
    {
        return 30.0f * Theme::Scale() + 3 * SlotPx() + TallPx() + 3 * GapPx();
    }

    // ★GI77: how far the first slot row sits below the column's top — i.e. the
    // height the loadout tab strip actually took. MEASURED during the last
    // Draw() rather than assumed: the 30*S above is a reservation that rounds
    // generously, and the item grid has to line up with the real slot edge,
    // not with a budget. 0 before the first draw.
    [[nodiscard]] float SlotsTopOffset();

    // ★The same row in SCREEN space. Not derivable from the offset above:
    // Draw() runs inside the "fab_left" CHILD, so that number is relative to
    // the child, and adding it to the MAIN window's origin lands a child's
    // worth too high (it did — the drawer's first row sat above the doll's).
    // Anything drawing outside both windows wants this one.
    [[nodiscard]] float SlotsTopScreen();

    // ---- accessory drawer (PLAN_ACCESSORY_DRAWER.md) ----------------------
    //
    // The doll holds four accessory cells (acc1..acc3 + accM — the strip's
    // fifth is the drawer's button), and the next worn accessory used to be
    // DROPPED: silently, with nothing on screen and nothing in the log.
    // Outfit mods routinely equip eight to fifteen — scarves, bags, belts,
    // glasses, cloaks, piercings all land on biped slots 44..60, which
    // SlotForArmor has no name for.
    //
    // The drawer is the rest of that list, in a panel docked to the main
    // window's left edge. The strip is effectively column 0 and the drawer
    // continues it, so between them nothing worn is invisible.

    // The lowest biped slot (30..61) this armour occupies, or a large number
    // when it claims none.
    //
    // ★THIS decides where an accessory sits, not the order it was equipped in.
    // Assignment used to be `accNext++`, so taking one item off and putting it
    // back rearranged the others. A slot number cannot drift: wear the same
    // outfit and the cells are the same cells.
    [[nodiscard]] int PrimarySlot(RE::TESBoundObject* a_obj);

    // How many worn accessories did not fit the doll's five cells. May be 0 —
    // the tab shows regardless, because a drawer that appears only once you
    // are overflowing is a drawer nobody knows exists.
    [[nodiscard]] int DrawerCount();

    [[nodiscard]] bool DrawerOpen();
    void               SetDrawerOpen(bool a_open);

    // ★NOT during loot/barter. The doll itself is hidden there (the partner
    // window takes that side of the screen), so a drawer hanging off it would
    // be a panel belonging to something the player cannot see.
    [[nodiscard]] bool DrawerAvailable();

    // Draws BOTH the tab and the panel, from outside the main window — UIRoot
    // calls this at top level, beside the shelf-bag windows.
    //
    // ★The tab sits in the main window's own margin, level with the first
    // slot row, and STAYS there -- it used to ride the panel's outer edge,
    // so the control that opens the drawer travelled with the drawer and had
    // to be found again after every slide. It still cannot be drawn BY the
    // main window (a widget clips to the window that declares it, and the
    // panel it toggles lies entirely outside that rectangle), so one function
    // owns both halves and a slide can never leave them disagreeing.
    //
    // Position is derived, never remembered: the panel's inner edge is the
    // main window's left edge and its first CELL — not its frame — sits on
    // the doll's first slot row, so moving the window moves the drawer with
    // it and the two grids read as one column.
    void DrawDrawer(const ImVec2& a_mainPos, const ImVec2& a_mainSize,
                    float a_rowTopScreen);

    // ★★What the TOOLTIP says an item is worn on, and it has to agree with the
    // doll: SlotAccepts already files every biped slot this UI does not know as
    // an ACCESSORY, so an unknown one reads "Accessory (47)" rather than having
    // a name invented for it.
    // ★The number is the SLOT (30..61), not the bit index -- that is what mod
    // pages and conflict guides quote, and the one a player can act on.
    // Empty when the thing is not worn at all.
    [[nodiscard]] std::string SlotLabel(RE::TESBoundObject* a_obj);
}
