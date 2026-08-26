#pragma once

#include <functional>
#include <string>
#include <vector>

// G1: gold as physical coin items (Mabinogi style). Gold001 stays the hidden
// LEDGER (shops/quests keep working untouched); the player's visible gold is
// MIRRORED into real MISC items from Grid Inventory.esp:
//   Coin_icon_01 (0x800) 1~4 G   | Coin_icon_02 (0x801) 5~9 G
//   Coin_icon_03 (0x802) 10~99 G | Coin_icon_04 (0x803) 100~1000 G (stacks)
//   Coin_Pouch   (0x804) 2x2, manual storage up to 10,000 G (G2: store/draw)
//   Coin_purseLarge (0x809) world-drop coin bag (abilities: 0x807/0x808)
// Decomposition: one 04 per full 1000, remainder becomes ONE coin of its
// tier. Same-form coins stack into a single grid tile with a count badge.
// The reconciler runs on the game thread (Tick) whenever the ledger moved;
// its own Add/Remove re-fire container events, which re-mark dirty and then
// reconcile to a zero diff — no loop.

namespace FUI::GoldCoins
{
    void InitForms();   // kDataLoaded: resolve the ESP records (soft-fails)
    [[nodiscard]] bool Ready();

    // coins + pouch — drop/sell style guards check this (G1: drops blocked)
    [[nodiscard]] bool IsCoinForm(RE::FormID a_id);
    [[nodiscard]] bool IsPouch(RE::FormID a_id);

    // GI52: the drawn-icon key for one of OUR gold forms ("msc_gold1".."4",
    // "msc_coinpouch"), or nullptr if the form isn't ours. The category-icon
    // rules are written against vanilla records, so without this every coin,
    // purse and pouch fell through to the catch-all and drew a pickaxe.
    // Answering here rather than in the icon module keeps the form list in the
    // one place that owns it.
    [[nodiscard]] const char* FallbackIconKey(RE::FormID a_id);

    [[nodiscard]] int PouchStored();   // 0..10000, cosave-persisted
    // ★Multi-pouch ready: capacity is PER FORM now. The shipped pouch
    // (0x804) is builtin at 10,000; any future pouch declares itself with
    // "pouchcap:N" in its item def, discovered through the resolver main.cpp
    // wires below. PouchCap() stays as the builtin/fallback figure.
    [[nodiscard]] int PouchCap();                                   // builtin 10,000
    [[nodiscard]] int PouchCapOfForm(RE::FormID a_id);              // 0 = not a pouch
    [[nodiscard]] int PouchCapOfKey(const std::string& a_tileKey);  // fallback = builtin
    [[nodiscard]] int MaxPouchCap();   // parking cap for gold awaiting a tile
    void SetPouchDefResolver(std::function<int(RE::FormID)> a_capOf);
    // ★multi-pouch vendor stock (OUR esp only), from main.cpp's def loop --
    // the same contract SetBagWares keeps for bags.
    void SetPouchWares(std::vector<RE::TESBoundObject*> a_wares);
    [[nodiscard]] RE::TESBoundObject* PouchForm();
    // ★Is a pouch actually ON the player right now? StoreToPouch does NOT ask
    // -- it only weighs the value against the cap and the walking gold, which
    // is right for a drop ONTO the pouch tile (the tile being there is proof
    // enough). A right-click on a coin has no such proof, so it asks here
    // first; without this, gold would vanish into a pouch sitting in a chest.
    // The pouch's own gold travels with it when it leaves (OnPouchLeftPlayer),
    // so "held" is the only state in which depositing means anything.
    [[nodiscard]] bool PouchHeld();
    // ICON to draw for the pouch tile right now: 0x804 N (0~2 G) /
    // 0x80C S (3~9) / 0x80D M (10~9999) / 0x80E F (full). Draw-time only —
    // the inventory item itself never changes form.
    [[nodiscard]] RE::TESBoundObject* PouchIconObject();   // window title etc.

