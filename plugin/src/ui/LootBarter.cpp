#include "ui/Badges.h"
#include "ui/Editor.h"
#include "ui/Equip.h"
#include "ui/Fallback.h"
#include "ui/LootBarter.h"
#include "game/Ledger.h"

#include "game/BagFilter.h"
#include "game/GoldCoins.h"
#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <set>
#include <vector>

namespace FUI::LootBarter
{
    // the ref the wares actually live in: a container/corpse IS the partner,
    // a merchant's stock is the vendor faction's container. Declared up here
    // because the ledgers below need to ask it what is already on the shelf.
    RE::TESObjectREFR* SourceRef();
    // (defined with the other board writes) split a shelf cell in two and
    // give the smaller half to the cursor -- the slider's confirm needs it
    // before the definitions further down.
    std::string SplitShelfCell(const std::string& a_key, int a_count);

    namespace
    {
        Mode                g_mode = Mode::kNormal;
        RE::ObjectRefHandle g_partner;

        bool g_partnerHovered = false;   // mouse over partner window (drag-to-store)

        // deferred item transfers — applied on Tick, game thread. Loot uses
        // kTake/kStore (no gold); barter uses kBuy/kSell (price settled too).
        struct XferReq
        {
            enum Dir { kTake, kStore, kBuy, kSell, kPickTake, kPickStore };
            Dir                 dir;
            RE::TESBoundObject* obj;
            int                 count;
            int                 price = 0;   // total gold for kBuy/kSell
            int                 base = 0;    // total BASE value (speech XP points)
            std::string         srcKey;      // kPickStore: tile the units leave
            // D4: WHICH sub-stack moves. 0/-1 = let the engine choose, which is
            // still right for fungible goods (arrows, ingots, gold).
            std::uint16_t       uid = 0;
            // GI25: the POOL, not a list position. A transfer runs on the next
            // Tick, and by then an index captured at click time can be stale --
            // the signature cannot be, it travels with the ExtraDataList.
            std::uint16_t       sig = 0;
            // The cell this came from was WORN. A worn unit with no signature
            // (a plain equipped dagger) cannot be named by pool at all -- uid 0
            // and sig 0 resolve to nullptr, the engine chooses, and a TEMPERED
            // spare in the same inventory can walk out instead of the one on the
            // body. The worn list is the only handle such a unit has.
            bool                fromWorn = false;
            // GI36: the outgoing units wore a star -- rule 58 kills it at the sink.
            bool                fav = false;
            // ★(1.3.3) ...and WHERE it sat, for the store/sell sinks. The pool
            // is a crowd -- two identical daggers share one signature, so the
            // sink took whichever list came first and the front one was never
            // what left. The position is VERIFIED at the sink (same pool,
            // unworn) so a stale one costs accuracy, never correctness -- which
            // is what the note above was guarding against.
            int                 xlIdx = -1;
            // ★(1.3.0) USE MODE: consume / read / learn the moment it lands,
            // rather than merely landing.
            // ★★LAST, and this time actually last. The first attempt put it
            // after `fromWorn` while calling it "appended last", and the three
            // braced initialisers further down silently shifted by one field --
            // the compiler caught it only because bool/int narrowing happened to
            // be involved. The same shape cost a whole test round in
            // OffBoardUnit, where no narrowing saved it. New fields go at the
            // END, and the site that sets this one builds the struct FIELD-WISE.
            bool                useAfter = false;
        };
        std::vector<XferReq> g_xfer;

        // ---- units this side has committed to hand over -----------------------
        //
        // The mirror of the player board's pending-remove. Taking, buying or
        // lifting drops the carry the instant the player commits, but the engine
        // does not move the item until the next Tick -- and CollectPartnerCells
        // reads the engine directly. For those frames the cell SAT BACK DOWN on
        // the partner board and was then snatched away: a blink on every drag
        // out of a container, corpse, shop or pocket.
        //
        // Keyed by (form, uid, sig) so a tempered unit on its way out never hides
        // a plain sibling that is staying put.
        std::map<std::string, int>            g_outPool;
        std::map<RE::FormID, int>             g_outForm;
        std::chrono::steady_clock::time_point g_outWhen{};
        constexpr std::chrono::seconds        kOutTTL{ 3 };

        std::string OutKey(RE::FormID a_id, std::uint16_t a_uid, std::uint16_t a_sig)
        {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%08X@%04X~%04X", a_id, a_uid, a_sig);
            return buf;
        }

        void NoteOut(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                     int a_count)
        {
            if (!a_obj || a_count <= 0) return;
            g_outPool[OutKey(a_obj->GetFormID(), a_uid, a_sig)] += a_count;
            g_outForm[a_obj->GetFormID()] += a_count;
            g_outWhen = std::chrono::steady_clock::now();
        }

        void ClearOut(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                      int a_count)
        {
            if (!a_obj || a_count <= 0) return;
            const auto k = OutKey(a_obj->GetFormID(), a_uid, a_sig);
            if (auto it = g_outPool.find(k); it != g_outPool.end()) {
                it->second -= a_count;
                if (it->second <= 0) g_outPool.erase(it);
            }
            if (auto it = g_outForm.find(a_obj->GetFormID()); it != g_outForm.end()) {
                it->second -= a_count;
                if (it->second <= 0) g_outForm.erase(it);
            }
        }

        // A request that never reached the engine (menu closed, roll lost, item
        // gone) must not keep hiding cells for the rest of the session.
        void SweepOut()
        {
            if (g_outForm.empty()) return;
            if (std::chrono::steady_clock::now() - g_outWhen > kOutTTL) {
                g_outPool.clear();
                g_outForm.clear();
            }
        }

        // ---- units this side has been PROMISED --------------------------------
        //
        // ★★THE EXACT MIRROR OF g_outPool, and the piece the board model needs
        // that the derivation never did. A store is queued from the render pass
        // and applied on the next Tick, so for those few frames the container
        // does not hold the units yet -- while the board ALREADY shows a cell for
        // them, because the drop is what created that cell. Without this the
        // reconcile would find a cell the engine cannot account for and shrink it
        // away before it ever drew: the item would blink out and reappear
        // somewhere else, which is the store half of every placement complaint.
        //
        // Same TTL escape as the out ledger: a request that never reached the
        // engine must not prop up a phantom cell for the rest of the session.
        struct InUnits
        {
            RE::FormID    form = 0;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            int           count = 0;
            // ★★HOW MUCH OF THIS FORM THE CONTAINER HELD WHEN THE PROMISE WAS
            // MADE, which is what lets the promise retire ITSELF.
            //
            // The first version relied on the transfer's own handler calling
            // ClearIn -- fine for the ordinary store, and quietly wrong for
            // gold, which does not travel that road at all: GoldCoins moves it
            // on its own queue. Nothing retired those promises, so for the
            // three seconds until the sweep the reconcile counted the septims
            // twice, grew a phantom cell for the surplus, and then -- shrinking
            // from the back, as it should -- took away the square the player
            // had aimed at and left the phantom in the front gap. Exactly the
            // report.
            //
            // Comparing against the engine needs no one to remember to call
            // anything: when the count has risen by what we promised, it
            // arrived, whichever road it came down.
            int seenAt = 0;
        };
        std::map<std::string, InUnits>        g_in;
        std::chrono::steady_clock::time_point g_inWhen{};

