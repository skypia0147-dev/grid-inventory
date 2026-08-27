#pragma once

#include <string>

// PLAN_LOOT_BARTER — the partner window (container / merchant) that opens
// ALONGSIDE the player grid, plus the global UI mode that the player-side
// right-click handler branches on so looting/bartering never collides with
// the normal equip action.
//
// Phase 1: mode state, vanilla-menu interception glue, and an empty docked
// partner window. Item display (Phase 2) and item movement (Phase 3/5) build
// on this scaffold.

namespace FUI::LootBarter
{
    enum class Mode
    {
        kNormal,   // plain inventory — right-click equips
        kLoot,     // container / corpse / companion — right-click takes/stores
        kBarter,   // merchant — right-click buys/sells
        kSteal,    // F6a: owned container — loot behaviour, takes use the
                   // engine's kSteal removal (crime/stolen handled for us)
        kPickpocket// F6b: living target — every move rolls the vanilla
                   // pickpocket formula (AttemptPickpocket); a failed roll
                   // raises the engine's crime response and force-closes
    };

    [[nodiscard]] Mode CurrentMode();

    // kSteal behaves exactly like kLoot everywhere except the removal reason
    // and the red STEAL chrome — every "is this the loot flow?" check goes
    // through here so the two can never drift apart.
    [[nodiscard]] inline bool IsLootMode(Mode a_mode)
    {
        return a_mode == Mode::kLoot || a_mode == Mode::kSteal;
    }

    // Enter loot/barter mode with the container/merchant reference. Called
    // from the menu-open interception (main.cpp) as the vanilla menu is
    // swallowed and our grid is opened. Stored as a handle internally.
    void Enter(Mode a_mode, RE::TESObjectREFR* a_partner);

    // Back to kNormal — called when our grid menu closes (UIRoot::OnClose).
    void Reset();

    // The container/merchant reference, resolved & null-checked (null in
    // kNormal or if the handle went stale).
    [[nodiscard]] RE::TESObjectREFR* Partner();

    // Draw the partner-side window(s). Called from UIRoot::Render inside the
    // ImGui frame. No-op in kNormal.
    void DrawWindows();

    // True if the mouse is over the partner window this frame (drag-to-store
    // target). Valid after DrawWindows ran; false in kNormal.
    [[nodiscard]] bool IsPartnerHovered();