    // G2: value represented by ONE coin tile of a_form at split-stack index
    // a_index (04 tiles show one-per-coin: index < fullThousands -> 1000,
    // last one -> the 100..999 remainder; tiers 1..3 always index 0).
    // Same rule, walking total supplied once -- for callers filling every tile
    // in a loop (InstanceValue re-walks the inventory on each call).

    // Number of grid tiles this coin form should display, computed from
    // WALKING gold (pending drops already subtracted). The rebuild uses this
    // instead of the live item count, which lags a tick behind a drop.
    // NOTE: AUTO tiles only — pinned purses (below) are counted separately.

    // Total AUTO coin cells (all tiers, 1x1 each) a given WALKING-gold amount
    // would occupy. Grid's spill pass uses this to add back the coin tiles a
    // barter payment just dissolved, so a purchase doesn't reuse the freed
    // cells (the item spills into a bag as if gold still filled the board).

    // Current walking gold (ledger - pouch - pinned): the pool the auto coin
    // tiles decompose from. Grid's spill pass pairs it with CoinTilesFor above.

    // ---- G4: pinned gold purses (Mabinogi split, symmetric with stacks) ----
    // A split fixes an amount (1..1000) onto a specific grid tile key; that
    // gold is subtracted from WALKING gold so the auto tier-decomposition
    // ignores it, and the purse keeps its exact value & position until merged
    // or dropped back. Value cap == one 04 coin (1000); larger never occurs.
    inline constexpr int kCoinCap = 1000;
    [[nodiscard]] int BandTier(int a_value);                       // value -> coin tier 0..3
    [[nodiscard]] RE::TESBoundObject* CoinForTier(int a_tier);     // 0x800..0x803

    // ★P2/3-5: the VANILLA gold, which is what actually sits in a container.
    // Our coin form is a tile identity on the player's board; the thing a chest
    // holds is Gold001, and anything that has to name the stored gold to the
    // partner side (a drop-cell note, for one) needs that form rather than ours.
    [[nodiscard]] RE::TESBoundObject* VanillaGold();

    // G2 interactions — mutate the ledger/pouch and mark the mirror dirty.
    // ★★PER POUCH. Every one of these used to be player-wide, which is why
    // two pouches in one inventory showed the same amount and the same
    // icon: there was a single number for all of them. The amount now hangs
    // off the TILE KEY, beside the pinned purses above and for the reason
    // LayoutEntry::rot and ::coin already give -- the engine has nowhere to
    // hang per-instance data, so the slot is what "this pouch" means.
    [[nodiscard]] int PouchStoredOf(const std::string& a_tileKey);
    // The icon band for an amount the CALLER knows -- a tile asks for its
    // own, a stored pouch asks for the one riding in the container spot.
    [[nodiscard]] RE::TESBoundObject* PouchIconObjectFor(int a_stored, int a_cap = 0,
                                                     RE::FormID a_form = 0);
    // A pouch's gold with no tile to sit on yet: a save older than v6, or a
    // pouch that just walked back in. The grid calls this with the pouch
    // tiles it found, split into tiles born THIS rebuild (a_fresh -- the
    // pouch that just walked in) and tiles that already existed (a_known).
    // ★(1.3.0) Returning shelf gold belongs to the pouch that carried it:
    // while the return grace holds, only a fresh tile may claim, so a
    // pre-existing empty pouch cannot swallow it.
    void ClaimReturned(const std::vector<std::string>& a_fresh,
                       const std::vector<std::string>& a_known);
    // A pouch's gold while the pouch is on a shelf: the container spot takes
    // it on the way out and gives it back on the way in, so the amount rides
    // with the pouch instead of hiding in a player-wide variable.
    [[nodiscard]] int TakeAwayGold(RE::FormID a_form = 0);
    // ★(1.5.x) the exact away parcel (form + amount) for a claimant that
    // knows what left with it; 0 = not (yet) away.
    [[nodiscard]] int TakeAwayParcelExact(RE::FormID a_form, int a_amount);
    void GiveAwayGold(int a_amount, RE::FormID a_form = 0);
    // ★(1.5.x) the reverse of TakeAwayGold: a claimed amount goes back on
    // the away list. Used when a bundled pouch leaves its bag ONTO the
    // shelf -- the cell being born claims a parcel, so the bundle entry's
    // amount has to become one again first.
    void RestoreAwayParcel(RE::FormID a_form, int a_amount);
    // ★(1.5.x) exact delivery for the bag flow: hand the parked parcel of
    // exactly (form, amount) to the given pouch TILE -- a wallet that only
    // travelled keeps its own amount. No-op when no such parcel waits (the
    // generic ClaimReturned pass handles the rest). Returns what moved.
    int ClaimParcelForTile(const std::string& a_tileKey, RE::FormID a_form,
                           int a_amount);
    // ★(1.3.2a) shelf-pouch banking: plain ledger credit/debit (no pouch
    // parking) -- the shelf spot is the book, these settle the engine gold.
    void CreditLedger(int a_amount);
    void DebitLedger(int a_amount);

