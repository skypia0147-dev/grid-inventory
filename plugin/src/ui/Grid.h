#pragma once

#include <source_location>

#include "game/Lotd.h"
#include "ui/ItemDef.h"
#include "ui/Lang.h"
#include "ui/Theme.h"

#include <functional>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>

namespace FUI::Grid
{
    // GridDef is an alias of the ONE shared FUI::ItemDef (Phase 2 D2) —
    // resolved by main.cpp through the ini-override -> category-preset path.
    using DefResolver = std::function<GridDef(RE::TESBoundObject*)>;
    void SetDefResolver(DefResolver a_resolver);

    // ★★USER-DECLARED UNIQUES, by FormID, true = force the mark on / false = off.
    // The built-in rule ("its enchantment has no base, so it can never be
    // learned") finds the Daedric artifacts and little else: a unique that is
    // unenchanted (the Longhammer, Valdr's Lucky Dagger), scripted (Nettlebane,
    // the Bloodskal Blade) or simply disenchantable (Grimsever, Okin, Eduj)
    // carries nothing in its record to separate it from ordinary gear, and a
    // mod's artifacts carry nothing either. Naming them in code meant a rebuild
    // per item and gave mod users no way in at all.
    // Overrides win over the rule in both directions, so a false positive can
    // be switched off as well.
    void SetUniqueOverrides(std::unordered_map<RE::FormID, bool> a_map);

    // Resolve an item's footprint def via the registered resolver (same path
    // the main grid uses). LootBarter reuses this so the partner window draws
    // items at their real size (sword 1x3, helmet 2x2) instead of 1x1.
    [[nodiscard]] GridDef ResolveDef(RE::TESBoundObject* a_obj);

    // ★Turn the [POOL]/[TAKE]/[CHECK] diagnostics on from the ini
    // ("!pooltrace = 1" in GridInventory_ui.ini). Ships OFF. These are what
    // turn "a spare vanished" or "it will not move" into a log line naming
    // the pool, the signature and which unit came off the board -- so a
    // report that cannot be reproduced here can still be diagnosed there.
    void SetPoolTrace(bool a_on);

    // *TEST ONLY ("!simdrift = 1"), ships OFF. Corrupts the identity the
    // carry-exclusion is given, reproducing a signature that moved under us
    // -- a held unit must still leave the board. Use it to verify the
    // fallback, never in a shipped configuration.
    void SetSimDrift(bool a_on);

    // ...and read back, so the ini writer can keep a switch the tester
    // turned on across a restart instead of making them set it every time.
    [[nodiscard]] bool PoolTrace();
    [[nodiscard]] bool SimDrift();

    // ★Tile keys of the coin pouches on the board right now, front cell
    // first. GoldCoins needs them for two things it cannot work out on its
    // own: which pouch an unaddressed deposit should fill, and which one a
    // returning (or pre-1.3.0) amount belongs to.
    [[nodiscard]] std::vector<std::string> PouchTiles();
    [[nodiscard]] std::string              AnyPouchTile();
    // ★(1.3.0) hand any waiting return to the pouch tiles, NEW tiles first
    // (the pouch that just walked in claims its own gold before any
    // pre-existing empty pouch gets a look). Called on every rebuild.
    void ClaimIncomingPouchGold();

    // Game-side actions (main.cpp wires these):
    //  sound(obj, up): vanilla per-item pick-up (up=true) / put-down sound
    //  dropToWorld(obj, count, xl): RemoveItem(kDropping) at the player's feet;
    //  count > 0 drops that many, <= 0 drops the whole stack.
    //  GI25: xl names the sub-stack to drop (nullptr = engine's choice, which is
    //  correct only for units that carry no extras at all).
    void SetGameCallbacks(std::function<void(RE::TESBoundObject*, bool)> a_sound,
                          std::function<void(RE::TESBoundObject*, int,
                                             RE::ExtraDataList*)> a_dropToWorld);

    // Click-carry state (C1~C7). ESC/right-click cancels; the menu's Cancel
    // handler must check IsHolding() before closing (A2).
    [[nodiscard]] bool IsHolding();
    // GI63: may the carried item be turned? The prompt bar hides the rotate keys
    // for square footprints, where pressing them does nothing.
    [[nodiscard]] bool HeldCanRotate();

    // GI63: what the tooltip is describing THIS FRAME, for the prompt bar.
    // ★Recorded by DrawItemTooltip, which every board shares (grid, doll,
    // partner), so the bar needs to know nothing about who is hovering what.
    // Stamped with the frame number rather than cleared at a fixed point: the
    // bar draws after the tooltips, and a "clear at end of frame" would have to
    // land in exactly one place between the two.
    struct HoverPrompt
    {
        bool active = false;
        bool canSplit = false;    // a stack / divisible gold: Shift+click applies
        bool canCompare = false;  // weapon or armour: hold Shift for the card
        bool canPick = false;     // left-click lifts it (false on a parked tile)
        bool canDrop = false;     // R discards one (never a quest item / partner)
        bool canFav = false;      // F stars it (never a coin / pouch / partner)
        bool hasVerb = false;
        Lang::Str verb{};         // the right-click action, already resolved by
                                  // the tooltip: equip / read / learn / use /
                                  // sell / store / open bag / restore / buy /
                                  // steal / plant / withdraw / unequip
        bool canRecharge = false; // T feeds it a soul gem (enchanted weapon,
                                  // not already full, not the partner's)
    };
    [[nodiscard]] HoverPrompt HoveredPrompt();
    void CancelHold();

    // Phase 5-B: the partner item currently carried on the cursor (null if the
    // held item isn't from a partner). The partner window hides it so its source
    // cell reads as empty while carried.
    [[nodiscard]] RE::TESBoundObject* HeldPartnerObject();

    // ★B4-4: is a CARRIER carry up -- the displaced second ring riding the
    // cursor? The quiet ring-swap handoff keys its "no redraw needed" on
    // exactly this: the drop path starts that carry before Wear runs, the
    // right-click router displaces with no carry at all.
    [[nodiscard]] bool CarrierCarryActive();

    // GI17: the same question narrowed to ONE sub-stack. Partner windows must
    // use this -- with several cells per form, the form-level test hides them
    // all the moment one is lifted.
    [[nodiscard]] bool IsHeldPartnerUnit(RE::TESBoundObject* a_obj,
                                         std::uint16_t a_uid, int a_xlIdx, int a_ord);