    // ---- item transfer (loot mode) ----------------------------------------
    // Queued from the render pass, applied on Tick (game thread) — engine
    // inventory moves must not run mid-frame.
    // D4: a_uid/a_xlIdx name WHICH sub-stack moves (0/-1 = engine picks, still
    // correct for fungible goods). partner -> player
    // a_fromWorn: the cell came from the actor'''s BODY. A worn unit with no
    // signature has no pool handle at all, so this is the only way to say
    // "the equipped one, not a spare".
    // a_useAfter: SHELF USE MODE (shift + right-click). The unit is taken and
    // then used exactly as a right-click on the player's own board would use it
    // -- the engine can only consume from the player's inventory, so passing
    // through it is not a shortcut but the only road there is.
    // ★S-0: returns whether the transfer was actually QUEUED. Almost every
    // caller ignores it and always could -- the request is fire-and-forget by
    // design. It exists for the one caller that has to arrange something else
    // around the arrival (announcing incoming gold, see GoldCoins::
    // ExpectIncoming): a promise made for a transfer that was refused up front
    // is a lie, and the refusal is invisible from outside without this.
    bool RequestTake(RE::TESBoundObject* a_obj, int a_count,
                     std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                     bool a_fromWorn = false, bool a_useAfter = false);
    // ★(1.5.x stack flow) THE WHOLE CELL, clamped to what the boards can hold.
    //
    // Right-clicking a stack used to open the quantity window; it hauls the
    // cell home now, which is the grammar gold has had since 1.5.0 and the
    // one this replaces it with everywhere a move only changes WHERE a thing
    // is. Shift+left still splits, so choosing an amount never left.
    //
    // The clamp is the slider's own (MaxAcceptUnits) moved to the call site:
    // whatever fits arrives and the remainder stays in the container -- the
    // same outcome as confirming the slider at its clamped maximum, minus the
    // question. Returns the units requested (0 = none fit; the note has
    // already played, and the caller must not queue anything else).
    int RequestTakeAll(RE::TESBoundObject* a_obj, int a_count,
                       std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                       bool a_fromWorn = false);
    // a_xlIdx: where the outgoing unit sits, so the sink removes THAT one
    // (see XferReq::xlIdx). -1 = unknown, historical behaviour.
    // a_srcKey: the tile the units leave (B3-b) -- rides the ledger request so
    // the engine's confirmation retires THAT cell and no other. "" = fragment.
    void RequestStore(RE::TESBoundObject* a_obj, int a_count,
                      std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                      bool a_fav = false, int a_xlIdx = -1,
                      const std::string& a_srcKey = {});   // player -> partner (GI36: a_fav)
    // barter (Phase 5): item move + gold settlement + speech xp, all on Tick.
    // a_baseTotal = total BASE value of the goods — vanilla speech XP points
    // (the haggled price doesn't matter for XP).
    void RequestBuy(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal = 0,
                    std::uint16_t a_uid = 0, std::uint16_t a_sig = 0);   // merchant -> player
    void RequestSell(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal = 0,
                     std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                     bool a_fav = false, int a_xlIdx = -1,
                     const std::string& a_srcKey = {});  // player -> merchant (GI36: a_fav)
    // F6b: pickpocket moves — each rolls PlayerCharacter::AttemptPickpocket
    // on the Tick (crime / XP / detection handled by the engine); a failed
    // roll force-closes the menu, already-succeeded moves stay.
    void RequestPickTake(RE::TESBoundObject* a_obj, int a_count,                      // target -> player
                         std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                         bool a_fromWorn = false);
    void RequestPickStore(RE::TESBoundObject* a_obj, int a_count,                       // player -> target
                          std::uint16_t a_uid, std::uint16_t a_sig,
                          const std::string& a_srcKey = {},
                          bool a_fav = false, int a_xlIdx = -1);                         // (reverse-pickpocket)
    void ProcessTransfers();   // UIRoot::Tick

    // ★(1.3.0) the gold riding the CARRIED shelf slot, or -1 when the
    // current carry did not come off a shelf. A pouch lifted inside the
    // container keeps its amount on its (reserved) slot -- the cursor ghost
    // asks here, since g_pouchStored only knows the player's own tiles.
    [[nodiscard]] int HeldShelfGold();

