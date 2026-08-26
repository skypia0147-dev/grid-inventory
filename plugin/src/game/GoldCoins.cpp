#include "game/GoldCoins.h"

#include "game/BagFilter.h"
#include "ui/Grid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace FUI::GoldCoins
{
    namespace
    {
        constexpr const char*   kPlugin = "Grid Inventory.esp";
        constexpr RE::FormID    kGold001 = 0x0000000F;
        constexpr int           kPouchCap = 10000;
        constexpr std::uint32_t kVersion = 8;   // v2..v6 as before, v7:+away parcels, v8:+per-form return park
        // ★★Where a pouch's gold waits while no tile owns it: a save written
        // before v6 (one amount for the whole player) and a pouch that has
        // just walked back into the inventory both land here, and the first
        // rebuild hands it to a real pouch tile. One road, one thing to test.
        constexpr const char* kReturnKey = "##pouch_incoming";

        RE::TESBoundObject* g_coins[4] = {};   // 0x800..0x803 (tiers 1..4)
        RE::TESBoundObject* g_pouch = nullptr;
        // world-drop purses by amount (all optional; larger stands in for a
        // missing smaller): 1~4 G drops as a vanilla loose-gold ref instead
        RE::TESBoundObject* g_sack = nullptr;       // 0x809 Coin_purseLarge (100~1000)
        RE::TESBoundObject* g_sackMed = nullptr;    // 0x80A Coin_purseMed   (10~99)
        RE::TESBoundObject* g_sackSmall = nullptr;  // 0x80B Coin_purseSmall (5~9)
        // pouch ICON variants by stored amount (0x804 N itself is 0~2 G);
        // draw-time substitutes only — the pouch ITEM stays 0x804
        RE::TESBoundObject* g_pouchS = nullptr;     // 0x80C Coin_Pouch_S_01 (3~9)
        RE::TESBoundObject* g_pouchM = nullptr;     // 0x80D Coin_Pouch_M_01 (10~9999)
        RE::TESBoundObject* g_pouchF = nullptr;     // 0x80E Coin_Pouch_F_01 (full)
        // ★★PER POUCH, NOT PER PLAYER. This was one int, which is exactly
        // why two pouches in the same inventory both read the same number
        // and drew the same icon -- there was only ever ONE number. It now
        // lives per TILE KEY, next to the (retired) pin map and for the same stated
        // reason: the engine has nowhere to hang per-instance data, and the
        // slot is what "this pouch" means to the board (LayoutEntry::rot,
        // LayoutEntry::coin already say so).
        std::map<std::string, int> g_pouchStored;
        // ★MULTI-POUCH: capacity is PER FORM. The shipped pouch seeds the map
        // at InitForms; any other form answers through the def resolver
        // ("pouchcap:N") and the answer is cached either way -- including the
        // negative, so a hot IsPouch cannot re-resolve the same form twice.
        std::map<RE::FormID, int>      g_pouchCaps;
        std::function<int(RE::FormID)> g_pouchCapOf;
        // ★The cache is written from whatever thread asks (IsPouch runs in
        // the container sink), so it locks -- the old IsPouch was pure reads
        // and needed nothing, but a cache is a write.
        std::mutex                     g_pouchCapMtx;
        // ★vendor stock, OUR esp only -- handed in from main.cpp's def loop
        // exactly like the bag wares (the lazy cap cache above cannot
        // enumerate pouches it has never been asked about)
        std::vector<RE::TESBoundObject*> g_pouchWares;
        // ★the un-designated deposit ladder: designation > cohesion (finish
        // the fullest pouch) > rear board position -- the same three steps
        // every other keyless coin rule walks.
        [[nodiscard]] std::vector<std::string> PouchFillOrder()
        {
            auto keys = Grid::PouchTiles();               // front-first
            std::reverse(keys.begin(), keys.end());       // rear-first ties
            std::stable_sort(keys.begin(), keys.end(),
                [](const std::string& a, const std::string& b) {
                    const auto ia = g_pouchStored.find(a);
                    const auto ib = g_pouchStored.find(b);
                    const int  va = ia == g_pouchStored.end() ? 0 : ia->second;
                    const int  vb = ib == g_pouchStored.end() ? 0 : ib->second;
                    return va > vb;                       // fullest first
                });
            return keys;
        }

        [[nodiscard]] int CapOfForm(RE::FormID a_id)
        {
            {
                std::lock_guard l(g_pouchCapMtx);
                if (const auto it = g_pouchCaps.find(a_id); it != g_pouchCaps.end()) {
                    return it->second;
                }
            }
            if (!g_pouchCapOf) return 0;   // defs not wired yet: no caching
            // ★re-entrancy breaker: if the resolver's road ever leads back
            // here (DefFor's pouch sizing did exactly that -- the stack
            // overflow of 2026-08-26), answer "not a pouch" instead of
            // recursing; the outer call still caches the real answer.
            static thread_local bool s_resolving = false;
            if (s_resolving) return 0;
            s_resolving = true;
            const int c = (std::max)(0, g_pouchCapOf(a_id));
            s_resolving = false;
            std::lock_guard l(g_pouchCapMtx);
            g_pouchCaps[a_id] = c;
            return c;
        }
        // "plugin|0xID[#n]" -> FormID (0 = unparseable). The tile-key format
        // Grid's FormKey writes; Dynamic| keys resolve by raw id.
        [[nodiscard]] RE::FormID FormOfKey(const std::string& a_key)
        {
            const auto bar = a_key.find('|');
            if (bar == std::string::npos || a_key.size() < bar + 4) return 0;
            const auto id = static_cast<RE::FormID>(
                std::strtoul(a_key.c_str() + bar + 3, nullptr, 16));
            if (a_key.compare(0, bar, "Dynamic") == 0) return id;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return 0;
            auto* f = dh->LookupForm(id, std::string_view(a_key).substr(0, bar));
            return f ? f->GetFormID() : 0;
        }
        [[nodiscard]] int CapOfKey(const std::string& a_key)
        {
            const int c = CapOfForm(FormOfKey(a_key));
            return c > 0 ? c : kPouchCap;   // unknown records keep the builtin
        }
        [[nodiscard]] int ParkSumFwd();   // the per-form parking, below
        [[nodiscard]] int PouchSum()
        {
            // parked returns are still pouch claims -- the invariant
            // (tiles == ledger - pouch) and the trims read them through here
            int s = ParkSumFwd();
            for (const auto& [k, v] : g_pouchStored) s += v;
            return s;
        }
        // The tile a keyless caller means: an engine event (a pouch left
        // the player) names no tile, so it acts on the fullest one.
        [[nodiscard]] std::string FullestPouch()
        {
            std::string best; int bv = -1;
            for (const auto& [k, v] : g_pouchStored) {
                // (the old reserved parking key lives in g_returnPark now --
                // this map holds real tiles only)
                if (v > bv) { bv = v; best = k; }
            }
            return best;
        }
        bool                g_dirty = true;
        bool                g_applying = false;   // reentrancy guard (Tick)

        // G4: pinned gold purses — grid tile key -> fixed amount (1..1000).
        // Subtracted from walking gold so the auto tier-decomposition ignores
        // them; each keeps its exact value & position until merged/dropped.

        // ★Vendor restock: the last stock CYCLE we seeded for each merchant.
        // Not a timestamp — a cycle index, floor(daysPassed / iDaysToRespawnVendor).
        // Storing the index rather than the time is what makes the shelf stable:
        // every visit inside one cycle computes the same number, so re-opening
        // the barter window cannot re-roll the lineup or refill something the
        // player just bought (which is exactly what the old "stock it whenever
        // it is missing" rule did — bags were infinitely purchasable).
        std::map<RE::FormID, std::uint32_t> g_vendorCycle;

        // ★★Merchants re-seeded since the last game load. NOT serialized, and
        // cleared on every revert — it is a per-load allowance, not save state.
        //
        // The cycle counter above says "I stocked this merchant this cycle",
        // and that is not the same as "the goods are still on the shelf". The
        // vendor chest respawns on its own and takes our wares with it (they
        // are not in its leveled entries), and that respawn is processed on the
        // FIRST load after the game is launched. Measured on one save, same
        // merchant, same cycle: first load after launch, the chest held 0 of
        // ours; reloading the same save in the same session, 8. The counter
        // said "done" both times, so the shelf stayed empty until the game was
        // restarted.
        // ★Bounded to once per load on purpose. Plain "stock it whenever it is
        // missing" is what made bags infinitely purchasable before (see above):
        // buy the last one, reopen, and it came back. With this the shelf can
        // only be replenished after the chest was emptied by something other
        // than the player's own shopping.
        std::set<RE::FormID> g_reseeded;

        // how many general-purpose bags a general store shows per cycle
        constexpr int kGenericBagsPerCycle = 3;
        std::vector<BagWare> g_bagWares;

        // stored gold travelling INSIDE a pouch that left the inventory
        // (chest storage / drop / companion). Global concept, not per-item:
        // pouches are MISC stacks with no per-instance identity — any pouch
        // re-entering the inventory restores it (gold can never be lost to
        // the tracking itself; a respawning container eating the pouch is
        // the intended penalty).
        // ★multi-pouch: gold travelling inside pouches that LEFT, kept as
        // PARCELS (form, amount) in departure order -- so a Large pouch can
        // come home with ITS gold instead of the builtin's. Form 0 = a
        // legacy save's single untagged sum. The serialized v3 field stays
        // the SUM (old readers keep their offsets); v7 appends the parcels.
        struct AwayParcel
        {
            RE::FormID form = 0;
            int        amount = 0;
        };
        std::vector<AwayParcel> g_awayParcels;
        [[nodiscard]] int AwaySum()
        {
            int t = 0;
            for (const auto& a : g_awayParcels) t += a.amount;
            return t;
        }
        // pick the parcel a return of `form` means: same-form FIFO, else the
        // oldest (legacy / mixed history)
        [[nodiscard]] std::size_t ParcelFor(RE::FormID a_form)
        {
            for (std::size_t i = 0; i < g_awayParcels.size(); ++i) {
                if (g_awayParcels[i].form == a_form) return i;
            }
            return 0;
        }
        // ★PARKING, AS PARCELS: gold that came home before its tile did.
        // A per-form SUM here merged two same-form wallets into one pot and
        // the claim re-split it by capacity -- two pouches of 9,000 and
        // 9,314 came back as 10,000 and 8,314 (measured 16:32). A wallet
        // that merely travelled must keep ITS amount, so each return stays
        // its own parcel and a claim hands ONE parcel to ONE tile. During
        // the fresh-tile grace a tagged parcel is RESERVED for its own
        // form's tiles; the grace expiring opens it to anyone, and what no
        // pouch can hold mints once the settle delay runs out, so money can
        // never strand. Form 0 = untagged (legacy saves).
        std::vector<AwayParcel> g_returnPark;
        [[nodiscard]] int ParkSum()
        {
            int t = 0;
            for (const auto& pc : g_returnPark) t += pc.amount;
            return t;
        }
        int ParkSumFwd() { return ParkSum(); }

        // ★(1.3.0-C) WHICH pouch tile the UI last committed to leave. The
        // container-change event only names the FORM, so OnPouchLeftPlayer
        // used to guess ("the fullest one") -- store pouch B while holding a
        // fuller A and A's gold walked out with B. Every UI exit path passes
        // through Grid::NotePendingRemove WITH its tile key; it names the
        // tile here. One-shot: consumed by the next OnPouchLeftPlayer,
        // cleared on session end / load (a stale hint must not outlive the
        // transaction it described).
        std::string g_leavingHint;

        // ★(1.3.0) claim passes during which ONLY a freshly-born pouch tile
        // may take the waiting return (see ClaimReturned). Armed whenever a
        // pouch's gold starts travelling home; a handful of passes covers
        // the request -> Tick -> rebuild gap with room to spare.
        int g_returnFreshGrace = 0;

        // ★(1.5.x) extra claim passes AFTER the grace before parked money
        // that found no pouch gives up and mints as coins -- a late-born
        // pouch tile (slow transfer, busy frame) must not lose its refill
        // to an eager mint. Re-armed whenever money parks.
        int g_parkMintDelay = 0;

        // auto-store: ledger snapshot of the PREVIOUS tick (after our own
        // ops). -1 = uninitialised (skip the first tick after load/new game
        // so the starting gold is NOT mistaken for a fresh pickup).
        int g_lastLedger = -1;

        // ★S-0: gold a transfer WE started is bringing in, not yet arrived.
        // See ExpectIncoming in the header for why this exists. The age is a
        // tick budget: the request lands a frame or two later, so anything
        // still outstanding well past that is never coming and is dropped
        // rather than left inflating walking gold forever.
        int g_expected = 0;
        int g_expectedAge = 0;
        constexpr int kExpectGrace = 120;   // ~2 s of ticks; the gap is 1-2

        // ---- single deferred-ledger queue ----------------------------------
        // Every engine gold mutation triggered from the RENDER pass or an
        // event sink is deferred to Tick (game thread). One queue replaces the
        // old trio (pendingDrops + ledgerCredit + ledgerDebit). Each op also
        // declares its ledger delta so WalkingGold reflects it THE SAME FRAME
        // — the coins/pouch update instantly even though the engine mutation
        // lands next tick.
        struct LedgerOp
        {
            enum Kind { kDropCoin, kPouchLeave, kPouchReturn,
                        kDebit,        // (1.3.2a) plain debit -- no awayGold tie
                        kStoreCoin };  // P2/3-5: gold into a container
            Kind kind;
            int  value;
            // kStoreCoin only: where it goes. A handle would be safer against a
            // cell unloading, but the op is consumed on the very next Tick and
            // the container is the one the player has open -- and LookupByID
            // returning null is already the "gone" answer this needs (원칙 2).
            RE::FormID target = 0;
        };
        std::vector<LedgerOp> g_pending;

        // net change the pending ops will make to the ledger (+ credit, - debit)
        // ★S-0: an announced incoming transfer counts too. It is not one of OUR
        // ops -- LootBarter moves those coins -- but from walking gold's point
        // of view the two are the same statement: this much is on its way, so
        // stop drawing the board as if it were not.
        int PendingLedgerDelta()
        {
            int d = g_expected;
            for (const auto& op : g_pending) {
                d += (op.kind == LedgerOp::kPouchReturn) ? op.value : -op.value;
            }
            return d;
        }

        // string (de)serialisation for the pinned map (cosave v4)
        bool WriteStr(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const std::uint32_t len = static_cast<std::uint32_t>(a_s.size());
            if (!a_intfc->WriteRecordData(len)) return false;
            return len == 0 || a_intfc->WriteRecordData(a_s.data(), len);
        }
        bool ReadStr(SKSE::SerializationInterface* a_intfc, std::string& a_out)
        {
            constexpr std::uint32_t kMaxStr = 256;
            std::uint32_t len = 0;
            if (!a_intfc->ReadRecordData(len) || len > kMaxStr) return false;
            a_out.resize(len);
            return len == 0 || a_intfc->ReadRecordData(a_out.data(), len);
        }

        // world Coin_Sack refs -> gold value they carry (cosave v2)
        std::map<RE::FormID, int> g_sackRefs;

        int CountOf(RE::PlayerCharacter* a_p, RE::TESBoundObject* a_obj)
        {
            if (!a_obj) return 0;
            int cnt = 0;
            auto inv = a_p->GetInventory([&](RE::TESBoundObject& o) { return &o == a_obj; });
            for (auto& [o, d] : inv) cnt = d.first;
            return cnt;
        }

        // ★Walking gold -> how many TILES, and that is now the whole rule:
        // one per capful, plus one for the remainder. The old version banded
        // the leftover into 1~4 / 5~9 / 10~99 / 100+ because each band had its
        // own picture; with one coin (see CoinForTier) those bands only ever
        // split a purse that could have been one tile -- 663 G came out as two
        // (measured in the mirror log: "663 G walking -> 1/0/0/1").
        //
        // Everything still lands in slot [0], because slot = tier and there is
        // one tier left. The array shape stays so the mirror's diff, its log
        // line and its callers are untouched.
    }

    void InitForms()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        for (int i = 0; i < 4; ++i) {
            g_coins[i] = dh->LookupForm<RE::TESObjectMISC>(0x800 + i, kPlugin);
        }
        g_pouch = dh->LookupForm<RE::TESObjectMISC>(0x804, kPlugin);
        if (g_pouch) {
            std::lock_guard l(g_pouchCapMtx);
            g_pouchCaps[g_pouch->GetFormID()] = kPouchCap;   // builtin
        }
        g_pouchS = dh->LookupForm<RE::TESObjectMISC>(0x80C, kPlugin);
        g_pouchM = dh->LookupForm<RE::TESObjectMISC>(0x80D, kPlugin);
        g_pouchF = dh->LookupForm<RE::TESObjectMISC>(0x80E, kPlugin);
        SKSE::log::info("[GOLD] pouch icon variants: S={} M={} F={}",
            g_pouchS ? "ok" : "missing", g_pouchM ? "ok" : "missing",
            g_pouchF ? "ok" : "missing");
        g_sack      = dh->LookupForm<RE::TESObjectMISC>(0x809, kPlugin);   // Coin_purseLarge
        g_sackMed   = dh->LookupForm<RE::TESObjectMISC>(0x80A, kPlugin);   // Coin_purseMed
        g_sackSmall = dh->LookupForm<RE::TESObjectMISC>(0x80B, kPlugin);   // Coin_purseSmall
        if (g_sack) {
            SKSE::log::info("[GOLD] purses: L={:08X} M={} S={}", g_sack->GetFormID(),
                g_sackMed ? "ok" : "missing", g_sackSmall ? "ok" : "missing");
        } else {
            SKSE::log::info("[GOLD] no Coin_purseLarge (0x809) — drops fall back to vanilla gold refs");
        }
        if (Ready()) {
            SKSE::log::info("[GOLD] coin forms resolved ({})", kPlugin);
        } else {
            SKSE::log::warn("[GOLD] '{}' missing or incomplete — coin mirror disabled", kPlugin);
        }
        g_dirty = true;
    }

    bool Ready()
    {
        return g_coins[0] && g_coins[1] && g_coins[2] && g_coins[3] && g_pouch;
    }

    bool IsCoinForm(RE::FormID a_id)
    {
        for (auto* c : g_coins) {
            if (c && c->GetFormID() == a_id) return true;
        }
        if (g_pouch && g_pouch->GetFormID() == a_id) return true;
        return CapOfForm(a_id) > 0;   // ★multi-pouch: same guards as the builtin
    }

    bool IsPouch(RE::FormID a_id)
    {
        if (g_pouch && g_pouch->GetFormID() == a_id) return true;
        return CapOfForm(a_id) > 0;   // ★multi-pouch: any def-declared pouch
    }

    const char* FallbackIconKey(RE::FormID a_id)
    {
        // One coin, one fallback icon -- the other three tier keys retired
        // with the tiers (see CoinForTier). The remaining forms are still
        // RECOGNISED here, because a save made before this change can hold
        // tiles keyed to them.
        for (int i = 0; i < 4; ++i) {
            if (g_coins[i] && g_coins[i]->GetFormID() == a_id) return "msc_gold1";
        }
        // ★★AND VANILLA GOLD, which draws on exactly one board: a container's.
        //
        // The player's own never shows this form -- the rebuild takes Gold001
        // out as the ledger and draws the coin mirror in its place -- so it was
        // never asked about here, and a chest full of septims fell through every
        // rule below it to the catch-all. The player saw a pickaxe where their
        // money was, which is the "stored gold has no icon" report.
        //
        // ★DRAW-TIME ONLY, exactly like the pouch's icon variants: the item in
        // the chest stays Gold001 and must. That is the form the rest of the
        // game reads -- followers, quest scripts, theft, chest respawns -- and
        // the form that survives this plugin being uninstalled. Swapping the
        // STORED item for our coin would hide the player's money from
        // everything but us, and destroy it the day the esp is removed. The
        // picture is ours to choose; the money is not.
        if (a_id == kGold001) return "msc_gold1";
        // the pouch, its draw-time icon variants, and the purse sizes all read
        // as one thing: a bag of coins
        for (auto* p : { g_pouch, g_pouchS, g_pouchM, g_pouchF,
                         g_sack, g_sackMed, g_sackSmall }) {
            if (p && p->GetFormID() == a_id) return "msc_coinpouch";
        }
        if (CapOfForm(a_id) > 0) return "msc_coinpouch";   // ★multi-pouch
        return nullptr;
    }

    int PouchStored() { return PouchSum(); }   // legacy: the whole player
    // ★★The reserved key is not a tile -- no board ever draws it. The grid
    // calls this once it knows which tiles are pouches, and the waiting
    // amount moves onto a pouch with room. Anything that will not fit
    // (the player carried one pouch, stored 10k, then lost the pouch) stays
    // parked rather than being silently destroyed.
    // ★(1.3.0) FRESH TILES FIRST, AND ONLY FRESH WHILE THE GRACE HOLDS.
    // Returning shelf gold belongs to the pouch that carried it, and that
    // pouch's tile is born a rebuild or two AFTER the amount is parked (the
    // engine transfer lands on a later Tick). "First tile with room" is how
    // a pre-existing EMPTY pouch swallowed a returning pouch's gold and the
    // returner itself drew empty. The grace counts claim passes, not time:
    // a pass that finds no fresh tile leaves the amount parked and burns
    // one; a save older than v6 parks with no grace and settles at once.
    void ClaimReturned(const std::vector<std::string>& a_fresh,
                       const std::vector<std::string>& a_known)
    {
        if (g_returnPark.empty()) return;
        const bool graced = g_returnFreshGrace > 0;
        // ★TILE-MAJOR, ONE PARCEL PER TILE. Each tile picks the oldest
        // parcel it may have, whole (cap-clamped; a remainder stays parked).
        // Parcel-major with cap-filling was the amount blender: it topped
        // the first tile to its cap out of every parcel in the pot.
        const auto claimOne = [&](const std::string& k) {
            // ★during the grace a tile that already holds money has had its
            // delivery (the exact-parcel claim, or a prior pass) -- topping
            // it up from someone else's parcel is the blending again. After
            // the grace, room is room: money never strands.
            if (graced) {
                const auto hi = g_pouchStored.find(k);
                if (hi != g_pouchStored.end() && hi->second > 0) return;
            }
            for (auto pc = g_returnPark.begin(); pc != g_returnPark.end();
                 ++pc) {
                // during the grace a tagged parcel is RESERVED for its own
                // form; afterwards any pouch may take it
                if (graced && pc->form != 0 && FormOfKey(k) != pc->form) {
                    continue;
                }
                int& held = g_pouchStored[k];
                const int move = (std::min)(pc->amount, CapOfKey(k) - held);
                if (move <= 0) {
                    if (held == 0) g_pouchStored.erase(k);
                    continue;   // this tile is full for THIS parcel's size
                }
                held += move;
                pc->amount -= move;
                SKSE::log::info("[GOLD] incoming {} G claimed by pouch '{}'",
                                move, k);
                g_dirty = true;
                if (pc->amount <= 0) g_returnPark.erase(pc);
                return;   // one parcel per tile per pass
            }
        };
        for (const auto& k : a_fresh) claimOne(k);
        if (!graced) {
            for (const auto& k : a_known) claimOne(k);
        }
        const bool leftOver = !g_returnPark.empty();
        if (leftOver) {
            if (g_returnFreshGrace > 0) {
                --g_returnFreshGrace;
            } else if (g_parkMintDelay > 0) {
                --g_parkMintDelay;
            } else {
                // ★(1.5.x) the grace is spent and EVERY pouch has been
                // offered the rest -- what none of them can hold comes back
                // as visible coins instead of sitting in an invisible lot.
                // (This is the old per-call cap's mint, moved to the one
                // moment the claim has actually settled -- the cap itself
                // spilled the SECOND of two same-form pouches coming home
                // together, because it capped the bucket per form while two
                // pouches' worth was legitimately on its way.)
                int homeless = 0;
                for (const auto& pc : g_returnPark) homeless += pc.amount;
                g_returnPark.clear();
                if (homeless > 0) {
                    SKSE::log::info(
                        "[GOLD] {} G found no pouch -- minted as coins",
                        homeless);
                    Grid::CoinIncome(homeless);
                    g_dirty = true;
                }
            }
        } else {
            g_returnFreshGrace = 0;
        }
    }

    // ★★A STORED POUCH KEEPS ITS GOLD, and the chest shelf is where it now
    // sits. OnPouchLeftPlayer already parks the amount in g_awayGold on its
    // way out; the container spot claims it here so the shelf can draw the
    // right icon and hand it back on the way home. Without this the amount
    // stayed in a player-wide variable and the stored pouch drew as EMPTY.
    // ★(1.5.x) the BAG flow's exact delivery: the incoming manifest knows
    // each pouch entry's amount AND which fresh tile it became, so the
    // matching parcel goes to that tile before the generic claim pass can
    // blur it. Exact (form, amount) match only -- anything the identity
    // cannot pin falls back to ClaimReturned. Returns what moved.
    int ClaimParcelForTile(const std::string& a_tileKey, RE::FormID a_form,
                           int a_amount)
    {
        if (a_amount <= 0) return 0;
        const auto it = std::find_if(g_returnPark.begin(), g_returnPark.end(),
            [&](const AwayParcel& pc) {
                return pc.form == a_form && pc.amount == a_amount;
            });
        if (it == g_returnPark.end()) return 0;
        int& held = g_pouchStored[a_tileKey];
        const int move = (std::min)(it->amount, CapOfKey(a_tileKey) - held);
        if (move <= 0) {
            if (held == 0) g_pouchStored.erase(a_tileKey);
            return 0;
        }
        held += move;
        it->amount -= move;
        if (it->amount <= 0) g_returnPark.erase(it);
        SKSE::log::info("[GOLD] {} G reunited with its own pouch tile '{}'",
                        move, a_tileKey);
        g_dirty = true;
        return move;
    }

    int TakeAwayGold(RE::FormID a_form)
    {
        if (g_awayParcels.empty()) return 0;
        const std::size_t pi = a_form != 0 ? ParcelFor(a_form) : 0;
        const int v = g_awayParcels[pi].amount;
        g_awayParcels.erase(g_awayParcels.begin() + static_cast<std::ptrdiff_t>(pi));
        return v;
    }

    // ★(1.5.x) the exact away parcel (form + amount), for a claimant that
    // knows precisely what left with it -- a bundle entry stamped at the
    // manifest build. 0 = no such parcel (yet); the caller decides whether
    // to wait or fall back to first-come.
    int TakeAwayParcelExact(RE::FormID a_form, int a_amount)
    {
        if (a_amount <= 0) return 0;
        const auto it = std::find_if(g_awayParcels.begin(), g_awayParcels.end(),
            [&](const AwayParcel& pc) {
                return pc.form == a_form && pc.amount == a_amount;
            });
        if (it == g_awayParcels.end()) return 0;
        g_awayParcels.erase(it);
        return a_amount;
    }

    // ★(1.5.x) a claimed amount becomes an away parcel again -- the bundled
    // pouch stepping out of its bag onto the shelf, where the cell being
    // born will claim it back through TakeAwayGold above.
    void RestoreAwayParcel(RE::FormID a_form, int a_amount)
    {
        if (a_amount <= 0) return;
        g_awayParcels.push_back({ a_form, a_amount });
        g_dirty = true;
    }

    // ★★CREDITED HERE, NOT HANDED TO AN EVENT. The first version parked the
    // amount in g_awayGold and left OnPouchReturned to pick it up -- but that
    // event fires on the ENGINE's container change, one whole frame before
    // the UI pass that retires the shelf slot. The credit therefore arrived
    // after the only reader had already given up ("pouch returned but
    // nothing was away"), and 6,943 G sat in a variable nobody read again.
    // A shelf that knows the amount can simply deposit it.
    void GiveAwayGold(int a_amount, RE::FormID a_form)
    {
        if (a_amount <= 0) return;
        g_pending.push_back({ LedgerOp::kPouchReturn, a_amount });
        // ★(1.5.x) one give-away, one parcel: the amount keeps its own
        // identity all the way to a tile (see ClaimReturned)
        g_returnPark.push_back({ a_form, a_amount });
        g_returnFreshGrace = 4;   // (1.3.0) the ARRIVING pouch's tile claims this
        g_parkMintDelay = 12;     // and the mint waits out a slow arrival
        g_dirty = true;
        SKSE::log::info("[GOLD] shelf handed back {} G -> waiting for a tile", a_amount);
    }

    // ★(1.3.2a) shelf-pouch banking. The shelf SPOT is the book; these move
    // the engine gold to match -- a plain ledger credit/debit with no pouch
    // parking, deferred to Tick like every other ledger op (WalkingGold
    // reflects them the same frame via PendingLedgerDelta).
    void CreditLedger(int a_amount)
    {
        if (a_amount <= 0) return;
        g_pending.push_back({ LedgerOp::kPouchReturn, a_amount });
        g_dirty = true;
    }

    void DebitLedger(int a_amount)
    {
        if (a_amount <= 0) return;
        g_pending.push_back({ LedgerOp::kDebit, a_amount });
        g_dirty = true;
    }

    void ExpectIncoming(int a_value)
    {
        if (a_value <= 0) return;
        g_expected += a_value;
        g_expectedAge = 0;   // a fresh announcement renews the whole budget
        g_dirty = true;
        SKSE::log::info("[GOLD] expecting {} G ({} announced, not yet arrived)",
                        a_value, g_expected);
    }

    int PouchStoredOf(const std::string& a_tileKey)
    {
        const auto it = g_pouchStored.find(a_tileKey);
        return it == g_pouchStored.end() ? 0 : it->second;
    }

    RE::TESBoundObject* PouchIconObject()
    {
        // stored-amount band -> icon variant (user spec 2026-07-22):
        // 0~2 = N (the pouch item itself), 3~9 = S, 10~9999 = M, cap = F.
        // A missing variant record falls through to the next lower band.
        return PouchIconObjectFor(PouchSum());
    }

    RE::TESBoundObject* PouchIconObjectFor(int a_stored, int a_cap,
                                           RE::FormID a_form)
    {
        // ★The N/S/M/F variants are the BUILTIN pouch's own art. Any other
        // pouch keeps its single model -- returning null leaves the caller's
        // icon exactly as it was.
        if (a_form != 0 && !(g_pouch && g_pouch->GetFormID() == a_form)) {
            return nullptr;
        }
        if (a_cap <= 0) a_cap = kPouchCap;
        if (a_stored >= a_cap && g_pouchF) return g_pouchF;
        if (a_stored >= 10 && g_pouchM) return g_pouchM;
        if (a_stored >= 3 && g_pouchS) return g_pouchS;
        return g_pouch;
    }

    int PouchCap() { return kPouchCap; }

    int PouchCapOfForm(RE::FormID a_id) { return CapOfForm(a_id); }

    int PouchCapOfKey(const std::string& a_tileKey) { return CapOfKey(a_tileKey); }

    int MaxPouchCap()
    {
        std::lock_guard l(g_pouchCapMtx);
        int m = kPouchCap;
        for (const auto& [id, c] : g_pouchCaps) m = (std::max)(m, c);
        return m;
    }

    void SetPouchWares(std::vector<RE::TESBoundObject*> a_wares)
    {
        g_pouchWares = std::move(a_wares);
        // capacity for each ware resolves lazily through the def hook; the
        // wares list only decides what a shopkeeper stocks
    }

    void SetPouchDefResolver(std::function<int(RE::FormID)> a_capOf)
    {
        g_pouchCapOf = std::move(a_capOf);
    }

    RE::TESBoundObject* PouchForm() { return g_pouch; }

    bool PouchHeld()
    {
        // ★Asked from a click handler, so it may run before the forms resolve
        // and on a frame where the player is not there at all (main menu).
        // ★multi-pouch: ANY pouch form counts. The builtin-only test made a
        // right-click deposit dead while only a Large pouch was carried
        // (user report) -- IsPouch is the one authority on pouch-ness.
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return false;
        if (g_pouch && p->GetItemCount(g_pouch) > 0) return true;
        for (const auto& [obj, data] : p->GetInventory(
                 [](RE::TESBoundObject& o) { return IsPouch(o.GetFormID()); })) {
            if (data.first > 0) return true;
        }
        return false;
    }

    void SetBagWares(std::vector<BagWare> a_wares)
    {
        g_bagWares = std::move(a_wares);
        int typed = 0;
        for (const auto& w : g_bagWares) typed += w.accept.empty() ? 0 : 1;
        logger::info("[VENDOR] {} bag ware(s): {} typed, {} general",
            g_bagWares.size(), typed, g_bagWares.size() - typed);
    }

    void SeedVendorStock(RE::Actor* a_merchant, RE::TESObjectREFR* a_container)
    {
        // ★★Every exit below used to be a silent `return`, and the log for a
        // shop visit was therefore EMPTY -- indistinguishable from "never
        // called". A run of these is the whole reason the "no bags on the first
        // load after launching the game" report could not be read off a log.
        // Each one names itself now; the reason must reach the log even when
        // the answer is "nothing to do".
        const auto bail = [&](const char* why) {
            logger::info("[VENDOR] skip ({}) - merchant {}", why,
                a_merchant ? a_merchant->GetDisplayFullName() : "<null>");
        };
        if (!a_merchant || !a_container) { bail("no merchant/container"); return; }

        auto* fac = a_merchant->GetVendorFaction();
        if (!fac) { bail("no vendor faction"); return; }
        auto* list = fac->vendorData.vendorSellBuyList;
        // unrestricted vendor: not a general store either
        if (!list) { bail("no sell/buy list"); return; }

        // "General goods" = the vendor's category list covers clutter. Testing
        // the KEYWORD rather than our own item matters: the pouch is a custom
        // MISC record with no VendorItem* keyword of its own, so matching it
        // against the list directly would exclude every merchant. Keywords keep
        // their EditorID at runtime, so no po3 Tweaks dependency here.
        // ★NOT a function-local static any more. A static caches the FIRST
        // lookup for the whole process, so one early miss would poison every
        // shop until the game is restarted -- exactly the shape of a
        // "only after a fresh launch" bug, and not something to leave standing
        // while investigating one.
        auto* clutter = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("VendorItemClutter");
        if (!clutter) { bail("VendorItemClutter keyword not found"); return; }
        const bool inList = list->HasForm(clutter);
        // The list is a whitelist unless notBuySell flips it into a blacklist.
        // ★No longer an early return: a SPECIALIST (alchemist, blacksmith) is
        // not a general store but still stocks its own typed bag, so the two
        // questions had to come apart.
        const bool isGeneral = !(fac->vendorData.vendorValues.notBuySell ? inList : !inList);

        // ---- restock cycle ------------------------------------------------
        // The merchant chest respawns on its own (CONT DATA bit1 is set on
        // every Merchant* chest, verified) every iDaysToRespawnVendor days,
        // and that reset WIPES what we put in — our items were never in the
        // chest's leveled entries, so they are not regenerated, just gone.
        // Nothing to clean up on our side; we only decide what goes back.
        auto* cal = RE::Calendar::GetSingleton();
        if (!cal) { bail("no Calendar"); return; }
        float days = 2.0f;
        if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
            if (auto* set = gs->GetSetting("iDaysToRespawnVendor")) {
                days = static_cast<float>(set->GetInteger());
            }
        }
        // ★Read the setting, never hardcode 2: merchant overhauls routinely
        // change it, and a hardcoded rhythm would leave OUR wares on a
        // different clock from every other item on the shelf.
        if (days < 0.1f) days = 2.0f;
        const auto cycle = static_cast<std::uint32_t>(cal->GetDaysPassed() / days);

        // ★★How many of OUR wares are on the shelf RIGHT NOW. This is not a
        // statistic -- it is the thing the restock decision turns on, because
        // "I stocked this cycle" and "the goods are still there" are different
        // claims and the chest's own respawn is what pulls them apart.
        // ★One pass, not one per ware: GetInventory() walks the whole list and
        // allocates a map on every call, and this now runs on every shop open.
        int oursInChest = 0;
        {
            std::set<RE::TESBoundObject*> ours;
            for (const auto& w : g_bagWares) {
                if (w.obj) ours.insert(w.obj);
            }
            if (g_pouch) ours.insert(g_pouch);
            for (auto* pw : g_pouchWares) {   // ★multi-pouch
                if (pw) ours.insert(pw);
            }
            for (const auto& [obj, data] : a_container->GetInventory()) {
                if (data.first > 0 && ours.contains(obj)) ++oursInChest;
            }
        }

        const RE::FormID mid = a_merchant->GetFormID();
        const auto it = g_vendorCycle.find(mid);
        const bool cycleDone = it != g_vendorCycle.end() && it->second >= cycle;
        // Nothing of ours on the shelf and we have not already replenished this
        // merchant since the load -> the chest was emptied by something that is
        // not the player's shopping (its own respawn), so the counter is lying.
        const bool emptied = oursInChest == 0 && !g_reseeded.contains(mid);

        if (cycleDone && !emptied) {
            // already stocked this cycle — buying does not refill it
            logger::info("[VENDOR] {}: cycle {} already stocked, {} of ours on the shelf",
                a_merchant->GetDisplayFullName(), cycle, oursInChest);
            return;
        }
        logger::info("[VENDOR] {} (0x{:08X}): cycle {} (day {:.1f} / {:.0f}), general={}, "
                     "{} of ours on the shelf -- {}",
            a_merchant->GetDisplayFullName(), mid, cycle, cal->GetDaysPassed(), days,
            isGeneral, oursInChest,
            cycleDone ? "chest respawned since we stocked it, re-seeding once this load"
                      : "new cycle");
        g_vendorCycle[mid] = cycle;
        g_reseeded.insert(mid);

        auto place = [&](RE::TESBoundObject* item, const char* why) {
            if (!item) return;
            for (const auto& [obj, data] : a_container->GetInventory(
                     [&](RE::TESBoundObject& o) { return &o == item; })) {
                (void)obj;
                if (data.first > 0) return;   // already there (player sold one back)
            }
            a_container->AddObjectToContainer(item, nullptr, 1, nullptr);
            logger::info("[VENDOR] cycle {} {}: stocked '{}' ({})",
                cycle, a_merchant->GetDisplayFullName(), item->GetName(), why);
        };

        // the pouch is core kit, not a lucky find: general goods, every cycle
        if (isGeneral) place(g_pouch, "always");
        // ★multi-pouch: def-declared pouches from OUR esp sell beside it --
        // an upgrade you can walk in and buy, not a lucky find
        if (isGeneral) {
            for (auto* pw : g_pouchWares) place(pw, "pouch upgrade");
        }

        // ---- typed bags: the shop that trades what the bag holds -----------
        // Guaranteed, not rotated. A player who walks to the alchemist for an
        // alchemy pouch should find one; making it a lucky draw would turn a
        // deliberate errand into a chore.
        std::vector<RE::TESBoundObject*> generic;
        for (const auto& w : g_bagWares) {
            if (!w.obj) continue;
            if (w.accept.empty()) { generic.push_back(w.obj); continue; }
            auto* kw = BagFilter::VendorKeyword(w.accept);
            if (kw && list->HasForm(kw) == !fac->vendorData.vendorValues.notBuySell) {
                place(w.obj, "typed");
            }
        }

        // ---- general-purpose bags: a fresh draw at the general store --------
        // Deterministic in (merchant, cycle): re-opening the window cannot
        // reshuffle the shelf, and no roll has to be stored anywhere.
        if (isGeneral && !generic.empty()) {
            const int n = static_cast<int>(generic.size());
            const int want = (std::min)(kGenericBagsPerCycle, n);
            std::uint32_t h = mid * 2654435761u ^ (cycle * 40503u);
            std::vector<int> idx(n);
            for (int i = 0; i < n; ++i) idx[i] = i;
            // Fisher-Yates off the same hash: picking `want` distinct entries
            // rather than `want` independent rolls, so the shelf never shows
            // the same bag twice.
            for (int i = n - 1; i > 0; --i) {
                h = h * 1664525u + 1013904223u;
                const int j = static_cast<int>(h % static_cast<std::uint32_t>(i + 1));
                std::swap(idx[i], idx[j]);
            }
            for (int i = 0; i < want; ++i) place(generic[idx[i]], "rotating");
        }
        logger::info("[VENDOR]   {} typed candidate(s), {} generic in the pool",
            g_bagWares.size() - generic.size(), generic.size());
    }

    namespace
    {
        int TierOf(RE::FormID a_id)
        {
            for (int i = 0; i < 4; ++i) {
                if (g_coins[i] && g_coins[i]->GetFormID() == a_id) return i;
            }
            return -1;
        }

        // ★S-G: WALKING GOLD IS GONE. Every coin tile owns its amount on its
        // SLOT now (Grid's layout is the one book), so the only pool question
        // left is "how much of the ledger is not the pouch's" -- the clamp a
        // physical op needs so a tile-backed amount can never spend the
        // pouch's share. Pinned purses ceased to be a special case: every
        // tile is what a pin used to be.
        int WalkingGold()
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return 0;
            auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
            if (!gold) return 0;
            return (std::max)(0,
                (CountOf(p, gold) + PendingLedgerDelta()) - PouchSum());
        }

        // world-drop purse by amount (same bands as the coin tiers).
        // nullptr = 1~4 G: drop a vanilla loose-gold ref instead.
        RE::TESBoundObject* SackFor(int a_value)
        {
            if (a_value >= 100) return g_sack;
            if (a_value >= 10)  return g_sackMed ? g_sackMed : g_sack;
            if (a_value >= 5)   return g_sackSmall ? g_sackSmall : g_sack;
            return nullptr;
        }
    }

    // ★★What the nth coin tile is WORTH -- and with one coin the answer is
    // the only one that can be: fill each tile to the cap in turn, and the
    // last one holds what is left.
    //
    // ★This is where retiring the tiers went wrong the first time and made the
    // gold VANISH. The tile COUNT was changed to follow the cap while this
    // still asked which BAND the remainder fell in -- so 663 G, now living in
    // one tier-0 tile, was asked "is the remainder between 1 and 4?", answered
    // no, and was valued at zero. A tile worth nothing draws nothing. Count and
    // value are one rule and have to move together.
    // ★The rule itself, with the walking total handed in. A caller filling N
    // coin tiles asks N times for a number that cannot change between the
    // questions -- and WalkingGold() reaches CountOf, which runs a filtered
    // GetInventory: an entry-list walk plus a map allocation. At 100,000 gold
    // that was a hundred inventory walks per rebuild, and the rebuild runs on
    // every player-side container delta.
    // ★S-G: InstanceValueAt / InstanceValue / CoinTileCount / CoinTilesFor
    // and WalkingGoldValue are gone with the decomposition -- every coin tile
    // owns its amount on its layout slot, and Grid::CoinIncome / CoinSpend /
    // CoinCensus are the only mechanisms that move those amounts.

    int UnsettledDelta()
    {
        return PendingLedgerDelta() + g_expected;
    }

    // ---- G4: pinned gold purses ------------------------------------------
    // ★★★ONE COIN, NOT FOUR (P2/3-5b).
    //
    // The mirror used to pick one of four coin forms by amount, so a pile of
    // ten drew differently from a pile of a thousand. It read well on the
    // player's board and nowhere else: a container has no such mirror, so gold
    // put in a chest came back as a raw thousand-in-one-cell stack with no
    // banding, no cap and no drag -- and the two sides could not be made to
    // agree without teaching the partner board four tiers it had never heard
    // of.
    //
    // So the tiers retire. Gold is ONE form with ONE picture, and the only
    // rule left is the stack cap both sides can keep: kCoinCap per tile. The
    // amount is already written on the tile as a number, which is the thing
    // players actually read.
    //
    // ★The identity stays OURS (the plugin's own coin form) rather than
    // becoming vanilla Gold001, deliberately: IsCoinForm is the gate that keeps
    // gold out of every ordinary transfer path, and widening it to vanilla gold
    // would change what that gate answers for a merchant's purse and a looted
    // chest at the same time. Same picture either way; this way nothing else
    // has to be re-checked.
    int BandTier(int)
    {
        return 0;   // retired: kept so the call sites read unchanged
    }

    RE::TESBoundObject* CoinForTier(int)
    {
        return g_coins[0];   // the one coin
    }

    RE::TESBoundObject* VanillaGold()
    {
        return RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
    }

    // ★S-G: the pin API is gone. Every coin tile owns its amount on its
    // layout slot (Grid::CoinValueOf / SetCoinRecord), which is exactly what
    // a pin used to be -- the special case became the only case.

    // ★No pouch named: a coin CLICKED into storage does not say which one.
    // Fill the fullest that still has room rather than spreading a little
    // into each -- a half-full pouch beside a half-full pouch is the state
    // nobody asked for, and it makes both icons lie about how much is left.
    int StoreToPouch(int a_value)
    {
        // ★multi-pouch cascade: finish the fullest pouch, then the next --
        // what will not fit anywhere comes back to the caller.
        int left = a_value, moved = 0;
        for (const auto& k : PouchFillOrder()) {
            if (left <= 0) break;
            const int s = StoreToPouch(k, left);
            left -= s;
            moved += s;
        }
        return moved;
    }

    int StoreToPouch(const std::string& a_tileKey, int a_value)
    {
        if (a_tileKey.empty()) return 0;
        int& held = g_pouchStored[a_tileKey];
        const int room = CapOfKey(a_tileKey) - held;   // ★the cap is PER POUCH (and per FORM now)
        const int s = (std::min)({ a_value, room, WalkingGold() });
        if (s <= 0) { if (held == 0) g_pouchStored.erase(a_tileKey); return 0; }
        held += s;
        g_dirty = true;
        SKSE::log::info("[GOLD] pouch '{}': +{} -> {}", a_tileKey, s, held);
        return s;
    }

    void WithdrawFrom(const std::string& a_tileKey, int a_value, bool a_sound)
    {
        const auto it = g_pouchStored.find(a_tileKey);
        if (it == g_pouchStored.end()) return;
        const int w = (std::min)(a_value, it->second);
        if (w <= 0) return;
        it->second -= w;
        if (it->second <= 0) g_pouchStored.erase(it);
        g_dirty = true;
        // gold putdown sfx — the coins audibly land back in your inventory
        if (a_sound) {
            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                if (auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001)) {
                    p->PlayPickUpSound(gold, false, false);
                }
            }
        }
        SKSE::log::info("[GOLD] pouch '{}': -{}", a_tileKey, w);
    }

    namespace
    {
        // SELL context = the pouch is being sold to a vendor: the stored gold
        // must pop back into walking coins first, then only the empty pouch
        // changes hands (no exploit). STORAGE/DROP instead lets the gold
        // travel with the pouch.
        // Currently detected by the vanilla BarterMenu. PLAN_LOOT_BARTER
        // Phase 5: our custom trade UI does NOT open BarterMenu, so OR in
        // `g_uiMode == kBarter` here once that global exists.
        bool g_barterContext = false;   // our custom barter UI open (Phase 5)

        bool InSellContext()
        {
            if (g_barterContext) return true;   // our grid barter UI
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::BarterMenu::MENU_NAME);
        }
    }

    void SetBarterContext(bool a_open) { g_barterContext = a_open; }

    void NotePouchLeaving(const std::string& a_tileKey)
    {
        g_leavingHint = a_tileKey;
    }

    void OnPouchLeftPlayer(RE::FormID a_form)
    {
        // ★(1.3.0-C) trust the UI's word over the guess -- and trust it even
        // when the named tile holds NOTHING: storing an empty pouch while a
        // full one stays behind must move zero gold ("fullest" moved the full
        // one's). The guess survives only for departures the UI never saw
        // (scripts, gift menu).
        std::string key = g_leavingHint;
        g_leavingHint.clear();
        if (key.empty()) key = FullestPouch();
        const int carried = PouchStoredOf(key);
        if (carried <= 0) return;

        // SALE: the vendor buys the pouch at its base (empty) value — the
        // stored gold pops back into walking coins first (no exploit).
        if (InSellContext()) {
            SKSE::log::info("[GOLD] pouch sold -> releasing {} G", carried);
            WithdrawFrom(key, carried);
            // ★S-G: the released claim is tile-share now -- mint it at once.
            // The mirror used to materialise "walking" gold here; without
            // this the coins stayed invisible until the next menu-open
            // census squared them (user report: sold pouch, gold unseen
            // until the inventory was reopened).
            Grid::CoinIncome(carried);
            return;
        }

        // STORAGE / DROP: the gold TRAVELS with the pouch (space-penalty
        // design: banking gold away is the point). Stored -> away immediately
        // (logical state), and a kPouchLeave op debits the ledger on Tick.
        // WalkingGold is unchanged this frame (stored and ledger fall together).
        SKSE::log::info("[GOLD] pouch '{}' left with {} G inside ({} pouch tile(s) left)",
            key, carried, g_pouchStored.size() - 1);
        // ★the parcel's form is the DRAINED BOOK's own pouch -- the event's
        // baseObj can disagree when a bag takes several pouches out in one
        // gesture (the hint queue is one slot deep), and a mismatched pair
        // is exactly the 663<->10,000 swap the bundle test measured.
        const RE::FormID pf = FormOfKey(key);
        g_awayParcels.push_back({ pf != 0 ? pf : a_form, carried });
        g_pending.push_back({ LedgerOp::kPouchLeave, carried });
        g_pouchStored.erase(key);
        g_dirty = true;
    }

    void OnPouchReturned(RE::FormID a_form)
    {
        if (g_awayParcels.empty()) {
            SKSE::log::info("[GOLD] pouch returned but nothing was away");
            return;
        }
        // ★multi-pouch: the parcel this return means -- same-form FIFO, the
        // oldest as the legacy fallback. Its own form's cap bounds the
        // parking; the spill is tiled at once (S-G).
        const std::size_t pi = ParcelFor(a_form);
        const int amount = g_awayParcels[pi].amount;
        const RE::FormID pform = g_awayParcels[pi].form;
        g_awayParcels.erase(g_awayParcels.begin() + static_cast<std::ptrdiff_t>(pi));
        SKSE::log::info("[GOLD] pouch returned with {} G inside", amount);
        g_pending.push_back({ LedgerOp::kPouchReturn, amount });
        // ★(1.5.x) one return, one parcel (see ClaimReturned)
        g_returnPark.push_back({ pform != 0 ? pform : a_form, amount });
        g_returnFreshGrace = 4;   // (1.3.0) the pouch that just walked in claims this
        g_parkMintDelay = 12;     // and the mint waits out a slow arrival
        g_dirty = true;
    }

    namespace
    {
        // The actual drop, run on the GAME thread from Tick (PlaceObjectAtMe
        // in the render pass leaves the ref's 3D unattached — invisible bag).
        // GI53: returns the gold actually debited (0 when placement failed),
        // so the caller's lastLedger stays honest.
        int ProcessDrop(RE::PlayerCharacter* a_p, RE::TESBoundObject* a_gold, int a_value)
        {
            const int v = (std::min)(a_value, CountOf(a_p, a_gold));
            if (v <= 0) return 0;

            a_p->PlayPickUpSound(a_gold, false, false);
            // purse size follows the amount (same bands as the coin tiers);
            // 1~4 G has no purse — a vanilla loose-gold ref reads better
            auto* sack = SackFor(v);
            if (sack) {
                // Direct placement — no inventory round-trip, so loot-HUD
                // widgets don't spam "Coinpurse received". The display name
                // carries the stored amount.
                if (auto ref = a_p->PlaceObjectAtMe(sack, false)) {
                    const RE::NiPoint3 ppos = a_p->GetPosition();
                    const float heading = a_p->GetAngleZ();
                    ref->SetPosition(RE::NiPoint3(
                        ppos.x + 60.0f * std::sin(heading),
                        ppos.y + 60.0f * std::cos(heading),
                        ppos.z + 8.0f));
                    char name[96];
                    std::snprintf(name, sizeof(name), "%s (%dG)",
                        sack->GetName() ? sack->GetName() : "?", v);
                    ref->SetDisplayName(name, true);
                    g_sackRefs[ref->GetFormID()] = v;
                    a_p->RemoveItem(a_gold, v, RE::ITEM_REMOVE_REASON::kRemove,
                        nullptr, nullptr);
                    SKSE::log::info("[GOLD] dropped a {} G sack ({:08X})",
                        v, ref->GetFormID());
                    return v;
                }
                SKSE::log::error("[GOLD] sack placement failed — gold kept");
                return 0;
            }
            // 1~4 G (or missing records): vanilla loose-gold ref
            a_p->RemoveItem(a_gold, v, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
            SKSE::log::info("[GOLD] dropped {} G to the world", v);
            return v;
        }

        // Apply one deferred op on the game thread. It also adjusts g_lastLedger
        // by its own ledger delta so the auto-store step that follows sees ONLY
        // external pickups, never our own mutations (order-independent).
        void ApplyLedgerOp(RE::PlayerCharacter* a_p, RE::TESBoundObject* a_gold,
                           const LedgerOp& a_op)
        {
            switch (a_op.kind) {
            case LedgerOp::kDropCoin:
                {
                    const int d = ProcessDrop(a_p, a_gold, a_op.value);
                    if (g_lastLedger >= 0) g_lastLedger -= d;
                }
                break;
            case LedgerOp::kStoreCoin:
                {
                    // ★Clamped to what the player actually has: a payment
                    // queued the same tick may have run first, and promising
                    // more than the purse holds is how a ledger drifts.
                    auto* dst = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_op.target);
                    const int d = (std::min)(a_op.value, CountOf(a_p, a_gold));
                    if (dst && d > 0) {
                        a_p->RemoveItem(a_gold, d, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                        nullptr, dst);
                        a_p->PlayPickUpSound(a_gold, false, false);
                        SKSE::log::info("[GOLD] {} G stored into '{}'", d,
                            dst->GetName() ? dst->GetName() : "?");
                    }
                    if (g_lastLedger >= 0) g_lastLedger -= d;
                }
                break;
            case LedgerOp::kPouchLeave:
                {
                    const int d = (std::min)(a_op.value, CountOf(a_p, a_gold));
                    if (d > 0) {
                        a_p->RemoveItem(a_gold, d, RE::ITEM_REMOVE_REASON::kRemove,
                            nullptr, nullptr);
                    }
                    // GI53: the ledger could cover only d of the promised value
                    // (a payment queued the same tick ran first). awayGold must
                    // shrink by the shortfall or the pouch comes back with free
                    // gold; lastLedger must track the ACTUAL debit or the next
                    // tick reads the difference as an external pickup.
                    if (d < a_op.value && !g_awayParcels.empty()) {
                        auto& last = g_awayParcels.back();   // the departure this op serves
                        last.amount = (std::max)(0, last.amount - (a_op.value - d));
                        if (last.amount <= 0) g_awayParcels.pop_back();
                    }
                    if (g_lastLedger >= 0) g_lastLedger -= d;
                }
                break;
            case LedgerOp::kPouchReturn:
                a_p->AddObjectToContainer(a_gold, nullptr, a_op.value, nullptr);
                if (g_lastLedger >= 0) g_lastLedger += a_op.value;
                break;
            case LedgerOp::kDebit:
                // (1.3.2a) shelf-pouch deposit: the value just left a coin
                // tile (walking), so the ledger covers it -- clamp anyway
                {
                    const int d = (std::min)(a_op.value, CountOf(a_p, a_gold));
                    if (d > 0) {
                        a_p->RemoveItem(a_gold, d, RE::ITEM_REMOVE_REASON::kRemove,
                            nullptr, nullptr);
                    }
                    if (g_lastLedger >= 0) g_lastLedger -= d;
                }
                break;
            }
        }
    }

    // ★★RETURNS WHAT IT ACTUALLY QUEUED, because it can queue nothing and the
    // caller has no other way to find out. The clamp to walking gold is right
    // -- this must never move money the player does not have -- but a caller
    // that read a refusal as success unpinned the purse and erased its cell
    // while the coins stayed put, which is how 663 G left a chest and came
    // back as a tile in the first free square (reported). See the callers:
    // a PINNED purse has to come home to walking before it can be spent.
    int StoreToContainer(RE::TESObjectREFR* a_dst, int a_value)
    {
        if (!a_dst || a_value <= 0) return 0;
        const int v = (std::min)(a_value, WalkingGold());
        if (v <= 0) {
            SKSE::log::warn("[GOLD] store of {} G declined -- only {} G is walking "
                            "(pinned {}, pouch {})",
                            a_value, WalkingGold(), 0, PouchSum());
            return 0;
        }
        g_pending.push_back({ LedgerOp::kStoreCoin, v, a_dst->GetFormID() });
        g_dirty = true;
        return v;
    }

    void DropAsGold(int a_value)
    {
        // Enqueue only — WalkingGold subtracts pending ops immediately, so the
        // coin tile disappears THIS frame; the ledger debit + sack spawn happen
        // on the next Tick (game thread). This keeps tiles == slots during the
        // rebuild, so the dropped cell stays the emptied one.
        const int v = (std::min)(a_value, WalkingGold());
        if (v <= 0) return;
        g_pending.push_back({ LedgerOp::kDropCoin, v });
        g_dirty = true;
    }

    bool TryPickUpSack(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) return false;
        auto* base = a_ref->GetBaseObject();
        if (!base) return false;
        if (base != g_sack && base != g_sackMed && base != g_sackSmall) return false;

        int v = 0;
        if (const auto it = g_sackRefs.find(a_ref->GetFormID()); it != g_sackRefs.end()) {
            v = it->second;
            g_sackRefs.erase(it);
        } else if (const char* nm = a_ref->GetDisplayFullName()) {
            // GI53: a sack the map no longer knows (lost cosave entry) must not
            // be destroyed for 0 G. The amount also rides the display name
            // "<sack> (1234G)" -- recover it from there.
            if (const char* par = std::strrchr(nm, '(')) v = std::atoi(par + 1);
        }
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
        if (!p || !gold || v <= 0) {
            SKSE::log::error("[GOLD] sack {:08X} not redeemable -- left in place",
                a_ref->GetFormID());
            return true;   // swallow the activation, keep the sack (no 0 G burn)
        }
        p->PlayPickUpSound(gold, true, false);
        p->AddObjectToContainer(gold, nullptr, v, nullptr);
        a_ref->Disable();
        a_ref->SetDelete(true);
        g_dirty = true;
        SKSE::log::info("[GOLD] picked up a {} G sack", v);
        return true;
    }

    void MarkDirty() { g_dirty = true; }

    void Tick()
    {
        if (!g_dirty || g_applying || !Ready()) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !p->Is3DLoaded()) return;
        g_dirty = false;

        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
        if (!gold) return;

        // STEP 1 — deferred ledger ops (coin drops + pouch leave/return). All
        // engine gold mutations from the render pass / event sinks land here on
        // the game thread. Each op self-adjusts g_lastLedger so step 2 sees
        // only external pickups. WalkingGold already reflected these ops the
        // frame they were queued (PendingLedgerDelta), so the UI never lagged.
        if (!g_pending.empty()) {
            g_applying = true;
            for (const auto& op : g_pending) ApplyLedgerOp(p, gold, op);
            g_pending.clear();
            g_applying = false;
        }

        // STEP 2 — auto-store: while a pouch is held, gold newly PICKED UP from
        // OUTSIDE (loot, sale income, rewards) goes straight into the pouch;
        // gold you were already walking with is left alone (banked manually).
        // Our own ops in step 1 already moved g_lastLedger, so only external
        // increases register here. First tick after load is skipped
        // (g_lastLedger < 0) so starting gold isn't swept in.
        {
            const int pre = CountOf(p, gold);
            int gain = (g_lastLedger >= 0) ? (std::max)(0, pre - g_lastLedger) : 0;
            // ★S-0: OUR OWN ARRIVALS ARE CLAIMED FIRST, and never swept.
            // An announced transfer is gold the player placed by hand; the
            // sweep is for gold that turned up. Settling the announcement here
            // rather than in a step of its own is deliberate -- this is the
            // only place that knows how much actually landed, so it is the
            // only place that can retire the promise honestly.
            if (g_expected > 0 && gain > 0) {
                const int ours = (std::min)(g_expected, gain);
                g_expected -= ours;
                gain -= ours;
                SKSE::log::info("[GOLD] {} G of the announced amount arrived "
                                "({} still expected)", ours, g_expected);
            }
            if (gain > 0 && PouchHeld()) {   // ★multi-pouch cascade
                for (const auto& key : PouchFillOrder()) {
                    if (gain <= 0) break;
                    int& held = g_pouchStored[key];
                    const int room = CapOfKey(key) - held;
                    const int store = (std::min)(gain, room);
                    if (store > 0) {
                        held += store;
                        gain -= store;   // ★S-G: swept gold is banked, not tiled
                        SKSE::log::info("[GOLD] auto-stored {} G into '{}' -> {}",
                            store, key, held);
                    }
                }
            }
            // ★S-G: the LEDGER MOVED, so the TILES move -- and Grid owns the
            // tiles now. Unexpected income fills the rear partial tile and
            // mints what will not fit; a spend debits partials before full
            // thousands, rear board position first. Both live in ONE place
            // (Grid::CoinIncome / CoinSpend), which is what §7 meant by "the
            // allocation rule is the only mechanism left" -- the walking
            // residual, the decomposition and the mirror that reconciled them
            // are gone.
            if (gain > 0) Grid::CoinIncome(gain);
            const int spend = (g_lastLedger >= 0) ? (std::max)(0, g_lastLedger - pre)
                                                  : 0;
            if (spend > 0) Grid::CoinSpend(spend);
            // ★The promise expires. A request that was refused downstream, or
            // a transfer that simply never landed, must not leave walking gold
            // permanently inflated -- the board would show coins that are not
            // there, and every later reconcile would trust the lie.
            if (g_expected > 0) {
                if (++g_expectedAge > kExpectGrace) {
                    SKSE::log::warn("[GOLD] {} G was announced but never arrived "
                                    "-- forgetting it", g_expected);
                    g_expected = 0;
                    g_expectedAge = 0;
                } else {
                    g_dirty = true;   // keep ticking until it settles or expires
                }
            } else {
                g_expectedAge = 0;
            }
        }

        // STEP 3 — baseline snapshot + mirror the walking gold into coins.
        const int onHand = CountOf(p, gold);   // coins actually in the inventory
        g_lastLedger = onHand;                 // auto-store baseline: ALWAYS the
                                               // real count, or the arrival that
                                               // settles the promise above could
                                               // never be seen to arrive.
        // ★★S-0: EVERY CLAIM BELOW IS MEASURED AGAINST THE LEDGER THE BOARD IS
        // DRAWING, which is the on-hand count plus whatever transfer has been
        // announced -- exactly the total WalkingGold() reports (g_pending was
        // applied and cleared in STEP 1, so the announcement is all that is
        // left of the delta).
        //
        // Using the raw count here would undo the announcement one line after
        // making it, and worse than that: the trims below would see a pinned
        // purse worth gold that has not landed yet and TRIM THE PURSE AWAY.
        // The player would watch the amount they just dragged out of a chest
        // shrink on the cursor.
        const int total = onHand + g_expected;

        // the pouch can never hold more than you own (shops spend the ledger)
        // ★Each pouch is capped on its own (kPouchCap is PER POUCH), and the
        // SUM can never exceed the ledger -- shops spend gold the pouches
        // still claim. Trim the last key first, exactly as the pinned purses
        // below are trimmed.
        for (auto it = g_pouchStored.begin(); it != g_pouchStored.end();) {
            if (it->second > CapOfKey(it->first)) it->second = CapOfKey(it->first);
            if (it->second <= 0) it = g_pouchStored.erase(it); else ++it;
        }
        // ★The trim ORDER is a rule, not a map accident (the pin trim's
        // lesson, said again for pouches): the unclaimed PARKING float pays
        // first, then the LESS-FULL pouch, rear board position breaking
        // ties -- the same ladder every keyless coin rule walks.
        if (PouchSum() > total) {
            int over = PouchSum() - total;
            // the unclaimed PARKING float pays first (any bucket)
            for (auto pk = g_returnPark.begin();
                 over > 0 && pk != g_returnPark.end();) {
                const int cut = (std::min)(over, pk->amount);
                over -= cut;
                pk->amount -= cut;
                SKSE::log::info("[GOLD] park pays: {} G (the spend reached "
                                "past the tiles)", cut);
                pk = pk->amount <= 0 ? g_returnPark.erase(pk) : std::next(pk);
            }
            std::vector<std::string> keys;
            keys.reserve(g_pouchStored.size());
            for (const auto& [k, v] : g_pouchStored) keys.push_back(k);
            const auto byPos = Grid::OrderKeysByPosition(keys);   // front-first
            std::map<std::string, int> rank;
            int r = 0;
            for (const auto& k : byPos) rank[k] = r++;
            std::sort(keys.begin(), keys.end(),
                [&](const std::string& a, const std::string& b) {
                    const int va = g_pouchStored[a], vb = g_pouchStored[b];
                    if (va != vb) return va < vb;               // less-full first
                    return rank[a] > rank[b];                   // rear first
                });
            for (const auto& k : keys) {
                if (over <= 0) break;
                auto pi2 = g_pouchStored.find(k);
                if (pi2 == g_pouchStored.end()) continue;
                const int cut = (std::min)(over, pi2->second);
                over -= cut;
                pi2->second -= cut;
                SKSE::log::info("[GOLD] pouch pays: '{}' -{} -> {} G (the "
                                "spend reached past the tiles)", k, cut,
                    pi2->second);
                if (pi2->second <= 0) g_pouchStored.erase(pi2);
            }
        }

        // ★S-G: THE MIRROR IS GONE. Coin tiles are owned slots in Grid's
        // layout and are emitted from it -- there is no per-tier item count to
        // reconcile, so the trims and the AddObject/RemoveItem loop that kept
        // it honest have nothing left to keep honest. What replaces them is
        // one invariant, checked and repaired in Grid::CoinCensus:
        //     Σ tile amounts == ledger − pouch
        //
        // The PHYSICAL coin items the mirror used to maintain are legacy now.
        // Purged here (once per load, and again if anything re-mints them):
        // nothing re-adds them, so a non-zero count only ever means an old
        // save arriving.
        g_applying = true;
        for (int i = 0; i < 4; ++i) {
            if (!g_coins[i]) continue;
            const int cur = CountOf(p, g_coins[i]);
            if (cur > 0) {
                p->RemoveItem(g_coins[i], cur, RE::ITEM_REMOVE_REASON::kRemove,
                    nullptr, nullptr);
                SKSE::log::info("[GOLD] ★S-G purge: {} legacy coin item(s) of "
                                "tier {} removed (tiles own their amounts now)",
                    cur, i);
            }
        }
        g_applying = false;
    }

    // ---- cosave: pouch stored amount (+v2: world sack refs) ----
    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kRecordType, kVersion)) return;
        // ★The v1 field STAYS IN ITS SLOT. Everything after it is read by
        // offset, so moving the head of the record would make every older
        // save parse garbage from here on. New data goes at the END.
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(PouchSum()));
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_sackRefs.size()));
        for (const auto& [id, val] : g_sackRefs) {
            a_intfc->WriteRecordData(id);
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(val));
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(AwaySum()));   // v3 slot: the SUM
        // v4 slot: the pinned map -- ★S-G writes it EMPTY. The field stays
        // in place so older readers keep their offsets; the amounts live on
        // the layout slots (Grid cosave) and have for several versions.
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(0));
        // v5: which restock cycle each merchant was last stocked for
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_vendorCycle.size()));
        for (const auto& [id, cyc] : g_vendorCycle) {
            a_intfc->WriteRecordData(id);
            a_intfc->WriteRecordData(cyc);
        }
        // v6: per-tile pouch amounts. The uint32 at the head is now only
        // there for older readers -- this is the authority.
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_pouchStored.size()));
        for (const auto& [key, val] : g_pouchStored) {
            WriteStr(a_intfc, key);
            a_intfc->WriteRecordData(static_cast<std::int32_t>(val));
        }
        // v7: the away parcels (which FORM left carrying what)
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_awayParcels.size()));
        for (const auto& a : g_awayParcels) {
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(a.form));
            a_intfc->WriteRecordData(static_cast<std::int32_t>(a.amount));
        }
        // v8: the per-form return park (money home before its tile)
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_returnPark.size()));
        for (const auto& pc : g_returnPark) {
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(pc.form));
            a_intfc->WriteRecordData(static_cast<std::int32_t>(pc.amount));
        }
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        std::uint32_t v = 0;
        if (a_intfc->ReadRecordData(v)) {
            // ★A save older than v6 carries ONE amount for the whole player.
            // It is parked on the reserved key and the first rebuild hands it
            // to a real pouch tile -- the same road a returning pouch takes,
            // so there is one path to test instead of two. From v6 the map at
            // the tail replaces it outright.
            const int one = static_cast<int>((std::min)(v, static_cast<std::uint32_t>(kPouchCap)));
            if (a_version < 6 && one > 0) {
                g_returnPark.push_back({ 0, one });   // untagged
            }
        }
        if (a_version >= 2) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 4096) {
                // GI53: an impossible count means the record is corrupt. The n
                // entries are still IN the stream -- skipping the loop but
                // reading on would parse sack pairs as awayGold (free gold).
                SKSE::log::error("[GOLD] GPCH sack count {} rejected -- record dropped", n);
                return;
            }
            {
                for (std::uint32_t i = 0; i < n; ++i) {
                    RE::FormID id = 0;
                    std::uint32_t val = 0;
                    if (!a_intfc->ReadRecordData(id) || !a_intfc->ReadRecordData(val)) break;
                    RE::FormID resolved = 0;
                    if (a_intfc->ResolveFormID(id, resolved)) {
                        g_sackRefs[resolved] = static_cast<int>(val);
                    }
                }
            }
        }
        if (a_version >= 3) {
            std::uint32_t away = 0;
            if (a_intfc->ReadRecordData(away)) {
                // legacy: one untagged parcel; a v7 tail replaces it below
                g_awayParcels.clear();
                if (away > 0) {
                    g_awayParcels.push_back(
                        { 0, static_cast<int>((std::min)(away, 1000000u)) });
                }
            }
        }
        if (a_version >= 4) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 65536) {   // GI53: corrupt count -> stop (see v2 note)
                SKSE::log::error("[GOLD] GPCH pin count {} rejected -- record dropped", n);
                return;
            }
            {
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::string  key;
                    std::int32_t val = 0;
                    if (!ReadStr(a_intfc, key) || !a_intfc->ReadRecordData(val)) break;
                    // ★S-G: read for the stream position, kept nowhere -- the
                    // layout slot has carried the same amount since G6, and
                    // CoinCensus squares any pre-G6 drift against the ledger.
                }
            }
        }
        if (a_version >= 5) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 65536) {   // GI53: corrupt count -> stop (see v2 note)
                SKSE::log::error("[GOLD] GPCH vendor count {} rejected -- record dropped", n);
                return;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::FormID    id = 0;
                std::uint32_t cyc = 0;
                if (!a_intfc->ReadRecordData(id) || !a_intfc->ReadRecordData(cyc)) break;
                RE::FormID resolved = 0;
                if (a_intfc->ResolveFormID(id, resolved)) g_vendorCycle[resolved] = cyc;
            }
        }
        if (a_version >= 6) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 4096) {   // corrupt count -> stop (see the v2 note)
                SKSE::log::error("[GOLD] GPCH pouch count {} rejected -- record dropped", n);
                return;
            }
            g_pouchStored.clear();   // the v1 field was only a fallback
            for (std::uint32_t i = 0; i < n; ++i) {
                std::string  key;
                std::int32_t val = 0;
                if (!ReadStr(a_intfc, key) || !a_intfc->ReadRecordData(val)) break;
                if (val <= 0) continue;
                if (key == kReturnKey) {
                    g_returnPark.push_back({ 0, val });   // old parked float
                } else {
                    g_pouchStored[key] = (std::min)(val, CapOfKey(key));
                }
            }
        }
        if (a_version >= 7) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 4096) {
                SKSE::log::error("[GOLD] GPCH parcel count {} rejected -- tail dropped", n);
                return;
            }
            g_awayParcels.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t form = 0;
                std::int32_t  amt = 0;
                if (!a_intfc->ReadRecordData(form) || !a_intfc->ReadRecordData(amt)) break;
                if (amt > 0) g_awayParcels.push_back({ form, amt });
            }
        }
        if (a_version >= 8) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 4096) {
                SKSE::log::error("[GOLD] GPCH park count {} rejected -- tail dropped", n);
                return;
            }
            g_returnPark.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t form = 0;
                std::int32_t  amt = 0;
                if (!a_intfc->ReadRecordData(form) || !a_intfc->ReadRecordData(amt)) break;
                if (amt > 0) g_returnPark.push_back({ form, amt });
            }
        }
        g_dirty = true;
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        g_pouchStored.clear();
        g_sackRefs.clear();
        g_awayParcels.clear();
        g_returnPark.clear();
        g_leavingHint.clear();   // (1.3.0-C) a hint from the previous save is a lie
        g_returnFreshGrace = 0;  // (1.3.0) ...and so is a grace from one
        g_parkMintDelay = 0;
        // per-save state: a new game must not inherit the last save's cycles,
        // or its first merchants look "already stocked" and skip a rotation
        g_vendorCycle.clear();
        // per-LOAD state: the re-seed allowance is renewed by every load, which
        // is exactly when the chest respawn that spends it happens
        g_reseeded.clear();
        g_pending.clear();
        g_lastLedger = -1;   // skip auto-store on the first tick after load
        // ★S-0: a promise about a transfer in the PREVIOUS save describes gold
        // this one has never seen. Same reason g_leavingHint is cleared above.
        g_expected = 0;
        g_expectedAge = 0;
        g_dirty = true;
    }
}