    // ★★S-0: GOLD WE ASKED FOR, STILL IN TRANSIT.
    //
    // Taking gold out of a container moves engine coins through LootBarter's
    // request queue, not through this book -- so this book has no way to tell
    // it apart from a purse the player stepped on in a cave. That mattered
    // twice, and both were reported as bugs:
    //
    //   - The auto-store (Tick STEP 2) exists to sweep exactly that kind of
    //     outside income into a held pouch, so a DRAGGED amount was swallowed
    //     by the pouch on arrival while the pinned purse on the cursor drew
    //     its value out of walking gold -- the coins the player had laid out
    //     shrank by what they had just dragged in (measured: 663 G).
    //   - Walking gold is (ledger + pending) - pouch - pinned, and the pin
    //     lands the instant the drop does while the ledger moves a tick later.
    //     One frame of a smaller total is one frame of a smaller coin tile:
    //     the blink.
    //
    // Announcing the transfer answers both from one place. The amount counts
    // toward walking gold THIS frame (so a pin against it nets to zero), and
    // the sweep knows to leave that much alone. Self-cancelling: whatever has
    // not arrived within a short grace is forgotten, so a refused or lost
    // request cannot inflate the total for the rest of the session.
    //
    // ★Announce only for a transfer that PLACES the gold (a drag, which
    // promises this many coins in that square). A right-click take is loot,
    // and loot going into a held pouch is the auto-store working.
    void ExpectIncoming(int a_value);
    int  StoreToPouch(const std::string& a_tileKey, int a_value);   // amount actually stored
    int  StoreToPouch(int a_value);   // no pouch named -> the fullest with room
    void WithdrawFrom(const std::string& a_tileKey, int a_value, bool a_sound = true);
    // pouch -> walking gold. a_sound=false when the caller immediately lifts
    // the amount onto the cursor (the pickup sound plays instead).
    // ★P2/3-5: hand gold to a CONTAINER -- a chest, a follower's pack --
    // instead of to the floor. The transfer runs on the Tick like every other
    // ledger op, because moving engine gold from the render pass is exactly the
    // hazard the queue exists to avoid.
    //
    // ★It does NOT ride the ordinary store path, and that is deliberate: a coin
    // tile is a MIRROR of this ledger, not an inventory item, so taking it
    // through RequestStore would have the transfer counted twice -- once by the
    // request ledger and once here. One debit, from the book that owns the
    // gold.
    // ★Returns the amount actually queued -- 0 means nothing moved. A caller
    // that assumes success will unpin a purse and erase its cell for a
    // transfer that never happened.
    int StoreToContainer(RE::TESObjectREFR* a_dst, int a_value);

    void DropAsGold(int a_value);     // ledger down; a Coin_Sack (0x805,
                                      // coinbaglarge.nif) ref lands at the feet
                                      // carrying the value; falls back to a
                                      // vanilla gold ref when the ESP record
                                      // is missing