    // ---- (1.3.0-D) bag bundles --------------------------------------------
    // A bag stored into a container takes its CONTENTS along. The shelf
    // remembers them as one bundle on the bag's spot -- their cells are
    // hidden (no opening a bag inside a chest, per spec), and taking the
    // bag back brings them home. Selling stays the old spill.
    struct BundleItem
    {
        RE::FormID    form = 0;
        int           count = 0;
        std::uint16_t sig = 0;   // preferred sub-stack on the trip back
        // ★(1.3.1) anchor inside the OPEN shelf-bag window (cosave v5).
        // -1 = unplaced (first-fit on the next open, then written back).
        int           col = -1;
        int           row = -1;
        // ★(1.3.2) quarter-turns clockwise, the angle it was dropped at
        // (cosave v6). Footprint w/h swap when odd.
        int           rot = 0;
        // ★(1.3.2) the marker bits this unit carried IN (cosave v7):
        // bit1 enchanted, bit2 unique, bit4 poisoned -- the same word
        // Item::glow and PartnerCell::glow hold. RECORDED rather than
        // recomputed: nothing can change an item's state while it sits in a
        // container, and asking the engine per entry per frame would put a
        // GetInventory walk on the draw path. The FAVOURITE star is
        // deliberately not among them -- see the store paths, which strip it
        // as the unit leaves the player.
        std::uint8_t  glow = 0;
        // ★(1.3.2) someone else's goods (cosave v8). NOT derivable at draw
        // time on this side: the player board keeps ownership per POOL in
        // its own table, and a shelf cell only knows "the whole container is
        // owned" (kSteal). A unit that was stolen when it went in stays
        // stolen in the bag, and the mark has to say so.
        bool          stolen = false;
        // ★★★A BAG HAS A NAME, NOT A NUMBER (cosave v12).
        //
        // A bundle carries a tree by having every entry name the bag it sits
        // in, and that name used to be an INDEX INTO THIS SAME VECTOR. Every
        // hole reported around nested shelf bags was one way for an index to
        // go stale: erasing an entry slid the ones after it, moving one
        // renumbered the whole tree while every reference kept the old
        // numbers, and a window holding a position ended up looking at
        // whatever slid into it. Five fixes in a row were index arithmetic.
        //
        // So the index is gone. The player's own board never had any of these
        // bugs, and the reason is one line: LayoutEntry::bag is a KEY. A name
        // survives every reorder, and nothing has to be repaired after one.
        //
        // id     : minted once (LootBarter::MintBundleId) and never rewritten
        //          -- not by a move, a deletion, or a trip between containers.
        // parent : the id of the bag entry this one sits in. 0 = directly
        //          inside the bag that owns the bundle.
        std::uint32_t parent = 0;
        std::uint32_t id = 0;
        // ★(1.5.x) a bundled POUCH's stored amount (cosave v15) -- the
        // nested twin of ContCell::gold, which is what gives a pouch inside
        // a shelved bag the same banking a directly shelved one has. -1 =
        // never claimed (an old save's entry, or a non-pouch); the claim
        // grace below retries against the away parcels exactly as a cell's
        // awaitGold does.
        int           gold = -1;
        // transient (NOT cosaved): reconcile passes left to claim a parcel.
        // Re-armed on load for pouch entries whose gold is still -1, which
        // is the whole v<15 migration -- their parcels are still away.
        int           awaitGold = 0;
        // transient: the EXACT parcel this entry expects (stamped from its
        // tile's amount at the manifest build). The claim matches on it
        // first, so two same-form pouches stored in one gesture cannot
        // trade amounts; 0 = no expectation (first-come, the old rule).
        int           wantGold = 0;
    };
    // ★A fresh, never-reused name for a bundle entry. One counter for the
    // whole session rather than one per container: an entry that crosses from
    // one bag to another -- or one chest to another -- keeps the id it was
    // born with, so no boundary has to renumber anything.
    [[nodiscard]] std::uint32_t MintBundleId();
    // Grid calls this as it queues the contents' stores; the bundle waits
    // (per container) for the bag's shelf spot to be born and rides it.
    void NoteBagBundle(RE::FormID a_bagForm, std::vector<BundleItem> a_items);
    // Grid's rebuild asks: the bundle whose bag just walked back in (a
    // fresh bag tile of this form). One-shot -- empty when none waits.
    [[nodiscard]] std::vector<BundleItem> TakeIncomingBundle(RE::FormID a_bagForm);
    // ★(1.3.1) a stored bag can be OPENED (right-click, player-bag grammar):
    // a real grid window over its bundle -- items draw at their anchors,
    // lift onto the cursor, and player items drop in. Top level (UIRoot).
    void DrawShelfBag();
    // ★(1.5.0 audit) is the shelf bag ON this spot showing its window?
    // The prompt bar's open/close verb for a partner bag cell asks here --
    // the player-side open-bag book knows nothing about shelf windows.
    [[nodiscard]] bool IsShelfBagOpen(std::string_view a_spotKey);
    // The active carry was lifted OUT of a shelf-bag bundle. Consuming it
    // (the take home, or a drop on the container board) removes the carried
    // units from the bundle -- which is what lets their engine item's cell
    // surface. Returns false when no bundle carry is active / form differs.
    [[nodiscard]] bool IsBundleCarry();
    // a_toPlayer: is this carry ending up in the PLAYER's hands, or back on
    // the shelf? A bag leaves with its branch either way, but the branch has
    // two different destinations -- home with it, or onto the shelf cell it is
    // becoming -- and only the caller knows which.
    bool ConsumeBundleCarry(RE::TESBoundObject* a_obj, int a_count,
                            bool a_toPlayer = true);
    // ★(1.3.2a) the shelf POUCH banks like the player's: right-click opens
    // its withdraw window (DrawShelfPouch, top level), and gold dropped on
    // its cell deposits. DepositOnHoveredPouch banks into the hovered pouch
    // cell's SLOT and returns the amount actually taken (0 = not over a
    // pouch / no room); the caller settles the player-side ledger.
    void DrawShelfPouch();
    int  DepositOnHoveredPouch(int a_value);
    // ★(1.4.4) shelf-internal banking: the PARTNER-carried gold cell dropped
    // on a shelf pouch cell deposits into it (it used to fall through to the
    // rearrange grammar and SWAP). Moves the amount between the two cells'
    // books and queues the matching engine debit; returns the amount moved
    // (0 = pouch full / not applicable -- the caller keeps the carry).
    int  DepositHeldGoldIntoShelfPouch(const std::string& a_pouchKey);
    // ★(1.5.x) the bundled twins: a pouch INSIDE an open shelf-bag window
    // banks like a shelf pouch cell. The bag window records which pouch
    // entry the cursor is over while a carry rides (per frame); the coin
    // drop routes ask here first. Deposit writes the entry's own amount and
    // returns what moved (0 = full / nothing hovered); the caller settles
    // the player-side ledger, same contract as DepositOnHoveredPouch.
    [[nodiscard]] bool IsBundlePouchHovered();
    int  DepositOnHoveredBundlePouch(int a_value);
    // ...and the PARTNER-carried gold cell dropped on one (the bundled twin
    // of DepositHeldGoldIntoShelfPouch): moves from the carried cell into
    // the entry's book and settles the engine stack itself.
    int  DepositHeldGoldIntoBundlePouch();
    // ★(1.5.x) player gold dropped on an open shelf-bag window (not on a
    // pouch): the bag window records the hovered square per frame; the Grid
    // coin route stores the physical Septims into the container and books
    // them here as a GOLD ENTRY of that bag. Returns the amount booked.
    [[nodiscard]] bool IsShelfBagHovered();
    int  IntakeGoldEntry(int a_amount);