    // GI30: the favourite of this unit's pool is currently worn (its tile is
    // parked without a cell). The doll shows the star meanwhile.
    [[nodiscard]] bool IsPoolStarWorn(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                      std::uint16_t a_sig);

    // ★1.0.5: ...and whether that pool is stolen goods, for the same reason —
    // equipping a stolen item must not make it stop looking stolen.
    [[nodiscard]] bool IsPoolStolen(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                    std::uint16_t a_sig);

    // GI18: content signature of the carried unit (0 = nothing, or a plain
    // unit). Survives a container move, unlike an ordinal or a uniqueID, so a
    // drop position recorded before the engine transfer can still be matched to
    // the cell that appears afterwards.
    [[nodiscard]] std::uint16_t HeldInstanceSig();

    // F7: carried footprint (cells) + grab offset (px) for the partner
    // grid's drop ghost / drop-cell math. False when nothing is carried.
    bool HeldFootprint(int& a_w, int& a_h, float& a_offX, float& a_offY);

    // v9.2: start carrying an item that is NOT in the grid (equipment doll
    // pickup — the unequip runs deferred, the carry starts immediately).
    // GI25: a_uid/a_sig identify the sub-stack being lifted (the doll's pickup
    // is the WORN one, which is usually the tempered or enchanted copy).
    // a_swappedOut: this unit was DISPLACED by a drop onto an occupied slot, so
    // the engine unequips it as part of that same equip. It stops counting as
    // worn the moment the equip lands, not when an unequip of its own runs.
    // a_count: how many units ride the cursor. One for anything worn a copy at
    // a time — but a quiver is unequipped whole, so the carry has to be whole
    // too, or the rest of it lands in the pack the instant it comes off.
    // a_fromCarrier: lifted from the SECOND ring slot -- never engine-worn,
    // so the carry must not claim a worn list (see Held::fromCarrier).
    void BeginCarry(RE::TESBoundObject* a_obj, std::uint16_t a_uid = 0,
                    std::uint16_t a_sig = 0, int a_hand = 0,
                    bool a_swappedOut = false, int a_count = 1,
                    bool a_fromCarrier = false);

    // Phase 5-B: carry a PARTNER (merchant/container) item on the cursor.
    // Dropping it onto the player grid takes (loot) or buys (barter).
    // a_value = base value per unit (for buy pricing).
    // a_offX/a_offY: grab offset px within the footprint (F7: pick up where
    // clicked, like player tiles); negative = centre (swap pickups).
    // D4: a_uid/a_xlIdx = the partner sub-stack picked up, so the eventual
    // take/buy moves THAT unit and not whichever one the engine fancies.
    // GI62: a_rot = the quarter-turn the cell sits at on the partner board, so
    // the carry (and any drop back into the inventory) keeps it.
    void BeginPartnerCarry(RE::TESBoundObject* a_obj, int a_count, int a_value,
                           float a_offX = -1.0f, float a_offY = -1.0f,
                           std::uint16_t a_uid = 0, int a_xlIdx = -1, int a_ord = 0,
                           int a_rot = 0);

    // Deferred rebuild (safe to request mid-draw; runs at FinishFrame).
    // ★B3-c: who asked, without touching thirty call sites. Before the board
    // can stop rebuilding, we have to know what has been making it rebuild --
    // the log shows several per second with the item count oscillating by one
    // (PLAN §4-2), and nobody has ever established why.
    void RequestRebuild(const std::source_location& a_where =
                            std::source_location::current());
    // ★B3 body: the partial board update for an engine unequip -- the return
    // leg of NotePendingEquip's optimistic hide. Reuses the shared unit walk
    // and MakeDisplayTile, scoped to ONE form; returns false whenever it is
    // not CERTAIN (stackables, typed-bag claims, no room, shape drift) and
    // the caller then runs the full rebuild -- correctness is unchanged, the
    // fast path only covers what it can prove. Every decline logs its reason
    // ("[B3]"), so coverage is measured, not assumed. Main thread only.
    // (Named OnEngineUnequip when the unequip event was its only caller; the
    // ContainerSink now routes EVERY player-side container delta through it,
    // making it the delta applier §8-10 aimed at -- scoped to one form, adds
    // only, everything else declines into the rebuild.)
    bool OnFormDelta(std::uint32_t a_form);
    // Trace switch: "!rbtrace = 1".
    [[nodiscard]] bool RebuildTrace();
    void               SetRebuildTrace(bool a_on);
    // ★TEST ONLY: "!rbdrop = Equip.cpp:890, Grid.cpp:3323" -- a comma separated
    // list of call sites whose rebuild requests are IGNORED.
    //
    // This is the instrument B3 actually needs. The board rebuilds because
    // seventy-five places ask it to, and the honest question about each one is
    // "what breaks if it does not". Answering that by deleting the call and
    // rebuilding the dll is slow and destructive; answering it from an ini line
    // costs a restart and leaves the code untouched. What breaks IS the answer:
    // it names exactly what that rebuild was maintaining.
    void SetRebuildDrop(const char* a_csv);
    // ★B3-a: an engine request that was never confirmed. Registered with the
    // Ledger at startup; see Ledger::SetOnExpire.
    void OnRequestExpired(std::uint32_t a_form, std::int32_t a_delta,
                          const char* a_who, const std::string& a_slot);
    // ★B3-b: a gear slot is no longer dropped when the request goes out -- it
    // is dropped when the engine CONFIRMS, and kept when it does not.
    // ★Both act on the CONFIRMED REQUEST'S OWN key, never "the front of the
    // form's queue": count-based popping let a world drop's confirmation eat
    // a pending store's key whenever two paths moved the same form. An empty
    // or unknown key is a no-op -- stackables never queue one.
    void CommitSlotDrop(std::uint32_t a_form, const std::string& a_slot);
    void CancelSlotDrop(std::uint32_t a_form, const std::string& a_slot);

    // Light path for rotation/scale edits: re-resolve defs + queue captures
    // WITHOUT re-placing the grid (footprints unchanged — no reflow, no IO).
    void RefreshDefs();

    // D3: equipping forgets the saved grid spot (fresh-pickup re-entry).
    // Rule 13: forget ONE tile's remembered cell (equip / sell / store / drop).
    void ForgetTile(const std::string& a_key);