    // PickUpHook: a Coin_Sack world ref activates as GOLD — consume the ref,
    // credit its recorded value to the ledger. Returns true when handled.
    bool TryPickUpSack(RE::TESObjectREFR* a_ref);

    // The pouch ITEM can leave the player (chest storage, drop, companion,
    // sale) — space-penalty design: banking gold away is the point.
    //  - SALE (BarterMenu open): stored gold pops back to walking coins
    //    first; the vendor buys an empty pouch (no exploit).
    //  - everything else: the gold TRAVELS with the pouch (ledger debited);
    //    any pouch re-entering the inventory brings it back (credit).
    // Both are called from the container sink; ledger ops run on Tick.
    void OnPouchLeftPlayer(RE::FormID a_form);
    // ★multi-pouch: the returning FORM picks its own away parcel (same-form
    // FIFO, oldest as the legacy fallback) and steers the tile claim.
    void OnPouchReturned(RE::FormID a_form);
    // ★(1.3.0-C) The UI names WHICH pouch tile is about to leave (store /
    // sell / drop paths all know their tile; the engine event only knows the
    // form). One-shot: consumed by the next OnPouchLeftPlayer, cleared on
    // session end / load. Without it the FULLEST pouch paid for every
    // departure, whichever pouch actually left.
    void NotePouchLeaving(const std::string& a_tileKey);

    // How the player is meant to GET the pouch and the bags. The esp carries
    // no leveled-list or vendor-list edit, so nothing stocks them -- and
    // vendors sell out of the vendor faction's merchantContainer, not their
    // own inventory, which is why SPID (an NPC distributor) cannot put them
    // on the shelf either. So we seed that chest ourselves when a barter
    // session opens. Nothing touches the esp's vendor lists, so no
    // leveled-list override can conflict with a merchant mod.
    //
    // Restocking runs on a CYCLE INDEX, floor(daysPassed / iDaysToRespawnVendor)
    // -- one seeding per merchant per cycle, so re-opening the window or
    // buying the shelf empty cannot refill it mid-cycle (the old "add
    // whenever missing" rule made bags infinitely purchasable). The chest's
    // own respawn wipes our items (they are not in its leveled entries);
    // the next cycle's seeding is what brings them back.
    //
    // Per cycle: general-goods vendors get the pouch plus
    // kGenericBagsPerCycle general-purpose bags, drawn deterministically
    // from hash(merchant, cycle) -- a rotating lineup, not a refill. Every
    // TYPED bag is a guaranteed item at the merchants whose sell/buy list
    // carries its filter's vendor keyword (the shop that trades what the
    // bag holds). The bag list itself arrives via SetBagWares below.
    void SeedVendorStock(RE::Actor* a_merchant, RE::TESObjectREFR* a_container);

    // ★Which bags exist, and what each accepts ("" = general purpose). Handed
    // in from main.cpp rather than hardcoded here: the bags are defined by the
    // item defs, so a FormID list in this file would be a second source of
    // truth that silently drifts the moment one is added.
    struct BagWare
    {
        RE::TESBoundObject* obj = nullptr;
        std::string         accept;
    };
    void SetBagWares(std::vector<BagWare> a_wares);

    // ★S-G: pending ledger ops + announced transfers still in flight.
    // Grid::CoinCensus refuses to square the tile invariant while this is
    // non-zero -- the tile half and the ledger half land on different frames.
    [[nodiscard]] int UnsettledDelta();
    void MarkDirty();   // player's Gold001 (or a coin form) changed
    void Tick();        // reconcile mirror -> inventory (game thread)

    // Phase 5: our custom barter UI doesn't open the vanilla BarterMenu, so it
    // flags "selling context" here — OnPouchLeftPlayer uses it to release the
    // stored gold on a pouch sale (§2-C) instead of letting it travel away.
    void SetBarterContext(bool a_open);

    inline constexpr std::uint32_t kRecordType = 'GPCH';
    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame(SKSE::SerializationInterface* a_intfc);
}