    // ★(1.5.x) a book read OFF THE SHELF (shift+right-click) offers the
    // world grammar: E takes it home while the page is up. The read branch
    // notes the cell; the input sink asks Armed and flags the take (thread-
    // safe); the render thread consumes the flag at the page-close edge.
    void NoteShelfBookRead(RE::TESBoundObject* a_book, std::uint16_t a_uid,
                           std::uint16_t a_sig, const std::string& a_spotKey);
    [[nodiscard]] bool ShelfBookTakeArmed();
    void FlagShelfBookTake();
    void ProcessShelfBookTake();   // page-close edge (UIRoot::Render)

    // ★(1.3.3) A LIVING FOLLOWER'S PACK IS 10 x 8. Chests, corpses and
    // merchants are unbounded and always answer true; a companion answers
    // from THIS frame's placement (partial stacks count as room). Every
    // player -> partner store asks before it queues.
    [[nodiscard]] bool PartnerHasRoomFor(RE::TESBoundObject* a_obj, int a_count = 1);

    // Quantity slider (Shift+right-click on a stack). Opens a small popup to
    // pick how many to move; confirm queues the transfer.
    // ★kShelfSplit is kPickup's twin on the far side of the window: shift +
    // left-click on a container cell splits that many onto the CURSOR instead
    // of hauling them home. Nothing leaves the container -- a lift is not a
    // take -- so the split is pure cell surgery, and where the fragment ends up
    // is then the ordinary drop grammar's question: back on the shelf, or over
    // on the player's board (which is where it becomes a take).
    enum class XferDir { kTake, kStore, kPickup, kBuy, kSell,    // kPickup = split onto cursor
                         kPickTake, kPickStore,                  // F6b pickpocket rolls
                         kShelfSplit,
                         // ★(1.5.x stack flow) R on a stack. kTake/kStore no
                         // longer raise this window at all -- moving a stack
                         // is a whole-cell act now, like gold -- so the only
                         // directions left that ASK are the ones where the
                         // answer cannot be undone with one more click:
                         // money (kBuy/kSell), the pickpocket roll, the
                         // deliberate split, and this.
                         kDrop };
    // a_srcKey (kPickup only): the grid tile the split quantity is taken from.
    // a_unitValue (kBuy/kSell only): the item's base value per unit, so the
    // confirmed quantity is priced (buy/sell price * count).
    // GI25: a_uid/a_sig name the POOL the units come from, so the deferred
    // confirm still moves the right sub-stack.
    void OpenSlider(RE::TESBoundObject* a_obj, int a_max, XferDir a_dir,
                    const std::string& a_srcKey = {}, int a_unitValue = 0,
                    std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                    bool a_worn = false,    // a_worn: the cell came off the body
                    bool a_fav = false,     // GI36: the cell wore a star
                    int a_xlIdx = -1);      // (1.3.3) which unit leaves
    void DrawSlider();   // UIRoot::Render (top level)
    [[nodiscard]] bool SliderActive();