    // Hide a unit whose EQUIP is queued for the next Tick, so it does not
    // flicker back into the cell it just left. NOT NotePendingRemove: that
    // resolves on a stock-count drop, which equipping never causes.
    // a_srcKey: the board tile the unit left from ("" if it came off the doll).
    // The pool holds that cell open until the engine applies the equip.
    // a_units: how many the equip takes. 1 for anything worn a copy at a time;
    // the whole tile for ammo, which is equipped by the quiverful.
    // a_xlIdx: where the unit sits in the entry's extraLists. The one handle
    // that cannot drift while the equip is in flight -- see the exclusion
    // ladder. -1 for a listless (plain) unit, which is the honest answer.
    void NotePendingEquip(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                          std::uint16_t a_sig, int a_hand = 0,
                          const std::string& a_srcKey = {}, int a_units = 1,
                          int a_xlIdx = -1);
    // Right-click on a book or note: show it in the game's OWN Book Menu.
    // Queued here, opened on the Tick — the menu must not be raised from
    // inside the render pass. While it is up, UIRoot stands down completely
    // (no draw, no input) so the book is visible and closable.
    void RequestBookRead(RE::TESObjectBOOK* a_book, std::uint16_t a_uid, std::uint16_t a_sig);
    void ProcessBookRead();   // UIRoot::Tick

    // GI32: apply queued favourite toggles. MUST run on the game thread --
    // the native SetFavorite refuses to add a second favourite from inside the
    // render pass.
    void ProcessFavorites();
    // Queue one unit's favourite toggle, named by uid+sig rather than by a
    // board position -- what the equipment doll and the accessory drawer have.
    void ToggleFavoriteUnit(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                            std::uint16_t a_sig, int a_xlIdx = -1);
    // ★B3: did a CLICK already take this form's tile off the board? The equip
    // event sink asks before deciding what a declined partial update means --
    // an equip from the wheel, a hotkey or a script has no click behind it and
    // therefore nothing that removed the tile. Claiming clears the record.
    [[nodiscard]] bool ClaimOptimisticRemove(RE::FormID a_form);
    void ClearPendingEquips();   // menu close / reset
    // ★A confirmed CONSUME releases its suppression entry at once. The entry
    // used to die only at the end of the rebuild it covered, and by then the
    // engine's count had already dropped -- for that one rebuild the stack was
    // subtracted twice, and at "last unit" the reconciler drained the slot to
    // zero and ERASED it, so the survivor re-entered at the front gap. Worn
    // equips never had the overlap (released mid-walk by the worn list);
    // consumables have no worn list, and the ledger's "use" confirmation is
    // their equivalent signal. Called from the Ledger OnConfirm delivery
    // (main thread, via Tick/Flush).
    void ReleaseAppliedPendingEquip(std::uint32_t a_form);
    // Equip::ProcessPending: the engine has run the queue. The next
    // rebuild consumes these and stops suppressing them.
    void MarkEquipsApplied();

    // C6: Equip::DrawSlot reports the hovered slot while an item is carried;
    // a left-click that frame tries to equip onto it.
    void NotifySlotDropTarget(const std::string& a_slotId);

    // Collect the player's inventory, place items (saved spots -> first-fit,
    // grid seniority), persist new placements, queue icon captures.
    // ★B3-c: a DIRECT rebuild bypasses the request gate entirely, so it has
    // to name itself the same way. These are the calls that showed up as
    // "(no ask?)" -- they were never wrong, just invisible.
    void Rebuild(const std::source_location& a_where =
                     std::source_location::current());

    // ★B4-1: the menu-open rebuild, made conditional. Rebuilds only when the
    // flag is up (every closed-menu count delta raised it through its event;
    // the census gate covers the event-less value drifts) or when the board
    // is empty (first open, post-load resets). A quiet open keeps the board
    // exactly as the player left it -- and keeps every ladder idle.
    void RebuildIfNeeded(const std::source_location& a_where =
                             std::source_location::current());

    // ★B4-2c: the engine confirmed an equip of this form (TESEquipEvent,
    // equipped == true) -- lands the oldest arriving pending-equip entry.
    // This is the worn-clock's authority now; called from the sink's task.
    void NoteEquipLanded(RE::FormID a_form);

    // ★...and its closing half: an unequip event retires the oldest LANDED
    // entry of the form -- arrived and left again, story over. See the
    // implementation note for the rapid-swap orphan it prevents.
    void ReleaseLandedPendingEquip(RE::FormID a_form);

    // Capacity system: true when a_obj could be added right now — it stacks
    // onto an existing tile, or its footprint first-fits into the HARD
    // 10 x 14 main board after every current occupant placed (bag-assigned
    // items consume their bag, not main). Headless: no UI, no persistence.
    [[nodiscard]] bool CanFitNewItem(RE::TESBoundObject* a_obj);

    // Phase 7: how many units (<= a_want) the inventory can accept right now —
    // partial-stack room + new tiles on the hard board + new tiles in open
    // bags. Stack buy/take sliders clamp their max to this.
    [[nodiscard]] int MaxAcceptUnits(RE::TESBoundObject* a_obj, int a_want);

    // Post-add capacity check (container-take bounce): the item is ALREADY in
    // the inventory — place everything on the hard board and report whether
    // a_obj's own tile failed to fit (stacking onto an owned tile never does).
    [[nodiscard]] bool WouldOverflow(RE::TESBoundObject* a_obj);

    // W1+W2: capacity-based encumbrance. Weight never limits the player (big
    // CarryWeight buffer); instead, when the hard board can't hold everything
    // (shop / console / scripted AddItem bypass the pickup gate), CarryWeight
    // is pushed below the inventory weight so the VANILLA over-encumbrance
    // (forced walk, no fast travel) kicks in.
    [[nodiscard]] bool IsOverloaded();   // S2 reads this for the crimson space value
    void MarkCapacityDirty();            // inventory/equip/loadout changed — recompute
    void CapacityTick();                 // per-frame: recompute when dirty + enforce CW