        // How many of a_obj a reference is holding right now.
        int RefCountOf(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_obj)
        {
            if (!a_ref || !a_obj) return 0;
            int n = 0;
            for (auto& [o, d] :
                 a_ref->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; })) {
                n += d.first;
            }
            return n;
        }

        void NoteIn(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                    int a_count)
        {
            if (!a_obj || a_count <= 0) return;
            const auto k = OutKey(a_obj->GetFormID(), a_uid, a_sig);
            const bool fresh = !g_in.contains(k);
            auto&      e = g_in[k];
            e.form = a_obj->GetFormID();
            e.uid = a_uid;
            e.sig = a_sig;
            e.count += a_count;
            // ★Only a FRESH promise takes a baseline: a second store while the
            // first is still in flight is more units owed against the same
            // starting point, not a new starting point.
            if (fresh) e.seenAt = RefCountOf(SourceRef(), a_obj);
            g_inWhen = std::chrono::steady_clock::now();
        }

        void ClearIn(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                     int a_count)
        {
            if (!a_obj || a_count <= 0) return;
            const auto k = OutKey(a_obj->GetFormID(), a_uid, a_sig);
            if (auto it = g_in.find(k); it != g_in.end()) {
                it->second.count -= a_count;
                if (it->second.count <= 0) g_in.erase(it);
            }
        }

        void SweepIn()
        {
            if (g_in.empty()) return;
            if (std::chrono::steady_clock::now() - g_inWhen > kOutTTL) g_in.clear();
        }

        // Vanilla speech XP for a barter transaction: the skill-use points are
        // the BASE value of the goods (the haggled price doesn't matter), fed
        // through the AVIF skill-use curve xp = useMult * points + offsetMult.
        // Read the Speech AVSK live so skill-tweak mods apply automatically.
        float SpeechXP(int a_baseTotal)
        {
            if (a_baseTotal <= 0) return 0.0f;
            float useMult = 0.55f, offset = 0.0f;   // vanilla Speech AVSK defaults
            if (auto* list = RE::ActorValueList::GetSingleton()) {
                if (auto* info = RE::ActorValueList::GetActorValueInfo(
                        RE::ActorValue::kSpeech);
                    info && info->skill) {
                    useMult = info->skill->useMult;
                    offset = info->skill->offsetMult;
                }
            }
            return useMult * static_cast<float>(a_baseTotal) + offset;
        }

        // quantity slider (Shift+right-click on a stack)
        struct SliderState
        {
            bool                active = false;
            RE::TESBoundObject* obj = nullptr;
            int                 max = 1;
            int                 value = 1;
            XferDir             dir = XferDir::kTake;
            std::string         srcKey;      // kPickup: tile the split is taken from
            std::uint16_t       uid = 0;     // GI25: the pool the units leave
            std::uint16_t       sig = 0;
            int                 unitValue = 0;   // kBuy/kSell: base value per unit
            bool                worn = false;    // the cell came off the body
            bool                fav = false;     // GI36
            int                 xlIdx = -1;      // (1.3.3) which unit leaves
        };
        SliderState g_slider;

        // Phase 5: favorite-sale confirm popup — a single favorited item asks
        // before selling (prevents fat-finger loss of a marked item).
        struct SellConfirm
        {
            bool                active = false;
            RE::TESBoundObject* obj = nullptr;
            int                 count = 0;
            int                 price = 0;
            int                 base = 0;   // total base value (speech XP points)
            std::string         srcKey;     // tile the sale drains (in-place removal)
            std::uint16_t       uid = 0;     // GI25
            std::uint16_t       sig = 0;
            bool                fav = false;   // GI36
            int                 xlIdx = -1;    // (1.3.3) which unit leaves
        };
        SellConfirm g_confirm;

        // F3/F4 merchant option toggles (settings window; persisted in the
        // ui ini by WinManager alongside skin/lang).
        bool g_merchGoldInf = false;
        bool g_merchBuysAll = false;

        // ---- THE CONTAINER'S BOARD ----------------------------------------
        //
        // ★★★A CELL, NOT A REMEMBERED POSITION. This is the change the whole
        // module turns on, so it is worth saying why plainly.
        //
        // The player's board keeps a real layout: a tile owns its position AND
        // its count, and the engine inventory is something the rebuild
        // RECONCILES against. Moving a tile is two field writes. This side used
        // to keep only positions, and rebuild every cell from the engine every
        // frame -- then run a three-tier matcher to guess which freshly derived
        // cell inherited which remembered position. There was no icon to move:
        // the cell the player dragged stopped existing at the end of the frame.
        //
        // While a form owned exactly one cell there was nothing to guess between
        // and none of this showed. The moment a form owned several -- gold, and
        // then every stack -- the guessing became the behaviour: dragging one
        // cell moved another, stores landed in the front gap, amounts shuffled.
        // Those were not separate bugs.
        //
        // So the cell is the state now. The engine total is an input to a
        // reconcile pass, never the source of cells, and every operation is a
        // write: move = col/row, merge = count, take = count, store = a new
        // cell. The matcher, the pending-spot queue and the acting-spot dance
        // are gone with the derivation that needed them.
        //
        // Key = an OPAQUE id ("Plugin.esp|0xLocalID" possibly with #n). Nothing
        // parses it any more -- what the cell holds is in the fields below.
        struct ContCell
        {
            // ---- WHAT ----
            // ★The form used to be recovered by parsing the key, which is what
            // made a cell's identity depend on its NAME. It is a field.
            RE::FormID form = 0;
            int        count = 0;   // ★units in this cell (never above the cap)
            int        xlIdx = -1;  // which extra list, for a per-unit cell
            // ★★A POUCH'S CELL CAN NOW OUTRUN ITS MONEY. The amount is parked
            // by the engine event that fires as the pouch LEAVES THE PLAYER --
            // next Tick -- while the cell is born the instant the player lets
            // go, which is the whole point of the board owning its cells. So a
            // pouch stored onto an aimed square asked for its gold a frame too
            // early, got nothing, and drew empty until it was taken back out
            // (reported). This counts down the passes during which the cell is
            // still expecting it. Not saved: a promise cannot survive a load.
            int        awaitGold = 0;

            // ---- WHERE ----
            int col = -1;
            int row = -1;
            int w = 1;   // footprint at record time: absent spots still hold
            int h = 1;   // their shelf open (board height includes them)
            // GI62: quarter-turns clockwise. w/h ALREADY reflect it -- this is
            // kept so the sprite draws at the angle the player left it at, and
            // so taking the item back carries the turn home.
            int rot = 0;
            // ★★A coin pouch on the shelf keeps the gold that walked in with
            // it. Same answer the grid gives on its own board (the amount
            // hangs off the SLOT, not the item) -- the engine has nowhere
            // to hang per-instance data, so the shelf position is what
            // "this pouch" means here.
            int gold = 0;
            // ★(1.3.0-D) a stored BAG's contents, riding its spot the same
            // way the pouch's gold does. Their cells are hidden while the
            // bundle holds them; the spot's death by TAKE sends them home,
            // any other death simply spills them onto the shelf (the hiding
            // is derived from live bundles, so dropping one un-hides).
            std::vector<BundleItem> bundle;
            // ★★★AND IF THIS CELL IS A BAG, THE BAG'S OWN NAME (cosave v13).
            //
            // A bundle entry has had one since v12, and a bag that sat on the
            // shelf did not -- so the same bag was called one thing inside
            // another bag and something else standing on the grid, and crossing
            // between the two renamed it. Nothing broke, but its WINDOW is
            // named after it, and a window that is renamed is a window ImGui
            // has never seen: it reopened at the default position every time
            // the bag changed parents ("가방의 UI 위치가 부모가 바뀔때마다
            // 바뀌는게 거슬린다").
            //
            // Minted on first need rather than at birth: most cells are not
            // bags, and a name nothing ever asks for is a name worth not
            // spending. 0 = not asked yet.
            std::uint32_t bagId = 0;
            // ★★WHICH UNIT THIS SPOT IS SHOWING -- as data, not as a name.
            // These used to live inside the spot's KEY, which made a shelf
            // position depend on the item's mutable state: clear an ownership
            // stamp, spend a charge, and the spot belonged to a pool nothing
            // answered to. It was replaced by a fresh one, and because a
            // pouch's gold and a bag's contents hang off the SPOT, the amount
            // and the bag went with it. Mirrors Grid::LayoutEntry.
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            // GI41: a worn unit is never interchangeable with a spare (one is
            // locked, the other is not), so worn-ness partitions the match
            // rather than merely scoring it.
            bool worn = false;
        };
        struct ContLayout
        {
            std::map<std::string, ContCell> cells;
            std::uint32_t                   stamp = 0;   // LRU recency
            // ★★★THE DEPOSIT LEDGER: how many of each form the PLAYER put in
            // here. Taking your own property back is not theft, and this is the
            // only way we can know which units are yours.
            //
            // Ownership was tried first and cannot work for us. It lives on an
            // ExtraDataList, a bare unit has none, and there is no way to give
            // it one -- ExtraDataList's constructor is declared but not
            // implemented in CommonLibSSE (LNK2019), and the favourite trick
            // that mints one only fires for an entry with no lists at all. So
            // a plain dagger stored while a tempered one sat in the pack could
            // never be marked, and came home stolen. A count needs no such
            // permission.
            //
            // ★Vanilla behaves as if it keeps one of these: three stolen sacks
            // in a barrel show the mark, put one of your own in and the mark
            // goes, take one back and it returns -- it is serving your deposit
            // first. Measured to survive a cell change and a save/load, so
            // this is written to the cosave too.
            std::map<RE::FormID, std::int32_t> deposits;
        };
        std::unordered_map<RE::FormID, ContLayout> g_contLayouts;
        // ★★A MERCHANT'S BOARD IS REAL TOO, it just is not remembered. Barter
        // used to run a separate derive-and-auto-pack path because it had no
        // spot memory to consult; now it gets an ordinary layout that lives for
        // as long as the window is open and is never written to a save. One code
        // path, and the difference between a chest and a shop is only whether
        // the board outlives the visit.
        ContLayout g_barterBoard;

        // ★★ASKED OF THE LAYOUT, NOT CARRIED ON THE CELL. The amount was
        // being copied onto PartnerCell during slot assignment and read back
        // at draw time -- one hop too many: the cell list is rebuilt between
        // the two, so a pouch that HAD been shelved with 6,943 G still drew
        // the empty-pouch art. The layout is the thing that actually keeps
        // the number; read it where it is used.
        [[nodiscard]] int ShelfGoldOf(const std::string& a_spotKey);
        std::uint32_t                              g_contStamp = 0;
        constexpr size_t                           kContLayoutMax = 128;

        // ---- (1.3.0-D) bundles in transit --------------------------------
        // STORE side: contents queued alongside their bag, waiting for the
        // bag's shelf spot to be born (the spot exists only after the engine
        // transfer lands and the cell appears). Per container, so a bundle
        // can never hide same-form items in some OTHER chest.
        struct PendingBundle
        {
            RE::FormID              cont = 0;   // container ref this store aimed at
            RE::FormID              bagForm = 0;
            std::vector<BundleItem> items;
        };
        std::vector<PendingBundle> g_pendingBundles;
        // TAKE side: a bundle whose bag is on its way to the player. The
        // grid's rebuild claims it when the bag's fresh tile appears and
        // routes the arrived contents back INTO the bag.
        std::vector<PendingBundle> g_incomingBundles;

        // ★★★THE NAME MINT. One counter for every bundle entry in the session.
        // Ids never repeat, so an entry that moves between bags or between
        // containers carries its name across untouched and nothing collides on
        // arrival. It is not saved: the load pass raises the counter past every
        // id it reads, which is the same guarantee with one less field.
        std::uint32_t g_nextBundleId = 1;
        std::uint32_t NextBundleId() { return g_nextBundleId++; }

        // Every entry that enters a bundle is named here, and nowhere else.
        BundleItem& AddBundle(std::vector<BundleItem>& a_v, BundleItem a_b)
        {
            a_b.id = NextBundleId();
            a_v.push_back(std::move(a_b));
            return a_v.back();
        }

        // ★Everything under a_id, at any depth, in whatever order the vector
        // happens to be in. The old walk relied on parents being written before
        // their children; a name does not care, so a branch survives a bundle
        // that has been rearranged.
        std::set<std::uint32_t> IdClosure(const std::vector<BundleItem>& a_v,
                                          std::uint32_t a_id)
        {
            std::set<std::uint32_t> fam;
            if (a_id == 0) return fam;
            fam.insert(a_id);
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto& b : a_v) {
                    if (b.id != 0 && fam.count(b.id) == 0 && fam.count(b.parent) != 0) {
                        fam.insert(b.id);
                        grew = true;
                    }
                }
            }
            return fam;
        }

        // ★The chain UPWARD from a_id: itself, the bag it is in, that bag's
        // bag. Used to refuse a drop that would put a bag inside itself -- a
        // question a flat index could not even be asked, so the loop it would
        // have made was simply unreachable data.
        std::set<std::uint32_t> Ancestry(const std::vector<BundleItem>& a_v,
                                         std::uint32_t a_id)
        {
            std::set<std::uint32_t> up;
            for (int guard = 0; guard < 32 && a_id != 0; ++guard) {
                if (!up.insert(a_id).second) break;
                const auto f = std::find_if(a_v.begin(), a_v.end(),
                    [&](const BundleItem& b) { return b.id == a_id; });
                a_id = f == a_v.end() ? 0 : f->parent;
            }
            return up;
        }

        // ★★CUT A BAG OUT OF A BUNDLE, BRANCH AND ALL, and hand the branch
        // back re-rooted so it can become a manifest of its own.
        //
        // Both ways a bag can leave a bundle need this -- dragged out to the
        // player, or taken by any other road. It used to have two traps to
        // remember: erase from the BACK, since each removal slid what followed
        // it, and then repair every survivor, because an entry that stayed
        // would otherwise point at whatever slid into its parent's place. A bag
        // adopting a stranger's contents was the same bug as a bag losing its
        // own, and both were the index.
        //
        // With names there is no order to respect and no survivor to repair: an
        // entry outside the closure is untouched by definition.
        std::vector<BundleItem> CutBranch(std::vector<BundleItem>& a_bundle,
                                          std::uint32_t a_id)
        {
            const auto fam = IdClosure(a_bundle, a_id);
            if (fam.empty()) return {};
            std::vector<BundleItem> out;
            for (const auto& b : a_bundle) {
                if (b.id == a_id || fam.count(b.id) == 0) continue;
                out.push_back(b);
                // the branch re-roots: what sat in the cut bag now sits in
                // "the bag that owns this bundle", whichever that turns out to
                // be on the other side of the move
                if (out.back().parent == a_id) out.back().parent = 0;
            }
            std::erase_if(a_bundle,
                [&](const BundleItem& b) { return fam.count(b.id) != 0; });
            return out;
        }

        // ★The takes that bring a cut branch home, plus the manifest that puts
        // it back inside the bag on arrival.
        void SendBranchHome(RE::TESBoundObject* a_bag, std::vector<BundleItem> a_branch)
        {
            if (!a_bag || a_branch.empty()) return;
            if (auto* src = SourceRef()) {
                auto inv = src->GetInventory();
                for (auto& b : a_branch) {
                    auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(b.form);
                    if (!obj) continue;
                    int present = 0;
                    if (const auto ei = inv.find(obj); ei != inv.end()) {
                        present = ei->second.first;
                    }
                    const int take = (std::min)(b.count, present);
                    // ★(1.5.x) gold: announce + clamp, same as the bag-cell
                    // take (see ConsumeActingSpot) -- the claim mints it
                    if (obj->IsGold()) {
                        b.count = take;
                        if (take > 0) GoldCoins::ExpectIncoming(take);
                    }
                    if (take > 0) RequestTake(obj, take, 0, b.sig, false);
                }
            }
            SKSE::log::info("[LOOT] nested bag leaves with {} entr(ies)",
                            a_branch.size());
            g_incomingBundles.push_back({ 0, a_bag->GetFormID(), std::move(a_branch) });
            if (g_incomingBundles.size() > 8) {
                g_incomingBundles.erase(g_incomingBundles.begin());
            }
        }


        // ---- (1.3.1/1.3.2c) OPEN shelf bags -------------------------------
        // One window per opened bag spot, ordered by open time (ESC closes
        // the newest first) -- the player-bag grammar, where every open bag
        // has its own window. The bag's form rides along for the title.
        // ★★A BAG INSIDE A SHELF BAG HAS NO SPOT OF ITS OWN, so a window on
        // one is named by the ENTRY it shows: one id.
        //
        // It used to be named by the way DOWN to it -- the spot, then (form,
        // col, row) per level -- because an index into the bundle would have
        // gone stale. So did the path: a square is where a bag SITS, so moving
        // a bag one square over renamed it, its own open window stopped
        // resolving, and the window closed. Moving something is not closing it.
        // An id is the one thing about a bag that a move does not touch.
        struct ShelfBagWin
        {
            std::string   spot;
            RE::FormID    form = 0;
            std::uint32_t bag = 0;   // 0 = the bag ON the spot
        };
        std::vector<ShelfBagWin> g_shelfBags;

        // (1.3.2a) the shelf POUCH withdraw window (one at a time, like the
        // player's) -- spot + form + the slider's current pick
        std::string g_shelfPouchSpot;
        RE::FormID  g_shelfPouchForm = 0;
        int         g_shelfPouchSlider = 0;
        // ★(1.5.x) which BUNDLE ENTRY the window is banking on. 0 = the cell
        // itself (the classic shelf pouch); an id names a pouch inside an
        // open shelf-bag window. Only meaningful while the spot is set --
        // every open site assigns it, so a stale value cannot act.
        std::uint32_t g_shelfPouchBundle = 0;

        // ★(1.5.x) the pouch entry the cursor is over while a carry rides,
        // re-recorded by the bag window every frame it draws (cleared at the
        // top of DrawShelfBag). The coin drop routes ask through
        // IsBundlePouchHovered/DepositOnHoveredBundlePouch.
        struct HoverBundlePouch
        {
            RE::FormID    cont = 0;
            std::string   spot;
            std::uint32_t id = 0;   // 0 = nothing hovered
        };
        HoverBundlePouch g_hoverBundlePouch;

        // ★(1.5.x) and the bag-window SQUARE under the carry, recorded by the
        // same pass (the drop ghost already computes it). What lets a player
        // COIN store into an open shelf bag: the Grid coin routes ask through
        // IsShelfBagHovered/IntakeGoldEntry.
        struct HoverBundleSquare
        {
            RE::FormID    cont = 0;
            std::string   spot;      // empty = nothing hovered
            std::uint32_t root = 0;  // which bag level the window shows
            int           col = -1;
            int           row = -1;
            bool          free = false;
        };
        HoverBundleSquare g_hoverBundleSq;

        // ★(1.5.x) a book read off the shelf: from the read click to the
        // page-close edge -- the page offers the world's E-take meanwhile
        struct ShelfBookRead
        {
            bool          active = false;
            RE::FormID    form = 0;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            std::string   spot;
        };
        ShelfBookRead     g_shelfBookRead;
        std::atomic<bool> g_shelfBookTakeFlag{ false };

        // a carry lifted out of that window. The engine item stays put (it is
        // already in the container, hidden by the bundle) -- consuming the
        // carry (take home / drop on the shelf) is what removes the entry and
        // lets the cell surface. col/row pin the EXACT entry (two identical
        // stacks are told apart only by their anchors).
        struct BundleCarry
        {
            bool          active = false;
            RE::FormID    cont = 0;
            std::string   spot;
            RE::FormID    form = 0;
            std::uint16_t sig = 0;
            int           col = -1;
            int           row = -1;
            // ★WHICH ENTRY, by name. This used to be the LEVEL it came out
            // of, which had to be compared against a window's level to tell the
            // carried entry from its lookalikes -- one comparison too many, and
            // any renumbering between the lift and the next frame made the two
            // disagree: the entry drew in its old square while also riding the
            // cursor. Two copies of one thing. The id IS the entry.
            std::uint32_t id = 0;
        };
        BundleCarry g_bundleCarry;

        // ★★A BAG THAT LEAVES A BUNDLE TO BECOME A CELL, named. The two halves
        // of that move are a Consume and a PlaceStoredCell, and only the first
        // knows where the bag came FROM while only the second knows where it
        // lands -- so the name is parked here for the one frame between them.
        // What it buys: the windows open on that bag and on everything inside
        // it stay open, hanging off the new cell instead of the old spot. A
        // window is a name, and none of those names changed.
        std::string   g_bagToCellSpot;
        std::uint32_t g_bagToCellId = 0;

        // ★(1.3.2) the marker bits of the unit riding the cursor from the
        // PARTNER side (a shelf cell, or a bundle entry). The carry itself
        // has nowhere to keep them and the shelf cell's own record dies with
        // the lift, so they are parked here until the drop seats them.
        std::uint8_t g_carryGlow = 0;
        bool         g_carryStolen = false;   // (1.3.2) its ownership, likewise

        [[nodiscard]] std::vector<BundleItem> TakePendingBundle(RE::FormID a_bagForm)
        {
            auto* p = Partner();
            const RE::FormID cont = p ? p->GetFormID() : 0;
            for (auto it = g_pendingBundles.begin(); it != g_pendingBundles.end(); ++it) {
                if (it->bagForm != a_bagForm || it->cont != cont) continue;
                auto v = std::move(it->items);
                g_pendingBundles.erase(it);
                // ★(1.5.x) a pouch riding in with the bag has its gold walking
                // separately as an away parcel (OnPouchLeftPlayer fires when
                // its engine item transfers). Arm the claim grace so the
                // reconcile marries them -- but only for entries that have
                // never claimed: a branch moved between bags keeps its amount.
                for (auto& b : v) {
                    if (b.gold < 0 && GoldCoins::IsPouch(b.form)) b.awaitGold = 8;
                }
                return v;
            }
            return {};
        }

        // pending drop-cell spot for a STACK store (slider round-trip)
        // ⛔F7's StoreHint is gone with the store slider (1.5.x stack flow).
        //  It existed to carry a drop cell ACROSS the quantity window, and a
        //  store no longer opens one -- the drop path places (and swaps) on
        //  the spot directly, the way the single-unit case always did.

        // GI18: drop positions waiting for their item to actually arrive. The
        // engine transfer runs on the next Tick, so the cell does not exist yet
        // when the player lets go -- and by then its ordinal in THIS container
        // is unknown anyway. (form, sig) is the only identity that survives the
        // move: the ExtraDataList travels intact, so its signature does too.

        // GI20: the pool slot of the cell the player is acting on. A take must
        // free THAT slot; without it the pool merely loses its trailing position
        // and every cell after the taken one shuffles up -- which is what made
        // looting follow storage order instead of the cursor.
        std::string g_actingSpot;

        // ★★A TAKE SPENDS A CELL, it does not necessarily END one. The cell
        // used to be erased outright, which was right only because a form owned
        // exactly one cell and taking meant taking all of it. A cell owns a count
        // now, so a take of three out of ten leaves seven where they were --
        // and the pouch's gold and the bag's bundle are only handed back when the
        // cell actually empties, which is the moment the thing truly leaves.
        ContLayout* BoardFor();   // defined beside the reconcile below

        void ConsumeActingSpot(RE::TESBoundObject* a_obj, int a_count)
        {
            if (g_actingSpot.empty()) return;
            std::vector<BundleItem> bundle;
            // ★THE BOARD THE PLAYER IS LOOKING AT, not the persistent book.
            // This reached g_contLayouts[partner] directly -- right for loot
            // and pickpocket, where that IS the board, and silently wrong in
            // BARTER, whose shelf lives in g_barterBoard (SpotMemoryOn is
            // false there). The clicked cell's retirement landed in a book
            // the shelf never reads, the reconcile saw one unit too many
            // and shrank FROM THE BACK -- so buying the front of three
            // identical weapons always vanished the rear one (user report).
            // BoardFor() is the one place that knows which book is current.
            if (auto* cl = BoardFor()) {
                // ★★(1.3.0-B) A TAKEN POUCH BRINGS ITS GOLD HOME. This erase
                // IS the take/buy path's slot retirement, and it is the only
                // place that knows exactly which slot leaves. The old hook
                // waited for the whole POOL to vanish from the board -- but
                // absent items KEEP their spot by design, so the common
                // single take never tripped it and the amount died with the
                // spot ("pouch returned but nothing was away"). Deposit
                // first, then retire.
                if (const auto si = cl->cells.find(g_actingSpot);
                    si != cl->cells.end()) {
                    si->second.count -= (std::max)(0, a_count);
                    if (si->second.count > 0) {
                        // part of the cell stayed: nothing retires, and the
                        // acting key clears so the next click names its own
                        g_actingSpot.clear();
                        return;
                    }
                    if (si->second.gold > 0) {
                        SKSE::log::info("[LOOT] pouch taken back with {} G ('{}')",
                            si->second.gold, g_actingSpot);
                        GoldCoins::GiveAwayGold(si->second.gold, si->second.form);
                    }
                    // ★(1.3.0-D) same retirement for a bag: its bundled
                    // contents leave WITH it (queued below, once the spot
                    // is gone and the acting key is cleared -- the takes
                    // re-enter this function and must find it empty).
                    bundle = std::move(si->second.bundle);
                }
                cl->cells.erase(g_actingSpot);
            }
            g_actingSpot.clear();

            if (!bundle.empty() && a_obj) {
                // ★(1.5.x) the pouches inside leave for the player too: their
                // amounts park as PARCELS before the engine items travel, and
                // the claim hands each parcel back -- to the exact tile the
                // entry becomes when the manifest lands (ClaimIncomingBundles
                // matches on this b.gold), else through the generic pass.
                // The amount STAYS on the manifest copy for that match; it is
                // harmless if stale, since the targeted claim only ever moves
                // money that is actually parked.
                for (auto& b : bundle) {
                    if (b.gold <= 0) continue;
                    SKSE::log::info(
                        "[LOOT] bundled pouch taken back with {} G", b.gold);
                    GoldCoins::GiveAwayGold(b.gold, b.form);
                    b.awaitGold = 0;
                }
                // bundles only ever ride LOOT-container spots, where the
                // partner ref IS the source the takes will pull from
                if (auto* src = Partner()) {
                    auto inv = src->GetInventory();
                    for (auto& b : bundle) {
                        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(b.form);
                        if (!obj) continue;
                        int present = 0;
                        if (const auto ei = inv.find(obj); ei != inv.end()) {
                            present = ei->second.first;
                        }
                        // the chest may have respawned some of it away: take
                        // what is actually there, silently drop the rest
                        const int take = (std::min)(b.count, present);
                        // ★(1.5.x) a GOLD entry: the arriving Septims merge
                        // straight into the ledger, and the claim will mint
                        // its coin record in the bag -- announce the amount
                        // so the income detector does not mint it TWICE, and
                        // clamp the manifest to what actually travels.
                        if (obj->IsGold()) {
                            b.count = take;
                            if (take > 0) GoldCoins::ExpectIncoming(take);
                        }
                        if (take <= 0) continue;
                        RequestTake(obj, take, 0, b.sig, false);
                    }
                }
                SKSE::log::info("[LOOT] bag taken back, {} bundled kind(s) follow",
                    bundle.size());
                g_incomingBundles.push_back({ 0, a_obj->GetFormID(), std::move(bundle) });
                // a manifest whose bag never arrived (refused take) must not
                // pile up and mis-claim some future bag of the same form
                if (g_incomingBundles.size() > 8) {
                    g_incomingBundles.erase(g_incomingBundles.begin());
                }
            }
        }

        // GI41: pickpocket boards keep their layout too.
        //
        // They were excluded, so the whole pool/slot pass (everything under
        // `if (cl)`) never ran for them -- every frame was a fresh first-fit in
        // ENUMERATION order. Planting an item on a mark therefore re-dealt the
        // entire board, and because the cell a lock belongs to is identified by
        // where it sits, the lock appeared to move onto a different dagger.
        //
        // A merchant can shuffle harmlessly because nothing there is locked.
        // A pickpocket target has worn gear that must stay put, so it needs the
        // same stable layout a container gets.
        //
        // IsLootMode() is deliberately NOT widened -- it also drives the red
        // STEAL chrome and the take/store semantics, neither of which applies.
        // ★★The bottom strip's height, derived from the TEXT it has to hold.
        // It was a flat 30*S, which fit while the strip drew at the default
        // font size -- then the label moved to Theme::TextOutlined at
        // FontValue() (20px) to match the player's gold bar, and 8 above plus
        // 20 of glyph left two pixels under it. The number now follows the
        // font, so the same thing cannot happen the next time either changes.
        [[nodiscard]] float BottomStripH()
        {
            const float S = Theme::Scale();
            return 8.0f * S + Theme::FontValue() + 10.0f * S;
        }

        bool SpotMemoryOn()
        {
            return IsLootMode(g_mode) || g_mode == Mode::kPickpocket;
        }

        // ★★(1.3.3) A FOLLOWER CARRIES 10 x 8, AND NO MORE. A chest is a
        // hole in the world and may be bottomless; a companion is a person
        // with a pack, and an unbounded one turns every follower into a
        // second inventory with no cost. The board still GROWS past it when
        // the follower already holds more (their own gear, a quest item, a
        // gift from a script) -- nothing is ever hidden or dropped. The
        // limit governs what the PLAYER may add, which is the only half the
        // player controls.
        constexpr int kCompanionRows = 8;

        [[nodiscard]] bool CompanionPartner()
        {
            if (!IsLootMode(g_mode)) return false;
            auto* p = Partner();
            auto* a = p ? p->As<RE::Actor>() : nullptr;
            // dead followers are corpses: loot them like any other container
            return a && !a->IsDead() && a->IsPlayerTeammate();
        }

        // ---- F6b: pickpocket helpers ----

        // vanilla success chance via the ENGINE formula (weight / value /
        // skills / perks / sleep bonus / 90% cap all handled inside)
        int PickpocketChance(RE::TESBoundObject* a_obj, int a_count,
                             RE::InventoryEntryData* a_entry)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* partner = Partner();
            auto* target = partner ? partner->As<RE::Actor>() : nullptr;
            if (!player || !target || !a_obj) return 0;
            const float thief = player->AsActorValueOwner()->GetActorValue(
                RE::ActorValue::kPickpocket);
            const float mark = target->AsActorValueOwner()->GetActorValue(
                RE::ActorValue::kPickpocket);
            const int unitValue = a_entry ? a_entry->GetValue()
                                          : static_cast<int>(a_obj->GetGoldValue());
            const auto value = static_cast<std::uint32_t>(
                (std::max)(0, unitValue * a_count));
            const float weight = a_obj->GetWeight() * static_cast<float>(a_count);
            const bool detected = target->RequestDetectionLevel(player) > 0;
            const int chance = RE::AIFormulas::ComputePickpocketSuccess(
                thief, mark, value, weight, player, target, detected, a_obj);
            return std::clamp(chance, 0, 100);
        }

        // Perfect Touch (Skyrim.esm PerfectTouch 0x058205): worn gear is
        // otherwise locked
        bool HasPerfectTouch()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(0x00058205);
            return player && perk && player->HasPerk(perk);
        }

        // Poisoned (Skyrim.esm Poisoned 0x105F28): planted poisons silently
        // harm the mark
        bool HasPoisonedPerk()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(0x00105F28);
            return player && perk && player->HasPerk(perk);
        }

        // stable item key (same format as the grid's FormKey).
        //
        // GI13: it must be per UNIT now. Keyed by form alone, every cell of the
        // same form resolved to the SAME remembered spot and they were all drawn
        // on top of each other -- which is why the second and later daggers
        // retriggered the hover blip every frame: overlapping InvisibleButtons
        // traded IsItemHovered() between them, so Sfx::HoverNote saw a different
        // item id each frame and re-fired forever.
        std::string PartnerKey(RE::TESForm* a_form, std::uint16_t a_uid = 0,
                               std::uint16_t a_sig = 0, int a_ord = 0)
        {
            // GetLocalFormID() dereferences the source file unchecked, so a
            // runtime-created form (brewed potion, player enchantment) has to
            // fall back to its whole FormID -- it has no local id to take.
            auto* file = a_form->GetFile(0);
            std::string key = file ? std::string(file->GetFilename()) : "Dynamic";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "|0x%06X",
                file ? a_form->GetLocalFormID() : a_form->GetFormID());
            key += buf;
            if (a_uid != 0) {
                std::snprintf(buf, sizeof(buf), "@%04X", a_uid);   // engine uniqueID
                key += buf;
            } else if (a_sig != 0) {
                std::snprintf(buf, sizeof(buf), "~%04X", a_sig);   // GI14 content signature
                key += buf;
            }
            if (a_ord > 0) {
                std::snprintf(buf, sizeof(buf), "#%d", a_ord);
                key += buf;
            }
            return key;
        }
    }


    Mode CurrentMode() { return g_mode; }

    void ForgetDeposits(RE::FormID a_containerID)
    {
        const auto it = g_contLayouts.find(a_containerID);
        if (it == g_contLayouts.end() || it->second.deposits.empty()) return;
        SKSE::log::info("[LOOT] container {:08X} reset -- dropping {} deposit(s)",
                        a_containerID, it->second.deposits.size());
        it->second.deposits.clear();
    }

    void Enter(Mode a_mode, RE::TESObjectREFR* a_partner)
    {
        g_mode = a_mode;
        g_partner = a_partner ? a_partner->CreateRefHandle() : RE::ObjectRefHandle{};
        // §2-C: a pouch sold while bartering releases its stored gold first
        GoldCoins::SetBarterContext(a_mode == Mode::kBarter);

        // F7: touch this container's spot memory (LRU) — the map is capped;
        // the least recently OPENED container falls off first
        if (IsLootMode(a_mode) && a_partner) {
            g_contLayouts[a_partner->GetFormID()].stamp = ++g_contStamp;
            while (g_contLayouts.size() > kContLayoutMax) {
                // ★(1.3.0-B) never evict a shelf that still banks a pouch's
                // gold -- the pouch is still IN that chest and the layout is
                // the only record of the amount. Oldest goldless one goes; if
                // every remembered chest banks gold, keep them all (the cap is
                // a memory bound, not a correctness bound).
                auto victim = g_contLayouts.end();
                for (auto it = g_contLayouts.begin(); it != g_contLayouts.end(); ++it) {
                    bool banks = false;
                    for (const auto& [k, s] : it->second.cells) {
                        if (s.gold > 0) { banks = true; break; }
                    }
                    if (banks) continue;
                    if (victim == g_contLayouts.end() ||
                        it->second.stamp < victim->second.stamp) {
                        victim = it;
                    }
                }
                if (victim == g_contLayouts.end()) break;
                g_contLayouts.erase(victim);
            }
        }

        // B: prefetch the partner's WHOLE stock up front — one caching burst
        // when the window opens instead of a trickle while scrolling. For
        // barter this is the vendor chest (SourceRef), not the actor.
        if (auto* src = SourceRef()) {
            // §RELEASE-①: stock the coin pouch at general-goods vendors before
            // the prefetch walks the list, so the new item gets its icon in the
            // same caching burst as the rest of the stock.
            if (a_mode == Mode::kBarter && a_partner) {
                GoldCoins::SeedVendorStock(a_partner->As<RE::Actor>(), src);
            }
            auto* cache = IconCache::GetSingleton();
            for (const auto& [obj, data] : src->GetInventory()) {
                if (obj && data.first > 0) cache->Prefetch(obj);
            }
        }
    }

    namespace
    {
        // ★B4-3b: a struck queue withdraws its ledger entries. Outgoing
        // requests submit at COMMIT time now (RequestStore/Sell/PickStore),
        // so anything still queued when the queue is cleared -- a lost
        // pickpocket roll force-closing the menu, a teardown whose flush
        // failed -- holds an open entry that no event will ever confirm.
        // Left alone they only expire noisily (the recovery is harmless),
        // but a cancelled request's SLOT KEY must come back deliberately:
        // queued, it would be consumed by the form's next confirmation --
        // the bug the two-phase drop exists to prevent. Entries already
        // processed this Tick were confirmed by their own events and match
        // nothing here -- the cancel is naturally idempotent.
        void CancelQueuedOutgoing(const char* a_why)
        {
            for (const auto& q : g_xfer) {
                if (!q.obj) continue;
                if (q.dir != XferReq::kStore && q.dir != XferReq::kSell &&
                    q.dir != XferReq::kPickStore) {
                    continue;
                }
                for (const auto& e :
                     Ledger::Cancel(q.obj->GetFormID(), q.count, a_why)) {
                    if (!e.slot.empty()) Grid::CancelSlotDrop(e.form, e.slot);
                }
            }
        }
    }

    void Reset()
    {
        // B12: a transfer queued on the final render frame would be dropped
        // silently — flush BEFORE the session state is torn down.
        // (ProcessTransfers resolves the container through g_partner and the
        // removal reason through g_mode; the old clear-first order nulled
        // SourceRef() so this flush never actually ran.)
        if (!g_xfer.empty()) ProcessTransfers();
        g_mode = Mode::kNormal;
        g_partner = {};
        CancelQueuedOutgoing("session teardown");   // B4-3b: their ledger halves
        g_xfer.clear();   // anything STILL queued (flush failed) is dropped...
        Grid::ClearAllPendingRemoves();   // ...so their pending marks must go too
        Grid::ClearDropHint();            // B2
        g_slider.active = false;
        g_confirm.active = false;
        g_in.clear();            // promises whose transfers were just flushed
        // ★A merchant's board lives exactly as long as the visit. It is a real
        // board while the window is open -- one code path with a chest -- and
        // it is never written to a save, because a shop restocks and its shelf
        // is not a place the player arranged.
        g_barterBoard.cells.clear();
        // (1.3.0-C) a leaving-pouch hint whose transfer never fired (lost
        // pick roll, refused move) must not name a tile for the NEXT session
        GoldCoins::NotePouchLeaving({});
        // (1.3.0-D) a store bundle whose bag spot was never born (the window
        // closed the same instant) is dropped -- the contents are simply
        // loose in the chest, visible next open. Incoming bundles SURVIVE:
        // their claim happens on the player-side rebuild, which may run
        // after this session is gone.
        g_pendingBundles.clear();
        g_bagToCellSpot.clear();   // an unfinished bag -> cell hand-off
        g_bagToCellId = 0;
        g_shelfBags.clear();   // (1.3.1) the shelf windows die with the session
        g_shelfPouchSpot.clear();
        g_shelfBookRead = {};   // (1.5.x) a page left open dies with it too
        g_shelfBookTakeFlag.store(false);
        g_bundleCarry = {};
        g_carryGlow = 0;
        g_carryStolen = false;   // ★its twin -- see the lift sites
        GoldCoins::SetBarterContext(false);
    }

    bool SliderActive() { return g_slider.active; }

    void OpenSlider(RE::TESBoundObject* a_obj, int a_max, XferDir a_dir,
                    const std::string& a_srcKey, int a_unitValue,
                    std::uint16_t a_uid, std::uint16_t a_sig, bool a_worn, bool a_fav,
                    int a_xlIdx)
    {
        if (!a_obj || a_max <= 1) return;
        // player-receiving dirs: cap the slider at what the boards (main +
        // open bags + partial stacks) can actually accept, so a stack buy/take
        // can never overflow the inventory (Phase 7 polish).
        if (a_dir == XferDir::kTake || a_dir == XferDir::kBuy ||
            a_dir == XferDir::kPickTake) {
            const int fit = Grid::MaxAcceptUnits(a_obj, a_max);
            if (fit <= 0) {
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                return;
            }
            a_max = fit;   // may become 1: the slider then offers exactly one
        }
        // start at half the max (split-friendly default), min 1
        // ★...except a DROP, which starts at ONE. R has always meant "get rid
        // of one of these" (it dropped a single unit per press), and the
        // window is here to make that reachable in bulk, not to change what
        // the key means. MAX is one click away for the other intent.
        g_slider = {};
        g_slider.active = true;
        g_slider.obj = a_obj;
        g_slider.max = a_max;
        g_slider.value = a_dir == XferDir::kDrop ? 1 : (std::max)(1, a_max / 2);
        g_slider.dir = a_dir;
        g_slider.srcKey = a_srcKey;
        g_slider.unitValue = a_unitValue;
        g_slider.uid = a_uid;   // GI25
        g_slider.sig = a_sig;
        g_slider.worn = a_worn;
        g_slider.fav = a_fav;   // GI36
        g_slider.xlIdx = a_xlIdx;   // (1.3.3)
        Sfx::SelectOn();   // click / shift+click opened the quantity popup
    }

    bool IsPopupOpen()
    {
        return g_confirm.active || g_slider.active || !g_shelfBags.empty() ||
               !g_shelfPouchSpot.empty();
    }

    bool CloseTopPopup()
    {
        // I/ESC close the TOP sub-window first, the inventory only when
        // nothing is left (confirm sits above the slider)
        if (g_confirm.active) {
            g_confirm.active = false;
            return true;
        }
        if (g_slider.active) {
            g_slider.active = false;
            Grid::ClearDropHint();   // B2
            return true;
        }
        if (!g_shelfPouchSpot.empty()) {   // (1.3.2a) the pouch window
            g_shelfPouchSpot.clear();
            return true;
        }
        if (!g_shelfBags.empty()) {   // (1.3.2c) the NEWEST bag window
            g_shelfBags.pop_back();
            return true;
        }
        return false;
    }

    namespace
    {
        // GI42: may a worn unit legally leave the partner right now? Loot and
        // owned-container theft always may (a corpse's gear is the point);
        // a living mark only with Perfect Touch; a merchant never.
        bool WornExportLegal()
        {
            return IsLootMode(g_mode) ||
                   (g_mode == Mode::kPickpocket && HasPerfectTouch());
        }

        // GI42: one resolution for click time and Tick time. NEVER cache the
        // returned pointer across frames -- resolve again where it is used.
        Grid::UnitChoice ResolveSource(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                       std::uint16_t a_sig, bool a_fromWorn)
        {
            auto* src = SourceRef();
            auto* entry = src ? Grid::LiveEntryOf(src, a_obj) : nullptr;
            if (a_fromWorn) {
                // a worn unit may have neither uid nor signature -- being worn
                // is itself the handle
                if (auto* xl = Grid::WornExtraMatching(entry, a_uid, a_sig, 0)) {
                    return { Grid::PickKind::kNamed, xl };
                }
                return Grid::PoolChoice(entry, a_uid, a_sig, /*nameWorn*/ true,
                                        WornExportLegal());
            }
            // GI35: a spare cell must never NAME the worn list -- but that only
            // removed OUR ability to pick it, not the engine's. PoolChoice is
            // what removes the engine's.
            return Grid::PoolChoice(entry, a_uid, a_sig, /*nameWorn*/ false,
                                    WornExportLegal());
        }

        // ★★★WHY A REFUSAL HAPPENED, not just that it did.
        //
        // Reported: left-clicking an item on a corpse answers "can't tell it
        // apart from the worn copy" and refuses, while R (take all) and
        // right-click both work. Not reproducible here, and the code alone
        // leaves a contradiction: the only path to this refusal with a uid or
        // sig in hand is LiveEntryOf returning nothing -- but that reads the
        // changes list, which GetInventoryChanges CREATES when absent, and a
        // worn item has to be in it to be worn at all.
        //
        // So the state is recorded rather than guessed at. Whichever of these
        // is false in a real report is the answer:
        //   entry=n            the changes list does not hold this form
        //   lists=0            it holds it, but with no extra data at all
        //   uid/sig non-zero   the board named a unit the source cannot show
        void LogRefusal(const char* a_where, RE::TESBoundObject* a_obj,
                        std::uint16_t a_uid, std::uint16_t a_sig, bool a_fromWorn)
        {
            auto* src   = SourceRef();
            auto* entry = (src && a_obj) ? Grid::LiveEntryOf(src, a_obj) : nullptr;
            int   lists = 0, worn = 0;
            if (entry && entry->extraLists) {
                for (auto* xl : *entry->extraLists) {
                    if (!xl) continue;
                    ++lists;
                    if (xl->HasType<RE::ExtraWorn>() ||
                        xl->HasType<RE::ExtraWornLeft>()) ++worn;
                }
            }
            SKSE::log::warn("[LOOT] {} refused '{}' uid={:04X} sig={:04X} fromWorn={} "
                            "| src={} entry={} lists={} wornLists={} mode={} wornLegal={}",
                a_where, a_obj ? a_obj->GetName() : "?", a_uid, a_sig, a_fromWorn,
                src ? "y" : "n", entry ? "y" : "n", lists, worn,
                static_cast<int>(g_mode), WornExportLegal());
        }
    }

    bool RequestTake(RE::TESBoundObject* a_obj, int a_count,
                     std::uint16_t a_uid, std::uint16_t a_sig, bool a_fromWorn,
                     bool a_useAfter)
    {
        if (a_obj && a_count > 0) {
            // GI42: refuse BEFORE arming any suppression -- a transfer that will
            // not run must not leave the board and the engine disagreeing.
            if (!ResolveSource(a_obj, a_uid, a_sig, a_fromWorn).ok()) {
                LogRefusal("take", a_obj, a_uid, a_sig, a_fromWorn);
                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                return false;
            }
            XferReq req;
            req.dir      = XferReq::kTake;
            req.obj      = a_obj;
            req.count    = a_count;
            req.uid      = a_uid;
            req.sig      = a_sig;
            req.fromWorn = a_fromWorn;
            req.useAfter = a_useAfter;
            g_xfer.push_back(std::move(req));
            NoteOut(a_obj, a_uid, a_sig, a_count);
            ConsumeActingSpot(a_obj, a_count);   // GI20: the hovered cell's slot, not the last one
            ConsumeBundleCarry(a_obj, a_count);   // (1.3.1) a shelf-bag carry taken home
            return true;
        }
        return false;
    }

    int RequestTakeAll(RE::TESBoundObject* a_obj, int a_count,
                       std::uint16_t a_uid, std::uint16_t a_sig, bool a_fromWorn)
    {
        if (!a_obj || a_count <= 0) return 0;
        // ★GOLD IS EXEMPT FROM THE CLAMP, not from the rule. Coins are a
        // mirror of the ledger and occupy no cells, so "how many fit" has no
        // meaning for them -- the same exemption the click paths state where
        // they gate on CanFitNewItem.
        int want = a_count;
        if (!a_obj->IsGold()) {
            want = Grid::MaxAcceptUnits(a_obj, a_count);
            if (want <= 0) {
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                return 0;
            }
        }
        if (!RequestTake(a_obj, want, a_uid, a_sig, a_fromWorn)) return 0;
        // ★Partial is a RESULT, not a refusal: what fit is already on its way,
        // and the note explains the units still sitting in the container so
        // the shortfall is never read as the click having missed.
        if (want < a_count) {
            Sfx::FailNote(Lang::T(Lang::Str::TookWhatFit));
            SKSE::log::info("[XFER] whole-cell take clamped: {} of {} '{}'",
                            want, a_count, a_obj->GetName());
        }
        return want;
    }

    void RequestStore(RE::TESBoundObject* a_obj, int a_count,
                      std::uint16_t a_uid, std::uint16_t a_sig, bool a_fav,
                      int a_xlIdx, const std::string& a_srcKey)
    {
        if (a_obj && a_count > 0) {
            g_xfer.push_back({ XferReq::kStore, a_obj, a_count, 0, 0, a_srcKey,
                               a_uid, a_sig, false, a_fav, a_xlIdx });
            // ★the units are the container's from this instant, whatever the
            // engine has got round to (see g_in)
            NoteIn(a_obj, a_uid, a_sig, a_count);
            // ★B4-3b: the ledger's books open when the PLAYER commits -- the
            // same moment the removal counters arm (NotePendingRemove rides
            // beside every caller of this function). Submitting from the
            // transfer Tick put the two on different clocks, and the click-
            // to-Tick window would go unsuppressed the day the counters are
            // absorbed. A queue struck before its Tick (a lost pickpocket
            // roll clears everything behind it) withdraws through Cancel.
            Ledger::Submit(a_obj->GetFormID(), -a_count, "store", a_uid, a_sig,
                           a_srcKey);
        }
    }

    void RequestBuy(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal,
                    std::uint16_t a_uid, std::uint16_t a_sig)
    {
        // (the guard had no braces: a rejected buy still consumed the acting
        // spot, so the next purchase landed on a cell it was not given)
        if (a_obj && a_count > 0) {
            if (!ResolveSource(a_obj, a_uid, a_sig, false).ok()) {   // GI42
                LogRefusal("buy", a_obj, a_uid, a_sig, false);
                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                return;
            }
            g_xfer.push_back({ XferReq::kBuy, a_obj, a_count, a_price, a_baseTotal,
                               {}, a_uid, a_sig });
            NoteOut(a_obj, a_uid, a_sig, a_count);
            ConsumeActingSpot(a_obj, a_count);   // GI20
        }
    }

    void RequestSell(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal,
                     std::uint16_t a_uid, std::uint16_t a_sig, bool a_fav,
                     int a_xlIdx, const std::string& a_srcKey)
    {
        if (a_obj && a_count > 0) {
            g_xfer.push_back({ XferReq::kSell, a_obj, a_count, a_price, a_baseTotal,
                               a_srcKey, a_uid, a_sig, false, a_fav, a_xlIdx });
            // B4-3b: see RequestStore -- the books open at the commit
            Ledger::Submit(a_obj->GetFormID(), -a_count, "sell", a_uid, a_sig,
                           a_srcKey);
        }
    }

    void RequestPickTake(RE::TESBoundObject* a_obj, int a_count,
                         std::uint16_t a_uid, std::uint16_t a_sig, bool a_fromWorn)
    {
        // (the guard had no braces, so the spot was consumed even when nothing
        // was queued -- a rejected request stole the next cell's placement)
        if (a_obj && a_count > 0) {
            if (!ResolveSource(a_obj, a_uid, a_sig, a_fromWorn).ok()) {   // GI42
                LogRefusal("request", a_obj, a_uid, a_sig, a_fromWorn);
                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                return;
            }
            g_xfer.push_back({ XferReq::kPickTake, a_obj, a_count, 0, 0, {}, a_uid, a_sig,
                               a_fromWorn });
            NoteOut(a_obj, a_uid, a_sig, a_count);
            ConsumeActingSpot(a_obj, a_count);   // GI25
        }
    }

    void RequestPickStore(RE::TESBoundObject* a_obj, int a_count,
                          std::uint16_t a_uid, std::uint16_t a_sig,
                          const std::string& a_srcKey, bool a_fav, int a_xlIdx)
    {
        if (a_obj && a_count > 0) {
            g_xfer.push_back({ XferReq::kPickStore, a_obj, a_count, 0, 0, a_srcKey,
                               a_uid, a_sig, false, a_fav, a_xlIdx });
            NoteIn(a_obj, a_uid, a_sig, a_count);   // see RequestStore
            // Every other player -> partner direction suppresses the tile at
            // REQUEST time; only this one waited for the Tick, so the planted
            // item sat back down on the board for a frame or two before being
            // taken away. Same rule as store/sell: the unit leaves the board the
            // moment the player commits, and ProcessTransfers clears it when the
            // engine count actually drops (or when the roll is lost).
            Grid::NotePendingRemove(a_obj, a_srcKey, a_count, a_xlIdx);
            // B4-3b: see RequestStore -- the books open at the commit
            Ledger::Submit(a_obj->GetFormID(), -a_count, "plant", a_uid, a_sig,
                           a_srcKey);
        }
    }

    namespace
    {
        // GI42: the Tick-time resolution -- same rules as click time, resolved
        // FRESH here because a list captured a frame ago may be gone.
        // ★★WHOSE CONTAINER IS THIS. Measured, after the stamp came back
        // "owner (none)" on a chest the engine itself had put us in kSteal for:
        // TESObjectREFR::GetOwner() answers for the REF, and a chest in a house
        // or a shop carries no owner of its own -- the CELL does. Asking only
        // the ref is how a theft could produce no owner to blame.
        //
        // ★StealAlarm was asking the same way, so the bounty roll has been
        // handed a null owner all along. It goes through here now too.
        // ---- deposit ledger ------------------------------------------------
        // See ContLayout::deposits for why a ledger and not ownership.

        [[nodiscard]] ContLayout* LedgerFor(RE::TESObjectREFR* a_src)
        {
            if (!a_src) return nullptr;
            const auto it = g_contLayouts.find(a_src->GetFormID());
            return it != g_contLayouts.end() ? &it->second : nullptr;
        }

        // How many of this form in this container are the player's own.
        [[nodiscard]] int DepositedCount(RE::TESObjectREFR* a_src,
                                         RE::TESBoundObject* a_obj)
        {
            if (!a_obj) return 0;
            auto* cl = LedgerFor(a_src);
            if (!cl) return 0;
            const auto it = cl->deposits.find(a_obj->GetFormID());
            return it != cl->deposits.end() ? (std::max)(0, it->second) : 0;
        }

        void DepositAdd(RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, int a_count)
        {
            if (!a_src || !a_obj || a_count <= 0) return;
            // The layout is created on open (Enter), so this normally finds one;
            // make it anyway rather than silently losing the deposit.
            auto& cl = g_contLayouts[a_src->GetFormID()];
            cl.deposits[a_obj->GetFormID()] += a_count;
        }

        // Spend up to a_count from the deposit and report how much was covered.
        [[nodiscard]] int DepositTake(RE::TESObjectREFR* a_src,
                                      RE::TESBoundObject* a_obj, int a_count)
        {
            if (!a_src || !a_obj || a_count <= 0) return 0;
            auto* cl = LedgerFor(a_src);
            if (!cl) return 0;
            const auto it = cl->deposits.find(a_obj->GetFormID());
            if (it == cl->deposits.end()) return 0;
            const int used = (std::min)(a_count, (std::max)(0, it->second));
            it->second -= used;
            if (it->second <= 0) cl->deposits.erase(it);
            return used;
        }

        // How many of this form the holder actually has right now. The store
        // path uses it to credit the ledger with what LEFT rather than with
        // what was asked for -- a RemoveItem that quietly takes nothing is the
        // engine's way of refusing, and it does not reach back to tell us.
        [[nodiscard]] int HeldCount(RE::TESObjectREFR* a_who, RE::TESBoundObject* a_obj)
        {
            if (!a_who || !a_obj) return 0;
            auto inv = a_who->GetInventory(
                [&](RE::TESBoundObject& o) { return std::addressof(o) == a_obj; });
            const auto it = inv.find(a_obj);
            return it != inv.end() ? it->second.first : 0;
        }

        // ★★★CAN WE PROVE THIS DEPOSIT IS THE PLAYER'S OWN?
        //
        // With the unit named, the unit answers: an owner on the list means it
        // is somebody else's and a chest is not a fence. Nameless is the hard
        // case -- ResolveExitUnit returns null both for a genuinely plain unit
        // (no list, therefore no owner, therefore the player's) and for a
        // fallback pick where we simply could not tell which unit is leaving.
        // The old test read the second as the first and handed a laundered
        // credit to anything it failed to resolve. So when we cannot name it,
        // claim the deposit only if NOTHING the player holds of that form
        // carries an owner: then whichever one left, it was clean.
        [[nodiscard]] bool DepositProvable(RE::TESObjectREFR* a_player,
                                           RE::TESBoundObject* a_obj,
                                           RE::ExtraDataList* a_xl)
        {
            if (a_xl) return !a_xl->GetOwner();
            if (!a_player || !a_obj) return false;
            auto* e = Grid::LiveEntryOf(a_player, a_obj);
            if (!e || !e->extraLists) return true;   // nothing but bare units
            for (auto* l : *e->extraLists) {
                if (l && l->GetOwner()) return false;
            }
            return true;
        }

        RE::TESForm* ContainerOwner(RE::TESObjectREFR* a_source)
        {
            if (!a_source) return nullptr;
            if (auto* own = a_source->GetOwner()) return own;
            if (auto* npc = a_source->GetActorOwner()) return npc;
            if (auto* fac = a_source->GetFactionOwner()) return fac;
            if (auto* cell = a_source->GetParentCell()) {
                if (auto* own = cell->GetOwner()) return own;
                if (auto* npc = cell->GetActorOwner()) return npc;
                if (auto* fac = cell->GetFactionOwner()) return fac;
            }
            return nullptr;
        }

        Grid::UnitChoice SourceUnit(RE::TESObjectREFR* a_source, const XferReq& a_r)
        {
            auto* entry = Grid::LiveEntryOf(a_source, a_r.obj);
            if (a_r.fromWorn) {
                if (auto* xl = Grid::WornExtraMatching(entry, a_r.uid, a_r.sig, 0)) {
                    return { Grid::PickKind::kNamed, xl };
                }
                return Grid::PoolChoice(entry, a_r.uid, a_r.sig, true, WornExportLegal());
            }
            return Grid::PoolChoice(entry, a_r.uid, a_r.sig, false, WornExportLegal());
        }

        // GI42 tripwire: the worn lists of one form, as VALUES (uid, sig, count,
        // hand) -- never pointers. Compared around an engine RemoveItem that ran
        // on an unproven nullptr; a difference means the engine moved a unit off
        // a body when we believed it could not.
        std::vector<std::array<int, 4>> WornPrint(RE::TESObjectREFR* a_ref,
                                                  RE::TESBoundObject* a_obj)
        {
            std::vector<std::array<int, 4>> out;
            auto* e = Grid::LiveEntryOf(a_ref, a_obj);
            if (!e || !e->extraLists) return out;
            for (auto* xl : *e->extraLists) {
                if (!xl) continue;
                const bool wr = xl->HasType<RE::ExtraWorn>();
                const bool wl = xl->HasType<RE::ExtraWornLeft>();
                if (!wr && !wl) continue;
                int u = 0;
                if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) u = xu->uniqueID;
                out.push_back({ u, static_cast<int>(Grid::InstanceSigOf(xl)),
                                (std::max)(1, xl->GetCount()), wl ? 2 : 1 });
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // Wraps an engine removal whose unit choice is UNPROVEN (kFallback):
        // logs loudly if a worn unit moved. No behaviour change -- this is the
        // instrument that decides whether kFallback ever needs to become a
        // refusal too.
        template <class F>
        void GuardedRemove(RE::TESObjectREFR* a_holder, RE::TESBoundObject* a_obj,
                           bool a_guard, const char* a_tag, F&& a_remove)
        {
            if (!a_guard) { a_remove(); return; }
            const auto before = WornPrint(a_holder, a_obj);
            a_remove();
            if (WornPrint(a_holder, a_obj) != before) {
                SKSE::log::error("[XFER] TRIPWIRE {}: engine fallback moved a WORN "
                                 "unit of '{}'", a_tag, a_obj->GetName());
            }
        }
    }

    void ProcessTransfers()
    {
        if (g_xfer.empty()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* source = SourceRef();
        if (!player || !source) { g_xfer.clear(); return; }

        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F);   // Gold001
        bool goldMoved = false;
        bool goldIn = false;    // received gold (sell) vs paid (buy) — one clink
        // vanilla per-item pickup/putdown sounds. Budgeted per batch so
        // "take all" doesn't fire dozens of overlapping sounds at once.
        int soundBudget = 4;
        auto itemSound = [&](RE::TESBoundObject* a_obj, bool a_up) {
            if (soundBudget <= 0 || !a_obj) return;
            --soundBudget;
            player->PlayPickUpSound(a_obj, a_up, false);
        };

        for (const auto& r : g_xfer) {
            // ★1.4/B0 -- registered HERE, once, for all six directions: this is
            // the last point before the engine is asked, and every branch below
            // ends in a RemoveItem that will come back to us as an event. The
            // direction test is the same one RequestXfer uses (see kTake ||
            // kBuy || kPickTake above), so the two cannot drift apart.
            if (r.obj && Ledger::Enabled()) {
                const bool incoming = r.dir == XferReq::kTake ||
                                      r.dir == XferReq::kBuy ||
                                      r.dir == XferReq::kPickTake;
                const char* who =
                    r.dir == XferReq::kTake      ? "take"  :
                    r.dir == XferReq::kStore     ? "store" :
                    r.dir == XferReq::kBuy       ? "buy"   :
                    r.dir == XferReq::kSell      ? "sell"  :
                    r.dir == XferReq::kPickTake  ? "steal" : "plant";
                // ★uid+sig ride along: B0 proved the engine's events never
                // name the unit (§8-2), so this is the only record that does.
                // ★B4-3b: INCOMING only. The outgoing directions submit where
                // the player commits (RequestStore/Sell/PickStore), the same
                // moment their suppression arms -- submitting them here put
                // the ledger and the removal counters on different clocks.
                // An arrival has no click-time suppression and its engine
                // call is this very Tick, so here stays its moment.
                if (incoming) {
                    Ledger::Submit(r.obj->GetFormID(), r.count, who,
                                   r.uid, r.sig, {});
                }
            }
            switch (r.dir) {
            case XferReq::kTake: {
                // partner -> player. Engine moves the extra data too (charge,
                // enchant, poison). Gold folds into the ledger; the coin mirror
                // rebuilds it into tiles. F6a: taking from an OWNED container
                // uses kSteal so the engine flags the items stolen — and the
                // WITNESS/bounty roll is a separate engine call (StealAlarm),
                // which the vanilla menu fires per taken item; without it a
                // seen theft never gained a bounty (user-reported).
                const auto pick = SourceUnit(source, r);
                if (!pick.ok()) {   // GI42: nameless AND unsafe -- do not move
                    ClearOut(r.obj, r.uid, r.sig, r.count);
                    Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                    SKSE::log::error("[XFER] take refused: unnameable '{}' uid "
                        "{:04X} sig {:04X}", r.obj->GetName(), r.uid, r.sig);
                    break;
                }
                if (g_mode == Mode::kSteal) {
                    player->StealAlarm(source, r.obj, r.count,
                        r.obj->GetGoldValue() * r.count, ContainerOwner(source), true);
                }
                // ★★★TAKING BACK YOUR OWN PROPERTY IS NOT THEFT, and the
                // deposit ledger is how we know which units are yours. See
                // ContLayout::deposits for why ownership could not do this job.
                //
                // The count is spent here, once, and drives everything below:
                // the removal reason, the two ownership stamps, and nothing
                // else. Ask for three when two are yours and the engine is
                // told about two clean ones and one theft, which is what
                // vanilla does with its own ledger.
                //
                // ★★★SPEND IT IN EVERY MODE, READ IT ONLY IN STEAL. The ledger
                // is written in every mode -- deliberately, because a plain
                // chest can become an owned one and a deposit made before that
                // should still be yours after. But it was only ever DEBITED in
                // steal mode, so the count on an ordinary chest could climb
                // and never fall. Use the same chest as a stash for an hour
                // and it carries a hundred phantom deposits into the cosave,
                // waiting for the day that ref turns hostile.
                const int spent = DepositTake(source, r.obj, r.count);
                const int fromDeposit = (g_mode == Mode::kSteal) ? spent : r.count;
                const int stolenCount = (std::max)(0, r.count - fromDeposit);
                const bool anyStolen  = g_mode == Mode::kSteal && stolenCount > 0;

                // ★★⑰ NAMING A LIST COSTS THE STEAL STAMP.
                //
                // Measured: taking four cabbages out of an owned chest hands
                // RemoveItem a named list holding all four, and the four arrive
                // NOT stolen -- while a fifth unit with no list of its own
                // arrives stolen. The log line that settled it:
                //
                //   want 4 | xl=named xlCount=4 | lists=1 [4x own=n uid=0 sig=0]
                //
                // The source list carries no ownership (an owned container owns
                // its contents; the items do not own themselves), and handing
                // the engine an existing list makes it MOVE that list -- so it
                // arrives exactly as it was, unstamped. kSteal only stamps what
                // the engine creates for itself, which is why the one listless
                // unit was the only stolen one. Four plus one.
                //
                // ★So we stop asking the engine to mark it, and mark it
                // ourselves. Snapshot which lists the player already had --
                // anything that appears after the move either came across from
                // the shelf or was minted for the occasion, and both are things
                // just taken out of somebody's chest.
                //
                // ★Dropping the NAME was the first attempt and it only covered
                // one shape: with a single plain list there is no tempered spare
                // to confuse the engine with, so leaving it unnamed was safe and
                // the whole stack came over stamped. Take TWO out of five and it
                // came apart again -- one from the list, one from the listless
                // remainder, one stamped and one not. Which lists the engine
                // creates and which it moves is not a thing to keep guessing at.
                // The name goes back (GI39 wants it), and the ownership is ours
                // to state.
                std::set<RE::ExtraDataList*> hadBefore;
                if (anyStolen) {
                    if (auto* e = Grid::LiveEntryOf(player, r.obj); e && e->extraLists) {
                        for (auto* l : *e->extraLists) {
                            if (l) hadBefore.insert(l);
                        }
                    }
                    // ★★SAY WHOSE IT IS WHILE IT IS STILL ON THE SHELF.
                    //
                    // Ownership lives on an ExtraDataList, and the engine hands
                    // out units from a container two different ways: it moves an
                    // existing list, or it lets units in as bare count and mints
                    // a list for them. Measured, taking two cabbages out of the
                    // Sleeping Giant, it did BOTH at once --
                    //
                    //   shelf before: delta=4 [4x own=-] lists=1
                    //   player after: delta=2 [1x own=<faction>] lists=1
                    //
                    // -- one unit minted and stamped, one let in bare. A bare
                    // unit has nowhere to carry an owner, so it arrives clean.
                    // Four-plus-one and one-plus-one were this same split at
                    // different sizes.
                    //
                    // Marking them AFTER the fact cannot fix the bare half:
                    // there is no list to write on, and ExtraDataList is a game
                    // class we cannot construct. Marking them BEFORE needs
                    // neither -- the shelf's list already exists, and whatever
                    // the engine then moves or splits off carries the owner with
                    // it. Which is also simply true: the items in a chest in
                    // somebody's inn are theirs, and the game only leaves that
                    // implicit until one of them travels.
                    //
                    // ★The owner was never missing, either. It is a faction with
                    // no NAME, so an earlier log printed "owner " and I read it
                    // as null and went hunting through the cell for it. It was
                    // on the ref the whole time.
                    // ★★ONLY AS MANY AS ARE BEING STOLEN. This stamped every
                    // unowned list on the shelf, which was harmless while the
                    // whole container was somebody else's and wrong the moment
                    // the deposit ledger arrived: ask for five where two are
                    // yours and all five were branded on the way out.
                    if (auto* owner = ContainerOwner(source)) {
                        if (auto* e = Grid::LiveEntryOf(source, r.obj); e && e->extraLists) {
                            int left = stolenCount;
                            for (auto* l : *e->extraLists) {
                                if (left <= 0) break;
                                if (l && !l->GetOwner()) { l->SetOwner(owner); --left; }
                            }
                        }
                    }
                }
                // ★★★kSteal IS ABOUT THE UNIT, NOT THE ROOM.
                //
                // This said "steal mode -> kSteal" for everything in the
                // container, and kSteal is how the engine is told to write
                // ownership onto what it hands over. So storing your own sword
                // in an innkeeper's chest and taking it straight back handed it
                // to you stamped -- confirmed by opening the VANILLA inventory
                // on the same item, where the mark was also there, so it was
                // real ownership rather than a mark of ours. It is the reported
                // Embassy case exactly: the player's own confiscated gear comes
                // home through a chest in a hostile cell.
                //
                // Vanilla does neither, and measurement says why: a unit that
                // really belongs to somebody ALREADY CARRIES THEM. Taking a
                // cheese off Lucan's shelf reads owner='Lucan Valerius' before
                // anything of ours runs. A unit the player put there carries
                // nothing. So ask the unit instead of the room.
                //
                // ★The crime is unaffected -- StealAlarm above is what the
                // guards answer to, and it still fires on the container's
                // ownership. Confirmed in game: NPCs react identically, both
                // storing and taking. Only the ownership stamp changes.
                // ★★TWO CALLS WHEN THE ASK STRADDLES THE LEDGER. The reason is
                // per-call, so three units of which two are yours cannot be
                // described in one: the engine would stamp all three or none.
                // Split it and each half arrives as what it is.
                // ★The deposit half goes FIRST -- vanilla serves your own
                // goods before the room's, and a player who takes two of three
                // should get the two that were theirs.
                GuardedRemove(source, r.obj,
                    pick.kind == Grid::PickKind::kFallback, "take", [&]() {
                    if (fromDeposit > 0) {
                        source->RemoveItem(r.obj, fromDeposit,
                            RE::ITEM_REMOVE_REASON::kRemove, pick.xl, player);
                    }
                    if (stolenCount > 0) {
                        // ★★★ASK AGAIN BETWEEN THE TWO CALLS. `pick.xl` named a
                        // unit in the SOURCE, and the call above just moved
                        // units out of it -- when the deposit half consumed
                        // that list outright the engine MOVES it to the player
                        // (measured, see the note above this case), so handing
                        // the same pointer back names a list the source no
                        // longer owns. The pickpocket path already re-resolves
                        // after engine code runs; this one did not.
                        RE::ExtraDataList* xl2 = pick.xl;
                        if (fromDeposit > 0) {
                            const auto again = SourceUnit(source, r);
                            xl2 = again.ok() ? again.xl : nullptr;   // null = engine picks
                        }
                        source->RemoveItem(r.obj, stolenCount,
                            g_mode == Mode::kSteal ? RE::ITEM_REMOVE_REASON::kSteal
                                                   : RE::ITEM_REMOVE_REASON::kRemove,
                            xl2, player);
                    }
                });
                if (anyStolen) {
                    // ★★A UNIT WITH NO LIST CANNOT BE STOLEN, because ownership
                    // lives on the list. That is the whole of this bug.
                    //
                    // Measured, taking two cabbages out of the Sleeping Giant:
                    //
                    //   shelf before: delta=4 [4x own=-] lists=1
                    //   player after: delta=2 [1x own=<faction>] lists=1
                    //
                    // Two units arrive. The engine gave ONE of them a list and
                    // stamped it -- kSteal marks what the engine creates -- and
                    // let the other in as bare count. No list, nowhere to write
                    // an owner, not stolen. Four-plus-one and one-plus-one were
                    // always this same split at different sizes.
                    //
                    // ★And the owner was never missing. It is a faction with no
                    // NAME, so the earlier log printed "owner " and I read that
                    // as null and went looking for it in the cell. It had been
                    // on the ref the whole time.
                    //
                    // So: stamp the lists that arrived unowned, and give the
                    // listless remainder a list of its own to be owned by. The
                    // engine's own stamp is left alone where it applied.
                    // ★A list that arrived without an owner is still marked
                    // here -- the pre-stamp covers what the shelf was already
                    // holding, and this covers anything the engine produced from
                    // outside it. A list that ALREADY has an owner is left
                    // alone: it was stolen before it got here, and rewriting
                    // whose it is would rewrite who the player answers to.
                    // ★★AND ONLY AS MANY AS WERE STOLEN, for the reason given
                    // at the shelf-side stamp above: the units that arrived
                    // are a mix of the player's deposit and the room's goods,
                    // and only the second half is anybody else's.
                    auto* owner = ContainerOwner(source);
                    int   marked = 0;
                    if (owner) {
                        if (auto* e = Grid::LiveEntryOf(player, r.obj); e && e->extraLists) {
                            for (auto* l : *e->extraLists) {
                                if (marked >= stolenCount) break;
                                if (!l || hadBefore.contains(l) || l->GetOwner()) continue;
                                l->SetOwner(owner);
                                ++marked;
                            }
                        }
                    }
                    if (marked > 0) {
                        SKSE::log::info("[STEAL] '{}' x{}: {} list(s) stamped for {:08X}",
                                        r.obj->GetName() ? r.obj->GetName() : "?",
                                        r.count, marked,
                                        owner ? owner->GetFormID() : 0);
                    }
                }
                ClearOut(r.obj, r.uid, r.sig, r.count);   // engine moved it
                itemSound(r.obj, true);
                // ★(1.3.0) SHELF USE MODE. The unit is in the player's pack as
                // of the RemoveItem above, so from here it is an ordinary
                // right-click -- the player-side path already knows every kind
                // (potion, food, scroll, spell tome). QUEUED, not called: the
                // engine call belongs to Equip::ProcessPending, and this is not
                // its turn.
                // ★xlIdx -1 on purpose: a list POSITION captured on the shelf
                // means nothing in the pack it has just arrived in. The
                // signature travelled with the ExtraDataList and still names it.
                if (r.useAfter) {
                    // ★It is passing through, not moving in: a typed bag must not
                    // adopt a unit that is about to be drunk.
                    Grid::NoteTransientArrival(r.obj->GetFormID());
                    Equip::UseItem(r.obj, r.uid, -1, r.sig, {}, r.count);
                }
                break;
            }
            case XferReq::kStore: {
                // GI36: resolve + strip the star in one call, then hand the very
                // list we got to the engine (rule 58).
                auto* sxl = Grid::ResolveExitUnit(r.obj, r.uid, r.sig, r.count,
                                                  r.fav ? r.count : 0, r.xlIdx);
                // ★★WRITE IT IN THE LEDGER. Your gear survives a chest in a
                // hostile cell because taking it back is not theft, and this
                // count is how the take side knows that (see
                // ContLayout::deposits). Recorded even outside steal mode: an
                // ordinary chest can become an owned one, and a deposit made
                // before that should still be yours afterwards.
                //
                // ★★★EXCEPT STOLEN GOODS, which a deposit would launder. The
                // ledger counts units, not identities, so a stolen sack put in
                // beside your own raises the count by one and comes back out
                // as yours -- and kRemove has the engine clear the ownership on
                // the way, so it is genuinely clean afterwards. Measured: five
                // in, five out, all five plain. A chest is not a fence.
                //
                // ★★AND CREDIT WHAT LEFT, AFTER IT LEAVES. This ran before the
                // engine call and booked the full requested count regardless
                // of what the engine did with it -- a refusal (RemoveItem
                // silently taking nothing, which is how the engine says no)
                // left a credit behind for units still in the player's hands.
                // Measure the holding across the call instead. See
                // DepositProvable for the other half: whether it is ours to
                // claim at all.
                const bool provable = DepositProvable(player, r.obj, sxl);
                const int  heldBefore = provable ? HeldCount(player, r.obj) : 0;
                // ★TEST ONLY (!simrefuse): skip the ENGINE CALL and nothing
                // else. A real refusal is RemoveItem quietly not taking -- our
                // own bookkeeping still runs, because the engine's silence does
                // not reach back and cancel it. The first version bailed out of
                // the whole case, which also skipped ClearPendingRemove, so the
                // tile stayed suppressed and the recovery rebuild had nothing to
                // show. That was a situation the game cannot produce: §10-7's
                // rule about stress tools, and I broke it the same day I wrote
                // it down.
                if (Ledger::SimRefuse()) {
                    SKSE::log::warn("[XFER] !simrefuse: engine call SKIPPED for "
                                    "store '{}' x{}", r.obj->GetName(), r.count);
                } else {
                    GuardedRemove(player, r.obj, sxl == nullptr, "store", [&]() {   // GI42
                        player->RemoveItem(r.obj, r.count,
                            RE::ITEM_REMOVE_REASON::kStoreInContainer, sxl, source);
                    });
                }
                if (provable) {
                    const int moved = (std::min)(r.count,
                        (std::max)(0, heldBefore - HeldCount(player, r.obj)));
                    if (moved > 0) DepositAdd(source, r.obj, moved);
                }
                // (B4-3c: no counter to drain -- the engine event that this
                // call just fired has already confirmed the ledger entry)
                ClearIn(r.obj, r.uid, r.sig, r.count);   // the engine caught up
                itemSound(r.obj, false);
                break;
            }
            case XferReq::kBuy: {
                // merchant -> player; player pays, merchant receives.
                const auto pick = SourceUnit(source, r);
                if (!pick.ok()) {   // GI42: e.g. the merchant WEARS the twin
                    ClearOut(r.obj, r.uid, r.sig, r.count);
                    Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                    SKSE::log::error("[XFER] buy refused: unnameable '{}' uid "
                        "{:04X} sig {:04X}", r.obj->GetName(), r.uid, r.sig);
                    break;
                }
                GuardedRemove(source, r.obj,
                    pick.kind == Grid::PickKind::kFallback, "buy", [&]() {
                    source->RemoveItem(r.obj, r.count, RE::ITEM_REMOVE_REASON::kRemove,
                        pick.xl, player);
                });
                if (gold && r.price > 0) {
                    player->RemoveItem(gold, r.price, RE::ITEM_REMOVE_REASON::kRemove,
                        nullptr, nullptr);
                    source->AddObjectToContainer(gold, nullptr, r.price, nullptr);
                    goldMoved = true;
                }
                ClearOut(r.obj, r.uid, r.sig, r.count);   // engine moved it
                itemSound(r.obj, true);   // the purchase lands in your hands
                // vanilla speech XP: base value through the AVIF skill curve
                if (const float xp = SpeechXP(r.base); xp > 0.0f)
                    player->AddSkillExperience(RE::ActorValue::kSpeech, xp);
                break;
            }
            case XferReq::kSell:
                // D4: same reasoning as kStore -- name the unit that leaves.
                // player -> merchant; player receives, merchant pays. kSelling
                // lets the engine clear ownership/stolen flags.
                {
                auto* sxl = Grid::ResolveExitUnit(r.obj, r.uid, r.sig, r.count,   // GI36
                                                  r.fav ? r.count : 0, r.xlIdx);
                GuardedRemove(player, r.obj, sxl == nullptr, "sell", [&]() {   // GI42
                    player->RemoveItem(r.obj, r.count, RE::ITEM_REMOVE_REASON::kSelling,
                        sxl, source);
                });
                }
                if (gold && r.price > 0) {
                    player->AddObjectToContainer(gold, nullptr, r.price, nullptr);
                    source->RemoveItem(gold, r.price, RE::ITEM_REMOVE_REASON::kRemove,
                        nullptr, nullptr);
                    goldMoved = true;
                    goldIn = true;
                }
                if (const float xp = SpeechXP(r.base); xp > 0.0f)
                    player->AddSkillExperience(RE::ActorValue::kSpeech, xp);
                break;
            case XferReq::kPickTake: {
                // GI42: judge the pick BEFORE the roll. Refusing after
                // AttemptPickpocket would leave the XP, the detection roll and
                // the sound already spent on a move that never happens.
                if (!SourceUnit(source, r).ok()) {
                    ClearOut(r.obj, r.uid, r.sig, r.count);
                    Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                    SKSE::log::error("[XFER] pick refused: unnameable '{}' uid "
                        "{:04X} sig {:04X}", r.obj->GetName(), r.uid, r.sig);
                    break;
                }
                // F6b: the ENGINE rolls the attempt (chance / detection /
                // crime response / XP live inside AttemptPickpocket).
                auto inv = source->GetInventory();
                const auto ei = inv.find(r.obj);
                if (ei == inv.end() || ei->second.first < r.count) break;
                const int before = ei->second.first;
                const bool won = player->AttemptPickpocket(source,
                    ei->second.second.get(), r.count, true);
                if (!won) {
                    // caught: the engine raised the alarm — force-close the
                    // menu; everything already taken this session stays.
                    // Nothing left the target, so drop every suppression this
                    // request and the ones still queued behind it armed.
                    g_outPool.clear();
                    g_outForm.clear();
                    CancelQueuedOutgoing("pickpocket caught");   // B4-3b
                    g_xfer.clear();
                    UIRoot::Close();
                    Grid::RequestRebuild();
                    Grid::MarkCapacityDirty();
                    return;
                }
                // defensive: some paths move the item inside the attempt —
                // move it ourselves only if it's still on the target
                int now = 0;
                for (auto& [obj2, d2] : source->GetInventory()) {
                    if (obj2 == r.obj) { now = d2.first; break; }
                }
                if (now >= before) {
                    // D4/GI42: resolve AGAIN -- the roll ran engine code, and a
                    // list captured before it is not trusted across that call.
                    const auto pick = SourceUnit(source, r);
                    if (!pick.ok()) {
                        ClearOut(r.obj, r.uid, r.sig, r.count);
                        SKSE::log::error("[XFER] pick refused post-roll: '{}'",
                            r.obj->GetName());
                        break;
                    }
                    GuardedRemove(source, r.obj,
                        pick.kind == Grid::PickKind::kFallback, "pick", [&]() {
                        source->RemoveItem(r.obj, r.count,
                            RE::ITEM_REMOVE_REASON::kSteal, pick.xl, player);
                    });
                }
                // ★★★KNOWN EXCEPTION, accepted (author's call) -- do not
                // re-investigate from scratch. AMMO the player reverse-
                // pickpocketed INTO the mark comes back WITHOUT a stolen tag.
                // Everything else does get one, ammo the mark owned itself does
                // get one, and every other STACKABLE (potions, ingredients) gets
                // one on the same round trip. Measured with a temporary
                // [PICKOWN] log, since removed; these three were ruled out:
                //   · AttemptPickpocket moving the item itself, which would skip
                //     the kSteal removal below   -> engineMovedItself = false
                //   · that removal not running   -> ourRemoveRan = true
                //   · a surviving PLAYER ownership stamp that IsOwnedBy then
                //     skips -> there is no stamp: the units arrive with NO
                //     ExtraDataList at all ("player lists: (none)")
                // With no list there is nothing to stamp either -- an
                // ExtraDataList cannot be created from a plugin -- so any fix
                // has to be upstream of the arrival, in whatever drops the list
                // on the way in or out. Not pursued.
                // ★Note this is also the one place our pickpocket matches VANILLA
                // and the rest of it does not: vanilla treats anything you put on
                // a mark as still yours, so taking it back is never theft. Ours
                // makes it the mark's property, which the author prefers and
                // deliberately kept -- ammo is simply the case that stayed
                // vanilla-shaped.
                ClearOut(r.obj, r.uid, r.sig, r.count);   // engine moved it
                itemSound(r.obj, true);
                break;
            }
            case XferReq::kPickStore: {
                // F6b reverse-pickpocket: same engine roll, item flows the
                // other way (fromContainer = false).
                auto pinv = player->GetInventory();
                const auto ei = pinv.find(r.obj);
                if (ei == pinv.end() || ei->second.first < r.count) break;
                const int before = ei->second.first;
                const bool won = player->AttemptPickpocket(source,
                    ei->second.second.get(), r.count, false);
                if (!won) {
                    // the roll failed: nothing leaves, so give back every
                    // suppression this request (and any still queued behind it)
                    // put in place at click time
                    g_outPool.clear();
                    g_outForm.clear();
                    g_in.clear();   // ...and nothing was promised to them either
                    // B4-3c: the counter drains that lived here went with the
                    // counters. r rides in g_xfer too -- its own ledger entry
                    // cancels with the queue's in one sweep.
                    CancelQueuedOutgoing("pickpocket roll lost");
                    g_xfer.clear();
                    UIRoot::Close();
                    Grid::RequestRebuild();
                    Grid::MarkCapacityDirty();
                    return;
                }
                // (the suppression was armed at REQUEST time -- see
                // RequestPickStore. Arming it here, one Tick after the click,
                // left the planted item on the board for those frames and then
                // yanked it: a plant blinked every single time.)
                int now = 0;
                for (auto& [obj2, d2] : player->GetInventory()) {
                    if (obj2 == r.obj) { now = d2.first; break; }
                }
                if (now >= before) {
                    // player side: the worn-excluding resolver, so planting an
                    // item can never hand over the copy you are wearing
                    // GI36: only inside the WIN branch -- a failed roll must
                    // leave the star exactly where it was.
                    auto* sxl = Grid::ResolveExitUnit(r.obj, r.uid, r.sig, r.count,
                                                      r.fav ? r.count : 0, r.xlIdx);
                    GuardedRemove(player, r.obj, sxl == nullptr, "plant", [&]() {   // GI42
                        player->RemoveItem(r.obj, r.count,
                            RE::ITEM_REMOVE_REASON::kStoreInContainer, sxl, source);
                    });
                }
                // (B4-3c: whichever branch moved the item -- the attempt
                // itself or our RemoveItem above -- its container event has
                // confirmed the ledger entry; no counter left to drain)
                // Poisoned perk: the vanilla menu applies a planted poison to
                // the mark OUTSIDE AttemptPickpocket, so mirror it here — cast
                // the poison's effects on the target (self-sourced caster, no
                // blame actor = silent, no aggro/crime) and consume ONE planted
                // unit; any extra units stay planted as regular items.
                if (auto* poison = r.obj->As<RE::AlchemyItem>();
                    poison && poison->IsPoison() && HasPoisonedPerk()) {
                    if (auto* mark = source->As<RE::Actor>()) {
                        if (auto* caster = mark->GetMagicCaster(
                                RE::MagicSystem::CastingSource::kInstant)) {
                            caster->CastSpellImmediate(poison, false, mark,
                                1.0f, false, 0.0f, nullptr);
                        }
                        mark->RemoveItem(r.obj, 1,
                            RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        SKSE::log::info("[PICK] Poisoned perk: applied to mark");
                    }
                }
                ClearIn(r.obj, r.uid, r.sig, r.count);   // the engine caught up
                itemSound(r.obj, false);
                break;
            }
            }
        }
        g_xfer.clear();
        if (goldMoved && gold) {
            // one coin clink per transaction batch: picked up when selling,
            // put down when paying
            player->PlayPickUpSound(gold, goldIn, false);
        }
        if (goldMoved) GoldCoins::MarkDirty();   // rebuild the coin mirror
        // ★★1.4/B3: no tail rebuild any more, and the reason is measured
        // (!rbdrop, §3-9): fourteen asks dropped over a full partner-window
        // session -- withdraw, store, buy, sell, take-all -- and nothing went
        // stale. Every unit this batch moved raises a container event, and the
        // sink's per-form reconcile (or its own rebuild fallback) is the one
        // that answers it. The pickpocket-failure returns above keep theirs:
        // they close the menu with suppressions half-armed, which is exactly
        // the discontinuity a full rebuild is for.
        Grid::MarkCapacityDirty();
    }

    namespace
    {
        // GI46: largest n in [0, a_cap] the payer can afford. Totals round ONCE
        // on the whole amount (B7), so a division is off by one at the edges --
        // probe the real total function instead (monotonic, binary search).
        int MaxAffordable(bool a_buy, RE::TESBoundObject* a_obj, int a_unitValue,
                          int a_cap, int a_gold)
        {
            if (a_cap <= 0) return 0;
            const auto total = [&](int a_n) {
                return a_buy ? BuyPriceTotal(a_obj, a_unitValue, a_n)
                             : SellPriceTotal(a_obj, a_unitValue, a_n);
            };
            if (total(a_cap) <= a_gold) return a_cap;
            int lo = 0, hi = a_cap;   // total(lo) affordable, total(hi) not
            while (hi - lo > 1) {
                const int mid = lo + (hi - lo) / 2;
                (total(mid) <= a_gold ? lo : hi) = mid;
            }
            return lo;
        }
    }

    // ★★★WHAT TO PUT IN A POPUP'S TITLE, coins included. The coin forms carry NO
    // name on purpose (the esp blanks them so TrueHUD skips them, and the grid
    // shows the amount badge instead), so the "?" fallback -- meant for a form
    // with a genuinely missing name -- is what the player saw when splitting
    // gold. Name it what the rest of the UI calls it; Lang::Str::Gold follows
    // the language setting.
    // ★SentenceCase because that string is the stats panel's LABEL and is
    // spelled "GOLD" for that row. Every other title here is an item name in
    // ordinary case, so the shout stood out as the one word in caps. Non-ASCII
    // scripts pass through untouched.
    // ★One function because it was two copies, and the second one's comment
    // ("Same nameless-coin case as the slider above") was the tell.
    [[nodiscard]] std::string PopupTitleOf(RE::TESBoundObject* a_obj)
    {
        if (a_obj && GoldCoins::IsCoinForm(a_obj->GetFormID()) &&
            !GoldCoins::IsPouch(a_obj->GetFormID())) {
            return Lang::SentenceCase(Lang::T(Lang::Str::Gold));
        }
        const char* n = a_obj ? a_obj->GetName() : nullptr;
        return (n && *n) ? n : "?";
    }

    void DrawSlider()
    {
        if (!g_slider.active) return;

        // UNIFIED popup chrome (user feedback: the plain bordered box read as
        // a different design language): same managed-window construction as
        // the settings / pouch / loadout-confirm windows — WinManager pos +
        // tracked TitleBar (torn frame, crimson strip, drag) + centred body.
        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 3.0f * btnW + 16.0f * S;   // GI46: MAX | OK | Cancel
        const float sliderW = 220.0f * S;

        const std::string title = PopupTitleOf(g_slider.obj);
        const char* const name = title.c_str();
        const char* lbl = Lang::T(Lang::Str::TakeLabel);
        switch (g_slider.dir) {
        case XferDir::kStore:     lbl = Lang::T(Lang::Str::StoreLabel); break;
        case XferDir::kPickup:    lbl = Lang::T(Lang::Str::SplitLabel); break;
        case XferDir::kBuy:       lbl = Lang::T(Lang::Str::BuyLabel); break;
        case XferDir::kSell:      lbl = Lang::T(Lang::Str::SellLabel); break;
        case XferDir::kPickStore: lbl = Lang::T(Lang::Str::StoreLabel); break;
        case XferDir::kDrop:      lbl = Lang::T(Lang::Str::DropLabel); break;
        default: break;   // kTake / kPickTake share the Take label
        }
        const bool barter = g_slider.dir == XferDir::kBuy ||
                            g_slider.dir == XferDir::kSell;

        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        // tracked (letter-spaced) title needs headroom over the raw text width
        const float contentW = (std::max)({ btnRow, sliderW,
            ImGui::CalcTextSize(name).x * 1.35f });
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + sp + ImGui::GetFrameHeight() +
                (barter ? lineH + sp : 0.0f) + 6.0f * S +
                ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        if (wm->BeginConfirmPopup("slider", "##gi_slider", name, size)) {
            g_slider.active = false;   // outside click closes
            Grid::ClearDropHint();     // B2
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(lbl).x);
        ImGui::TextColored(sk.inkDim, "%s", lbl);
        center(sliderW);
        const int qtyBefore = g_slider.value;
        // The floor is 0, not 1: dragging all the way down is how a player says
        // "none of it", and confirming there is the same as cancelling.
        const bool qtyMoved =
            Theme::ChromeSliderInt("##qty", &g_slider.value, 0, g_slider.max, sliderW);
        g_slider.value = (std::max)(0, (std::min)(g_slider.value, g_slider.max));
        if (qtyMoved && g_slider.value != qtyBefore) {
            // direction-aware scrub tick, rate-limited so a fast drag
            // doesn't machine-gun the blip
            static double s_lastTick = 0.0;
            const double now = ImGui::GetTime();
            if (now - s_lastTick > 0.06) {
                s_lastTick = now;
                if (g_slider.value > qtyBefore) Sfx::SelectOn();
                else                            Sfx::SelectOff();
            }
        }
        // GI51: LEFT/RIGHT nudge the quantity by 1 (hold repeats). In menu
        // mode the engine translates A/D into arrow GFx codes, so WASD works
        // through the same binding. GI52 (review): every popup key stands
        // down while a text field owns the keyboard -- typed spaces/enters
        // must never confirm a popup that happens to be open.
        const bool typing = ImGui::GetIO().WantTextInput;
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && g_slider.value > 0) {
            --g_slider.value;
            Sfx::SelectOff();
        }
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) &&
            g_slider.value < g_slider.max) {
            ++g_slider.value;
            Sfx::SelectOn();
        }

        // live total price for barter (updates as the quantity moves) — turns
        // crimson when the payer can't afford it (buy: player, sell: merchant)
        if (barter) {
            const bool buy = g_slider.dir == XferDir::kBuy;
            const int total = buy
                ? BuyPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value)
                : SellPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value);
            const bool broke = buy ? Grid::GoldAmount() < total
                                   : (total > 0 && MerchantGold() < total);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d G", total);
            center(ImGui::CalcTextSize(buf).x);
            // ★Val(), not sk.hi. A figure is a figure on every skin, and on
            // SIMPLE sk.hi is a dark blue that reads as structure — a border,
            // not a number (see Theme::ValVec).
            ImGui::TextColored(broke ? ImVec4(0.8f, 0.32f, 0.28f, 1.0f)
                                     : Theme::ValVec(), "%s", buf);
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        // GI46: one-click whole-stack transfer. For barter the cap also folds
        // in the payer's purse -- buying 200 arrows with 50 arrows' gold moves
        // the 50, instead of buzzing "not enough gold" and moving nothing.
        // ★M is the Max KEY -- what the pad's Y arrives as (TranslatePadButtons
        // remaps it while a popup is up), and a small bonus for the keyboard.
        bool maxPress = Sfx::Button(Lang::T(Lang::Str::MaxLabel), ImVec2(btnW, 0)) ||
                        (!typing && ImGui::IsKeyPressed(ImGuiKey_M, false));
        if (maxPress) {
            int cap = g_slider.max;
            if (g_slider.dir == XferDir::kBuy) {
                cap = MaxAffordable(true, g_slider.obj, g_slider.unitValue, cap,
                                    Grid::GoldAmount());
            } else if (g_slider.dir == XferDir::kSell) {
                cap = MaxAffordable(false, g_slider.obj, g_slider.unitValue, cap,
                                    MerchantGold());
            }
            if (cap < 1) {
                Sfx::FailNote(Lang::T(g_slider.dir == XferDir::kBuy
                    ? Lang::Str::NotEnoughGold : Lang::Str::MerchantNoGold));
                maxPress = false;
            } else {
                g_slider.value = cap;   // the confirm path below reads this
            }
        }
        ImGui::SameLine(0.0f, 8.0f * S);
        const bool ok = maxPress ||
                        Sfx::Button(Lang::T(Lang::Str::Confirm), ImVec2(btnW, 0)) ||
                        (!typing && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_Space, false)));
        ImGui::SameLine(0.0f, 8.0f * S);
        const bool cancel = Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true) ||
                            ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        // Confirming at zero means "none of it" -- take the cancel path rather
        // than firing a request for 0 units.
        if (ok && g_slider.value > 0) {
            switch (g_slider.dir) {
            // ⛔kTake and kStore no longer reach this switch: a move is a
            //  whole-cell act now (1.5.x stack flow) and raises no window, so
            //  the two arms that answered one are gone rather than left to
            //  read as live paths. OpenSlider still knows kTake for the
            //  CLAMP it shares with kBuy/kPickTake.
            case XferDir::kShelfSplit: {
                const std::string nk = SplitShelfCell(g_slider.srcKey, g_slider.value);
                if (!nk.empty()) {
                    g_actingSpot = nk;   // the carry names its own cell
                    Grid::BeginPartnerCarry(g_slider.obj, g_slider.value,
                                            g_slider.unitValue, -1.0f, -1.0f,
                                            g_slider.uid, g_slider.xlIdx, 0, 0);
                }
                break;
            }
            case XferDir::kPickTake:
                RequestPickTake(g_slider.obj, g_slider.value, g_slider.uid, g_slider.sig,
                                g_slider.worn);
                break;
            case XferDir::kPickStore:
                // pending-remove is noted on the WIN inside the Tick (a lost
                // roll must leave the tile untouched)
                RequestPickStore(g_slider.obj, g_slider.value, g_slider.uid, g_slider.sig,
                                 g_slider.srcKey, g_slider.fav, g_slider.xlIdx);
                break;
            case XferDir::kDrop:
                // ★the tile's own hand does the dropping (layout, bag reflow
                // and the star's death all live on that side) -- this window
                // only carries the number to it
                Grid::DropTileUnits(g_slider.srcKey, g_slider.value);
                break;
            case XferDir::kPickup: Grid::PickupPartial(g_slider.obj, g_slider.value, g_slider.srcKey, g_slider.max); break;
            case XferDir::kBuy: {
                const int total = BuyPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value);
                if (Grid::GoldAmount() < total) {
                    Sfx::FailNote(Lang::T(Lang::Str::NotEnoughGold));
                } else if (Grid::MaxAcceptUnits(g_slider.obj, g_slider.value) < g_slider.value) {
                    // space may have changed since the slider opened
                    Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                } else {
                    RequestBuy(g_slider.obj, g_slider.value, total,
                        g_slider.unitValue * g_slider.value,
                        g_slider.uid, g_slider.sig);
                }
                break;
            }
            case XferDir::kSell: {
                const int total = SellPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value);
                if (total > 0 && MerchantGold() < total) {
                    Sfx::FailNote(Lang::T(Lang::Str::MerchantNoGold));
                } else {
                    RequestSell(g_slider.obj, g_slider.value, total,
                        g_slider.unitValue * g_slider.value,
                        g_slider.uid, g_slider.sig, g_slider.fav, g_slider.xlIdx,
                        g_slider.srcKey);
                    Grid::NotePendingRemove(g_slider.obj, g_slider.srcKey, g_slider.value,
                                            g_slider.xlIdx);
                }
                break;
            }
            }
            g_slider.active = false;
        } else if (cancel || ok) {   // `ok` here can only be a zero-quantity confirm
            g_slider.active = false;
            Grid::ClearDropHint();   // B2: the pending drop-cell hint dies with the slider
        }
        ImGui::End();
    }

    void AskSellConfirm(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal,
                        const std::string& a_srcKey, std::uint16_t a_uid, std::uint16_t a_sig,
                        bool a_fav, int a_xlIdx)
    {
        if (a_obj && a_count > 0) {
            g_confirm = { true, a_obj, a_count, a_price, a_baseTotal, a_srcKey, a_uid, a_sig,
                          a_fav, a_xlIdx };
            Sfx::SelectOn();
        }
    }

    bool ConfirmActive() { return g_confirm.active; }

    void DrawConfirm()
    {
        if (!g_confirm.active) return;

        // UNIFIED popup chrome — same managed construction as the slider /
        // pouch / loadout-confirm windows (title = item name, centred body).
        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;

        const std::string title = PopupTitleOf(g_confirm.obj);
        const char* const name = title.c_str();
        const char* question = Lang::T(Lang::Str::SellFavoriteConfirm);
        char priceLine[32];
        std::snprintf(priceLine, sizeof(priceLine), "%d G", g_confirm.price);

        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float contentW = (std::max)({ btnRow,
            ImGui::CalcTextSize(question).x,
            ImGui::CalcTextSize(name).x * 1.35f });
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + 2.0f * lineH + sp + 6.0f * S +
                ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        if (wm->BeginConfirmPopup("sellconfirm", "##gi_sellconfirm", name, size)) {
            g_confirm.active = false;   // outside click cancels
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(question).x);
        ImGui::TextColored(sk.ink, "%s", question);
        center(ImGui::CalcTextSize(priceLine).x);
        ImGui::TextColored(Theme::ValVec(), "%s", priceLine);   // a price is a figure

        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        const bool typing = ImGui::GetIO().WantTextInput;   // GI52
        const bool ok = Sfx::Button(Lang::T(Lang::Str::Confirm), ImVec2(btnW, 0)) ||
                        (!typing && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_Space, false)));
        ImGui::SameLine(0.0f, 8.0f * S);
        const bool cancel = Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true) ||
                            ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ok) {
            RequestSell(g_confirm.obj, g_confirm.count, g_confirm.price, g_confirm.base,
                        g_confirm.uid, g_confirm.sig, g_confirm.fav, g_confirm.xlIdx,
                        g_confirm.srcKey);
            Grid::NotePendingRemove(g_confirm.obj, g_confirm.srcKey, g_confirm.count,
                                    g_confirm.xlIdx);
            g_confirm.active = false;
        } else if (cancel) {
            g_confirm.active = false;
        }
        ImGui::End();
    }