    // Phase 5: favorite-sale confirm popup. a_baseTotal = speech XP points.
    void AskSellConfirm(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal = 0,
                        const std::string& a_srcKey = {},
                        std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                        bool a_fav = true,
                        int a_xlIdx = -1);   // GI25 / GI36 (this popup only fires for a star)
    void DrawConfirm();   // UIRoot::Render (top level)
    [[nodiscard]] bool ConfirmActive();

    // I/ESC layering: close the topmost open popup (confirm > slider);
    // returns false when neither is open (the caller may close more UI)
    // true while EITHER sub-popup is up (confirm sits above the slider). The
    // close-order stack treats this module as one layer and lets CloseTopPopup
    // keep its own internal order.
    [[nodiscard]] bool IsPopupOpen();
    bool CloseTopPopup();

    // ---- barter pricing (Phase 4) -----------------------------------------
    // UESP formula: factor = fBarterMax - (fBarterMax-fBarterMin)*min(speech,100)/100
    // buy  = round(value * buyMod  * factor)   (player buys FROM the merchant)
    // sell = round(value * sellMod / factor)   (player sells TO the merchant)
    // buyMod/sellMod = perk coefficients (Haggling/Allure) from HandleEntryPoint
    // (kModBuyPrices/kModSellPrices); 1.0 with no perks -> pure speech factor.
    [[nodiscard]] int BuyPrice(RE::TESBoundObject* a_item, int a_baseValue);
    [[nodiscard]] int SellPrice(RE::TESBoundObject* a_item, int a_baseValue);
    // B7: stack totals round ONCE on the total (per-unit rounding x count
    // inflated cheap bulk buys / deflated bulk sells); floor 1 gold per unit
    [[nodiscard]] int BuyPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count);
    [[nodiscard]] int SellPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count);
    [[nodiscard]] int MerchantGold();   // SourceRef()'s Gold001 count; 0 outside kBarter

    // F3 (settings): unlimited merchant gold. While ON, MerchantGold() reports
    // INT_MAX so every "can the merchant pay?" check passes; the bottom bar
    // shows an infinity glyph instead. Settlement still moves real coins (the
    // merchant's stash simply bottoms out at 0 — harmless, never shown).
    [[nodiscard]] bool MerchantGoldInfinite();
    void SetMerchantGoldInfinite(bool a_on);

    // F4 (settings): lift the vendor CATEGORY whitelist/blacklist so any
    // merchant buys anything. The stolen-goods rule is NOT lifted — stolen
    // items still need a fence (buysStolen), matching the confirmed spec.
    [[nodiscard]] bool MerchantBuysAll();
    void SetMerchantBuysAll(bool a_on);

    // ---- F7: container spot memory (kLoot / kSteal) ------------------------
    // The partner grid remembers per-container item spots ("organize your
    // chest" like the player board). Barter keeps the auto-pack.

    // Store-drop target for the carried item: onCell = the grab-adjusted
    // footprint anchor landed on the partner board (spot-memory mode).
    // freeSpot = the whole footprint fits with no overlap; occ = EXACTLY one
    // overlapping item (the swap partner); neither = 2+ blockers (invalid).
    // Anchor math mirrors the player grid (mouse - grab offset, clamped).
    struct StoreDrop
    {
        bool                onCell = false;
        bool                freeSpot = false;
        int                 col = -1;
        int                 row = -1;
        RE::TESBoundObject* occ = nullptr;
        int                 occCount = 0;
        int                 occValue = 0;
        int                 occCol = 0;   // occupant's anchor (swap target spot)
        int                 occRow = 0;
        // GI24: WHICH occupant. A swap hands it to the cursor, so the carry has
        // to inherit its sub-stack AND its pool slot -- otherwise it is picked up
        // as an anonymous (uid 0, xlIdx -1, ord 0) unit, its slot stays behind
        // unreserved, and dropping it lands in a fresh slot at the front.
        std::uint16_t       occUid = 0;
        int                 occXlIdx = -1;
        int                 occOrd = 0;
        std::string         occSpotKey;
        int                 occRot = 0;   // GI62: and the angle it lies at
    };
    [[nodiscard]] StoreDrop QueryStoreDrop();

    // GI24: adopt a pool slot for a carry that did not start with a click (the
    // swap hands the displaced occupant to the cursor). Its slot must be held
    // for it, or the next drop treats it as a brand-new arrival.
    void NoteCarriedSpot(const std::string& a_spotKey);

    // ---- writing to the container's board ---------------------------------
    //
    // ★★★These replace NoteStoreSpot / SetStoreSpotHint, which were NOTES: a
    // claim pushed onto a pending queue describing where an item ought to end
    // up once the engine got round to moving it, for a matcher to try to honour
    // some frames later. The container owns its cells now, so these are writes,
    // and they take effect the instant the player lets go.

    // The carried cell (the one this carry was lifted from) lands on an empty
    // square of the same shelf. No engine transfer is involved -- nothing
    // enters or leaves the container -- which is what made the old road absurd:
    // a rearrange had to travel machinery built for transfers.
    bool MoveHeldCell(int a_col, int a_row, int a_rot);

    // ...or lands on another cell, and the two trade places.
    bool SwapHeldCellWith(const std::string& a_otherKey);

    // ...or lands on a cell holding the same thing, and they become one up to
    // the cap. Returns what would not fit (0 = all of it went, -1 = not a
    // merge at all); the caller keeps carrying the remainder.
    int MergeHeldCellInto(const std::string& a_otherKey);

    // A PLAYER item put down on the shelf: the cell exists from this instant.
    // The engine transfer backing it is queued separately and lands next Tick;
    // the promised-units ledger keeps the reconcile from doubting the cell in
    // between. a_rot = the quarter-turn it was dropped at, which is how a
    // rotation crosses from the inventory into a container.
    // ★a_uid/a_sig name the POOL the arriving units belong to, and they are
    // not decoration: the reconcile matches this cell to the engine by pool, so
    // a cell minted without the unit's uid sits in a pool nothing arrives into
    // and is swept away on the very next pass -- the aimed square would be lost
    // for exactly the items most worth aiming (a named weapon, a tempered one).
    void PlaceStoredCell(RE::TESBoundObject* a_obj, int a_count,
                         int a_col, int a_row, int a_rot = 0,
                         std::uint16_t a_uid = 0, std::uint16_t a_sig = 0);

    // A store with no aimed square (right-click, take-all, a slider the player
    // opened from the list): no cell is placed, but the units still have to
    // count as present until the engine catches up, or the reconcile would show
    // them arriving twice.
    void NoteStoredUnits(RE::TESBoundObject* a_obj, int a_count,
                         std::uint16_t a_uid = 0, std::uint16_t a_sig = 0);

    // ⛔AimStoreAt is gone (1.5.x stack flow). It held the square a stack was
    //  dropped on ACROSS the store quantity popup, because the drop happened
    //  before the number was known. A store asks no number now, so the drop
    //  path places on that square itself -- and swaps with an occupant, which
    //  the round trip never could.

    // cosave 'GCLY' v1: container ref FormID -> (item key -> spot), LRU 128.
    // main.cpp owns the record loop.
    inline constexpr std::uint32_t kContRecordType = 'GCLY';
    // ★A container that respawned is a different container as far as its
    // contents go: the player's deposit went with the reset, and a ledger that
    // outlives it hands the replacements over as the player's own. Called from
    // the TESResetEvent sink.
    void ForgetDeposits(RE::FormID a_containerID);

    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame();

    // Phase 6: does the ACTIVE merchant BUY this player item? Enforces the
    // vendor faction's category list (vendorSellBuyList + notBuySell) and the
    // stolen policy (buysStolen / buysNonStolen). Always true outside kBarter or
    // when the partner has no vendor faction (plain container / corpse).
    // a_stolen = the item is NOT owned by the player (Grid caches this).
    [[nodiscard]] bool MerchantBuys(RE::TESBoundObject* a_obj, bool a_stolen);
}