    // S2: stats panel "Space X / Y" — used cells on the main board (from the
    // last Rebuild; can exceed the total while overloaded) and the hard cap.
    // S2 stats panel "Space X / Y" — and the ONE source for "how much room is
    // left" (take-all budget included). Both halves span the main board plus
    // every owned bag, open or closed; the trash is storage for neither.
    [[nodiscard]] int SpaceUsed();
    [[nodiscard]] int SpaceTotal();
    [[nodiscard]] int CellSpanOf(RE::TESBoundObject* a_obj);   // grid cells an item occupies
    // shift+lclick split -> held fragment. a_srcKey = the tile it leaves.
    // a_srcTotal (coins only) = the source tile's full gold value, so an AUTO
    // coin tile is converted whole-to-pin before the split (siblings untouched).
    void PickupPartial(RE::TESBoundObject* a_obj, int a_count,
                       const std::string& a_srcKey, int a_srcTotal = 0);

    // Draw the main tetris grid inside the current ImGui window.
    void Draw();

    [[nodiscard]] int GoldAmount();   // v9: UIRoot draws the GOLD bar

    // B: report gold spent by a barter purchase this frame. The next Rebuild's
    // spill pass adds back the coin tiles the payment dissolved, so the bought
    // item spills into a bag instead of reusing the freed cells.
    void NotePaidGold(int a_price);

    // Phase 7: sold/stored units whose engine removal is still queued on the
    // transfer Tick. The rebuild subtracts them immediately and drains the
    // NAMED tile in place (a_key may be empty for carried fragments). Clear is
    // called right after the engine RemoveItem lands; ClearAll on window close.
    // a_xlIdx: WHERE the outgoing unit sits in the engine entry, when the
    // caller knows (-1 = it does not). The pool name and the count cannot
    // say which of several look-alike units is leaving, and a signature
    // that moves mid-flight leaves the exclusion nothing to match -- the
    // list position cannot move when the values inside it do.
    void NotePendingRemove(RE::TESBoundObject* a_obj, const std::string& a_key,
                           int a_count, int a_xlIdx = -1);
    void ClearAllPendingRemoves();

    // ★B4-4: take one tile off the display -- the ring router's partial exit
    // (its tail rebuild repainted the swap window for one tile's removal).
    // The layout half is ForgetTile's, called by rule 13 at the same site.
    void DropTileDisplay(const std::string& a_key, RE::TESBoundObject* a_obj);

    // ★B4-4: a carrier-route equip LANDS at DualRing::Wear -- no engine equip
    // event will ever come for the ring itself, so nothing else can retire
    // its equip-queue entry. Left in the books, the entries a swap spam piles
    // up each kept excluding one unit of the form until the TTL swept them,
    // and an innocent same-form spare in the pack blinked (user report).
    // Retires the oldest arriving entry of the form.
    void ReleasePendingEquipFor(RE::FormID a_form);
    // B2: expire the partner-drop placement hint (qty slider cancelled/closed)
    void ClearDropHint();

    // I1: rich tooltip — name + damage/armor/effects + weight/value.
    // Shared by grid tiles and equip slots; call while the item is hovered.
    // a_coinValue >= 0 adds a highlighted gold line (G2: coin tiles show the
    // amount they represent; the pouch shows its stored amount).
    // a_price >= 0 (Phase 4) adds a barter price line — Buy (a_isBuy) or Sell.
    //
    // GI1/D1: a_owner names WHOSE inventory the per-instance extras come from.
    // It used to be hard-wired to the player, so hovering a merchant's ordinary
    // steel sword printed the poison / charge / temper / enchant of the player's
    // OWN steel sword. nullptr keeps the player default.
    //
    // a_scope names WHICH sub-stack of that entry the tooltip describes:
    //   kUnit — exactly the one at (a_uid, a_xlIdx): a player grid tile, one unit
    //   kWorn — the sub-stack on the body: the equipment doll
    //   kAny  — the first sub-stack carrying each trait: aggregate partner cells,
    //           which are one cell per FORM with a stack badge (by design)
    // The item-def key for a form ("Skyrim.esm|0x0139B9"). The 3D inspect view
    // files an adopted angle/scale under this key, so anything that opens the
    // view has to name the item the same way the editor does.
    [[nodiscard]] std::string DefKeyOf(RE::TESForm* a_form);

    enum class ExtraScope { kUnit, kWorn, kAny };

    // GI50: what the hovered TILE is, as opposed to what the item is. The
    // tooltip needs this to name the right-click action (it differs per tile
    // kind and per mode) and to say why an action would be refused. None of it
    // is derivable from the object — the drawing code already knows it, so it
    // hands it over rather than making the tooltip guess.
    struct TileContext
    {
        std::string_view key;
        bool             isBag    = false;
        bool             parked   = false;   // sitting in the trash
        bool             partner  = false;   // the other side's grid
        bool             equipSlot = false;  // equipment doll, not a grid cell
        // ★These used to be looked up FROM `key`, in a cache keyed by the
        // item's mutable state. The tile already knows -- it read them off its
        // own live sub-stack this rebuild -- so it simply says so.
        // (Appended last: the five-element aggregate initialisers stay valid.)
        bool             stolen   = false;
        bool             quest    = false;
    };

    // The cell lattice for a cols x rows board at `base` — tiles, a carve or a
    // hairline, whichever the skin says. Partner windows call this so both
    // halves of the screen show the same board.
    void DrawCellLattice(ImDrawList* a_dl, const ImVec2& a_base, int a_cols, int a_rows);

    // ★The occupied-cell wash for ONE cell, with the insets the lattice uses:
    // half a groove where a neighbouring cell continues, nothing at the board
    // edge, both scaled. Lives here because the rule belongs to the grid --
    // the shelf-bag window used to approximate it with a flat 1px margin and
    // sat a hair off the lattice for it.
    void ShadeCell(ImDrawList* a_dl, const ImVec2& a_base, int a_col, int a_row,
                   int a_cols, int a_rows, ImU32 a_col32);

    // ★The ink skin's lattice, drawn AFTER the occupied ground rather than
    // under it -- see the definition for why "the ground steps around the
    // divider" cannot work for a mark that snaps to whole pixels.
    void DrawInkLattice(ImDrawList* a_dl, const ImVec2& a_base, int a_cols, int a_rows);

    void DrawItemTooltip(RE::TESBoundObject* a_obj, int a_count, int a_coinValue = -1,
                         int a_price = -1, bool a_isBuy = false,
                         RE::TESObjectREFR* a_owner = nullptr,
                         ExtraScope a_scope = ExtraScope::kAny,
                         std::uint16_t a_uid = 0, int a_xlIdx = -1,
                         // kWorn only: WHICH worn unit. With one copy in each
                         // hand "the worn list of this form" is ambiguous, so the
                         // doll passes the identity its slot recorded.
                         std::uint16_t a_sig = 0, int a_hand = 0,
                         const TileContext& a_tile = {});