namespace
{
    int ShelfGoldOf(const std::string& a_spotKey)
    {
        if (a_spotKey.empty()) return 0;
        auto* p = Partner();
        if (!p) return 0;
        const auto ci = g_contLayouts.find(p->GetFormID());
        if (ci == g_contLayouts.end()) return 0;
        const auto si = ci->second.cells.find(a_spotKey);
        return si == ci->second.cells.end() ? 0 : si->second.gold;
    }
}

    int HeldShelfGold()
    {
        // ★(1.5.x) a BUNDLE-carried pouch keeps its amount on its entry --
        // the cursor ghost asks here too, so the band survives the lift
        if (g_bundleCarry.active) {
            const auto ci = g_contLayouts.find(g_bundleCarry.cont);
            if (ci != g_contLayouts.end()) {
                if (const auto si = ci->second.cells.find(g_bundleCarry.spot);
                    si != ci->second.cells.end()) {
                    for (const auto& b : si->second.bundle) {
                        if (b.id == g_bundleCarry.id) {
                            return (std::max)(-1, b.gold);
                        }
                    }
                }
            }
            return -1;
        }
        // only a partner-side carry has a reserved shelf slot to ask
        if (g_actingSpot.empty() || !Grid::HeldPartnerObject()) return -1;
        return ShelfGoldOf(g_actingSpot);
    }

    std::uint32_t MintBundleId() { return NextBundleId(); }

    void NoteBagBundle(RE::FormID a_bagForm, std::vector<BundleItem> a_items)
    {
        auto* p = Partner();
        if (!p || a_items.empty()) return;
        g_pendingBundles.push_back({ p->GetFormID(), a_bagForm, std::move(a_items) });
        SKSE::log::info("[LOOT] bag bundle noted: {} kind(s) follow bag {:08X}",
            g_pendingBundles.back().items.size(), a_bagForm);
    }

    std::vector<BundleItem> TakeIncomingBundle(RE::FormID a_bagForm)
    {
        for (auto it = g_incomingBundles.begin(); it != g_incomingBundles.end(); ++it) {
            if (it->bagForm != a_bagForm) continue;
            auto v = std::move(it->items);
            g_incomingBundles.erase(it);
            return v;
        }
        return {};
    }

    bool IsBundleCarry() { return g_bundleCarry.active; }

    bool IsShelfBagOpen(std::string_view a_spotKey)
    {
        for (const auto& w : g_shelfBags) {
            if (w.bag == 0 && w.spot == a_spotKey) return true;
        }
        return false;
    }

    bool ConsumeBundleCarry(RE::TESBoundObject* a_obj, int a_count, bool a_toPlayer)
    {
        if (!g_bundleCarry.active || !a_obj ||
            a_obj->GetFormID() != g_bundleCarry.form) {
            return false;
        }
        g_bundleCarry.active = false;
        const auto ci = g_contLayouts.find(g_bundleCarry.cont);
        if (ci == g_contLayouts.end()) return true;
        const auto si = ci->second.cells.find(g_bundleCarry.spot);
        if (si == ci->second.cells.end()) return true;
        auto& bundle = si->second.bundle;
        // ★the id names the exact entry, whatever has happened to the vector
        // since the lift. The anchor used to do this, and an anchor is where
        // something SITS -- two identical stacks were told apart by their
        // squares, so anything that moved either of them picked the wrong one.
        auto it = std::find_if(bundle.begin(), bundle.end(), [&](const BundleItem& b) {
            return b.id == g_bundleCarry.id;
        });
        if (it == bundle.end()) {
            it = std::find_if(bundle.begin(), bundle.end(), [&](const BundleItem& b) {
                return b.form == g_bundleCarry.form && b.sig == g_bundleCarry.sig;
            });
        }
        if (it != bundle.end()) {
            // ★★A BAG LEAVES WITH ITS BRANCH, however it leaves. Dragging one
            // out of a shelf bag used to erase the single entry and orphan
            // everything under it -- the contents stayed in the bundle pointing
            // at a parent that was gone, and only surfaced when the OUTER bag
            // came home. Same tree, same rule as the store side.
            const std::uint32_t cid = it->id;
            if (Grid::ResolveDef(a_obj).bag != 0) {
                auto branch = CutBranch(bundle, cid);
                if (a_toPlayer) {
                    // ★(1.5.x) pouches inside the leaving branch: amounts park
                    // as parcels; the manifest keeps them for the exact-tile
                    // claim -- the same give-back the bag-cell take does.
                    for (auto& b : branch) {
                        if (b.gold <= 0) continue;
                        SKSE::log::info(
                            "[LOOT] bundled pouch taken back with {} G", b.gold);
                        GoldCoins::GiveAwayGold(b.gold, b.form);
                        b.awaitGold = 0;
                    }
                    SendBranchHome(a_obj, std::move(branch));
                } else if (!branch.empty()) {
                    // ★★ONTO THE SHELF, NOT HOME. Dropping a nested bag onto the
                    // container's own grid makes it a shelf CELL, and its branch
                    // belongs to that cell -- not in the player's pack. This
                    // sent it home regardless, so putting bag B out on the shelf
                    // posted its contents to the inventory instead (reported).
                    //
                    // The queue the store side already uses is the right place:
                    // the cell being born claims it (PlaceStoredCell ->
                    // TakePendingBundle), which is the same door a bag stored
                    // from the player's board comes through.
                    if (auto* pref = Partner()) {
                        g_pendingBundles.push_back({ pref->GetFormID(),
                                                     a_obj->GetFormID(),
                                                     std::move(branch) });
                        g_bagToCellSpot = g_bundleCarry.spot;
                        g_bagToCellId = cid;
                    }
                }
            } else {
                it->count -= a_count;
                if (it->count <= 0) {
                    // ★(1.5.x) a POUCH stepping out of its bag: the amount has
                    // to move books before the entry dies. Home to the player
                    // -> park it (the fresh tile claims); out onto the shelf ->
                    // it becomes an away parcel again, which is exactly what
                    // the cell being born will claim (the reconcile door).
                    if (it->gold > 0) {
                        if (a_toPlayer) {
                            SKSE::log::info(
                                "[LOOT] bundled pouch taken back with {} G",
                                it->gold);
                            GoldCoins::GiveAwayGold(it->gold, it->form);
                        } else {
                            SKSE::log::info(
                                "[LOOT] bundled pouch steps onto the shelf "
                                "with {} G", it->gold);
                            GoldCoins::RestoreAwayParcel(it->form, it->gold);
                        }
                    }
                    bundle.erase(it);   // no survivors to repair
                }
            }
        }
        return true;
    }

    // ★(1.3.1/1.3.2c) an OPEN shelf bag: a REAL bag window over the bundle
    // record -- the player-bag grammar on the container side. Items draw at
    // their anchors on the bag's own grid, lift onto the cursor (drop on
    // the player grid = take home, drop on the shelf = out of the bag, drop
    // back inside = rearrange, drop in ANOTHER open shelf bag = move over),
    // and a carried player item drops IN. Typed bags carry their COLLECT
    // button. The engine items are already in the container throughout;
    // only the bundle record moves until a transfer is actually queued.
    namespace
    {
        // one bag window. a_ord staggers the default position; returns
        // false when the window should close (the bag left the shelf).
        // ★Is the bag this window shows still in the bundle? 0 is the bag ON
        // the spot, which lives exactly as long as the cell does; anything else
        // is a nested entry, and its absence means it is gone -- taken, sold,
        // spilled -- so the window closes rather than showing a level that no
        // longer exists. There is no walking left: the name is the answer.
        bool BagAlive(const std::vector<BundleItem>& a_bundle, std::uint32_t a_id)
        {
            return a_id == 0 ||
                   std::any_of(a_bundle.begin(), a_bundle.end(),
                       [&](const BundleItem& b) { return b.id == a_id; });
        }

        // ★BY VALUE, DELIBERATELY. This took `const ShelfBagWin&` straight out
        // of g_shelfBags and then pushed onto that same vector when a nested
        // bag was opened -- one reallocation and every later read of a_w.spot
        // was freed memory. The struct is a string and two ints, and there are
        // never more than a handful of open bags; the copy costs nothing and
        // the alternative is a reference whose lifetime the body has to keep
        // reasoning about.
        bool DrawOneShelfBag(RE::TESObjectREFR* p, ContLayout& a_cl,
                             ShelfBagWin a_w, int a_ord)
        {
        const auto si = a_cl.cells.find(a_w.spot);
        if (si == a_cl.cells.end()) return false;   // the bag left the shelf
        auto& bundle = si->second.bundle;
        // ★which bag this window is showing
        const std::uint32_t root = a_w.bag;
        if (!BagAlive(bundle, root)) return false;   // that bag is no longer here
        auto* bagObj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_w.form);
        if (!bagObj) return false;
        const auto bagDef = Grid::ResolveDef(bagObj);
        const int cols = (std::max)(1, bagDef.bw);
        int rows = (std::max)(1, bagDef.bh);

        // ---- seat the bundle: saved anchors first, first-fit for the rest.
        // Fresh fits are WRITTEN BACK so the layout survives the session
        // (cosave v5/v6) exactly like a player bag's. A carry lifted from
        // THIS window is excluded entirely -- its cells free up while it
        // rides the cursor, and a cancel simply re-seats it (colliding
        // anchors refit, nothing is lost).
        // ★★THE CARRIED ENTRY IS THE ONE WITH THE CARRY'S NAME. It used to be
        // matched on (level, form, square) -- three things that all change --
        // so a lift that renumbered anything left the entry drawing in its old
        // square while also riding the cursor. Two copies of one thing.
        const bool carryOut = g_bundleCarry.active &&
                              g_bundleCarry.spot == a_w.spot &&
                              g_bundleCarry.cont == p->GetFormID();
        auto isCarried = [&](const BundleItem& a_b) {
            return carryOut && a_b.id == g_bundleCarry.id;
        };
        auto dimsOf = [&](const BundleItem& a_b, RE::TESBoundObject* a_obj,
                          int& a_w, int& a_h) {
            const auto d = Grid::ResolveDef(a_obj);
            a_w = (std::max)(1, d.w);
            a_h = (std::max)(1, d.h);
            if (a_b.rot & 1) std::swap(a_w, a_h);
        };
        constexpr int kGrowRows = 16;
        const int maxRows = rows + kGrowRows;
        std::vector<char> occ(static_cast<std::size_t>(cols) * maxRows, 0);
        auto fits = [&](int a_c, int a_r, int a_w, int a_h) {
            if (a_c < 0 || a_r < 0 || a_c + a_w > cols || a_r + a_h > maxRows) return false;
            for (int y = 0; y < a_h; ++y)
                for (int x = 0; x < a_w; ++x)
                    if (occ[static_cast<std::size_t>(a_r + y) * cols + a_c + x]) return false;
            return true;
        };
        auto mark = [&](int a_c, int a_r, int a_w, int a_h, char a_v) {
            for (int y = 0; y < a_h; ++y)
                for (int x = 0; x < a_w; ++x)
                    occ[static_cast<std::size_t>(a_r + y) * cols + a_c + x] = a_v;
        };
        struct Seat
        {
            int                 idx;
            int                 col, row, w, h, rot;
            RE::TESBoundObject* obj;
        };
        std::vector<Seat> seats;
        // ★★S-3-4: ONLY WHAT SITS DIRECTLY IN THIS BAG. A bundle is a tree
        // now -- a nested bag's own contents ride in the same vector, pointing
        // at their parent -- so seating every entry would spill a whole branch
        // into the window of the bag that merely contains it.
        //
        // The nested bag itself IS seated: it is an entry at this level, and it
        // draws as one cell. Opening it from inside a chest is deliberately not
        // offered yet; the requirement was that a nesting survives the trip
        // intact, and it does.
        auto atThisLevel = [&](const BundleItem& a_b) { return a_b.parent == root; };
        for (int i = 0; i < static_cast<int>(bundle.size()); ++i) {
            if (isCarried(bundle[i]) || !atThisLevel(bundle[i])) continue;
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(bundle[i].form);
            if (!obj) continue;
            int w = 1, h = 1;
            dimsOf(bundle[i], obj, w, h);
            if (bundle[i].col >= 0 && fits(bundle[i].col, bundle[i].row, w, h)) {
                mark(bundle[i].col, bundle[i].row, w, h, 1);
                seats.push_back({ i, bundle[i].col, bundle[i].row, w, h,
                                  bundle[i].rot, obj });
            } else {
                bundle[i].col = bundle[i].row = -1;   // stale anchor: refit below
            }
        }
        for (int i = 0; i < static_cast<int>(bundle.size()); ++i) {
            if (bundle[i].col >= 0 || isCarried(bundle[i]) ||
                !atThisLevel(bundle[i])) continue;
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(bundle[i].form);
            if (!obj) continue;
            int w = 1, h = 1;
            dimsOf(bundle[i], obj, w, h);
            for (int r = 0; r < maxRows && bundle[i].col < 0; ++r) {
                for (int c = 0; c < cols; ++c) {
                    if (!fits(c, r, w, h)) continue;
                    mark(c, r, w, h, 1);
                    bundle[i].col = c;
                    bundle[i].row = r;
                    seats.push_back({ i, c, r, w, h, bundle[i].rot, obj });
                    break;
                }
            }
        }
        for (const auto& s : seats) rows = (std::max)(rows, s.row + s.h);

        // ---- the window: the player bag-window chrome, one for one --------
        auto* wm = WinManager::GetSingleton();
        const float S = Theme::Scale();
        const float cell = Grid::CellPx();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float topPad = Theme::TitleTopPad();
        const ImVec2 size(cols * cell + 2.0f * Theme::PadX() * S +
                              2.0f * Theme::FrameInsetX(),
                          rows * cell + 54.0f * S + 2.0f * Theme::FrameInsetY());
        // ★★THE WINDOW IS NAMED AFTER THE BAG ITEM ITSELF -- the same key the
        // player board's own bag window uses (a single-copy bag's tile key is
        // its bare form key), so the position follows the bag between the
        // inventory and any shelf, and between shelves. It was keyed by the
        // cell's bundle id before, and that id is MINTED FRESH on every store:
        // ImGui filed each re-shelving as a window it had never seen and
        // dropped it at the default spot (user report). Two same-form bags
        // open at once now share one name -- a rarity we accept for the
        // position stability every single-copy bag gets.
        // (bagId still mints here: the cell-follow machinery reads it.)
        if (a_w.bag == 0 && si->second.bagId == 0) si->second.bagId = NextBundleId();
        const std::string wid = Grid::DefKeyOf(bagObj);
        wm->ApplyNext(wid,
            ImVec2(disp.x * 0.52f + a_ord * 44.0f * S,
                   (disp.y - size.y) * 0.5f + a_ord * 36.0f * S),
            size, WinManager::Anchor::kTopLeft, topPad);
        ImGui::Begin(("##sb_" + wid).c_str(), nullptr, kManagedWinFlags);
        // ★★(1.3.2) NO NoteOverlayRect HERE. This is a BAG window, not a
        // popup -- and registering it made the window block its own cells:
        // every lift is gated on `IsItemHovered() && !MouseInOverlay()`, and
        // the mouse is always inside the rect the window itself just
        // registered. Dropping IN still worked (that path asks
        // IsWindowHovered, which knows nothing about overlays), so the
        // symptom was exactly "things go into the bag but cannot come out".
        // The player's own bag windows register nothing for the same reason;
        // z-order already keeps clicks from falling through to the boards.
        // ★★(1.3.2) the COLLECT button's width has to be RESERVED on the
        // title bar before it is drawn: TitleBar lays a full-width drag
        // strip across the row, and an unreserved button sits UNDER it --
        // present, correct, and unclickable (the player's own bag windows
        // pass the same reservation for the same reason).
        const char* colLbl = Lang::T(Lang::Str::BagCollect);
        const bool  hasCollect = !bagDef.accept.empty();
        const float colW = hasCollect
                               ? ImGui::CalcTextSize(colLbl).x + 16.0f * S
                               : 0.0f;
        wm->TitleBar(wid,
            bagObj->GetName() && *bagObj->GetName() ? bagObj->GetName() : "?",
            colW);

        // ★(1.3.2b) typed bags carry their COLLECT on the shelf too: sweep
        // this container's matching LOOSE items into the bag. The swept
        // cells vanish (the bundle hides them) and their spots die in the
        // ordinary dead-pool prune next frame.
        if (hasCollect && !Grid::IsHolding()) {
            const ImVec2 keep = ImGui::GetCursorScreenPos();
            const float lineH = ImGui::GetTextLineHeight();
            const float btnH = lineH + 6.0f * S;
            const float textTop = WinManager::TitleTextY(wid, lineH);
            ImGui::SetCursorScreenPos(ImVec2(
                ImGui::GetWindowPos().x + size.x - colW - Theme::TopControlRightPad(),
                textTop - (btnH - lineH) * 0.5f));
            const auto& sk = Theme::S();
            if (Sfx::Button(("##sbcollect_" + wid).c_str(), ImVec2(colW, btnH))) {
                // already-bundled counts across THIS container hide cells;
                // collect only what is still loose
                std::map<RE::FormID, int> taken;
                for (const auto& [k2, s2] : a_cl.cells) {
                    for (const auto& b2 : s2.bundle) taken[b2.form] += b2.count;
                }
                int swept = 0;
                if (auto* src = SourceRef()) {
                    for (auto& [obj2, data2] : src->GetInventory()) {
                        const int total = data2.first;
                        if (!obj2 || total <= 0 || obj2->IsGold()) continue;
                        const RE::FormID fid2 = obj2->GetFormID();
                        if (GoldCoins::IsCoinForm(fid2)) continue;   // incl. pouch
                        if (Grid::ResolveDef(obj2).bag != 0) continue;
                        const char* nm2 = obj2->GetName();
                        if (!nm2 || !*nm2 || !obj2->GetPlayable()) continue;
                        if (BagFilter::FilterOf(obj2) != bagDef.accept) continue;
                        const int loose = total - taken[fid2];
                        if (loose <= 0) continue;
                        AddBundle(bundle, { fid2, loose, 0, -1, -1, 0,
                                            0, false, root });
                        taken[fid2] += loose;
                        swept += loose;
                    }
                }
                if (swept > 0) Sfx::BagOpen();
            }
            const bool hovC = ImGui::IsItemHovered();
            const ImVec2 bp = ImGui::GetItemRectMin();
            const ImVec2 bs = ImGui::GetItemRectSize();
            const ImVec2 ts = ImGui::CalcTextSize(colLbl);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(bp.x + (bs.x - ts.x) * 0.5f,
                       bp.y + (bs.y - ts.y) * 0.5f),
                hovC ? Theme::Val() : ImGui::GetColorU32(sk.inkDim), colLbl);
            ImGui::SetCursorScreenPos(keep);
        }

        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 winPos = ImGui::GetWindowPos();
        const ImVec2 base(winPos.x + Theme::PadX() * S + Theme::FrameInsetX(),
                          winPos.y + 40.0f * S + Theme::FrameInsetY() * 0.5f);

        // ★★RESERVE THE GRID AREA FIRST. SetCursorScreenPos alone does not
        // grow the window's content bounds, and ImGui 1.92 raises its error
        // overlay for every item button placed beyond them -- that overlay
        // then swallowed hover AND clicks, which read back as "the window
        // closes the moment an item is clicked" and "nothing can be dropped
        // in". One Dummy over the whole grid makes every later position a
        // known-inside one.
        ImGui::SetCursorScreenPos(base);
        ImGui::Dummy(ImVec2(cols * cell, rows * cell));

        // chrome, ground, then the lattice -- the partner board's order
        Grid::DrawCellLattice(dl, base, cols, rows);
        const ImU32 shade = Theme::OccupiedGround();
        // ★★PER CELL, WITH THE LATTICE'S OWN INSETS. This drew ONE rectangle
        // over the whole footprint with a flat 1px margin, which is not what
        // the player's board does and not what the grid underneath looks like:
        // the inset between two cells is half a groove, the inset at the board
        // edge is none, and both scale. A two-cell item therefore painted over
        // its own internal groove and sat a hair off the lattice everywhere
        // else -- "the item background does not quite line up with the grid".
        // The rule belongs to the grid, so it is asked for rather than guessed.
        for (const auto& s : seats) {
            for (int gr = 0; gr < s.h; ++gr) {
                for (int gc = 0; gc < s.w; ++gc) {
                    Grid::ShadeCell(dl, base, s.col + gc, s.row + gr,
                                    cols, rows, shade);
                }
            }
        }
        Grid::DrawInkLattice(dl, base, cols, rows);

        // a right-click take, applied AFTER the loop below has finished
        // drawing -- see the note at the click
        std::uint32_t       takeId = 0;
        RE::TESBoundObject* takeObj = nullptr;
        int                 takeCount = 0;
        std::uint16_t       takeSig = 0;

        auto* cache = IconCache::GetSingleton();
        for (const auto& s : seats) {
            const auto& b = bundle[s.idx];
            const ImVec2 p0(base.x + s.col * cell, base.y + s.row * cell);
            const float bw = s.w * cell, bh = s.h * cell;
            // ★(1.5.x) a bundled POUCH draws its own band, same as a shelf
            // pouch cell -- the bare form's art reads "empty" whatever it holds
            RE::TESBoundObject* iconObj = s.obj;
            if (b.gold > 0) {
                if (auto* v = GoldCoins::PouchIconObjectFor(b.gold,
                        GoldCoins::PouchCapOfForm(b.form), b.form)) {
                    iconObj = v;
                }
            }
            const IconCache::Icon* icon = cache->Get(iconObj);
            if (!icon) {
                cache->QueueCapture(iconObj);
                icon = Fallback::Get(iconObj);
            }
            if (icon && icon->srv) {
                // contain-fit inside the UPRIGHT box (the sprite is not
                // rotated, only its draw quad -- GI62), the one icon path
                // (gain/blend) + the same shadow as every board
                const float iw = static_cast<float>(icon->w);
                const float ih = static_cast<float>(icon->h);
                const float upW = ((s.rot & 1) ? s.h : s.w) * cell;
                const float upH = ((s.rot & 1) ? s.w : s.h) * cell;
                const float k = (std::min)(upW / (std::max)(1.0f, iw),
                                           upH / (std::max)(1.0f, ih)) * 0.92f;
                const float dw = iw * k, dh = ih * k;
                const float deg = s.rot * 90.0f;
                const ImVec2 ctr(p0.x + bw * 0.5f, p0.y + bh * 0.5f);
                // ★(1.3.2) the rarity halo, UNDER the sprite -- the same pass
                // and the same call both boards make. Its absence was the
                // whole "no markers inside a stored bag" report.
                Grid::DrawGlow(dl, s.obj, b.glow,
                    ImVec2(ctr.x - dw * 0.5f, ctr.y - dh * 0.5f),
                    ImVec2(ctr.x + dw * 0.5f, ctr.y + dh * 0.5f),
                    p0, ImVec2(p0.x + bw, p0.y + bh), s.rot);
                Grid::DrawItemShadow(dl, icon->srv, ctr, dw, dh, deg);
                UIRoot::DrawItemIconRot(dl, icon->srv, ctr, ImVec2(dw, dh), deg);
            }
            // ★(1.3.2) the marker tray: poison, and "stolen" when the whole
            // container is someone else's. No star -- a stored unit is not
            // the player's favourite any more (the store strips it).
            // ★b.stolen ALONE -- what this unit actually carries, not what
            // container it happens to be sitting in. See the note on the
            // partner board's tray for why the mode is no longer part of it.
            Grid::DrawMarkerTray(dl, p0, ImVec2(p0.x + bw, p0.y + bh),
                                 false, b.stolen,
                                 (b.glow & 0x4) != 0, false);
            {   // GI8: extension overlay (socket wells), same as every board
                Badges::TileShape shape;
                shape.w = s.w;
                shape.h = s.h;
                const bool hovB = UIRoot::CursorOwnsWindow() &&
                                  ImGui::IsMouseHoveringRect(
                                      p0, ImVec2(p0.x + bw, p0.y + bh), false);
                Badges::Draw(dl, p0, bw, bh, shape, p->GetFormID(),
                             b.form, 0, hovB);
            }
            if (b.count > 1) {
                char cnt[16];
                std::snprintf(cnt, sizeof(cnt), "%d", b.count);
                Grid::DrawCountBadge(dl, p0, cnt);
            }
            // ★(1.5.x) while a carry rides, record the pouch entry under the
            // cursor -- the coin drop routes deposit through this (the bag
            // window has no cells for QueryStoreDrop to see)
            if (Grid::IsHolding() && GoldCoins::IsPouch(b.form) &&
                ImGui::IsWindowHovered(
                    ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + bw, p0.y + bh),
                                           false)) {
                g_hoverBundlePouch = { p->GetFormID(), a_w.spot, b.id };
            }
            if (!Grid::IsHolding()) {
                char idbuf[24];
                std::snprintf(idbuf, sizeof(idbuf), "##sbc%u", b.id);
                ImGui::SetCursorScreenPos(p0);
                ImGui::InvisibleButton(idbuf, ImVec2(bw, bh));
                if (ImGui::IsItemHovered() && !UIRoot::MouseInOverlay()) {
                    dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                        Theme::Acc(0.10f));
                    // ★(1.5.x) a bundled pouch's tooltip prints its amount --
                    // the same "N / cap G" line the shelf pouch cell shows
                    const int tipGold = GoldCoins::IsPouch(b.form)
                                            ? (std::max)(0, b.gold) : -1;
                    Grid::DrawItemTooltip(s.obj, b.count, tipGold, -1, false,
                                          SourceRef(), Grid::ExtraScope::kAny,
                                          0, -1, 0, 0,
                                          Grid::TileContext{ {}, false, false, true, false });
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        // lift it: the bundle keeps the entry until the carry
                        // is CONSUMED (take home / shelf drop); a cancel puts
                        // it visually straight back
                        g_bundleCarry = { true, p->GetFormID(), a_w.spot,
                                          b.form, b.sig, s.col, s.row, b.id };
                        g_carryGlow = b.glow;   // (1.3.2)
                        g_carryStolen = b.stolen;
                        Grid::BeginPartnerCarry(s.obj, b.count, 0,
                                                -1.0f, -1.0f, 0, -1, 0, s.rot);
                    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) &&
                               GoldCoins::IsPouch(b.form)) {
                        // ★(1.5.x) a bundled POUCH manages on right-click,
                        // exactly like a shelf pouch cell: the withdraw window
                        // opens over THIS entry's amount (never a take -- the
                        // pouch leaves the bag by drag, like a bag does)
                        if (g_shelfPouchSpot == a_w.spot &&
                            g_shelfPouchBundle == b.id) {
                            g_shelfPouchSpot.clear();
                            g_shelfPouchBundle = 0;
                            Sfx::SelectOff();
                        } else {
                            g_shelfPouchSpot = a_w.spot;
                            g_shelfPouchBundle = b.id;
                            g_shelfPouchForm = b.form;
                            g_shelfPouchSlider =
                                (std::max)(1, (std::max)(0, b.gold) / 2);
                            Sfx::SelectOn();
                        }
                    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) &&
                               Grid::ResolveDef(s.obj).bag != 0) {
                        // ★★AND A BAG OPENS, which is what right-click on a bag
                        // means everywhere else. Its window is this one's path
                        // plus a step, so a bag three deep is three steps -- the
                        // nesting has no special case and no depth limit.
                        const std::uint32_t sub = b.id;
                        const auto open = std::find_if(g_shelfBags.begin(),
                            g_shelfBags.end(), [&](const ShelfBagWin& a_o) {
                                return a_o.spot == a_w.spot && a_o.bag == sub;
                            });
                        if (open != g_shelfBags.end()) {
                            g_shelfBags.erase(open);
                            Sfx::SelectOff();
                        } else {
                            g_shelfBags.push_back({ a_w.spot, b.form, sub });
                            Sfx::SelectOn();
                        }
                    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) &&
                               Grid::ResolveDef(s.obj).bag == 0) {
                        // ★★RIGHT-CLICK TAKES IT, like every other cell on this
                        // side of the window. Only the lift was ever wired up
                        // here, so an item inside a shelved bag was the one
                        // thing in a container that could not be taken the
                        // ordinary way -- it had to be dragged out by hand.
                        //
                        // The units are already IN the container; the bundle
                        // only hides them. So dropping the entry is what makes
                        // them the shelf's again, and the take is then the same
                        // request any visible cell would make. No acting cell:
                        // this take retires no cell, because the bundle entry
                        // was never one.
                        //
                        // ★★A BAG IS EXCLUDED, and that is this board's rule
                        // rather than an exception to it: right-click on a bag
                        // is ALWAYS the window toggle, and a bag changes
                        // containers by DRAG only. The shelf's own top-level
                        // bags have worked that way since 1.3.1; letting a
                        // nested one be taken by right-click made this the one
                        // place the rule did not hold. Dragging it out still
                        // brings its whole branch.
                        // ★(1.5.x) gold is exempt from the room check, the
                        // partner board's own rule -- it merges into the
                        // ledger and needs no square of its own
                        if (s.obj->IsGold() || Grid::CanFitNewItem(s.obj)) {
                            // ★NOTED, NOT DONE. This loop draws as well as
                            // listens, and mutating the bundle inside it meant
                            // leaving early -- which skipped drawing every seat
                            // after the clicked one for that frame. They came
                            // back the next, which is exactly the "some cells
                            // blink when I take another item" report, and why it
                            // looked like a property of the CELL rather than of
                            // the item: it was the ones later in seat order.
                            takeId = b.id;
                            takeObj = s.obj;
                            takeCount = b.count;
                            takeSig = b.sig;
                        } else {
                            Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                        }
                    }
                }
            }
        }

        // ★The deferred right-click take. Every seat has been drawn by now, so
        // the bundle may safely move under `seats`: nothing reads it again this
        // frame, and the next pass re-seats from the shortened list.
        //
        // The units are already IN the container -- the bundle only hides them
        // -- so dropping the entry is what makes them the shelf's again, and
        // the take is then the same request any visible cell would make. No
        // acting cell is named: this take retires no cell, because the bundle
        // entry never was one.
        if (takeId != 0 && takeObj) {
            std::erase_if(bundle,
                [&](const BundleItem& b) { return b.id == takeId; });
            g_actingSpot.clear();
            RequestTake(takeObj, takeCount, 0, takeSig);
        }

        // ★(1.3.2) drop ghost, the same one both boards draw: green = the
        // footprint seats here, red = it does not (a single blocker still
        // swaps, exactly as on the partner board, and reads red there too).
        // Its anchor is the DROP's anchor verbatim, so preview and result
        // cannot disagree.
        if (Grid::IsHolding() &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            int hw = 1, hh = 1;
            float gox = 0.0f, goy = 0.0f;
            if (Grid::HeldFootprint(hw, hh, gox, goy)) {
                const ImVec2 gm = ImGui::GetIO().MousePos;
                const int gc = static_cast<int>(std::floor(
                    (gm.x - base.x) / cell - (hw - 1) * 0.5f));
                const int gr = static_cast<int>(std::floor(
                    (gm.y - base.y) / cell - (hh - 1) * 0.5f));
                if (gc >= 0 && gr >= 0 && gc + hw <= cols && gr + hh <= rows) {
                    const bool ok = fits(gc, gr, hw, hh);
                    const ImU32 ghost = ok ? IM_COL32(90, 170, 90, 90)
                                           : IM_COL32(190, 60, 60, 110);
                    const ImVec2 g0(base.x + gc * cell, base.y + gr * cell);
                    dl->AddRectFilled(g0,
                        ImVec2(g0.x + hw * cell, g0.y + hh * cell), ghost);
                    // ★(1.5.x) the square, for the coin routes (player gold
                    // stored INTO this bag lands here). A TYPED bag records
                    // nothing: the coin route would bypass its filter, and
                    // gold does not answer to any collect type.
                    if (bagDef.accept.empty()) {
                        g_hoverBundleSq = { p->GetFormID(), a_w.spot, root,
                                            gc, gr, ok };
                    }
                }
            }
        }

        // ---- drops INTO the window ---------------------------------------
        // The full player-bag grammar: a free rect seats it, ONE blocker is
        // the C4 swap (incoming takes the occupant's anchor, the occupant
        // rides the cursor), more keeps carrying. Sources: a rearrange from
        // this bag, a PLAYER item (engine store rides along), or a plain
        // SHELF cell (already in the container -- its slot retires and the
        // entry simply hides it).
        if (Grid::IsHolding() &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            RE::TESBoundObject* hobj = nullptr;
            int hcount = 0, hrot = 0;
            std::uint16_t hsig = 0;
            bool fromPartner = false;
            const std::uint8_t hglow = g_carryGlow;   // (1.3.2) markers in transit
            if (Grid::PeekHeldForShelf(hobj, hcount, hsig, hrot, fromPartner)) {
                int hw = 1, hh = 1;
                float hox = 0.0f, hoy = 0.0f;
                Grid::HeldFootprint(hw, hh, hox, hoy);   // already-rotated dims
                const ImVec2 m = ImGui::GetIO().MousePos;
                // the carry centres on the cursor: centre the footprint too
                const int c = static_cast<int>(std::floor(
                    (m.x - base.x) / cell - (hw - 1) * 0.5f));
                const int r = static_cast<int>(std::floor(
                    (m.y - base.y) / cell - (hh - 1) * 0.5f));

                const bool bundleRe = g_bundleCarry.active;
                const bool sameBag = bundleRe &&
                                     g_bundleCarry.spot == a_w.spot &&
                                     g_bundleCarry.cont == p->GetFormID();
                const RE::FormID hfid = hobj->GetFormID();
                // eligibility: a rearrange is already inside; a player item
                // answers to HeldShelfStorable. Typed bags keep their filter
                // -- a move from ANOTHER bag answers to it too.
                // ★★A BAG MAY GO IN, which it could not while a bundle was one
                // flat list -- "no nesting" was a statement about the data, not
                // a rule anyone chose. The data is a tree now.
                // ★(1.5.x) AND A POUCH MAY GO IN: BundleItem::gold holds its
                // amount now (the cell's gold rides along below). Plain coins
                // stay out -- a coin cell has no entry grammar in a bag.
                const bool intakeOk =
                    bundleRe ||
                    (fromPartner ? (!GoldCoins::IsCoinForm(hfid) ||
                                    GoldCoins::IsPouch(hfid))
                                 : Grid::HeldShelfStorable() != nullptr);
                const bool filterOk = sameBag || bagDef.accept.empty() ||
                                      BagFilter::FilterOf(hobj) == bagDef.accept;
                // ★★AND NOT INTO ITSELF. A bag can be lifted out of one window
                // and dropped into another, and one of those windows may be its
                // own -- or one belonging to something inside it. A flat index
                // could not be asked that question, so the loop it would have
                // made was simply unreachable data: a branch parented to itself,
                // visible nowhere and freed by nothing.
                const bool loopOk =
                    !(bundleRe && root != 0 &&
                      Ancestry(bundle, root).count(g_bundleCarry.id) != 0) &&
                    // ★★AND A SHELF BAG IS NOT DROPPED INTO ITSELF. The cell
                    // being carried is still on the board, so its window is
                    // still open, and the intake would retire that very cell --
                    // the same cell whose bundle it is writing into. Not a
                    // logical loop but a live reference to a map node being
                    // erased under it.
                    !(fromPartner && !bundleRe && g_actingSpot == a_w.spot);
                // ★(1.5.x) GOLD OVER A BUNDLED POUCH IS A DEPOSIT, never an
                // intake -- letting the generic grammar have it was the
                // measured swap (the gold seated in the bag, the pouch rode
                // the cursor). A shelf gold CELL is left unconsumed here so
                // the drop routes (DropPartnerHeld) run the deposit; a
                // carried gold ENTRY deposits right here, entry to entry.
                const bool goldOnPouch = hobj->IsGold() &&
                                         g_hoverBundlePouch.id != 0;
                if (goldOnPouch && bundleRe) {
                    bool done = false;
                    const auto pci = g_contLayouts.find(g_hoverBundlePouch.cont);
                    if (pci != g_contLayouts.end() &&
                        pci->second.cells.contains(g_hoverBundlePouch.spot)) {
                        auto& psc = pci->second.cells.at(g_hoverBundlePouch.spot);
                        const auto pe = std::find_if(psc.bundle.begin(),
                            psc.bundle.end(), [&](const BundleItem& b2) {
                                return b2.id == g_hoverBundlePouch.id;
                            });
                        const auto sci = g_contLayouts.find(g_bundleCarry.cont);
                        if (pe != psc.bundle.end() &&
                            GoldCoins::IsPouch(pe->form) &&
                            sci != g_contLayouts.end()) {
                            const auto ssi =
                                sci->second.cells.find(g_bundleCarry.spot);
                            if (ssi != sci->second.cells.end()) {
                                auto& srcB = ssi->second.bundle;
                                const auto ge = std::find_if(srcB.begin(),
                                    srcB.end(), [&](const BundleItem& b2) {
                                        return b2.id == g_bundleCarry.id;
                                    });
                                if (ge != srcB.end()) {
                                    const int have = (std::max)(0, pe->gold);
                                    const int room =
                                        GoldCoins::PouchCapOfForm(pe->form) -
                                        have;
                                    const int moved =
                                        (std::min)(ge->count, room);
                                    if (moved > 0) {
                                        // book first, THEN the erase that may
                                        // invalidate pe (same vector)
                                        pe->gold = have + moved;
                                        ge->count -= moved;
                                        if (ge->count <= 0) srcB.erase(ge);
                                        // physical Septims become the pouch's
                                        // virtual gold -- the container stack
                                        // falls by the same amount (the cell
                                        // deposit's own lifecycle)
                                        if (auto* src2 = SourceRef()) {
                                            NoteOut(hobj, 0, 0, moved);
                                            const RE::FormID srcId2 =
                                                src2->GetFormID();
                                            const RE::FormID goldId2 =
                                                hobj->GetFormID();
                                            SKSE::GetTaskInterface()->AddTask(
                                                [srcId2, goldId2, moved]() {
                                                auto* sr = RE::TESForm::LookupByID<
                                                    RE::TESObjectREFR>(srcId2);
                                                auto* go = RE::TESForm::LookupByID<
                                                    RE::TESBoundObject>(goldId2);
                                                if (sr && go) {
                                                    sr->RemoveItem(go, moved,
                                                        RE::ITEM_REMOVE_REASON::kRemove,
                                                        nullptr, nullptr);
                                                }
                                                if (go) ClearOut(go, 0, 0, moved);
                                            });
                                        }
                                        SKSE::log::info(
                                            "[LOOT] bundled gold -> bundled "
                                            "pouch: {} G", moved);
                                        g_bundleCarry.active = false;
                                        Grid::DropHeldForShelf();
                                        done = true;
                                    }
                                }
                            }
                        }
                    }
                    if (!done) Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                } else if (!goldOnPouch && intakeOk && filterOk && loopOk) {
                    // seats blocking the drop rect
                    std::vector<int> blocks;
                    for (int sn = 0; sn < static_cast<int>(seats.size()); ++sn) {
                        const auto& s = seats[sn];
                        const bool hit = !(c + hw <= s.col || s.col + s.w <= c ||
                                           r + hh <= s.row || s.row + s.h <= r);
                        if (hit) blocks.push_back(sn);
                    }
                    int  dropC = c, dropR = r;
                    int  liftIdx = -1;
                    bool ok = false;
                    if (fits(c, r, hw, hh)) {
                        ok = true;
                    } else if (blocks.size() == 1) {
                        const auto& bs = seats[blocks[0]];
                        mark(bs.col, bs.row, bs.w, bs.h, 0);   // judge with it freed
                        if (fits(bs.col, bs.row, hw, hh)) {
                            dropC = bs.col;
                            dropR = bs.row;
                            liftIdx = blocks[0];
                            ok = true;
                        }
                    }
                    if (ok) {
                        // the displaced occupant, remembered BEFORE the bundle
                        // mutates under it
                        BundleItem lifted{};
                        RE::TESBoundObject* liftedObj = nullptr;
                        if (liftIdx >= 0) {
                            lifted = bundle[seats[liftIdx].idx];
                            liftedObj = seats[liftIdx].obj;
                        }
                        bool consumed = false;
                        if (bundleRe) {
                            // (1.3.2c) pull the entry out of the bundle it was
                            // lifted from -- which may be ANOTHER open bag --
                            // and seat it here
                            if (const auto sci = g_contLayouts.find(g_bundleCarry.cont);
                                sci != g_contLayouts.end()) {
                                if (const auto ssi =
                                        sci->second.cells.find(g_bundleCarry.spot);
                                    ssi != sci->second.cells.end()) {
                                    auto& srcB = ssi->second.bundle;
                                    const auto bit = std::find_if(srcB.begin(),
                                        srcB.end(), [&](const BundleItem& b) {
                                            return b.id == g_bundleCarry.id;
                                        });
                                    if (bit != srcB.end()) {
                                        // ★★★NOTHING IS RENUMBERED, EVER.
                                        // Where an entry sits and which bag it
                                        // is in are two fields; its NAME is a
                                        // third, and a move writes only the
                                        // first two.
                                        //
                                        // This is where the tree used to come
                                        // apart. Erase-and-append reordered the
                                        // vector while every parent in it was
                                        // an INDEX into that vector, so putting
                                        // one item down renumbered everything
                                        // and every reference kept the old
                                        // numbers -- measured: moving the item
                                        // in bag A sent it from #0 to the end
                                        // and pulled the rest down one, leaving
                                        // bag B's children claiming an index
                                        // that was now their uncle. B's window
                                        // stayed open and seated nothing: "the
                                        // contents disappeared." Then in-place
                                        // moves patched that, and the open
                                        // windows had to be patched too because
                                        // a window was named by the SQUARE the
                                        // bag sat in. None of that survives a
                                        // name.
                                        if (&srcB == &bundle) {
                                            bit->col = dropC;
                                            bit->row = dropR;
                                            bit->rot = hrot & 3;
                                            bit->parent = root;
                                        } else {
                                            // ★A genuine crossing between two
                                            // cells' bundles: the entry and its
                                            // whole branch change vectors, ids
                                            // intact. The mint is global, so
                                            // there is nothing on this side to
                                            // collide with and nothing to remap.
                                            BundleItem moved = *bit;
                                            auto branch = CutBranch(srcB, moved.id);
                                            moved.col = dropC;
                                            moved.row = dropR;
                                            moved.rot = hrot & 3;
                                            moved.parent = root;
                                            bundle.push_back(moved);
                                            for (auto& b : branch) {
                                                if (b.parent == 0) b.parent = moved.id;
                                                bundle.push_back(b);
                                            }
                                            // ★the branch changed CELLS, so any
                                            // window open on it hangs off this
                                            // cell now. Its name is unchanged;
                                            // only the spot it lives on moved.
                                            const auto fam =
                                                IdClosure(bundle, moved.id);
                                            for (auto& w : g_shelfBags) {
                                                if (w.spot == g_bundleCarry.spot &&
                                                    fam.count(w.bag) != 0) {
                                                    w.spot = a_w.spot;
                                                }
                                            }
                                        }
                                        consumed = true;
                                    }
                                }
                            }
                            g_bundleCarry.active = false;
                            Grid::DropHeldForShelf();
                        } else if (fromPartner && hobj->IsGold() &&
                                   liftIdx >= 0 &&
                                   bundle[seats[liftIdx].idx].form == hfid) {
                            // ★(1.5.x) GOLD MERGES INTO GOLD, stack grammar --
                            // a swap of two money stacks is never the gesture
                            bundle[seats[liftIdx].idx].count += hcount;
                            const std::string from = g_actingSpot;
                            if (!from.empty()) {
                                a_cl.cells.erase(from);
                                g_actingSpot.clear();
                            }
                            liftIdx = -1;   // merged: nothing is displaced
                            consumed = true;
                            Grid::DropHeldForShelf();
                        } else if (fromPartner) {
                            // off the shelf, into the bag: retire its slot --
                            // the engine item never moves, the entry hides it
                            // ★★AND A SHELF BAG BRINGS ITS OWN INSIDES. The
                            // cell's bundle IS what is in that bag, and retiring
                            // the cell used to drop it on the floor: bag B put
                            // back into bag A arrived empty while everything
                            // that had been in B surfaced loose on the shelf.
                            // It becomes B's branch here, every name intact.
                            std::vector<BundleItem> carried;
                            std::uint32_t cellName = 0;
                            int cellGold = -1;    // ★(1.5.x) a pouch cell's book
                            int cellAwait = 0;    // ...and its unclaimed grace
                            const std::string from = g_actingSpot;
                            if (!from.empty()) {
                                if (const auto ai = a_cl.cells.find(from);
                                    ai != a_cl.cells.end()) {
                                    carried = std::move(ai->second.bundle);
                                    cellName = ai->second.bagId;
                                    if (GoldCoins::IsPouch(ai->second.form)) {
                                        cellGold =
                                            (std::max)(0, ai->second.gold);
                                        cellAwait = ai->second.awaitGold;
                                    }
                                }
                                a_cl.cells.erase(from);
                                g_actingSpot.clear();
                            }
                            // off a shelf cell: the container's own ownership
                            // is all this side knows (kSteal = owned whole)
                            // ★the entry keeps the name the CELL had, when the
                            // cell had one -- the mirror of the move out, and
                            // the other half of "one bag, one name"
                            // ★What the carried unit actually is. Storing into
                            // an owned container does not make a clean item
                            // stolen -- measured, no ownership is written --
                            // so the bundle must not record one either.
                            BundleItem ni{ hfid, hcount, hsig,
                                           dropC, dropR, hrot & 3, hglow,
                                           g_carryStolen,
                                           root };
                            ni.id = cellName != 0 ? cellName : NextBundleId();
                            // ★(1.5.x) a pouch cell's gold becomes the
                            // entry's -- the amount changes books, not owners.
                            // A cell still waiting on its parcel hands the
                            // wait over too.
                            ni.gold = cellGold;
                            ni.awaitGold = cellAwait;
                            const std::uint32_t nid = ni.id;
                            bundle.push_back(std::move(ni));
                            for (auto& b : carried) {
                                if (b.parent == 0) b.parent = nid;
                                bundle.push_back(b);
                            }
                            // ★and the windows that hung off that cell hang off
                            // this one now -- the mirror of the move out
                            if (!from.empty()) {
                                for (auto& w : g_shelfBags) {
                                    if (w.spot != from) continue;
                                    if (w.bag == 0) {
                                        w.spot = a_w.spot;
                                        w.bag = nid;
                                    } else if (std::any_of(carried.begin(),
                                                   carried.end(),
                                                   [&](const BundleItem& b) {
                                                       return b.id == w.bag;
                                                   })) {
                                        w.spot = a_w.spot;
                                    }
                                }
                            }
                            consumed = true;
                            Grid::DropHeldForShelf();
                        } else {
                            RE::FormID    f = 0;
                            int           cnt = 0;
                            std::uint16_t sg = 0;
                            int           rt = 0;
                            std::uint8_t  gl = 0;
                            bool          st = false;
                            const bool heldBag = Grid::ResolveDef(hobj).bag != 0;
                            if (Grid::CommitHeldToShelfBag(f, cnt, sg, rt, gl, st)) {
                                auto& nb =
                                    AddBundle(bundle, { f, cnt, sg, dropC, dropR,
                                                        rt, gl, st, root });
                                // ★(1.5.x) a player POUCH dropped in: its gold
                                // walks separately (the sink parcels it when
                                // the engine item transfers) -- arm the claim
                                if (GoldCoins::IsPouch(f)) nb.awaitGold = 8;
                                const std::uint32_t nid = nb.id;
                                // ★★AND A BAG BRINGS ITS INSIDES. The commit
                                // queues the contents' stores and parks their
                                // manifest -- the same door a bag stored onto
                                // the shelf GRID comes through -- and here it is
                                // spliced in under the entry that just landed.
                                // Without this the drop was refused outright
                                // ("no bag inside a bag"), which was a statement
                                // about the old flat data and not a rule anyone
                                // chose: a bag would not go into an open shelf
                                // bag at all (reported).
                                if (heldBag) {
                                    for (auto& b : TakePendingBundle(f)) {
                                        if (b.parent == 0) b.parent = nid;
                                        bundle.push_back(b);
                                    }
                                }
                                consumed = true;
                            }
                        }
                        if (consumed && liftIdx >= 0 && liftedObj) {
                            // C4: the occupant rides the cursor in its place
                            g_bundleCarry = { true, p->GetFormID(), a_w.spot,
                                              lifted.form, lifted.sig,
                                              lifted.col, lifted.row, lifted.id };
                            g_carryGlow = lifted.glow;   // (1.3.2)
                            g_carryStolen = lifted.stolen;
                            Grid::BeginPartnerCarry(liftedObj, lifted.count, 0,
                                                    -1.0f, -1.0f, 0, -1, 0,
                                                    lifted.rot);
                        }
                    }
                }
            }
        }

        // player-bag parity: NO outside-click close -- a bag window closes by
        // its right-click toggle, or ESC (newest first)
        ImGui::End();
        return true;
        }
    }

    void DrawShelfBag()
    {
        // ★(1.5.x) the hovered-pouch/square records live one frame: whatever
        // this pass draws re-records them. Cleared BEFORE the empty-out so
        // closing the last window also drops them.
        g_hoverBundlePouch = {};
        g_hoverBundleSq = {};
        if (g_shelfBags.empty()) return;
        if (!IsLootMode(g_mode)) { g_shelfBags.clear(); return; }
        auto* p = Partner();
        if (!p) { g_shelfBags.clear(); return; }
        const auto ci = g_contLayouts.find(p->GetFormID());
        if (ci == g_contLayouts.end()) { g_shelfBags.clear(); return; }
        for (std::size_t i = 0; i < g_shelfBags.size();) {
            if (DrawOneShelfBag(p, ci->second, g_shelfBags[i], static_cast<int>(i))) {
                ++i;
            } else {
                g_shelfBags.erase(g_shelfBags.begin() +
                                  static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    // ★(1.3.2a) the shelf POUCH's withdraw window -- the player pouch's
    // grammar over the SLOT's amount. Withdrawing credits the ledger and
    // puts a pinned purse on the cursor, exactly like the player's.
    void DrawShelfPouch()
    {
        if (g_shelfPouchSpot.empty()) return;
        if (!IsLootMode(g_mode)) { g_shelfPouchSpot.clear(); return; }
        auto* p = Partner();
        if (!p) { g_shelfPouchSpot.clear(); return; }
        const auto ci = g_contLayouts.find(p->GetFormID());
        if (ci == g_contLayouts.end()) { g_shelfPouchSpot.clear(); return; }
        const auto si = ci->second.cells.find(g_shelfPouchSpot);
        if (si == ci->second.cells.end()) {   // the pouch left the shelf
            g_shelfPouchSpot.clear();
            return;
        }
        // ★(1.5.x) which book: the cell's own gold, or a bundled entry's.
        // The entry is re-found every frame by its id -- a take, a move or a
        // sale that removes it simply closes the window.
        BundleItem* bentry = nullptr;
        if (g_shelfPouchBundle != 0) {
            const auto be = std::find_if(si->second.bundle.begin(),
                si->second.bundle.end(), [&](const BundleItem& b) {
                    return b.id == g_shelfPouchBundle;
                });
            if (be == si->second.bundle.end()) {   // the pouch left the bag
                g_shelfPouchSpot.clear();
                g_shelfPouchBundle = 0;
                return;
            }
            bentry = &*be;
        }
        const RE::FormID bankForm =
            bentry ? bentry->form : si->second.form;
        const int stored =
            bentry ? (std::max)(0, bentry->gold) : si->second.gold;
        if (g_shelfPouchSlider > stored) g_shelfPouchSlider = stored;

        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;
        char line[64];
        std::snprintf(line, sizeof(line), "%s: %d / %dG",
            Lang::T(Lang::Str::StoredLabel), stored,
            GoldCoins::PouchCapOfForm(bankForm));
        const float sliderW = 220.0f * S;
        const float contentW = (std::max)({ btnRow, sliderW,
            ImGui::CalcTextSize(line).x });
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float topPad = Theme::TitleTopPad();
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + ImGui::GetFrameHeight() + 6.0f * S +
                2.0f * sp + ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        wm->ApplyNext("shelfpouch",
            ImVec2((disp.x - size.x) * 0.5f, (disp.y - size.y) * 0.5f), size,
            WinManager::Anchor::kTopLeft, topPad);
        ImGui::Begin("##grid_shelfpouch", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        auto* pobj = RE::TESForm::LookupByID<RE::TESBoundObject>(g_shelfPouchForm);
        wm->TitleBar("shelfpouch",
            pobj && pobj->GetName() && *pobj->GetName() ? pobj->GetName() : "?",
            0.0f, true);

        if (!ImGui::IsWindowAppearing() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered()) {
            g_shelfPouchSpot.clear();
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(line).x);
        ImGui::TextColored(sk.ink, "%s", line);
        center(sliderW);
        Theme::ChromeSliderInt("##shelfdraw", &g_shelfPouchSlider, 0, stored,
                               sliderW, "%dG");
        const bool typing = ImGui::GetIO().WantTextInput;
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) &&
            g_shelfPouchSlider > 0) {
            --g_shelfPouchSlider;
        }
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) &&
            g_shelfPouchSlider < stored) {
            ++g_shelfPouchSlider;
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        const bool can = g_shelfPouchSlider > 0;
        const bool keyOk = !typing &&
                           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_Space, false));
        ImGui::BeginDisabled(!can);
        if (Sfx::Button(Lang::T(Lang::Str::Withdraw), ImVec2(btnW, 0)) ||
            (can && keyOk)) {
            const int v = g_shelfPouchSlider;
            // ★(1.5.x) the withdraw shrinks whichever book the window is on
            if (bentry) {
                bentry->gold = stored - v;
            } else {
                si->second.gold -= v;
            }
            // the shelf's book shrinks; the ledger grows to match, and the
            // amount rides the cursor as a pinned purse (player grammar)
            GoldCoins::CreditLedger(v);
            Grid::CarryWithdrawnGold(v);
            g_shelfPouchSpot.clear();
            g_shelfPouchBundle = 0;
            g_shelfPouchSlider = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 8.0f * S);
        if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
            g_shelfPouchSpot.clear();
        }
        ImGui::End();
    }

    int DepositOnHoveredPouch(int a_value)
    {
        if (a_value <= 0 || !IsLootMode(g_mode)) return 0;
        const auto sd = QueryStoreDrop();
        if (!sd.onCell || !sd.occ || !GoldCoins::IsPouch(sd.occ->GetFormID())) {
            return 0;
        }
        // BoardFor, not g_contLayouts directly -- loot mode makes them the
        // same book today, but the barter mismatch (ConsumeActingSpot) came
        // from exactly this shortcut drifting out of a mode it was safe in.
        auto* cl = BoardFor();
        if (!cl) return 0;
        const auto si = cl->cells.find(sd.occSpotKey);
        if (si == cl->cells.end()) return 0;
        const int room = GoldCoins::PouchCapOfForm(si->second.form) - si->second.gold;
        const int moved = (std::min)(a_value, room);
        if (moved <= 0) return 0;   // full: the coin keeps riding
        si->second.gold += moved;
        SKSE::log::info("[LOOT] deposited {} G into shelf pouch '{}' -> {}",
            moved, sd.occSpotKey, si->second.gold);
        return moved;
    }

    // ★(1.5.x) a book read off the shelf: the page offers the world's E-take.
    // (state lives beside the other window state near g_shelfPouchSpot; the
    // input sink writes only the atomic flag from its own thread, and the
    // render thread does the take where every other transfer starts)
    void NoteShelfBookRead(RE::TESBoundObject* a_book, std::uint16_t a_uid,
                           std::uint16_t a_sig, const std::string& a_spotKey)
    {
        if (!a_book) return;
        g_shelfBookRead = { true, a_book->GetFormID(), a_uid, a_sig, a_spotKey };
        g_shelfBookTakeFlag.store(false);
    }

    bool ShelfBookTakeArmed()
    {
        return g_shelfBookRead.active && UIRoot::IsBookOpen();
    }

    void FlagShelfBookTake() { g_shelfBookTakeFlag.store(true); }

    void ProcessShelfBookTake()
    {
        if (!g_shelfBookRead.active) return;
        const ShelfBookRead req = g_shelfBookRead;
        g_shelfBookRead = {};   // one page, one chance -- close clears it
        if (!g_shelfBookTakeFlag.exchange(false)) return;
        if (!IsLootMode(g_mode)) return;   // the session ended under the page
        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(req.form);
        if (!obj) return;
        if (!obj->As<RE::TESObjectBOOK>() || !Grid::CanFitNewItem(obj)) {
            if (!Grid::CanFitNewItem(obj)) {
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
            }
            return;
        }
        g_actingSpot = req.spot;   // GI20: the read cell is the one that leaves
        RequestTake(obj, 1, req.uid, req.sig, false);
        SKSE::log::info("[LOOT] book taken from the page (E)");
    }

    // ★(1.5.x) the bundled twin. The bag window recorded which pouch entry
    // the carry is over this frame; the coin routes land here first.
    bool IsBundlePouchHovered() { return g_hoverBundlePouch.id != 0; }

    int DepositOnHoveredBundlePouch(int a_value)
    {
        if (a_value <= 0 || !IsLootMode(g_mode) || g_hoverBundlePouch.id == 0) {
            return 0;
        }
        const auto ci = g_contLayouts.find(g_hoverBundlePouch.cont);
        if (ci == g_contLayouts.end()) return 0;
        const auto si = ci->second.cells.find(g_hoverBundlePouch.spot);
        if (si == ci->second.cells.end()) return 0;
        const auto be = std::find_if(si->second.bundle.begin(),
            si->second.bundle.end(), [&](const BundleItem& b) {
                return b.id == g_hoverBundlePouch.id;
            });
        if (be == si->second.bundle.end() || !GoldCoins::IsPouch(be->form)) {
            return 0;
        }
        const int held = (std::max)(0, be->gold);
        const int room = GoldCoins::PouchCapOfForm(be->form) - held;
        const int moved = (std::min)(a_value, room);
        if (moved <= 0) return 0;   // full: the coin keeps riding
        // (awaitGold deliberately untouched: a deposit racing the arrival
        // claim must not strand the parcel that is still on its way)
        be->gold = held + moved;
        SKSE::log::info("[LOOT] deposited {} G into bundled pouch -> {}",
            moved, be->gold);
        return moved;
    }

    // ★(1.5.x) player gold stored INTO an open shelf-bag window: the caller
    // (the Grid coin route) has already stored the physical Septims into the
    // container; this books them as a GOLD ENTRY in the hovered bag so they
    // arrive hidden inside it rather than surfacing as a loose shelf cell.
    // Merges into an existing gold entry at that level (stack grammar).
    bool IsShelfBagHovered() { return !g_hoverBundleSq.spot.empty(); }

    int IntakeGoldEntry(int a_amount)
    {
        if (a_amount <= 0 || !IsLootMode(g_mode) ||
            g_hoverBundleSq.spot.empty()) {
            return 0;
        }
        auto* vg = GoldCoins::VanillaGold();
        if (!vg) return 0;
        const auto ci = g_contLayouts.find(g_hoverBundleSq.cont);
        if (ci == g_contLayouts.end()) return 0;
        const auto si = ci->second.cells.find(g_hoverBundleSq.spot);
        if (si == ci->second.cells.end()) return 0;
        // the arriving Septims are spoken for (the same note the shelf-cell
        // store makes) -- without it the reconcile counts them twice
        NoteIn(vg, 0, 0, a_amount);
        for (auto& b : si->second.bundle) {
            if (b.form == vg->GetFormID() &&
                b.parent == g_hoverBundleSq.root) {
                b.count += a_amount;
                SKSE::log::info("[LOOT] {} G stored into the open bag -> {}",
                    a_amount, b.count);
                return a_amount;
            }
        }
        AddBundle(si->second.bundle,
                  { vg->GetFormID(), a_amount, 0,
                    g_hoverBundleSq.free ? g_hoverBundleSq.col : -1,
                    g_hoverBundleSq.free ? g_hoverBundleSq.row : -1,
                    0, 0, false, g_hoverBundleSq.root });
        SKSE::log::info("[LOOT] {} G stored into the open bag (new entry)",
            a_amount);
        return a_amount;
    }

    int DepositHeldGoldIntoShelfPouch(const std::string& a_pouchKey)
    {
        if (!IsLootMode(g_mode)) return 0;
        auto* cl = BoardFor();
        auto* src = SourceRef();
        if (!cl || !src || g_actingSpot.empty() || a_pouchKey == g_actingSpot) {
            return 0;
        }
        const auto pi = cl->cells.find(a_pouchKey);
        const auto gi = cl->cells.find(g_actingSpot);
        if (pi == cl->cells.end() || gi == cl->cells.end()) return 0;
        if (!GoldCoins::IsPouch(pi->second.form)) return 0;
        auto* gobj = RE::TESForm::LookupByID<RE::TESBoundObject>(gi->second.form);
        if (!gobj || !gobj->IsGold()) return 0;
        const int room = GoldCoins::PouchCapOfForm(pi->second.form) - pi->second.gold;
        const int moved = (std::min)((std::max)(0, gi->second.count), room);
        if (moved <= 0) return 0;   // full: the gold keeps riding

        pi->second.gold += moved;
        gi->second.count -= moved;
        if (gi->second.count <= 0) {
            cl->cells.erase(gi);
            g_actingSpot.clear();   // the carried cell is spent
        }
        // The chest's PHYSICAL coins become the pouch's virtual ones -- the
        // same lifecycle the pouch-leave op runs on the player (RemoveItem
        // into nowhere; take-back / withdraw mints them back). The engine
        // stack must fall by the same amount, or the reconcile re-mints a
        // gold cell for coins the book no longer shows. Queued to the game
        // thread (this runs in the render pass); NoteOut hides the amount
        // from the walk meanwhile -- the transfer queue's own pattern.
        NoteOut(gobj, 0, 0, moved);
        const RE::FormID srcId = src->GetFormID();
        const RE::FormID goldId = gobj->GetFormID();
        SKSE::GetTaskInterface()->AddTask([srcId, goldId, moved]() {
            auto* srcRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(srcId);
            auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(goldId);
            if (srcRef && gold) {
                srcRef->RemoveItem(gold, moved, RE::ITEM_REMOVE_REASON::kRemove,
                                   nullptr, nullptr);
            }
            if (gold) ClearOut(gold, 0, 0, moved);
        });
        SKSE::log::info("[LOOT] shelf gold -> shelf pouch: {} G into '{}' -> {}",
            moved, a_pouchKey, pi->second.gold);
        return moved;
    }

    // ★(1.5.x) shelf gold onto a pouch INSIDE an open shelf-bag window: the
    // bundled twin of the deposit above -- the carried gold CELL's units
    // become the entry's virtual amount, engine stack settled the same way.
    int DepositHeldGoldIntoBundlePouch()
    {
        if (!IsLootMode(g_mode) || g_hoverBundlePouch.id == 0) return 0;
        auto* cl = BoardFor();
        auto* src = SourceRef();
        if (!cl || !src || g_actingSpot.empty()) return 0;
        const auto gi = cl->cells.find(g_actingSpot);
        if (gi == cl->cells.end()) return 0;
        auto* gobj = RE::TESForm::LookupByID<RE::TESBoundObject>(gi->second.form);
        if (!gobj || !gobj->IsGold()) return 0;
        const auto ci = g_contLayouts.find(g_hoverBundlePouch.cont);
        if (ci == g_contLayouts.end()) return 0;
        const auto si = ci->second.cells.find(g_hoverBundlePouch.spot);
        if (si == ci->second.cells.end()) return 0;
        const auto be = std::find_if(si->second.bundle.begin(),
            si->second.bundle.end(), [&](const BundleItem& b) {
                return b.id == g_hoverBundlePouch.id;
            });
        if (be == si->second.bundle.end() || !GoldCoins::IsPouch(be->form)) {
            return 0;
        }
        const int held = (std::max)(0, be->gold);
        const int room = GoldCoins::PouchCapOfForm(be->form) - held;
        const int moved = (std::min)((std::max)(0, gi->second.count), room);
        if (moved <= 0) return 0;   // full: the gold keeps riding

        be->gold = held + moved;    // (awaitGold untouched -- see the coin route)
        gi->second.count -= moved;
        if (gi->second.count <= 0) {
            cl->cells.erase(gi);
            g_actingSpot.clear();   // the carried cell is spent
        }
        // same engine settle as the cell-to-cell deposit above
        NoteOut(gobj, 0, 0, moved);
        const RE::FormID srcId = src->GetFormID();
        const RE::FormID goldId = gobj->GetFormID();
        SKSE::GetTaskInterface()->AddTask([srcId, goldId, moved]() {
            auto* srcRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(srcId);
            auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(goldId);
            if (srcRef && gold) {
                srcRef->RemoveItem(gold, moved, RE::ITEM_REMOVE_REASON::kRemove,
                                   nullptr, nullptr);
            }
            if (gold) ClearOut(gold, 0, 0, moved);
        });
        SKSE::log::info("[LOOT] shelf gold -> bundled pouch: {} G -> {}",
            moved, be->gold);
        return moved;
    }

    RE::TESObjectREFR* Partner()
    {
        if (!g_partner) return nullptr;
        auto ref = g_partner.get();
        return ref ? ref.get() : nullptr;
    }

    RE::TESObjectREFR* SourceRef()
    {
        auto* partner = Partner();
        if (!partner) return nullptr;
        // BARTER: the partner is the merchant ACTOR, but the wares live in the
        // vendor faction's merchantContainer — not the actor's own inventory
        // (that's the merchant's personal belongings). LOOT: the container /
        // corpse ref itself is the source.
        if (g_mode == Mode::kBarter) {
            if (auto* actor = partner->As<RE::Actor>()) {
                if (auto* fac = actor->GetVendorFaction();
                    fac && fac->vendorData.merchantContainer) {
                    return fac->vendorData.merchantContainer;
                }
            }
        }
        return partner;
    }

    namespace
    {
        // UESP barter formula: factor = fBarterMax - (fBarterMax-fBarterMin) *
        // min(speech,100)/100 (defaults 3.3, 2.0). Read the settings live so
        // balance mods (Trade & Barter etc.) apply automatically.
        float BasePriceFactor()
        {
            float bMax = 3.3f, bMin = 2.0f;
            if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
                if (auto* s = gs->GetSetting("fBarterMax")) bMax = s->GetFloat();
                if (auto* s = gs->GetSetting("fBarterMin")) bMin = s->GetFloat();
            }
            float skill = 15.0f;
            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                if (auto* avo = p->AsActorValueOwner()) {
                    skill = avo->GetActorValue(RE::ActorValue::kSpeech);
                }
            }
            skill = std::clamp(skill, 0.0f, 100.0f);
            const float f = bMax - (bMax - bMin) * (skill / 100.0f);
            return f > 0.01f ? f : 0.01f;   // guard the sell-price divide
        }

        // Perk price coefficient (Haggling/Allure etc.) via HandleEntryPoint —
        // the engine multiplies `mod` in place by every kMultiplyValue perk on
        // the player for this entry point. 1.0 when no such perk applies.
        // Confirmed arg layout: (ENTRY_POINT, perkOwner, TESBoundObject*, float*).
        float PerkPriceMod(RE::TESBoundObject* a_item,
                           RE::BGSEntryPoint::ENTRY_POINT a_entry)
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p || !a_item) return 1.0f;
            float mod = 1.0f;
            RE::BGSEntryPoint::HandleEntryPoint(a_entry, p, a_item, &mod);
            return mod;
        }
    }

    int BuyPrice(RE::TESBoundObject* a_item, int a_baseValue)
    {
        if (a_baseValue <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModBuyPrices);
        return (std::max)(1,
            static_cast<int>(std::lround(a_baseValue * mod * BasePriceFactor())));
    }

    int SellPrice(RE::TESBoundObject* a_item, int a_baseValue)
    {
        if (a_baseValue <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices);
        // B7: floor 1, not 0 — a harsh barter multiplier used to round a
        // base-valuable item down to a ZERO-gold sale (item left, no pay)
        // (formula verified vanilla-exact 2026-07-28: lround(value*mod/factor),
        // mod/factor live-read -- the old +-1 was a polluted base, not this)
        return (std::max)(1,
            static_cast<int>(std::lround(a_baseValue * mod / BasePriceFactor())));
    }

    // B7: stack totals round ONCE on the total (vanilla behaviour) — the old
    // per-unit lround * count inflated cheap bulk buys (arrows: 100 x each
    // unit rounded up) and deflated bulk sells.
    int BuyPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count)
    {
        if (a_unitValue <= 0 || a_count <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModBuyPrices);
        return (std::max)(a_count, static_cast<int>(std::lround(
            static_cast<double>(a_unitValue) * a_count * mod * BasePriceFactor())));
    }

    int SellPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count)
    {
        if (a_unitValue <= 0 || a_count <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices);
        return (std::max)(a_count, static_cast<int>(std::lround(
            static_cast<double>(a_unitValue) * a_count * mod / BasePriceFactor())));
    }

    int MerchantGold()
    {
        if (g_mode != Mode::kBarter) return 0;
        // F3: unlimited mode — every affordability check compares against
        // INT_MAX and passes; the bottom bar branches to the infinity glyph.
        if (g_merchGoldInf) return (std::numeric_limits<int>::max)();
        auto* src = SourceRef();
        if (!src) return 0;
        for (auto& [obj, data] : src->GetInventory()) {
            if (obj && obj->IsGold()) return data.first;
        }
        return 0;
    }

    bool MerchantGoldInfinite() { return g_merchGoldInf; }
    void SetMerchantGoldInfinite(bool a_on) { g_merchGoldInf = a_on; }
    bool MerchantBuysAll() { return g_merchBuysAll; }
    void SetMerchantBuysAll(bool a_on) { g_merchBuysAll = a_on; }

    namespace
    {
        // Phase 6: a vendor VEND list matches an item by exact base form or by
        // any of the item's keywords (vanilla lists hold VendorItem* keywords).
        bool VendorListMatches(RE::BGSListForm* a_list, RE::TESBoundObject* a_obj)
        {
            if (!a_list || !a_obj) return false;
            if (a_list->HasForm(a_obj)) return true;
            if (auto* kwf = a_obj->As<RE::BGSKeywordForm>()) {
                for (auto* kw : kwf->GetKeywords()) {
                    if (kw && a_list->HasForm(kw)) return true;
                }
            }
            return false;
        }
    }

    bool MerchantBuys(RE::TESBoundObject* a_obj, bool a_stolen)
    {
        if (g_mode != Mode::kBarter || !a_obj) return true;
        // the custom coin pouch is always sellable (its sale releases stored
        // gold, §2-C) — exempt from the vendor category/stolen filter.
        if (GoldCoins::IsPouch(a_obj->GetFormID())) return true;
        auto* partner = Partner();
        auto* actor = partner ? partner->As<RE::Actor>() : nullptr;
        auto* fac = actor ? actor->GetVendorFaction() : nullptr;
        if (!fac) return true;   // no vendor data (plain container): unrestricted
        const auto& vv = fac->vendorData.vendorValues;

        // stolen: only a FENCE (buysStolen) accepts stolen goods. Non-stolen
        // items are always acceptable — buysNonStolen is a rare "fence-only"
        // flag that is 0 on ordinary merchants, so requiring it would (wrongly)
        // reject everything. Don't use it.
        if (a_stolen && !vv.buysStolen) return false;

        // F4: category restriction lifted — the stolen rule above stays.
        if (g_merchBuysAll) return true;

        // category: the VEND list is a whitelist (notBuySell=false) or a
        // blacklist (notBuySell=true). No list -> category unrestricted.
        auto* list = fac->vendorData.vendorSellBuyList;
        if (!list) return true;
        const bool inList = VendorListMatches(list, a_obj);
        return vv.notBuySell ? !inList : inList;
    }

    bool IsPartnerHovered() { return g_partnerHovered; }


    // ---- Phase 3: partner-window stages (bodies moved verbatim) ----
    namespace
    {
        // one cell per item form, vanilla-style stack badge (no Mabinogi
        // split on the partner side, per spec)
        struct PartnerCell
        {
            RE::TESBoundObject* obj;
            int count; int value;
            int w; int h; int col; int row;
            std::uint8_t glow;
            int  chance = -1;    // F6b: pickpocket % (whole stack), -1 = n/a
            bool locked = false; // F6b: worn without Perfect Touch
            // GI13: which sub-stack this cell shows. Gear is enumerated per
            // UNIT here, exactly like the player grid, so a tempered dagger and
            // a plain one never share a cell.
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;       // GI14 content signature (uid-less units)
            int           xlIdx = -1;
            int           ord = 0;       // GI13: nth cell of this identity
            bool          perUnit = false;
            bool          worn = false;  // on the body (corpse / pickpocket target)
            // GI42: clicking this cell could make the engine grab an ILLEGAL
            // worn unit (same plain form on the body, no naming handle). Locked
            // up front -- vanilla locks the whole row for the same reason.
            bool          unnameable = false;
            // ★★Is taking THIS unit a theft? Not the same question as "whose
            // container is this". Taking back what you deposited is not
            // stealing however hostile the room -- which is the whole of the
            // reported Embassy complaint -- and the deposit ledger is what
            // knows the difference. Everything else in an owned container is
            // somebody's.
            bool          stolen = false;
            // GI20: the pool slot this cell was assigned this frame. Taking the
            // cell must free THIS slot -- otherwise the pool just loses its
            // trailing position and everything after the taken cell shuffles up,
            // which is exactly "it loots in storage order, not the one I hovered".
            std::string   spotKey;
            // ★What the SHELF is holding for this cell -- a coin pouch's own
            // gold. Carried onto the cell during slot assignment so the draw
            // does not have to reach back into the layout.
            int           gold = 0;
            // GI62: quarter-turns clockwise. Set through SetRot so w/h and the
            // angle can never disagree -- every placement test on this side reads
            // w/h directly, and a stale pair would place the cell wrong.
            int           rot = 0;
            void SetRot(int a_rot)
            {
                if (((rot ^ a_rot) & 1) != 0) std::swap(w, h);
                rot = a_rot & 3;
            }
        };

        // F7: this frame's partner-grid geometry for drop-cell math — set by
        // DrawWindows while a spot-memory (kLoot/kSteal) grid is drawing
        bool                     g_partnerGridLive = false;
        ImVec2                   g_partnerBase{};      // scroll-adjusted origin
        ImVec2                   g_partnerClipMin{};   // grid child rect
        ImVec2                   g_partnerClipMax{};
        int                      g_partnerRows = 0;
        std::vector<PartnerCell> g_lastCells;          // this frame's placement
        // ★(1.3.3) measured height of everything above the board (title bar,
        // section label, window padding, title clearance). -1 = not measured
        // yet. Independent of the row count, so it converges on the second
        // frame and then holds. Reset when the skin or the scale changes.
        float                    g_partnerChromeH = -1.0f;

        // ★Search on the partner board. Its cells are rebuilt EVERY frame, so
        // there is no board version to hang a key-set off the way the player's
        // grid does — but a form's name does not change, so the answer caches
        // per FORM and survives every rebuild. The term itself is the player
        // grid's (Grid::SearchTerm): one box searches both sides of the trade,
        // which is the whole point when you are looking for what to buy.
        std::unordered_map<RE::FormID, bool> g_findCache;
        std::string                          g_findTerm;

        [[nodiscard]] bool FindMisses(RE::TESBoundObject* a_obj)
        {
            const std::string& term = Grid::SearchTerm();
            if (term.empty()) return false;
            if (term != g_findTerm) {   // a new term invalidates every answer
                g_findCache.clear();
                g_findTerm = term;
            }
            if (!a_obj) return true;
            const RE::FormID id = a_obj->GetFormID();
            if (const auto it = g_findCache.find(id); it != g_findCache.end()) {
                return !it->second;
            }
            const bool hit = Grid::SearchMatches(a_obj->GetName());
            g_findCache.emplace(id, hit);
            return !hit;
        }

        // stage 1: collect partner items (worn included, for corpses)
        // The engine's own snapshot type. Taken once per frame and passed
        // around: GetInventory allocates every entry it returns, so asking
        // twice would be two walks and two sets of pointers.
        using InvMap = RE::TESObjectREFR::InventoryItemMap;

        // ---- POOL IDENTITY --------------------------------------------------
        //
        // What makes two units interchangeable: the same form, the same naming
        // handle, the same worn-ness. Cells of one pool differ only in HOW MANY
        // they hold, which is exactly why the reconcile below may move units
        // between them freely and must never move them across.
        std::string PoolOf(RE::FormID a_form, std::uint16_t a_uid,
                           std::uint16_t a_sig, bool a_worn)
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%08X@%04X~%04X%c",
                          a_form, a_uid, a_sig, a_worn ? 'W' : '.');
            return buf;
        }
        std::string PoolOf(const ContCell& a_c)
        {
            return PoolOf(a_c.form, a_c.uid, a_c.sig, a_c.worn);
        }

        // A fresh opaque id for a new cell in this container. It reads like a
        // form key because that is pleasant in a log; nothing parses it.
        std::string MintCellKey(ContLayout& a_cl, RE::TESBoundObject* a_obj)
        {
            const std::string base = PartnerKey(a_obj, 0, 0, 0);
            for (int n = 0;; ++n) {
                std::string k = n == 0 ? base : base + "#" + std::to_string(n);
                if (!a_cl.cells.contains(k)) return k;
            }
        }

        // ★Is this cell the one riding the cursor? It used to be asked as "is
        // this UNIT held", matched by a per-form ordinal computed while cells
        // were being derived -- which could name a different cell, or none, the
        // moment a form owned more than one. A carry names a cell.
        bool HeldCell(const std::string& a_key)
        {
            return !a_key.empty() && a_key == g_actingSpot &&
                   Grid::HeldPartnerObject() != nullptr;
        }

        ContLayout* BoardFor()
        {
            auto* p = Partner();
            if (!p) return nullptr;
            if (SpotMemoryOn()) return &g_contLayouts[p->GetFormID()];
            return &g_barterBoard;
        }

        // ---- RECONCILE ------------------------------------------------------
        //
        // ★★★THE ONE PLACE THE ENGINE TOUCHES THIS BOARD, and it touches it as a
        // CORRECTION rather than as a source. Everything the player does writes
        // to the cells directly -- move is col/row, merge is count, take is
        // count, store is a new cell -- and this pass exists only to answer the
        // question those writes cannot: what changed underneath us. A script
        // emptied the barrel, a follower ate something, a transfer we queued
        // finally landed.
        //
        // It is the player board's ACQUIRE/RELEASE, said on this side:
        //   want > have -> fill partial cells in position order, then add
        //                  cap-sized cells
        //   want < have -> shrink from the back, delete what empties
        //   no engine presence at all -> the cells of that pool die (and their
        //                  pouch gold goes home, exactly as before)
        //
        // ★DELETION COMES LAST, and that ordering is the whole migration. A save
        // written before cells owned a count reads back count 0 -- which means
        // "unspecified", not "empty" -- and the fill phase pours the container's
        // real contents into those cells in the positions they were left in. Had
        // the empty check run first, every shelf in every save would have been
        // swept and re-first-fitted once.
        struct SeenPool
        {
            RE::TESBoundObject* obj = nullptr;
            std::uint16_t       uid = 0;
            std::uint16_t       sig = 0;
            int                 xlIdx = -1;
            bool                worn = false;
            int                 count = 0;
            int                 w = 1;
            int                 h = 1;
        };

        void ReconcileContainer(ContLayout& a_cl, RE::TESObjectREFR* a_source,
                                const InvMap& a_inv)
        {
            // ---- 1. what the engine says is in here ----
            // working copies: units already committed to the player leave this
            // board NOW, not one Tick later (see g_outPool), and units riding
            // inside a shelved bag are the bag's, not the shelf's.
            auto outPool = g_outPool;
            auto outForm = g_outForm;
            std::map<RE::FormID, int> bundled;
            for (const auto& [k, c] : a_cl.cells) {
                for (const auto& b : c.bundle) bundled[b.form] += b.count;
            }
            if (auto* pref = Partner()) {
                for (const auto& pb : g_pendingBundles) {
                    if (pb.cont != pref->GetFormID()) continue;
                    for (const auto& b : pb.items) bundled[b.form] += b.count;
                }
            }

            std::map<std::string, SeenPool> seen;
            for (auto& [obj, data] : a_inv) {
                const int count = data.first;
                if (!obj || count <= 0) continue;
                // barter: the merchant's gold is BOOKKEEPING (shown in the
                // bottom bar), not a purchasable ware
                if (g_mode == Mode::kBarter && obj->IsGold()) continue;
                const char* name = obj->GetName();
                if (!name || !*name) continue;       // unnamed (our coins) skipped
                if (!obj->GetPlayable()) continue;   // vanilla parity: hidden forms
                auto*      entry = data.second.get();
                const auto def = Grid::ResolveDef(obj);
                const int  dw = (std::max)(1, def.w);
                const int  dh = (std::max)(1, def.h);

                // GI13: gear (cap 1) is enumerated per UNIT, matching the player
                // grid -- a tempered dagger and a plain one are never one cell.
                // Stackables stay one pool: their units really are
                // interchangeable, and one pool per arrow would be absurd.
                // Worn units are KEPT (a_skipWorn=false): on a corpse or a
                // pickpocket target, what the NPC is wearing is the point.
                const bool perUnit = Grid::StackCap(obj) <= 1;
                std::vector<Grid::UnitRef> units;
                if (perUnit) Grid::EnumerateUnits(entry, count, units, false);
                if (units.empty()) units.push_back({ 0, 0, -1 });

                for (const auto& u : units) {
                    int have = perUnit ? 1 : count;
                    {   // already promised to the player
                        auto pi = outPool.find(OutKey(obj->GetFormID(), u.uid, u.sig));
                        auto fi = outForm.find(obj->GetFormID());
                        int  owed = 0;
                        if (pi != outPool.end() && pi->second > 0)      owed = pi->second;
                        else if (fi != outForm.end() && fi->second > 0) owed = fi->second;
                        if (owed > 0) {
                            // both counters track the same units, so they have
                            // to fall together or a later cell is hidden twice
                            const int take = (std::min)(owed, have);
                            if (pi != outPool.end()) pi->second = (std::max)(0, pi->second - take);
                            if (fi != outForm.end()) fi->second = (std::max)(0, fi->second - take);
                            have -= take;
                        }
                    }
                    if (auto bi = bundled.find(obj->GetFormID());
                        bi != bundled.end() && bi->second > 0 && have > 0) {
                        const int hide = (std::min)(bi->second, have);
                        bi->second -= hide;
                        have -= hide;
                    }
                    if (have <= 0) continue;

                    bool worn = u.worn;
                    // A STACKABLE's placeholder unit resolves to no list at all,
                    // so an equipped torch on a mark reported "not worn": no
                    // tint, and no pickpocket lock. Ask the entry instead.
                    if (!perUnit && !worn && entry && entry->extraLists) {
                        for (auto* x2 : *entry->extraLists) {
                            if (x2 && (x2->HasType<RE::ExtraWorn>() ||
                                       x2->HasType<RE::ExtraWornLeft>())) {
                                worn = true;
                                break;
                            }
                        }
                    }
                    auto& sp = seen[PoolOf(obj->GetFormID(), u.uid, u.sig, worn)];
                    sp.obj = obj;
                    sp.uid = u.uid;
                    sp.sig = u.sig;
                    sp.xlIdx = u.xlIdx;
                    sp.worn = worn;
                    sp.w = dw;
                    sp.h = dh;
                    sp.count += have;
                }
            }

            // ★Units we have promised this container but the engine has not
            // moved yet count as PRESENT. See g_in: the cell for them already
            // exists, because the drop is what made it.
            for (auto it = g_in.begin(); it != g_in.end();) {
                const auto& e = it->second;
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(e.form);
                if (!obj || e.count <= 0) { it = g_in.erase(it); continue; }
                // ★What has actually turned up since the promise was made. Any
                // road counts -- the transfer queue, GoldCoins' own queue, a
                // script -- because the question is only whether the container
                // has them now.
                int held = 0;
                for (const auto& [o, d] : a_inv) {
                    if (o && o->GetFormID() == e.form) held += d.first;
                }
                const int owed = e.count - (std::max)(0, held - e.seenAt);
                if (owed <= 0) { it = g_in.erase(it); continue; }
                auto&      sp = seen[PoolOf(e.form, e.uid, e.sig, false)];
                const auto def = Grid::ResolveDef(obj);
                sp.obj = obj;
                sp.uid = e.uid;
                sp.sig = e.sig;
                sp.worn = false;
                sp.w = (std::max)(1, def.w);
                sp.h = (std::max)(1, def.h);
                sp.count += owed;
                ++it;
            }

            // ---- 2. bring the cells into line, pool by pool ----
            std::map<std::string, std::vector<std::string>> byPool;
            for (const auto& [k, c] : a_cl.cells) byPool[PoolOf(c)].push_back(k);
            const auto inOrder = [&](std::vector<std::string>& a_keys) {
                std::sort(a_keys.begin(), a_keys.end(),
                    [&](const std::string& x, const std::string& y) {
                        const auto& cx = a_cl.cells[x];
                        const auto& cy = a_cl.cells[y];
                        if (cx.row != cy.row) return cx.row < cy.row;
                        if (cx.col != cy.col) return cx.col < cy.col;
                        return x < y;
                    });
            };

            for (auto& [pool, sp] : seen) {
                const int cap = (std::max)(1, Grid::StackCap(sp.obj));
                auto&     mine = byPool[pool];
                inOrder(mine);
                int have = 0;
                for (const auto& k : mine) have += (std::max)(0, a_cl.cells[k].count);
                int diff = sp.count - have;

                if (diff > 0) {
                    // fill what is already on the shelf, front first...
                    for (const auto& k : mine) {
                        if (diff <= 0) break;
                        auto&     c = a_cl.cells[k];
                        const int room = cap - (std::max)(0, c.count);
                        if (room <= 0) continue;
                        const int add = (std::min)(room, diff);
                        c.count = (std::max)(0, c.count) + add;
                        diff -= add;
                    }
                    // ...then lay out new cells for the rest
                    while (diff > 0) {
                        const int take = (std::min)(cap, diff);
                        ContCell  nc;
                        nc.form = sp.obj->GetFormID();
                        nc.uid = sp.uid;
                        nc.sig = sp.sig;
                        nc.xlIdx = sp.xlIdx;
                        nc.worn = sp.worn;
                        nc.count = take;
                        nc.w = sp.w;
                        nc.h = sp.h;
                        // col/row left at -1: the placement pass first-fits it
                        const std::string k = MintCellKey(a_cl, sp.obj);
                        // ★A bag arriving on the shelf claims the bundle its
                        // store queued, and a pouch claims the gold that walked
                        // out with it. This is where a cell is BORN now -- there
                        // used to be three such doors and they disagreed.
                        nc.bundle = TakePendingBundle(nc.form);
                        if (GoldCoins::IsPouch(nc.form)) {
                            nc.gold = GoldCoins::TakeAwayGold(nc.form);
                            if (nc.gold > 0) {
                                SKSE::log::info("[LOOT] pouch shelved with {} G ('{}')",
                                                nc.gold, k);
                            }
                        }
                        a_cl.cells[k] = std::move(nc);
                        mine.push_back(k);
                        diff -= take;
                    }
                } else if (diff < 0) {
                    // shrink from the back, so what the player put in front
                    // stays where they put it
                    int owe = -diff;
                    for (auto it = mine.rbegin(); it != mine.rend() && owe > 0; ++it) {
                        auto&     c = a_cl.cells[*it];
                        const int cut = (std::min)(owe, (std::max)(0, c.count));
                        c.count -= cut;
                        owe -= cut;
                    }
                }
            }

            // ★A pouch cell that was born before its money. One claim per
            // pass and one cell at a time -- only one pouch can be in flight,
            // since only one thing can ride the cursor.
            for (auto& [k, c] : a_cl.cells) {
                if (c.awaitGold <= 0) continue;
                if (const int g = GoldCoins::TakeAwayGold(c.form); g > 0) {
                    c.gold += g;
                    c.awaitGold = 0;
                    SKSE::log::info("[LOOT] pouch shelved with {} G ('{}', late claim)",
                                    g, k);
                } else {
                    --c.awaitGold;
                }
                break;
            }

            // ★(1.5.x) and the BUNDLED pouches claim theirs the same way. No
            // one-at-a-time here: a bag arrives with all of its pouches at
            // once. An entry stamped with its tile's amount (wantGold) takes
            // exactly THAT parcel -- two same-form pouches in one gesture
            // used to trade amounts on first-come -- and only falls back to
            // first-come on the grace's last breath, so money never strands.
            for (auto& [k, c] : a_cl.cells) {
                for (auto& b : c.bundle) {
                    if (b.awaitGold <= 0) continue;
                    int g = 0;
                    if (b.wantGold > 0) {
                        g = GoldCoins::TakeAwayParcelExact(b.form, b.wantGold);
                        if (g <= 0 && b.awaitGold > 1) {
                            --b.awaitGold;   // its own parcel is still on the way
                            continue;
                        }
                    }
                    if (g <= 0) g = GoldCoins::TakeAwayGold(b.form);
                    if (g > 0) {
                        b.gold = (std::max)(0, b.gold) + g;
                        b.awaitGold = 0;
                        b.wantGold = 0;
                        SKSE::log::info(
                            "[LOOT] bundled pouch holds {} G (bag '{}', late claim)",
                            g, k);
                    } else {
                        --b.awaitGold;
                    }
                }
            }

            // ---- 3. and only now, what is gone is gone ----
            // A cell whose pool the engine no longer reports, or that ended this
            // pass holding nothing. ★Same death rites as before: a pouch's money
            // goes home rather than dying with its cell, and a bag's bundle is
            // handed back the same way.
            for (auto it = a_cl.cells.begin(); it != a_cl.cells.end();) {
                const bool poolGone = !seen.contains(PoolOf(it->second));
                if (!poolGone && it->second.count > 0) { ++it; continue; }
                if (it->second.gold > 0) {
                    SKSE::log::info("[LOOT] shelf cell dropped, {} G goes home ('{}')",
                                    it->second.gold, it->first);
                    GoldCoins::GiveAwayGold(it->second.gold, it->second.form);
                }
                // ★(1.5.x) same rites for the pouches bundled INSIDE it
                for (const auto& b : it->second.bundle) {
                    if (b.gold <= 0) continue;
                    SKSE::log::info(
                        "[LOOT] bundled pouch's {} G goes home (bag cell '{}' dropped)",
                        b.gold, it->first);
                    GoldCoins::GiveAwayGold(b.gold, b.form);
                }
                it = a_cl.cells.erase(it);
            }
        }

        std::vector<PartnerCell> CollectPartnerCells(RE::TESObjectREFR* source)
        {
            std::vector<PartnerCell> cells;
            // GI20: the acting cell only lives as long as the interaction that
            // named it. A carry that gets cancelled, or a quantity slider that
            // gets closed, must not leave it primed for the NEXT take.
            if (!Grid::IsHolding() && !g_slider.active && !g_confirm.active) {
                g_actingSpot.clear();
                g_carryGlow = 0;                // (1.3.2) markers die with it
                // ★★★AND THE OTHER MARKER. g_carryStolen was written only by
                // the bundle lift and cleared by nobody, so one stolen item
                // lifted out of a shelf bag latched it true for the session:
                // every later drop into a bag was recorded stolen, in the
                // cosave, and a genuinely stolen cell dropped in afterwards
                // came out clean. Set in one place, cleared in none -- exactly
                // the pairing §6-2 is about.
                g_carryStolen = false;
                g_bundleCarry.active = false;   // (1.3.1) cancelled carry: the
                                                // entry was never removed, so
                                                // the item is simply back in
                                                // its bag -- nothing to undo
            }
            SweepOut();
            SweepIn();
            auto* cl = BoardFor();
            if (!cl) return cells;

            const auto inv = source->GetInventory();
            ReconcileContainer(*cl, source, inv);

            // ---- the VIEW ----
            // One PartnerCell per ContCell, carrying the things only the draw
            // needs: price, markers, pickpocket odds, locks. Nothing here
            // decides what exists or how much of it -- that was settled above.
            const bool carrying = Grid::HeldPartnerObject() != nullptr;
            std::map<std::string, int> ordOf;   // nth cell of its pool
            // ★A SECOND COUNTER, BECAUSE THE LEDGER COUNTS SOMETHING ELSE.
            // `ord` is the cell's place within its POOL, which is what the
            // carry and the swap need. The deposit ledger is keyed by FORM.
            // Comparing one against the other gave every pool of a form its
            // own run of low ordinals, so a single deposit cleared the first
            // cell of each -- store one plain sword in the innkeeper's chest
            // and his enchanted one stopped reading as stolen too.
            std::map<RE::FormID, int> formOrd;
            for (const auto& [key, c] : cl->cells) {
                // ★THE CELL IN HAND IS NOT ON THE SHELF, and that is the whole
                // of it now. This used to be a per-unit test against an ordinal
                // computed while deriving cells (IsHeldPartnerUnit), which meant
                // it could name the wrong cell -- or, once a form owned several,
                // none at all. A carry names a cell; the shelf skips that cell.
                if (carrying && key == g_actingSpot) continue;
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(c.form);
                if (!obj || c.count <= 0) continue;

                RE::InventoryEntryData* entry = nullptr;
                int                     unitValue = 0;
                if (const auto ii = inv.find(obj); ii != inv.end()) {
                    entry = ii->second.second.get();
                    unitValue = entry ? entry->GetValue() : 0;
                }
                const auto def = Grid::ResolveDef(obj);
                const bool perUnit = Grid::StackCap(obj) <= 1;
                auto* xl = Grid::ExtraForInstance(entry, c.uid, c.xlIdx);
                // ★★★FALL BACK TO THE POOL WHEN THE INSTANCE CANNOT BE NAMED.
                //
                // ExtraForInstance finds a list by uid, or failing that by its
                // recorded position. A unit stored on OUR side of a container
                // has neither: the engine assigns no uid, and the cell carries
                // xlIdx = -1 because the position was never recorded. Measured:
                // `want uid=0000 sig=B825 idx=-1 -> xl=N` against a container
                // holding exactly `[0 uid=0000 sig=B825 own=00000007]`. The
                // signature matched perfectly and was simply never consulted.
                //
                // Everything the list carries goes with that miss -- temper,
                // enchantment, price, and the ownership the stolen mark reads
                // -- which is why a second tempered dagger in a barrel made
                // BOTH read as plain and eventually marked all of them stolen.
                //
                // ★The pool is the right granularity for a display: units that
                // share a signature carry the same temper and the same price,
                // so reading either answers the cell's question. It is NOT
                // precise enough to move an item by, which is exactly why
                // transfers keep using their own resolution.
                if (!xl) xl = Grid::ExtraForPool(entry, c.uid, c.sig);
                // GI43: a per-unit cell is priced from ITS list -- a tempered
                // unit buys/sells at the vanilla tempered value.
                int value = perUnit ? Grid::UnitValueWith(obj, xl) : unitValue;
                // ★Gold is the exception, and only because `value` is a
                // PER-UNIT price everywhere else: a purse is worth its coins.
                if (obj->IsGold()) value = c.count;

                PartnerCell pc{ obj, c.count, value,
                                (std::max)(1, def.w), (std::max)(1, def.h), -1, -1,
                                Grid::GlowBits(obj, entry, xl) };
                pc.uid = c.uid;
                pc.sig = c.sig;
                pc.xlIdx = c.xlIdx;
                pc.perUnit = perUnit;
                pc.worn = c.worn;
                pc.gold = c.gold;
                pc.spotKey = key;
                pc.ord = ordOf[PoolOf(c)]++;
                pc.col = c.col;
                pc.row = c.row;
                pc.SetRot(c.rot);
                // GI42: the lock's resolution must MATCH the naming resolution.
                // Locking only the worn cell while a spare cell could still pull
                // the worn unit out through the engine was the bypass.
                pc.unnameable = Grid::PoolChoice(entry, pc.uid, pc.sig, pc.worn,
                                    WornExportLegal()).kind ==
                                Grid::PickKind::kUnresolved;
                if (g_mode == Mode::kPickpocket) {
                    pc.chance = PickpocketChance(obj, pc.count, entry);
                    pc.locked = (pc.worn && !HasPerfectTouch()) || pc.unnameable;
                } else {
                    pc.locked = pc.unnameable;
                }
                // ★An owned container makes its contents stolen goods -- EXCEPT
                // what the player put there, which says so on itself. The
                // player's own base form is 0x7; anything else (or nothing) in
                // an owned container is somebody else's.
                // ★An owned container's contents are stolen goods -- except
                // what the player deposited, which the ledger remembers. The
                // cells of one FORM are numbered by formOrd, so the first N of
                // them are the N units the player put here: mark from there on.
                // (pc.ord counts within the pool and belongs to the carry; the
                // two were the same number only while a form had one pool.)
                if (g_mode == Mode::kSteal) {
                    if (xl && xl->GetOwner()) {
                        // ★A unit that NAMES an owner is theirs whatever the
                        // ledger says, and -- the point of putting this first
                        // -- it must not eat a deposit credit. The credit then
                        // lands on the unowned units, which are the ones the
                        // player actually put here.
                        pc.stolen = true;
                    } else {
                        const int fo = formOrd[obj->GetFormID()]++;
                        pc.stolen = fo >= DepositedCount(source, obj);
                    }
                }
                cells.push_back(std::move(pc));
            }
            // ---- self-check: THE PARTNER BOARD ------------------------------
            // The player board has had [FLICK] since P1; this side had nothing,
            // so a container/corpse/merchant blink could only ever be judged by
            // eye. Logged only when the cell set CHANGES, so it stays quiet.
            {
                // ★★HASH EVERY FRAME, FORMAT ONLY WHEN IT CHANGES.
                //
                // This built one std::string per cell, sorted the lot, then
                // concatenated them into a third -- every frame, on a merchant
                // window that can carry two hundred items, to answer a question
                // whose answer is "no" almost always. The check earns its place;
                // paying for it while nothing moves does not (원칙 3).
                //
                // The mix is commutative on purpose: the order cells happen to
                // come out in is not news, and a reordering that read as a
                // change would make the log lie about blinking.
                std::uint64_t sig = 1469598103934665603ull ^ cells.size();
                for (const auto& c : cells) {
                    std::uint64_t h = c.obj ? c.obj->GetFormID() : 0u;
                    h = h * 1099511628211ull + static_cast<std::uint64_t>(c.count);
                    h = h * 1099511628211ull + (c.worn ? 1ull : 0ull);
                    sig += h ^ (h >> 29);
                }
                static std::uint64_t s_prev = 0;
                static std::size_t   s_prevN = 0;
                if (sig != s_prev) {
                    // the readable form, built once, on the frame it changed
                    std::vector<std::string> ks;
                    ks.reserve(cells.size());
                    for (const auto& c : cells) {
                        ks.push_back(std::format("{}x{}{}", c.count,
                            c.obj ? c.obj->GetName() : "?", c.worn ? "*" : ""));
                    }
                    std::sort(ks.begin(), ks.end());
                    std::string line = std::to_string(cells.size());
                    for (const auto& k : ks) { line += " | "; line += k; }
                    SKSE::log::info("[PFLICK] cells {} -> {}  {}",
                        s_prev == 0 ? std::string("-") : std::to_string(s_prevN),
                        cells.size(), line);
                    s_prev = sig;
                    s_prevN = cells.size();
                }
            }
            return cells;
        }

        // stage 2: footprint-aware placement on the 10-wide board; only the
        // SIZE is honoured (stacks stay one footprint + badge). F7: in a
        // spot-memory mode (kLoot/kSteal) remembered spots place FIRST
        // (fixed), the rest first-fits, and the result is recorded back —
        // the very first look auto-packs and immediately becomes the saved
        // arrangement. Returns the furthest occupied/remembered row.
        // ---- PLACEMENT ------------------------------------------------------
        //
        // ★★What is left of a pass that used to be three hundred lines. It was
        // long because it had to GUESS: cells were derived fresh every frame, so
        // something had to decide which of them inherited which remembered
        // position, and the answer was a three-tier matcher over pools, worn-ness
        // and signatures, plus a prune for positions nothing claimed.
        //
        // Cells are state now. A cell already knows where it is, because the
        // move that put it there wrote it down. All that remains is the honest
        // remainder of the job: give a cell that has never been placed a home,
        // and hand the drawn list its positions.
        int PlacePartnerCells(std::vector<PartnerCell>& cells)
        {
            const int cols = Grid::kCols;
            // (1.3.3) a living follower's pack is capped -- see CompanionPartner
            const bool companionBoard = CompanionPartner();
            std::vector<std::vector<bool>> occ;
            auto ensureRow = [&](int r) {
                while (static_cast<int>(occ.size()) <= r) occ.emplace_back(cols, false);
            };
            auto fits = [&](int c, int r, int w, int h) {
                if (c < 0 || r < 0 || c + w > cols) return false;
                for (int y = 0; y < h; ++y) {
                    ensureRow(r + y);
                    for (int x = 0; x < w; ++x)
                        if (occ[r + y][c + x]) return false;
                }
                return true;
            };
            auto mark = [&](int c, int r, int w, int h) {
                for (int y = 0; y < h; ++y) {
                    ensureRow(r + y);
                    for (int x = 0; x < w; ++x) occ[r + y][c + x] = true;
                }
            };

            ContLayout* cl = BoardFor();

            // pass 1: everything that already has a home keeps it.
            // ★(1.3.3) a COMPANION's board is 10 x 8, and a position from before
            // that cap (or from the +2 spare rows every other container gets)
            // would hold the board open past it -- the pack drew nine rows for an
            // item nobody could see down there. Out-of-cap cells are refused here
            // and first-fit back inside by pass 2.
            for (auto& it : cells) {
                if (it.col < 0 || it.row < 0) continue;
                const bool inCap = !companionBoard || it.row + it.h <= kCompanionRows;
                if (inCap && fits(it.col, it.row, it.w, it.h)) {
                    mark(it.col, it.row, it.w, it.h);
                } else {
                    it.col = -1;
                    it.row = -1;
                    if (it.rot != 0) it.SetRot(0);   // stand it up rather than lose it
                }
            }

            // pass 2: first-fit for everything unplaced
            // ★(1.3.3) a companion fills its 10 x 8 pack FIRST and only
            // grows past it when what it already carries genuinely will not
            // fit (its own gear, a script's gift). The cap governs what the
            // player may add; it never hides what is already inside.
            for (auto& it : cells) {
                if (companionBoard) {
                    for (int r = 0; r + it.h <= kCompanionRows && it.col < 0; ++r) {
                        for (int c = 0; c < cols; ++c) {
                            if (!fits(c, r, it.w, it.h)) continue;
                            it.col = c;
                            it.row = r;
                            mark(c, r, it.w, it.h);
                            break;
                        }
                    }
                    if (it.col >= 0) continue;
                }
                for (int r = companionBoard ? kCompanionRows : 0; it.col < 0; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        if (fits(c, r, it.w, it.h)) {
                            it.col = c;
                            it.row = r;
                            mark(c, r, it.w, it.h);
                            break;
                        }
                    }
                }
            }

            // pass 3: hand the first-fit results back to the cells that were
            // placed this frame. ★Field-wise, never a whole-struct init: the
            // position is this pass's to write, and the count, the pouch's gold
            // and the bag's bundle are the cell's own and must survive it.
            if (cl) {
                for (const auto& it : cells) {
                    const auto ci = cl->cells.find(it.spotKey);
                    if (ci == cl->cells.end()) continue;
                    ci->second.col = it.col;
                    ci->second.row = it.row;
                    ci->second.w = it.w;
                    ci->second.h = it.h;
                    ci->second.rot = it.rot;
                }
            }

            // actual height = furthest occupied row. GI15: absent items no
            // longer hold a shelf open -- their cells were reconciled away.
            int placedRows = 0;
            for (auto& it : cells) placedRows = (std::max)(placedRows, it.row + it.h);
            return placedRows;
        }

        // stage 3: partner grid lines + cells (hover / tooltip / carry /
        // take / buy) + scroll extent — runs inside the grid child
        void DrawPartnerCells(std::vector<PartnerCell>& cells, int rows,
                              float gridW, float sbW, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            auto* cache = IconCache::GetSingleton();
            const auto& sk = Theme::S();
            const float cell = Grid::CellPx();
            const int cols = Grid::kCols;
            // ★The board, drawn by the SAME function the player's grid uses.
            // This was a private accent hairline, which meant a skin that
            // carves or tiles its cells did so on one half of the screen only
            // — and on SIMPLE that hairline is near-invisible navy on a light
            // blue panel, so the merchant's board had no cells at all.
            Grid::DrawCellLattice(dl, base, cols, rows);

            // (the board's outer edge is drawn OUTSIDE this child, at a fixed
            //  position -- see PartnerBoardEdge)

            // GI16: occupied-cell shading, the same pass the player grid runs
            // (DrawOccupancyPass). Its absence here was never a deliberate
            // choice -- the partner window simply drew sprites onto bare grid
            // lines, so the two halves of the same screen read differently.
            // Worn gear takes a pale amber fill instead of the neutral shade:
            // on a corpse or a pickpocket target, "this is on the body" is the
            // one distinction that changes what the player does next.
            // same ground as the player's grid and the doll (see Grid.cpp)
            const ImU32 shadeCol = Theme::OccupiedGround();
            // ★★★NO COLOUR FOR "WORN". It used to take a hardcoded amber ground
            // -- the one colour on this board that did not come from the skin,
            // which is why it looked wrong on every one of them -- and the
            // colour could not carry the meaning anyway. Nothing teaches a
            // player that a pale ground means "this is on the body"; the state
            // was read as new-pickup instead, by the person who designed it.
            // Worse, it collided with what a ground already says here: our own
            // grid uses ground tints for new-pickup and open-bag.
            // ★It is a MARK now, in the corner, the way the lock says "you
            // cannot have this". See the worn silhouette further down.
            // ★The clearance is the skin's, not a constant. Same rule as the
            // player's grid: an engraved skin carves after the cell, an ink
            // skin rules ON TOP afterwards and so needs none, and only a plain
            // hairline wants a pixel either side. A fixed 1.0 left a bright
            // seam on the ink skins and shrank the cell on the engraved ones,
            // which is the "subtly different from the inventory" part.
            const float in0 = (sk.engravedCells || Theme::InkChrome()) ? 0.0f : 1.0f;
            const float in1 = sk.engravedCells
                                ? Theme::kGrooveW * Theme::Scale() * 0.5f
                            : Theme::InkChrome() ? 0.0f
                                                 : 1.0f;
            for (const auto& it : cells) {
                if (HeldCell(it.spotKey)) continue;
                for (int y = 0; y < it.h; ++y) {
                    for (int x = 0; x < it.w; ++x) {
                        const int cc = it.col + x, rr = it.row + y;
                        const ImVec2 c0(base.x + cc * cell, base.y + rr * cell);
                        const ImVec2 q0(c0.x + (cc > 0 ? in1 : in0),
                                        c0.y + (rr > 0 ? in1 : in0));
                        const ImVec2 q1(c0.x + cell - (cc + 1 < cols ? in1 : in0),
                                        c0.y + cell - (rr + 1 < rows ? in1 : in0));
                        dl->AddRectFilled(q0, q1, shadeCol);
                    }
                }
            }

            // ★★★The ink skins' lattice, and the reason the partner board had
            // NO cells at all on them: DrawCellLattice deliberately draws
            // nothing under InkChrome -- the marks have to land OVER the
            // occupied ground, not be cleared around by it -- and the second
            // call that actually draws them was only ever made by the player's
            // grid. Same order as there: chrome, ground, THEN the lattice.
            Grid::DrawInkLattice(dl, base, cols, rows);

            // cells
            for (size_t i = 0; i < cells.size(); ++i) {
                auto& it = cells[i];
                // carried FROM here: keep the slot reserved (placement above still
                // counted it, so neighbours don't shift), just don't draw it — the
                // cell reads empty until the trade actually completes.
                if (HeldCell(it.spotKey)) continue;
                const ImVec2 p0(base.x + it.col * cell, base.y + it.row * cell);
                const float bw = it.w * cell, bh = it.h * cell;   // footprint box

                // click target: right-click = TAKE (loot mode). Barter buy is
                // Phase 5. Gold ignores space; other items need a free cell.
                // While carrying, SKIP the cell buttons — they'd swallow the drop
                // click over an occupied cell (drag-to-store must reach the window
                // hover test regardless of what's under the cursor).
                if (!Grid::IsHolding()) {
                    char idbuf[16];
                    std::snprintf(idbuf, sizeof(idbuf), "##pc%zu", i);
                    ImGui::SetCursorScreenPos(p0);
                    ImGui::InvisibleButton(idbuf, ImVec2(bw, bh));
                    if (ImGui::IsItemHovered() && !UIRoot::MouseInOverlay()) {
                        dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                            Theme::Acc(0.10f));
                        Sfx::HoverNote(ImGui::GetItemID());   // partner cell hover
                        // Phase 4: rich tooltip; barter side shows the BUY price
                        int price = -1;
                        if (g_mode == Mode::kBarter && it.value > 0 && !it.obj->IsGold()) {
                            price = BuyPrice(it.obj, it.value);
                        }
                        // D1: read the PARTNER's entry, not the player's —
                        // this is what leaked our own item's extras onto a
                        // merchant's identical ware. kAny: the cell is a
                        // per-form aggregate with a stack badge (by design).
                        // GI13: a per-unit cell must ALWAYS read its own
                        // sub-stack. Falling back to kAny for a plain unit (uid
                        // 0, no list) made every ordinary dagger in the chest
                        // display the tempered one's stats -- kAny means "first
                        // list carrying the trait", which is exactly the bug
                        // this whole refactor exists to kill.
                        // ★(1.3.0) a shelved pouch's tooltip prints its stored
                        // amount -- the icon already asked the layout (below),
                        // but this call passed -1 and the "N / cap G" line
                        // never drew.
                        const int tipGold =
                            GoldCoins::IsPouch(it.obj->GetFormID())
                                ? ShelfGoldOf(it.spotKey) : -1;
                        Grid::DrawItemTooltip(it.obj, it.count, tipGold, price, true,
                                              SourceRef(),
                                              it.perUnit ? Grid::ExtraScope::kUnit
                                                         : Grid::ExtraScope::kAny,
                                              // ★it.sig, not 0. The partner
                                              // side has no uid and no list
                                              // position to offer, so the
                                              // signature is the only handle
                                              // the tooltip can resolve with.
                                              it.uid, it.xlIdx, it.sig, 0,
                                              // ★(1.5.0 audit) the SPOT KEY
                                              // rides along so the bag verb
                                              // can ask whether its window is
                                              // already open (IsShelfBagOpen)
                                              Grid::TileContext{ it.spotKey,
                                                  false, false, true, false });
                        // C: 3D view, same as the player's grid. Vanilla files
                        // Item Zoom under the kItemMenu context, which the
                        // container and barter screens share with the
                        // inventory -- turning a ware over before buying it is
                        // vanilla behaviour, not an extra.
                        if (ImGui::IsKeyPressed(ImGuiKey_C, false) &&
                            !ImGui::GetIO().WantTextInput) {
                            UIRoot::OpenInspect(it.obj, Grid::DefKeyOf(it.obj));
                        }
                    }
                    // Phase 5-B: plain left-click (no shift) picks the item onto the
                    // cursor — drag to the player grid to TAKE (loot) / BUY (barter).
                    // Merchant gold isn't carriable.
                    if (UIRoot::MouseInOverlay()) {
                        // popup chrome overlaps this cell: no click-through
                    } else if (Editor::IsEditMode()) {
                        // ★★EDIT reaches the PARTNER board too. A def belongs to
                        // the FORM, so an item's footprint, icon and rotation are
                        // the same object whether the item is in the pack or on a
                        // merchant's shelf -- and a shelf is the only place many
                        // forms are ever seen. Having to buy something before it
                        // could be sized was an accident of where the click
                        // handler lived, not a rule anyone chose.
                        // ★It PRE-EMPTS the take / buy / lift branches below, the
                        // same contract the player grid keeps: in EDIT a click
                        // selects and does nothing else. Gold included -- the
                        // coin tiles are editable on our own board.
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                            Editor::Select(it.obj, Grid::DefKeyOf(it.obj));
                        }
                    // ★★P2/3-5c: GOLD LIFTS LIKE ANYTHING ELSE ON THIS BOARD.
                    //
                    // The first attempt gave gold a road of its own -- take the
                    // band on click, hand back a pinned purse -- and it was
                    // wrong for a reason the ordinary carry has always known:
                    // a lift is not a TAKE. Picking a coin cell up to move it
                    // one square left inside the chest took the money into the
                    // player's inventory, and dropping it back stored it again,
                    // so the log showed 11 G leaving and returning in a loop
                    // (measured: 663 -> 652 -> 663, once a second).
                    //
                    // On this side of the window gold IS an item -- it is
                    // Gold001 in a container, not the ledger's face -- so it can
                    // ride the same carry as everything else, and the whole
                    // grammar comes with it: drop it back on the partner grid to
                    // rearrange, drop it on your own board to take it.
                    } else if (!ImGui::GetIO().KeyShift &&
                        ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        if (it.locked) {
                            // F6b: worn without Perfect Touch / GI42: unnameable
                            Sfx::FailNote(Lang::T(it.unnameable
                                ? Lang::Str::AmbiguousUnit
                                : Lang::Str::PickpocketBlocked));
                        } else {
                            // GI62c: centred on the cursor, same as a player tile
                            // -- rotation has to spin about the cursor, and that
                            // only holds if the cursor is the item's middle from
                            // the moment it is lifted (-1 = centre).
                            g_actingSpot = it.spotKey;   // GI20: this cell's slot
                            g_carryGlow = it.glow;       // (1.3.2) markers ride along
                            g_carryStolen = it.stolen;   // ★including this one
                            Grid::BeginPartnerCarry(it.obj, it.count, it.value,
                                -1.0f, -1.0f, it.uid, it.xlIdx, it.ord, it.rot);
                        }
                    }
                    // TAKE trigger: right-click (whole move) OR shift+left-click
                    // (explicit split). Either way a stack (>1) opens the quantity
                    // slider first; a single item moves at once.
                    else if (IsLootMode(g_mode)) {
                        const bool rcRaw = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                        // ★(1.3.0) USE MODE -- SkyUI's "Shift = Equip Mode",
                        // spelled in our grammar. There the primary key is E and
                        // Shift changes what E does; here taking is the RIGHT
                        // button, so Shift changes what the right button does.
                        // It cannot collide with shift+LEFT (split): different
                        // button, which is how SkyUI keeps the two apart as well.
                        // kLoot covers container, corpse AND companion, which is
                        // the whole of the intended scope. Pickpocket has its own
                        // block below and is deliberately left out -- using an
                        // item off a living mark is not a thing to offer.
                        const bool useRc = rcRaw && ImGui::GetIO().KeyShift;
                        const bool rc = rcRaw && !useRc;
                        const bool splitLc = ImGui::GetIO().KeyShift &&
                                             ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        // ★(1.3.1) a BAG manages on right-click HERE TOO -- the
                        // player board's grammar (bag right-click is ALWAYS the
                        // window toggle; trade and storage happen by drag only)
                        // now holds on both sides. Taking the shelf bag home is
                        // the DRAG; right-click opens its bundle window.
                        // ★(1.3.2a) and the POUCH manages the same way: its
                        // right-click is the withdraw window over the SLOT's
                        // amount, exactly the player pouch's grammar.
                        if ((rc || splitLc) && GoldCoins::IsPouch(it.obj->GetFormID())) {
                            if (g_shelfPouchSpot == it.spotKey) {
                                g_shelfPouchSpot.clear();
                                Sfx::SelectOff();
                            } else if (!it.spotKey.empty()) {
                                g_shelfPouchSpot = it.spotKey;
                                g_shelfPouchBundle = 0;   // (1.5.x) the cell itself
                                g_shelfPouchForm = it.obj->GetFormID();
                                g_shelfPouchSlider =
                                    (std::max)(1, ShelfGoldOf(it.spotKey) / 2);
                                Sfx::SelectOn();
                            }
                        } else if ((rc || splitLc) && Grid::ResolveDef(it.obj).bag != 0) {
                            const auto sb = std::find_if(g_shelfBags.begin(),
                                g_shelfBags.end(), [&](const ShelfBagWin& a_w) {
                                    return a_w.spot == it.spotKey;
                                });
                            if (sb != g_shelfBags.end()) {
                                g_shelfBags.erase(sb);
                                Sfx::BagClose();
                            } else if (!it.spotKey.empty()) {
                                g_shelfBags.push_back({ it.spotKey,
                                                        it.obj->GetFormID() });
                                Sfx::BagOpen();
                            }
                        // ★★USE MODE lands here -- AFTER the pouch and the bag,
                        // whose right-click already means something of their own
                        // (withdraw, open). Ordering does the exclusion, so there
                        // is no list of special forms to keep in step with them.
                        // ★★★BOOKS ONLY, and the limit is deliberate.
                        //
                        // Consumables went through "take one unit, then use it",
                        // because the engine can only consume from the player's
                        // own inventory. That worked -- and reliably took the
                        // game down on the SECOND potion drunk out of a
                        // container, always in the same place: the icon path,
                        // making a virtual call on an object that is no longer
                        // one.
                        //
                        // Five hypotheses were tested against crash logs and
                        // ruled out: a stale view index, the engine's
                        // loadedModels list, a dead form pointer, typed-bag
                        // routing, and a tile object that was never on the
                        // board. The last one is instructive -- the pointer WAS
                        // valid at the previous rebuild and the memory behind it
                        // was gone by the time it was drawn, which no test that
                        // compares pointers can catch.
                        //
                        // Whatever that is, it is not this feature's to fix, and
                        // a convenience is not worth a reproducible CTD. Books,
                        // notes and spell tomes need no round trip through the
                        // pack at all (a book is read where it lies), never
                        // touched the failing path, and were verified working --
                        // so that is what the mode does. Anything else falls
                        // through to an ordinary take, which is what shift+right
                        // did before this existed.
                        } else if (useRc && it.obj->As<RE::TESObjectBOOK>()) {
                            if (it.locked) {   // GI42: the twin is worn
                                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                            } else if (auto* bk = it.obj->As<RE::TESObjectBOOK>();
                                       bk && !bk->TeachesSpell()) {
                                // ★READ IN PLACE. A book needs no owner to be
                                // read, so taking one first would leave a note
                                // you only meant to glance at sitting in the
                                // pack. A spell tome is NOT this case -- reading
                                // one destroys it, so it has to be ours first.
                                // ★(1.5.x) the SHELF page, not the inventory
                                // read: the engine's Use needs the player's
                                // own copy and raised no page for a book still
                                // in the chest (the 8/24 rework's regression).
                                Grid::RequestShelfBookPage(bk, it.uid, it.sig);
                                // ...and the page offers E-take, the world
                                // book's own grammar
                                NoteShelfBookRead(it.obj, it.uid, it.sig,
                                                  it.spotKey);
                            } else if (auto* sp = it.obj->As<RE::TESObjectBOOK>()
                                                      ->GetSpell();
                                       sp &&
                                       RE::PlayerCharacter::GetSingleton() &&
                                       RE::PlayerCharacter::GetSingleton()
                                           ->HasSpell(sp)) {
                                // ★(1.5.x) a tome whose spell is already known:
                                // nothing to learn, so nothing is taken and
                                // nothing happens (user rule) -- the round trip
                                // used to strand the tome in the pack.
                            } else if (!Grid::CanFitNewItem(it.obj)) {
                                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                            } else {
                                // One unit, taken and then used: the engine can
                                // only consume from the player's own inventory,
                                // so passing through it is the only road there
                                // is, not a shortcut.
                                g_actingSpot = it.spotKey;   // GI20
                                RequestTake(it.obj, 1, it.uid, it.sig, it.worn,
                                            /*useAfter=*/true);
                            }
                        } else if (rc || splitLc) {
                            if (it.locked) {   // GI42
                                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                            } else if (!(it.obj->IsGold() || Grid::CanFitNewItem(it.obj))) {
                                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                            } else if (splitLc && it.count > 1) {
                                // ★shift+left = SPLIT ONTO THE CURSOR, the same
                                // gesture and the same meaning it has on the
                                // player's own board. Right-click still hauls
                                // the whole cell home.
                                g_actingSpot = it.spotKey;   // GI20
                                OpenSlider(it.obj, it.count, XferDir::kShelfSplit,
                                           it.spotKey, it.value, it.uid, it.sig,
                                           it.worn, false, it.xlIdx);
                            } else if (it.obj->IsGold()) {
                                // ★(PLAN_SPACE_AUTHORITY §7, user rule) a
                                // right-click on a GOLD cell hauls THAT cell's
                                // whole amount -- no quantity window. Three
                                // thousands and a remainder are four clicks.
                                // The acting spot pins the clicked cell, so
                                // the amount that leaves is this tile's and no
                                // sibling's; shift+left keeps the split slider
                                // for choosing an amount. No fit gate: gold
                                // occupies no cells (mirror), same exemption
                                // the pickpocket branch states.
                                g_actingSpot = it.spotKey;   // GI20
                                RequestTake(it.obj, it.count, it.uid, it.sig, it.worn);
                            } else {
                                // ★(1.5.x stack flow) THE WHOLE CELL, exactly
                                // as the gold branch above hauls its whole
                                // amount. A stack used to raise the quantity
                                // window here; it does not any more, because a
                                // take only changes WHERE the units are and
                                // one right-click on the tile that arrives
                                // sends them straight back. Shift+left still
                                // splits (the branch above), so choosing an
                                // amount never stopped being possible -- it
                                // stopped being compulsory.
                                g_actingSpot = it.spotKey;   // GI20
                                RequestTakeAll(it.obj, it.count, it.uid, it.sig, it.worn);
                            }
                        }
                    } else if (g_mode == Mode::kPickpocket) {
                        // F6b: right-click / shift+left = ATTEMPT the lift
                        // (stack -> quantity slider). The roll runs on the Tick.
                        const bool rc = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                        const bool splitLc = ImGui::GetIO().KeyShift &&
                                             ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        if (rc || splitLc) {
                            if (it.locked) {
                                Sfx::FailNote(Lang::T(it.unnameable
                                    ? Lang::Str::AmbiguousUnit
                                    : Lang::Str::PickpocketBlocked));
                            } else if (!(it.obj->IsGold() || Grid::CanFitNewItem(it.obj))) {
                                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                            } else if (it.count > 1) {
                                OpenSlider(it.obj, it.count, XferDir::kPickTake, {}, 0, it.uid, it.sig, it.worn);
                            } else {
                                g_actingSpot = it.spotKey;
                                RequestPickTake(it.obj, 1, it.uid, it.sig, it.worn);
                            }
                        }
                    } else if (g_mode == Mode::kBarter) {
                        // BUY: right-click the merchant's item. A stack opens the
                        // quantity slider; a single item buys at once. Blocked when
                        // the player can't afford it or has no room.
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !it.obj->IsGold()) {
                            if (it.locked) {   // GI42: merchant wears the twin
                                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                            } else {
                            g_actingSpot = it.spotKey;   // GI20
                            if (it.count > 1) {
                                OpenSlider(it.obj, it.count, XferDir::kBuy, {}, it.value, it.uid, it.sig);
                            } else {
                                const int total = BuyPrice(it.obj, it.value);
                                if (Grid::GoldAmount() < total) {
                                    Sfx::FailNote(Lang::T(Lang::Str::NotEnoughGold));
                                } else if (!Grid::CanFitNewItem(it.obj)) {
                                    Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                                } else {
                                    RequestBuy(it.obj, 1, total, it.value, it.uid, it.sig);
                                }
                            }
                            }
                        }
                    }
                }

                // GI51: same rule as the player's grid — a chest full of items
                // with no captures yet must still READ as a chest full of
                // items, which is exactly what you need to decide whether to
                // take anything.
                // ★A SHELVED POUCH DRAWS ITS OWN BAND. Without this the chest
                // showed the empty-pouch art for a pouch with 6,000 G in it:
                // the icon came from the bare form, and the amount lived
                // somewhere the shelf could not see.
                RE::TESBoundObject* cellIconObj = it.obj;
                const int shelfGold = GoldCoins::IsPouch(it.obj->GetFormID())
                                          ? ShelfGoldOf(it.spotKey) : 0;
                if (shelfGold > 0) {
                    if (auto* v = GoldCoins::PouchIconObjectFor(shelfGold,
                            GoldCoins::PouchCapOfForm(it.obj->GetFormID()),
                            it.obj->GetFormID())) {
                        cellIconObj = v;
                    }
                }
                const IconCache::Icon* cellIcon = cache->Get(cellIconObj);
                if (!cellIcon) {
                    cache->QueueCapture(cellIconObj);   // first sight — render next frames
                    cellIcon = Fallback::Get(cellIconObj);
                }
                if (const auto* icon = cellIcon) {
                    // contain the icon inside the footprint box, keeping aspect.
                    // ★GI62: measure against the UPRIGHT box. The sprite is not
                    // rotated -- only its draw quad is -- so a turned cell would
                    // otherwise fit a tall sword into a short cell height and
                    // shrink it to a third of its size (user-reported).
                    const bool turned = (it.rot & 1) != 0;
                    const float fitW = turned ? bh : bw;
                    const float fitH = turned ? bw : bh;
                    const float sc = (std::min)(fitW / static_cast<float>(icon->w),
                                                fitH / static_cast<float>(icon->h)) * 0.85f;
                    const float dw = icon->w * sc;
                    const float dh = icon->h * sc;
                    const ImVec2 ctr(p0.x + bw * 0.5f, p0.y + bh * 0.5f);
                    const ImVec2 i0(ctr.x - dw * 0.5f, ctr.y - dh * 0.5f);
                    const ImVec2 i1(ctr.x + dw * 0.5f, ctr.y + dh * 0.5f);
                    // rarity glow UNDER the sprite — same pass as the player grid
                    Grid::DrawGlow(dl, it.obj, it.glow, i0, i1,
                        p0, ImVec2(p0.x + bw, p0.y + bh), it.rot);
                    const bool cellFallback = cellIcon && !cache->Get(cellIconObj);
                    const auto cdef = Grid::ResolveDef(it.obj);
                    // both styles — the partner board draws the same item the
                    // player's board does, and one offset rule serves both
                    const ImVec2 nudge = Grid::RotatedOffset(cdef.fx, cdef.fy, it.rot);
                    const float pdeg = (cellFallback ? cdef.frot : 0.0f) + it.rot * 90.0f;
                    // ★1.0.5: same shadow as the player's board
                    Grid::DrawItemShadow(dl, icon->srv,
                                         ImVec2(ctr.x + nudge.x, ctr.y + nudge.y),
                                         dw, dh, pdeg);
                    UIRoot::DrawItemIconRot(dl, icon->srv,
                        ImVec2(ctr.x + nudge.x, ctr.y + nudge.y), ImVec2(dw, dh), pdeg);
                    // ★Same wash and the same alpha the player's board uses for
                    // a search miss — one search, one look, both windows.
                    if (FindMisses(it.obj)) {
                        dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                            IM_COL32(6, 6, 10, 168));
                    }
                    // ★1.0.5: the shared marker tray, so poison keeps showing
                    // here now that DrawGlow no longer draws it. Favourite
                    // stays off -- a PartnerCell is a container's item, not the
                    // player's, and carries no star.
                    // ★"On the body" joins them rather than being drawn beside
                    // them: it used to own the same corner the tray starts from
                    // and covered the poison droplet outright. Not while LOCKED
                    // -- the lock already means worn and owns that square.
                    // ★★★STOLEN IS ON IN kSteal, and the mode is what says so --
                    // the cell carries no flag of its own because nothing has
                    // been taken yet. An owned container is owned WHOLE, so
                    // every item in it will become stolen goods the moment it
                    // moves, and the mark says that in advance.
                    // This is vanilla's own economy of symbols: the hand it puts
                    // beside an item in a stealable container is the SAME hand
                    // it puts beside that item once it is in your pack. One
                    // mark, one meaning -- "someone else's". Ours is the crimson
                    // dot, and it now works the same way on both sides.
                    // ★A per-item mark rather than the window's STEAL label
                    // alone, because the label did not reach the person who
                    // designed it: the behaviour (goods turning stolen) was
                    // noticed, the label was not.
                    // ★★PER UNIT, not per container. This was `g_mode ==
                    // kSteal` -- everything in an owned chest stamped -- and
                    // that put a crimson dot on the player's OWN sword the
                    // moment they stored it, which reads as "this mod turned my
                    // gear into contraband" (reported from the Embassy quest,
                    // where confiscated gear comes home through such a chest).
                    // Dropping the mark entirely was tried next and lost real
                    // information: an owned container's contents ARE stolen
                    // goods and the board should say so.
                    // pc.stolen answers the actual question -- see where it is
                    // computed.
                    Grid::DrawMarkerTray(dl, p0, ImVec2(p0.x + bw, p0.y + bh),
                                         false, it.stolen,
                                         (it.glow & 0x4) != 0,
                                         it.worn && !it.locked);
                }
                // ★The EDIT selection ring, so the partner board can say WHICH
                // form is being edited -- without it the click has no visible
                // answer and the panel's numbers look like they belong to
                // nothing. A plain rect rather than the player board's mask
                // outline because a partner cell IS a rectangle (no polyomino).
                // ★IsEditMode() FIRST. DefKeyOf builds a string, and this runs
                // once per visible cell every frame -- a merchant's hundred
                // wares was ~12k heap allocations a second to answer a question
                // that is "no" whenever EDIT is closed, which is almost always.
                // Rule 3: no string work on the per-frame path.
                if (Editor::IsEditMode() && Editor::IsSelected(Grid::DefKeyOf(it.obj))) {
                    dl->AddRect(p0, ImVec2(p0.x + bw, p0.y + bh),
                                Theme::Col(Theme::S().sel, 1.0f), 0.0f, 0, 2.0f);
                }
                // GI8: extension overlay (socket wells). ★This is the side that
                // matters most for the socket mod -- the point is to SEE what a
                // chest holds and decide whether to take it, so the badges have
                // to be on the partner cell, not only on our own grid.
                // The partner window draws plain rectangles (no polyomino mask),
                // so the default full-rect shape is correct here.
                {
                    Badges::TileShape shape;
                    shape.w = it.w;
                    shape.h = it.h;
                    auto       pr = g_partner.get();
                    // ★IsMouseHoveringRect is geometry only -- it clips, but it
                    // never asks who is on top, so a badge under another window
                    // still lit up. Cosmetic where the other two were not, but
                    // it is the same missing question, so it gets the same gate.
                    // (The cell's InvisibleButton is not available here: it is
                    // only submitted while nothing is being carried.)
                    const bool hov =
                        UIRoot::CursorOwnsWindow() &&
                        ImGui::IsMouseHoveringRect(
                            p0, ImVec2(p0.x + bw, p0.y + bh), false);
                    Badges::Draw(dl, p0, bw, bh, shape,
                                 pr ? pr->GetFormID() : 0u,
                                 it.obj->GetFormID(), it.uid, hov);
                }

                if (it.count > 1) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", it.count);
                    Grid::DrawCountBadge(dl, p0, buf);
                }

                // GI13: worn-on-body marker. Corpse looting shows the NPC's
                // equipped gear alongside their pack, and there was no way to
                // tell which was which. Accent hairline + a small up-chevron at
                // the bottom-right marker spot (the padlock's slot is only used
                // in pickpocket mode, and a locked cell is worn by definition).
                // F6b: corner grammar — bottom-left success %, colour-coded
                // (green 90+ / yellow 50~89 / red <50); locked worn gear dims
                // with a small padlock at the bottom-right marker spot.
                if (g_mode == Mode::kPickpocket) {
                    if (it.locked) {
                        // greyed out: "and you can't have it"
                        dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                            IM_COL32(0, 0, 0, 90));
                        const float ps = 10.0f * Theme::Scale();
                        const ImVec2 lp(p0.x + bw - ps - 4.0f, p0.y + bh - ps - 4.0f);
                        const ImU32 lc = IM_COL32(220, 200, 150, 230);
                        // body + shackle
                        dl->AddRectFilled(ImVec2(lp.x, lp.y + ps * 0.45f),
                            ImVec2(lp.x + ps, lp.y + ps), lc, 1.5f);
                        dl->AddCircle(ImVec2(lp.x + ps * 0.5f, lp.y + ps * 0.30f),
                            ps * 0.30f, lc, 0, 1.5f);
                    } else if (it.chance >= 0) {
                        const ImU32 cc = it.chance >= 90 ? IM_COL32(120, 205, 110, 255)
                                       : it.chance >= 50 ? IM_COL32(225, 195, 90, 255)
                                                         : IM_COL32(225, 95, 80, 255);
                        char pb[8];
                        std::snprintf(pb, sizeof(pb), "%d%%", it.chance);
                        const float th = ImGui::GetTextLineHeight();
                        const ImVec2 tp(p0.x + 3.0f, p0.y + bh - th - 2.0f);
                        const ImU32 outline = IM_COL32(0, 0, 0, 255);
                        dl->AddText(ImVec2(tp.x - 1, tp.y), outline, pb);
                        dl->AddText(ImVec2(tp.x + 1, tp.y), outline, pb);
                        dl->AddText(ImVec2(tp.x, tp.y - 1), outline, pb);
                        dl->AddText(ImVec2(tp.x, tp.y + 1), outline, pb);
                        dl->AddText(tp, cc, pb);
                    }
                }
            }
            // F7: drop ghost — green = free spot (stores/moves here), red =
            // swap / invalid. Spot-memory modes only (barter auto-packs; a
            // preview would lie).
            // ★★(1.3.2) COMPUTED HERE, not via QueryStoreDrop. That helper
            // refuses unless `g_partnerHovered` is set -- and that flag is
            // published AFTER this function returns, having been cleared at
            // the top of the frame, so the query answered "not on a cell"
            // every single time and the ghost never drew on any container.
            // The anchor formula below is QueryStoreDrop's, verbatim, so
            // preview and result still cannot disagree.
            if (SpotMemoryOn() && Grid::IsHolding() &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                int hw = 1, hh = 1;
                float ox = 0.0f, oy = 0.0f;
                if (Grid::HeldFootprint(hw, hh, ox, oy)) {
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    int gc = static_cast<int>(std::lround((m.x - base.x - ox) / cell));
                    int gr = static_cast<int>(std::lround((m.y - base.y - oy) / cell));
                    gc = (std::max)(0, (std::min)(Grid::kCols - hw, gc));
                    gr = (std::max)(0, (std::min)(rows - hh, gr));
                    int blockers = 0;
                    for (const auto& pc : cells) {
                        if (pc.col < 0 ||
                            HeldCell(pc.spotKey)) {
                            continue;
                        }
                        if (gc < pc.col + pc.w && gc + hw > pc.col &&
                            gr < pc.row + pc.h && gr + hh > pc.row) {
                            ++blockers;
                        }
                    }
                    const ImU32 ghost = blockers == 0 ? IM_COL32(90, 170, 90, 90)
                                                      : IM_COL32(190, 60, 60, 110);
                    const ImVec2 g0(base.x + gc * cell, base.y + gr * cell);
                    dl->AddRectFilled(g0,
                        ImVec2(g0.x + hw * cell, g0.y + hh * cell), ghost);
                }
            }

            // scroll extent = exactly the grid height. The cells were drawn with
            // absolute SetCursorScreenPos, so reset the cursor to `base` before the
            // Dummy — otherwise it stacks onto the last tile's position and the
            // wheel scrolls far past the grid.
            ImGui::SetCursorScreenPos(base);
            // +1 matches the child's +1 so the bottom grid line stays inside the
            // view even when scrolled fully to the end (otherwise it sits exactly
            // on the clip edge and vanishes).
            ImGui::Dummy(ImVec2(gridW + sbW, rows * cell + 1.0f));
        }

        // stage 4: bottom strip — barter = merchant GOLD bar, loot = the
        // R shortcut hint (design pass C/G); runs inside the window
        void DrawPartnerBottomBar(const ImVec2& size, float a_gridW)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            const float insX = Theme::FrameInsetX(), insY = Theme::FrameInsetY();
            // bottom strip (design pass C/G): same divider + label/value grammar
            // as the player window's GOLD bar. Barter = merchant gold; loot = the
            // R shortcut hint (discoverability).
            {
                auto* wdl = ImGui::GetWindowDrawList();
                const ImVec2 wp = ImGui::GetWindowPos();
                const float gy = wp.y + size.y - insY - BottomStripH();
                // ★★The window's content inset is (insX + PadX) -- that is what
                // this window's PushStyleVar(WindowPadding) sets. This strip is
                // drawn straight onto the draw list and so bypassed it, sitting
                // PadX further left than everything else in the window. On the
                // ink skins the frame is a brush stroke rather than a hairline,
                // so "slightly outside the inset" became the label lying ON the
                // border. Same inset as the content it sits under.
                const float pad = Theme::PadX() * S;
                const float x0 = wp.x + insX + pad;
                // ★★★TO THE BOARD'S RIGHT EDGE, NOT THE WINDOW'S. The window is
                // `gridW + sbW` wide, and sbW is the SCROLLBAR GUTTER -- 0 until
                // the partner happens to hold more than 12 rows of goods, 14px
                // after that. Ruling this strip off the window width therefore
                // ran it 14px past the last column and underneath the scrollbar,
                // on merchants only, which is exactly how a footer rule reads as
                // "the scrollbar has a border along its bottom" (user report,
                // twice). A container that does not scroll never showed it,
                // which is what made it look like a scrollbar defect.
                // ★The gutter is CHROME, not content: the strip belongs to the
                // board above it and must be the same width whether or not the
                // wares happen to overflow.
                const float x1 = x0 + a_gridW;
                wdl->AddLine(ImVec2(x0, gy), ImVec2(x1, gy), Theme::Rule());
                // ★★★THE SAME GLYPH STYLE AS THE PLAYER'S GOLD BAR. This strip
                // is the partner's half of one line across the screen, and it
                // was drawn with plain AddText at the default font size while
                // the player's half used Theme::TextOutlined at FontValue().
                // On a skin whose chrome carries outlines, the two halves then
                // wore different type -- same row, same grammar, different
                // lettering. One helper, one size, both sides.
                const float vpx = Theme::FontValue();
                const float ty  = gy + 8.0f * S;
                if (g_mode == Mode::kBarter) {
                    char amt[32];
                    if (g_merchGoldInf) {
                        std::snprintf(amt, sizeof(amt), "\xE2\x88\x9E");   // U+221E
                    } else {
                        std::snprintf(amt, sizeof(amt), "%d", MerchantGold());
                    }
                    char lbl[96];
                    if (sk.diamondLabels) {
                        std::snprintf(lbl, sizeof(lbl), "\xE2\x97\x87 %s",
                            Lang::T(Lang::Str::MerchantGoldLabel));
                    } else {
                        std::snprintf(lbl, sizeof(lbl), "%s",
                            Lang::T(Lang::Str::MerchantGoldLabel));
                    }
                    // ★Chrome at FULL alpha, as the player's label has it. The
                    // 0.72 here was a second opinion about the same token.
                    Theme::TextOutlined(wdl, ImVec2(x0, ty),
                        sk.diamondLabels ? Theme::Col(sk.sel, 1.0f) : Theme::Chrome(1.0f),
                        lbl, vpx);
                    const float amtW = Theme::TrackedSize(amt, vpx, 0.0f).x;
                    Theme::TextOutlined(wdl, ImVec2(x1 - amtW, ty),
                        Theme::GoldCol(), amt, vpx);
                } else if (g_mode == Mode::kPickpocket) {
                    // ★Nothing. The strip exists to say what the R key does or
                    // what the merchant can pay; a pickpocket has neither, and
                    // repeating the window's own title down here said the same
                    // word twice on one screen.
                } else {
                    Theme::TextOutlined(wdl, ImVec2(x0, ty), Theme::Col(sk.inkDim, 1.0f),
                        Lang::T(Lang::Str::HintTakeAll), vpx);
                }
            }
        }

        // stage 5: R = take everything that FITS (loot; window-hover gated)
        void TakeAllShortcut(std::vector<PartnerCell>& cells)
        {
            // R = take everything that FITS (loot). Gold ignores space; other
            // items consume free cells (approximate — ignores fragmentation, but
            // never over-takes). Stops once the grid is full. Gated on THIS window
            // being hovered — the key is global, so an ungated R also fired while
            // the cursor was over the PLAYER window (whose hover+R means "drop
            // one"), taking the whole container by accident (user-reported).
            if (IsLootMode(g_mode) && !g_slider.active &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_R, false) && !ImGui::GetIO().WantTextInput) {
                // total/used already span the bags (open or closed) — adding
                // bag room on top of that double-counted it and over-took
                int free = Grid::SpaceTotal() - Grid::SpaceUsed();
                for (auto& c : cells) {
                    if (c.obj->IsGold()) {
                        RequestTake(c.obj, c.count);   // gold bypasses space
                        continue;
                    }
                    const int span = Grid::CellSpanOf(c.obj);
                    if (span <= free && Grid::CanFitNewItem(c.obj)) {
                        // ★(1.3.0) name the slot so the take retires IT --
                        // pouch gold and bag bundles ride the slot, and the
                        // pool-prune fallback only ran for vanished pools.
                        g_actingSpot = c.spotKey;
                        RequestTake(c.obj, c.count);
                        free -= span;
                    }
                }
            }
        }
    }

    void DrawWindows()
    {
        g_partnerHovered = false;
        g_partnerGridLive = false;   // F7: re-armed below while drawing
        if (g_mode == Mode::kNormal) return;
        auto* partner = Partner();
        auto* source = SourceRef();   // merchant wares vs container/corpse
        if (!partner || !source) return;

        auto cells = CollectPartnerCells(source);        // stage 1
        const int placedRows = PlacePartnerCells(cells); // stage 2

        const char* title = partner->GetDisplayFullName();
        if (!title || !*title) {
            title = g_mode == Mode::kBarter ? "MERCHANT" : "CONTAINER";
        }

        const auto& sk = Theme::S();
        const float S    = Theme::Scale();
        const float cell = Grid::CellPx();
        const int   cols = Grid::kCols;
        // the measurement belongs to ONE skin at ONE scale: both change the
        // title bar's height and the padding, so a stale figure would size
        // the window for the skin the player just left
        {
            static float s_forScale = -1.0f;
            static int   s_forSkin = -1;
            const int    skin = Theme::SkinIndex();
            if (s_forScale != S || s_forSkin != skin) {
                s_forScale = S;
                s_forSkin = skin;
                g_partnerChromeH = -1.0f;
            }
        }
        // F7: spot-memory containers get +2 spare rows of arranging space
        // (fixed width, variable height per spec)
        // ★(1.3.3) a COMPANION's board is exactly its capacity -- 10 x 8, no
        // spare rows, so the pack the player may fill is the pack they see.
        // It still stretches when what is already inside overflows it (see
        // CompanionPartner): the cap governs additions, never visibility.
        const bool  companion = CompanionPartner();
        const int   rows = companion
                               ? (std::max)(kCompanionRows, placedRows)
                               : (std::max)(4, placedRows) + (SpotMemoryOn() ? 2 : 0);
        const int   visRows = (std::min)(rows, 12);   // scroll past 12 rows
        const float gridW = cols * cell;
        // reserve the scrollbar gutter ONLY when the list actually scrolls —
        // otherwise it leaves a permanent lopsided right margin (measured: the
        // right gap was insX + sbW while the left was just insX).
        const float sbW  = (rows > visRows) ? ImGui::GetStyle().ScrollbarSize : 0.0f;
        const float insX = Theme::FrameInsetX(), insY = Theme::FrameInsetY();
        const float barH = 26.0f * S, labelH = 24.0f * S;

        auto* wm = WinManager::GetSingleton();
        // width = grid + scrollbar gutter + symmetric frame insets (no lopsided
        // right margin). The child below is gridW + sbW so the 10 columns stay
        // full and the scrollbar sits in its own gutter, not over the tiles.
        // +30: bottom strip — barter = merchant GOLD bar (mirrors the player
        // window's), loot = shortcut hint line (design pass C/G)
        // ★+2 x PadX. TitleBar starts the content at PadX from the left edge
        // (every managed window does), so a width sized without it runs the
        // last cell column PadX past the right edge, where ImGui clips it.
        // Invisible on skins whose PadX is baked into a frame inset; on SIMPLE
        // (inset 0, PadX 8) the merchant's grid simply lost its right column.
        // ★Same clearance the player's window takes, paid for the same way --
        // the two sit side by side, so a pad on one title line and not the
        // other is visible as a step between them.
        const float topPad = Theme::TitleTopPad();
        // ★★(1.3.3) PadX()/PadY() ARE ALREADY SCALED (12 * g_scale). Multiplying
        // by S again made this window's padding S^2 -- the same fault the title
        // bar had in 1.2.1, invisible at the 1440p reference where S == 1 and
        // a growing gap everywhere else. The height's own 14 * S was a
        // hand-fitted stand-in for that padding and could only agree with it
        // by accident, which is the strip of empty grid under the board.
        // Both halves now name the SAME quantity, so the window is exactly
        // its content: title + label + rows + bottom strip + the padding.
        const float padX = Theme::PadX(), padY = Theme::PadY();
        // The chrome above the board -- title bar, section label, the window's
        // own top padding and the title's clearance -- MEASURED last frame
        // (see the note by EndChild). The first frame of a session uses the
        // old estimate and is corrected on the next one.
        // ★topPad is subtracted because ApplyNext adds it back: the measured
        // height already contains it (it is measured from the window's top).
        const float chromeH = g_partnerChromeH > 0.0f
                                  ? g_partnerChromeH
                                  : barH + labelH + 2.0f * padY;
        const ImVec2 size(gridW + sbW + 2.0f * (insX + padX),
                          chromeH + visRows * cell + 1.0f +
                              BottomStripH() + insY - topPad);
        ImVec2 defPos(120.0f, 200.0f);
        if (auto* m = wm->Find("main"); m && m->posKnown) {
            defPos = ImVec2(m->pos.x - size.x - 12.0f * S, m->pos.y);
        }
        wm->ApplyNext("partner", defPos, size, WinManager::Anchor::kTopLeft, topPad);
        // managed-window rule: WindowPadding must equal the frame inset, else
        // the content starts at the default padding (left) while the width was
        // sized from insX (right) — a lopsided right margin. (See memory note.)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(insX + padX, insY + padY));   // (1.3.3) no second scale
        ImGui::Begin("##gi_partner", nullptr, kManagedWinFlags);
        wm->TitleBar("partner", title);

        // F6a/F6b: red crime chrome — border + label line flip to crimson so
        // an owned container / a living mark is unmistakable.
        static constexpr ImU32 kStealRed = IM_COL32(168, 44, 32, 220);
        // ★★★NO RED WINDOW BORDER. It stroked a square 2px line where the SKIN
        // draws its own frame -- over a brush stroke on the ink skins, over a
        // torn edge on Fable, half-buried under the bevel elsewhere -- so it
        // read as a stray rectangle rather than as chrome. Moving it a
        // frame-inset inward did not help: it was still a second frame drawn in
        // a grammar no skin uses. The warning belongs to the LABEL, which is
        // where a player looks to find out what this window is.
        if (g_mode == Mode::kSteal || g_mode == Mode::kPickpocket) {
            // ★Same style as every other section label -- the skin decides
            // between a crimson diamond and outlined chrome -- with only the
            // COLOUR overridden. Drawing this one by hand is what left it
            // wearing the diamond on skins that had moved on from it.
            const ImVec4 warn = ImGui::ColorConvertU32ToFloat4(kStealRed);
            UIRoot::SectionLabel(Lang::T(g_mode == Mode::kSteal
                                             ? Lang::Str::StealTitle
                                             : Lang::Str::PickpocketTitle),
                                 &warn);
        } else {
            UIRoot::SectionLabel(g_mode == Mode::kLoot ? "CONTENTS" : "WARES");
        }
        // merchant gold moved to the bottom GOLD bar (design pass C) — the
        // player and partner windows now mirror each other's layout

        // (no NoScrollWithMouse — the wheel must scroll the item list)
        // +1px so the right/bottom grid lines (at exactly gridW / rows*cell)
        // aren't clipped away by the child's edge.
        ImGui::BeginChild("##partner_grid", ImVec2(gridW + sbW + 1.0f, visRows * cell + 1.0f),
            ImGuiChildFlags_None, ImGuiWindowFlags_None);
        const ImVec2 base = ImGui::GetCursorScreenPos();

        // ★★The BOARD's screen rect, published in EVERY mode. It used to be set
        // only under SpotMemoryOn because only the drop-cell maths read it --
        // but "is the cursor on the board?" is now the question that decides
        // whether a drop is a sale at all (see g_partnerHovered below), and a
        // merchant has to answer it too.
        g_partnerClipMin = ImGui::GetWindowPos();
        {
            const ImVec2 ws = ImGui::GetWindowSize();
            g_partnerClipMax = ImVec2(g_partnerClipMin.x + ws.x, g_partnerClipMin.y + ws.y);
        }

        // F7: publish this frame's grid geometry + placement for the
        // drop-cell math (QueryStoreDrop) — spot-memory modes only
        if (SpotMemoryOn()) {
            g_partnerGridLive = true;
            g_partnerBase = base;   // scroll-adjusted content origin
            g_partnerRows = rows;
            g_lastCells = cells;
        }

        DrawPartnerCells(cells, rows, gridW, sbW, base); // stage 3
        // ★★★TRIED AND REVERTED: a board-edge AddRect here, copied from
        // DrawGridChrome. It was never needed. Both lattices already close
        // their own boundary -- the hairline pass runs `c <= cols` / `r <=
        // rows` so the first and last lines ARE the edge, and the ink pass
        // strokes all four sides after its inner rules. The missing edge that
        // started this was the ink lattice not being called at all (fixed in
        // DrawPartnerCells), not a missing frame.
        // The rect only added faults of its own: anchored inside the child it
        // scrolled with the goods, and pulled out here it boxed in the
        // scrollbar -- a line under the bar on a merchant, which is what the
        // board's own edge never draws.
        ImGui::EndChild();

        // ★★(1.3.3) MEASURE THE CHROME, DO NOT GUESS IT. The bottom strip is
        // drawn at an ABSOLUTE offset from the window's bottom edge, so any
        // disagreement between the requested height and what the title, the
        // section label and the padding actually consume shows up as a band
        // of empty grid between the board and that strip -- which is what it
        // was. Three hand-fitted constants could only ever agree with the
        // real layout by luck, and did not on this skin/scale.
        // ★What is cached is the part that does NOT depend on the row count,
        // so it converges once and then holds: a value that never changes
        // cannot oscillate the window's size (the readback trap in
        // reference_imgui_pixel_traps).
        {
            const float used = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y;
            const float measured = std::round(used - (visRows * cell + 1.0f));
            if (measured > 0.0f && std::abs(measured - g_partnerChromeH) > 0.5f) {
                g_partnerChromeH = measured;
            }
        }

        DrawPartnerBottomBar(size, gridW);               // stage 4
        TakeAllShortcut(cells);                          // stage 5

        // record hover for the drag-to-store drop test (whole window + child)
        // ★AllowWhenBlockedByActiveItem, matching the player grid's gate. The
        // drop CLICK activates whatever ImGui item sits under the cursor, and
        // without this flag the window that owns it reports "not hovered" on
        // the one frame the drop is resolved. RootAndChildWindows because the
        // goods live in a scrolling child.
        //
        // ★★★...WHICH IS WHY THIS IS THE BOARD, NOT THE WINDOW. Every consumer
        // of this flag asks a DROP question (the three kPartner* routes, the
        // in-container rearrange, and two negative guards on player-grid drops),
        // and the contract they were written against is stated at the rearrange:
        // "Chrome (titlebar / bottom bar) cancels back to its spot."
        // The title bar used to honour it by accident -- its drag strip owned
        // ActiveId, so a plain IsWindowHovered went false there. Adding the flag
        // above took that accident away and a drop on the title bar SOLD the
        // item instead of cancelling. The rect test restores the contract on
        // purpose rather than by side effect.
        // ★MouseInOverlay for the other half: an overlay's chrome is drawn WIDER
        // than its ImGui rect, so z-order alone leaves a ~14px band where a
        // pickup is refused but a drop went through -- the same asymmetry the
        // player grid had, fixed there this release and still here.
        {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const bool onBoard = m.x >= g_partnerClipMin.x && m.x < g_partnerClipMax.x &&
                                 m.y >= g_partnerClipMin.y && m.y < g_partnerClipMax.y;
            g_partnerHovered =
                onBoard &&
                UIRoot::CursorOwnsWindow(ImGuiHoveredFlags_RootAndChildWindows);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ---- F7: container spot memory — public surface ----

    // ★(1.3.3) reads g_lastCells -- this frame's partner placement -- so it
    // lives down here with the other consumers of it.
    bool PartnerHasRoomFor(RE::TESBoundObject* a_obj, int a_count)
    {
        // only a living follower is bounded; chests, corpses and merchants
        // answer yes as they always have
        if (!a_obj || !CompanionPartner()) return true;
        // gold is weightless bookkeeping on both sides, and a stack that
        // MERGES needs no cell of its own
        if (a_obj->IsGold()) return true;
        const int cap = Grid::StackCap(a_obj);
        if (cap > 1) {
            int loose = 0;
            for (const auto& c : g_lastCells) {
                if (c.obj == a_obj) loose += c.count;
            }
            if (loose > 0 && (loose % cap) != 0 &&
                a_count <= cap - (loose % cap)) {
                return true;   // room inside a stack already on the shelf
            }
        }
        // ...otherwise it needs a free rect inside the 10 x kCompanionRows
        // pack, measured against the board the player is looking at.
        const auto d = Grid::ResolveDef(a_obj);
        const int w = (std::max)(1, d.w), h = (std::max)(1, d.h);
        const int cols = Grid::kCols;
        std::vector<char> occ(static_cast<std::size_t>(cols) * kCompanionRows, 0);
        for (const auto& c : g_lastCells) {
            if (c.col < 0) continue;
            for (int y = 0; y < c.h; ++y) {
                const int rr = c.row + y;
                if (rr < 0 || rr >= kCompanionRows) continue;
                for (int x = 0; x < c.w; ++x) {
                    const int cc = c.col + x;
                    if (cc < 0 || cc >= cols) continue;
                    occ[static_cast<std::size_t>(rr) * cols + cc] = 1;
                }
            }
        }
        for (int r = 0; r + h <= kCompanionRows; ++r) {
            for (int c = 0; c + w <= cols; ++c) {
                bool free = true;
                for (int y = 0; y < h && free; ++y) {
                    for (int x = 0; x < w; ++x) {
                        if (occ[static_cast<std::size_t>(r + y) * cols + c + x]) {
                            free = false;
                            break;
                        }
                    }
                }
                if (free) return true;
            }
        }
        return false;
    }

    StoreDrop QueryStoreDrop()
    {
        StoreDrop d;
        if (!g_partnerGridLive) return d;
        // ★★★The MIRROR of the player grid's fault, and it was here too: the
        // clip test below is pure geometry, so a bag window dragged on TOP of
        // the merchant still let a drop in the overlap land in the merchant's
        // board underneath. `g_partnerGridLive` says the board was drawn this
        // frame, not that it is the thing under the cursor.
        // ★This one cannot ask ImGui itself -- it is queried at drop time, long
        // after the partner's Begin/End scope closed -- so it reads the answer
        // the window recorded while it WAS current.
        if (!g_partnerHovered) return d;
        int hw = 1, hh = 1;
        float offX = 0.0f, offY = 0.0f;
        if (!Grid::HeldFootprint(hw, hh, offX, offY)) return d;
        const ImVec2 m = ImGui::GetIO().MousePos;
        // must be inside the VISIBLE grid child (the bottom bar / titlebar
        // share row coordinates once scrolled)
        if (m.x < g_partnerClipMin.x || m.x >= g_partnerClipMax.x ||
            m.y < g_partnerClipMin.y || m.y >= g_partnerClipMax.y) {
            return d;
        }
        // anchor = footprint top-left from the GRAB point, rounded and
        // clamped to the board — the player grid's C2 formula verbatim
        const float cell = Grid::CellPx();
        int c = static_cast<int>(std::lround((m.x - g_partnerBase.x - offX) / cell));
        int r = static_cast<int>(std::lround((m.y - g_partnerBase.y - offY) / cell));
        c = (std::max)(0, (std::min)(Grid::kCols - hw, c));
        r = (std::max)(0, (std::min)(g_partnerRows - hh, r));
        d.onCell = true;
        d.col = c;
        d.row = r;
        // footprint-overlap blockers (partner cells are plain rectangles):
        // none = free spot, exactly one = swap partner, 2+ = invalid
        int blockers = 0;
        for (const auto& pc : g_lastCells) {
            if (pc.col < 0 || HeldCell(pc.spotKey)) continue;
            if (c < pc.col + pc.w && c + hw > pc.col &&
                r < pc.row + pc.h && r + hh > pc.row) {
                if (++blockers == 1) {
                    d.occ = pc.obj;
                    d.occCount = pc.count;
                    d.occValue = pc.value;
                    d.occCol = pc.col;
                    d.occRow = pc.row;
                    d.occUid = pc.uid;        // GI24
                    d.occXlIdx = pc.xlIdx;
                    d.occOrd = pc.ord;
                    d.occSpotKey = pc.spotKey;
                    d.occRot = pc.rot;        // GI62
                }
            }
        }
        if (blockers == 0) {
            d.freeSpot = hw <= Grid::kCols && hh <= g_partnerRows;
        } else if (blockers > 1) {
            d.occ = nullptr;   // 2+ blockers: neither free nor a swap
        }
        return d;
    }

    void NoteCarriedSpot(const std::string& a_spotKey)
    {
        g_actingSpot = a_spotKey;   // GI24: a swap starts a carry we did not click
    }

    // ---- THE WRITES -------------------------------------------------------
    //
    // ★★★Everything here used to be a NOTE: a claim pushed onto a pending
    // queue, describing where an item ought to end up once the engine got round
    // to moving it, for a matcher to try to honour some frames later. The
    // container owns its cells now, so a move is a move.

    bool MoveHeldCell(int a_col, int a_row, int a_rot)
    {
        auto* cl = BoardFor();
        if (!cl || g_actingSpot.empty()) return false;
        const auto it = cl->cells.find(g_actingSpot);
        if (it == cl->cells.end()) return false;
        if (((a_rot ^ it->second.rot) & 1) != 0) std::swap(it->second.w, it->second.h);
        it->second.col = a_col;
        it->second.row = a_row;
        it->second.rot = a_rot & 3;
        g_actingSpot.clear();
        return true;
    }

    bool SwapHeldCellWith(const std::string& a_otherKey)
    {
        auto* cl = BoardFor();
        if (!cl || g_actingSpot.empty() || a_otherKey == g_actingSpot) return false;
        const auto mine = cl->cells.find(g_actingSpot);
        const auto other = cl->cells.find(a_otherKey);
        if (mine == cl->cells.end() || other == cl->cells.end()) return false;
        std::swap(mine->second.col, other->second.col);
        std::swap(mine->second.row, other->second.row);
        g_actingSpot.clear();
        return true;
    }

    int MergeHeldCellInto(const std::string& a_otherKey)
    {
        auto* cl = BoardFor();
        if (!cl || g_actingSpot.empty() || a_otherKey == g_actingSpot) return -1;
        const auto mine = cl->cells.find(g_actingSpot);
        const auto other = cl->cells.find(a_otherKey);
        if (mine == cl->cells.end() || other == cl->cells.end()) return -1;
        if (PoolOf(mine->second) != PoolOf(other->second)) return -1;
        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(mine->second.form);
        const int cap = obj ? (std::max)(1, Grid::StackCap(obj)) : 1;
        const int room = cap - other->second.count;
        if (room <= 0) return -1;
        const int moved = (std::min)(room, mine->second.count);
        other->second.count += moved;
        mine->second.count -= moved;
        const int left = mine->second.count;
        if (left <= 0) {
            cl->cells.erase(mine);
            g_actingSpot.clear();
        }
        return left;
    }

    // ★Split a shelf cell in two and hand the smaller half to the cursor. The
    // engine is untouched: both halves are still in the container, which is
    // exactly why this can be plain cell surgery. The pouch's gold and a bag's
    // bundle deliberately do NOT come along -- neither form can split (cap 1),
    // so reaching here with one would be a bug, and copying them would be a
    // duplication bug on top of it.
    std::string SplitShelfCell(const std::string& a_key, int a_count)
    {
        auto* cl = BoardFor();
        if (!cl || a_count <= 0) return {};
        const auto it = cl->cells.find(a_key);
        if (it == cl->cells.end() || it->second.count <= a_count) return {};
        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(it->second.form);
        if (!obj) return {};
        ContCell nc = it->second;
        nc.count = a_count;
        nc.col = -1;   // unplaced: it is on the cursor, not on the shelf
        nc.row = -1;
        nc.gold = 0;
        nc.bundle.clear();
        nc.bagId = 0;   // and it is not the same bag -- see above, it is not one
        it->second.count -= a_count;
        const std::string k = MintCellKey(*cl, obj);
        cl->cells[k] = std::move(nc);
        return k;
    }

    void NoteStoredUnits(RE::TESBoundObject* a_obj, int a_count,
                         std::uint16_t a_uid, std::uint16_t a_sig)
    {
        NoteIn(a_obj, a_uid, a_sig, a_count);
    }

    void PlaceStoredCell(RE::TESBoundObject* a_obj, int a_count,
                         int a_col, int a_row, int a_rot,
                         std::uint16_t a_uid, std::uint16_t a_sig)
    {
        auto* cl = BoardFor();
        if (!cl || !a_obj || a_count <= 0) return;
        const auto def = Grid::ResolveDef(a_obj);
        const bool turned = (a_rot & 1) != 0;
        ContCell c;
        c.form = a_obj->GetFormID();
        c.uid = a_uid;
        c.sig = a_sig;
        c.count = a_count;
        c.col = a_col;
        c.row = a_row;
        c.rot = a_rot & 3;
        c.w = turned ? (std::max)(1, def.h) : (std::max)(1, def.w);
        c.h = turned ? (std::max)(1, def.w) : (std::max)(1, def.h);
        // ★A bag brings its bundle and a pouch its gold, at the one door a
        // shelf cell is born through now. There used to be three doors and they
        // disagreed about which of them claimed the amount.
        c.bundle = TakePendingBundle(c.form);
        if (GoldCoins::IsPouch(c.form)) {
            c.gold = GoldCoins::TakeAwayGold(c.form);
            // nothing parked yet: the pouch has not left the player. Wait for it.
            if (c.gold <= 0) c.awaitGold = 8;
        }
        const std::string key = MintCellKey(*cl, a_obj);
        // ★the windows follow the bag onto its new cell (see g_bagToCellSpot),
        // and the cell takes over the name the entry was carrying -- which is
        // what keeps the window's key, and so its position, unchanged
        if (!g_bagToCellSpot.empty()) {
            c.bagId = g_bagToCellId;
            for (auto& w : g_shelfBags) {
                if (w.spot != g_bagToCellSpot) continue;
                if (w.bag == g_bagToCellId) {
                    w.spot = key;   // the bag itself: it IS the cell now
                    w.bag = 0;
                } else if (std::any_of(c.bundle.begin(), c.bundle.end(),
                               [&](const BundleItem& b) { return b.id == w.bag; })) {
                    w.spot = key;   // something inside it: same name, new cell
                }
            }
            g_bagToCellSpot.clear();
            g_bagToCellId = 0;
        }
        cl->cells[key] = std::move(c);
    }

    // ---- F7: cosave 'GCLY' v1 ----
    // [u32 containers]{ u32 refID, u32 stamp, u32 spots,
    //                   { str key, i32 col, i32 row, i32 w, i32 h } }
    namespace
    {
        constexpr std::uint32_t kContMaxStr = 512;
        constexpr std::uint32_t kContMaxEntries = 65536;
        constexpr std::uint32_t kContCosaveVersion = 15;   // ★v15: a bundled pouch's gold   // v14: the deposit ledger   // ★v13: a bag CELL's name   // ★v12: a bundle entry's NAME (id + parent id, replacing the index)   // v11: a bundle entry's parent (nested bags)   // v2: per-spot rotation  v3: a stored pouch's gold  v4: a stored bag's bundle  v5: bundle anchors  v6: bundle rotation  v7: bundle markers  v8: bundle stolen flag  v9: the spot's binding hints  v10: the cell owns form + count + xlIdx

        // ★v9 migration: before this, a spot's binding lived inside its KEY
        // ("form~B825!worn#1"). Read it back out and put it where it belongs.
        // The key itself is left alone -- nothing recomputes it any more, so an
        // old one is simply an opaque spot name.
        void SpotHintsFromKey(const std::string& a_key, std::uint16_t& a_uid,
                              std::uint16_t& a_sig, bool& a_worn)
        {
            const auto bar = a_key.find('|');
            const auto from = bar == std::string::npos ? 0 : bar + 1;
            a_worn = a_key.find("!worn", from) != std::string::npos;
            const auto at = a_key.find('@', from);
            if (at != std::string::npos) {
                a_uid = static_cast<std::uint16_t>(
                    std::strtoul(a_key.c_str() + at + 1, nullptr, 16));
                return;
            }
            const auto tilde = a_key.find('~', from);
            if (tilde != std::string::npos) {
                a_sig = static_cast<std::uint16_t>(
                    std::strtoul(a_key.c_str() + tilde + 1, nullptr, 16));
            }
        }

        // ★★v<=9 MIGRATION, the other half. A cell's FORM used to live inside
        // its key ("Skyrim.esm|0x01397D#2") because the key was the cell's
        // identity; it is a field now, and this is the one place that still
        // reads the old spelling. Nothing else parses a key any more.
        RE::FormID FormFromSpotKey(const std::string& a_key)
        {
            const auto bar = a_key.find('|');
            if (bar == std::string::npos) return 0;
            const std::string file = a_key.substr(0, bar);
            const auto cut = a_key.find_first_of("@~#!", bar + 1);
            const std::string idPart = cut == std::string::npos
                                           ? a_key.substr(bar + 1)
                                           : a_key.substr(bar + 1, cut - bar - 1);
            const auto local = static_cast<RE::FormID>(
                std::strtoul(idPart.c_str(), nullptr, 16));
            if (file == "Dynamic") {
                // a runtime form: the whole id was written, and it is not stable
                // across a load -- the cell simply dies on the next reconcile
                return 0;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupFormID(local, file) : 0;
        }

        bool ContWriteStr(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const auto len = static_cast<std::uint32_t>(a_s.size());
            if (!a_intfc->WriteRecordData(len)) return false;
            return len == 0 || a_intfc->WriteRecordData(a_s.data(), len);
        }

        bool ContReadStr(SKSE::SerializationInterface* a_intfc, std::string& a_out)
        {
            std::uint32_t len = 0;
            if (!a_intfc->ReadRecordData(len) || len > kContMaxStr) return false;
            a_out.assign(len, '\0');
            return len == 0 || a_intfc->ReadRecordData(a_out.data(), len);
        }
    }

    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kContRecordType, kContCosaveVersion)) {
            SKSE::log::error("[LOOT] cosave: OpenRecord GCLY failed");
            return;
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_contLayouts.size()));
        for (const auto& [refID, cl] : g_contLayouts) {
            a_intfc->WriteRecordData(refID);
            a_intfc->WriteRecordData(cl.stamp);
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(cl.cells.size()));
            for (const auto& [key, s] : cl.cells) {
                ContWriteStr(a_intfc, key);
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.col));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.row));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.w));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.h));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.rot));   // v2
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.gold));  // v3
                // v4: the stored bag's bundle (raw FormIDs; resolved on load)
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.bundle.size()));
                for (const auto& b : s.bundle) {
                    a_intfc->WriteRecordData(b.form);
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.count));
                    a_intfc->WriteRecordData(b.sig);
                    // v5: the entry's anchor inside the bag window
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.col));
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.row));
                    // v6: its quarter-turns
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.rot));
                    // v7: its marker bits
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.glow));
                    // v8: someone else's goods
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.stolen ? 1 : 0));
                    // ★v12: the entry's NAME, and the name of the bag it is
                    // in. v11 wrote an index into this same vector -- see the
                    // load for the one-pass migration off it.
                    a_intfc->WriteRecordData(b.id);
                    a_intfc->WriteRecordData(b.parent);
                    // ★v15: a bundled pouch's gold (-1 = never claimed)
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.gold));
                }
                // ★v9: which unit this spot is showing. A hint, not a name:
                // a stale one only weakens the next match, and the fallback
                // keeps the spot (and the gold and the bundle riding on it).
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.uid));
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.sig));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.worn ? 1 : 0));
                // ★v10: WHAT the cell is, no longer inferable from its key.
                // The form is written raw and resolved on load like any other,
                // and the count is the field the whole board turns on.
                a_intfc->WriteRecordData(s.form);
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.count));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.xlIdx));
                // ★v13: if this cell is a bag, the bag's own name
                a_intfc->WriteRecordData(s.bagId);
            }
            // ★v14: the deposit ledger. Measured to matter across a save/load:
            // vanilla still treats a stored item as yours after one, and the
            // Embassy quest hands your gear back a whole party later.
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(cl.deposits.size()));
            for (const auto& [form, n] : cl.deposits) {
                a_intfc->WriteRecordData(form);
                a_intfc->WriteRecordData(static_cast<std::int32_t>(n));
            }
        }
        SKSE::log::info("[LOOT] cosave: saved {} container layouts", g_contLayouts.size());
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version < 1 || a_version > kContCosaveVersion) {
            SKSE::log::warn("[LOOT] cosave: unsupported container record v{} (max v{}) — skipped",
                            a_version, kContCosaveVersion);
            return;
        }
        std::unordered_map<RE::FormID, ContLayout> loaded;
        std::uint32_t nCont = 0;
        if (!a_intfc->ReadRecordData(nCont) || nCont > kContMaxEntries) return;
        std::uint32_t maxStamp = 0;
        for (std::uint32_t i = 0; i < nCont; ++i) {
            RE::FormID refID = 0;
            std::uint32_t stamp = 0, nSpots = 0;
            if (!a_intfc->ReadRecordData(refID) || !a_intfc->ReadRecordData(stamp) ||
                !a_intfc->ReadRecordData(nSpots) || nSpots > kContMaxEntries) {
                return;   // truncated: bail clean (partial data stays unused)
            }
            // load-order shift: resolve the ref id; unresolvable containers
            // are PRUNED (their spots are read past regardless)
            RE::FormID resolved = 0;
            const bool ok = a_intfc->ResolveFormID(refID, resolved);
            ContLayout cl;
            cl.stamp = stamp;
            for (std::uint32_t j = 0; j < nSpots; ++j) {
                std::string key;
                std::int32_t col = 0, row = 0, w = 1, h = 1, rot = 0;
                if (!ContReadStr(a_intfc, key) || !a_intfc->ReadRecordData(col) ||
                    !a_intfc->ReadRecordData(row) || !a_intfc->ReadRecordData(w) ||
                    !a_intfc->ReadRecordData(h)) {
                    return;
                }
                if (a_version >= 2) {   // GI62 (older saves: everything upright)
                    if (!a_intfc->ReadRecordData(rot)) return;
                }
                std::int32_t gold = 0;
                if (a_version >= 3) {   // a stored pouch's own gold
                    if (!a_intfc->ReadRecordData(gold)) return;
                }
                std::vector<BundleItem> bundle;
                if (a_version >= 4) {   // a stored bag's bundle
                    std::uint32_t nb = 0;
                    if (!a_intfc->ReadRecordData(nb) || nb > kContMaxEntries) return;
                    bundle.reserve(nb);
                    // ★v<=11 migration: the id given to the entry at each
                    // ORIGINAL index, filled as the entries are read. A v11
                    // parent always pointed BACKWARDS (the manifest was written
                    // parent-first), so by the time one is read its target is
                    // already in this table -- one forward pass, no sorting.
                    // Dropped entries still take a slot, or the table would
                    // shift under the very indices it exists to translate.
                    std::vector<std::uint32_t> idOf;
                    for (std::uint32_t bi = 0; bi < nb; ++bi) {
                        RE::FormID    f = 0;
                        std::int32_t  bc = 0;
                        std::uint16_t bs = 0;
                        if (!a_intfc->ReadRecordData(f) || !a_intfc->ReadRecordData(bc) ||
                            !a_intfc->ReadRecordData(bs)) {
                            return;
                        }
                        std::int32_t bcol = -1, brow = -1, brot = 0;
                        if (a_version >= 5) {   // bundle anchors
                            if (!a_intfc->ReadRecordData(bcol) ||
                                !a_intfc->ReadRecordData(brow)) {
                                return;
                            }
                        }
                        if (a_version >= 6) {   // bundle rotation
                            if (!a_intfc->ReadRecordData(brot)) return;
                        }
                        std::int32_t bglow = 0;
                        if (a_version >= 7) {   // bundle markers
                            if (!a_intfc->ReadRecordData(bglow)) return;
                        }
                        std::int32_t bstolen = 0;
                        if (a_version >= 8) {   // someone else's goods
                            if (!a_intfc->ReadRecordData(bstolen)) return;
                        }
                        // ★v12: names. v11 wrote an index; anything older
                        // had no nesting at all, and "directly in the stored
                        // bag" is what 0 means -- so for those the default IS
                        // the migration.
                        std::uint32_t bid = 0, bparent = 0;
                        if (a_version >= 12) {
                            if (!a_intfc->ReadRecordData(bid) ||
                                !a_intfc->ReadRecordData(bparent)) {
                                return;
                            }
                        } else if (a_version >= 11) {
                            std::int32_t oldParent = -1;
                            if (!a_intfc->ReadRecordData(oldParent)) return;
                            if (oldParent >= 0 &&
                                oldParent < static_cast<std::int32_t>(idOf.size())) {
                                bparent = idOf[static_cast<std::size_t>(oldParent)];
                            }
                        }
                        if (bid == 0) bid = NextBundleId();
                        idOf.push_back(bid);
                        // ★v15: the entry's gold. Read BEFORE the resolve
                        // branch so a dropped entry still consumes its bytes.
                        std::int32_t bgold = -1;
                        if (a_version >= 15) {
                            if (!a_intfc->ReadRecordData(bgold)) return;
                        }
                        // load-order shift: unresolvable contents are dropped
                        // (their engine items simply stay visible on the shelf)
                        RE::FormID rf = 0;
                        if (a_intfc->ResolveFormID(f, rf) && rf != 0 && bc > 0) {
                            BundleItem bi{ rf, bc, bs, bcol, brow, brot & 3,
                                           static_cast<std::uint8_t>(bglow),
                                           bstolen != 0, bparent };
                            bi.id = bid;
                            bi.gold = bgold;
                            // ★v<15 migration (and any unclaimed entry): its
                            // parcel is still on the away list -- re-arm the
                            // claim grace so the reconcile marries them.
                            if (bi.gold < 0 && GoldCoins::IsPouch(rf)) {
                                bi.awaitGold = 8;
                            }
                            bundle.push_back(std::move(bi));
                        }
                    }
                }
                ContCell sp{};
                sp.col = col;
                sp.row = row;
                sp.w = w;
                sp.h = h;
                sp.rot = rot & 3;
                sp.gold = gold;
                sp.bundle = std::move(bundle);
                if (a_version >= 9) {
                    std::uint32_t su = 0, ss = 0;
                    std::int32_t  sw = 0;
                    if (!a_intfc->ReadRecordData(su) || !a_intfc->ReadRecordData(ss) ||
                        !a_intfc->ReadRecordData(sw)) {
                        return;
                    }
                    sp.uid  = static_cast<std::uint16_t>(su);
                    sp.sig  = static_cast<std::uint16_t>(ss);
                    sp.worn = sw != 0;
                } else {
                    SpotHintsFromKey(key, sp.uid, sp.sig, sp.worn);
                }
                if (a_version >= 10) {
                    RE::FormID   rawForm = 0;
                    std::int32_t sc = 0, sx = -1;
                    if (!a_intfc->ReadRecordData(rawForm) ||
                        !a_intfc->ReadRecordData(sc) || !a_intfc->ReadRecordData(sx)) {
                        return;
                    }
                    RE::FormID rf = 0;
                    if (a_intfc->ResolveFormID(rawForm, rf)) sp.form = rf;
                    sp.count = sc > 0 ? sc : 0;
                    sp.xlIdx = sx;
                    // ★v13: a save from before this simply has not named its
                    // bag cells yet, and 0 says exactly that -- the first time
                    // one is opened it gets a name, which is where they all
                    // come from anyway.
                    if (a_version >= 13) {
                        if (!a_intfc->ReadRecordData(sp.bagId)) return;
                    }
                } else {
                    // ★★v<=9 MIGRATION. The form used to live inside the key and
                    // the count did not exist anywhere. Parse the one, leave the
                    // other at 0 -- which reads as UNSPECIFIED, not empty, and
                    // the first reconcile pours the container's real contents
                    // into these cells in the positions the player left them.
                    // (See ReconcileContainer: deletion runs after the fill, and
                    // that ordering is the entire migration.)
                    sp.form = FormFromSpotKey(key);
                }
                cl.cells[std::move(key)] = std::move(sp);
            }
            // ★v14: the deposit ledger. Older saves simply have none -- their
            // already-stored goods read as the container's, and anything put
            // in from now on is recorded properly.
            if (a_version >= 14) {
                std::uint32_t nDep = 0;
                if (!a_intfc->ReadRecordData(nDep) || nDep > kContMaxEntries) return;
                for (std::uint32_t k = 0; k < nDep; ++k) {
                    RE::FormID   rawForm = 0;
                    std::int32_t n = 0;
                    if (!a_intfc->ReadRecordData(rawForm) ||
                        !a_intfc->ReadRecordData(n)) {
                        return;
                    }
                    RE::FormID depForm = 0;
                    // Load-order shift: an unresolvable form is dropped, the
                    // same as an unresolvable container.
                    if (a_intfc->ResolveFormID(rawForm, depForm) && depForm != 0 &&
                        n > 0) {
                        cl.deposits[depForm] = n;
                    }
                }
            }
            if (ok && resolved != 0) {
                maxStamp = (std::max)(maxStamp, stamp);
                loaded[resolved] = std::move(cl);
            }
        }
        g_contLayouts = std::move(loaded);
        g_contStamp = maxStamp;
        // ★the mint resumes past everything this save already named, which is
        // why the counter itself never needs to be written down
        for (const auto& [rid, cl2] : g_contLayouts) {
            for (const auto& [k2, c2] : cl2.cells) {
                g_nextBundleId = (std::max)(g_nextBundleId, c2.bagId + 1);
                for (const auto& b2 : c2.bundle) {
                    g_nextBundleId = (std::max)(g_nextBundleId, b2.id + 1);
                }
            }
        }
        SKSE::log::info("[LOOT] cosave: loaded {} container layouts", g_contLayouts.size());
    }

    void RevertGame()
    {
        g_contLayouts.clear();
        g_barterBoard.cells.clear();   // a shop shelf never outlives its visit
        g_in.clear();                  // promises from the previous save
        g_contStamp = 0;
        g_pendingBundles.clear();   // (1.3.0-D) bundles from the previous save
        g_incomingBundles.clear();
        g_shelfBags.clear();        // windows onto a board that no longer exists
        g_bagToCellSpot.clear();
        g_bagToCellId = 0;
        g_nextBundleId = 1;         // names belong to the save that minted them
        g_partnerGridLive = false;
        g_lastCells.clear();
    }
}