    // ★An item's name as the GAME would print it. Quest items name themselves
    // through their own extra data, not their base form: a Missives note's FULL
    // is literally "Missive: Deliver Weapon to <Alias=Destination>" and the
    // engine substitutes the alias per instance only when asked via the
    // ExtraDataList. Reading TESForm::GetName() showed users the raw token.
    // Player-renamed items resolve through the same call, so this is also the
    // one place that has to know about custom names.
    //   a_xl    — the sub-stack being described (nullptr if none/aggregate)
    //   a_entry — fallback for an AGGREGATE cell ONLY: the first named
    //             sub-stack, which is what the vanilla item card shows for a
    //             merged row.
    // ★Do NOT pass an entry when the caller is describing ONE unit. A null
    // a_xl then means "this unit carries no extra data", and the right answer
    // is the base name — the entry's name belongs to whichever sub-stack
    // happens to be first, which is how three plain daggers all came out
    // "Fine Dagger" after one of them was tempered.
    [[nodiscard]] const char* DisplayNameOf(RE::TESBoundObject* a_obj,
                                            RE::ExtraDataList* a_xl = nullptr,
                                            RE::InventoryEntryData* a_entry = nullptr);

    // Item base value for barter pricing (InventoryEntryData::GetValue cached
    // at Rebuild). -1 if unknown / not a priceable item.
    [[nodiscard]] int ItemValue(RE::TESBoundObject* a_obj);

    // ★★★1.0.5 — rarity is the CELL'S GROUND, not a halo over it.
    //
    // Every halo variant failed the same way and it is worth writing down why,
    // because the failure looks like a tuning problem and is not one. A halo is
    // translucent, so what reaches the screen is the panel plus a little
    // colour. Against cream parchment (189,174,147) the blue halo lands at
    // (157,170,182): the hue moves a lot and the LUMINANCE moves by four. The
    // shape is iso-luminant with its background, the eye gets colour but no
    // form, and every fix that stays translucent — more alpha, a black copy
    // underneath, a vignette around it — is still negotiating with whatever is
    // behind it.
    //
    // Filling the cell stops negotiating. At 78% the ground is the same colour
    // on parchment as on a dark panel (82,108,161 vs 45,75,134 — both plainly
    // blue), so the skin, the daylight behind a glass panel, and the icon's own
    // brightness all stop being variables. This is what inventory grids that
    // must be read at a glance do; the halo was the wrong instrument.
    //
    // ★★1.0.5 — the state markers along a tile's bottom edge, in ONE place.
    //
    // There were three hand-written copies: the grid's tray, the doll's lone
    // favourite diamond (at its own 6.5px radius) and the droplet that used to
    // ride inside DrawGlow. They disagreed on size, on colour and on where
    // poison went, so an item looked different depending on which window it
    // was in — and on the doll the droplet landed on top of the diamond.
    //
    // Order, right to left: stolen ● · poison 💧 · favourite ◆.
    //
    // ★Every marker is bounded by the SAME square. The droplet is narrower
    // because its point makes it tall, so its width is derived from the shared
    // height instead of set alongside it; the diamond lost the 1.15 vertical
    // stretch it used to have. Three of them plus the gaps now measure
    // 4 + 8 + 3 + 8 + 3 + 8 = 34px against a 38.4px cell, so the full set fits
    // inside one tile — it did not before (40px), and spilled onto the
    // neighbour.
    void DrawMarkerTray(ImDrawList* a_dl, const ImVec2& a_boxMin, const ImVec2& a_boxMax,
                        bool a_fav, bool a_stolen, bool a_poisoned,
                        bool a_worn = false);

    // ★★1.0.5 — the item's own drop shadow, so a pale sprite does not vanish
    // into a pale panel. Cream parchment sits at 177,160,131 once the occupied
    // overlay is on it, and polished steel is brighter than that: the icon had
    // colour but no edge, the same iso-luminance trap the rarity halo fell
    // into. A dark halo behind the sprite gives it one at every skin.
    //
    // ★★Three settings, and they are the three a drop shadow has anywhere else:
    // DISTANCE (px toward the lower right, 0 = ambient), BLUR (px of spread,
    // 0 = a hard silhouette), OPACITY. See Theme::ShadowDist / Blur / Opacity.
    //
    // ★It draws the ICON'S OWN SPRITE in black, stamped repeatedly on a ring —
    // no separate texture. An earlier build sampled a silhouette baked for the
    // retired rarity halo, but that sprite lives on a 96px canvas: the capture
    // was downsampled to it, blurred, then stretched back up to the tile, and
    // the round trip put a floor under the softness that no radius could get
    // below. Stamping runs at full capture resolution, so blur 0 really is the
    // exact outline. Consecutive stamps share a texture and merge into one
    // draw command, so the cost is vertices rather than draw calls.
    //
    // Callers pass the sprite geometry they are ABOUT to draw with, so the
    // shadow can never drift out of register with the icon.
    void DrawItemShadow(ImDrawList* a_dl, void* a_srv, const ImVec2& a_centre,
                        float a_dw, float a_dh, float a_deg);

    // ★★★1.0.5 — rarity is a CORNER WEDGE, and nothing else.
    //
    // Everything that tried to colour the item's own area failed, and the
    // reason is worth keeping: a translucent halo is iso-luminant against a
    // light panel, and an opaque ground turns the metadata into the loudest
    // thing on the tile. Both were also fighting each skin's palette, because
    // blue simply does not exist in a parchment world.
    //
    // A wedge sidesteps all of it by being SMALL. At roughly a quarter of a
    // cell a colour reads as an accent rather than as wallpaper, so it can sit
    // outside the skin's palette without looking foreign, and the icon keeps
    // its own ground. It also costs two triangles.
    //
    // Drawn once per ITEM at the footprint's top-right, not per cell.
    // ★a_relic (1.4.4) rides the same wedge: a relic still owed to the museum
    // takes the colour outright, a donated one hands it back to the item's own
    // rarity, and a donated item with no rarity gets the grey. Full reasoning
    // at the implementation.
    void DrawRarityWedge(ImDrawList* a_dl, const ImVec2& a_boxMin,
                         const ImVec2& a_boxMax, std::uint8_t a_haloBits,
                         Lotd::Status a_relic);

    // Rarity glow, shared with the partner (loot/barter) window so its items
    // glow exactly like the player grid's.
    //   bit1 = enchanted (EITM or crafted ExtraEnchantment)  -> halo
    //   bit2 = unique (DESC, cached)                         -> halo
    //   bit4 = poisoned                                      -> corner droplet
    // ★Only bits 1|2 are a HALO. Bit 4 is a marker and must never reach the
    // rarity switch, or a poison-only item takes the "both rarities" red.
    // (There was a bit 8 for temper; nothing marks temper any more -- it is in
    // the name, the damage number and the price, and by mid-game it is on
    // nearly every weapon, so the mark carried no information.)
    // DrawGlow paints the tinted silhouette halo mapped onto the DRAWN icon rect
    // (radial across the cell box as the style-0 / no-sprite fallback).
    // GI1/D2: a_xl names the SUB-STACK being drawn -- pass nullptr for a plain
    // unit (one with no ExtraDataList of its own) or when there is no entry.
    // Before GI1 this scanned the whole entry, so a single enchanted sword in a
    // stack of three made all three glow.
    [[nodiscard]] std::uint8_t GlowBits(RE::TESBoundObject* a_obj,
                                        RE::InventoryEntryData* a_entry,
                                        RE::ExtraDataList* a_xl);

    // ★★"Is this a ring?" -- in ONE place, because it was four: the doll slot,
    // the item category, the fallback icon and bag eligibility each asked
    // HasPartOf(kRing) on their own.
    // ★That bit can only ever answer for ONE worn ring. BipedAnim is
    // `BIPOBJECT objects[kTotal]` -- a single item per slot -- so however a
    // multi-ring mod is built, exactly one of the rings can hold kRing(6) and
    // every other one must sit on a custom slot. The four copies therefore
    // agreed on the first ring and quietly disagreed about the rest: filed as
    // a nondescript accessory, drawn with the wrong icon, and allowed into a
    // bag that refuses jewellery.
    // ClothingRing is the ring's OWN keyword -- unlike ArmorJewelry, which
    // amulets carry too -- so it answers for all of them.
    // ★HasKeywordID compares FormIDs directly: no form to look up, no
    // data-handler timing to get wrong, safe on a per-frame path.
    [[nodiscard]] inline bool IsRing(const RE::TESObjectARMO* a_armo)
    {
        constexpr RE::FormID kClothingRing = 0x0010CD09;   // Skyrim.esm
        return a_armo &&
               (a_armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kRing) ||
                a_armo->HasKeywordID(kClothingRing));
    }

    // GI1: one entry's units, in a stable order, each bound to the sub-stack it
    // belongs to. uid = ExtraUniqueID (0 when the engine assigned none),
    // xlIdx = position in entry->extraLists (-1 = a plain unit with no list).
    struct UnitRef
    {
        std::uint16_t uid = 0;     // ExtraUniqueID, 0 = the engine assigned none
        std::uint16_t sig = 0;     // GI14 content signature, 0 = a plain unit
        int           xlIdx = -1;  // position in entry->extraLists, -1 = plain
        // GI41: what the WALK knew and used to throw away. Asking again later
        // means asking by xlIdx, and a position stops being true the moment a
        // list is added or removed -- planting an item on a pickpocket mark
        // moved the "worn" answer onto a different cell, so the lock jumped to
        // an item the target was not wearing. Carry it instead.
        bool          worn = false;
        int           hand = 0;    // 1 right, 2 left (0 = not worn)
    };

    // Walk an entry into per-unit refs. a_skipWorn=false keeps the body-worn
    // unit (corpses and pickpocket targets show what the NPC wears).
    void EnumerateUnits(RE::InventoryEntryData* a_entry, int a_count,
                        std::vector<UnitRef>& a_out, bool a_skipWorn = true);

    // Units a single tile/cell may hold: 1 for gear and coins, else the
    // category cap (with the editor's per-item stack:N override applied).
    [[nodiscard]] int StackCap(RE::TESBoundObject* a_obj);

    // ★P2/3-1: is this form a BAG? Asked from outside the grid because a bag is
    // now the one worn thing that keeps its tile, so rule 13 ("equipping forgets
    // the cell") has to make an exception for it -- a bag that never left the
    // board never left its cell either.
    [[nodiscard]] bool IsBagForm(RE::TESBoundObject* a_obj);

    // The worn sub-stack of an entry (equipment doll: the unit on the body).
    // a_hand: 0 = either, 1 = RIGHT (ExtraWorn), 2 = LEFT (ExtraWornLeft). The
    // hand is what separates two copies of one form worn at the same time.
    [[nodiscard]] RE::ExtraDataList* WornExtraOf(RE::InventoryEntryData* a_entry,
                                                 int a_hand = 0);
    [[nodiscard]] RE::ExtraDataList* WornExtraMatching(RE::InventoryEntryData* a_entry,
                                                       std::uint16_t a_uid,
                                                       std::uint16_t a_sig,
                                                       int a_hand = 0);

    // GI1: the engine's OWN entry for a form in a container (GetInventory hands
    // out copies, so anything mutated through those is silently discarded).
    [[nodiscard]] RE::InventoryEntryData* LiveEntryOf(RE::TESObjectREFR* a_owner,
                                                      RE::TESBoundObject* a_obj);

    // GI1: the sub-stack named by (uid, xlIdx). uid wins when the engine
    // assigned one; otherwise the recorded position, revalidated against the
    // CURRENT list. nullptr = a plain unit, or that instance is gone.
    // NEVER store the result -- the engine reallocates and frees these.
    [[nodiscard]] RE::ExtraDataList* ExtraForInstance(RE::InventoryEntryData* a_entry,
                                                      std::uint16_t a_uid, int a_xlIdx);

    // GI25: resolve by POOL (uid, else content signature) and never by list
    // position. Transfers must use this: they are queued and run a frame or more
    // later, by which time a captured position can be stale.
    [[nodiscard]] RE::ExtraDataList* ExtraForPool(RE::InventoryEntryData* a_entry,
                                                  std::uint16_t a_uid, std::uint16_t a_sig);

    // GI36: resolve the sub-stack that is really LEAVING the bag and strip its
    // favourite star in the same call. Outbound sinks (sell / store / plant /
    // trash / world drop) call this instead of ExtraForPool and pass the result
    // straight to RemoveItem. Keeping resolution and star-removal together is
    // the point: clearing by pool name stripped every look-alike's star.
    //   a_starred = how many of the a_count outgoing units wore a star (0 = none).
    //               The caller always knows this; the sink never can.
    // Returns nullptr for "let the engine pick", only within a pool whose members
    // are genuinely interchangeable.
    [[nodiscard]] RE::ExtraDataList* ResolveExitUnit(RE::TESBoundObject* a_obj,
                                                     std::uint16_t a_uid,
                                                     std::uint16_t a_sig,
                                                     int a_count, int a_starred,
                                                     // ★a_xlIdx: WHERE the unit
                                                     // the player pointed at sits.
                                                     // A pool is a crowd -- without
                                                     // this, whichever list comes
                                                     // first leaves, so selling the
                                                     // front dagger sold the back
                                                     // one. Verified before use; a
                                                     // stale position simply falls
                                                     // back to the pool rule.
                                                     int a_xlIdx = -1);

    // GI42: how a transfer NAMES the unit it moves. RemoveItem's a_extraList is
    // the only selector the engine has, and nullptr is not a fallback -- it is
    // "engine, pick one". That is only legal when every unit the engine could
    // pick is interchangeable with the one that was clicked, under both content
    // (sig/uid) and the rules in force (may a worn unit leave the body?).
    // The pickpocket lock bypass came from conflating "could not resolve" with
    // "no list, any unit is fine" -- both were nullptr.
    enum class PickKind : std::uint8_t
    {
        kNamed,       // this exact ExtraDataList
        kAnyIsSafe,   // no list: every candidate identical -- nullptr is PROVEN safe
        kFallback,    // nullptr, unproven: content look-alikes exist (tolerated + logged)
        kUnresolved   // a WORN unit the rules forbid could be grabbed -- do not move
    };
    struct UnitChoice
    {
        PickKind           kind = PickKind::kUnresolved;
        RE::ExtraDataList* xl = nullptr;
        [[nodiscard]] bool ok() const { return kind != PickKind::kUnresolved; }
    };
    // The proof, three lines: any worn list + illegal => kUnresolved; any list
    // whose content differs from a plain unit => kFallback; else kAnyIsSafe.
    // Needs NO knowledge of the engine's selection order -- that is the point.
    //   a_nameWorn : may the worn list be NAMED (only for a request from a worn cell)
    //   a_wornLegal: may a worn unit legally leave (loot/steal, or perk in pickpocket)
    [[nodiscard]] UnitChoice PoolChoice(RE::InventoryEntryData* a_entry,
                                        std::uint16_t a_uid, std::uint16_t a_sig,
                                        bool a_nameWorn, bool a_wornLegal);

    // GI43: the ENGINE's own price for ONE unit. A throwaway stack entry points
    // at the unit's REAL list and is priced by the same native the vanilla
    // barter row uses, so temper folds in exactly like vanilla (10 -> 11 at
    // +10%) with no formula of ours. The single-element BSSimpleList lives
    // entirely on the stack (head node is embedded -- no allocation) and the
    // list is DETACHED before the entry is destroyed, so no engine-owned
    // memory is ever touched by a destructor of ours.
    // a_xl == nullptr (a plain unit / an aggregate) falls back to the base value.
    [[nodiscard]] int UnitValueWith(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_xl);

    // (A NON-PLAYER source needs the same answer with worn lists ALLOWED — the
    // partner board deliberately shows worn gear, so a take there has to be
    // able to name it. That is PoolChoice's a_nameWorn, not a resolver of its
    // own: the standalone ExtraForPoolOnPartner wrapper was left with no
    // callers once PoolChoice absorbed the case, and went in 1.0.5.)

    // Content signature of a sub-stack (0 = none). Stable across container moves.
    [[nodiscard]] std::uint16_t InstanceSigOf(RE::ExtraDataList* a_xl);
    // GI62: a_rot = the tile's quarter-turn, so the SILHOUETTE halo lies at the
    // same angle as the sprite it was cut from (the radial style is symmetric).
    void DrawGlow(ImDrawList* a_dl, RE::TESBoundObject* a_obj, std::uint8_t a_bits,
                  const ImVec2& a_iconMin, const ImVec2& a_iconMax,
                  const ImVec2& a_boxMin, const ImVec2& a_boxMax, int a_rot = 0);

    // Phase 2 shared cell renderer: count/value badge hugging the tile's
    // top-left corner, full black outline (player tiles + partner grid).
    void DrawCountBadge(ImDrawList* a_dl, const ImVec2& a_tileMin, const char* a_text);

    // ★(1.3.1) soul-gem recharge (hover + T -- the vanilla ChargeItem key):
    // a popup lists the player's filled soul gems; picking one recharges the
    // hovered enchanted weapon. Worn units included -- while equipped the
    // charge is drained as DAMAGE on the hand's item-charge AV, so the
    // restore goes through RestoreActorValue and the ExtraCharge both.
    void OpenRecharge(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                      std::uint16_t a_sig, bool a_worn, int a_hand);
    void DrawRechargeWindow();          // top level, pouch-window pattern
    [[nodiscard]] bool IsRechargeOpen();
    bool CloseRecharge();               // I/ESC layering
    void ProcessRecharge();             // UIRoot::Tick -- engine mutations

    // ★(1.3.1) shelf-bag intake: the held PLAYER-side item, when it may be
    // stored into a shelf bag (not a bag itself, not a coin tile, not
    // quest-locked). nullptr otherwise -- peek before committing.
    [[nodiscard]] RE::TESBoundObject* HeldShelfStorable();
    // Commit it into the open shelf bag: queue the store, note the pending
    // remove, drop the carry. Fills the bundle entry's identity. False when
    // nothing eligible rides the cursor.
    bool CommitHeldToShelfBag(RE::FormID& a_form, int& a_count,
                              std::uint16_t& a_sig, int& a_rot,
                              std::uint8_t& a_glow, bool& a_stolen);
    // ★(1.3.2) the carry's quarter-turns (0..3) -- the shelf-bag window
    // seats and draws its entries at the angle they were dropped at.
    [[nodiscard]] int HeldRot();
    // ★(1.3.2a) put a withdrawn shelf-pouch amount on the cursor as a
    // pinned purse (player-pouch withdraw grammar; excess stays walking).
    void CarryWithdrawnGold(int a_value);
    // The whole carry, for the shelf-bag window's partner-side intake:
    // false = nothing held. DropHeldForShelf resets the carry WITHOUT any
    // engine transfer (the item never left the container).
    bool PeekHeldForShelf(RE::TESBoundObject*& a_obj, int& a_count,
                          std::uint16_t& a_sig, int& a_rot, bool& a_fromPartner);
    void DropHeldForShelf();

    // G2: coin-pouch withdraw window (slider) — top level, settings pattern.
    void DrawPouchWindow();
    // GI63: the withdraw window takes the same keys as the quantity slider, so
    // the prompt bar has to know it is up (it is not a LootBarter slider).
    [[nodiscard]] bool IsPouchOpen();
    bool ClosePouch();   // I/ESC layering: close the withdraw window if open

    // F2: trash window — a 6x4 virtual bag view ("__trash") holding items
    // PARKED for deletion. Parked items stay in the engine inventory and
    // occupy no board space; deletion is confirmed when the window (or the
    // whole menu) closes, oldest-first when the board needs room (FIFO).
    [[nodiscard]] bool IsTrashOpen();
    void ToggleTrash();          // the trash-can button
    bool CloseTrash();           // I/ESC layering: confirm-all + close if open
    // ---- find by name -------------------------------------------------------
    // ★Deliberately NOT a filter. Matching tiles keep their place and the rest
    // dim, because a grid inventory's whole premise is that you remember where
    // you put things — hiding the misses would erase the board you learned.
    // The term is folded to lower case on the way in; matching is a substring.
    [[nodiscard]] bool SearchActive();
    [[nodiscard]] const std::string& SearchTerm();
    void SetSearch(const char* a_term);
    bool ClearSearch();   // I/ESC layering: false when nothing was set
    // true = this tile does NOT match, so the caller should dim it. Cheap: the
    // name comparison ran once per board change, this only asks a set.
    [[nodiscard]] bool SearchMisses(const std::string& a_key);
    // ★The raw test, for boards this module does not own (the partner window
    // rebuilds its cells every frame, so it caches by FORM instead of by key).
    // Returns true when no term is set, so callers need no special case.
    // Not free — it lower-cases the name — so cache the answer, never call it
    // once per tile per frame.
    [[nodiscard]] bool SearchMatches(const char* a_name);

    // ★The close-order stack needs to ASK, not just tell: it records the order
    // layers opened in, which means observing each one every frame.
    [[nodiscard]] bool IsTrashConfirmOpen();
    bool CloseTrashConfirm();    // I/ESC layering: dismiss the favorite ask
    void DrawTrashConfirm();     // favorite-intake confirm popup (top level)
    void ProcessTrashDeletes();  // UIRoot::Tick — engine RemoveItem, deferred

    // Draw one managed window per OPEN bag (call from UIRoot::Render after
    // the main window). E1~E5: multi-open, remembered, fixed bw x bh grids.
    void DrawBagWindows();

    // End-of-frame: held-item cursor icon, drop/swap/discard input, deferred
    // rebuilds. Call after every grid/window drew, before WinManager::Update.
    void FinishFrame();

    // Per-save persistence of placements + open bags (SKSE cosave). The global
    // layout ini caused cross-save contamination (any load showed the LAST
    // session's arrangement); it remains only as a one-shot legacy fallback
    // for saves that predate the 'GLAY' record. main.cpp owns the record loop
    // and dispatches by type.
    inline constexpr std::uint32_t kRecordType = 'GLAY';
    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame(SKSE::SerializationInterface* a_intfc);
    void MarkLayoutFresh();   // kNewGame: start empty, skip the legacy-ini fallback

    // GI65: the menu is closing — everything on the board counts as seen, and
    // the counts become the baseline the next opening is measured against.
    // ★Shelf USE MODE: this form is arriving only to be consumed, so typed-bag
    // routing must leave it alone (see g_transientArrivals).
    void NoteTransientArrival(RE::FormID a_form);

    void NoteInventorySeen();

    // ★One form counts as seen. EQUIPPING something is a stronger "I have
    // looked at this" than hovering it, so the NEW mark has no business
    // surviving it.
    // ★★It raises the BASELINE, not just the mark. Clearing the mark alone
    // would not hold: equipping takes the tile off the board, and unequipping
    // brings back a tile the new-item test has never seen, so the mark simply
    // comes back. The baseline is what that test compares against.
    void NoteFormSeen(RE::TESBoundObject* a_obj);

    // Grid pixel metrics (main window sizes itself around these).
    inline constexpr int   kCols    = 10;
    inline constexpr int   kMinRows = 14;
    inline constexpr float kCell    = 48.0f;   // base cell at UI scale 1.0

    // H′: every layout metric goes through the global UI scale.
    // ★Two scales: Theme::Scale() is the UI's, CellScale() is the board's.
    // Everything measured in CELLS — the grid, the equipment slots, the item
    // pictures — rides both; text, buttons and padding ride only the first.
    [[nodiscard]] inline float CellPx()
    {
        return kCell * Theme::Scale() * Theme::CellScale();
    }

    // GI62: the drawn-icon nudge (`fx`, in cells) is authored for an UPRIGHT
    // tile, where "off-centre" means sideways. Turn the tile and the nudge has
    // to turn with it, or a rotated bow drifts along the wrong axis.
    // ★Lives in the header because BOTH boards draw rotated sprites -- the same
    // rule written twice is the same rule until the day one copy is edited.
    // ★Two axes now. The one-axis form turned (cells, 0) by the tile's rotation;
    // this turns (x, y) by the same quarter turns, and reduces to the old
    // results exactly when y is 0 -- rot 1 sent (c,0) to (0,c), which is what
    // (-y, x) gives.
    [[nodiscard]] inline ImVec2 RotatedOffset(float a_x, float a_y, int a_rot)
    {
        const float px = a_x * CellPx();
        const float py = a_y * CellPx();
        switch (a_rot & 3) {
        case 1:  return ImVec2(-py, px);
        case 2:  return ImVec2(-px, -py);
        case 3:  return ImVec2(py, -px);
        default: return ImVec2(px, py);
        }
    }
}
