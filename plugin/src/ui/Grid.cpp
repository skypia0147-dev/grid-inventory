#include "ui/Badges.h"
#include "ui/Editor.h"
#include "ui/Equip.h"
#include "ui/Fallback.h"
#include "ui/Grid.h"
#include "game/Ledger.h"
#include "ui/IconCache.h"
#include "ui/ItemPreview.h"
#include "ui/Lang.h"
#include "ui/LootBarter.h"
#include "ui/Sfx.h"
#include "ui/Wheeler.h"
#include "game/BagFilter.h"
#include "game/Census.h"
#include "game/Costume.h"
#include "game/WornLedger.h"
#include "game/DualRing.h"
#include "game/GoldCoins.h"
#include "ui/Loadout.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>   // strcmp: the carry-exclusion fallback names its reason
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// B-4: tetris grids (main + bag windows). Placement is a 1:1 port of the JS
// implementation in view/GridInventory/index.html (maskOf / placeItems:
// saved-spot pass with grid seniority, then first-fit; bag overflow falls
// back to main), layout ini format unchanged (PLAN_B §2-J).

namespace FUI::Grid
{
    namespace
    {
        constexpr const char* kLayoutPath = "Data/SKSE/Plugins/GridInventory_layout.ini";

        struct Mask
        {
            std::vector<std::vector<bool>> rows;
            int w = 1;
            int h = 1;
        };

        struct Item
        {
            std::string         key;
            RE::TESBoundObject* obj = nullptr;
            int                 count = 1;
            GridDef             def;
            Mask                mask;
            std::string         inBag;      // "" = main grid
            int                 col = -1;   // -1 = no saved position
            int                 row = -1;
            bool                fixed = false;
            bool                overflow = false;
            bool                fav = false;   // vanilla favorite flag (Q menu)
            // bit1 enchanted, bit2 unique (DESC) -> rarity halo (bits 1|2 only)
            // bit4 poisoned                      -> top-right droplet marker
            std::uint8_t        glow = 0;
            int                 coinValue = -1;   // G2/G4: gold value of a coin tile (-1 = not a coin)
            // GI1: which engine sub-stack this tile shows.
            //   uid   = ExtraUniqueID, 0 when the list has none (or none at all)
            //   xlIdx = position in entry->extraLists, -1 = plain unit (no list)
            // uid is authoritative and survives a save; xlIdx is the fallback for
            // the many lists the engine never assigns a uniqueID to (tempered /
            // poisoned / renamed). NEVER cache the ExtraDataList* itself -- the
            // engine reallocates and frees them (freeing one under us was a real
            // CTD; see the note above extraOf in DrawItemTooltip).
            std::uint16_t       uid = 0;
            std::uint16_t       sig = 0;   // GI25: content signature (uid-less units)
            int                 xlIdx = -1;
            // ★Derived LIVE from the bound sub-stack every rebuild, never cached
            // under a key. See PoolIsStolen / PoolIsQuest.
            bool                stolen = false;   // foreign owner -> fences only
            bool                quest  = false;   // quest alias -> cannot leave
            // GI62: quarter-turns clockwise (0..3). `mask` is ALREADY rotated by
            // it -- this is kept only for drawing the sprite at the same angle.
            int                 rot = 0;
            // ★P2/3-1: this tile is ALSO on the body. True only for a worn BAG,
            // the one unit allowed to be in two places at once (see the worn
            // exception in EnumerateUnitRefs), so the tray can say so.
            bool                worn = false;
        };

        struct LayoutEntry
        {
            int         col = 0;
            int         row = 0;
            std::string bag;
            int         count = 0;   // G4: tile's owned quantity (Mabinogi
                                     // split/merge). 0 = unspecified -> the
                                     // reconciler fills it like a fresh pickup
                                     // (legacy saves & never-split forms).
            // GI62: quarter-turns clockwise (0..3) the player left this tile at.
            // ★Rotation lives on the SLOT, not on the item -- the engine has no
            // place to hang per-instance data (ExtraDataList cannot be created),
            // and the slot is exactly what "this spot on the board" means. It
            // follows the item into a container because the drop hint carries it,
            // and it dies when the spot dies (dropped to the world, equipped).
            int         rot = 0;
            // ★★G6: a COIN tile's amount, kept on the SLOT. -1 = not a coin.
            // It used to be derived from the tile's ordinal, and the ordinal
            // was re-assigned by grid position on every rebuild -- so a 1000 G
            // tile carried to the back of the board became the last ordinal and
            // came up as the remainder, while the 660 that had been there
            // turned into a thousand. The amount followed the CELL instead of
            // the tile the player was holding.
            // Keeping it here is the same answer `rot` already gave: the engine
            // has nowhere to hang per-instance data, and the slot is what "this
            // spot on the board" means. The ordinal still decides how many
            // tiles exist -- that comes from the walking total -- but no longer
            // decides which one is worth what.
            int         coin = -1;
            // ★★★THE SLOT REMEMBERS WHICH UNIT IT WAS SHOWING -- AS DATA.
            //
            // These two used to live INSIDE the key ("base~B825#2"), which made
            // them identity: change the item and the tile was renamed, so its
            // remembered cell belonged to a name nothing answered to any more.
            // The whole apparatus that grew around that -- a slot-inheritance
            // pass, a drift simulator, an eight-tier exclusion ladder -- existed
            // only to carry cells across those renames.
            //
            // Here they are a HINT: what this slot showed last rebuild, used to
            // recognise the same unit again. A hint may go stale with no
            // consequence beyond a weaker match, because the fallback is "keep
            // the cells you already have, in position order" -- never "mint a
            // new tile". The key itself is now minted once and never changes.
            std::uint16_t uid = 0;    // ExtraUniqueID, 0 = the engine gave none
            std::uint16_t sig = 0;    // content signature, 0 = a plain unit
            // Where the unit sat in entry->extraLists last rebuild. Transient by
            // nature (any insert or removal shifts it), so it is only ever a
            // tie-breaker -- never persisted, never trusted alone.
            int         xlIdx = -1;
        };

        // one grid: main (""), a bag (bag item key), or the trash.
        // ★`open` is the ONLY thing the window state decides. Every bag that
        //  exists as a tile gets a view and a real placement pass, because its
        //  cells exist whether or not the player is looking at them; a closed
        //  bag simply draws no window. Before this, a view was created only for
        //  OPEN bags, so a closed bag subtracted board space (its contents are
        //  hidden) without ever contributing any — loot and purchases were
        //  refused with a half-empty bag on the belt, and right-clicking the
        //  bag "fixed" it (user report).
        struct View
        {
            std::string      bagKey;   // "" = main
            std::string      bagName;  // window title for bags
            std::string      accept;   // typed bag: the BagFilter id it takes
            bool             carried = false;   // its tile is on the cursor
            int              cols = kCols;
            int              minRows = kMinRows;
            int              maxRows = 4096;
            std::vector<int> items;    // indices into g_items
            int              rows = kMinRows;
            bool             open = true;   // false = holds items, draws no window
        };

        struct Held
        {
            std::string         key;
            RE::TESBoundObject* obj = nullptr;
            Mask                mask;
            int                 count = 1;
            bool                isBag = false;
            float               defScale = 1.0f;
            float               offX = 0.0f;   // grab offset px within the footprint
            float               offY = 0.0f;
            bool                justPicked = true;
            bool                preSplit = false;   // came from a shift+lclick split slider
            int                 coinValue = -1;     // G2/G4: carried coin's gold value
            bool                fromPartner = false;   // carried FROM the merchant/container
            int                 partnerValue = 0;      // its base value (buy pricing)
            // GI1: the sub-stack this carry came from. For a GRID pickup the uid
            // also lives in `key`; for a PARTNER carry the key is "##partner"
            // (not a real tile) so it has to be carried explicitly.
            // Appended LAST on purpose -- the six aggregate initialisers below
            // stay valid and default both.
            int                 xlIdx = -1;
            std::uint16_t       uid = 0;
            int                 hand = 0;   // 1 right, 2 left -- doll carries only
            // Lifted off the DOLL (still worn until the unequip lands) rather
            // than off the board. Only THAT carry may be excluded without
            // actually coming out of the board set -- a board carry is a board
            // unit and must be removed, or a spare of the same form stays
            // counted and the drop lands as a fresh arrival.
            bool                fromDoll = false;
            // GI19: two PLAIN units of the same form share uid 0 and xlIdx -1 --
            // they are indistinguishable by content, and that is the whole point
            // of them being plain. Only their cell ordinal tells them apart, so
            // the carry has to remember which one was lifted or picking up one
            // of two identical daggers looked like picking up both.
            int                 partnerOrd = 0;
            std::uint16_t       sig = 0;   // GI25: survives the queue delay
            // ★GI36's argument, applied to the two markers as well: once the unit
            // is mid-flight there is no tile left to ask, so the answers travel
            // WITH the carry. Set at pickup from the tile that was lifted.
            bool                stolen = false;
            bool                quest  = false;
            // Displaced by a drop onto an OCCUPIED slot. The engine unequips this
            // unit as part of the very equip we just queued, so it stops being
            // worn exactly when that equip lands -- and after that point it must
            // NOT keep claiming a worn list, because the only one left belongs to
            // the unit that replaced it. Identity cannot tell the two apart (same
            // form, same signature, same hand), so the pending equip's lifetime is
            // what draws the line: while it exists the swap is still in flight.
            bool                swappedOut = false;
            // GI36: this carry left the board wearing a star. The exit sinks
            // cannot recompute it -- once the unit is mid-flight there is no
            // tile left to ask -- so it travels WITH the carry.
            bool                fav = false;
            // GI62: quarter-turns clockwise. A/D turn it WHILE carried; `mask`
            // is re-derived on every turn so the ghost, the collision test and
            // the drop all see the turned footprint with no further plumbing.
            int                 rot = 0;
            // ★The SPRITE's angle, which chases the footprint rather than
            // matching it. Kept as an unwrapped running total (…-90, 0, 90,
            // 180, 270…) instead of rot*90: from 270 back to 0 the short way
            // is forwards, and a wrapped value would spin the item three
            // quarters backwards to get there.
            float               rotDeg = 0.0f;    // what is drawn this frame
            float               rotAim = 0.0f;    // where it is heading
            float               rotFrom = 0.0f;   // where this turn started
            float               rotT = 1.0f;      // 0..1 through the turn (1 = settled)
            int                 rotPrev = 0;      // the footprint it just left
            // ★★Which POOL this carry left, for the frames where `key` cannot
            // say. A split fragment clears its key on purpose ("assigned on
            // drop"), and PoolOfKey("") is "" -- which matches no pool, so the
            // carried units were subtracted from NOBODY and the reconciler saw
            // the source stack as short by exactly the amount on the cursor. It
            // then refilled it: three arrows split off ten left three on the
            // cursor and ten on the board.
            // Set from the SOURCE TILE's key, so a fragment split off a stolen
            // stack is attributed to the stolen pool rather than to the plain
            // one it does not belong to.
            std::string         srcPool;
            // ★★Lifted from the SECOND RING slot: its unit was never
            // engine-worn (a carrier wears the effect), so this carry has NO
            // worn list to be backed by -- treating it as fromDoll-worn made
            // the accounting consume the FIRST ring's list instead (same
            // plain form) and that unit leaked onto the board. Appended LAST
            // (§10-6).
            bool                fromCarrier = false;
            // ★ONE PATH / O-0: a SYNTHETIC carry -- one the code made on the
            // player's behalf so a right-click can travel the same road a drag
            // does. It is born and consumed inside one call, so it must never
            // reach the cursor: no icon, no pickup sound, no second frame. The
            // flag is what every "is the player holding something" reader uses
            // to tell the two apart, and what the leak guard in FinishFrame
            // watches for. Appended LAST (§10-6).
            bool                transient = false;
            // ★S1: this carry's board tile has not been detached yet -- the C1
            // lift fires inside the tile loop, where g_items must not move.
            // FinishFrame stashes it (StashTileForCarry) before the drop
            // resolution runs; the one-frame latency is what the deferred
            // rebuild always had. Appended LAST (§10-6).
            bool                needsDetach = false;
            // Adopting an angle (lifting a tile that already lies on its side)
            // must NOT animate -- there is nothing to show, the item was already
            // like that. Only a keypress starts a turn.
            void SetRot(int a_rot)
            {
                rot = rotPrev = a_rot & 3;
                rotDeg = rotAim = rotFrom = static_cast<float>(rot) * 90.0f;
                rotT = 1.0f;
            }
        };

        // GI64: thousands separators. Five figures of gold are hard to read as a
        // bare run of digits, and the pouch line prints two of them side by side.
        [[nodiscard]] std::string Commas(int a_v)
        {
            std::string s = std::to_string(a_v);
            for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
                s.insert(static_cast<std::size_t>(i), ",");
            }
            return s;
        }

        // GI63: the tooltip's hover, stamped with the frame it happened on.
        struct HoverRec
        {
            int       frame = -1;
            bool      canSplit = false;
            bool      canCompare = false;
            bool      canDrop = false;
            bool      canFav = false;
            bool      hasVerb = false;
            Lang::Str verb{};
            bool      canRecharge = false;   // (1.3.1) T, on an enchanted weapon
            bool      canShelfUse = false;   // (1.5.0) Shift+RMB reads on the shelf
            Lang::Str useVerb{};
        };
        HoverRec g_hoverPrompt;

        // drop candidate under the cursor this frame (set by DrawGridView)
        struct DropTarget
        {
            bool                     has = false;
            int                      view = -1;
            int                      col = 0;
            int                      row = 0;
            std::vector<int>         blockers;   // g_items indices
            bool                     valid = false;
        };

        bool g_overloaded = false;      // W2: hard board can't hold everything
        bool g_capacityDirty = true;    // recompute on next CapacityTick
        int  g_spaceUsed = 0;           // S2: cells occupied (main board + bags)
        int  g_spaceTotal = kCols * kMinRows;   // + every owned bag's grid

        // ★W3: CARRY WEIGHT -> OWNED CELLS. External CW past the baseline
        // converts to cells appended past the hard board (left-to-right in a
        // partial row), and the crimson overload line becomes the stepped
        // ownership boundary. Derived from the live AV every CapacityTick --
        // nothing is saved. 0 per-cell = feature off.
        int g_cwPerCell = 10;     // !cwcells: CW per cell
        // baseline -- only CW above this converts. 0 = AUTO: the player
        // RACE's own baseCarryWeight, so an overhaul that changes the base
        // (Requiem lines and the like) is tracked without any setting.
        int g_cwBase = 0;
        int g_cwMaxCells = 50;    // bonus cap
        int g_cwBonusCells = 0;   // current, recomputed by CapacityTick

        bool g_pouchOpen = false;       // G2: coin-pouch withdraw window
        int  g_pouchSlider = 0;
        // ★WHICH pouch the window is drawing from. It used to need no such
        // thing because there was one amount for the whole player; with the
        // amount on the tile, a window that does not name its pouch would
        // withdraw from whichever one the map happened to list first.
        std::string g_pouchTile;


        // ★B4-3c: the pending-removal counters that lived here for three
        // versions -- g_pendingRemoveForm / Pool / Xl and their TTL stamps,
        // rule 5's "traces in many places" -- are ABSORBED into the request
        // ledger. Every reader asks Ledger::OpenOutgoingOf/Count now: the
        // entry carries the same uid/sig the pool prefix used to encode, the
        // slot key for the two-phase drop, and one frame-count expiry instead
        // of a parallel TTL. Measured to agree across two submit phases (28
        // boundary audits, zero mismatches) before the counters went.
        // ★B3-b: gear slots whose drop is waiting on the engine's answer. FIFO
        // per form; CommitSlotDrop erases them, CancelSlotDrop lets them live.
        std::unordered_map<RE::FormID, std::vector<std::string>> g_pendingSlotDrop;

        // ---- B3-c: rebuild provenance ---------------------------------------
        // ★RequestRebuild is called from EVENT THREADS (ContainerSink, the
        // host API), and NoteRebuildRan drains these on the main thread. The
        // flag is atomic and the containers share one mutex -- without it,
        // "!rbtrace = 1" plus one container event was a concurrent std::map
        // write, i.e. the observation tool was itself a CTD (원칙 4).
        std::atomic<bool>          g_rbTrace{ false };
        std::mutex                 g_rbMtx;
        std::vector<std::string>   g_rbDrop;    // TEST ONLY, see SetRebuildDrop
        std::map<std::string, int> g_rbAsk;      // askers since the last rebuild
        int                        g_rbRuns = 0; // rebuilds since the last report
        std::chrono::steady_clock::time_point g_rbWindow{};

        // "F:/.../ui/Grid.cpp:1234" -> "Grid.cpp:1234"
        std::string ShortSite(const std::source_location& a_w)
        {
            std::string f = a_w.file_name() ? a_w.file_name() : "?";
            const auto cut = f.find_last_of("/\\");
            if (cut != std::string::npos) f = f.substr(cut + 1);
            return f + ":" + std::to_string(a_w.line());
        }

        // ...list position of a tile PARKED IN THE TRASH, whose unit is still
        // owned but occupies no cell. Runtime only: after a load the
        // signatures are stable again and the strict match is enough.
        std::map<std::string, int>          g_trashXl;

        // Units whose EQUIP is queued for the next Tick. Deliberately separate
        // from the removal counters above: an equipped item stays in the stock
        // at full count, so a removal entry for it can never resolve. This one
        // is cleared by Equip::ProcessPending the moment it has applied the
        // queue, with a TTL only as a safety net for a rejected equip.
        // B-model: units that are NOT ON THE BOARD, named by identity rather than
        // by a pool string. The board's input set is built with these already
        // removed -- the old scheme left them in and subtracted them later, in
        // several places with different timing, which is what produced ghost
        // tiles, spares jumping to the front and cumulative disappearances.
        struct OffBoardUnit
        {
            std::string   base;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            const char*   why = "?";   // diagnostics only
            // True for units that are MID-TRANSITION between the board and the
            // body, where "is it worn yet?" decides whether it still needs to be
            // removed from the set:
            //   held (from the doll) -- unequip queued, still worn for now
            //   equipping            -- equip queued, not worn yet
            // False for reasons that never involve the body -- reserved, removing,
            // trash -- because for those, consuming a worn entry would let the
            // removal be absorbed by an unrelated equipped copy.
            bool          mayBeWorn = false;
            // The engine has run the equip. Identity matching cannot decide this:
            // the worn list's signature does not always equal the one recorded at
            // request time, so "is a worn list matching me?" answered NO forever
            // and the entry kept subtracting a spare on every rebuild. The queue
            // itself knows when it applied, so let it say so.
            bool          applied = false;
            // Two units of ONE form worn at once (a dagger in each hand) have the
            // same uid AND the same signature -- content cannot tell them apart,
            // and the hand is the only thing that can. Without it, lifting the
            // left one excluded the RIGHT one and left a ghost on the board.
            // 0 = not worn / either, 1 = right, 2 = left.
            int           hand = 0;
            // The board cell this unit left FROM, if it left from one. A unit in
            // transit still owns that cell until the transition completes: the
            // pool must hold the slot open rather than re-pack the survivors into
            // the front slots, which empties WHOSE-EVER cell happens to sort last
            // instead of the one the player acted on. Empty for units that never
            // had a cell (lifted off the doll, held by a preset).
            std::string   srcKey;
            // Heading ONTO the body rather than already on it. Such a unit cannot
            // be backed by a worn list until its equip has actually run -- with a
            // copy in each hand a strict identity match still found the OTHER
            // hand's list, and the accounting wrote the unit off as already worn.
            // (Appended last so the aggregate initialisers above stay valid.)
            bool          arriving = false;
            // *The units POSITION in the entrys extraLists. Unlike the
            // signature this cannot drift: it is where the list sits, not
            // what the list says. When a carried units numbers move under
            // us this is the only handle left that still points at the same
            // unit -- see the carry fallback in EnumerateUnitRefs.
            int           xlIdx = -1;
            // ★How MANY units this one entry stands for. One, for everything you
            // wear a single copy of -- but a quiver is equipped by the tileful,
            // and a suppression worth 1 against a 100-arrow equip left the board
            // showing 99 spare arrows for the frame or two before the engine
            // caught up. Only the stackable branch reads this; the per-unit
            // walker never sees an entry worth more than one.
            int           units = 1;
            // ★B4-2c: the ENGINE confirmed this equip -- TESEquipEvent with
            // equipped == true, delivered through NoteEquipLanded. This is
            // what `applied` above always wanted to mean: `applied` flips
            // when OUR CALL RETURNS (ProcessPending marks the whole queue),
            // which says nothing about whether the engine actually wore the
            // unit. The worn-clock reads THIS field now; `applied` stays for
            // the release bookkeeping that genuinely is about the call.
            // (Appended last, same aggregate rule as `arriving`.)
            bool          landed = false;
            // ★Ring session: does this form CONFIRM? Gear (the worn ledger's
            // types) gets a TESEquipEvent and retires through landed-release /
            // match-release / the settle sweep. A consumable never does -- the
            // applied-erase in FinalizeRebuild is ITS retirement, and it must
            // stop eating gear entries there: every erase requested another
            // rebuild, and during a swap run that cascade was measured at 11
            // rebuilds per second -- the blink's engine room.
            bool          confirmable = false;
        };
        std::vector<OffBoardUnit>             g_pendingEquip;
        std::chrono::steady_clock::time_point g_pendingEquipWhen{};
        constexpr std::chrono::seconds      kPendingEquipTTL{ 2 };
        // How long the request stream must be QUIET before a landed record is
        // considered settled (its equip event arrived and nothing new is in
        // flight). Well past any engine equip latency, well under the TTL.
        constexpr std::chrono::milliseconds kPendingEquipSettle{ 250 };

        // GI1 tile-key grammar:  formKey [ '@' uid-hex ] [ '#' ordinal ]
        //
        //   "Skyrim.esm|0x012EB7"         plain stack, tile 0
        //   "Skyrim.esm|0x012EB7#2"       plain stack, tile 2   (interchangeable units)
        //   "Skyrim.esm|0x012EB7@A31F"    the ONE unit carrying ExtraUniqueID 0xA31F
        //   "Skyrim.esm|0x012EB7@A31F#1"  that list's 2nd unit (GetCount() > 1)
        //
        // Before GI1 a gear tile was identified by its ORDINAL, which the
        // collection loop assigns in inventory-walk order — so equipping one of
        // three identical swords shifted every later tile onto its neighbour's
        // saved spot (and onto its neighbour's per-instance data). The uid binds
        // a tile to the engine's own sub-stack instead.
        //
        // BaseKey strips BOTH suffixes: every one of its ~16 call sites compares
        // the result against a FormKey, so "the form this tile shows" is exactly
        // what they all want.
        // Suffixes are only ever appended AFTER the "|0xID" part, so scan from
        // there: a PLUGIN FILENAME may legitimately contain '#' or '@', and
        // scanning from the start would cut the key inside the mod's name.
        // (The pre-GI1 code used rfind('#'), which was accidentally safe for
        // '#' and would not have been for '@'.)
        std::size_t KeySuffixPos(const std::string& a_key)
        {
            const auto bar = a_key.find('|');
            return a_key.find_first_of("@~#", bar == std::string::npos ? 0 : bar + 1);
        }

        // GI14: a CONTENT signature for a sub-stack the engine never gave a
        // uniqueID -- which is most of them (tempering, poison, charge, a
        // rename all create a list, none of them a uid).
        //
        // Without it those units fell back to the shared ordinal pool, so
        // storing a tempered dagger next to a plain one RENUMBERED both: the
        // tempered list is enumerated first, took ordinal 0, and inherited the
        // plain dagger's remembered cell. The two visibly swapped places.
        //
        // Two units with identical contents hash the same, and that is correct:
        // they ARE interchangeable. Retempering changes the hash, so the tile
        // moves -- the item genuinely changed state.
        std::uint16_t InstanceSig(const RE::ExtraDataList* a_xl)
        {
            if (!a_xl) return 0;
            std::uint32_t h = 2166136261u;                    // FNV-1a
            auto mix = [&h](std::uint32_t v) {
                for (int i = 0; i < 4; ++i) {
                    h ^= (v >> (i * 8)) & 0xFF;
                    h *= 16777619u;
                }
            };
            auto* xl = const_cast<RE::ExtraDataList*>(a_xl);
            // Did this list actually carry anything that distinguishes the unit?
            // A list can exist for reasons that say NOTHING about the item --
            // ExtraWorn on an equipped plain sword, a favourite hotkey.
            // Hashing none of the seven below still produced a
            // non-zero constant (0x1CD9), which invented a pool that no longer
            // existed once the list was dropped: lifting an equipped plain
            // weapon off the doll keyed the carry to "base~1CD9", the unequipped
            // unit came back as a PLAIN unit, the two no longer matched, and the
            // grid drew a tile for the item it was already carrying.
            bool mixed = false;
            // OWNERSHIP is part of the identity. A stolen dagger and a clean one
            // are not interchangeable -- an ordinary merchant refuses the first
            // and buys the second -- so they must not share a tile. Leaving it
            // out meant stolen-ness could only be tracked per FORM, and putting
            // one dagger into an owned chest and taking it back branded every
            // dagger the player owned, tempered ones included.
            if (auto* owner = xl->GetOwner()) {
                // ...but ONLY foreign ownership. An item stamped as the player's
                // own is not distinguishable from a listless copy in any way that
                // matters, and hashing it would split ordinary tiles for nothing.
                auto* pc = RE::PlayerCharacter::GetSingleton();
                if (!pc || owner != pc->GetActorBase()) {
                    mixed = true;
                    mix(0x4F574E52u);
                    mix(owner->GetFormID());
                }
            }
            // A quest unit cannot be dropped, sold or stored while an ordinary
            // copy of the same form can be. Same argument as ownership: not
            // interchangeable, therefore not the same pool. Without this the
            // quest lock could only be tracked per FORM, and one quest-flagged
            // potion locked every potion of that kind in the pack.
            if (xl->HasQuestObjectAlias()) {
                mixed = true;
                mix(0x51554553u);
            }
            if (const auto* x = xl->GetByType<RE::ExtraHealth>()) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &x->health, sizeof(bits));
                mixed = true;
                mix(0x48454C54u); mix(bits);
            }
            // ★★AMOUNT is part of identity, and dropping it was a mistake worth
            // recording. It was removed once to stop a tile jumping when its
            // pool changed mid-play: every swing of an enchanted weapon spends
            // charge, the signature moved, the new prefix had no remembered
            // slots, and the tile was reborn in the first free cell.
            //
            // That fixed the jump by making a half-charged sword and a full one
            // THE SAME UNIT — and units inside one pool are deliberately
            // indistinguishable, so the two could no longer be told apart or
            // swapped. The player names them "the spent one" and "the full one";
            // that is the definition of not interchangeable.
            //
            // Identity and placement are separate problems. This answers only
            // "are these the same thing" — honestly, amount included. Keeping a
            // unit's CELL when its answer changes is the slot-inheritance pass
            // in the pool walker, which is where that belongs.
            if (const auto* x = xl->GetByType<RE::ExtraEnchantment>()) {
                mixed = true;
                mix(0x454E4348u);
                mix(x->enchantment ? x->enchantment->GetFormID() : 0u);
                mix(x->charge);
            }
            if (const auto* x = xl->GetByType<RE::ExtraCharge>()) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &x->charge, sizeof(bits));
                mixed = true;
                mix(0x43485247u); mix(bits);
            }
            if (const auto* x = xl->GetByType<RE::ExtraPoison>()) {
                mixed = true;
                mix(0x50534E4Eu);
                mix(x->poison ? x->poison->GetFormID() : 0u);
                mix(x->count);
            }
            if (const auto* x = xl->GetByType<RE::ExtraSoul>()) {
                mixed = true;
                mix(0x534F554Cu);
                mix(static_cast<std::uint32_t>(x->GetContainedSoul()));
            }
            // ExtraTextDisplayData is deliberately NOT hashed.
            //
            // The engine splits a unit out of its stack when it is equipped and
            // copies ExtraHealth to the new list but NOT the display name, so the
            // SAME dagger answered one signature in the pack and another on the
            // body. Every match across that boundary then failed: lifting off the
            // doll left a duplicate on the board, and equipping subtracted a
            // spare because "is it worn yet?" could never say yes.
            //
            // A name the engine can drop underneath us is not identity. Renamed
            // items therefore share a pool with their unrenamed twins -- the
            // tooltip still shows the real name, since that reads the unit's own
            // list rather than the signature.
            // Nothing distinguishing -> this unit belongs to the PLAIN pool,
            // exactly like a unit with no list at all. Both must answer 0.
            if (!mixed) return 0;
            const std::uint16_t sig = static_cast<std::uint16_t>((h ^ (h >> 16)) & 0xFFFF);
            return sig == 0 ? 1 : sig;   // 0 is reserved for "no signature"
        }

        std::string BaseKey(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            return cut == std::string::npos ? a_key : a_key.substr(0, cut);
        }

        // The engine applies an equip a frame or two after we ask; IsWorn() turning
        // true is the only signal actually in step with it.
        void ReleasePendingEquip(const std::string& a_baseKey)
        {
            std::erase_if(g_pendingEquip,
                [&](const OffBoardUnit& u) { return u.base == a_baseKey; });
        }

        // Per-UNIT release: a queued equip is done when a WORN list matching that
        // unit's identity exists. Defined after g_held (it has to ask what the
        // cursor is holding); declared here because the pool walker calls it.
        void ReleaseWornPendingEquips(const std::string& a_baseKey,
                                      RE::InventoryEntryData* a_entry);

        // ---- GI20: pools ----------------------------------------------------
        //
        // A POOL is a set of units that are interchangeable WITH EACH OTHER:
        //
        //   "form@A31F"  exactly one unit (the engine gave it a uniqueID)
        //   "form~7C2E"  every unit whose extras hash the same (both +10% ones)
        //   "form"       every unit with no extras at all
        //
        // Inside a pool "which one" is a question with no answer -- and asking it
        // was the mistake. What matters is only that the pool has as many slots
        // as units, and that removing a unit frees the slot the player acted on.
        //
        // So: a pool's units are assigned to that pool's remembered slots IN
        // POSITION ORDER, exactly like the stackable branch has always done.
        // Nothing is ever renumbered, so nothing can jump.
        std::string PoolOfKey(const std::string& a_key)
        {
            const auto bar = a_key.find('|');
            const auto h = a_key.find('#', bar == std::string::npos ? 0 : bar + 1);
            return h == std::string::npos ? a_key : a_key.substr(0, h);
        }

        std::string PoolPrefix(const std::string& a_base, std::uint16_t a_uid,
                               std::uint16_t a_sig)
        {
            char buf[8];
            if (a_uid != 0) { std::snprintf(buf, sizeof(buf), "@%04X", a_uid); return a_base + buf; }
            if (a_sig != 0) { std::snprintf(buf, sizeof(buf), "~%04X", a_sig); return a_base + buf; }
            return a_base;
        }

        // The signature encoded in a key ('~XXXX'), 0 when there is none.
        std::uint16_t SigOf(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            if (cut == std::string::npos || a_key[cut] != '~') return 0;
            return static_cast<std::uint16_t>(
                std::strtoul(a_key.c_str() + cut + 1, nullptr, 16));
        }

        // Is this key bound to a specific sub-stack rather than to a position
        // in a sequence? BOTH marks count.
        //
        // GI14 added '~sig' but left the old `UidOf(key) != 0` test in the two
        // places that decide "ordinal tile or not" -- and UidOf returns 0 for a
        // '~sig' key. So a tempered dagger was swept into the dense ORDINAL
        // re-key: storing the plain dagger next to it renamed "form~7C2E" to
        // "form" and handed it the plain tile's slot. The wrong tile emptied.
        bool IsInstanceKey(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            return cut != std::string::npos && (a_key[cut] == '@' || a_key[cut] == '~');
        }

        // 0 = no instance (a unit of the plain stack).
        std::uint16_t UidOf(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            if (cut == std::string::npos || a_key[cut] != '@') return 0;
            return static_cast<std::uint16_t>(
                std::strtoul(a_key.c_str() + cut + 1, nullptr, 16));
        }

        // '@' = engine uniqueID (authoritative, survives a save)
        // '~' = content signature (GI14 fallback for the uid-less majority)
        std::string TileKey(const std::string& a_base, std::uint16_t a_uid,
                            std::uint16_t a_sig, int a_ord)
        {
            std::string k = a_base;
            char buf[8];
            if (a_uid != 0) {
                std::snprintf(buf, sizeof(buf), "@%04X", a_uid);
                k += buf;
            } else if (a_sig != 0) {
                std::snprintf(buf, sizeof(buf), "~%04X", a_sig);
                k += buf;
            }
            if (a_ord > 0) k += "#" + std::to_string(a_ord);
            return k;
        }

        // A carry's uid lives in its tile key for grid pickups and in the
        // explicit field for partner carries (whose key is "##partner").
        struct Held;
        [[nodiscard]] std::uint16_t HeldUidOf(const std::string& a_key, std::uint16_t a_field)
        {
            // ★The FIELD wins. It used to be the other way round, back when a
            // key was the authority on identity -- but a key minted since 1.3.2
            // carries none, and a key inherited from an older save carries
            // whatever the slot happened to be showing when it was named, which
            // may no longer be what is standing on it. The field is set from the
            // slot's live hint at pickup, so it is right in both worlds.
            if (a_field != 0) return a_field;
            return UidOf(a_key);   // legacy key from a pre-1.3.2 save
        }

        // GI1: resolve the ExtraDataList a TILE stands for -- never "the first
        // one", which is what GlowBits / extraOf / ToggleFavorite all did (a
        // stack of three swords with one enchanted showed three glowing tiles,
        // leaked the enchanted one's tooltip onto the plain ones, and put the
        // favourite star on the wrong sword).
        //
        // uid wins when the engine assigned one. Otherwise fall back to the
        // position recorded at collection time. Returns nullptr for a plain
        // unit, which is the correct answer: it genuinely has no list.
        //
        // ★The index is a HINT, not a promise: entry->extraLists is a linked
        // list the engine reorders behind us, so between collection and this
        // call the n-th entry may be a different unit. Nothing here revalidates
        // it — an earlier version of this comment claimed it did, which is worse
        // than silence, because a caller reading it would trust a check that was
        // never written. Doing it properly needs the unit's signature passed in
        // (ExtraForPool below takes one and is safe for that reason); prefer
        // that resolver wherever the pool is known.
        RE::ExtraDataList* ExtraForTile(RE::InventoryEntryData* a_entry,
                                        std::uint16_t a_uid, int a_xlIdx)
        {
            if (!a_entry || !a_entry->extraLists) return nullptr;
            if (a_uid != 0) {
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl) continue;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                        xu && xu->uniqueID == a_uid) {
                        return xl;
                    }
                }
                return nullptr;   // that instance left the inventory
            }
            if (a_xlIdx < 0) return nullptr;   // plain unit
            int i = 0;
            for (auto* xl : *a_entry->extraLists) {
                if (i++ == a_xlIdx) return xl;
            }
            return nullptr;
        }

        // GI40: is ANY unit of this pool favourited?
        //
        // Worn lists count. Equipping splits a unit off and the engine carries
        // the ExtraHotkey over to the worn list, so excluding it would make the
        // spares left in the bag go dark the instant one was put on -- which is
        // exactly the bug this replaced. A worn plain dagger hashes to the same
        // signature as its spares (ExtraWorn is not part of InstanceSig), so it
        // lands in the same pool, which is the point.
        bool PoolHasStar(RE::InventoryEntryData* a_entry,
                         std::uint16_t a_uid, std::uint16_t a_sig)
        {
            if (!a_entry || !a_entry->extraLists) return false;
            for (auto* xl : *a_entry->extraLists) {
                if (!xl || !xl->HasType<RE::ExtraHotkey>()) continue;
                if (a_uid != 0) {
                    const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                    if (xu && xu->uniqueID == a_uid) return true;
                    continue;
                }
                if (InstanceSig(xl) == a_sig) return true;
                // ★★★THE PLAIN POOL NO LONGER BORROWS ITS NEIGHBOUR'S STAR.
                //
                // There used to be `if (a_sig == 0) return true;` here, on the
                // reasoning that a plain unit owns no ExtraDataList and so a
                // star for it has nowhere to live -- the engine writes into a
                // variant sibling's list instead. That measurement was real,
                // and it stopped being the whole story the day ProcessFavorites
                // learned to LIFT every existing star before calling
                // SetFavorite: with the entry momentarily bare, the engine
                // mints a fresh list, and the plain pool ends up owning one
                // like everybody else. The line outlived the problem.
                //
                // What it cost while it stayed: star the tempered sword and the
                // plain one lit up beside it, because "any star on this entry"
                // is true for a plain pool by definition. The favourites menu
                // -- reading the engine rather than us -- showed only the
                // tempered one, which is how it was caught.
                //
                // ★If a plain unit ever genuinely fails to get its own list,
                // the star belongs to the sibling that did, and NOT lighting up
                // is then the honest answer: that is the unit the game will
                // actually reach for.
            }
            return false;
        }

        // ★A MARKER IS A PROPERTY OF THE UNIT, NOT OF A NAME WE INVENTED FOR IT.
        //
        // Stolen-ness and the quest lock used to be cached in maps keyed by
        // PoolPrefix(base, uid, sig) -- a string with the item's MUTABLE state
        // baked into it. Every path that changed that state (a poison applied, a
        // charge spent, a trip to the grindstone) silently re-keyed the item and
        // orphaned its marker, so the caches had to be torn down and rebuilt from
        // the whole inventory on every single rebuild just to stay honest.
        //
        // Reading the live list instead cannot go stale. It is the same shape
        // PoolHasStar has always used, and it removes two more consumers of
        // "identity spelled as a string".
        template <class Pred>
        bool PoolAny(RE::InventoryEntryData* a_entry, std::uint16_t a_uid,
                     std::uint16_t a_sig, Pred a_pred)
        {
            if (!a_entry || !a_entry->extraLists) return false;
            for (auto* xl : *a_entry->extraLists) {
                if (!xl || !a_pred(xl)) continue;
                if (a_uid != 0) {
                    const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                    if (xu && xu->uniqueID == a_uid) return true;
                    continue;
                }
                // Ownership and the quest alias are both hashed INTO the
                // signature, so a stolen unit and a clean one are different
                // pools by construction and this comparison is exact -- the
                // split the old cache achieved by writing two map entries.
                if (InstanceSig(xl) == a_sig) return true;
            }
            return false;
        }

        // Not the player's -> stolen. An UNOWNED item counts as the player's:
        // most loot in the world carries no owner at all.
        bool PoolIsStolen(RE::InventoryEntryData* a_entry, std::uint16_t a_uid,
                          std::uint16_t a_sig)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !a_entry) return false;
            return PoolAny(a_entry, a_uid, a_sig, [&](RE::ExtraDataList* a_xl) {
                auto* owner = a_xl->GetOwner();
                return owner && !a_entry->IsOwnedBy(player, owner, true);
            });
        }

        // ★THE QUEST-OBJECT POLICY, in one place (1.4.0, feedback ⑤⑭).
        //
        // A quest object may CHANGE CONTAINERS -- a chest, a follower's pack --
        // but may never be DESTROYED or SOLD. Phase 7 wrote the stricter rule
        // ("never leaves the inventory") and it broke vanilla quests that are
        // FINISHED by putting the item in a container: Arniel's warped soul gem
        // into the Dwemer convector, Klimmek's supplies into the chest outside
        // High Hrothgar. Both work in the vanilla menu; both were dead ends with
        // this mod installed (user reports).
        //
        // The engine does not enforce the old rule -- we invented it -- so the
        // exits that LOSE the item keep their guard and the ones that merely
        // MOVE it open up:
        //   blocked  world drop (R / void), sell, trash, pickpocket PLANT
        //   allowed  store into a container, hand to a follower
        // The plant stays blocked deliberately: a mark wanders off with it in a
        // way a chest never does, and no report asks for it.
        //
        // The detection itself is the ENGINE'S OWN -- HasQuestObjectAlias is
        // exactly what InventoryEntryData::IsQuestObject walks -- so this was
        // never a matter of flagging too much, only of forbidding too much.
        bool PoolIsQuest(RE::InventoryEntryData* a_entry, std::uint16_t a_uid,
                         std::uint16_t a_sig)
        {
            return PoolAny(a_entry, a_uid, a_sig, [](RE::ExtraDataList* a_xl) {
                return a_xl->HasQuestObjectAlias();
            });
        }

        // Does USING this destroy it? Potions, food and ingredients are eaten;
        // a spell tome is consumed by the reading, while a quest journal or a
        // note is a BOOK that survives being opened -- so the tome test is the
        // spell, not the form type. Everything else is worn, wielded or poked
        // by a script and is still there afterwards.
        bool ConsumingWouldEatIt(RE::TESBoundObject* a_obj)
        {
            if (!a_obj) return false;
            if (a_obj->Is(RE::FormType::AlchemyItem) ||
                a_obj->Is(RE::FormType::Ingredient)) {
                return true;
            }
            const auto* book = a_obj->As<RE::TESObjectBOOK>();
            return book && book->TeachesSpell();
        }

        // GI25: resolve by POOL -- uid first, then content signature. Unlike
        // ExtraForTile this never falls back to a list POSITION, so it stays
        // correct across the frames between queueing a transfer and the engine
        // actually running it.
        // a_allowWorn: ONLY for a non-player source. A corpse's or a mark's worn
        // gear is shown on the partner board on purpose, so a take has to be able
        // to name it -- excluding it there returned nullptr, the engine picked for
        // itself, and looting an NPC's equipped sword could move a spare from the
        // same inventory instead. Never true for the player's own side (see below).
        RE::ExtraDataList* ExtraForPoolImpl(RE::InventoryEntryData* a_entry,
                                            std::uint16_t a_uid, std::uint16_t a_sig,
                                            bool a_allowWorn = false)
        {
            if (!a_entry || !a_entry->extraLists) return nullptr;
            // The WORN list must never be a candidate here. Tile enumeration
            // already skips it, so every unit this resolver is asked about is a
            // SPARE -- but the resolver matched on signature alone, and an
            // equipped plain item (a list holding only ExtraWorn) hashes to the
            // same value as any other list carrying none of the six extras
            // InstanceSig looks at (a favourited or stolen spare). Selling or
            // trashing the spare could then hand the engine the equipped list
            // instead: the item in the player's hand gets sold, or destroyed.
            // Everything that genuinely wants the worn list goes through
            // WornExtraOf() instead, so excluding it here costs nothing.
            const auto worn = [a_allowWorn](const RE::ExtraDataList* a_xl) {
                if (a_allowWorn) return false;
                auto* xl = const_cast<RE::ExtraDataList*>(a_xl);
                return xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>();
            };
            if (a_uid != 0) {
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl || worn(xl)) continue;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                        xu && xu->uniqueID == a_uid) return xl;
                }
                return nullptr;
            }
            // GI39: a PLAIN unit (sig 0) can still live in a list. The engine
            // groups identical units into ONE ExtraDataList carrying a count, so
            // "no signature" never meant "no list" -- this used to bail out at
            // sig 0 and hand RemoveItem a nullptr. The engine then picked for
            // itself, and with a tempered spare of the same form in the bag it
            // could walk THAT out: the plain dagger you clicked stayed put and
            // the tempered one left. Worn lists are excluded just above, which
            // is the only reason sig 0 was unsafe to match on before.
            for (auto* xl : *a_entry->extraLists) {
                if (!xl || worn(xl) || InstanceSig(xl) != a_sig) continue;
                // GI42: a uid unit is the sole member of its own pool ("@uid"),
                // so it can never be the answer to a sig- or plain-pool request.
                // The star-clearing loop in ResolveExitUnit already had this
                // filter -- same concept, one implementation now.
                if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                    xu && xu->uniqueID != 0) continue;
                return xl;
            }
            return nullptr;   // genuinely listless (or every candidate is worn)
        }

        // Same, starting from the player's live InventoryChanges (the engine's
        // OWN entry -- GetInventory hands out copies, so mutations there are
        // silently discarded).
        RE::InventoryEntryData* LiveEntry(RE::TESObjectREFR* a_owner,
                                          RE::TESBoundObject* a_obj)
        {
            if (!a_owner || !a_obj) return nullptr;
            auto* changes = a_owner->GetInventoryChanges();
            if (!changes || !changes->entryList) return nullptr;
            for (auto* e : *changes->entryList) {
                if (e && e->object == a_obj) return e;
            }
            return nullptr;
        }

        // GI43: a TILE's engine-true value (temper folded in like vanilla).
        // GI44: resolved by POOL (uid, sig), never by recorded position -- the
        // tile was collected at rebuild time and list positions drift, and the
        // sale itself already resolves by pool, so the price must match it.
        int TileValue(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig)
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            return UnitValueWith(a_obj,
                ExtraForPoolImpl(LiveEntry(p, a_obj), a_uid, a_sig));
        }


        // forward decl: g_layout defined below; NextTileKey returns the lowest
        // unused "#k" ordinal for a form (k==0 is the bare baseKey). Used when a
        // split fragment lands on an empty cell and needs its own persistent tile.
        std::map<std::string, LayoutEntry>& Layout();
        std::string PlacePin(int a_value, int a_col, int a_row, const std::string& a_bag);  // G4 (defined later)
        // ★★The pool a SLOT belongs to, read from the slot's hints rather
        // than decoded out of its name. This is the whole redesign in one
        // function: the pool is still a real and useful grouping -- it is what
        // "these units are interchangeable" means, and stackable tiles genuinely
        // have to split along it -- but it is now DERIVED from what the slot is
        // showing, so it can change without the slot being renamed.
        //
        // The fallback matters: a key from before 1.3.2 (or from a save) still
        // carries its pool in its name, and a slot that has already been erased
        // has nothing else to answer with. Parsing the name is exactly right for
        // both, and wrong for nothing, because a new key never has a suffix.
        std::string PoolOfKey(const std::string& a_key);
        std::string PoolOfSlot(const std::string& a_key)
        {
            auto& layout = Layout();
            const auto it = layout.find(a_key);
            if (it == layout.end()) return PoolOfKey(a_key);
            if (it->second.uid == 0 && it->second.sig == 0) {
                // no hint yet (a legacy entry, or one loaded from a save before
                // the first rebuild): its NAME is still the best evidence.
                return PoolOfKey(a_key);
            }
            return PoolPrefix(BaseKey(a_key), it->second.uid, it->second.sig);
        }

        // fwd: the coin-record verb lives with its S0 siblings below, but the
        // bag-store manifest walk (StoreBagContents) needs it first
        void SetCoinRecord(const std::string& a_key, int a_value);

        std::string NextTileKey(const std::string& a_baseKey)
        {
            auto& layout = Layout();
            // a key is taken if it's placed OR reserved by a pin not yet placed
            auto taken = [&](const std::string& k) {
                return layout.contains(k);   // ★S-G: every coin tile has a record
            };
            if (!taken(a_baseKey)) return a_baseKey;
            for (int k = 1;; ++k) {
                const std::string cand = a_baseKey + "#" + std::to_string(k);
                if (!taken(cand)) return cand;
            }
        }

        // "unique" = has a DESC description (artifacts). The lookup walks the
        // string tables, so cache the verdict per form for the session.
        // ★★★"UNIQUE" IS "CANNOT BE DISENCHANTED". Measured against Skyrim.esm
        // rather than assumed, because the old test -- "has a non-empty DESC" --
        // was picking a set that has nothing to do with artifacts:
        //
        //   criterion                    weapons hit (of 2484)   Dawnbreaker
        //   DESC non-empty (old)                33                   NO
        //   enchanted, no base ench            113                   YES
        //
        // Every WEAP record HAS a DESC subrecord and its string id is 0 --
        // Dawnbreaker, Volendrung and a plain iron dagger are indistinguishable
        // by it. The 33 it did hit are silver swords, the Akaviri katanas and
        // Nightingale gear: Skyrim uses DESC for "this item needs a sentence of
        // explanation", not for "this item is an artifact".
        // The new test catches Dawnbreaker, Volendrung, Mehrunes' Razor, the
        // Mace of Molag Bal, the Ebony Blade, Chillrend, Keening and the
        // Nightingale weapons. A player-made enchantment always has a base
        // (the one they learned), so it never qualifies.
        // ★STAVES ARE EXCLUDED. All 64 of them are undisenchantable, so among
        // staves the signal carries no information at all -- it would light up
        // every staff in the game to flag the handful that are special.
        // ★Known misses, and they are structural: Nettlebane has NO enchantment
        // (its effect is scripted) and the Blade of Woe's enchantment does have
        // a base. Nothing in the record separates those from ordinary gear.
        // ★★NAMED EXCEPTIONS, because some artifacts carry NOTHING in their
        // record that separates them from ordinary gear:
        //   Nettlebane      no enchantment at all -- its effect is scripted
        //   Blade of Woe    its enchantment does have a base (x2: the reward
        //                   copy and the one Astrid carries)
        // No test over the record can reach these, so they are named. Resolved
        // through the data handler rather than compared as raw FormIDs, so the
        // load order may put Skyrim.esm wherever it likes.
        // ★Rebuilt until the handler exists: caching an empty set on a call
        // that happened before data load would make the list permanently dead.
        // Declared in GridInventory_unique.ini and pushed in by main.cpp.
        std::unordered_map<RE::FormID, bool> g_uniqueOverride;
        bool                                 g_uniqueLoaded = false;
        // ★The verdict, cached per form. At namespace scope rather than inside
        // the query, because a reload of the file has to be able to drop it.
        std::unordered_map<RE::FormID, bool> g_uniqueCache;
        void InvalidateUniqueCache() { g_uniqueCache.clear(); }

        bool IsUniqueCached(RE::TESBoundObject* a_obj)
        {
            if (const auto it = g_uniqueCache.find(a_obj->GetFormID());
                it != g_uniqueCache.end()) {
                return it->second;
            }
            const auto* ef = a_obj->As<RE::TESEnchantableForm>();
            const auto* en = ef ? ef->formEnchanting : nullptr;
            bool uniq = en && !en->data.baseEnchantment;
            if (uniq) {
                if (const auto* w = a_obj->As<RE::TESObjectWEAP>()) {
                    // staves: all 64 are undisenchantable, so the signal is
                    // constant among them and says nothing
                    if (w->GetWeaponType() == RE::WEAPON_TYPE::kStaff) uniq = false;
                    // ★BOUND weapons pass the test for a reason that has nothing
                    // to do with rarity -- a conjured sword's enchantment is not
                    // one you could ever learn. Six of them (sword / bow /
                    // battleaxe, each with a Mystic variant) were coming up red.
                    if (w->IsBound()) uniq = false;
                }
            }
            // ★The FILE wins, both ways: it can name a unique the record cannot
            // describe, and it can switch off one the rule got wrong.
            if (const auto ov = g_uniqueOverride.find(a_obj->GetFormID());
                ov != g_uniqueOverride.end()) {
                uniq = ov->second;
            }
            // ★Nothing is cached until the overrides have been loaded. Asked
            // before that, every declared unique would come back "ordinary" --
            // and STAY that way for the session, which is the whole failure the
            // file exists to prevent.
            if (g_uniqueLoaded) g_uniqueCache.emplace(a_obj->GetFormID(), uniq);
            return uniq;
        }

        // G3: Mabinogi stacking — units per TILE. 1 = never stacks (equipment
        // incl. rings, weapons, gold coins); consumables/materials stack up to
        // a per-category cap and spill into extra tiles beyond it.
        // F-key favorite toggle. GetInventory returns entry COPIES —
        // SetFavorite must mutate the engine's OWN entry, so walk
        // InventoryChanges::entryList for the real one.
        // D3: this took extraLists->front(), and AddExtraList pushes FRONT --
        // so front() is "whatever was given a personality most recently".
        // Pressing F on a plain sword put the star on the enchanted one sitting
        // next to it. The tile now names its own sub-stack; nullptr means a
        // plain unit, which is exactly what the no-lists case always passed.
        // Defined after g_layout / FormKey (it edits our own per-tile flag).
        void ToggleFavorite(const std::string& a_key, RE::TESBoundObject* a_obj,
                            std::uint16_t a_uid, int a_xlIdx, std::uint16_t a_sig);

        int StackCapOf(RE::TESBoundObject* a_obj)
        {
            const RE::FormID fid = a_obj->GetFormID();
            if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) return 1;
            // ★★VANILLA GOLD IS A STACK WITH A BIG CAP, and saying so here is
            // what lets the container side stop treating money as a special
            // case -- the cap-sized banding it used to do by hand is just this
            // number going through the ordinary rule.
            //
            // ★It answers for exactly ONE board. The player's own never sees
            // this form: the rebuild takes Gold001 out as the ledger and shows
            // the coin mirror in its place. A chest is where the gold really is
            // Gold001, and where it really does need to sit a capful to a cell
            // for the same reason a hundred arrows do. Without this line it
            // falls through to the misc default and a chest banks the player's
            // fortune twenty septims at a time.
            if (a_obj->IsGold()) return GoldCoins::kCoinCap;
            if (a_obj->Is(RE::FormType::Armor) ||
                a_obj->Is(RE::FormType::Weapon)) return 1;
            if (a_obj->Is(RE::FormType::Ammo)) return 100;          // arrows/bolts
            if (a_obj->Is(RE::FormType::AlchemyItem)) return 10;    // potions/food
            if (a_obj->Is(RE::FormType::Ingredient)) return 10;
            if (a_obj->Is(RE::FormType::Scroll)) return 10;
            if (a_obj->Is(RE::FormType::SoulGem)) return 10;
            if (a_obj->Is(RE::FormType::Book)) return 10;
            return 20;   // misc: ores, ingots, leather, gems, keys...
        }

        int MaskCells(const std::vector<std::vector<bool>>& a_rows)
        {
            int n = 0;
            for (const auto& r : a_rows) {
                for (bool c : r) {
                    if (c) ++n;
                }
            }
            return n;
        }

        DefResolver                                    g_resolver;

        // ONE tile holds at most this many units (Phase 2: the former 8-site
        // `(baseCap>1 && stack>0) ? stack : baseCap` copies converge here):
        // gear/coins = 1, else the editor per-item override (stack:N) if any,
        // else the category cap.
        int EffectiveCap(RE::TESBoundObject* a_obj, const GridDef& a_def)
        {
            // ★A container never stacks. A bag's tile IS the container — its
            // identity is what holds the contents (LayoutEntry::bag names this
            // key) — so two of them sharing one tile would make "which bag" a
            // question with no answer. The coin pouch is the same story: it
            // carries a stored amount of its own.
            if (a_def.bag != 0) return 1;
            if (a_obj && GoldCoins::IsPouch(a_obj->GetFormID())) return 1;
            const int baseCap = StackCapOf(a_obj);
            return (std::max)(1, (baseCap > 1 && a_def.stack > 0) ? a_def.stack : baseCap);
        }
        int EffectiveCap(RE::TESBoundObject* a_obj)
        {
            return EffectiveCap(a_obj, g_resolver ? g_resolver(a_obj) : GridDef{});
        }

        std::vector<Item>                              g_items;

        // ★★A VIEW HOLDS INDICES INTO g_items, NOT COPIES. Anything that
        // rebuilds the board between a view being built and being drawn leaves
        // those indices pointing past the end -- and reading past the end of a
        // vector hands back memory that still LOOKS like an Item, with an `obj`
        // that still looks like a pointer. It survives every null check and
        // dies later, deep in the icon path, on the first read of a vtable.
        //
        // That is exactly the shape of a CTD reported while using items
        // straight out of a container (skyrim_cast<TESModel*> ->
        // __non_rtti_object, inside DrawBagWindows). Whether a stale index is
        // really the source is not yet proven -- so this makes it SAYABLE:
        // out of range is skipped and named, instead of being undefined.
        const Item* ItemAt(int a_idx)
        {
            if (a_idx >= 0 && a_idx < static_cast<int>(g_items.size())) {
                return &g_items[a_idx];
            }
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                SKSE::log::error("[GRID] view index {} is past the board ({} items) "
                                 "-- the board was rebuilt while a view was being "
                                 "drawn. Tile skipped.", a_idx, g_items.size());
            }
            return nullptr;
        }
        std::vector<View>                              g_views;    // [0] = main
        std::map<std::string, LayoutEntry>             g_layout;
        std::map<std::string, LayoutEntry>& Layout() { return g_layout; }

        // ★(1.3.0-D) a bag committed to a container takes its CONTENTS along:
        // queue each content tile's store and hand the manifest to the shelf
        // (LootBarter parks it until the bag's spot is born). Coin tiles are
        // mirror artefacts and never travel; a nested bag stays behind,
        // matching the "no bag inside a bag" rule everywhere else.
        // ★★★A BAG TRAVELS AS A SUBTREE, not as one layer.
        //
        // This used to skip nested bags outright (`if (it.def.bag != 0)
        // continue`), so putting a bag inside a bag and then storing the outer
        // one left the inner one -- and everything in it -- behind on the
        // board. Nesting is a tree; storing a bag has to move the whole branch.
        //
        // The walk is a STACK, not recursion: the depth is the player's data,
        // and user data does not belong on the call stack.
        //
        // Every entry records the NAME of the bag it sits in -- an id minted
        // once and never rewritten -- so nothing in the manifest depends on the
        // order it was written in, or on where an entry ends up once it lands.
        // The walk still happens to be parent-first, which is all the restore
        // needs to have a parent's tile ready when its children are read (see
        // ClaimIncomingBundles).
        void StoreBagContents(const std::string& a_bagKey, RE::TESBoundObject* a_bagObj)
        {
            std::vector<LootBarter::BundleItem> manifest;
            // (bag key, the id of that bag's entry; 0 = the bag being stored,
            // which has no entry of its own)
            std::vector<std::pair<std::string, std::uint32_t>> todo{ { a_bagKey, 0u } };
            std::set<std::string> seen{ a_bagKey };   // a cycle cannot form, but
                                                      // a corrupt save must not spin
            while (!todo.empty()) {
                const auto [bagKey, parent] = todo.back();
                todo.pop_back();
            for (auto& it : g_items) {
                if (it.inBag != bagKey || !it.obj) continue;
                if (it.coinValue >= 0) {
                    // ★(1.5.x) GOLD RIDES IN THE BAG NOW (it used to stay
                    // home and fall out of the store -- reported). A coin
                    // tile is a ledger mirror with no engine item, so its
                    // value stores into the container as physical Septims
                    // and the manifest books a GOLD ENTRY at its anchor --
                    // the same shape the open-bag window's intake makes.
                    auto* dst = LootBarter::Partner();
                    auto* vg = GoldCoins::VanillaGold();
                    if (!dst || !vg || it.coinValue <= 0) continue;
                    const int moved =
                        GoldCoins::StoreToContainer(dst, it.coinValue);
                    if (moved <= 0) continue;   // refused: the tile stays home
                    LootBarter::NoteStoredUnits(vg, moved);
                    if (moved < it.coinValue) {
                        SetCoinRecord(it.key, it.coinValue - moved);
                    } else {
                        g_layout.erase(it.key);
                    }
                    LootBarter::BundleItem gbi{ vg->GetFormID(), moved, 0,
                                                it.col, it.row, it.rot & 3,
                                                0, false, parent };
                    gbi.id = LootBarter::MintBundleId();
                    manifest.push_back(std::move(gbi));
                    SKSE::log::info("[GOLD] {} G rides in the stored bag",
                                    moved);
                    continue;
                }
                // ★the entry's name, minted before anything can refer to it
                const std::uint32_t bid = LootBarter::MintBundleId();
                if (it.def.bag != 0) {
                    // ★A nested bag is BOTH an entry and a branch: it is stored
                    // like any other item, and its own contents are walked next
                    // with this entry's NAME as their parent.
                    if (!seen.insert(it.key).second) continue;
                    todo.push_back({ it.key, bid });
                }
                LootBarter::RequestStore(it.obj, it.count, it.uid, it.sig, it.fav,
                                         it.xlIdx, it.key);
                NotePendingRemove(it.obj, it.key, it.count, it.xlIdx);
                // ★(1.3.2) the tile's marker bits ride along; the favourite
                // star does NOT -- RequestStore's fav argument strips it as
                // the unit leaves, so a stored item is never starred.
                // ★★AND WHERE IT SAT. The manifest has had anchor fields since
                // v5 and this filled them with -1 -- "unplaced" -- so a bag put
                // in a chest came home with its contents first-fit into a
                // stranger's order. The player arranged that interior; a trip
                // through a container is not a reason to undo it. (Reported as
                // "the bag works, but the items inside get rearranged".)
                LootBarter::BundleItem bi{ it.obj->GetFormID(), it.count,
                                           it.sig, it.col, it.row, it.rot & 3,
                                           it.glow, it.stolen, parent };
                bi.id = bid;
                // ★(1.5.x) a POUCH entry is stamped with ITS tile's amount,
                // so the shelf claim can pick the exact parcel -- two
                // same-form pouches in one store used to trade amounts
                // (first-come parcels), which read as a position swap.
                if (GoldCoins::IsPouch(bi.form)) {
                    bi.wantGold = GoldCoins::PouchStoredOf(it.key);
                }
                manifest.push_back(std::move(bi));
            }
            }
            if (!manifest.empty() && a_bagObj) {
                LootBarter::NoteBagBundle(a_bagObj->GetFormID(), std::move(manifest));
            }
        }

        // E4b: bags may nest inside GENERAL bags (manual placement only).
        // The one thing that must never form is a containment loop — walk
        // a_outer's chain of containers upward; hitting a_inner means the
        // drop would put a bag (transitively) inside itself.
        [[nodiscard]] bool NestsWithin(const std::string& a_inner, std::string a_outer)
        {
            for (int guard = 0; guard < 16 && !a_outer.empty(); ++guard) {
                if (a_outer == a_inner) return true;
                const auto it = g_layout.find(a_outer);
                a_outer = it == g_layout.end() ? std::string{} : it->second.bag;
            }
            return false;
        }
        // ★Typed bags: tile keys minted during THIS rebuild — i.e. things that
        // just entered the inventory. Rebuilt every pass, never persisted:
        // being new is a property of this frame, not of the save.
        std::vector<std::string>                       g_freshTiles;
        // ★★ARRIVALS THAT ARE ONLY PASSING THROUGH. Shelf USE MODE takes one
        // unit and consumes it in the same breath -- the item is in the pack for
        // a frame or two and then gone. Routing such a unit into a typed bag
        // means building a tile inside a bag view and destroying it again
        // immediately, which is churn for something the player never sees.
        //
        // It is also the only difference between the cases that crashed and the
        // ones that did not: a potion has a typed bag and is claimed; a book or
        // a spell tome has none and is not. Two potions drunk out of a container
        // in a row took the game down inside DrawBagWindows every time, and
        // nothing else did.
        // ★A small TTL in FRAMES (it was rebuilds -- but S4 made rebuilds
        // rare, and a lingering entry would swallow a real purchase of the
        // same form later). The take and the use land on different ticks, so
        // the entry has to outlive both; the sweep runs in CapacityTick.
        // ★(1.5.x) `suppressed`: OnFormDelta swallowed a delta for this form
        // (no tile minted -- the tome blink). If the unit is still there when
        // the TTL runs out (the use was refused), one rebuild surfaces it.
        struct TransientArrival
        {
            int  frames = 90;
            bool suppressed = false;
        };
        std::map<RE::FormID, TransientArrival>         g_transientArrivals;

        // ★★★VALIDATING A POINTER WITHOUT TOUCHING IT.
        //
        // A tile's `obj` has been reaching the icon path as something that is
        // not a form: readable memory whose vtable is not a game vtable, so the
        // first VIRTUAL call on it (Is() -> GetFormType()) jumps to 0x1 and the
        // process dies. Four hypotheses have now failed to explain where it
        // comes from -- a stale view index (ruled out: the guard never fired),
        // the engine's loadedModels list, a dead form, typed-bag routing (ruled
        // out: it still crashed with the routing suppressed).
        //
        // Every check so far had to DEREFERENCE the suspect to learn anything,
        // which is the very act that crashes. This does not: every object that
        // goes onto the board comes from the player's own inventory during
        // Rebuild, so recording those pointers gives a set to test membership
        // in -- an integer comparison, no read of the object at all.
        //
        // If a tile's obj is not in this set, the Item was written after the
        // rebuild that made it. That is the answer we have not been able to get
        // any other way, and it is logged with the tile's own key.
        std::unordered_set<const void*>                g_liveObjs;

        bool ObjLooksLive(const void* a_obj)
        {
            return a_obj && g_liveObjs.contains(a_obj);
        }
        // ★(1.3.2) EVERY tile minted this rebuild, hand-placed ones included.
        // g_freshTiles is the ROUTING list and a tile placed by the player's
        // own hand is struck off it (the drop hint wins over the filters) --
        // but "did this just arrive" is a different question, and the bundle
        // claim asks that one. Dragging a stored bag home always sets a drop
        // hint, so the bag was never in g_freshTiles and its contents spilled
        // onto the main board instead of going back inside it.
        std::vector<std::string>                       g_arrivedTiles;
        // ★Typed bags: which filters the player currently has a USABLE bag for
        // — a copy parked in the trash or riding the cursor does not count
        // (no slot can route to it, so treating it as held just fragments the
        // main board into unmergeable half-stacks). Used by the stackable
        // arrival path to decide whether new units may top up an existing
        // pile or should start a fresh tile the claim can route.
        std::set<std::string>                          g_typedBagsHeld;
        // ...and every bag FORM's accept ("" = general purpose), so the fill
        // loop can tell "a pile inside a bag of the item's own kind" (top up)
        // from "a pile inside some other bag" (leave it as the player left it).
        std::map<std::string, std::string>             g_bagAcceptByForm;
        // ★...and which of those had no room last pass. Skipping the main pile
        // only pays off if the bag can actually take the arrival; with a FULL
        // bag it would start a new tile every pickup, each one bouncing back to
        // main, and the board would fill with half-stacks of the same ore. One
        // rebuild of lag is harmless here — it self-corrects the moment the bag
        // has space again.
        std::set<std::string>                          g_typedBagFull;
        std::set<std::string>                          g_openBags; // remembered (E2)
        std::unordered_set<std::string>                g_prevKeys;

        // ---- GI65: "new since you last looked" -----------------------------
        // ★A tile is new when BOTH are true: its key did not exist last rebuild,
        // AND the form's total count went up. Either alone is wrong -- splitting
        // a stack makes a key without gaining anything, and topping up arrows
        // 50 -> 70 gains without making a key. Requiring both is what lets this
        // ignore every in-inventory rearrangement without listing them.
        std::unordered_set<std::string> g_newTiles;    // marked right now
        std::unordered_map<RE::FormID, int> g_seenCount;   // counts as of the last look
        bool g_seenValid = false;   // false until a snapshot exists (fresh game / load)
        bool g_suppressNew = false; // one rebuild after a load: everything looks new
        // ★Debug switch for the [POOL]/[FAV]/[XL]/[CHECK]/[FLICK] diagnostics.
        // Ships OFF, and the call sites stay wired on purpose: these self-checks
        // are what turned "a spare vanished" and "it flickers" into a log line
        // naming the pool, so rebuilding them from scratch at the next report
        // would cost far more than the dead branch does. Everything expensive
        // (the per-tile string keys especially) lives INSIDE the guard.
        // ★DIAGNOSTIC BUILD SWITCH. Shipped OFF; the diagnostic package flips
        // it to true and nothing else. It cannot be done through the ini for the
        // people who need it -- GridInventory_ui.ini is written by the plugin at
        // runtime, so an existing player already has one in their overwrite and
        // it shadows anything a mod folder ships. Compiling the default in is
        // the only form of the switch that reaches them.
        bool g_poolTrace = false;

        // *TEST ONLY ("!simdrift = 1" in GridInventory_ui.ini), ships OFF.
        // Hands the carry-exclusion a DELIBERATELY WRONG identity -- the
        // state a signature drift leaves the board in. With this on, picking
        // a weapon up should STILL take it off the board; if it does not, the
        // carry fallback in EnumerateUnitRefs is not covering the case.
        bool g_simDrift = false;

        // GI32: favourite syncs waiting for the game thread (see ToggleFavorite)
        struct FavSync
        {
            RE::TESBoundObject* obj = nullptr;
            std::uint16_t       uid = 0;
            int                 xlIdx = -1;
            // ★The doll and the drawer know a unit by uid+sig and never by a
            // list index -- a worn unit owns no cell, so there is no position
            // to record. ProcessFavorites falls back to the pool with this.
            std::uint16_t       sig = 0;
            // ★★The doll's request names a unit ON THE BODY. Pool resolution
            // refuses worn lists by design (a sale must never resolve to the
            // hand), so without this flag the request fell through to
            // SetFavorite(entry, nullptr) -- and with only the worn list in
            // the entry, the engine minted a fresh {Hotkey} list: a phantom
            // unit the board drew as a second, fully-charged copy of a
            // once-fired weapon. The hand disambiguates a copy in each fist.
            bool                worn = false;
            int                 hand = 0;
        };
        std::vector<FavSync> g_favSync;

        // ★[FAV] TRIPWIRE (unconfirmed user sighting: "the star vanished
        // after the charge drained", seen once, never reproduced). Every
        // legitimate star removal has a witness that files the form here
        // before the next rebuild -- a queued toggle (ProcessFavorites), an
        // outbound unit (ResolveExitUnit, rule 58), the phantom heal. A form
        // still IN the inventory whose last hotkey is gone with NO witness
        // means something outside this plugin stripped it; the standing
        // suspect is the engine's ExtraCharge writeback onto the worn list,
        // which measurably rewrites that list during combat (three census
        // relabels in one session). Cheap enough to stay on for good.
        std::unordered_set<RE::FormID> g_starMemo;
        std::unordered_set<RE::FormID> g_starChangeOk;
        bool                           g_starMemoValid = false;

        // ★★★FORMS A CLICK ALREADY TOOK OFF THE BOARD, waiting for the engine's
        // equip event to catch up.
        //
        // The equip side of the event sink swallows a declined partial update,
        // and the reason is sound as far as it goes: a click-path equip removed
        // the tile optimistically the moment it was clicked, so there is
        // nothing left for a rebuild to do, and a torch's "still worn" decline
        // must not start one. What that reasoning never covered is an equip
        // that did NOT come from a click -- the quick wheel, a hotkey, a
        // script. Nothing removes the tile for those, and the decline that
        // would have said so is thrown away, so the tile stands until some
        // unrelated rebuild happens past it. Measured in a session log: a
        // wheel equip of an iron helmet, no removal, decline swallowed, and
        // seven seconds of luck before something else rebuilt.
        //
        // So the sink stops guessing and asks. A click that really removed a
        // tile leaves its form here; the sink claims it and stays quiet.
        // Nothing to claim means nothing removed the tile, and the decline is
        // escalated after all.
        std::unordered_set<RE::FormID> g_optimisticGone;
        // Last drawn set per gear pool, so a rebuild that changes what is on the
        // board can say so. A flicker is a change that comes back -- invisible
        // to the conservation check, which only sees one frame at a time, but
        // obvious as "2 -> 1 -> 2" in a transition log.
        // (Read only under g_poolTrace; it was pencilled in as TEMPORARY and has
        // outlived several bug hunts since.)
        std::unordered_map<std::string, std::string>   g_flickPrev;
        std::unordered_map<RE::FormID, int>            g_values;   // Phase 4: form -> GetValue (barter)
        // ★★Find by name, WITHOUT filtering. A filter that hides the misses
        // would also erase the thing a grid inventory is for — remembering
        // where you put something. Matches keep their place and everything
        // else dims, so the board you learned stays the board you see.
        // ★Names are compared ONCE per board change, never per frame: the
        // result is a set of keys, and drawing only asks the set. Lower-casing
        // every tile's name each frame would be exactly the per-frame string
        // work the render path must not do.
        std::string                     g_search;        // lower-cased, "" = off
        std::unordered_set<std::string> g_searchHit;     // keys that match
        std::uint32_t                   g_boardVersion = 0;   // bumped by Rebuild
        std::uint32_t                   g_searchVersion = 0;  // what the set was built from
        int                                            g_gold = 0;
        std::optional<Held>                            g_held;
        DropTarget                                     g_target;

        // ---- ★PLAN_SPACE_AUTHORITY S0: the board verbs ---------------------
        //
        // A verb updates exactly the tiles it names and maintains the §4-2
        // books by hand; anything it cannot PROVE declines into the caller's
        // existing RequestRebuild -- the OnFormDelta doctrine applied to the
        // board's own gestures. Every verb answers false for "I could not do
        // this provably", and the caller's rebuild is the unchanged fallback,
        // so a verb can never be less correct than the path it replaces.
        //
        // §4-2 checklist a mutating verb settles (measured off FinalizeRebuild):
        //   1. g_layout col/row/bag/count/rot     (the caller's PlaceTile, or here)
        //   2. new marks: only MINTING paths touch g_newTiles/g_prevKeys
        //   3. g_spaceUsed increment/decrement + MarkCapacityDirty
        //   4. g_liveObjs: derived at rebuild; a stale member is permissive
        //   5. g_boardVersion++ when the tile SET changes (search)
        //   6. icon queue: only minting paths (draw-time queue self-heals)
        //   7. pool hints (uid/sig) ride the Item / the carry
        //   8. coins: NEVER through these verbs (§7) -- callers keep the tail
        //
        // ★THREAD/PHASE RULE: g_items/g_views mutate ONLY outside the draw
        // loop (FinishFrame, Tick, input handlers that already run Rebuild()
        // directly). The one in-draw gesture, the C1 lift, defers through
        // Held::needsDetach and is executed at FinishFrame -- the same one-
        // frame latency the deferred rebuild always had.

        // The tile riding the cursor, OFF the board -- the cursor space's
        // board-side half. Erasing at lift and re-inserting at put keeps every
        // g_items consumer (capacity B5, coin partition, pouch lists) seeing
        // exactly what the rebuild model showed them: a carried tile is not on
        // the board. The layout entry stays put (cancel returns home).
        std::optional<Item> g_stash;

        bool StashTileForCarry(const std::string& a_key);            // Lift
        bool UnstashTileTo(const std::string& a_bag, int a_col, int a_row,
                           int a_rot, int a_count);                  // Put
        bool UnstashTileHome();                                      // cancel
        void DiscardStash(const char* a_why);                        // Exit spent it
        void RefreshPoolFlagsFor(RE::TESBoundObject* a_obj);         // star/stolen/quest, in place
        bool SetTileDisplayCount(const std::string& a_key, int a_count);   // Merge/split
        // Park/Restore: move ONE tile between views (a_col < 0 = first-fit;
        // an occupied remembered spot degrades to first-fit, restore's rule).
        bool MoveTileToView(const std::string& a_key, const std::string& a_bag,
                            int a_col, int a_row);
        // ...and its in-draw deferral: a right-click park fires inside the
        // tile loop, where g_items/g_views must not move. Queued here, run at
        // FinishFrame before the drop resolution; a failed move rebuilds.
        struct ViewMoveReq
        {
            std::string key;
            std::string bag;
            int         col = -1;
            int         row = -1;
        };
        std::vector<ViewMoveReq> g_viewMoveQ;
        void RunQueuedViewMoves();
        // ★S1: the trash button fires mid-draw; the (always empty) trash view
        // is appended at FinishFrame instead of by a rebuild.
        bool g_wantTrashView = false;
        // ★S2: the expiry's one-tile recovery (defined with the verb bodies)
        bool ReEmitTileAt(std::uint32_t a_form, const std::string& a_key);
        // ★S-G: the layout book's coin accessors -- the pin API's successors,
        // now that every coin tile owns its amount on its slot. A fresh record
        // is born UNPLACED (col -1), or the placer guard never fires and it
        // sits down at [0,0] on top of whatever lives there (the mint trap).
        [[nodiscard]] int CoinRecordOf(const std::string& a_key)
        {
            const auto li = g_layout.find(a_key);
            return li == g_layout.end() ? -1 : li->second.coin;
        }
        void SetCoinRecord(const std::string& a_key, int a_value)
        {
            const auto li = g_layout.find(a_key);
            if (a_value <= 0) {
                if (li != g_layout.end()) li->second.coin = -1;
                return;
            }
            const int v = (std::min)(a_value, GoldCoins::kCoinCap);
            if (li != g_layout.end()) {
                li->second.coin = v;
                return;
            }
            auto& le = g_layout[a_key];
            le.col = -1;
            le.row = -1;
            le.count = 1;
            le.coin = v;
        }
        // occupancy of one view (defined with the verb bodies; S3's Enter
        // seats fresh units into bag views through it)
        std::vector<std::vector<bool>> ViewOccOf(const View& a_v, int a_skipIdx = -1);
        // One-shot: the STACK TILE a unit was just taken from. Stackables have no
        // per-unit identity, so rule 2-B decides it -- the cell the player acted
        // on is the cell that gives one up. Without this the drain took from
        // whichever tile sorted last, so equipping the single torch you had just
        // set down in front emptied the stack behind it instead.
        struct DrainHint { std::string baseKey; std::string key; };
        DrainHint g_drainHint;

        // ★B3 symmetric half: the use/equip CLICK's board-side work, deferred
        // to FinishFrame (the click fires mid-draw, and mutating g_items there
        // breaks the very loop that heard it -- the reason this used to be a
        // RequestRebuild). The !rbdrop interrogation of the right-click tail
        // (§3-6) named that rebuild's only use/equip job as "draw the
        // optimistic exit NOW"; this does exactly that job and nothing else.
        // FormID, not a pointer (원칙 2) -- resolved when consumed.
        // take: units leaving the clicked tile (0 = derive via EquipCountFor,
        // the use/equip legacy). drained: NotePendingRemove already adjusted
        // the LAYOUT count (store/sell paths) -- the partial must then sync
        // only the display, or the stack would be drained twice.

        // ★ONE PATH: the right-clicked tile and where it is bound, waiting for
        // FinishFrame.
        //
        // A drag never mutates the board from inside the tile loop either --
        // the pickup only names what is held, and ResolveDrop runs later, in
        // FinishFrame, where erasing items is safe. A click that wants the same
        // road has to wait in the same queue, so this holds the key until then.
        // (That deferral is why the old click-remove slot existed at all; this
        // is the same idea with the whole action in it rather than its
        // aftermath.)
        //
        // ★The DESTINATION travels with it as a small enum rather than as a
        // route pointer: the click site sits a few thousand lines above the
        // handlers, and naming the road it wants is clearer than reaching for
        // a function it cannot see yet.
        enum class ClickRoute : std::uint8_t { kUse, kStore, kSell, kPlant };
        struct ClickAction
        {
            std::string key;
            ClickRoute  route = ClickRoute::kUse;
        };
        std::optional<ClickAction> g_clickAction;

        // B2: one-shot placement hint for the next ACQUIRE that creates a new
        // tile of this form (partner-drop lands at the drop cell without a
        // premature layout entry). col<0 = no hint.
        // GI62: `rot` is the return leg of the same journey NoteStoreSpot makes
        // outbound -- a sword taken back out of a chest on its side lands on the
        // board on its side. Without it the turn survived only one direction.
        // ★★`pool` as well as `baseKey`. The hint says "the tile this arrival
        // mints goes HERE", and since tiles pool by sub-stack signature the
        // arrival belongs to one pool -- but the hint named only the form, and
        // the plain pool sorts first, so it took (or discarded) a hint armed for
        // a signed one and the signed tile first-fitted into the front gap.
        // Empty pool = "any", which is what a carry with no signature wants.
        struct DropHint { std::string baseKey; std::string pool; int col = -1; int row = -1;
                          std::string bag; int rot = 0;
            // ★HOW MANY were let go of. A drag says an amount as well as a
            // square, and without this the hint could only ever describe where
            // the LEFTOVER went -- see the placement in ACQUIRE. 0 = whatever
            // arrives (a single item, a buy, an older caller).
            int count = 0;
            [[nodiscard]] bool Wants(const std::string& a_base,
                                     const std::string& a_pool) const
            {
                return col >= 0 && baseKey == a_base && (pool.empty() || pool == a_pool);
            }
        };
        DropHint                                       g_dropHint;
        std::string                                    g_slotTarget;   // hovered equip slot (C6)
        // ★atomic: set from event threads (ContainerSink, host API), consumed
        // once per frame by FinishFrame on the main thread.
        std::atomic<bool>                              g_needRebuild{ false };

        // ---- F2: trash window (parked-for-deletion buffer) ----
        // Parked tiles are ordinary layout entries with bag == kTrashKey; the
        // engine inventory is untouched until deletion is CONFIRMED (window /
        // menu close, or FIFO eviction when the 6x4 board needs room).
        constexpr const char* kTrashKey  = "__trash";
        constexpr int         kTrashCols = 6;
        constexpr int         kTrashRows = 4;
        bool                                     g_trashOpen = false;
        // ★Defined with the other trash intake paths, far below;
        // the right-click handler that calls it is far above.
        bool RightClickIntoTrash(const Item& a_it);
        std::map<std::string, LayoutEntry>       g_trashReturn;   // key -> pre-park spot
        std::deque<std::string>                  g_trashOrder;    // FIFO, oldest first
        // GI25: a queued deletion names its POOL. Form + count alone let the
        // engine bin whichever copy it liked -- so emptying the trash could
        // destroy the tempered sword instead of the plain one parked there.
        struct TrashDelete
        {
            RE::FormID    form = 0;
            int           count = 0;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            bool          fav = false;   // GI36: carried to the deletion sink
            // *WHICH unit -- recorded when the tile was parked. Without it
            // the deletion resolves by pool and takes whichever list comes
            // first: bin the front dagger, lose the back one.
            int           xlIdx = -1;
            // ★Appended LAST (§10-6): the parked tile's key, so the ledger
            // request names the CELL and the engine's confirmation retires it
            // -- trash-parked gear was the one layout entry no prune path
            // ever reached (GI1 skips kTrashKey by design).
            std::string   key;
        };
        std::vector<TrashDelete>                g_trashDeleteQ;  // engine removals (Tick)
        struct TrashAsk   // favorite-intake confirm popup
        {
            bool                active = false;
            RE::TESBoundObject* obj = nullptr;
            std::string         key;
            int                 count = 0;
            int                 col = -1;
            int                 row = -1;
            int                 xlIdx = -1;   // which unit (see g_trashXl)
            // ★The carry's identity has to survive the popup round-trip too:
            // the tile snaps back while the question is on screen, so by the
            // time the answer arrives there is nothing left to ask.
            std::uint16_t       uid = 0;
            std::uint16_t       sig = 0;
            // ★★★AND THE TURN. Without it the confirm path re-derived the
            // footprint from the def alone -- upright -- while the drop that
            // opened the popup had been validated against the TURNED one. A
            // 1x4 weapon laid flat then asked the trash for room as if it were
            // still standing, found none across four rows, and TrashMakeRoomFor
            // answered by evicting parked items to make space that was already
            // there. Eviction here is a real RemoveItem: those items are gone.
            int                 rot = 0;
        };
        TrashAsk                                 g_trashAsk;

        // units of this form parked in the trash (they occupy no board space)
        int TrashedUnits(const std::string& a_baseKey)
        {
            int n = 0;
            for (const auto& [k, le] : g_layout) {
                if (le.bag == kTrashKey && BaseKey(k) == a_baseKey) {
                    n += (std::max)(1, le.count);
                }
            }
            return n;
        }
        std::function<void(RE::TESBoundObject*, bool)> g_sound;
std::function<void(RE::TESBoundObject*, int, RE::ExtraDataList*)> g_dropWorld;

        // Stable item key across load orders: "Plugin.esp|0xLocalID"
        //
        // A runtime-created form (player-brewed potion, player-enchanted weapon)
        // has NO source file, and GetLocalFormID() dereferences that file without
        // checking it -- so it must never be called for one. Such a form has no
        // local id anyway; its whole FormID is the identity.
        std::string FormKey(RE::TESForm* a_form)
        {
            auto* file = a_form->GetFile(0);
            std::string key = file ? std::string(file->GetFilename()) : "Dynamic";
            char buf[16];
            std::snprintf(buf, sizeof(buf), "|0x%06X",
                file ? a_form->GetLocalFormID() : a_form->GetFormID());
            return key + buf;
        }

        // P1: EVERY unit that is not on the board, in one list, named by identity.
        //
        // These used to be six separate corrections: worn was dropped by the walker
        // while the caller ALSO subtracted it from a scalar count, and reservations,
        // queued removals and trash parking were subtracted from that same scalar.
        // The walk then reconstructed "who to hide" from the DIFFERENCE between the
        // two -- so a scalar subtraction and a set removal for the same unit
        // CANCELLED, and the unit reappeared. That is why two identical items broke
        // everything and one identical item looked fine.
        //
        // Worn units are still handled by the walker's own skipWorn (they are never
        // in the set to begin with); everything else is named here.
        std::vector<OffBoardUnit> OffBoardUnitsFor(RE::TESBoundObject* a_obj,
                                                   const std::string& a_base)
        {
            std::vector<OffBoardUnit> out;
            if (!a_obj) return out;

            // (!simdrift) corrupt EVERY off-board identity, not just the
            // carry: a queued sale, a queued store and a trash park go through
            // the same exclusion and can be caught by the same drift.
            // ★★A uniqueID DOES NOT DRIFT. The engine assigns it once and it
            // stays; nothing in the game makes one vanish. Zeroing it here (the
            // original behaviour) did not model a hostile reality -- it modelled
            // an IMPOSSIBLE one, in which an off-board unit has no identity at
            // all. Placement survives that by design (its last tier keeps the
            // cells regardless). EXCLUSION cannot: "which unit is on the cursor"
            // has no safe fallback, only a guess, so the switch was manufacturing
            // symptoms that cannot occur in a real game and sending the
            // investigation after them.
            //
            // What the switch is FOR is the honest hazard: a signature that moved
            // because the ITEM moved -- a poison applied, a charge spent, a
            // grindstone. dS below still does exactly that.
            const auto dU = [](std::uint16_t u) -> std::uint16_t { return u; };
            const auto dS = [](std::uint16_t sg) -> std::uint16_t {
                // ★A LISTLESS UNIT CANNOT DRIFT. Its signature is 0 because
                // there is nothing to hash -- no temper, no charge, no
                // poison -- so corrupting it models a state the engine can
                // never produce, and sends the walk hunting for a pool that
                // has never existed. The test has to be hostile, not
                // impossible.
                return (g_simDrift && sg != 0)
                    ? static_cast<std::uint16_t>((sg ^ 0xA5A5u) | 1u) : sg;
            };

            // fromPartner: the cursor is holding the CONTAINER's item, not one of
            // ours. Subtracting it from the player's board hid a spare of the same
            // form for as long as the carry lasted -- take a dagger off a merchant
            // while you own three, and one of yours blinked out until you put it
            // down. The carry is the partner board's business, not this one's.
            if (g_held && g_held->obj && !g_held->fromPartner &&
                FormKey(g_held->obj) == a_base) {
                // A SWAPPED-OUT carry stops being worn the moment the equip that
                // displaced it lands -- and the worn list left behind belongs to
                // the unit that replaced it, which is identical in every respect.
                // Keeping mayBeWorn on made the carry match THAT list, so nothing
                // came out of the pack and the displaced dagger sat on the cursor
                // AND on the board. The in-flight equip is the only honest clock:
                // while it exists the swap has not completed.
                // ★★★ONLY A SAME-FORM SWAP NEEDS A CLOCK AT ALL.
                //
                // The hazard being guarded against is narrow: dagger swapped for
                // dagger, where the worn list left on the body after the equip
                // lands belongs to the unit that REPLACED us and is identical in
                // every respect. There, an in-flight equip of this same form is
                // the only honest signal that the swap has not completed.
                //
                // Swapping for a DIFFERENT form produces no such record -- and
                // reading its absence as "you are not worn any more" was simply
                // false: the displaced dagger is still on the body until the
                // engine takes it off. The carry could then match no worn unit,
                // fell through every exact tier, and was written off as "any
                // listed unit" -- removing a sibling, freeing its cell, and
                // putting a weapon the player never touched somewhere else.
                // (Measured: worn 1, "REMOVED as any listed unit", freed=1.)
                // ★A CARRIER lift was never engine-worn: no worn list is its
                // to claim, and claiming one anyway ate the FIRST ring's list
                // (same plain form) and leaked that unit onto the board.
                // ★B4-2c: the ledger's doffing entry IS the clock now. It
                // opens when the lift begins (BeginCarry -> NoteDoffing) and
                // closes when the engine's own unequip event lands -- every
                // lift shape, one rule. The pendingEquip scan it replaces
                // could only reason about the SAME-FORM SWAP (the one shape
                // that leaves an identical worn list behind) and answered
                // every other shape by assumption; the assumption was right,
                // but only because the swap was the only shape that ever
                // asked. A carrier lift stays out on its own flag -- it was
                // never engine-worn and has no doffing entry to consult.
                bool stillWorn = g_held->fromDoll && !g_held->fromCarrier &&
                                 g_held->obj &&
                                 WornLedger::Doffing(g_held->obj->GetFormID());
                // (!simdrift) both halves of the identity are corrupted, so
                // every exact path misses and only the carry fallback can
                // still take the unit off the board -- the thing under test.
                const std::uint16_t hUid = dU(g_held->uid);
                const std::uint16_t hSig = dS(g_held->sig);
                OffBoardUnit h;
                h.base      = a_base;
                h.uid       = hUid;
                h.sig       = hSig;
                h.why       = "held";
                h.mayBeWorn = stillWorn;
                h.hand      = g_held->hand;
                h.srcKey    = g_held->key;
                h.xlIdx     = g_held->xlIdx;
                out.push_back(std::move(h));
            }
            for (const auto& u : g_pendingEquip) {                     // equip queued
                if (u.base == a_base) out.push_back(u);
            }
            // held back by an INACTIVE preset: the preset recorded which unit
            for (const std::uint16_t sg : Loadout::ReservedSigs(a_obj->GetFormID())) {
                out.push_back({ a_base, 0, sg, "reserved" });
            }
            // engine removal queued (sold / stored / dropped) -- read from the
            // LEDGER since B4-3c: the request's own record, uid and sig
            // included (rule 2: the request is the only thing that knows the
            // unit). ★The list position no longer rides -- PLAN §7's rule,
            // identity carries over and coordinates are dropped. The xlIdx
            // tier this forfeits covered one shape: a signature drifting
            // DURING a removal window frames long; the sig and plain tiers
            // below answer everything else, and the audit rounds (18+10, two
            // phases) never caught the books apart.
            for (const auto& e : Ledger::OpenOutgoingOf(a_obj->GetFormID())) {
                for (int i = 0; i < -e.delta; ++i) {
                    OffBoardUnit r;
                    r.base  = a_base;
                    r.uid   = dU(e.uid);
                    r.sig   = dS(e.sig);
                    r.why   = "removing";
                    r.xlIdx = -1;
                    out.push_back(std::move(r));
                }
            }
            // parked in the trash: still owned, but occupies no board space
            for (const auto& [k, le] : g_layout) {
                if (le.bag != kTrashKey || BaseKey(k) != a_base) continue;
                // ★★...and it is not counted TWICE. A parked tile lifted onto
                // the cursor is already named above as the carry; naming it
                // again here asked the walk to take two units off the board
                // for one item, and the second request fell through to the
                // carry fallback -- which removed an innocent sibling. That
                // is why the dagger still in the pack jumped into the bin.
                if (g_held && k == g_held->key) continue;
                const auto ti = g_trashXl.find(k);
                const int  txl = ti == g_trashXl.end() ? -1 : ti->second;
                for (int i = 0, n = (std::max)(1, le.count); i < n; ++i) {
                    // ★★★READ THE SLOT, NOT ITS NAME. This asked the KEY which
                    // unit was binned -- and since 1.3.2 a key is a CELL NAME
                    // with no identity in it, so every trash record answered
                    // "uid 0, sig 0": a plain unit, whatever was actually
                    // thrown away. Binning a tempered dagger therefore removed
                    // a PLAIN one from the board, and the tempered one stayed
                    // -- "I trashed the tempered one and the plain one went
                    // in instead". (Measured: 14 trash records, every one of
                    // them uid 0000 sig 0000.)
                    //
                    // The slot has known since the rebuild that bound it.
                    OffBoardUnit t;
                    t.base  = a_base;
                    t.uid   = dU(le.uid);
                    t.sig   = dS(le.sig);
                    t.why   = "trash";
                    t.xlIdx = txl;
                    out.push_back(std::move(t));
                }
            }
            // ★THE SECOND RING. Its unit stands in the PACK while a carrier
            // wears its effect, so no worn list ever names it -- the stackable
            // branch subtracts it by hand (wornUnits += 1) but the GEAR walk
            // had no idea, listed it, and a pair of enchanted rings drew three
            // tiles for two units (user report: "복사된 걸로 보인다"). One
            // off-board unit, form-level identity: the carrier records only
            // the FORM, and the units it can stand in for are interchangeable
            // to the effect rule anyway. mayBeWorn=false -- the body never
            // wears this unit, so it must not consume a worn list.
            // ★...but NOT while an equip of this form is still in flight: the
            // pending-equip suppression already hides the unit on its way to
            // the carrier, and both at once hid a SPARE as well -- the
            // identical ring in the pack flickered for the rebuild or two
            // until the suppression died (user report). The entry takes over
            // when the suppression releases: a seamless handoff, one
            // exclusion at every moment.
            // ★...and not while the carrier's own unit RIDES THE CURSOR: a
            // fromCarrier carry IS that unit, already excluded as "held".
            // ★★That is ONLY the LIFT window, and the take-off flag is what
            // names it. The old test was object identity -- but units of one
            // form share one TESBoundObject, so "held == second" was also
            // true for a DISPLACED former second ring while its same-form
            // successor stood on the carrier, and this exclusion went dark:
            // the successor, still in the pack, drew on the board as a third
            // copy next to the cursor and the doll (user report). The
            // drop-swap window this guard once also covered is the
            // pending-equip guard's below -- the accepted drop always files
            // an entry (NotePendingEquip), and that suppression spans the
            // window until Wear has run.
            if (auto* second = DualRing::Second();
                second && FormKey(second) == a_base &&
                !(g_held && g_held->fromCarrier && g_held->obj == second &&
                  DualRing::TakeOffPending()) &&
                std::none_of(g_pendingEquip.begin(), g_pendingEquip.end(),
                             [&](const OffBoardUnit& u) { return u.base == a_base; })) {
                OffBoardUnit r;
                r.base = a_base;
                r.sig  = DualRing::SecondSig();   // 0 for every vanilla ring
                r.why  = "ring2";
                out.push_back(std::move(r));
            }
            return out;
        }

        // A queued equip is done when a worn list matching that unit exists --
        // but "matching" has to be strict about BOTH things identity is made of:
        //
        //   HAND   Equipping to the left while an identical item is worn on the
        //          right read as "already landed" the instant it was queued.
        //
        //   COUNT  In a same-form swap the DISPLACED occupant is still worn and
        //          is identical in every respect to the unit coming in. Its list
        //          proved the new equip had landed before the engine ran it, the
        //          entry vanished, and the incoming unit fell straight back onto
        //          the board -- so the player saw it on the cursor AND in the
        //          grid. The carried unit already owns part of that worn total,
        //          so a release needs MORE than it accounts for -- and "part"
        //          is its whole COUNT, not one unit (a quiver is worn by the
        //          hundred).
        //
        // Both only misfire when the two units share a pool, which is exactly why
        // this survived every test that used two DIFFERENT items.
        void ReleaseWornPendingEquips(const std::string& a_baseKey,
                                      RE::InventoryEntryData* a_entry)
        {
            if (g_pendingEquip.empty() || !a_entry || !a_entry->extraLists) return;
            std::erase_if(g_pendingEquip, [&](const OffBoardUnit& u) {
                if (u.base != a_baseKey) return false;
                int matching = 0;
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl) continue;
                    const bool L = xl->HasType<RE::ExtraWornLeft>();
                    const bool R = xl->HasType<RE::ExtraWorn>();
                    if (!L && !R) continue;
                    if (u.hand == 1 && !R) continue;
                    if (u.hand == 2 && !L) continue;
                    std::uint16_t uid = 0;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
                    if (uid == u.uid && InstanceSig(xl) == u.sig) {
                        matching += (std::max)(1, xl->GetCount());
                    }
                }
                const bool carriedOwnsIt =
                    g_held && g_held->fromDoll && g_held->obj &&
                    FormKey(g_held->obj) == a_baseKey &&
                    g_held->uid == u.uid && g_held->sig == u.sig &&
                    (u.hand == 0 || g_held->hand == 0 || g_held->hand == u.hand);
                // ★★"Owns ONE" was the whole assumption, and a quiver breaks it.
                // `matching` is a sum of LIST COUNTS, so a carried 90-arrow
                // quiver contributes 90 to it — and 90 > 1 released the incoming
                // equip's suppression on the spot. The board then counted the
                // arriving hundred as still-in-the-pack for the frame or two
                // before the engine ran, which is the tile that flickered in
                // and out at the front. Full stacks hid it: 100 worn against a
                // 100 carry cancelled exactly, so nothing was left over to show.
                const int carriedUnits =
                    carriedOwnsIt ? (std::max)(1, g_held->count) : 0;
                return matching > carriedUnits;
            });
        }


        // GI33: favourites are VANILLA's. We only draw the star.
        //
        // Owning them per tile could not work: the engine keeps ONE
        // ExtraDataList per set of identical units, so "this dagger, not that
        // one" has nowhere to live. Every attempt to hold that intent ourselves
        // ended up fighting the engine over the Q menu. F now marks the tile's
        // own sub-stack and the star is read straight back out of it, so
        // identical units share the mark while tempered and plain stay apart.
        // (GI34 attaches the ExtraHotkey directly for that second part -- the
        // engine's own call is entry-scoped and refuses a second variant.)
        //
        // Queued for the game thread like every other inventory mutation here.
        void ToggleFavorite(const std::string& a_key, RE::TESBoundObject* a_obj,
                            std::uint16_t a_uid, int a_xlIdx, std::uint16_t a_sig = 0)
        {
            if (!a_obj || a_key.empty()) return;
            g_favSync.push_back({ a_obj, a_uid, a_xlIdx, a_sig });
        }


        // Writes a tile's PLACEMENT and nothing else.
        //
        // Every drop path used to assign a whole LayoutEntry (`g_layout[k] = {col,
        // row, bag, count}`), which silently reset every OTHER field on the tile.
        // The favourite flag lives there now, so moving a starred tile wiped its
        // star -- and the reconcile then re-adopted the star onto whichever key of
        // that pool sorted first. That is why moving the middle tempered dagger
        // put the star on a different one, why the star then "followed" that tile
        // (it was already the first key, so re-adoption landed on itself), and why
        // a plain star vanished for good once a tempered one held the pool.
        // The tile gives up its CELL but survives (it is riding the cursor).
        // Erasing the entry instead threw its flags away with the position: a
        // swap wiped the displaced tile's star, and the reconcile then handed
        // the mark to whichever sibling sorted first -- "A를 B에 스왑했더니
        // 별이 C로 갔다". Use this wherever the item is not actually leaving.
        void ParkTile(const std::string& a_key)
        {
            const auto li = g_layout.find(a_key);
            if (li == g_layout.end()) return;
            li->second.col = -1;
            li->second.row = -1;
            li->second.bag.clear();
        }

        // GI62: a_rot < 0 leaves the tile's rotation alone. Only a DROP knows
        // the angle the player chose, so only a drop passes one; every other
        // caller (park, restore, split) is moving a tile that keeps its own.
        void PlaceTile(const std::string& a_key, int a_col, int a_row,
                       const std::string& a_bag, int a_count, int a_rot = -1)
        {
            auto& le = g_layout[a_key];
            le.col = a_col;
            le.row = a_row;
            le.bag = a_bag;
            le.count = a_count;
            if (a_rot >= 0) le.rot = a_rot & 3;
        }

        bool g_layoutLoaded = false;   // capacity checks run with the menu closed

        // LEGACY MIGRATION ONLY: the ini is read ONCE per process into a frozen
        // snapshot; record-less (pre-cosave) saves migrate from that snapshot.
        // It is never written any more — the live per-change write leaked the
        // current session's unsaved arrangement into any old save loaded later
        // (the ini was global, not per-save).
        void LoadLayout()
        {
            static bool s_read = false;
            static std::map<std::string, LayoutEntry> s_snapLayout;
            static std::set<std::string> s_snapBags;

            g_layoutLoaded = true;
            if (s_read) {
                g_layout = s_snapLayout;
                g_openBags = s_snapBags;
                return;
            }
            s_read = true;
            g_layout.clear();
            g_openBags.clear();
            std::ifstream in(kLayoutPath);
            if (!in) return;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == ';' || line[0] == '[') continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                auto trim = [](std::string s) {
                    const auto b = s.find_first_not_of(" \t\r");
                    const auto e = s.find_last_not_of(" \t\r");
                    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
                };
                const std::string key = trim(line.substr(0, eq));
                std::string rest = trim(line.substr(eq + 1));
                if (key.empty()) continue;

                if (key == "!openbags") {   // remembered open-bag list (E2)
                    std::istringstream ss(rest);
                    std::string tok;
                    while (std::getline(ss, tok, ';')) {
                        if (!tok.empty()) g_openBags.insert(tok);
                    }
                    continue;
                }

                LayoutEntry le;
                std::replace(rest.begin(), rest.end(), ',', ' ');
                std::istringstream ss(rest);
                if (!(ss >> le.col >> le.row)) continue;
                ss >> le.bag;   // optional third token
                g_layout[key] = le;
            }
            s_snapLayout = g_layout;
            s_snapBags = g_openBags;
        }

        // ---- cosave (per-save layout; the global ini is legacy fallback) ----

        // v2: per-tile count (G4)
        // v3: GI1 tile keys may carry an "@uid" instance suffix. The BINARY
        //     format is unchanged -- keys were already length-prefixed strings,
        //     so v2 records load field-for-field and simply arrive with no
        //     instance keys (every unit reads as plain, uid 0). A v2 save's
        //     gear therefore keeps its saved cells; only units the engine had
        //     uid'd get re-seated once, on the first rebuild.
        constexpr std::uint32_t kCosaveVersion = 9;   // v9: the slot's binding hints
        constexpr std::uint32_t kMaxStr = 512;
        constexpr std::uint32_t kMaxEntries = 65536;

        bool WriteStr(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const auto len = static_cast<std::uint32_t>(a_s.size());
            if (!a_intfc->WriteRecordData(len)) return false;
            return len == 0 || a_intfc->WriteRecordData(a_s.data(), len);
        }

        bool ReadStr(SKSE::SerializationInterface* a_intfc, std::string& a_out)
        {
            std::uint32_t len = 0;
            if (!a_intfc->ReadRecordData(len) || len > kMaxStr) return false;
            a_out.assign(len, '\0');
            return len == 0 || a_intfc->ReadRecordData(a_out.data(), len);
        }

        // ---- placement (JS maskOf / placeItems 1:1) ----

        Mask MaskOf(const GridDef& a_def)
        {
            Mask m;
            if (!a_def.shape.empty()) {
                std::istringstream ss(a_def.shape);
                std::string tok;
                int w = 1;
                while (std::getline(ss, tok, '|')) {
                    std::vector<bool> row;
                    for (char c : tok) row.push_back(c == '1');
                    w = (std::max)(w, static_cast<int>(row.size()));
                    m.rows.push_back(std::move(row));
                }
                if (m.rows.empty()) m.rows.push_back({ true });
                for (auto& r : m.rows) r.resize(w, false);
                m.w = (std::min)(w, kCols);
                m.h = static_cast<int>(m.rows.size());
                return m;
            }
            m.w = (std::min)(kCols, (std::max)(1, a_def.w));
            m.h = (std::max)(1, a_def.h);
            m.rows.assign(m.h, std::vector<bool>(m.w, true));
            return m;
        }

        // GI62: the footprint turned a_rot quarter-turns CLOCKWISE (0..3).
        // ★The mask is the ONE source every consumer already reads -- placement,
        // collision, the drop ghost, the hover hit test and the occupancy shading
        // all walk it. Rotating HERE is what makes rotation a one-line change at
        // every one of those sites instead of a special case in each.
        [[nodiscard]] Mask RotateMask(const Mask& a_mask, int a_rot)
        {
            Mask m = a_mask;
            for (int i = 0; i < (a_rot & 3); ++i) {
                Mask r;
                r.w = m.h;
                r.h = m.w;
                r.rows.assign(r.h, std::vector<bool>(r.w, false));
                for (int y = 0; y < m.h; ++y) {
                    for (int x = 0; x < m.w; ++x) {
                        if (m.rows[y][x]) r.rows[x][m.h - 1 - y] = true;
                    }
                }
                m = std::move(r);
            }
            return m;
        }

        // A footprint's def + rotation in one call (the pairing is always this).
        [[nodiscard]] Mask MaskOf(const GridDef& a_def, int a_rot)
        {
            return RotateMask(MaskOf(a_def), a_rot);
        }


        // GI62: a tinted image drawn at an angle. The silhouette halo is cut
        // from the sprite's OWN alpha, so it has to lie at the sprite's angle --
        // an upright halo under a turned sword reads as a second, wrong-shaped
        // item behind the first. (The radial style is symmetric and needs none
        // of this.)
        void AddImageRot(ImDrawList* a_dl, void* a_tex, const ImVec2& a_c,
                         const ImVec2& a_size, float a_deg, ImU32 a_tint)
        {
            const float hx = a_size.x * 0.5f;
            const float hy = a_size.y * 0.5f;
            if (std::fabs(a_deg) < 0.01f) {
                a_dl->AddImage(reinterpret_cast<ImTextureID>(a_tex),
                    ImVec2(a_c.x - hx, a_c.y - hy), ImVec2(a_c.x + hx, a_c.y + hy),
                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), a_tint);
                return;
            }
            const float r = a_deg * 3.14159265f / 180.0f;
            const float cs = std::cos(r);
            const float sn = std::sin(r);
            const ImVec2 o[4] = { { -hx, -hy }, { hx, -hy }, { hx, hy }, { -hx, hy } };
            ImVec2 p[4];
            for (int i = 0; i < 4; ++i) {
                p[i] = ImVec2(a_c.x + o[i].x * cs - o[i].y * sn,
                              a_c.y + o[i].x * sn + o[i].y * cs);
            }
            a_dl->AddImageQuad(reinterpret_cast<ImTextureID>(a_tex),
                p[0], p[1], p[2], p[3],
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f),
                ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f), a_tint);
        }

        // ★GI62d: THE PIVOT CELL — the one cell that stays put through a turn.
        //
        // An item does not rotate about its geometric centre; it rotates about a
        // cell, and that cell is what the cursor holds. A 1x2 dagger pivots on
        // its grip: turn it four times and the blade sweeps up, right, down,
        // left while the grip never moves -- the four positions together make
        // the cross. Centring on the middle instead made rot 0 and rot 2 occupy
        // the SAME two cells, so four presses only ever produced two layouts and
        // nothing appeared to revolve.
        //
        // The pivot is carried THROUGH the rotation like any other cell, which
        // is why it must be derived from the upright footprint and turned, not
        // recomputed from the turned one.
        void PivotCell(const GridDef& a_def, int a_rot, int& a_x, int& a_y)
        {
            const Mask m0 = MaskOf(a_def);      // always the UPRIGHT footprint
            int w = m0.w, h = m0.h;
            int x = w / 2, y = h / 2;           // middle cell, biased low on even sides
            for (int i = 0; i < (a_rot & 3); ++i) {
                const int nx = h - 1 - y;
                const int ny = x;
                x = nx;
                y = ny;
                std::swap(w, h);
            }
            a_x = x;
            a_y = y;
        }

        // ★GI62f: the CORNER pivot — a lattice point (a cell corner), not a cell
        // and not the item's exact centre.
        //
        // A 2x3's true centre is half a cell off vertically, so gripping the
        // centre put the cursor on a cell EDGE and the turn came out half a cell
        // shy of the cross. Rounding that half-cell OUT to the nearest corner
        // gives a point the grid can hold in both orientations -- and the four
        // rotations then fan out around it into the cross the user drew.
        // 2x4 already had an integral centre, so it is unchanged by this.
        void CornerPivot(const GridDef& a_def, int a_rot, int& a_x, int& a_y)
        {
            const Mask m0 = MaskOf(a_def);
            int w = m0.w, h = m0.h;
            int x = (w + 1) / 2, y = (h + 1) / 2;   // round the half-cell out
            for (int i = 0; i < (a_rot & 3); ++i) {
                const int nx = h - y;   // corners turn about the lattice, so no -1
                const int ny = x;
                x = nx;
                y = ny;
                std::swap(w, h);
            }
            a_x = x;
            a_y = y;
        }

        // Where the cursor grips the item. Every pickup and every turn goes
        // through this, so the point it names never moves during a rotation.
        //
        // ★Two regimes, and the SHORT SIDE decides which (settled with the user
        // against real items, not derived):
        //
        //  short side >= 2  -- turn about the item's true CENTRE. Before and
        //    after then share the middle block and read as a cross. 2x3 and 2x4
        //    are this. 0 and 180 degrees land on the same cells, so there are
        //    two layouts, not four -- that is the accepted trade.
        //
        //  short side == 1  -- a bar. Turn about a CELL, so the four rotations
        //    land on four DIFFERENT neighbours (up, right, down, left) and the
        //    four together form the cross: a dagger pivoting on its grip. Its
        //    centre would sit half a cell off the grid anyway.
        //    1x2, 2x1 and 1x4 are this; 1x3 lands identically either way.
        //
        // Both mistakes have been made: the cell pivot everywhere threw 2x4 off
        // by half a footprint, and the centre everywhere collapsed the dagger's
        // four positions into two.
        void HoldByPivot(Held& a_held, const GridDef& a_def)
        {
            const Mask m0 = MaskOf(a_def);
            if ((std::min)(m0.w, m0.h) > 1) {
                int cx = 0, cy = 0;
                CornerPivot(a_def, a_held.rot, cx, cy);
                a_held.offX = static_cast<float>(cx) * CellPx();
                a_held.offY = static_cast<float>(cy) * CellPx();
                return;
            }
            int px = 0, py = 0;
            PivotCell(a_def, a_held.rot, px, py);
            a_held.offX = (static_cast<float>(px) + 0.5f) * CellPx();
            a_held.offY = (static_cast<float>(py) + 0.5f) * CellPx();
        }

        // The same grip, for a footprint that is not the carried one (the
        // departing-shape outline drawn during a turn).
        void GripOffset(const GridDef& a_def, int a_rot, float& a_offX, float& a_offY)
        {
            const Mask m0 = MaskOf(a_def);
            if ((std::min)(m0.w, m0.h) > 1) {
                int cx = 0, cy = 0;
                CornerPivot(a_def, a_rot, cx, cy);
                a_offX = static_cast<float>(cx) * CellPx();
                a_offY = static_cast<float>(cy) * CellPx();
                return;
            }
            int px = 0, py = 0;
            PivotCell(a_def, a_rot, px, py);
            a_offX = (static_cast<float>(px) + 0.5f) * CellPx();
            a_offY = (static_cast<float>(py) + 0.5f) * CellPx();
        }

        // Only items whose FOOTPRINT changes may turn. A square tile would spin
        // its drawing and pack identically, which reads as the key being broken
        // ("I pressed D and nothing happened") on the potions and ingots that
        // make up most of a bag -- and turning a potion label upside down is not
        // a feature anyone asked for. A free shape always qualifies: even when
        // its bounding box is square, the cells it covers move.
        // ★★1.0.5: ask the MASK, not the shape string. "A free shape always
        // qualifies" was wrong for symmetric ones — a plus (010|111|010) is
        // identical at all four rotations, so pressing the key did nothing and
        // produced exactly the "I pressed D and nothing happened" this function
        // exists to prevent. Comparing a quarter turn against the original
        // catches every symmetry for free, including squares.
        [[nodiscard]] bool CanRotate(const GridDef& a_def)
        {
            const Mask m0 = MaskOf(a_def);
            const Mask m1 = RotateMask(m0, 1);
            if (m0.w != m1.w || m0.h != m1.h) return true;
            for (int y = 0; y < m0.h; ++y) {
                for (int x = 0; x < m0.w; ++x) {
                    if (m0.rows[y][x] != m1.rows[y][x]) return true;
                }
            }
            return false;
        }





        // ★W3: the ownership predicates. Rows below kMinRows are always the
        // player's; carry-weight bonus cells extend ownership past the hard
        // board, filling the next row left-to-right -- so the boundary is a
        // step, and a footprint is owned only when EVERY occupied cell is.
        [[nodiscard]] bool OwnedCellAt(int a_col, int a_row)
        {
            if (a_row < kMinRows) return true;
            const int full = g_cwBonusCells / kCols;
            const int part = g_cwBonusCells % kCols;
            if (a_row < kMinRows + full) return true;
            return a_row == kMinRows + full && a_col < part;
        }
        [[nodiscard]] bool OwnedFootprint(int a_col, int a_row, const Mask& a_m)
        {
            for (int y = 0; y < a_m.h; ++y) {
                for (int x = 0; x < a_m.w; ++x) {
                    if (a_m.rows[y][x] && !OwnedCellAt(a_col + x, a_row + y)) {
                        return false;
                    }
                }
            }
            return true;
        }
        // rows needed to SHOW the owned region (a fresh unlock must be
        // visible to drop into, not only reachable by overflow)
        [[nodiscard]] int OwnedRowSpan()
        {
            return kMinRows + g_cwBonusCells / kCols +
                   (g_cwBonusCells % kCols ? 1 : 0);
        }

        int PlaceItems(std::vector<Item*>& a_list, int a_cols, int a_minRows,
                       int a_maxRows, int a_ownedExtra = 0)
        {
            // ★W3: a SIM caller (maxRows == minRows, the hard board) may own
            // extra cells past it -- carry-weight bonus. The display caller
            // (huge maxRows) is unlimited exactly as before.
            const bool hard = a_maxRows <= a_minRows;
            const int  exFull = a_ownedExtra / a_cols;
            const int  exPart = a_ownedExtra % a_cols;
            const int  limRows =
                hard ? a_minRows + exFull + (exPart ? 1 : 0) : a_maxRows;
            auto ownedSim = [&](int c, int r) {
                if (!hard) return true;
                if (r < a_minRows + exFull) return true;
                return r == a_minRows + exFull && exPart > 0 && c < exPart;
            };
            std::vector<std::vector<bool>> occ;
            auto ensureRow = [&](int r) {
                while (static_cast<int>(occ.size()) <= r) occ.emplace_back(a_cols, false);
            };
            auto fits = [&](int c, int r, const Mask& m) {
                if (r + m.h > limRows) return false;
                for (int y = 0; y < m.h; ++y) {
                    ensureRow(r + y);
                    for (int x = 0; x < m.w; ++x) {
                        if (m.rows[y][x] &&
                            (occ[r + y][c + x] || !ownedSim(c + x, r + y))) {
                            return false;
                        }
                    }
                }
                return true;
            };
            auto mark = [&](int c, int r, const Mask& m) {
                for (int y = 0; y < m.h; ++y)
                    for (int x = 0; x < m.w; ++x)
                        if (m.rows[y][x]) occ[r + y][c + x] = true;
            };

            // pass 1: saved spots — items ALREADY in the grid last render first
            auto pass1 = [&](Item& it) {
                it.fixed = false;
                it.overflow = false;
                if (it.col >= 0 && it.row >= 0 && it.col + it.mask.w <= a_cols &&
                    fits(it.col, it.row, it.mask)) {
                    mark(it.col, it.row, it.mask);
                    it.fixed = true;
                }
            };
            for (auto* it : a_list) if (g_prevKeys.contains(it->key)) pass1(*it);
            for (auto* it : a_list) if (!g_prevKeys.contains(it->key)) pass1(*it);

            // pass 2: first-fit the rest (row -> col)
            for (auto* it : a_list) {
                if (it->fixed) continue;
                auto tryFit = [&](const Mask& m) {
                    if (m.w > a_cols) return false;
                    for (int r = 0; r + m.h <= limRows; ++r) {
                        for (int c = 0; c <= a_cols - m.w; ++c) {
                            if (fits(c, r, m)) {
                                mark(c, r, m);
                                it->col = c;
                                it->row = r;
                                return true;
                            }
                        }
                    }
                    return false;
                };
                if (tryFit(it->mask)) continue;
                // ★GI62: a turned tile that lost its saved spot stands back UP
                // rather than falling off the board. The turn is a property of
                // the spot, so losing the spot loses the turn -- the same rule
                // every other spot-losing path already follows. Standing a sword
                // up is a far smaller surprise than it vanishing.
                if (it->rot != 0) {
                    Mask upright = MaskOf(it->def);
                    if (tryFit(upright)) {
                        it->rot = 0;
                        it->mask = std::move(upright);
                        continue;
                    }
                } else if (CanRotate(it->def)) {
                    // ★The mirror fallback. A FRESH tile arrives upright (rot 0
                    // -- the turn is a property of the spot, and it has none
                    // yet), but the capacity gate green-lights pickups by
                    // trying BOTH orientations. Landing has to honour that
                    // promise: a 1x3 staff green-lit for a 3x1 gap otherwise
                    // sailed past it into the growth rows, upright, and the
                    // pack went overloaded with the gap still empty (user
                    // report). Turn it before letting it fall off the board.
                    Mask turned = MaskOf(it->def, 1);
                    if (tryFit(turned)) {
                        it->rot = 1;
                        it->mask = std::move(turned);
                        continue;
                    }
                }
                it->overflow = true;
            }
            return (std::max)(a_minRows, static_cast<int>(occ.size()));
        }

        // ---- shared grid renderer (JS makeTile / gridShades / linesFor) ----

        // draws one grid at the current cursor pos; returns via g_target when
        // the carried item hovers this grid
        // ---- Phase 3: DrawGridView passes (bodies moved verbatim) ----

        // pass 1: hairline cell grid + outer border + overflow-zone marking
        void DrawGridChrome(View& a_view, int a_viewIdx, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            const auto& sk = Theme::S();
            const float gridW = a_view.cols * CellPx();
            const float gridH = a_view.rows * CellPx();
            DrawCellLattice(dl, base, a_view.cols, a_view.rows);
            // ★GI78: NOT on the main board. This rect is anchored to `base`,
            // which is the grid's content origin and therefore SCROLLS. On the
            // scrolling board it lands exactly on the board edge at scroll 0
            // (two 20% layers on one pixel row = 36%, the thicker look), slides
            // out of view in the middle, and comes back at the bottom — so the
            // border appeared to thin out and thicken as the player wheeled.
            // The board's own edge is drawn in Draw() at a FIXED position and
            // owns that line; bag and partner views have no such pass and still
            // need this one.
            // ★No outer frame when the board is tiles-on-panel: the cells
            // already say where it ends, and an accent ring there is the
            // darkest token in the skin drawn over the brightest ground.
            // ★Also skipped when the board is CARVED: that pass already runs
            // its lines along the outer edge, and an accent ring on top would
            // be a third stroke there — plus it is the same near-invisible
            // rust the carve was brought in to replace.
            if (a_viewIdx != 0 && !sk.engravedCells && !sk.translucent) {
                // ★Half a pixel in, for the reason the doll's slot border
                // carries: AddRect strokes ON the path, so the outer half of
                // the line sits past base+gridW -- and a board whose right edge
                // lands on its column's clip loses it.
                dl->AddRect(ImVec2(base.x + 0.5f, base.y + 0.5f),
                            ImVec2(base.x + gridW - 0.5f, base.y + gridH - 0.5f),
                            Theme::Acc(0.20f));
            }

            // GI28: the cell the last action aimed to empty, fading out. If the
            // flash and the gap are not the same cell, the bug is on screen.
            const float now = static_cast<float>(ImGui::GetTime());

            // design pass F: overflow-zone marking — rows past the OWNED
            // region are TEMPORARY (they collapse the moment space frees up).
            // A crimson boundary + faint tint says "this shelf is borrowed".
            // ★W3: the boundary is the OWNERSHIP edge now -- carry-weight
            // bonus cells push it down, and a partial row makes it a step.
            {
                const int  full = kMinRows + g_cwBonusCells / kCols;
                const int  part = g_cwBonusCells % kCols;
                const auto tintC = IM_COL32(204, 81, 72, 14);
                const auto lineC = IM_COL32(204, 81, 72, 200);
                if (a_viewIdx == 0 && a_view.rows > full) {
                    const float cp = CellPx();
                    const float yF = base.y + full * cp;
                    if (part > 0) {
                        const float xP = base.x + part * cp;
                        // tint: the unowned tail of the partial row, then
                        // everything below it
                        dl->AddRectFilled(ImVec2(xP, yF),
                            ImVec2(base.x + gridW, yF + cp), tintC);
                        if (a_view.rows > full + 1) {
                            dl->AddRectFilled(ImVec2(base.x, yF + cp),
                                ImVec2(base.x + gridW, base.y + gridH), tintC);
                        }
                        // the step: under the owned cells, up, then across
                        dl->AddLine(ImVec2(base.x, yF + cp), ImVec2(xP, yF + cp),
                                    lineC, 2.0f);
                        dl->AddLine(ImVec2(xP, yF + cp), ImVec2(xP, yF),
                                    lineC, 2.0f);
                        dl->AddLine(ImVec2(xP, yF), ImVec2(base.x + gridW, yF),
                                    lineC, 2.0f);
                    } else {
                        dl->AddRectFilled(ImVec2(base.x, yF),
                            ImVec2(base.x + gridW, base.y + gridH), tintC);
                        dl->AddLine(ImVec2(base.x, yF),
                            ImVec2(base.x + gridW, yF), lineC, 2.0f);
                    }
                }
            }
        }

        // pass 2: occupied-cell shading. GI50: the footprint hover border
        // (corner-fade / edge outline) is gone on user request -- hover
        // feedback is pass 4's accent tint + hover note; the corner-fade
        // look now belongs to the STATUS rings (temper/poison) instead.
        void DrawOccupancyPass(View& a_view, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            const auto& sk = Theme::S();
            // ★Alpha 1.0 on a light panel, matching the doll: Equip draws a
            // worn slot with Col(shade, 1.0f) while this used the skin's own
            // alpha, so the same colour said "occupied" loudly on one half of
            // the window and almost nothing on the other.
            const ImU32 shadeCol = Theme::OccupiedGround();
            // ★GI69: the NEW mark rides along here — a flat wash over the whole
            // cell instead of light bleeding in from the tile's border.
            //
            // Three border treatments were tried and all failed the same way.
            // A glow that starts at the edge means two adjacent new tiles each
            // light the seam from their own side, so the dark grid hairline
            // ends up sandwiched between two bright bands and reads STRONGER
            // than anywhere else on the board — the exact opposite of the
            // intent. Halving the depth on a shared side fixed the width but
            // not that contrast; insetting the start only moved it. The seam is
            // not a tuning problem, it is what "draw on the border" costs.
            //
            // A wash has no seam to get wrong: a tile looks identical whether
            // it stands alone or sits in the middle of a freshly looted block,
            // and every new tile keeps its own mark (which the alternative --
            // outlining only the outside of a group -- gives up).
            // ★★★...AND THE NEW MARK IS NO LONGER ONE OF THEM. It was a wash
            // too -- IM_COL32(242, 245, 250, 22), near-white at 8.6% -- and
            // that colour was a CONSTANT while the bag mark below reads its own
            // from the skin. Tuned against the dark boards it works: on Simple
            // Charcoal the occupied cell moves about +19 a channel. On Sumi
            // Parchment the cell is already #9E9178 and the same wash moves it
            // +7/+9/+11, under a paper texture. White on white. The mark was
            // there and answered nothing -- a 1x1 broom read as never having
            // arrived (user report).
            //
            // It draws as a corner dot now, in DrawItemsPass, for two reasons a
            // wash cannot give: it does not depend on the ground it sits on, so
            // twenty skins need no twenty values; and it survives being small,
            // which is exactly the case that failed.
            // ★An OPEN bag tints its own tile, so "which of these five bags is
            // the window I am looking at" is answerable on the board instead of
            // by opening each one. Same wash treatment as the NEW mark, and for
            // the same reason — see the note above on why a border fails.
            //
            // ★The COLOUR belongs to the skin, not to this file. It was a
            // constant tuned against the dark skins, which on a light-panelled
            // skin lands close enough to the cell to vanish — the mark existed
            // but answered nothing.
            const ImU32 kOpenBagCol = Theme::Col(sk.bagOpen);
            // The fill must not cover the grid's own chrome. A hairline sits ON
            // the boundary, so it needs 1px clearance on BOTH sides; a groove
            // is carved AFTER the cell, so the leading edge takes none and the
            // trailing edge takes the full groove.
            // ★★The clearance has to MATCH THE DIVIDER, and 1.0 only ever
            // matched a 1px hairline. An ink skin rules its cells with a ~3px
            // mark, so the shade covered part of it and left bare paper on
            // both sides -- a bright seam running through every multi-cell
            // item, which is the opposite of what the clearance is for. Ask
            // the skin how wide its line is instead of assuming.
            // ★★INK CLEARS NOTHING. Its rules are drawn after this pass, on
            // top, so the ground fills the cell edge to edge -- and the two
            // stop having to agree on a number. They could not: the mark snaps
            // to whole pixels and the cell boundary does not, so a clearance of
            // "half the width" landed a fraction either side and the grid was
            // covered at some columns and left a bright seam at others. The
            // fix is not a better fraction, it is not needing one.
            const float shadeIn0 = sk.engravedCells ? 0.0f
                                 : Theme::InkChrome() ? 0.0f : 1.0f;
            const float shadeIn1 = sk.engravedCells
                ? Theme::kGrooveW * Theme::Scale() * 0.5f
                : Theme::InkChrome() ? 0.0f : 1.0f;
            // ★★1.0.5 — TRIED AND REVERTED: making a multi-cell item one
            // seamless surface (skip the inset where the neighbour is the same
            // item) and drawing the seam back on top as a black low-alpha line.
            //
            // It failed on COLOUR. Leaving the groove uncovered is not merely
            // "not painting" — it is what lets the SKIN's own divider show
            // through, whatever that skin decided a divider looks like: a
            // carved groove here, bare panel there, an accent hairline on
            // glass. Any line drawn back on top has to pick one colour, and one
            // colour cannot be all of those. Black darkens correctly on every
            // ground but stops matching the lattice in the empty cells beside
            // it, so an item's inner seams and the board's own grid no longer
            // look like the same grid.
            //
            // The inset below is therefore positional on purpose. The rule is
            // not "where does this item end" but "where does a groove exist",
            // and a groove exists between any two cells of the board.
            for (int idx : a_view.items) {
                const Item* itP = ItemAt(idx);   // guarded: see ItemAt
                if (!itP) continue;
                const auto& it = *itP;
                if (it.overflow || it.col < 0) continue;
                const bool bagOpen = it.def.bag != 0 && g_openBags.contains(it.key);
                for (int y = 0; y < it.mask.h; ++y) {
                    for (int x = 0; x < it.mask.w; ++x) {
                        if (!it.mask.rows[y][x]) continue;
                        const ImVec2 p0(base.x + (it.col + x) * CellPx(),
                                        base.y + (it.row + y) * CellPx());
                        const ImVec2 p1(p0.x + CellPx(), p0.y + CellPx());
                        // ...and the fill follows the SAME half-groove rule, or
                        // the shade sits off-centre from the face beneath it
                        const int gc = it.col + x, gr = it.row + y;
                        const ImVec2 q0(
                            p0.x + (gc > 0 ? shadeIn1 : shadeIn0),
                            p0.y + (gr > 0 ? shadeIn1 : shadeIn0));
                        const ImVec2 q1(
                            p1.x - (gc + 1 < a_view.cols ? shadeIn1 : shadeIn0),
                            p1.y - (gr + 1 < a_view.rows ? shadeIn1 : shadeIn0));
                        dl->AddRectFilled(q0, q1, shadeCol);
                        // (rarity is a corner wedge drawn once per item in
                        //  pass 4 now — nothing rarity-related belongs in this
                        //  per-cell loop any more. See Grid.h.)
                        if (bagOpen) dl->AddRectFilled(q0, q1, kOpenBagCol);
                    }
                }
            }
        }

        // pass 3: rarity glow UNDER every sprite
        // GI46/GI49: status rings (poison green / temper gold) on a tile border.
        // ONE implementation shared by the player board's glow pass and the
        // partner/doll DrawGlow -- the first cut lived only in DrawGlow, so
        // the player board showed no rings and its unmasked halo switch read
        // the new bits as "both rarities" red.
        // GI50: EXACTLY the retired hover border's recipe (single-layer
        // Theme::CornerFade, default 0.32 fade) so the rings inherit that
        // sleek look -- only the colors differ: temper white, poison green.
        // ---- GI66: tile markers, one shared spec -----------------------------
        // ★Every marker on a tile is the same WIDTH and carries the same black
        // rim. Before this the tray shapes were sized from a half-radius and the
        // poison state was a border ring instead of a marker at all, so nothing
        // agreed with anything. One width and one rim is what makes four
        // different shapes read as one family.
        // ★★1.0.5 — every metric below is a FRACTION OF THE CELL, not a pixel
        // count scaled by the UI factor.
        //
        // They used to be `constant * Theme::Scale()`, which follows the UI
        // scale but ignores the CELL scale — and the cell follows both. Shrink
        // the board to 0.60 and the tile loses a quarter of its width while
        // every marker stays put, so the tray grows relative to the item it is
        // annotating until it no longer fits. Anchoring to CellPx() makes the
        // proportion a property of the design instead of an accident of which
        // slider was moved.
        //
        // The divisor is the DEFAULT cell (48 x 0.80 = 38.4), so these are the
        // same pixel sizes as before at default settings: mark 8, rim 1.5,
        // gap 3, inset 4, wedge 14.
        constexpr float kCellRef   = 48.0f * 0.80f;
        // ★1.5 -> 1.0, with the half pixel handed to the shape: the marker's
        // OUTER size is rim + box, so 8.0/1.5 and 8.5/1.0 occupy exactly the
        // same 9.5px while the coloured part grows. A thinner outline that also
        // shrank the mark would have made the whole thing quieter, which was
        // not the point.
        constexpr float kMarkFrac  =  8.5f / kCellRef;   // marker box
        constexpr float kRimFrac   =  1.0f / kCellRef;   // black outline
        constexpr float kGapFrac   =  3.0f / kCellRef;   // between tray markers
        constexpr float kInsetFrac =  4.0f / kCellRef;   // from the tile edge

        // ★A hairline that lands under a pixel does not get crisper, it gets
        // grey: ImGui antialiases without snapping. Below CELL ~0.75 the
        // fraction would do exactly that, so the outline holds at one pixel.
        [[nodiscard]] float RimPx() { return (std::max)(1.0f, CellPx() * kRimFrac); }
        // ★The wedge is bigger than a tray marker because a triangle carries
        // about half the area of the square bounding it, and because the rim is
        // inset on all three sides — that costs rim*(2+sqrt2) of leg, leaving a
        // coloured triangle the size of a tray marker.
        constexpr float kWedgeFrac = 14.0f / kCellRef;


        // Poison: a droplet in the tile's free corner (top-right). It replaced a
        // green border ring -- the ring shared its whole edge with the "new item"
        // inner glow, and shared its colour with nothing, so it read as damage to
        // the tile rather than as a property of the item.
        // The point sits kTipD radii above the centre, so the whole drop is
        // 2.6r tall against 2r wide -- the proportion that reads as a droplet
        // rather than as a balloon or a spike.
        constexpr float kDropTipD = 1.6f;
        [[nodiscard]] constexpr float DropH(float a_w) { return a_w * 0.5f * (kDropTipD + 1.0f); }

        // a_centre is the round part's centre; the point reaches DropH above it.
        void DrawPoisonDrop(ImDrawList* a_dl, const ImVec2& a_centre, float a_w)
        {
            const float r = a_w * 0.5f;
            const float rim = RimPx();
            // ★Where the arc must stop is the TANGENT point, not a guessed
            // angle: from a tip d away, the tangents touch at acos(r/d) either
            // side of straight up. Guessing leaves a visible corner where the
            // straight edge meets the curve.
            const float half = std::acos(1.0f / kDropTipD) * 57.2957795f;
            const float start = 270.0f + half;
            const float sweep = 360.0f - 2.0f * half;
            ImVec2 p[18];
            int n = 0;
            for (int i = 0; i <= 14; ++i) {
                const float t = (start + sweep * static_cast<float>(i) / 14.0f) *
                                0.017453292f;
                p[n++] = ImVec2(a_centre.x + r * std::cos(t),
                                a_centre.y + r * std::sin(t));
            }
            p[n++] = ImVec2(a_centre.x, a_centre.y - r * kDropTipD);   // the point
            a_dl->AddConvexPolyFilled(p, n, IM_COL32(79, 194, 98, 255));
            a_dl->AddPolyline(p, n, IM_COL32(11, 11, 11, 255), ImDrawFlags_Closed, rim);
            // the liquid highlight -- what makes it read as a DROP at 10px
            a_dl->AddCircleFilled(ImVec2(a_centre.x - r * 0.30f, a_centre.y + r * 0.12f),
                                  r * 0.30f, IM_COL32(255, 255, 255, 110));
        }
        // pass 4: per-tile sprite + badges/markers + hover/click input
        // ★★1.0.5 — WHICH ITEM OWNS THE CELL UNDER THE CURSOR (-1 = none).
        //
        // ImGui hit-boxes are rectangles, so one InvisibleButton per item means
        // a free-form tile claims its own empty notch. Filtering the button's
        // result by the mask (MaskHit) stops the wrong tile from answering, but
        // it cannot hand the hit to the RIGHT one: ImGui gives hover to exactly
        // one widget per frame, and when that widget is the L-shape the click
        // lands on nobody. Observed in game — an item placed in the notch shows
        // its own tooltip yet neither tile can be picked up.
        //
        // A cell has exactly ONE owner, so asking the GRID instead of asking
        // each widget removes the ambiguity at the source. This is also what
        // ends the machine-gun hover blip (LootBarter.cpp:313): the id stops
        // alternating because only one tile ever considers itself hovered.
        //
        // O(items in this view), once per frame.
        [[nodiscard]] int OwnerAt(const View& a_view, const ImVec2& a_base,
                                  const ImVec2& a_pt)
        {
            const float c = CellPx();
            if (c <= 0.0f) return -1;
            const int cx = static_cast<int>(std::floor((a_pt.x - a_base.x) / c));
            const int cy = static_cast<int>(std::floor((a_pt.y - a_base.y) / c));
            if (cx < 0 || cy < 0 || cx >= a_view.cols || cy >= a_view.rows) return -1;
            for (int idx : a_view.items) {
                if (idx < 0 || idx >= static_cast<int>(g_items.size())) continue;
                const Item* itP = ItemAt(idx);   // guarded: see ItemAt
                if (!itP) continue;
                const auto& it = *itP;
                if (it.overflow || it.col < 0) continue;
                const int mx = cx - it.col, my = cy - it.row;
                if (mx < 0 || my < 0 || mx >= it.mask.w || my >= it.mask.h) continue;
                if (it.mask.rows[my][mx]) return idx;
            }
            return -1;
        }

        // ★Where a corner mark belongs on a free-form footprint: the outermost
        // OCCUPIED cell in the requested direction, not the bounding box's
        // corner. A favourite diamond on an empty notch reads as belonging to
        // whatever item is sitting in that notch (user-reported, with a
        // screenshot of exactly that).
        //   a_bottom = false -> top row first (rarity wedge, top-right)
        //   a_bottom = true  -> bottom row first (marker tray, bottom-right)
        [[nodiscard]] ImVec2 AnchorCell(const Mask& a_mask, const ImVec2& a_p0,
                                        bool a_bottom)
        {
            for (int i = 0; i < a_mask.h; ++i) {
                const int y = a_bottom ? a_mask.h - 1 - i : i;
                for (int x = a_mask.w - 1; x >= 0; --x) {
                    if (a_mask.rows[y][x]) {
                        return ImVec2(a_p0.x + x * CellPx(), a_p0.y + y * CellPx());
                    }
                }
            }
            return a_p0;
        }

        // ★★1.0.5 — the footprint's OUTLINE, following the mask.
        //
        // Every occupied cell contributes the edges its neighbours do not
        // cover. No closed loop is assembled on purpose: a staircase touches
        // itself diagonally, so a lattice point can carry four edges and
        // "which edge continues this loop" has no single answer. Drawing the
        // edges themselves sidesteps that entirely — and a ring's inner
        // boundary comes out for free, since those edges have no neighbour
        // either.
        //
        // Corners are filled by extending each segment half a thickness at
        // both ends, which is what a closed polyline's miter join does.
        //
        // ★A solid rectangle yields exactly its four outer edges at exactly
        // the cell boundary, which is where AddRect drew them — so the 99%
        // case is unchanged.
        void DrawMaskOutline(ImDrawList* a_dl, const Mask& a_mask,
                             const ImVec2& a_p0, ImU32 a_col, float a_thick)
        {
            if (!a_dl) return;
            const float c = CellPx();
            const float e = a_thick * 0.5f;
            auto solid = [&](int a_x, int a_y) {
                return a_x >= 0 && a_y >= 0 && a_x < a_mask.w && a_y < a_mask.h &&
                       a_mask.rows[a_y][a_x];
            };
            for (int y = 0; y < a_mask.h; ++y) {
                for (int x = 0; x < a_mask.w; ++x) {
                    if (!a_mask.rows[y][x]) continue;
                    const float x0 = a_p0.x + x * c, y0 = a_p0.y + y * c;
                    const float x1 = x0 + c, y1 = y0 + c;
                    if (!solid(x, y - 1)) {
                        a_dl->AddLine(ImVec2(x0 - e, y0), ImVec2(x1 + e, y0), a_col, a_thick);
                    }
                    if (!solid(x, y + 1)) {
                        a_dl->AddLine(ImVec2(x0 - e, y1), ImVec2(x1 + e, y1), a_col, a_thick);
                    }
                    if (!solid(x - 1, y)) {
                        a_dl->AddLine(ImVec2(x0, y0 - e), ImVec2(x0, y1 + e), a_col, a_thick);
                    }
                    if (!solid(x + 1, y)) {
                        a_dl->AddLine(ImVec2(x1, y0 - e), ImVec2(x1, y1 + e), a_col, a_thick);
                    }
                }
            }
        }

        // ★★1.0.5 M1 — where a sprite sits on a free-form footprint.
        //
        // The sprite is a rectangle and the footprint is not, so the two need a
        // meeting point. Two numbers do it:
        //
        //   centre  the centroid of the OCCUPIED cells, not the box's middle.
        //           On an L the mass is toward the bend, and an axe sprite —
        //           itself L-shaped, haft plus head — lands on it.
        //   fill    sqrt(occupied / box). A linear factor on the size, taken
        //           from an area ratio, so a footprint using 5 of 9 cells draws
        //           its icon at 75% instead of spilling across the four it does
        //           not own.
        //
        // ★No clipping. Clipping cuts the blade off a shape whose sprite is
        // centred on the box, which reads worse than the overflow it fixes —
        // moving and sizing the sprite is the fix, cutting it is not.
        //
        // ★A solid rectangle has every cell set: the centroid IS the box's
        // middle and fill is exactly 1, so the arithmetic below reduces to what
        // it replaced. The 99% case is untouched by construction, not by luck.
        struct MaskFit
        {
            ImVec2 centre;
            float  fill = 1.0f;
        };

        [[nodiscard]] MaskFit FitOf(const Mask& a_mask, const ImVec2& a_p0)
        {
            const float c = CellPx();
            float sx = 0.0f, sy = 0.0f;
            int n = 0;
            // ★★fill is the REACH of the shape per axis, not its area.
            //
            // It used to be sqrt(occupied / box), which reads a T as 5 of 9
            // cells and shrinks the icon to 74.5%. But a T spans all three
            // columns AND all three rows -- there is nothing to shrink for. The
            // area rule only makes sense for a shape that is genuinely smaller
            // than its box (a diagonal pair, say), and it was punishing every
            // shape that merely has a hole in it: a crossbow on a T footprint
            // came out a quarter too small and could not be lined up with the
            // arms it was drawn to fill.
            //
            // Widest row and tallest column, whichever is the tighter fraction:
            //   T (3x3)         rows max 3/3, cols max 3/3  -> 1.00  (was 0.75)
            //   diagonal (2x2)  rows max 1/2, cols max 1/2  -> 0.50  (was 0.71)
            //   solid rect      every row and column full   -> 1.00  (unchanged)
            //
            // ★A shape narrower than its box still shrinks, which is the case
            // the original rule was written for -- it just no longer counts the
            // holes of a shape that reaches every edge.
            int maxRow = 0;
            int minX = a_mask.w, maxX = -1, minY = a_mask.h, maxY = -1;
            std::vector<int> colN(static_cast<std::size_t>((std::max)(1, a_mask.w)), 0);
            for (int y = 0; y < a_mask.h; ++y) {
                int rowN = 0;
                for (int x = 0; x < a_mask.w; ++x) {
                    if (!a_mask.rows[y][x]) continue;
                    sx += static_cast<float>(x) + 0.5f;
                    sy += static_cast<float>(y) + 0.5f;
                    ++n;
                    ++rowN;
                    ++colN[static_cast<std::size_t>(x)];
                    minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
                    minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
                }
                maxRow = (std::max)(maxRow, rowN);
            }
            if (n == 0) {
                return { ImVec2(a_p0.x + a_mask.w * c * 0.5f,
                                a_p0.y + a_mask.h * c * 0.5f), 1.0f };
            }
            int maxCol = 0;
            for (const int v : colN) maxCol = (std::max)(maxCol, v);
            // ★★The centre is the occupied BOX's middle, not the centroid.
            //
            // A centroid is pulled toward whichever side holds more cells: a T
            // has three cells on its top row and two down its stem, so its
            // centroid sits 0.4 of a cell above the middle and the sprite rides
            // up by that much -- the crossbow's bow hung off the top edge while
            // the bottom cell stayed empty.
            //
            // But the icon is DRAWN in the same shape as the footprint that was
            // painted for it. The hole in a T is a hole in the picture too, so
            // weighting by it moves the sprite away from the very cells it was
            // meant to fill. Box against box is what lines them up.
            //
            // ★Occupied box, not the declared one, so a mask with an empty edge
            // row still centres on what it actually covers. A painted shape is
            // trimmed already (Editor's PainterToDef), so for those two the
            // answer is identical -- and identical to plain w/2, h/2 for every
            // solid rectangle, which is the 99% case.
            const int bw = maxX - minX + 1;
            const int bh = maxY - minY + 1;
            const float fillX = static_cast<float>(maxRow) /
                                static_cast<float>((std::max)(1, bw));
            const float fillY = static_cast<float>(maxCol) /
                                static_cast<float>((std::max)(1, bh));
            return { ImVec2(a_p0.x + static_cast<float>(minX + maxX + 1) * 0.5f * c,
                            a_p0.y + static_cast<float>(minY + maxY + 1) * 0.5f * c),
                     (std::min)(fillX, fillY) };
        }

        void DrawItemsPass(View& a_view, const ImVec2& base)
        {
            auto* cache = IconCache::GetSingleton();
            auto* dl = ImGui::GetWindowDrawList();
            const auto& io = ImGui::GetIO();
            const auto& sk = Theme::S();
            // ★One lookup for the whole pass: the cell under the cursor has a
            // single owner, and only that tile may consider itself hovered.
            // ImGui's own gate is applied ONCE here, at window level — a popup
            // on top, or another window in front, and nothing in this grid is
            // hovered. AllowWhenBlockedByActiveItem so that the tile's own
            // InvisibleButton going active (the frame a drag starts) does not
            // make the grid drop its hover.
            const bool winHov = ImGui::IsWindowHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            const int ownerIdx = winHov ? OwnerAt(a_view, base, io.MousePos) : -1;
            // items
            for (int idx : a_view.items) {
                const Item* itP = ItemAt(idx);   // guarded: see ItemAt
                if (!itP) continue;
                const auto& it = *itP;
                if (it.overflow || it.col < 0) continue;
                const ImVec2 p0(base.x + it.col * CellPx(), base.y + it.row * CellPx());
                const float  w = it.mask.w * CellPx();
                const float  h = it.mask.h * CellPx();

                // ★See g_liveObjs: membership only, never a read. A tile
                // whose object was not on the board at the last rebuild is not
                // drawn, and says so once with enough to find it by.
                //
                // ★★It asks about it.obj, and it asks FIRST. The test used to
                // run on the icon object, one line after that object had been
                // resolved through it.obj->GetFormID() -- so it dereferenced
                // the very pointer it existed to distrust, and then judged a
                // DIFFERENT pointer. The icon variants are static forms
                // GoldCoins resolved once from the plugin; they never sit in
                // the player's inventory, so they can never be in a set built
                // from the board, and testing them meant every pouch tile was
                // skipped for a reason that had nothing to do with liveness.
                if (!ObjLooksLive(it.obj)) {
                    static bool s_said = false;
                    if (!s_said) {
                        s_said = true;
                        SKSE::log::error("[GRID] tile '{}' (view '{}', idx {}) holds an "
                                         "object that was not on the board at the last "
                                         "rebuild ({}). Tile skipped.",
                            it.key, a_view.bagKey, idx,
                            static_cast<const void*>(it.obj));
                    }
                    continue;
                }
                // pouch tile: the ICON follows the stored amount (N/S/M/F
                // variants) while the item itself stays the pouch form
                RE::TESBoundObject* iconObj = it.obj;
                if (GoldCoins::IsPouch(it.obj->GetFormID())) {
                    // ★ITS OWN amount -- two pouches drew the same icon while
                    // this asked the player-wide total.
                    if (auto* v = GoldCoins::PouchIconObjectFor(
                            GoldCoins::PouchStoredOf(it.key),
                            GoldCoins::PouchCapOfKey(it.key),
                            it.obj->GetFormID())) iconObj = v;
                }
                const IconCache::Icon* iconPtr = cache->Get(iconObj);
                if (iconObj != it.obj) {
                    // ALWAYS queue the variant (no-op when its current key is
                    // cached): during live EDIT the pin fallback keeps Get()
                    // returning the last completed capture, so a miss-only
                    // queue would never capture the new rotation (the icon
                    // froze after the first change until EDIT closed).
                    cache->QueueCapture(iconObj);
                    if (!iconPtr) {
                        // not captured yet: show the base pouch icon meanwhile
                        // (the pouch must NEVER go blank)
                        iconPtr = cache->Get(it.obj);
                    }
                }
                // ★GI51: a tile is drawable from the moment it exists. A 3D
                // capture RAISES the quality; it is not the precondition for
                // showing anything. Before this, an item with no capture yet
                // was an empty frame — and a heavily modded load order is
                // mostly such items for the first several minutes, which is
                // the single most reported complaint. The capture is still
                // queued: the category icon holds the seat, it doesn't take it.
                bool viaFallback = false;
                Fallback::KeyXform fbx;   // GI60: the icon key's own transform
                if (!iconPtr) {
                    cache->QueueCapture(iconObj);
                    const auto fb = Fallback::GetDrawn(iconObj);
                    iconPtr = fb.icon;
                    fbx = fb.x;
                    viaFallback = iconPtr != nullptr;
                }
                // An item def that names a value beats the icon default; an
                // untouched field (1 / 0 / 0) means "follow the icon".
                const float fSc = it.def.fscale != 1.0f ? it.def.fscale : fbx.scale;
                const float fRot = it.def.frot != 0.0f ? it.def.frot : fbx.rot;
                const float fOfs = it.def.fx != 0.0f ? it.def.fx : fbx.x;
                const float fOfsY = it.def.fy;   // the key xform has no Y
                if (const auto* icon = iconPtr) {
                    // M1: mask-aware centre + size. fill == 1 and centre ==
                    // box middle for every solid rectangle.
                    const MaskFit fit = FitOf(it.mask, p0);
                    float dw, dh;
                    if (viaFallback) {
                        // ★A category icon is a SQUARE 128px drawing, not an
                        // alpha-trimmed model shot. Fitting it to the LONG axis
                        // (correct for a capture, which is trimmed to the
                        // model's real bounds) blew a square up to the long side
                        // of a 1x2 cell and it spilled over the neighbours — you
                        // could no longer tell which cell the item was in.
                        // CONTAIN it instead, at the same ratio the container
                        // window uses, so one item is one size everywhere.
                        const float sc = (std::min)(w / static_cast<float>(icon->w),
                                                    h / static_cast<float>(icon->h)) *
                                         0.85f * fSc * fit.fill;
                        dw = icon->w * sc;
                        dh = icon->h * sc;
                    } else {
                        // alpha-trimmed sprite: aspect-preserving fit — long
                        // axis = footprint long axis * def.scale (draw-time
                        // zoom: linear, instant, structurally clip-free)
                        const float target =
                            (std::max)(w, h) * 0.95f * it.def.scale * fit.fill;
                        const float ms = static_cast<float>((std::max)(icon->w, icon->h));
                        dw = icon->w / ms * target;
                        dh = icon->h / ms * target;
                    }
                    // ★No longer drawn-icons-only. The nudge existed to correct
                    // a ROTATED DRAWING's centre of mass, which a 3D capture
                    // does not have -- but a free-form footprint gives a capture
                    // the same problem from the other side: an L or a T has an
                    // off-centre place the sprite belongs in, and the automatic
                    // centre cannot know which. Both styles get to say.
                    const ImVec2 nudge = RotatedOffset(fOfs, fOfsY, it.rot);
                    const ImVec2 c(fit.centre.x + nudge.x, fit.centre.y + nudge.y);
                    const float  deg = (viaFallback ? fRot : 0.0f) + it.rot * 90.0f;
                    DrawItemShadow(dl, icon->srv, c, dw, dh, deg);
                    UIRoot::DrawItemIconRot(dl, icon->srv, c, ImVec2(dw, dh), deg);
                    // Phase 5/6: in barter, dim what can't be sold — coins
                    // (mirror) and items the merchant won't buy (category /
                    // stolen). The tile carries its own answer.
                    if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter &&
                        (it.coinValue >= 0 ||
                         !LootBarter::MerchantBuys(it.obj, it.stolen))) {
                        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h),
                            IM_COL32(8, 8, 8, 150));
                    }
                    // ★A search dims the misses instead of hiding them — same
                    // wash as "the merchant won't buy this", a shade lighter so
                    // the two states stay distinguishable when they overlap.
                    if (SearchMisses(it.key)) {
                        // ★Per CELL: over the bounding box this dimmed the
                        // empty notch too, darkening whatever item was sitting
                        // in it — a filter that hides items it did not match.
                        for (int my = 0; my < it.mask.h; ++my) {
                            for (int mx = 0; mx < it.mask.w; ++mx) {
                                if (!it.mask.rows[my][mx]) continue;
                                const ImVec2 c0(p0.x + mx * CellPx(),
                                                p0.y + my * CellPx());
                                dl->AddRectFilled(c0,
                                    ImVec2(c0.x + CellPx(), c0.y + CellPx()),
                                    IM_COL32(6, 6, 10, 168));
                            }
                        }
                    }
                } else {
                    // Neither a capture nor a category icon (asset folder
                    // missing, or a form no rule matches) -> placeholder frame.
                    // The queue request already happened above, which is what
                    // lets the grid heal a cache miss on its own: a style flip
                    // changes which sprite Get() looks for, and captures used
                    // to be queued only from Rebuild.
                    // ★Follows the mask: a rectangle here framed cells the item
                    // does not own, and this frame is precisely what a player
                    // stares at while a heavily modded load order catches up on
                    // captures — the one moment the footprint is all there is
                    // to look at. The 4px inset and the rounding are gone with
                    // the rectangle; an outline that traces the shape says more
                    // than a rounded box that does not.
                    DrawMaskOutline(dl, it.mask, p0, Theme::Acc(0.35f), 1.0f);
                }

                // GI8: extension overlay (socket wells). Between the sprite and
                // the corner chrome, so the count badge and marker tray stay
                // readable on top of it.
                if (!Editor::IsEditMode()) {
                    Badges::TileShape shape;
                    shape.w = it.mask.w;
                    shape.h = it.mask.h;
                    shape.cells = 0;
                    for (int my = 0; my < it.mask.h && my < 8; ++my) {
                        for (int mx = 0; mx < it.mask.w && mx < 8; ++mx) {
                            if (it.mask.rows[my][mx]) shape.cells |= 1ull << (my * 8 + mx);
                        }
                    }
                    // ★The grid already decided who owns the cell under the
                    // cursor (see ownerIdx). Asking the mouse about the
                    // bounding box answered yes on the empty notch of a
                    // free-form tile, so a socket lit up for a cell this item
                    // does not even occupy.
                    const bool bh = (idx == ownerIdx);
                    Badges::Draw(dl, p0, w, h, shape,
                                 0x14u,   // the player's own grid
                                 it.obj->GetFormID(), it.uid, bh);
                }

                // (★GI69: the NEW mark moved to pass 2 -- it is a wash on the
                // cell now, not a glow on the border, so it belongs UNDER the
                // sprite with the rest of the cell shading.)

                // Mabinogi-style TOP-LEFT badge: stack count — or, on a coin
                // tile, the gold value it represents (G2)
                {
                    char buf[16];
                    buf[0] = '\0';
                    const RE::FormID bfid = it.obj->GetFormID();
                    if (GoldCoins::IsCoinForm(bfid) && !GoldCoins::IsPouch(bfid)) {
                        std::snprintf(buf, sizeof(buf), "%d", it.coinValue);   // G4: stored on the tile
                    } else if (it.count > 1) {
                        std::snprintf(buf, sizeof(buf), "%d", it.count);
                    }
                    if (buf[0]) DrawCountBadge(dl, p0, buf);
                }

                // marker tray (BOTTOM-RIGHT, user-picked system 3): one row of
                // shape+colour coded state markers, anchored right, fixed order
                // favorite ◆(hi) · poison ●(green drop) · stolen ●(crimson).
                // The top-left stays reserved for the count/coin badge.
                // ★★1.0.5: the QUEST triangle is gone — the property it marked
                // is already enforced (a quest item refuses to be dropped or
                // sold and says so), the items are rare, and it cost a marker
                // slot on every tile that had one. Poison came down from the
                // top-right corner to join the row. Everything is drawn by the
                // shared tray now; see Grid.h.
                // ★★1.0.5 — anchored to an OCCUPIED cell, not the bounding
                // box. On a free-form footprint the box's corner can be an
                // empty notch, and a favourite diamond parked there reads as
                // belonging to whatever item is placed in that notch (user
                // screenshot). Solid rectangles are unaffected: their corner
                // cell IS the box corner.
                const ImVec2 trayCell = AnchorCell(it.mask, p0, /*bottom*/ true);
                DrawMarkerTray(dl, trayCell,
                               ImVec2(trayCell.x + CellPx(), trayCell.y + CellPx()),
                               it.fav,
                               it.stolen,
                               (it.glow & 0x4) != 0,
                               it.worn);   // P2/3-1: a worn bag says so

                // ★1.0.5 rarity: one wedge at the footprint's top-right, over
                // the sprite. Drawn here rather than in the occupancy pass so a
                // multi-cell item gets ONE mark instead of one per cell.
                const ImVec2 wedgeCell = AnchorCell(it.mask, p0, /*bottom*/ false);
                DrawRarityWedge(dl, wedgeCell,
                                ImVec2(wedgeCell.x + CellPx(), wedgeCell.y + CellPx()),
                                it.glow,
                                it.obj ? Lotd::Of(it.obj->GetFormID())
                                       : Lotd::Status::kNotRelic);

                // ★★★THE NEW MARK, and it goes AFTER the wedge on purpose.
                //
                // Both live in the footprint's top-right cell -- the rarity
                // wedge is a triangle in that exact corner -- so whichever is
                // drawn last is the one you see. New wins, and it should: the
                // wedge is a permanent fact about the item and will still be
                // there tomorrow, while "new" clears the moment the tile is
                // looked at (GI65). Covering a lasting mark with a transient
                // one costs nothing; the other order costs the whole feature,
                // because the case that needs it most -- a small item in a
                // full board -- is the case with a wedge on it.
                //
                // ★★FIXED WHITE WITH THE TRAY'S OWN RIM, and the argument is
                // already written down a few functions along, at kFavCol: the
                // favourite mark used to take the skin's accent and read as a
                // different thing per skin, so it was pinned to white and given
                // a near-black outline instead. That outline is what carries
                // it on parchment -- the colour never has to fight the ground,
                // because the ground never touches it.
                //
                // The same is true here, and more so: this mark means "arrived
                // since you last looked", which is not a statement about the
                // wallpaper either. Deriving it from the skin's luminance was
                // one skin-dependent answer replacing another; a white dot
                // ringed in black is one mark that means one thing on all
                // twenty boards.
                // ★The SIZE comes from the tray's own fractions, not from
                // numbers that happen to look right. kMarkFrac and kInsetFrac
                // are what a tray marker measures itself by, so the two marks
                // stay the same size when the cell size changes and when
                // somebody retunes one of those constants. Hand-picked pixels
                // drifted small the first time and would have drifted again.
                if (g_newTiles.contains(it.key)) {
                    constexpr ImU32 kNewDot = IM_COL32(245, 242, 234, 255);
                    constexpr ImU32 kNewRim = IM_COL32(11, 11, 11, 255);
                    const float cell  = CellPx();
                    const float inset = cell * kInsetFrac;
                    // ★★SIZED BETWEEN THE TWO HONEST ANSWERS, because both were
                    // measured and both were wrong. Area-matched (x0.80) the dot
                    // read smaller than the diamond; width-matched (x1.00) it
                    // read bigger -- a circle fills its width, a diamond only
                    // touches it at four points, and the eye splits the
                    // difference. So does this: the geometric mean of the two,
                    // judged against the diamond in a screenshot at each step.
                    const float r = cell * kMarkFrac * 0.5f * 0.89f;
                    // ★...but the RIM has to be thicker than the tray's, which
                    // is not a fudge. RimPx() is one pixel at ordinary UI
                    // scales, and one antialiased pixel spread around a CURVE
                    // covers less of any given pixel than the same pixel laid
                    // along the diamond's four straight edges. Equal numbers,
                    // unequal weight. This is the number that makes them look
                    // like the same outline. 1.7 overshot -- the dot read as
                    // bigger than the diamond, because a rim grows the mark on
                    // BOTH sides of r. This is the smallest bump that still
                    // reads as the same weight of line.
                    const float rimW = RimPx() * 1.3f;
                    // The tray hugs the bottom edge; this is the same corner
                    // arithmetic mirrored to the top.
                    const ImVec2 c(wedgeCell.x + cell - r - inset,
                                   wedgeCell.y + r + inset);
                    dl->AddCircleFilled(c, r, kNewDot, 20);
                    dl->AddCircle(c, r, kNewRim, 20, rimW);
                }

                // H1: edit-mode selection highlight (skin sel colour)
                // ★★1.0.5 — the GRID always uses an even outline, on every skin.
                // It used to branch on sk.cornerFade (corner-fade skins got the
                // doubled 1px-inset fade instead). Two reasons it does not:
                //
                //   1. A cell's ground is a hard-edged rectangle here, so an
                //      even line sits on it more naturally than a fade whose
                //      whole idea is soft, empty edges.
                //   2. Free-form footprints (PLAN_POLYOMINO) turn "the four
                //      corners" into a question — a T has six convex corners
                //      and two concave ones, a ring has two separate outlines.
                //      An even line generalises to a polyline for free; a
                //      corner fade has to be told which ends of which edge are
                //      convex before it can draw anything.
                //
                // The EQUIP doll keeps the corner fade (Equip.cpp): its slots
                // are always rectangles, and that fade is those skins' look.
                //
                // ★★And it follows the MASK, not the bounding box — a selection
                // ring around empty cells says the item is somewhere it is not.
                // This is the payoff of the even-line decision: a polyline
                // traces any shape, a corner fade would first have to be told
                // which ends of which edge are convex.
                // ★IsEditMode() first: BaseKey returns a std::string BY VALUE,
                // and a tile key is past SSO, so this was a heap allocation per
                // tile per frame to answer a question that is "no" whenever the
                // editor is shut -- which is always, in play. Same move the
                // DefKeyOf call below already made.
                if (Editor::IsEditMode() && Editor::IsSelected(BaseKey(it.key))) {
                    DrawMaskOutline(dl, it.mask, p0, Theme::Col(sk.sel, 1.0f), 2.0f);
                }

                // hover / pickup / bag-toggle surface
                ImGui::SetCursorScreenPos(p0);
                // ★PushID hashes the key in place; "##it_" + it.key built and
                // destroyed a second heap string for every tile, every frame.
                ImGui::PushID(it.key.c_str());
                ImGui::InvisibleButton("##it", ImVec2(w, h));
                ImGui::PopID();
                // ★★The button still exists — it reserves the rect and gives
                // this tile an ImGui id (drag, tooltips and Sfx all key off
                // it) — but it is NOT what decides the hit. The GRID decides:
                // `ownerIdx` is the item owning the cell under the cursor, and
                // only that tile answers. That is what lets an item sitting in
                // an L's notch be hovered AND clicked, which one-button-per-
                // item cannot do no matter how its result is filtered.
                //
                // ★IsItemHovered() is deliberately NOT part of this. ANDing it
                // in would re-introduce the very bug: on the frame ImGui hands
                // hover to the overlapping L, the item that actually owns the
                // cell reads false and the click is swallowed by both. ImGui's
                // gate is applied once per pass instead (`winHov` above).
                const bool mine = (idx == ownerIdx);
                auto tileHovered = [&] { return mine; };
                auto tileClicked = [&](ImGuiMouseButton a_btn) {
                    return mine && ImGui::IsMouseClicked(a_btn);
                };
                // overlay chrome (popups/settings/EDIT panel) extends beyond
                // the ImGui window rect — block hover/interaction underneath
                const bool ovl = UIRoot::MouseInOverlay();
                // C: vanilla "Item Zoom" (controlmap Item Zoom = 0x2E) — the
                // rotatable 3D view, the ONLY way to read detail that lives on
                // the model (dragon-claw glyphs = door puzzle solution).
                // Works in EDIT mode too, and targets the same object/key the
                // editor would select so an adopted angle lands on that def.
                auto inspectHere = [&]() {
                    if (iconObj != it.obj) UIRoot::OpenInspect(iconObj, FormKey(iconObj));
                    else                   UIRoot::OpenInspect(it.obj, BaseKey(it.key));
                };
                if (!g_held && !ovl && tileHovered() &&
                    ImGui::IsKeyPressed(ImGuiKey_C, false) &&
                    !ImGui::GetIO().WantTextInput) {
                    inspectHere();
                }
                if (!g_held && !ovl && tileHovered()) {   // v9 hover tint
                    // ★The tint follows the MASK, not the box: on an L the
                    // empty notch used to light up with the item.
                    for (int my = 0; my < it.mask.h; ++my) {
                        for (int mx = 0; mx < it.mask.w; ++mx) {
                            if (!it.mask.rows[my][mx]) continue;
                            const ImVec2 c0(p0.x + mx * CellPx(), p0.y + my * CellPx());
                            dl->AddRectFilled(c0,
                                ImVec2(c0.x + CellPx(), c0.y + CellPx()),
                                Theme::Acc(0.10f));
                        }
                    }
                    Sfx::HoverNote(ImGui::GetItemID());   // cursor entered this tile
                    g_newTiles.erase(it.key);   // GI65: looked at = no longer new
                }
                if (Editor::IsEditMode()) {
                    // H1: clicks select for editing (no carry, no equip).
                    // Split tiles ("#k") edit their BASE key — the def store
                    // is per-form, so overrides apply to every copy.
                    // Pouch: edit the CURRENTLY DISPLAYED variant (N/S/M/F)
                    // — set the stored amount to a band to tune that band's
                    // icon; the tile always shows what is being edited.
                    if (!ovl && tileClicked(ImGuiMouseButton_Left)) {
                        if (iconObj != it.obj) {
                            Editor::Select(iconObj, FormKey(iconObj));
                        } else {
                            Editor::Select(it.obj, BaseKey(it.key));
                        }
                    }
                    if (!ovl && tileHovered()) {
                        // EDIT mode's name box is a tooltip too — same ground,
                        // same hairline, same ink as the item tooltip
                        Theme::PushTipStyle();
                        ImGui::SetTooltip("%s", it.obj->GetName());
                        Theme::PopTipStyle();
                    }
                } else if (!g_held && !ovl) {
                    if (tileHovered()) {   // tooltip suppressed while carrying
                        const RE::FormID fid = it.obj->GetFormID();
                        const bool isCoin = GoldCoins::IsCoinForm(fid) &&
                                            !GoldCoins::IsPouch(fid);
                        if (!isCoin) {   // coins: value badge only, no tooltip
                            // Phase 4/5: in barter, the player side shows the SELL
                            // price — including the pouch (a real sellable item).
                            int price = -1;
                            if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter) {
                                const int val = TileValue(it.obj, it.uid, it.sig);   // GI43
                                if (val > 0) price = LootBarter::SellPrice(it.obj, val);
                            }
                            // D1: a player TILE is exactly one unit — read its
                            // own sub-stack, not "any list on the entry"
                            // ★GI63: a coin tile has to hand over ITS OWN value.
                            // Passing -1 made the tooltip think the tile held no
                            // gold, so "can this be split" was false on every
                            // coin -- the one item type where splitting is the
                            // main thing you do with it.
                            DrawItemTooltip(it.obj, it.count,
                                GoldCoins::IsPouch(fid) ? GoldCoins::PouchStoredOf(it.key)
                                                        : it.coinValue,
                                price, false, nullptr, ExtraScope::kUnit,
                                it.uid, it.xlIdx, 0, 0,
                                TileContext{ it.key, it.def.bag != 0,
                                             it.inBag == kTrashKey, false, false,
                                             it.stolen, it.quest });
                            // (1.3.1) T = recharge, the vanilla ChargeItem key.
                            // OpenRecharge validates (enchanted weapon, not
                            // already full) and stays silent otherwise.
                            if (ImGui::IsKeyPressed(ImGuiKey_T, false) &&
                                !ImGui::GetIO().WantTextInput) {
                                OpenRecharge(it.obj, it.uid, it.sig, false, 0);
                            }
                        } else {
                            // ★GI63: a coin draws no tooltip on purpose -- the
                            // amount badge already says everything a card could.
                            // But the prompt bar reads the TOOLTIP's record, so
                            // skipping it left gold as the one thing you could
                            // hover with no keys shown at all. Record it here.
                            //   no verb  -- coins have no right-click action
                            //   no star  -- a coin is a mirror of the ledger
                            //   no compare, and split only above 1 gold
                            g_hoverPrompt = { ImGui::GetFrameCount(),
                                              it.coinValue > 1, false,
                                              true, false, false, {} };
                        }
                        // D1: vanilla-style discard — hover + R drops ONE unit
                        // (spam R for more; carry-outside stays the full-stack drop).
                        // G2: a coin drops its VALUE as real world gold; the
                        // pouch drops as an item — its stored gold travels
                        // with it (container sink handles the ledger).
                        if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
                            !ImGui::GetIO().WantTextInput) {
                            if (it.quest) {   // Phase 7: can't drop
                                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                            } else if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) {
                                // G4: a pinned purse releases its fixed value; an
                                // auto coin drops its ordinal value. Either way
                                // DropAsGold debits the (now-walking) gold.
                                GoldCoins::DropAsGold(it.coinValue);
                                g_layout.erase(it.key);   // free THIS slot; rebuild re-maps survivors by position
                                RequestRebuild();
                            } else {
                                if (it.count <= 1) {   // last unit: tile disappears
                                    g_layout.erase(it.key);
                                    if (it.def.bag != 0) {   // E4: contents back to main
                                        g_openBags.erase(it.key);
                                        for (auto& [k, le] : g_layout) {
                                            if (le.bag == it.key) le.bag.clear();
                                        }
                                    }
                                }
                                if (g_dropWorld) {
                                    g_dropWorld(it.obj, 1,   // GI36: star dies with it
                                        ResolveExitUnit(it.obj, it.uid, it.sig, 1,
                                                        it.fav ? 1 : 0, it.xlIdx));
                                }
                                RequestRebuild();
                            }
                        }
                        // F: vanilla favorite toggle (feeds the Q menu);
                        // coins/pouch are mirror artefacts — not favoritable
                        if (ImGui::IsKeyPressed(ImGuiKey_F, false) &&
                            !ImGui::GetIO().WantTextInput &&
                            !GoldCoins::IsCoinForm(fid)) {
                            ToggleFavorite(it.key, it.obj, it.uid, it.xlIdx, it.sig);
                            Sfx::Favorite();
                            // ★S1: no rebuild -- the star lands when the engine
                            // applies it (ProcessFavorites), and THAT refreshes
                            // this form's tiles in place. The rebuild here drew
                            // a board on which nothing had changed yet.
                        }
                    }
                    if (tileClicked(ImGuiMouseButton_Left)) {   // C1: pickup
                        const RE::FormID lfid = it.obj->GetFormID();
                        const bool coin = GoldCoins::IsCoinForm(lfid) && !GoldCoins::IsPouch(lfid);
                        if (io.KeyShift && coin && it.coinValue > 1) {
                            // G4: shift+left-click on a gold tile = VALUE split
                            // slider (1..coinValue, starts at half). The chosen
                            // amount lands on the cursor as a pinned purse.
                            LootBarter::OpenSlider(it.obj, it.coinValue, LootBarter::XferDir::kPickup, it.key);
                        } else if (io.KeyShift && GoldCoins::IsPouch(lfid)) {
                            // G2: shift+left-click on the pouch = withdraw
                            // window, same as right-click (user shortcut)
                            g_pouchOpen = true;
                            g_pouchTile = it.key;
                            g_pouchSlider = (std::max)(1, GoldCoins::PouchStoredOf(it.key) / 2);
                            Sfx::SelectOn();
                        } else if (io.KeyShift && it.count > 1 && !coin) {
                            // shift+left-click on a stack = split slider (ALWAYS
                            // — plain inventory too). The chosen amount lands on
                            // the cursor; drop it on the container to store, or
                            // outside every window to discard just that many.
                            LootBarter::OpenSlider(it.obj, it.count, LootBarter::XferDir::kPickup, it.key);
                        } else {
                            // ★GI62c/d: the cursor takes the item by its PIVOT
                            // CELL, wherever it was clicked. Holding the clicked
                            // point (the old F7 behaviour) meant the first A/D
                            // had to re-grab, so the item slid AND turned in one
                            // instant -- and a turn that travels is not read as
                            // a turn. Gripping the pivot from the start makes
                            // every rotation a clean revolution about one cell.
                            g_held = Held{ it.key, it.obj, it.mask, it.count, it.def.bag != 0,
                                           it.def.scale, 0.0f, 0.0f, true };
                            g_held->coinValue = it.coinValue;   // G4: -1 for non-coins
                            g_held->xlIdx = it.xlIdx;           // GI1
                            g_held->uid = it.uid;
                            g_held->sig = it.sig;               // GI25
                            g_held->fav = it.fav;               // GI36
                            g_held->stolen = it.stolen;         // (1.3.2)
                            g_held->quest  = it.quest;
                            g_held->SetRot(it.rot);               // GI62: lift it as it lies
                            HoldByPivot(*g_held, it.def);         // GI62d
                            if (g_poolTrace) {
                                const auto le = g_layout.count(it.key) ? g_layout[it.key]
                                                                      : LayoutEntry{};
                                SKSE::log::info("[ACT] lift-from-grid '{}' key '{}' at [{},{}]",
                                    it.obj->GetName(), it.key, le.col, le.row);
                            }
                            if (g_sound) g_sound(it.obj, true);
                            // ★S1: the cells free next frame as they always
                            // did -- via the deferred STASH now, not a full
                            // rebuild (FinishFrame runs it; a miss rebuilds)
                            g_held->needsDetach = true;
                        }
                    } else if (tileClicked(ImGuiMouseButton_Right)) {
                        // bag / pouch right-click is ALWAYS manage (toggle /
                        // withdraw), mode-independent — trade & storage happen
                        // by drag only (confirmed spec). Everything else
                        // branches on the UI mode so loot/barter never fires
                        // the equip action.
                        // ★S1: a branch that repainted itself (or changed no
                        // board state at all) says so, and the catch-all
                        // rebuild at the chain's end stands down for it.
                        bool rcQuiet = false;
                        if (it.inBag == kTrashKey) {
                            // F2: right-click on a PARKED tile = restore. Its
                            // pre-park spot is tried first; taken/gone -> the
                            // placer first-fits (col -1).
                            LayoutEntry back;   // default: first-fit to main
                            back.col = -1;
                            back.row = -1;
                            back.count = it.count;
                            if (const auto ri = g_trashReturn.find(it.key);
                                ri != g_trashReturn.end()) {
                                back = ri->second;
                                back.count = it.count;
                                // ★THE KEY FIRST, THE NODE SECOND. Reversed,
                                // map::erase(ri) destroys the node and the next
                                // line reads `ri->first` out of freed memory --
                                // a tile key is 19 chars, past SSO, so that is
                                // a freed heap buffer handed to erase().
                                g_trashXl.erase(ri->first);
                                g_trashReturn.erase(ri);
                            }
                            g_layout[it.key] = back;
                            if (g_sound) g_sound(it.obj, true);
                            // ★S1: the restore is ONE tile moving back to the
                            // board -- queued (this runs mid-draw), seated at
                            // FinishFrame; an occupied spot first-fits there.
                            g_viewMoveQ.push_back({ it.key, back.bag,
                                                    back.col, back.row });
                            rcQuiet = true;
                        } else if (it.def.bag != 0) {   // E2: bag right-click = window toggle
                            if (g_openBags.contains(it.key)) {
                                g_openBags.erase(it.key);
                                Sfx::BagClose();
                            } else {
                                g_openBags.insert(it.key);
                                Sfx::BagOpen();
                            }
                            // ★S1: the window follows the flag IN PLACE -- the
                            // view (and its placement) exists either way, open
                            // only decides the drawing (its own stated rule).
                            for (auto& v : g_views) {
                                if (v.bagKey == it.key) {
                                    v.open = g_openBags.contains(it.key);
                                    break;
                                }
                            }
                            rcQuiet = true;
                        } else if (GoldCoins::IsPouch(it.obj->GetFormID())) {
                            g_pouchOpen = true;   // G2: withdraw window
                            // start at half the stored amount (split-friendly default)
                            g_pouchTile = it.key;
                            g_pouchSlider = (std::max)(1, GoldCoins::PouchStoredOf(it.key) / 2);
                            Sfx::SelectOn();
                            rcQuiet = true;   // ★S1: a window opened, no tile moved
                        } else if (RightClickIntoTrash(it)) {
                            // ★★While the TRASH is open, a right-click bins the
                            // tile instead of doing whatever it would otherwise
                            // do -- and this is where that override belongs.
                            // ★BELOW the restore, so a tile already in the bin
                            // still comes back out. Below the bag and pouch
                            // cases, because their right-click is declared
                            // mode-independent management just above and an
                            // open bin is another mode: taking the toggle away
                            // would remove the organising tool at exactly the
                            // moment it is being used. Both still go in by
                            // drag, as they always did.
                            // ★ABOVE the mode branches, so loot, barter and
                            // pickpocket all give way to it -- with the bin
                            // open, "throw this away" is the intent on screen.
                            // ★Returns false when the bin is shut, so every
                            // branch below is untouched in ordinary play.
                            rcQuiet = true;   // ★S1: park queued / refusal said no
                        } else if (LootBarter::IsLootMode(LootBarter::CurrentMode())) {
                            // loot: right-click stores this tile into the
                            // container — a stack (>1) opens the quantity
                            // slider first. Coins are mirror artefacts
                            // (excluded); the pouch stores fine (gold travels
                            // via the container sink).
                            // ★ONE PATH / O-2: the whole branch is now one
                            // line and a road name. Everything it used to do --
                            // the room check, the slider for a stack, the store
                            // request, the pending remove, the display's exit --
                            // already existed in WholeStore, written once for
                            // the drag. This was the second copy.
                            // (bags never reach here: their right-click is the
                            // window toggle above, and their storage is
                            // DRAG-only -- that path bundles the contents.)
                            const RE::FormID fid = it.obj->GetFormID();
                            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                                g_clickAction =
                                    ClickAction{ it.key, ClickRoute::kStore };
                            }
                        } else if (LootBarter::CurrentMode() ==
                                   LootBarter::Mode::kPickpocket) {
                            // F6b: right-click = reverse-pickpocket this tile
                            // onto the mark (engine roll on the Tick)
                            // ★ONE PATH / O-3: the plant's own copy retires.
                            // Quest guard, slider, roll request -- all of it is
                            // WholePickStore, written once for the drag.
                            const RE::FormID fid = it.obj->GetFormID();
                            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                                g_clickAction =
                                    ClickAction{ it.key, ClickRoute::kPlant };
                            }
                        } else if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter) {
                            // Phase 5: SELL this tile to the merchant. A stack
                            // opens the quantity slider; a single item sells at
                            // once. Coins excluded (mirror); the pouch sells via
                            // the gold-travel path (§2-C). Blocked when the
                            // merchant can't afford it.
                            // ★ONE PATH / O-3: likewise -- the price check,
                            // the merchant's purse, the star's confirm and the
                            // slider all live in WholeSell now, and the star's
                            // confirm reached the drag for the first time on
                            // the way (it had only ever guarded this click).
                            const RE::FormID fid = it.obj->GetFormID();
                            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                                g_clickAction =
                                    ClickAction{ it.key, ClickRoute::kSell };
                            }
                        } else if (auto* bk = it.obj->As<RE::TESObjectBOOK>()) {
                            // Books and notes had no reachable action at all:
                            // right-click fell through to equip, which rejects
                            // them, so a quest note could be carried but never
                            // read (user report).
                            // ★★★AND A SPELL TOME IS READ LIKE ANY OTHER BOOK.
                            // It used to keep the EQUIP path, because equipping
                            // a tome does teach the spell -- but that is the
                            // raw learn, not the reading. Vanilla opens the
                            // page and settles the tome when the page closes,
                            // and everything hung off that moment -- a skill
                            // gate, a quest fragment, a mod's prompt -- runs
                            // there. Reported: "the game normally says you lack
                            // the skill; with this mod there is no prompt, the
                            // book vanishes and the spell is added."
                            //
                            // We were not showing the book at all. Now we do,
                            // and the engine decides what reading it means.
                            RequestBookRead(bk, it.uid, it.sig);
                        // ★Coins are a MIRROR of the gold ledger, not real
                        // tiles, and the prompt bar already offers them no verb.
                        // The old type gate refused them for the wrong reason
                        // (they are MISC) and dropping the gate would have let a
                        // click reach the engine with a coin form in hand — the
                        // one place in this window where that is never wanted.
                        // The pouch is excluded above by its own branch.
                        } else if (GoldCoins::IsCoinForm(it.obj->GetFormID())) {
                            // ★Right-click banks THIS TILE's gold, and only it.
                            // The amount is the tile's own (coinValue), never
                            // the walking total, so three 1000 G tiles deposit
                            // a thousand at a time and the other two stay put.
                            //
                            // Order matters. Unpin FIRST: a pinned purse is
                            // subtracted from walking gold, and StoreToPouch
                            // draws FROM walking -- ask before unpinning and it
                            // sees a pool the purse is not in and stores 0.
                            // Same sequence the drop-on-pouch path uses.
                            //
                            // Restore the pin when nothing was stored (pouch
                            // full, or gone), so a refused deposit leaves the
                            // board exactly as it was. A PARTIAL store needs no
                            // restore: the remainder is already back in walking
                            // gold and redraws itself as auto coin tiles.
                            //
                            // ★★ERASE THE SLOT, pinned or not. An AUTO tile is
                            // not a free-floating decoration -- it owns a
                            // g_layout entry like everything else, and the
                            // rebuild re-keys this form's auto slots #0,#1,...
                            // BY GRID POSITION (see the coin branch in the
                            // rebuild). So the slots decide which cells exist
                            // and the walking total decides how many. Take the
                            // gold without erasing a slot and the count simply
                            // drops by one -- the rebuild keeps the front cells
                            // and truncates the LAST one, which is why clicking
                            // the first of eight emptied the eighth. Erasing
                            // this slot is what makes the emptied cell the one
                            // that was clicked; the survivors re-map around it.
                            // Same reason the drop-on-pouch path erases too.
                            if (GoldCoins::PouchHeld() && it.coinValue > 0) {
                                const int v = it.coinValue;
                                // ★S-G: the tile's record IS the amount -- a
                                // partial store shrinks it, a full one erases
                                const int stored = GoldCoins::StoreToPouch(v);
                                if (stored > 0 && stored < v) {
                                    SetCoinRecord(it.key, v - stored);
                                }
                                if (stored >= v) {
                                    g_layout.erase(it.key);
                                    if (g_sound) g_sound(it.obj, false);
                                }
                                // refused outright: the record never moved
                            }
                        } else {
                            // ★ONE PATH / O-1: D3's right-click -- use, or put
                            // on -- is now a TRANSFER like any other, and it
                            // travels the road a drag travels. All this branch
                            // does is name the tile; FinishFrame lifts it into
                            // a carry and hands it to the USE route, where the
                            // guards and the bookkeeping live (see WholeUse).
                            //
                            // The quest-consume refusal moved there with it:
                            // a guard belongs beside the destination it is
                            // guarding, not beside the click that aimed at it.
                            g_clickAction =
                                ClickAction{ it.key, ClickRoute::kUse };
                        }
                        // ★S1: only branches that neither routed (clickAction)
                        // nor spoke for themselves (rcQuiet) still pay the
                        // catch-all -- today that is the mode branches' inline
                        // engine work and the coin actions (§7's rule).
                        if (!g_clickAction && !rcQuiet) RequestRebuild();
                    }
                }
            }
        }

        // GI62: A / D turn the carried item one quarter (A anticlockwise,
        // D clockwise). Rotation exists ONLY while carried, which is what keeps
        // it simple: the slot a turn applies to is never in question -- it is
        // the one the player is about to drop onto, and the drop already proves
        // the turned footprint fits before it lands.
        //
        // ★Runs at the TOP of the frame, before any grid computes its drop
        // candidate. Turning it at FinishFrame (after every grid drew) would
        // leave the ghost and the collision test a frame behind the sprite.
        void RotateHeldItem()
        {
            if (!g_held) return;
            auto& held = *g_held;

            // ★GI62b: the sprite EASES to the new angle while the footprint
            // snapped to it instantly. Both halves of that matter. The snap is
            // what the drop is judged against, so it can never lag; the ease is
            // the only thing that says "this turned" rather than "this became a
            // different shape". Without it a 1x2 reads as a silent swap of width
            // for height -- on an even-sided footprint the before and after
            // share just one cell, so there is no visible pivot to infer a
            // rotation from (an odd 1x3 keeps its middle cell and reads as a
            // cross, which is why only the long items felt like they turned).
            if (held.rotT < 1.0f) {
                constexpr float kTurnSec = 0.18f;
                const float dt = (std::min)(ImGui::GetIO().DeltaTime, 0.1f);
                held.rotT = (std::min)(1.0f, held.rotT + dt / kTurnSec);
                // ease-out-back: overshoots ~9 degrees, then settles. A plain
                // decay curve was tried first -- it eases INTO the target and
                // the last 20 degrees crawl, which reads as the sprite drifting
                // rather than being turned. The small overshoot is what makes
                // the eye register a quarter-turn as an event.
                constexpr float c1 = 1.70158f;
                constexpr float c3 = c1 + 1.0f;
                const float p = held.rotT - 1.0f;
                const float e = 1.0f + c3 * p * p * p + c1 * p * p;
                held.rotDeg = held.rotFrom + (held.rotAim - held.rotFrom) * e;
                if (held.rotT >= 1.0f) held.rotDeg = held.rotAim;
            }

            if (ImGui::GetIO().WantTextInput) return;
            // A/D also step the quantity slider. The two cannot overlap in
            // practice (the slider resolves BEFORE a fragment reaches the
            // cursor), but one keypress must never mean two things.
            if (LootBarter::SliderActive()) return;
            // ★(1.3.2) REPORTED: "A/D do nothing". With the Korean IME in
            // hangul mode the OS routes the letter into composition and the
            // WM_KEYDOWN that feeds ImGui never arrives -- the PHYSICAL key
            // state does not lie (same trick the arrow keys use during text
            // input), so it backs the ImGui read up. A same-frame OR is one
            // press; the 2-frame guard stops the two paths double-firing
            // when they disagree by a frame.
            static bool s_aWas = false, s_dWas = false;
            static int  s_lastRotFrame = -1000;
            const bool aNow = (GetAsyncKeyState('A') & 0x8000) != 0;
            const bool dNow = (GetAsyncKeyState('D') & 0x8000) != 0;
            const bool aEdge = aNow && !s_aWas;
            const bool dEdge = dNow && !s_dWas;
            s_aWas = aNow;
            s_dWas = dNow;
            const bool ccw = ImGui::IsKeyPressed(ImGuiKey_A, false) || aEdge;
            const bool cw  = ImGui::IsKeyPressed(ImGuiKey_D, false) || dEdge;
            if (ccw == cw) return;   // neither, or both in the same frame
            if (ImGui::GetFrameCount() - s_lastRotFrame < 2) return;
            s_lastRotFrame = ImGui::GetFrameCount();
            const auto def = Grid::ResolveDef(held.obj);
            if (!CanRotate(def)) return;   // square footprint: nothing would move
            held.rotPrev = held.rot;
            held.rot = (held.rot + (cw ? 1 : -1) + 4) & 3;
            // Start THIS turn from whatever is on screen right now -- pressing D
            // twice quickly picks up mid-swing instead of jumping back.
            held.rotFrom = held.rotDeg;
            held.rotAim += cw ? 90.0f : -90.0f;   // unwrapped: always the short way
            held.rotT = 0.0f;
            held.mask = MaskOf(def, held.rot);
            HoldByPivot(held, def);   // GI62d: the pivot cell does not move
            Sfx::Focus();
        }

        // pass 5: carried item -> drop candidate + placement ghost (C2)
        void ComputeDropCandidate(View& a_view, int a_viewIdx, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            const auto& io = ImGui::GetIO();
            const float gridW = a_view.cols * CellPx();
            const float gridH = a_view.rows * CellPx();
            // ★★★ASK IMGUI WHO OWNS THE CURSOR, then measure. The rect test
            // below knows only geometry, and geometry does not include DEPTH:
            // with a merchant window laid over a bag, every cell of the bag it
            // covers still answered "the cursor is inside me", so selling into
            // the overlap stored the item in the hidden bag -- or, if that cell
            // was occupied, swapped and handed the buried item to the cursor
            // (user report). Windows on top of it were simply not part of the
            // question.
            // ★The same two gates pass 4 applies to HOVER, and for the same
            // reasons: IsWindowHovered resolves z-order, and MouseInOverlay
            // covers the chrome that popups/settings/EDIT draw OUTSIDE their
            // ImGui rect. Where an item can be picked up is where it can be
            // dropped -- one pass answering that differently from the other is
            // what let a drop land somewhere a click could not reach.
            // ★Both halves, and the flag -- see UIRoot::CursorOwnsWindow.
            const bool cursorIsOurs = UIRoot::CursorOwnsWindow();
            // carried item: this grid is the drop candidate while hovered (C2)
            if (g_held && cursorIsOurs &&
                io.MousePos.x >= base.x && io.MousePos.x < base.x + gridW &&
                io.MousePos.y >= base.y && io.MousePos.y < base.y + gridH) {
                auto& held = *g_held;
                const float px = io.MousePos.x - base.x - held.offX;
                const float py = io.MousePos.y - base.y - held.offY;
                int col = static_cast<int>(std::lround(px / CellPx()));
                int row = static_cast<int>(std::lround(py / CellPx()));
                col = (std::max)(0, (std::min)(a_view.cols - held.mask.w, col));
                row = (std::max)(0, (std::min)(a_view.rows - held.mask.h, row));

                g_target = {};
                g_target.has = true;
                g_target.view = a_viewIdx;
                g_target.col = col;
                g_target.row = row;

                // E4b: a bag may be dropped into a GENERAL bag (user request:
                // stow spare bags). Still refused: the trash (delete via drop
                // stays a non-goal), typed bags (they hold their filter, not
                // luggage), and any drop that would loop the containment chain.
                const bool bagInBag = held.isBag && !a_view.bagKey.empty() &&
                                      (a_view.bagKey == kTrashKey ||
                                       !a_view.accept.empty() ||
                                       NestsWithin(held.key, a_view.bagKey));
                const bool sizeOk = held.mask.w <= a_view.cols && held.mask.h <= a_view.rows;

                for (int idx : a_view.items) {
                    const Item* otherP = ItemAt(idx);   // guarded: see ItemAt
                    if (!otherP) continue;
                    const auto& other = *otherP;
                    if (other.overflow || other.col < 0) continue;
                    bool hit = false;
                    for (int y = 0; y < held.mask.h && !hit; ++y) {
                        for (int x = 0; x < held.mask.w && !hit; ++x) {
                            if (!held.mask.rows[y][x]) continue;
                            const int lc = col + x - other.col, lr = row + y - other.row;
                            if (lc >= 0 && lr >= 0 && lc < other.mask.w && lr < other.mask.h &&
                                other.mask.rows[lr][lc]) {
                                hit = true;
                            }
                        }
                    }
                    if (hit) g_target.blockers.push_back(idx);
                }
                // ★Typed bag: a hand drop obeys the same rule the routing does.
                // Without this the automatic half looks finished while the bag
                // still holds anything you drag in — and the code elsewhere
                // ASSUMES a typed bag's contents match its filter (the "bag is
                // full" signal is measured from its own kind overflowing).
                const bool wrongKind = !a_view.accept.empty() && held.obj &&
                                       BagFilter::FilterOf(held.obj) != a_view.accept;
                // ★W3: on the MAIN board the footprint must be OWNED -- the
                // unowned tail of a partial carry-weight row (and the growth
                // zone below it) draws, but a hand drop there is refused, the
                // same answer every queue verb gives. Red ghost says so.
                const bool ownedOk = !a_view.bagKey.empty() ||
                                     OwnedFootprint(col, row, held.mask);
                g_target.valid = !bagInBag && !wrongKind && sizeOk && ownedOk &&
                                 g_target.blockers.empty();

                // ghost: green = ok, red = badspot
                const ImU32 ghost = g_target.valid ? IM_COL32(90, 170, 90, 90)
                                                   : IM_COL32(190, 60, 60, 110);
                for (int y = 0; y < held.mask.h; ++y) {
                    for (int x = 0; x < held.mask.w; ++x) {
                        if (!held.mask.rows[y][x]) continue;
                        const ImVec2 p0(base.x + (col + x) * CellPx(), base.y + (row + y) * CellPx());
                        dl->AddRectFilled(p0, ImVec2(p0.x + CellPx(), p0.y + CellPx()), ghost);
                    }
                }

                // ★GI62b: while the turn plays, OUTLINE the footprint it just
                // left. Filled cells say "here is where it goes"; the outline
                // says "here is where it was", and the two together are the
                // rotation. This is what carries the odd cases: 1x3 turns into
                // a cross on its own, but 2x3 and 1x2 have no shared middle
                // cell, so without the departing shape beside the new one there
                // is nothing on screen that reads as a turn rather than a
                // resize. Anchored the way the OLD footprint would be anchored
                // under this very cursor, so the pair keeps the exact offset
                // the item actually moved by.
                if (held.rotT < 1.0f && held.rotPrev != held.rot) {
                    const auto pdef = Grid::ResolveDef(held.obj);
                    const Mask pm = MaskOf(pdef, held.rotPrev);
                    float pox = 0.0f, poy = 0.0f;
                    GripOffset(pdef, held.rotPrev, pox, poy);   // GI62d: same grip
                    const int pc = (std::max)(0, (std::min)(a_view.cols - pm.w,
                        static_cast<int>(std::lround(
                            (io.MousePos.x - base.x - pox) / CellPx()))));
                    const int pr = (std::max)(0, (std::min)(a_view.rows - pm.h,
                        static_cast<int>(std::lround(
                            (io.MousePos.y - base.y - poy) / CellPx()))));
                    const float fade = 1.0f - held.rotT;
                    const ImU32 line = IM_COL32(225, 205, 150,
                        static_cast<int>(170.0f * fade));
                    for (int y = 0; y < pm.h; ++y) {
                        for (int x = 0; x < pm.w; ++x) {
                            if (!pm.rows[y][x]) continue;
                            const ImVec2 q0(base.x + (pc + x) * CellPx(),
                                            base.y + (pr + y) * CellPx());
                            dl->AddRect(q0, ImVec2(q0.x + CellPx(), q0.y + CellPx()),
                                line, 0.0f, 0, 2.0f);
                        }
                    }
                }
            }
        }

        void DrawGridView(View& a_view, int a_viewIdx)
        {
            const float gridW = a_view.cols * CellPx();
            const float gridH = a_view.rows * CellPx();
            const ImVec2 base = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(gridW, gridH));

            DrawGridChrome(a_view, a_viewIdx, base);      // pass 1
            DrawOccupancyPass(a_view, base);              // pass 2
            DrawInkLattice(ImGui::GetWindowDrawList(), base,
                           a_view.cols, a_view.rows);     // pass 2b (ink only)
            // (the old pass 3 — rarity HALO — is gone: rarity is drawn as the
            //  cell's ground inside DrawOccupancyPass now. See Grid.h.)
            DrawItemsPass(a_view, base);                  // pass 4
            ComputeDropCandidate(a_view, a_viewIdx, base);// pass 5
        }
    }

    void SetDefResolver(DefResolver a_resolver)
    {
        g_resolver = std::move(a_resolver);
    }

    void SetUniqueOverrides(std::unordered_map<RE::FormID, bool> a_map)
    {
        g_uniqueOverride = std::move(a_map);
        g_uniqueLoaded = true;
        // ★The verdict is cached per form, so a reload has to drop the cache or
        // the file would only take effect on a restart -- and the file is
        // hot-reloaded on every inventory open, which is the whole point.
        InvalidateUniqueCache();
    }

    GridDef ResolveDef(RE::TESBoundObject* a_obj)
    {
        return (g_resolver && a_obj) ? g_resolver(a_obj) : GridDef{};
    }

    void SetGameCallbacks(std::function<void(RE::TESBoundObject*, bool)> a_sound,
                          std::function<void(RE::TESBoundObject*, int,
                                             RE::ExtraDataList*)> a_dropToWorld)
    {
        g_sound = std::move(a_sound);
        g_dropWorld = std::move(a_dropToWorld);
    }

    bool IsHolding()
    {
        return g_held.has_value();
    }

    bool HeldCanRotate()
    {
        return g_held && CanRotate(ResolveDef(g_held->obj));
    }

    HoverPrompt HoveredPrompt()
    {
        if (g_hoverPrompt.frame != ImGui::GetFrameCount()) return {};
        return { true, g_hoverPrompt.canSplit, g_hoverPrompt.canCompare,
                 true, g_hoverPrompt.canDrop, g_hoverPrompt.canFav,
                 g_hoverPrompt.hasVerb, g_hoverPrompt.verb,
                 g_hoverPrompt.canRecharge,
                 g_hoverPrompt.canShelfUse, g_hoverPrompt.useVerb };
    }

    bool IsPouchOpen() { return g_pouchOpen; }

    // GI17: "is THIS unit the one on the cursor". The form-level question was
    // never enough once a partner stack became several cells: lifting one of
    // three identical daggers hid every cell of that form, which reads exactly
    // like picking up all three at once.
    // GI18: the content signature of the carried unit, resolved from whichever
    // side actually owns it right now. 0 = nothing held, or a plain unit.
    std::uint16_t HeldInstanceSig()
    {
        // ★★★THE CARRY ALREADY KNOWS. This re-derived the signature by POSITION
        // -- ExtraForTile(entry, uid, xlIdx) -- and ExtraForTile's own comment
        // says a position is a hint, not a promise, and to prefer the resolver
        // that takes a signature wherever the pool is known. Here the pool is
        // not merely known, it is the field right next to this one.
        //
        // It failed outright for a doll carry, which is the one case that
        // matters: BeginCarry parks xlIdx at -1 on purpose (see the long note
        // there -- an unequip GUARANTEES a list is removed, so an index written
        // now names somebody else by the time it is read), and a unit with no
        // ExtraUniqueID has uid 0 too. Both resolvers then return null and this
        // returned 0, minting the destination cell as a PLAIN pool. A tempered
        // sword lifted off the body and dropped into a chest bound to nothing
        // and sat down wherever first-fit put it.
        //
        // The store path beside it was already passing a_held.sig to OpenSlider
        // and RequestStore while these callers went through here, so the same
        // move described the same unit two different ways.
        return g_held ? g_held->sig : 0;
    }

    bool IsHeldPartnerUnit(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                           int a_xlIdx, int a_ord)
    {
        if (!g_held || !g_held->fromPartner || g_held->obj != a_obj) return false;
        return g_held->uid == a_uid && g_held->xlIdx == a_xlIdx &&
               g_held->partnerOrd == a_ord;
    }

    RE::TESBoundObject* HeldPartnerObject()
    {
        return (g_held && g_held->fromPartner) ? g_held->obj : nullptr;
    }

    bool HeldFootprint(int& a_w, int& a_h, float& a_offX, float& a_offY)
    {
        if (!g_held) return false;
        a_w = g_held->mask.w;
        a_h = g_held->mask.h;
        a_offX = g_held->offX;
        a_offY = g_held->offY;
        return true;
    }

    namespace
    {
        // ★Ring session: the cursor's exits, by ORIGIN (user spec). A carry
        // cancelled (ESC, menu close) or refused at a drop returns to the
        // space it was lifted FROM -- the doll slot, the second-ring carrier
        // -- not to the grid, which never owned it. Returns false when the
        // origin IS the grid (or is gone: a displaced occupant's slot is
        // taken); the caller redraws and the saved layout entry does the rest.
        //
        // The re-wear rides the same pending pipeline as every equip (queue ->
        // Tick -> router/conflict pass/srcList), and NotePendingEquip bridges
        // the frames until the engine's confirm -- the same bridge a drop uses.
        bool ReturnCarryToOrigin(const Held& a_h)
        {
            if (!a_h.obj || a_h.swappedOut) return false;
            if (a_h.fromCarrier) {
                auto* armo = a_h.obj->As<RE::TESObjectARMO>();
                if (!armo || DualRing::Second()) return false;
                // the lift queued a carrier stand-down; it must not fire after
                // the re-wear and strip the ring we just put back
                DualRing::CancelTakeOff();
                NotePendingEquip(a_h.obj, a_h.uid, a_h.sig, 0, a_h.key, 1,
                                 a_h.xlIdx);
                Equip::RequestWear(a_h.obj, a_h.uid, a_h.sig, 0, a_h.count,
                                   /*a_second=*/true);
                return true;
            }
            if (a_h.fromDoll) {
                NotePendingEquip(a_h.obj, a_h.uid, a_h.sig, a_h.hand, a_h.key,
                                 Equip::EquipCountFor(a_h.obj, a_h.count),
                                 a_h.xlIdx);
                Equip::RequestWear(a_h.obj, a_h.uid, a_h.sig, a_h.hand,
                                   a_h.count, /*a_second=*/false);
                return true;
            }
            return false;
        }
    }

    void CancelHold()
    {
        if (!g_held) return;
        const Held h = *g_held;
        g_held.reset();
        if (ReturnCarryToOrigin(h)) {
            // The board never owned this carry: nothing moved on the grid,
            // so nothing is redrawn here. The pending bridge covers the
            // frames until the engine confirms the re-wear.
            SKSE::log::info("[ACT] carry cancel -> origin slot '{}'",
                h.obj ? h.obj->GetName() : "?");
            DiscardStash("doll-origin cancel");   // a board tile it never was
            return;
        }
        // ★S1: a whole-tile board carry goes home as ONE tile -- its layout
        // entry was never erased and its Item rides the stash. Everything the
        // stash cannot prove (a fragment, a partner carry, a lift whose
        // detach never ran, a spot that stopped existing) falls through to
        // the rebuild, which re-derives exactly as before.
        if (!h.preSplit && !h.fromPartner && UnstashTileHome()) return;
        Rebuild();   // the item resumes its saved spot (pickup never erased it)
    }

    void BeginCarry(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                    int a_hand, bool a_swappedOut, int a_count,
                    bool a_fromCarrier)
    {
        if (!a_obj || g_held) return;
        const GridDef def = g_resolver ? g_resolver(a_obj) : GridDef{};
        Mask m = MaskOf(def);
        const float ox = m.w * CellPx() * 0.5f;
        const float oy = m.h * CellPx() * 0.5f;
        // The key must name the unit's POOL, not just its form. Keyed to the
        // bare FormKey (the PLAIN pool), a tempered weapon picked off the doll
        // made the enumeration hold back a PLAIN unit instead: the tempered one
        // dropped back onto its old cell and the plain one rode the cursor.
        // With the pool prefix a doll carry behaves exactly like a board carry --
        // the unit's own slot is reserved for it and cancel restores it there.
        // ...but it must not COLLIDE with a sibling that is already on the board.
        // The worn unit had no tile (worn units are not enumerated), so it needs
        // a key of its own; taking the bare pool prefix stole the key of a spare
        // sitting in that same pool, whose remembered cell was then excluded as
        // "the carried tile" and lost -- the spare jumped to the first gap and a
        // cancel could not put it back. Take the lowest ordinal nobody holds.
        // GI31: if this pool has a PARKED star -- the entry left behind when the
        // favourite was equipped -- the unit coming off the doll IS that one, so
        // carry its key. Taking a fresh key instead left the parked entry stranded
        // (cell-less, starred, nothing worn) and the tile that landed had no star.
        std::string carryKey;
        {
        }
        {
            // ★The last place a pool label was still becoming a SLOT KEY.
            // Named after the form, like every other tile since 1.3.2 -- what
            // this carry is stays in the fields below, where it can change
            // without renaming anything.
            // ★★P2/3-1: A BAG LIFTED OFF THE DOLL KEEPS ITS OWN TILE'S KEY.
            //
            // Everything below mints a key that deliberately AVOIDS the ones in
            // use, because a worn unit historically had no tile of its own and
            // taking one would steal a spare's. A worn bag broke that premise:
            // it does have a tile, its contents point at that tile's key, and a
            // fresh key orphaned every one of them -- so lifting the bag off
            // the doll closed it and spilled the contents onto the main grid
            // (user report, and the exact moment named). Adopting the key keeps
            // the carry, the contents and the cancel all talking about the same
            // bag.
            if (IsBagForm(a_obj)) {
                for (const auto& t : g_items) {
                    if (t.obj == a_obj && t.def.bag != 0 && t.inBag != kTrashKey) {
                        carryKey = t.key;
                        break;
                    }
                }
            }
            if (carryKey.empty()) {
                carryKey = FormKey(a_obj);
                for (int n = 1; g_layout.contains(carryKey); ++n) {
                    carryKey = FormKey(a_obj) + "#" + std::to_string(n);
                }
            }
        }
        g_held = Held{ std::move(carryKey), a_obj, std::move(m),
                       (std::max)(1, a_count), def.bag != 0, def.scale, ox, oy, true };
        g_held->fromDoll = true;   // still worn until the unequip lands
        g_held->hand = a_hand;
        g_held->uid = a_uid;   // GI25: the doll hands over the WORN sub-stack,
        g_held->sig = a_sig;   // which is usually the tempered/enchanted copy
        // ★No tile to copy the markers off here, so ask the engine ONCE, at
        // lift -- a carry is a click, not a frame.
        if (auto* pDoll = RE::PlayerCharacter::GetSingleton()) {
            auto* de = LiveEntry(pDoll, a_obj);
            g_held->stolen = PoolIsStolen(de, a_uid, a_sig);
            g_held->quest  = PoolIsQuest(de, a_uid, a_sig);
            // ★★★AND NO LIST POSITION -- DELIBERATELY.
            //
            // Recording the worn list's index here looked like the obvious fix
            // ("a position cannot drift the way a signature can") and it was
            // wrong for exactly this carry. A position is stable against VALUES
            // moving inside a list; it is not stable against lists being ADDED
            // OR REMOVED, and lifting off the doll GUARANTEES a removal -- that
            // is what unequipping is. A plain unit's worn list holds nothing but
            // ExtraWorn, so the engine drops it entirely and every later index
            // slides down by one; the number we wrote down then names a
            // different unit, and the ladder confidently removes the wrong one.
            //
            // A doll carry does not need it. While it is still on the body the
            // hand identifies it exactly (one item per hand); once the unequip
            // lands it is either back in a list, where its signature matches, or
            // listless, where the plain pool is the only place it can be.
            g_held->xlIdx = -1;
        }
        g_held->swappedOut = a_swappedOut;
        // (B4-4: swapSameForm retired -- the doffing clock replaced its one
        // reader, the pendingEquip same-form-swap scan)
        g_held->fromCarrier = a_fromCarrier;
        // B4-2c: a doll lift IS an unequip request -- tell the worn ledger at
        // the same moment the carry begins, so Doffing() can answer the
        // "still on the body?" question from the request/event lifecycle
        // instead of scanning the equip queue. A carrier lift was never
        // engine-worn; there is nothing to doff.
        if (!a_fromCarrier) {
            WornLedger::NoteDoffing(a_obj->GetFormID(), a_hand);
        }
        if (g_poolTrace) {
            SKSE::log::info("[ACT] lift-from-doll '{}' hand={} uid {:04X} sig {:04X} key '{}'",
                a_obj->GetName(), a_hand, a_uid, a_sig, g_held->key);
        }
        if (g_sound) g_sound(a_obj, true);
        // ★B4-4: a CARRIER lift changes nothing the board draws -- the unit
        // was never on it (carrier-worn, ring2-excluded) and its exclusion
        // hands off to `held` without a gap. The redraw here was one of the
        // frames painted mid-handoff in the ring swap window (the deferred
        // blink's habitat). An engine-worn lift keeps its redraw: the
        // parked-star bookkeeping (GI30/31) still draws through it.
        // ★Ring session: a SWAP-DISPLACED lift changes nothing either -- the
        // occupant was worn (no tile) and goes straight to the cursor, and
        // this redraw fired in the middle of every drop-swap ([RB] measured
        // it as half the 11/s storm). Only the plain doll lift keeps its
        // redraw, and no engine churn races that one.
        if (!a_fromCarrier && !a_swappedOut) RequestRebuild();
    }

    void BeginPartnerCarry(RE::TESBoundObject* a_obj, int a_count, int a_value,
                           float a_offX, float a_offY,
                           std::uint16_t a_uid, int a_xlIdx, int a_ord, int a_rot)
    {
        if (!a_obj || g_held) return;
        const GridDef def = g_resolver ? g_resolver(a_obj) : GridDef{};
        // GI62: lift it as it lies on the other side, so a sword stored on its
        // side comes back across still on its side.
        const int rot = CanRotate(def) ? (a_rot & 3) : 0;
        Mask m = MaskOf(def, rot);
        Held h;
        h.key = "##partner";   // not a real grid tile
        h.obj = a_obj;
        h.SetRot(rot);
        h.mask = std::move(m);
        h.count = a_count;
        h.isBag = def.bag != 0;
        h.defScale = def.scale;
        // F7: pick up where clicked (player-tile parity); centre fallback
        // for swap pickups (no meaningful click point)
        HoldByPivot(h, def);   // GI62d: gripped by the pivot cell, like a tile
        if (a_offX >= 0.0f) h.offX = a_offX;   // explicit grab point (legacy path)
        if (a_offY >= 0.0f) h.offY = a_offY;
        h.justPicked = true;
        h.fromPartner = true;
        h.partnerValue = a_value;
        h.uid = a_uid;       // D4: which sub-stack was picked up
        h.xlIdx = a_xlIdx;
        h.partnerOrd = a_ord;   // GI19: and which cell, for plain look-alikes
        if (auto* p = LootBarter::Partner()) {   // GI25: signature for the transfer
            h.sig = InstanceSig(ExtraForTile(LiveEntry(p, a_obj), a_uid, a_xlIdx));
        }
        g_held = std::move(h);
        if (g_sound) g_sound(a_obj, true);
    }

    void OnRequestExpired(std::uint32_t a_form, std::int32_t a_delta, const char* a_who,
                          const std::string& a_slot)
    {
        // ★The cell comes back FIRST -- a rebuild before this would re-mint the
        // slot and first-fit it into the front gap, which is the very bug.
        if (a_delta < 0) CancelSlotDrop(a_form, a_slot);
        // ★S2: the undo B3-b promised. The kept slot gets its unit re-drawn,
        // ONE tile wide -- no other tile moves (원칙 3: the refusal is
        // confined to the cell it named). Anything unproven still re-derives.
        const auto* form = RE::TESForm::LookupByID(a_form);
        if (a_delta < 0 && ReEmitTileAt(a_form, a_slot)) {
            SKSE::log::warn("[GRID] '{}' {:+d} ({}) was never confirmed -- its "
                            "tile is back, nothing else moved",
                form && form->GetName() ? form->GetName() : "?", a_delta, a_who);
            return;
        }
        SKSE::log::warn("[GRID] '{}' {:+d} ({}) was never confirmed -- rebuilding "
                        "to recover",
            form && form->GetName() ? form->GetName() : "?", a_delta, a_who);
        RequestRebuild();
    }

    bool RebuildTrace() { return g_rbTrace.load(std::memory_order_relaxed); }

    void SetRebuildTrace(bool a_on)
    {
        g_rbTrace.store(a_on, std::memory_order_relaxed);
        if (a_on) SKSE::log::info("[RB] rebuild provenance ON (B3-c)");
    }

    void SetRebuildDrop(const char* a_csv)
    {
        std::lock_guard rb(g_rbMtx);
        g_rbDrop.clear();
        std::string s = a_csv ? a_csv : "";
        std::size_t at = 0;
        while (at <= s.size()) {
            const auto comma = s.find(',', at);
            std::string one = s.substr(at, comma == std::string::npos
                                              ? std::string::npos : comma - at);
            // trim
            const auto b = one.find_first_not_of(" \t");
            const auto e2 = one.find_last_not_of(" \t");
            if (b != std::string::npos) g_rbDrop.push_back(one.substr(b, e2 - b + 1));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        for (const auto& d : g_rbDrop) {
            SKSE::log::warn("[RB] ★!rbdrop -- rebuild requests from '{}' will be "
                            "IGNORED", d);
        }
    }

    void RequestRebuild(const std::source_location& a_where)
    {
        // ★A rebuild request is COALESCED -- many asks, one run -- so the
        // interesting number is not "who asked" but "who was asking when it
        // finally ran". Both are recorded; the run drains the list.
        // ★Runs on event threads too: the site containers live under g_rbMtx
        // and the flag write below is atomic, so no marshalling is needed.
        const bool trace = g_rbTrace.load(std::memory_order_relaxed);
        {
            std::lock_guard rb(g_rbMtx);
            const bool needSite = trace || !g_rbDrop.empty();
            const std::string site = needSite ? ShortSite(a_where) : std::string{};
            for (const auto& d : g_rbDrop) {
                if (site == d) {
                    if (trace) SKSE::log::info("[RB] dropped ask from {}", site);
                    return;   // the experiment: pretend nobody asked
                }
            }
            if (trace) ++g_rbAsk[site];
        }
        g_needRebuild.store(true, std::memory_order_release);
    }

    // Called by Rebuild(), which is where an ask actually becomes work.
    void NoteRebuildRan(const std::source_location& a_direct)
    {
        if (!g_rbTrace.load(std::memory_order_relaxed)) return;
        ++g_rbRuns;

        std::string who;
        {
            std::lock_guard rb(g_rbMtx);
            for (const auto& [site, n] : g_rbAsk) {
                if (!who.empty()) who += ", ";
                who += site;
                if (n > 1) who += "x" + std::to_string(n);
            }
            g_rbAsk.clear();
        }
        // ★No pending ask means somebody called Rebuild() outright. That is a
        // legitimate thing to do -- it just has to be visible, because B3 has to
        // account for every path that rebuilds the board.
        if (who.empty()) who = "DIRECT " + ShortSite(a_direct);
        SKSE::log::info("[RB] run #{} <- {}", g_rbRuns, who);

        // A rate line once a second: the oscillation in §4-2 is a RATE problem,
        // and a per-run line alone does not show it.
        const auto now = std::chrono::steady_clock::now();
        if (g_rbWindow.time_since_epoch().count() == 0) g_rbWindow = now;
        if (now - g_rbWindow >= std::chrono::seconds{ 1 }) {
            SKSE::log::info("[RB] ---- {} rebuild(s) in the last second ----", g_rbRuns);
            g_rbRuns = 0;
            g_rbWindow = now;
        }
    }

    void RefreshDefs()
    {
        auto* cache = IconCache::GetSingleton();
        for (auto& it : g_items) {
            it.def = g_resolver ? g_resolver(it.obj) : GridDef{};
            // mask untouched: this path is for orientation-only edits
            cache->QueueCapture(it.obj);   // no-op when the key is cached
        }
    }

    // GI30: is this pool's favourite currently ON THE BODY? A parked star -- a
    // starred layout entry with no cell -- is exactly that: the unit it belongs
    // to left the board to be worn. The doll draws the mark while it is away, so
    // the star never appears to vanish.
    bool IsPoolStarWorn(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                        std::uint16_t a_sig)
    {
        // GI40: ask the POOL, exactly like the grid tiles do. The worn list is
        // one member of it -- whether the engine happens to be keeping the
        // hotkey there or on a spare's list is an implementation detail that
        // must not decide whether the doll shows a star.
        auto* p = RE::PlayerCharacter::GetSingleton();
        return PoolHasStar(LiveEntryOf(p, a_obj), a_uid, a_sig);
    }

    // ★1.0.5: the doll needs this for the same reason it needs the star —
    // wearing a stolen item must not make it stop looking stolen. Keyed the way
    // the grid keys it (pool prefix), so the tile and the slot agree.
    bool IsPoolStolen(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                      std::uint16_t a_sig)
    {
        if (!a_obj) return false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        return PoolIsStolen(LiveEntry(player, a_obj), a_uid, a_sig);
    }

    void ForgetTile(const std::string& a_key)
    {
        // Rule 13: leaving the board forgets the cell, and EQUIPPING is leaving
        // the board -- same as selling, storing or dropping. Instance tiles used
        // to be kept so an unequip landed back in its own cell, but that made
        // equipping the one exception to an otherwise uniform rule, and with two
        // units of one form the returning copy could claim the other's cell.
        //
        // The key names the tile the player actually acted on, which is what the
        // pool model requires: the slot that empties is the one they clicked, not
        // an arbitrary sibling. In-memory; the cosave persists on game save.
        if (a_key.empty()) return;
        g_layout.erase(a_key);
    }

    void NotifySlotDropTarget(const std::string& a_slotId)
    {
        g_slotTarget = a_slotId;
    }

    // ---- Phase 2: ONE inventory walk for display + capacity ----
    namespace
    {
        // skip rules shared by EVERY inventory walk. The four hand-copied
        // filters drifted apart once already (a missed GetPlayable made
        // hidden scripting copies overload the board) — one predicate now.
        bool SkipInventoryEntry(RE::TESBoundObject* a_obj, int a_count)
        {
            if (!a_obj || a_count <= 0) return true;
            // L1: hold back only the RESERVED UNITS, not the whole entry —
            // the surplus (looted, bought, crafted) stays on the board.
            if (a_count <= Loadout::ReservedCount(a_obj->GetFormID())) return true;
            // coins may be deliberately UNNAMED (ESP) so loot-HUD widgets skip
            // their mirror adds — the grid shows them regardless (value badge)
            const char* name = a_obj->GetName();
            if ((!name || !*name) && !GoldCoins::IsCoinForm(a_obj->GetFormID())) return true;
            // vanilla parity: NON-PLAYABLE forms never show in the vanilla UI
            // (mods keep hidden scripting copies in the inventory)
            if (!a_obj->GetPlayable()) return true;
            return false;
        }

        // ---- GI1/GI2: the ONE enumeration of a cap-1 entry into tile identities ----
        // The display collector and the capacity sims MUST agree about which
        // keys exist, or the sim looks up saved spots under keys the board never
        // creates and models a different board than the one on screen.
        //
        // An entry's units are not interchangeable: some carry an ExtraDataList
        // (enchanted / tempered / poisoned / renamed / worn), some don't, and one
        // list can stand for several units (GetCount). Two traps:
        //   * enumerate only the lists -> plain units vanish
        //     (3 swords, 1 enchanted -> 1 list -> 2 tiles lost)
        //   * enumerate only the count -> tiles are bare ordinals again, so
        //     equipping one shifts every later tile onto its neighbour's saved
        //     spot AND its neighbour's per-instance data
        // Hence: plain = count - SUM(list.GetCount()).
        //
        // a_units is the caller's already-adjusted board total (worn copy and
        // pending removals subtracted); the surplus is drained from the plain
        // pool first so a sold unit never orphans an instance-keyed tile.
        struct UnitTile
        {
            std::string   key;
            int           xlIdx = -1;   // index in entry->extraLists, -1 = plain
            // ★Handed over explicitly rather than decoded back out of `key`.
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
        };

        // a_skipWorn=false keeps the body-worn unit in the walk: corpses and
        // pickpocket targets show what the NPC is wearing.
        // a_base non-empty: also drop the units that are NOT ON THE BOARD -- the one
        // riding the cursor and any whose equip the engine has not applied yet. Doing
        // it HERE, once, is the whole point: the board's input set is correct by
        // construction and every downstream pass just renders what it is handed.
        // Removal is best-effort by design: while a lifted unit is still WORN it is
        // already absent (skipWorn), and asking to remove it again must be a no-op,
        // not an over-subtraction.
        // ★★★THE ONE POOL WALK. Two of these existed -- the board's and the
        // capacity sim's -- with the same skeleton, different container types
        // and two different spellings of "is this the plain pool"
        // (`xuid == 0 && sg == 0` against `PoolPrefix == base`, which agreed only
        // by luck. They HAVE to produce the same keys: the sim decides whether a
        // pickup fits and the board decides where it goes, so a disagreement is
        // a bounce against a tile that is on screen.
        //
        // Returns SIGNED pools only, already netted against the removals queued
        // out of them; `a_signedOut` is what to take off the form-wide total
        // before the plain pool claims the remainder. `a_countTrashed` is for the
        // sim, where parked units have been subtracted form-wide and parking
        // leaves no trace on an extraList -- the layout is the only witness.
        [[nodiscard]] std::map<std::string, int> SignedPools(
            RE::InventoryEntryData* a_entry, const std::string& a_base,
            int& a_signedOut, bool a_countTrashed)
        {
            std::map<std::string, int> pools;
            a_signedOut = 0;
            if (a_entry && a_entry->extraLists) {
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl) continue;
                    // worn units are off the board entirely, and the caller's
                    // total has already taken them out
                    if (xl->HasType<RE::ExtraWorn>() ||
                        xl->HasType<RE::ExtraWornLeft>()) {
                        continue;
                    }
                    std::uint16_t xuid = 0;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                        xuid = xu->uniqueID;
                    }
                    const std::string p = PoolPrefix(a_base, xuid, InstanceSig(xl));
                    if (p == a_base) continue;   // plain: the caller's remainder
                    const int n = (std::max)(1, xl->GetCount());
                    pools[p] += n;
                    a_signedOut += n;
                }
            }
            // B4-3c: the queued removals, per pool, from the ledger -- the
            // entry's own uid/sig name the pool the way the slot key used to
            std::map<std::string, int> removing;
            if (a_entry && a_entry->object) {
                for (const auto& e :
                     Ledger::OpenOutgoingOf(a_entry->object->GetFormID())) {
                    removing[PoolPrefix(a_base, e.uid, e.sig)] += -e.delta;
                }
            }
            for (auto& [p, n] : pools) {
                if (const auto pi = removing.find(p); pi != removing.end()) {
                    const int gone = (std::min)(n, pi->second);
                    n -= gone;
                    a_signedOut -= gone;   // ...so the plain remainder gets it back
                }
                if (!a_countTrashed) continue;
                int parked = 0;
                for (const auto& [k, le] : g_layout) {
                    if (le.bag == kTrashKey && BaseKey(k) == a_base &&
                        PoolOfSlot(k) == p) {
                        parked += (std::max)(1, le.count);
                    }
                }
                const int binned = (std::min)(n, parked);
                n -= binned;
                a_signedOut -= binned;
            }
            return pools;
        }

        void EnumerateUnitRefs(int a_count, int a_units, RE::InventoryEntryData* a_entry,
                               std::vector<UnitRef>& a_out, bool a_skipWorn = true,
                               const std::string& a_base = {})
        {
            struct Inst { std::uint16_t uid; std::uint16_t sig; int units; int xlIdx;
                          int hand = 0; bool worn = false; };
            std::vector<Inst> insts;
            std::vector<Inst> wornUnits;   // skipped above; never part of the board
            int listed = 0;
            // ★★P2/3-1: A WORN BAG STAYS ON THE BOARD, and it is the only unit
            // that may be in two places at once.
            //
            // Wearing a backpack used to make its tile vanish -- worn units are
            // not enumerated -- and the rebuild then read the missing tile as
            // "that bag is gone", which spills the contents onto the main grid
            // (user report (7)). The bag was still on your back and still held
            // nothing. Report (15), "a bag-flagged backpack cannot be equipped",
            // is the same event described from the other side: nothing blocks
            // the equip, it just stops behaving like a bag the moment it lands.
            //
            // A container you are WEARING is exactly the container you most
            // expect to keep using, so the tile stays: same cells, same
            // subgrid, plus a worn marker in the tray. The doll shows it too,
            // which is the exception this comment exists to justify.
            const bool wornBagStays =
                a_entry && a_entry->object && g_resolver &&
                g_resolver(a_entry->object).bag != 0;
            if (a_entry && a_entry->extraLists) {
                int xi = 0;
                for (auto* xl : *a_entry->extraLists) {
                    const int idx = xi++;
                    if (!xl) continue;
                    const int n = (std::max)(1, xl->GetCount());
                    listed += n;
                    if (a_skipWorn && !wornBagStays &&
                        (xl->HasType<RE::ExtraWorn>() ||
                         xl->HasType<RE::ExtraWornLeft>())) {
                        // remember WHICH units these were: the off-board pass below
                        // has to tell "this unit never entered the set" from "this
                        // unit is on the board and must come out".
                        std::uint16_t wuid = 0;
                        if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                            wuid = xu->uniqueID;
                        }
                        wornUnits.push_back({ wuid, InstanceSig(xl), n, idx,
                                              xl->HasType<RE::ExtraWornLeft>() ? 2 : 1 });
                        continue;
                    }
                    std::uint16_t uid = 0;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
                    // GI41: only reachable with a_skipWorn=false (partner boards
                    // show what the NPC wears). Record it HERE, where it is a
                    // fact, instead of leaving the consumer to guess by position.
                    const bool wornHere = xl->HasType<RE::ExtraWorn>() ||
                                          xl->HasType<RE::ExtraWornLeft>();
                    insts.push_back({ uid, InstanceSig(xl), n, idx,
                                      wornHere ? (xl->HasType<RE::ExtraWornLeft>() ? 2 : 1) : 0,
                                      wornHere });
                }
            }
            int plain = a_count - listed;
            if (plain < 0) {
                SKSE::log::warn("[GRID] GI2: count {} < listed {} — clamped", a_count, listed);
                plain = 0;
            }
            int have = plain;
            for (const auto& in : insts) have += in.units;
            for (int drop = have - a_units; drop > 0;) {
                if (plain > 0) { const int t = (std::min)(plain, drop); plain -= t; drop -= t; continue; }
                if (insts.empty()) break;
                const int t = (std::min)(insts.back().units, drop);
                insts.back().units -= t;
                drop -= t;
                if (insts.back().units <= 0) insts.pop_back();
            }
            // Which of the hashed extras each list actually carries. The signature
            // was observed to CHANGE when a unit is equipped, which breaks every
            // worn-boundary match; this names the extra responsible.
            if (g_poolTrace && a_entry && a_entry->extraLists && !a_base.empty()) {
                int xi2 = 0;
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl) { ++xi2; continue; }
                    std::string f;
                    if (xl->HasType<RE::ExtraHealth>())          f += "HEALTH ";
                    if (xl->HasType<RE::ExtraEnchantment>())     f += "ENCH ";
                    if (xl->HasType<RE::ExtraCharge>())          f += "CHARGE ";
                    if (xl->HasType<RE::ExtraPoison>())          f += "POISON ";
                    if (xl->HasType<RE::ExtraSoul>())            f += "SOUL ";
                    if (xl->HasType<RE::ExtraTextDisplayData>()) f += "NAME ";
                    // ★the star's seat, per list -- the vanishing-favorite
                    // report (star gone after charge drain) needs to see
                    // whether the engine's ExtraCharge writeback ever drops
                    // the ExtraHotkey riding the same worn list
                    if (xl->HasType<RE::ExtraHotkey>())          f += "HOT ";
                    if (xl->HasType<RE::ExtraWorn>())            f += "|worn ";
                    if (xl->HasType<RE::ExtraWornLeft>())        f += "|wornL ";
                    SKSE::log::info("[XL] {} [{}] sig {:04X} count {} : {}",
                        a_base, xi2, InstanceSig(xl), xl->GetCount(),
                        f.empty() ? "(none)" : f);
                    ++xi2;
                }
            }

            if (!a_base.empty()) {
                auto takeOne = [&](std::uint16_t uid, std::uint16_t sig, bool mayBeWorn,
                                   const char* why, int hand, int xlIdx) {
                    const auto trace = [&](const char* from) {
                        if (g_poolTrace) {
                            SKSE::log::info("[TAKE] {} uid {:04X} sig {:04X} -> {} "
                                            "(worn {}, insts {}, plain {})",
                                why, uid, sig, from, wornUnits.size(), insts.size(), plain);
                        }
                    };
                    // A unit that is STILL WORN was never in the set: `listed`
                    // already accounted for it, so `plain` excludes it too. Asking
                    // to remove it again used to fall through to `--plain` and take
                    // an innocent SPARE instead -- lifting a plain equipped sword
                    // off the doll made every plain copy vanish until the unequip
                    // landed. Consume the worn entry instead and do nothing.
                    if (mayBeWorn) {
                        for (auto it = wornUnits.begin(); it != wornUnits.end(); ++it) {
                            if (it->uid != uid || it->sig != sig) continue;
                            if (hand != 0 && it->hand != hand) continue;   // other hand
                            if (--it->units <= 0) wornUnits.erase(it);
                            trace("worn (already off the board)");
                            return;
                        }
                        // ★★THE HAND IS AN EXACT IDENTIFIER, not a guess: you can
                        // only wear one thing in one hand. So a carry lifted OFF
                        // THE DOLL, in the frames before the unequip lands, IS
                        // the worn unit in that hand -- whatever its signature
                        // has done since, whether the engine ever gave it a
                        // uniqueID, wherever its list has moved to.
                        //
                        // Only for "held". An "equipping" record is heading ONTO
                        // the body and has not arrived; consuming the worn unit
                        // in its hand would take the one it is REPLACING.
                        if (hand != 0 && why && std::strcmp(why, "held") == 0) {
                            for (auto it = wornUnits.begin(); it != wornUnits.end(); ++it) {
                                if (it->hand != hand) continue;
                                if (--it->units <= 0) wornUnits.erase(it);
                                trace("worn, by hand (the doll carry)");
                                return;
                            }
                        }
                    }
                    for (auto it = insts.begin(); it != insts.end(); ++it) {
                        if (it->uid != uid || it->sig != sig) continue;
                        if (--it->units <= 0) insts.erase(it);
                        trace("REMOVED from insts");
                        return;
                    }
                    // *THE LIST POSITION, for every off-board unit that knows
                    // one -- a carry, a queued sale or store, a trash park.
                    // The signature hashes the unit's numbers and any of them
                    // can move while the unit is in flight; where the list
                    // sits in the entry cannot. Asked BEFORE the plain pool
                    // so a listed unit is never mistaken for a listless one.
                    // ★★A PLAIN RECORD HAS NO LIST POSITION. uid 0 and sig 0 mean
                    // "nothing distinguishes this unit", which is the definition
                    // of listless -- so any position recorded for it is a
                    // leftover from a moment when it DID have a list (a worn unit
                    // carries one holding only ExtraWorn). The instant that list
                    // goes away, every later index slides down and the recorded
                    // number names somebody else. Measured: a plain dagger lifted
                    // off the doll recorded index 0; the unequip dropped its
                    // ExtraWorn-only list; index 0 then meant the POISONED
                    // dagger, which was duly removed from the board -- and its
                    // cell, still remembered, was handed a plain unit instead.
                    // The tile did not move; its contents changed underneath it.
                    //
                    // Such a record is answered correctly one tier down: still in
                    // a list -> the strict pass above; genuinely listless -> the
                    // plain pool below.
                    if (xlIdx >= 0 && !(uid == 0 && sig == 0)) {
                        for (auto it = insts.begin(); it != insts.end(); ++it) {
                            if (it->xlIdx != xlIdx) continue;
                            if (--it->units <= 0) insts.erase(it);
                            trace("REMOVED by list position");
                            return;
                        }
                        // ...and a WORN unit that moved: same reasoning as the
                        // strict worn pass above -- it was never in the set.
                        if (mayBeWorn) {
                            for (auto it = wornUnits.begin(); it != wornUnits.end(); ++it) {
                                if (it->xlIdx != xlIdx) continue;
                                if (hand != 0 && it->hand != hand) continue;
                                if (--it->units <= 0) wornUnits.erase(it);
                                trace("worn, by list position");
                                return;
                            }
                        }
                    }
                    if (uid == 0 && sig == 0 && plain > 0) {
                        --plain;
                        trace("REMOVED from plain");
                        return;
                    }
                    // ★SAME POOL IS ENOUGH, for every off-board record.
                    //
                    // A pool is the set of units that are interchangeable by
                    // construction, so taking "one of this pool" is not a guess
                    // -- it is the only answer the model has. The strict test
                    // above also demands the uniqueID, and some records simply
                    // do not carry one: an inactive loadout reserves its gear by
                    // SIGNATURE alone, so the moment the engine gave that unit a
                    // uniqueID the reservation stopped matching and the preset's
                    // held-back armour drew on the board as well.
                    if (sig != 0) {
                        for (auto it = insts.begin(); it != insts.end(); ++it) {
                            if (it->sig != sig) continue;
                            if (--it->units <= 0) insts.erase(it);
                            trace("REMOVED by signature (same pool)");
                            return;
                        }
                        if (mayBeWorn) {
                            for (auto it = wornUnits.begin(); it != wornUnits.end(); ++it) {
                                if (it->sig != sig) continue;
                                if (hand != 0 && it->hand != hand) continue;
                                if (--it->units <= 0) wornUnits.erase(it);
                                trace("worn, by signature (same pool)");
                                return;
                            }
                        }
                    }
                    // ★★★A CARRIED UNIT COMES OFF THE BOARD, WHATEVER ITS
                    // SIGNATURE SAYS NOW.
                    //
                    // The signature hashes the unit's own numbers -- temper
                    // (ExtraHealth), charge (ExtraCharge), poison doses -- and
                    // any of them can move between the frame the item was
                    // picked up and the rebuild that follows: the engine splits
                    // a unit out of its stack to hand it over, a script tops a
                    // charge up, an ownership stamp lands. When it does, the
                    // exact (uid, sig) match above finds nothing and the old
                    // code did NOTHING -- so the unit sat on the cursor AND on
                    // the board at once. It looked like a duplicate, it blocked
                    // its own cell, and dropping it therefore fell through to
                    // the first free square. Reported as "weapons and armour
                    // cannot be moved, they jump to the front gap"; a plain
                    // item (no list, no signature) was never affected, which is
                    // exactly which items the reports named.
                    //
                    // For a HELD unit the count is not in question -- the player
                    // is holding it -- so only "which one" is, and inside a pool
                    // that has no answer by construction (units are placed in
                    // position order). Take the nearest identity we can prove,
                    // then any unit at all; an unmatched carry must never leave
                    // the board's total one too high.
                    // ★Only the carry. A queued removal or a trash-parked unit
                    // that fails to match is a different question -- guessing
                    // there would hide a unit the player still owns.
                    // ★A CARRY AND A TRASH PARK ARE BOTH CERTAIN: the player
                    // is holding one and we ourselves parked the other, so
                    // "it is not on the board" is a fact in both cases and
                    // only WHICH unit is open to question. A queued sale or
                    // store is NOT certain -- it may still fail -- so those
                    // keep the strict rule, where a wrong guess would hide
                    // something the player still owns.
                    const bool held = why &&
                        (std::strcmp(why, "held") == 0 ||
                         std::strcmp(why, "trash") == 0);
                    if (held) {
                        // *THE LIST POSITION FIRST. It is the one handle that
                        // cannot drift -- the signature hashes the unit's
                        // numbers, the uniqueID can be absent, but WHERE the
                        // list sits in the entry does not change when a value
                        // inside it does. Taking "any unit" instead was wrong
                        // in exactly the case that matters: with three daggers
                        // in the pack it removed one of the OTHERS, so the
                        // carried one stayed drawn (a duplicate) while an
                        // innocent sibling blinked out.
                        if (uid != 0) {   // same unique id, drifted signature
                            for (auto it = insts.begin(); it != insts.end(); ++it) {
                                if (it->uid != uid) continue;
                                if (--it->units <= 0) insts.erase(it);
                                trace("REMOVED by uid (signature drifted)");
                                return;
                            }
                        }
                        // A carry that had NO list is a plain unit, and the
                        // plain pool is where it must come from -- reaching
                        // into `insts` first would take a listed sibling and
                        // leave the carried one drawn.
                        // ★★...and ONLY a carry that genuinely had no list. The
                        // test used to be `xlIdx < 0` alone, which is also true
                        // of a carry that HAD one and simply never wrote the
                        // position down -- every lift off the doll, until the
                        // fix above. A record carrying a signature came out of a
                        // list by definition, so taking a listless unit for it
                        // is not a guess that might be wrong: it is provably the
                        // wrong unit. Fall through to "any listed unit" instead.
                        if (xlIdx < 0 && sig == 0 && plain > 0) {
                            --plain;
                            trace("REMOVED from plain (carry had no list)");
                            return;
                        }
                        // ★★★A DOLL CARRY THAT IS STILL ON THE BODY MUST NOT TAKE
                        // A UNIT OFF THE BOARD.
                        //
                        // If any worn unit of this form remains, the carry is one
                        // of them -- we simply cannot always say WHICH. A
                        // same-form swap makes two indistinguishable worn lists
                        // for a frame or two, which is exactly when every tier
                        // above comes up empty.
                        //
                        // The two possible mistakes are not equal. Guessing takes
                        // a SIBLING off the board, and the cell it was standing
                        // on is then unclaimed and erased -- permanent, and the
                        // weapon reappears somewhere the player never put it.
                        // Doing nothing leaves the carried unit counted for one
                        // rebuild, which draws a duplicate tile that the next
                        // rebuild clears by itself. Take the cheaper mistake.
                        // ★★...but ONLY while the carry may actually still be on
                        // the body. `mayBeWorn` is that clock; `wornUnits` is
                        // not, and using it was a real regression: after a
                        // same-form swap the worn list belongs to the item that
                        // just went ON, while the carry has already come off. The
                        // net then refused to take the carry off the board, so it
                        // hung on the cursor AND stood on the grid -- a duplicate
                        // that did NOT clear on the next rebuild, because nothing
                        // about the situation changes until the player lets go.
                        // (Measured: '#1' at [3,0] sig 4BF1 while 4BF1 rode the
                        // cursor.)
                        if (mayBeWorn && !wornUnits.empty()) {
                            trace("still on the body (no-op)");
                            return;
                        }
                        if (!insts.empty()) {
                            auto it = std::prev(insts.end());
                            if (--it->units <= 0) insts.erase(it);
                            trace("REMOVED as any listed unit (carry must leave)");
                            return;
                        }
                        if (plain > 0) {
                            --plain;
                            trace("REMOVED as any plain unit (carry must leave)");
                            return;
                        }
                    }
                    trace("no match (no-op)");
                };
                for (const auto& u : OffBoardUnitsFor(a_entry ? a_entry->object : nullptr,
                                                     a_base)) {
                    // ★★AN ARRIVING EQUIP IS NOT ON THE BODY YET. Its record says
                    // mayBeWorn because it is heading there, but until the engine
                    // has actually run it the only worn list of this form belongs
                    // to the unit it is REPLACING. Matching that one writes the
                    // incoming unit off as already worn, so it stays counted on
                    // the board for a frame and the outgoing one is hidden twice
                    // -- a tile blinking out and back during a swap.
                    //
                    // ★B4-2c: `landed`, not `applied`. The old clock flipped
                    // when OUR CALL returned, a frame or more before the
                    // engine actually wore anything; this one flips on the
                    // engine's own TESEquipEvent (NoteEquipLanded) -- the
                    // event IS the moment the old clock was guessing at.
                    // Identity still cannot answer it: with two identical
                    // weapons the two lists are indistinguishable.
                    const bool worn = u.mayBeWorn && !(u.arriving && !u.landed);
                    takeOne(u.uid, u.sig, worn, u.why, u.hand, u.xlIdx);
                }
            }

            // ORDER MATTERS: plain units come FIRST so their ordinals never
            // shift when a list appears or disappears. Emitting lists first was
            // what let a freshly tempered dagger take ordinal 0 and steal the
            // plain dagger's cell.
            for (int k = 0; k < plain; ++k) a_out.push_back({ 0, 0, -1 });
            for (const auto& in : insts) {
                for (int k = 0; k < in.units; ++k) {
                    a_out.push_back({ in.uid, in.sig, in.xlIdx, in.worn, in.hand });
                }
            }
        }

        // a_instanceKeys=false forces the historical ordinal keys. BAGS take that
        // path: a bag's tile key is referenced by every entry it holds
        // (LayoutEntry::bag), so re-keying one would orphan its contents. They
        // would recover -- E4 spills them back to main -- but silently moving a
        // player's bag contents to buy an instance binding no bag needs is a bad
        // trade.
        // a_mutate: only the display collector may drop a pool's surplus slots.
        // The capacity sims run the same walk read-only and must not touch
        // g_layout -- they only need the SAME keys the board will use.
        void EnumerateUnitTiles(const std::string& a_base, int a_count, int a_units,
                                RE::InventoryEntryData* a_entry,
                                std::vector<UnitTile>& a_out,
                                bool a_instanceKeys = true, bool a_mutate = false)
        {
            // Walk EVERY non-worn unit first: which ones to hide is a per-pool
            // question, and the walk cannot answer it (it has no keys).
            std::vector<UnitRef> refs;
            EnumerateUnitRefs(a_count, (std::numeric_limits<int>::max)(), a_entry, refs,
                              true, a_base);

            // group into pools, preserving order within each
            std::map<std::string, std::vector<UnitRef>> pools;
            for (const auto& r : refs) {
                pools[a_instanceKeys ? PoolPrefix(a_base, r.uid, r.sig) : a_base]
                    .push_back(r);
            }

            // GI22: hide the units that are on their way out -- from the pools
            // they actually left. Whatever the caller could not account for
            // (console removals) comes off the plain pool first, then the rest.
            // B4-3c: read from the ledger -- the entry's uid/sig name the pool
            // the way the slot key's prefix used to.
            int surplus = static_cast<int>(refs.size()) - a_units;
            std::map<std::string, int> hide;
            if (surplus > 0 && a_entry && a_entry->object) {
                std::map<std::string, int> removing;
                for (const auto& e :
                     Ledger::OpenOutgoingOf(a_entry->object->GetFormID())) {
                    removing[PoolPrefix(a_base, e.uid, e.sig)] += -e.delta;
                }
                for (auto& [prefix, members] : pools) {
                    if (surplus <= 0) break;
                    const auto pi = removing.find(prefix);
                    if (pi == removing.end()) continue;
                    const int t = (std::min)({ surplus, pi->second,
                                               static_cast<int>(members.size()) });
                    hide[prefix] += t;
                    surplus -= t;
                }
            }
            // L1/D4-b: an inactive preset holds ONE SPECIFIC unit. Take it from
            // the pool it actually belongs to, before the generic rule below --
            // which prefers the plain pool and so hid the plain dagger while the
            // preset was wearing the tempered one, swapping them on screen.
            if (surplus > 0 && a_instanceKeys && a_entry && a_entry->object) {
                for (const std::uint16_t rs :
                     Loadout::ReservedSigs(a_entry->object->GetFormID())) {
                    if (surplus <= 0) break;
                    const std::string prefix = PoolPrefix(a_base, 0, rs);
                    const auto pit = pools.find(prefix);
                    if (pit == pools.end()) continue;
                    if (static_cast<int>(pit->second.size()) - hide[prefix] <= 0) continue;
                    ++hide[prefix];
                    --surplus;
                }
            }

            if (surplus > 0) {   // leftovers: plain pool first
                for (int pass = 0; pass < 2 && surplus > 0; ++pass) {
                    for (auto& [prefix, members] : pools) {
                        if (surplus <= 0) break;
                        const bool plain = (prefix == a_base);
                        if ((pass == 0) != plain) continue;
                        const int room = static_cast<int>(members.size()) - hide[prefix];
                        const int t = (std::min)(surplus, (std::max)(0, room));
                        hide[prefix] += t;
                        surplus -= t;
                    }
                }
            }

            // Cells that belong to a unit IN TRANSIT. A unit that left the board
            // keeps its cell until the transition completes -- otherwise `want`
            // drops while the cell is still remembered, the survivors take
            // slots[0..want-1] in sort order, and the cell that ends up unused is
            // whichever sorted LAST rather than the one the player acted on. The
            // real cell then empties a frame or two later, when the applied equip
            // forgets it, so a sibling blinks off and back on. Holding the cell
            // makes the board show the right thing on the FIRST frame.
            std::set<std::string> inTransit;
            for (const auto& u : OffBoardUnitsFor(a_entry ? a_entry->object : nullptr,
                                                  a_base)) {
                if (!u.srcKey.empty()) inTransit.insert(u.srcKey);
            }

            // ---- the slots this FORM owns ---------------------------------
            // ONE list for the whole base. There is no per-pool slot list any
            // more, because a slot is no longer NAMED after the pool it happens
            // to be showing -- see LayoutEntry::sig. That single change is what
            // lets the matching below fall back to "keep the cells you have"
            // instead of "mint a new tile at first-fit".
            const auto slotOrder = [](const auto& x, const auto& y) {
                const bool xLive = g_prevKeys.contains(x.first);
                const bool yLive = g_prevKeys.contains(y.first);
                if (xLive != yLive) return xLive;
                // A parked tile has no cell yet -- it must not sort to the front
                // and take a placed sibling's position (GI30).
                const bool xP = x.second.col < 0, yP = y.second.col < 0;
                if (xP != yP) return !xP;
                if (x.second.bag != y.second.bag) return x.second.bag < y.second.bag;
                if (x.second.row != y.second.row) return x.second.row < y.second.row;
                return x.second.col < y.second.col;
            };
            // ★B3-b: keys whose drop is waiting on the engine's answer. The
            // two-phase deletion keeps the entry in g_layout (cancel-safe), so
            // without this exclusion the DEPARTING unit's cell was still a
            // claimable slot -- and position-order matching handed it to the
            // SURVIVOR. Storing the front of two identical daggers made the
            // back one jump to the front cell, then jump back when the commit
            // finally erased the slot: the reported "blink", front-store only,
            // because storing the back one hands the survivor its own front
            // cell. Excluded here, the entry survives untouched (the prune
            // below only walks `slots`) until Commit erases it or Cancel
            // returns it to circulation.
            std::set<std::string> pendingDrop;
            for (const auto& [fid, qs] : g_pendingSlotDrop) {
                for (const auto& qk : qs) {
                    if (BaseKey(qk) == a_base) pendingDrop.insert(qk);
                }
            }
            std::vector<std::pair<std::string, LayoutEntry>> slots;
            for (const auto& [k, le] : g_layout) {
                if (BaseKey(k) != a_base) continue;
                // a trash-parked tile is not a board cell: its unit is already in
                // the off-board list, so counting its slot here would leave
                // slots > units and hand a survivor somebody else's cell
                if (le.bag == kTrashKey) continue;
                // a carried tile takes its slot with it: excluding both the unit
                // and the slot keeps the remaining ones where they are (and
                // restores it on cancel). Same for a queued equip.
                if (inTransit.contains(k)) continue;
                if (pendingDrop.contains(k)) continue;   // ★see above
                slots.push_back({ k, le });
            }
            std::sort(slots.begin(), slots.end(), slotOrder);

            // The lowest free ordinal for this FORM: base, base#1, base#2 ...
            // Minted once and never recomputed -- an ordinal is a SLOT NAME now,
            // not a description of what is standing on it.
            std::set<std::string> minted;   // this walk's own reservations
            const auto freeKey = [&]() {
                for (int n = 0;; ++n) {
                    std::string k = n == 0 ? a_base : a_base + "#" + std::to_string(n);
                    if (!g_layout.contains(k) && !minted.contains(k) &&
                        !(g_held && k == g_held->key)) {
                        minted.insert(k);
                        return k;
                    }
                }
            };

            // ---- the survivors, in enumeration order -----------------------
            // `hide` answered HOW MANY of each pool to drop -- a quantity
            // question, which is why it is still asked per pool. WHICH SLOT each
            // survivor gets is a different question, and the two no longer have
            // to agree on a name. Dropping the TRAILING members preserves the
            // order the walk chose (plain units first, so their positions never
            // shift when a list appears or disappears).
            // ★★A HIDDEN UNIT STILL HOLDS ITS CELL. It is hidden because it is
            // spoken for -- a queued removal, an inactive loadout preset -- not
            // because it stopped existing, and the moment the preset is switched
            // back it must land where the player left it.
            //
            // So hidden units are MATCHED like everybody else and simply not
            // DRAWN. Dropping them from the set instead left their cells
            // unclaimed, and the prune at the bottom then deleted them: every
            // preset switch quietly cost the player that item's position.
            std::vector<UnitRef> live;
            std::vector<char>    hidden;
            for (auto& [prefix, members] : pools) {
                std::size_t want = members.size();
                if (const int h = hide[prefix]; h > 0) {
                    want -= (std::min)(want, static_cast<std::size_t>(h));
                }
                for (std::size_t i = 0; i < members.size(); ++i) {
                    live.push_back(members[i]);
                    hidden.push_back(i >= want ? 1 : 0);
                }
            }

            // ---- MATCHING: units to slots, best evidence first -------------
            //
            // The old code looked a slot up BY NAME and, when the name had
            // changed under it (a poison applied, a charge spent, a swap on the
            // doll), concluded the unit was new and gave it a fresh tile at
            // first-fit -- while the cell it actually came from was pruned as
            // surplus. That is "my sword jumped to the front gap", and no amount
            // of care inside the lookup could fix it, because the lookup was
            // being asked a question its key could not answer.
            //
            // Matching asks instead: of the cells this form already owns, which
            // one was showing THIS unit? Every tier is evidence, ordered
            // most-specific first so strong evidence is never out-bid by weak.
            // The last tier accepts ANY leftover cell, and that is the line that
            // makes the design safe: a stale hint costs a weaker match, never a
            // lost position.
            std::vector<int>  slotOf(live.size(), -1);
            std::vector<int>  tierOf(live.size(), -1);
            std::vector<char> taken(slots.size(), 0);
            int tier = 0;
            const auto pass = [&](auto&& a_ok) {
                for (std::size_t u = 0; u < live.size(); ++u) {
                    if (slotOf[u] >= 0) continue;
                    for (std::size_t s = 0; s < slots.size(); ++s) {
                        if (taken[s] || !a_ok(live[u], slots[s].second)) continue;
                        slotOf[u] = static_cast<int>(s);
                        tierOf[u] = tier;
                        taken[s]  = 1;
                        break;
                    }
                }
                ++tier;
            };
            if (a_instanceKeys) {
                // 0: the engine gave this unit a name of its own. Unambiguous.
                pass([](const UnitRef& u, const LayoutEntry& e) {
                    return u.uid != 0 && e.uid == u.uid; });
                // 1: same contents AND the same place in the list -- nothing
                //    about this unit changed since the last rebuild.
                pass([](const UnitRef& u, const LayoutEntry& e) {
                    return u.uid == 0 && e.uid == 0 &&
                           e.sig == u.sig && e.xlIdx == u.xlIdx; });
                // 2: same contents, the list moved under it. Content beats
                //    position: one insert shifts every later index at once,
                //    whereas contents only change when the ITEM changes.
                pass([](const UnitRef& u, const LayoutEntry& e) {
                    return u.uid == 0 && e.uid == 0 && e.sig == u.sig; });
                // 3: ★the CENSUS paired the vanished kind this slot is still
                //    labelled with against the new contents this unit carries
                //    (fewest changed axes first, normalised distance as the
                //    tiebreak -- the B1 rule promoted, §8-4). Content
                //    evidence, so it outranks the list POSITION below: xlIdx
                //    is the engine's to reorder, and every relabel B1 ever
                //    measured was gear walking THIS ladder -- the stackable
                //    relabel block was the rule's second seat, not its first.
                //    Peeked, not taken: this predicate runs once per
                //    (unit, slot) pair, and the binding retires itself -- the
                //    slot's sig is rewritten to the unit's on commit, so the
                //    next rebuild matches at tier 1/2 without asking again.
                {
                    const RE::FormID cform = a_entry && a_entry->object
                                                 ? a_entry->object->GetFormID()
                                                 : 0;
                    pass([cform](const UnitRef& u, const LayoutEntry& e) {
                        if (!cform || u.uid != 0 || e.uid != 0) return false;
                        if (e.sig == u.sig) return false;   // tier 2's business
                        const auto want = Census::PeekPair(cform, e.sig);
                        return want && *want == u.sig;
                    });
                }
                // 4: same place in the list, contents changed -- exactly what a
                //    poison, a spent charge or a grindstone does. This is the
                //    tier the whole redesign exists for. (Was tier 3 before
                //    the census pass above -- old logs read one lower.)
                pass([](const UnitRef& u, const LayoutEntry& e) {
                    return u.uid == 0 && e.uid == 0 &&
                           u.xlIdx >= 0 && e.xlIdx == u.xlIdx; });
            }
            // 5: anything left, in position order. Units that reach here are
            //    interchangeable as far as any evidence we have can tell, and
            //    "keep the board the player arranged" is the right answer for
            //    interchangeable things.
            pass([](const UnitRef&, const LayoutEntry&) { return true; });

            if (a_mutate && g_poolTrace &&
                (live.size() > 1 || !slots.empty() ||
                 !OffBoardUnitsFor(a_entry ? a_entry->object : nullptr, a_base).empty())) {
                std::string off;
                for (const auto& u : OffBoardUnitsFor(
                         a_entry ? a_entry->object : nullptr, a_base)) {
                    off += std::format("{}(uid {:04X} sig {:04X}) ", u.why, u.uid, u.sig);
                }
                int freed = 0, shown = 0;
                for (std::size_t s = 0; s < slots.size(); ++s) {
                    if (!taken[s]) ++freed;
                }
                for (std::size_t u = 0; u < live.size(); ++u) {
                    if (!hidden[u]) ++shown;
                }
                SKSE::log::info("[POOL] {} units={} shown={} slots={} freed={} | off: {}",
                    a_base, live.size(), shown, slots.size(), freed,
                    off.empty() ? "- " : off);
            }

            for (std::size_t u = 0; u < live.size(); ++u) {
                // hidden: it kept its cell above (that is the point), but it
                // draws no tile and never mints one.
                if (hidden[u]) continue;
                std::string key;
                if (slotOf[u] >= 0) {
                    key = slots[slotOf[u]].first;
                } else {
                    // genuinely new to the board: every existing cell of this
                    // form is spoken for.
                    key = freeKey();
                    if (a_mutate) {
                        g_layout[key] = LayoutEntry{ -1, -1, {}, 1 };
                        // Typed bags: THIS is the moment a tile first exists, and
                        // the only moment routing may choose for the player
                        // (PLAN_TYPED_BAGS 4-1).
                        g_freshTiles.push_back(key);
                        g_arrivedTiles.push_back(key);   // (1.3.2)
                    }
                }
                // The slot records which unit it ended up bound to. This is a
                // HINT for the next rebuild, not a name -- writing it can never
                // move a tile, only improve the next match.
                if (a_mutate) {
                    auto& le = g_layout[key];
                    le.uid   = live[u].uid;
                    le.sig   = live[u].sig;
                    le.xlIdx = live[u].xlIdx;
                }
                if (a_mutate && g_poolTrace) {
                    const auto le = g_layout.count(key) ? g_layout[key] : LayoutEntry{};
                    SKSE::log::info("[POOL]   -> '{}' at [{},{}] uid {:04X} sig {:04X} {}",
                        key, le.col, le.row, live[u].uid, live[u].sig,
                        slotOf[u] >= 0 ? std::format("(kept, tier {})", tierOf[u])
                                       : std::string("(NEW)"));
                }
                a_out.push_back({ key, live[u].xlIdx, live[u].uid, live[u].sig });
            }

            // ---- cells nothing claimed ------------------------------------
            // NOT while a unit of this base is in transit: lifting a worn unit
            // off the doll starts the carry a frame or more BEFORE the unequip
            // lands, so for those frames the unit is still worn and absent from
            // `live` while its cell is still remembered. Erasing here took an
            // innocent sibling's cell, which then first-fit into the front gap
            // and could not be put back by cancelling. The set reconciles by
            // itself once the engine catches up.
            //
            // A preset reservation and a trash park are NOT transit -- they are
            // steady state, they answer the same on every rebuild, and holding
            // this back for them (on one measured session, 137 times) cost the
            // player their layout for nothing.
            bool inFlight = false;
            if (a_entry) {
                for (const auto& u : OffBoardUnitsFor(a_entry->object, a_base)) {
                    if (u.why && (std::strcmp(u.why, "reserved") == 0 ||
                                  std::strcmp(u.why, "trash") == 0)) {
                        continue;
                    }
                    inFlight = true;
                    break;
                }
            }
            if (a_mutate && !inFlight) {
                // ★★★A RESERVED UNIT IS ABSENT, NOT GONE, AND IT KEEPS ITS CELL.
                //
                // The `hide` pass above was supposed to leave a reserved unit's
                // slot claimed while the unit itself stayed off the board. It
                // never ran: both callers pass INT_MAX for a_units, so `surplus`
                // is always negative and `hide` is always empty -- the design in
                // that comment has been dead the whole time. Meanwhile the walk
                // drops reserved units from `refs` outright, so nothing claims
                // their slots and this loop erases them. Turn a preset off and
                // that item alone came back to the first free gap.
                //
                // Spare them by name instead of reviving the surplus machinery,
                // which would risk HIDING units that ought to be drawn -- a far
                // worse failure than a forgotten cell.
                std::set<std::string> reserved;
                if (a_instanceKeys && a_entry && a_entry->object) {
                    for (const std::uint16_t rs :
                         Loadout::ReservedSigs(a_entry->object->GetFormID())) {
                        reserved.insert(PoolPrefix(a_base, 0, rs));
                    }
                }
                for (std::size_t s = 0; s < slots.size(); ++s) {
                    if (taken[s]) continue;
                    if (!reserved.empty() &&
                        reserved.contains(PoolOfSlot(slots[s].first))) {
                        continue;
                    }
                    g_layout.erase(slots[s].first);
                }
            }
        }

        struct CapTiles
        {
            std::vector<Item>     tiles;
            std::set<std::string> bagKeys;   // tile keys of PRESENT bag items
        };

        // Headless tile collection for the capacity sims (MaxAcceptUnits /
        // WouldOverflow / ComputeOverloaded — formerly three drifting copies).
        // Ordinal #k tiles of up-to-cap units (G4 per-tile counts stay a
        // display concern); saved spots from g_layout; overflow-zone spots are
        // TEMPORARY and never honoured; a bag FORM still owned anchors its
        // contents even while its tile is transiently absent, and dangling bag
        // refs are cleared here — the post-load false-overload fix, now shared
        // by all three sims instead of only ComputeOverloaded.
        // Shared tail of both collectors: E4b nesting scope + E4 dangling-ref
        // reflow. Pure over (tiles, owned bag forms) -- see the call sites.
        void NormalizeBagRefs(CapTiles& a_out, const std::set<std::string>& a_bagForms)
        {
            {
                std::map<std::string, std::string> bagAccept;   // bag tile key -> accept
                for (const auto& it : a_out.tiles) {
                    if (it.def.bag != 0) bagAccept[it.key] = it.def.accept;
                }
                for (auto& it : a_out.tiles) {
                    if (it.def.bag == 0 || it.inBag.empty()) continue;
                    const auto ba = bagAccept.find(it.inBag);
                    if (ba == bagAccept.end() || !ba->second.empty()) it.inBag.clear();
                }
            }
            for (auto& it : a_out.tiles) {
                if (!it.inBag.empty() && !a_out.bagKeys.contains(it.inBag) &&
                    !a_bagForms.contains(BaseKey(it.inBag))) {
                    it.inBag.clear();
                }
            }
        }

        // *B5: BaseKey -> form, the reverse of FormKey. The layout reader
        // below needs it for keys whose form has no displayed tile -- the
        // preset-hidden cells that the first observation round caught the
        // g_items reader missing.
        RE::TESBoundObject* ObjFromBaseKey(const std::string& a_base)
        {
            const auto bar = a_base.find('|');
            if (bar == std::string::npos || a_base.size() < bar + 4) return nullptr;
            const auto id = static_cast<RE::FormID>(
                std::strtoul(a_base.c_str() + bar + 3, nullptr, 16));
            RE::TESForm* f = nullptr;
            if (a_base.compare(0, bar, "Dynamic") == 0) {
                f = RE::TESForm::LookupByID(id);
            } else if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                f = dh->LookupForm(id, std::string_view(a_base).substr(0, bar));
            }
            return f ? f->As<RE::TESBoundObject>() : nullptr;
        }

        // *B5: the same collection READ OFF THE BOARD -- and the board's
        // truth is g_layout, not g_items. The first observation round proved
        // the distinction: preset-hidden units draw no tile but HOLD their
        // cells, so the display reader under-modelled exactly where it
        // matters (a pickup green-lit onto a hidden tile's cell). The layout
        // IS the persisted placement 1.4 made authoritative; reading anything
        // else re-derives it. Runs BESIDE the engine walk for observation;
        // the [CAP] divergence log decides the flip (the B0~B2 precedent).
        void CollectCapacityTilesFromBoard(CapTiles& a_out)
        {
            // ★B5, third and final shape -- the DISPLAY reader, restored.
            // The layout walk between tried to answer occupancy from
            // g_layout alone and needed an ownership filter, a reserved
            // yield and a worn exemption to approximate what g_items simply
            // is: the set of tiles that occupy cells. Its reserved yield
            // then mis-picked by key order whenever the reserved unit was
            // worn or hidden, and the sim under-modelled -- items past the
            // overload line with no overload called (user report, the
            // symmetric failure to the phantom refusals before it).
            //
            // Every class that pushed the reader off g_items has its real
            // answer elsewhere now: reserved-hidden cells are FREE by
            // design (user-corrected semantics -- a soft memory, not a
            // wall); worn leaks and unowned ghosts are swept from the
            // layout at every rebuild's end (rule 13's missed enforcement);
            // in-transit cells were free in the engine walk too; and a
            // stale or never-built board is the gate freshen's job.
            std::set<std::string> bagForms;
            if (auto* player = RE::PlayerCharacter::GetSingleton();
                player && g_resolver) {
                for (auto& [obj, data] : player->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    if (const GridDef bd = g_resolver(obj); bd.bag != 0) {
                        bagForms.insert(FormKey(obj));
                    }
                }
            }
            for (const auto& it : g_items) {
                if (it.inBag == kTrashKey) continue;   // deletion buffer: no cells
                Item t = it;
                if (t.def.bag != 0) a_out.bagKeys.insert(t.key);
                // overflow-zone spots are TEMPORARY and never honoured
                // (★W3: the zone starts past the OWNED cells now)
                if (t.inBag.empty() && t.col >= 0 && t.row >= 0 &&
                    !OwnedFootprint(t.col, t.row, t.mask)) {
                    t.col = -1;
                    t.row = -1;
                }
                a_out.tiles.push_back(std::move(t));
            }
            NormalizeBagRefs(a_out, bagForms);
        }

        // *B5-b: the flip. Two observation rounds settled it -- the engine
        // walk's divergences were ITS OWN defects (thirty-two preset-reserved
        // cells it modelled as free, because SkipInventoryEntry drops a fully
        // reserved form while the hidden tiles keep their cells), and the one
        // transient it modelled better (an arrival still at [-1,-1]) cannot
        // reach the real gates, which freshen the layout before they ask.
        // Two hundred lines of worn arithmetic, ledger subtraction and ring
        // accounting existed to re-derive what g_layout simply knows.
        void CollectCapacityTiles(CapTiles& a_out)
        {
            CollectCapacityTilesFromBoard(a_out);
        }
    }

    // ---- Phase 3: Rebuild stages (bodies moved verbatim) ----
    namespace
    {
        // ★B3 prep: promoted from a [&] lambda inside CollectDisplayTiles. The
        // partial-update path (AddUnitToBoard) has to mint a tile OUTSIDE a
        // rebuild, and a capture-everything lambda cannot be called from
        // there. Everything the tile needs arrives as a parameter now; the
        // body is the lambda's, verbatim. a_entry may carry stale pointers
        // after an engine change -- callers pass the LIVE entry.
        void MakeDisplayTile(RE::TESBoundObject* a_obj, RE::InventoryEntryData* a_entry,
                             const GridDef& a_gdef, std::uint8_t a_glow,
                             const std::string& a_key, int a_cnt,
                             const LayoutEntry& a_le, int a_xlIdx = -1)
        {
            const int col = a_le.col, row = a_le.row, rot = a_le.rot;
            const std::string& bag = a_le.bag;
            Item it;
            it.key = a_key;
            it.obj = a_obj;
            // P2/3-1: only a bag can be both worn and shown, so only a bag can
            // answer yes here -- entry-level IsWorn is enough for one of those.
            it.worn = a_gdef.bag != 0 && a_entry && a_entry->IsWorn();
            it.glow = a_glow;
            it.count = a_cnt;
            it.def = a_gdef;
            // GI62: the footprint the player left this tile at. Everything
            // downstream -- placement, collision, hit test, ghost, shading
            // -- reads `mask` and needs no further knowledge of rotation.
            it.rot = CanRotate(it.def) ? (rot & 3) : 0;
            it.mask = MaskOf(it.def, it.rot);
            it.col = col;
            it.row = row;
            it.inBag = bag;
            it.uid = a_le.uid;   // ★from the slot's hint, not from its name
            it.sig = a_le.sig;
            it.xlIdx = a_xlIdx;
            // ★★A TILE DRAWN FROM ITS LAYOUT ENTRY ALONE HAS NO LIVE
            // POSITION. Trash-parked tiles are emitted straight out of
            // g_layout with no unit walk behind them, so xlIdx is -1 --
            // and with it every per-unit answer went blank: no enchant
            // halo, no poison drop, and a tooltip that resolved no list
            // at all, so a TEMPERED dagger sitting in the bin drew as a
            // plain one. Taking it back out "fixed" it, because then the
            // walk had a position for it again.
            //
            // The slot still knows WHICH unit it holds. Resolve the list
            // from that instead: identity outlives position, which is the
            // entire reason it lives on the slot.
            if (it.xlIdx < 0 && (it.uid != 0 || it.sig != 0) &&
                a_entry && a_entry->extraLists) {
                int xi = 0;
                for (auto* xl : *a_entry->extraLists) {
                    const int here = xi++;
                    if (!xl) continue;
                    // a worn list belongs to the copy on the body
                    if (xl->HasType<RE::ExtraWorn>() ||
                        xl->HasType<RE::ExtraWornLeft>()) {
                        continue;
                    }
                    const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                    if (it.uid != 0) {
                        if (xu && xu->uniqueID == it.uid) { it.xlIdx = here; break; }
                        continue;
                    }
                    // a uid unit is the sole member of its own pool, so it
                    // can never answer a signature-pool request
                    if (xu && xu->uniqueID != 0) continue;
                    if (InstanceSig(xl) == it.sig) { it.xlIdx = here; break; }
                }
            }
            // D2: the crafted-enchant glow belongs to THIS unit
            if (const auto* xl = ExtraForTile(a_entry, it.uid, it.xlIdx)) {
                if (const auto* xe = xl->GetByType<RE::ExtraEnchantment>();
                    xe && xe->enchantment) {
                    it.glow |= 1;
                }
                // GI66: poison only. The temper bit was computed here
                // too and nothing reads it any more -- temper lives in
                // the name, the damage number and the price, and marking
                // it as well meant marking almost every weapon past
                // mid-game.
                if (const auto* xp = xl->GetByType<RE::ExtraPoison>();
                    xp && xp->poison) {
                    it.glow |= 4;
                }
            }
            // GI40: the star belongs to the POOL, not to one list.
            //
            // Asking a single ExtraDataList was wrong in both directions.
            // Entry-level (vanilla's test) starred every copy of the form,
            // tempered ones included. List-level lost the star the moment
            // anything split the pool -- equipping one dagger moves the
            // ExtraHotkey onto the worn list, and all the spares standing
            // in the bag went dark even though nothing was unfavourited.
            //
            // The pool is the right unit: it is exactly "the identical
            // things", which is what the engine can mark and what rule 53
            // promises. Tempered and plain stay separate (rule 54) because
            // they are separate pools.
            it.fav = PoolHasStar(a_entry, it.uid, it.sig);
            // ★Same question, same shape: asked of the LIVE list rather
            // than of a cache keyed by the tile's name.
            it.stolen = PoolIsStolen(a_entry, it.uid, it.sig);
            it.quest  = PoolIsQuest(a_entry, it.uid, it.sig);
            // overflow-zone spots (past the OWNED cells -- ★W3) are TEMPORARY
            // — never honour them, so the item first-fits back INTO the
            // board the moment space frees up and the extra rows collapse.
            if (it.inBag.empty() && it.col >= 0 && it.row >= 0 &&
                !OwnedFootprint(it.col, it.row, it.mask)) {
                it.col = -1;
                it.row = -1;
            }
            // E3: bags live in main — except a parked (empty) bag in
            // the trash (F2 allows trashing an empty bag), and (E4b) a
            // bag stowed by hand inside a GENERAL bag. Anything else a
            // bag ref points at (typed bag, stale key) reflows to main.
            if (it.def.bag != 0 && it.inBag != kTrashKey && !it.inBag.empty()) {
                const auto ba = g_bagAcceptByForm.find(BaseKey(it.inBag));
                if (ba == g_bagAcceptByForm.end() || !ba->second.empty()) {
                    it.inBag.clear();
                }
            }
            g_items.push_back(std::move(it));
        }

        // stage 1+2: walk the live inventory into display tiles — coin
        // mirror partition + G4 per-tile reconcile. Fills g_items and the
        // per-form caches (g_gold / g_values).
        void CollectDisplayTiles(RE::PlayerCharacter* player)
        {
            // ★Typed bags: one pass to learn which filters the player has a bag
            // for, BEFORE any tile is built — the stackable path needs to know
            // while it is deciding where new units go, and the bag tiles do not
            // exist yet at that moment.
            g_typedBagsHeld.clear();
            g_bagAcceptByForm.clear();
            // Copies that no routing can reach: parked in the trash (queued
            // for deletion — CollectBagSlots skips them) or riding the cursor
            // (no place on the board). A filter whose every bag is unreachable
            // must NOT count as held, or the skip below keeps starting fresh
            // tiles that nothing can claim — one orphan half-stack per pickup.
            std::map<std::string, int> unreachable;
            for (const auto& [k, le] : g_layout) {
                if (le.bag == kTrashKey) ++unreachable[BaseKey(k)];
            }
            if (g_held && g_held->isBag && g_held->obj) {
                ++unreachable[FormKey(g_held->obj)];
            }
            for (auto& [obj, pair] : player->GetInventory()) {
                if (!obj || pair.first <= 0) continue;
                const GridDef d = g_resolver ? g_resolver(obj) : GridDef{};
                if (!d.bag) continue;
                const std::string fk = FormKey(obj);
                g_bagAcceptByForm[fk] = d.accept;
                if (d.accept.empty()) continue;
                const auto u = unreachable.find(fk);
                if (pair.first > (u == unreachable.end() ? 0 : u->second)) {
                    g_typedBagsHeld.insert(d.accept);
                }
            }

            // ---- collect ----
            for (auto& [obj, pair] : player->GetInventory()) {
                const int count = pair.first;
                auto* entry = pair.second.get();
                if (!obj || count <= 0) continue;
                if (obj->IsGold()) { g_gold = count; continue; }
                if (SkipInventoryEntry(obj, count)) continue;   // shared filter (Phase 2)

                // Phase 4/5: cache the barter base value for this form. Coins are
                // excluded (mirror, not sellable) but the POUCH is a real sellable
                // item.
                // GI44: the FORM base, never entry->GetValue(). The entry-level
                // native folds in whichever list it fancies, so ONE tempered
                // dagger silently raised the cached base of every plain sibling
                // to 11 -- which is also where the old "Sell 4 vs vanilla 3"
                // mystery came from (the display showed the honest 10 while the
                // sale priced the polluted 11). Temper is folded in per UNIT by
                // UnitValueWith now; the base must stay variant-blind.
                if (entry && !(GoldCoins::IsCoinForm(obj->GetFormID()) &&
                               !GoldCoins::IsPouch(obj->GetFormID()))) {
                    g_values[obj->GetFormID()] = obj->GetGoldValue();
                    // Phase 6: stolen = not owned by the player (defaultTo=true so an
                    // unowned item counts as the player's = not stolen). Merchants
                    // that don't buy stolen refuse it (unless a fence).
                    //
                    // Per SUB-STACK: the engine stamps ownership on the list it
                    // moved, so only that unit is stolen. Asking the entry (which
                    // answers "does ANY list have a foreign owner") branded the
                    // whole form.
                    // ★★The two marker CACHES that used to be rebuilt here are
                    // gone. They were keyed by PoolPrefix(base, uid, sig) -- a
                    // name with the item's mutable state inside it -- so every
                    // poison, charge and grindstone visit orphaned an entry, and
                    // the only way to stay honest was to tear the whole thing
                    // down and rebuild it from the entire inventory every single
                    // rebuild. makeTile now asks the live sub-stack instead
                    // (PoolIsStolen / PoolIsQuest), which cannot go stale.
                }

                // G3: Mabinogi stacking — a tile holds at most StackCapOf units;
                // overflow spills into extra tiles keyed "#k" (each keeps its own
                // saved spot). Cap-1 forms (gear/coins) get one tile per unit; a
                // worn copy sits on the doll while its spares stay on the board.
                const GridDef gdef = g_resolver ? g_resolver(obj) : GridDef{};
                const int cap = EffectiveCap(obj, gdef);
                const bool worn = entry && entry->IsWorn();
                // The suppression exists only for the gap between requesting an
                // equip and the engine applying it. IsWorn() turning true IS the
                // engine applying it -- releasing on our own queue instead was a
                // frame or two early, and the pool stayed double-hidden (the last
                // spare blinked out) until the next rebuild.
                // Release only the units the engine has ACTUALLY put on. IsWorn()
                // is entry-level: with a copy already equipped it was true from the
                // start, so the suppression died in the same rebuild that armed it
                // and the queued unit showed a ghost tile until the engine caught up.
                ReleaseWornPendingEquips(FormKey(obj), entry);
                // How many units the body is ACTUALLY wearing. IsWorn() is
                // entry-level -- one torch in hand made the whole FORM "worn", so
                // all three left the board and looked equipped together. Lifting
                // the one real torch off the doll then "released" the other two,
                // which is what "2 come back and only 1 is on the cursor" was.
                // Only ever one torch is lit; the other two were never equipped.
                int wornUnits = 0;
                if (worn && entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                   xl->HasType<RE::ExtraWornLeft>())) {
                            wornUnits += (std::max)(1, xl->GetCount());
                        }
                    }
                }
                if (worn && wornUnits <= 0) wornUnits = 1;   // worn but unlisted
                // ★★The SECOND ring is worn without the engine knowing it: a
                // carrier wears its enchantment while the ring itself stays in
                // the pack, so it carries no ExtraWorn for the count above to
                // find. Counted by hand, or the player sees the very same ring
                // on the doll AND on the board at once.
                if (DualRing::Second() == obj) wornUnits += 1;
                // ★★★P2/3-1, AND THIS IS THE LINE THAT DECIDES IT. A worn unit
                // leaves the board by being SUBTRACTED here -- not by the
                // per-unit skip further down, which is what the first attempt
                // at this exception patched, to no visible effect (the tile
                // still vanished, the contents still spilled, and every other
                // symptom in the report followed from that one).
                //
                // A BAG is the single form that does not leave: wearing a
                // backpack is not putting it away, and the container you have
                // on your back is the one you most expect to keep using. So it
                // keeps its cells, its subgrid and its right-click, and the
                // tray marker says it is also on the doll.
                if (gdef.bag != 0) wornUnits = 0;
                int units = count - wornUnits -
                            Loadout::ReservedCount(obj->GetFormID());
                // Phase 7: units sold/stored whose engine removal is still queued
                // leave the board NOW (pending-drop pattern) — else the interim
                // rebuild re-seats them as a fresh pickup at the front.
                units -= Ledger::OpenOutgoingCount(obj->GetFormID());   // B4-3c
                // ---- units in transit (stackables) -----------------------------
                // This branch knew only about `carried`, `reserved` and queued
                // removals -- none of the mid-transition machinery the gear branch
                // has. Two blinks came straight out of that:
                //
                //   equip   the unit went back onto the board for the frame or two
                //           between asking and the engine doing it
                //   unlift  a stackable lifted off the doll was subtracted TWICE,
                //           once as worn and once as carried, until the unequip
                //           landed -- so the stack dipped by one and came back
                //
                // Both are the same rule: an off-board unit is subtracted unless a
                // worn list is already accounting for it.
                int wornFree = wornUnits;
                const std::string baseKey = FormKey(obj);
                int carried = 0;
                // (fromPartner excluded for the same reason as the gear branch:
                // the container's item on the cursor is not one of ours)
                if (g_held && g_held->obj && !g_held->fromPartner &&
                    FormKey(g_held->obj) == baseKey) {
                    carried = g_held->count;
                    // ★★A carry that was DISPLACED BY A SWAP is not backed by
                    // the worn list any more -- the unit that replaced it is.
                    // The gear branch has always known this (its `stillWorn`
                    // test); the stackable branch never got the rule, and for a
                    // SAME-FORM swap that is fatal: the worn count stays at 100
                    // because the NEW quiver is wearing it, so the displaced
                    // hundred was credited as worn while also riding the cursor,
                    // and the board grew a second hundred at the first free
                    // cell. Different forms escaped it only because the old
                    // form's worn count really does drop to zero.
                    // Measured: stock=200 wornUnits=100 carried=0 place=100
                    // owned=0 diff=100 -> a tile conjured out of the carry.
                    const bool stillWorn = g_held->fromDoll && !g_held->swappedOut;
                    if (stillWorn && carried > 0) {
                        // ★As many as the carry actually holds, not one: a
                        // quiver comes off the doll whole, and cancelling a
                        // single unit against a 100-arrow carry left 99 of them
                        // counted twice -- once as worn, once as carried.
                        const int fromWorn = (std::min)(wornFree, carried);
                        wornFree -= fromWorn;   // the carry IS those worn units
                        carried  -= fromWorn;   // ...so they are already out of `units`
                    }
                }
                for (const auto& u : g_pendingEquip) {
                    if (u.base != baseKey) continue;
                    // ★u.units, not 1: an ammo equip takes the whole tile.
                    const int n = (std::max)(1, u.units);
                    const int fromWorn = (std::min)(wornFree, n);
                    wornFree -= fromWorn;           // the engine already took these
                    units -= (n - fromWorn);        // the rest are still in the pack
                }
                // ★A stackable carried off the DOLL is the one case where the
                // carry and the board share a key namespace, and every quiver
                // bug lived in these numbers. Kept behind the trace switch so
                // the next one is a flag flip, not a rebuild of the reasoning.
                const bool qTrace = g_poolTrace && g_held && g_held->obj &&
                                    g_held->fromDoll &&
                                    FormKey(g_held->obj) == baseKey;
                if (qTrace) {
                    SKSE::log::info(
                        "[QUIVER] {} stock={} wornUnits={} units={} carried={} "
                        "wornFree={} carry={}x'{}' swappedOut={} pend={}",
                        obj->GetName(), count, wornUnits, units, carried, wornFree,
                        g_held->count, g_held->key, g_held->swappedOut,
                        g_pendingEquip.size());
                }
                if (units <= 0) continue;        // the single worn copy: doll only
                const int tiles = (units + cap - 1) / cap;

                // rarity glow: enchanted (EITM / player-crafted) and unique (DESC).
                // GI1/D2: the base-form part is per FORM; the crafted-enchant bit
                // is per SUB-STACK and is added in makeTile from that tile's own
                // list. Folding it in here is what made every copy of a form glow
                // because one of them was enchanted.
                std::uint8_t glow = 0;
                if (const auto* ef = obj->As<RE::TESEnchantableForm>();
                    ef && ef->formEnchanting) {
                    glow |= 1;
                }
                if (IsUniqueCached(obj)) glow |= 2;

                // ★S-G: COIN TILES ARE NOT WALKED ANY MORE. Every coin
                // tile owns its amount on its layout slot, and the emission
                // pass at the end of this function draws them straight out of
                // g_layout (the trash-parked precedent). The physical coin
                // items this branch used to partition are purged at load; any
                // straggler that still walks past here is skipped so it can
                // never tile twice. The pouch still takes the generic path.
                if (GoldCoins::IsCoinForm(obj->GetFormID()) &&
                    !GoldCoins::IsPouch(obj->GetFormID())) {
                    continue;
                }

                // emit one tile Item at a saved (or fresh, col<0) spot
                // ★Everything about a tile now comes from its SLOT. The old
                // signature took col/row/bag/rot loose and dug uid/sig out of the
                // key; passing the LayoutEntry says plainly where a tile's
                // properties live, and there is no longer anywhere else to look.
                // ★B3 prep: the body is MakeDisplayTile now -- promoted so the
                // partial-update path can mint tiles outside a rebuild. This
                // wrapper only pins the loop-local context.
                auto makeTile = [&](const std::string& key, int cnt,
                                    const LayoutEntry& le, int xlIdx = -1) {
                    MakeDisplayTile(obj, entry, gdef, glow, key, cnt, le, xlIdx);
                };

                if (cap <= 1) {
                    // GI1/GI2: gear / non-stackable — one tile per UNIT, each
                    // bound to the engine sub-stack it actually shows.
                    std::vector<UnitTile> units_v;
                    EnumerateUnitTiles(baseKey, count,   // P1: see the sim's note
                                       (std::numeric_limits<int>::max)(), entry, units_v,
                                       gdef.bag == 0, /*mutate=*/true);
                    // ---- self-check: CONSERVATION ----------------------------
                    // Every unit the engine says the player owns has to be in
                    // exactly one place: drawn on the board, worn, or named in the
                    // off-board set. If the three do not add up to the engine's
                    // count, something was removed twice or not at all -- which is
                    // what "a spare vanished", "it is on the cursor AND the board"
                    // and "it flickers" all look like from the inside. Reporting
                    // the mismatch here means the log identifies the fault without
                    // anyone having to describe the symptom.
                    if (g_poolTrace) {
                        int wornN = 0;
                        if (entry && entry->extraLists) {
                            for (auto* xl : *entry->extraLists) {
                                if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                           xl->HasType<RE::ExtraWornLeft>())) {
                                    wornN += (std::max)(1, xl->GetCount());
                                }
                            }
                        }
                        const auto offv = OffBoardUnitsFor(obj, baseKey);
                        int drawn = 0;
                        for (const auto& u : units_v) {
                            drawn += (std::max)(1, g_layout.count(u.key)
                                                       ? g_layout[u.key].count : 1);
                        }
                        // An off-board unit that is STILL WORN in the engine is
                        // already inside wornN -- lifting a worn item off the doll
                        // starts the carry before the unequip lands, and a queued
                        // equip is worn before the entry clears. Counting it in
                        // both places reported a phantom -1 on every carry.
                        // STRICT match only. WornExtraMatching falls back to "any
                        // worn list of this form" so the doll never shows a blank,
                        // which is right for display and wrong for counting: a
                        // carried TEMPERED dagger matched the PLAIN one on the
                        // body, was written off as already-worn, and the check
                        // reported a unit missing that was never missing.
                        const auto wornBacked = [&](const OffBoardUnit& o) {
                            if (!entry || !entry->extraLists) return false;
                            for (auto* xl : *entry->extraLists) {
                                if (!xl) continue;
                                const bool L = xl->HasType<RE::ExtraWornLeft>();
                                const bool R = xl->HasType<RE::ExtraWorn>();
                                if (!L && !R) continue;
                                if (o.hand == 1 && !R) continue;
                                if (o.hand == 2 && !L) continue;
                                std::uint16_t u = 0;
                                if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                                    u = xu->uniqueID;
                                }
                                if (u == o.uid && InstanceSig(xl) == o.sig) return true;
                            }
                            return false;
                        };
                        std::string why;
                        int offLoose = 0, wornTaken = 0;
                        for (const auto& o : offv) {
                            why += std::string(o.why) + " ";
                            // an ARRIVING unit only counts as worn once its equip
                            // has actually run -- the engine's event says so
                            // (B4-2c: same clock as the walk, or the check
                            // would flag the very windows the flip closed)
                            if (o.mayBeWorn && !(o.arriving && !o.landed) &&
                                wornTaken < wornN && wornBacked(o)) {
                                ++wornTaken;
                                continue;
                            }
                            ++offLoose;
                        }
                        if (why.empty()) why = "-";
                        const int total = drawn + wornN + offLoose;
                        if (total != count) {
                            SKSE::log::warn("[CHECK] {} MISMATCH engine={} drawn={} worn={} "
                                            "offboard={} ({}) -> {} units unaccounted",
                                baseKey, count, drawn, wornN, offv.size(),
                                why, count - total);
                        }
                        // ---- self-check: FLICKER ---------------------------------
                        // A settled board rebuilds to the SAME drawn set every frame.
                        // Logging only the transitions keeps this quiet: a clean
                        // right-click equip is one line ("3 -> 2"), while a flicker
                        // is a pair that comes straight back ("3 -> 2" then "2 -> 3"
                        // with no user action between them). The conservation check
                        // above cannot see this -- it only ever looks at one frame.
                        {
                            std::vector<std::string> ks;
                            ks.reserve(units_v.size());
                            for (const auto& u : units_v) ks.push_back(u.key);
                            std::sort(ks.begin(), ks.end());
                            std::string sig = std::to_string(drawn);
                            for (const auto& k : ks) { sig += ' '; sig += k; }
                            auto& prev = g_flickPrev[baseKey];
                            if (prev != sig) {
                                SKSE::log::info("[FLICK] {} drawn {} -> {} (engine={} "
                                                "worn={} off={} [{}])  {}",
                                    baseKey,
                                    prev.empty() ? std::string("-")
                                                 : prev.substr(0, prev.find(' ')),
                                    drawn, count, wornN, offv.size(), why, sig);
                                prev = sig;
                            }
                        }
                        // two tiles on one cell is always wrong
                        std::set<std::pair<int, int>> cells;
                        for (const auto& u : units_v) {
                            const auto li = g_layout.find(u.key);
                            if (li == g_layout.end() || li->second.col < 0) continue;
                            if (!cells.insert({ li->second.col, li->second.row }).second) {
                                SKSE::log::warn("[CHECK] {} OVERLAP at [{},{}] key '{}'",
                                    baseKey, li->second.col, li->second.row, u.key);
                            }
                        }
                    }

                    // F2: a trash-parked GEAR tile is deliberately not a board
                    // cell -- the pool skips its slot and its unit is already in
                    // the off-board set -- but it still has to be DRAWN, in the
                    // trash view. Nothing emitted one, so binning a dagger made it
                    // vanish outright: absent from the grid, absent from the bin,
                    // and "restored" only because closing the trash cleared the
                    // bag off a layout entry that had never been shown. (The
                    // stackable branch never had this hole: its slot loop keeps
                    // trash entries and only skips them when choosing what to
                    // fill or drain.)
                    std::vector<std::string> parked;
                    for (const auto& [k, le] : g_layout) {
                        if (le.bag != kTrashKey || BaseKey(k) != baseKey) continue;
                        // ★★A PARKED TILE THAT IS ON THE CURSOR IS NOT IN THE
                        // BIN. Lifting one out of the trash leaves its layout
                        // entry in place (a cancel has to put it back), and
                        // this loop drew from that entry unconditionally -- so
                        // the item hung on the cursor AND sat in the bin at the
                        // same time. One item, drawn twice, looks like two.
                        if (g_held && k == g_held->key) continue;
                        parked.push_back(k);
                    }
                    for (const auto& k : parked) {
                        const auto& le = g_layout[k];
                        makeTile(k, (std::max)(1, le.count), le);
                    }

                    for (const auto& u : units_v) {
                        LayoutEntry le;
                        if (const auto li = g_layout.find(u.key); li != g_layout.end()) le = li->second;

                        // B2: partner-drop hint — a NEW gear tile lands at the
                        // drop cell too (the stackable branch below already
                        // consumes it; without this, looted gear ignored the
                        // drop position and first-fitted into any free cell).
                        //
                        // GI21: the test is "has no position yet", NOT "has no
                        // entry". Pool assignment now creates a placeholder entry
                        // (col -1) for a fresh unit so two arrivals in one rebuild
                        // cannot claim the same key -- which made the old
                        // entry-missing test permanently false, and every dragged
                        // loot silently first-fit into the front gap instead of
                        // landing where it was dropped.
                        if (le.col < 0 &&
                            g_dropHint.Wants(baseKey,
                                PoolPrefix(baseKey, u.uid, u.sig))) {
                            le.col = g_dropHint.col;
                            le.row = g_dropHint.row;
                            le.bag = g_dropHint.bag;
                            le.rot = g_dropHint.rot;   // GI62
                            g_layout[u.key] = le;   // persist the placement
                            g_dropHint = {};
                            // ★Typed bags: hand-placed — the pool walk already
                            // listed this unit as fresh, so unlist it or the
                            // claim overrides the very cell the hint just set.
                            std::erase(g_freshTiles, u.key);
                        }
                        // (the hints were written by EnumerateUnitTiles, so the
                        //  copy read above already carries them)
                        makeTile(u.key, 1, le, u.xlIdx);
                    }
                    continue;   // form handled
                }

                // ---- G4: explicit per-tile counts (Mabinogi split/merge) ----
                // A tile OWNS its quantity; the reconciler only closes the gap
                // between the saved sum and the engine's live count. The carried
                // tile/fragment of THIS form is excluded from placement and its
                // units are represented by g_held->count instead — so splitting a
                // stack never looks like a shrink that gets re-absorbed.
                struct Slot { std::string key; LayoutEntry le; };
                std::vector<Slot> slots;
                for (auto& [k, v] : g_layout) {
                    if (BaseKey(k) != baseKey) continue;
                    if (g_held && k == g_held->key) continue;   // carried whole tile
                    slots.push_back({ k, v });
                }
                std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) {
                    if (a.le.bag != b.le.bag) return a.le.bag < b.le.bag;
                    if (a.le.row != b.le.row) return a.le.row < b.le.row;   // top-left order
                    return a.le.col < b.le.col;
                });

                // ---- POOLS: a signed sub-stack is not the same thing -----------
                // ★★★Every attribute that hangs off a pool -- the stolen dot, the
                // quest lock, the merchant's refusal -- is recorded under
                // PoolPrefix(base, uid, sig). Gear tiles carry that prefix
                // because they come from EnumerateUnitTiles; STACKABLE tiles were
                // keyed `base#n` with no signature at all, so PoolOfKey handed
                // back the bare base and none of those lookups could ever match.
                // A stolen gem showed no dot, sold to any merchant instead of a
                // fence, and a quest-flagged stack could be dropped (user report).
                // ★Patching the LOOKUP could not fix it: a stackable tile has no
                // sub-stack identity, so "5 amethysts" may be 1 stolen + 4 clean
                // and no single answer is right for that tile. The tiles have to
                // split, which is what pooling by signature does -- the same rule
                // the gear branch already follows, and the position-order slot
                // assignment both branches share is unchanged.
                // ★Only signed sub-stacks split out (ownership, quest alias,
                // health, enchantment, charge, poison, soul). Everything with no
                // list, which is nearly every stack in the game, stays in the
                // plain pool and is keyed exactly as before.
                int signedUnits = 0;
                auto poolUnits = SignedPools(entry, baseKey, signedUnits,
                                             /*countTrashed=*/false);
                // ★★Every pool the LAYOUT knows about, seeded at zero. A signed
                // pool is otherwise only created from a live extraList, so when
                // the last unit of one leaves (a quest script reclaiming its
                // item, an engine removal landing) its slots belong to no pool at
                // all: never drained, because the drain walks pools -- yet still
                // emitted, because the emit walks the layout. The tile then sits
                // on the board for good and FinalizeRebuild writes it to the
                // cosave. The old form-wide `owned` sum drained these for free.
                for (const auto& s : slots) {
                    poolUnits.emplace(PoolPrefix(baseKey, s.le.uid, s.le.sig), 0);
                }
                // ★The plain pool takes the REMAINDER, so every subtlety the
                // form-wide accounting above worked out (worn, loadout reserves,
                // queued removals, pending equips) stays attached to it rather
                // than being re-derived per pool and getting one of them wrong.
                poolUnits[baseKey] = (std::max)(0, units - signedUnits);

                // ★The carry belongs to ONE pool. Its key says which -- except
                // for a split fragment, whose key is deliberately empty until it
                // lands (PoolOfKey("") matches nothing, so the carry used to be
                // subtracted from no pool at all and the source stack was
                // refilled underneath it). That case carries the pool itself.
                std::string carryPool;
                if (carried > 0 && g_held) {
                    carryPool = g_held->key.empty() ? g_held->srcPool
                                                    : PoolPrefix(baseKey, g_held->uid,
                                                                  g_held->sig);
                    if (carryPool.empty()) carryPool = baseKey;   // pre-GI carries
                }

                // slots grouped by pool, in one pass (see the loop's note)
                std::map<std::string, std::vector<std::size_t>> byPool;
                for (std::size_t si = 0; si < slots.size(); ++si) {
                    byPool[PoolPrefix(baseKey, slots[si].le.uid,
                                      slots[si].le.sig)].push_back(si);
                }
                static const std::vector<std::size_t> kNoSlots;

                // ★★A WHOLE STACK CAN CHANGE POOL AT ONCE. Five amethysts lifted
                // from an owned chest become stolen together; laundering them
                // clears it again. The units never moved -- only what they ARE
                // changed -- so the pile must not move either.
                //
                // Draining the vacated pool and minting tiles in the arriving one
                // is what the old code did, and it walked the pile across the
                // board. Re-labelling the slots they are already standing on is
                // the stackable form of what the gear matcher does above, and
                // because it is a FIELD WRITE rather than a rename, nothing that
                // POINTS at a key (a bag's contents, a pinned purse) can be
                // orphaned by it.
                {
                    std::vector<std::string> vacated, arriving;
                    for (const auto& [pk, idxs] : byPool) {
                        const auto w = poolUnits.find(pk);
                        if (w == poolUnits.end() || w->second <= 0) vacated.push_back(pk);
                    }
                    for (const auto& [pk, want] : poolUnits) {
                        if (want > 0 && !byPool.contains(pk)) arriving.push_back(pk);
                    }
                    auto relabel = [&](const std::string& from,
                                       const std::string& to, const char* how) {
                        const auto node = byPool.find(from);
                        if (node == byPool.end()) return;
                        for (const auto si : node->second) {
                            slots[si].le.uid = UidOf(to);
                            slots[si].le.sig = SigOf(to);
                            auto& live = g_layout[slots[si].key];
                            live.uid = slots[si].le.uid;
                            live.sig = slots[si].le.sig;
                        }
                        if (g_poolTrace) {
                            SKSE::log::info("[POOL] relabel {} slot(s) '{}' -> '{}' ({})",
                                            node->second.size(), from, to, how);
                        }
                        byPool[to] = node->second;
                        byPool.erase(from);
                    };
                    // ★★The census ASSIGNS the pairs now (B1 rule promoted,
                    // §8-4): fewest changed axes first, normalised distance
                    // as the tiebreak. What decided this before was map order
                    // over sig-keyed pool strings -- hash order, no order at
                    // all. The habitat is an UNWORN item whose values change
                    // while the menu is shut -- the grindstone above all:
                    // tempering moves no counts, so no event fires, and two
                    // same-form blades re-tempered in one session meet the
                    // next open as an N x M cross that hash order could seat
                    // in each other's cells (§1(b)). NOT the combat-drain
                    // case: charge drains on WIELDED units, which are off the
                    // board -- nothing to relabel (user caught this comment
                    // claiming otherwise). Uid-keyed pools ('@') stay out of
                    // it twice over: their key survives a value change so
                    // they never relabel, and SigOf reads 0 on them, which
                    // would collide with the plain pool's legitimate sig 0.
                    std::vector<std::string> vacLeft;
                    for (const auto& from : vacated) {
                        std::optional<std::uint16_t> want;
                        if (UidOf(from) == 0) {
                            want = Census::TakePair(obj->GetFormID(), SigOf(from));
                        }
                        bool paired = false;
                        if (want) {
                            for (auto at = arriving.begin(); at != arriving.end(); ++at) {
                                if (UidOf(*at) == 0 && SigOf(*at) == *want) {
                                    relabel(from, *at, "census");
                                    arriving.erase(at);
                                    paired = true;
                                    break;
                                }
                            }
                        }
                        if (!paired) vacLeft.push_back(from);
                    }
                    // Leftovers keep the old first-come pairing: right for
                    // the 1<->1 case that needs no rule (either answer is the
                    // same relabel), and the honest fallback for a drift the
                    // census never saw -- its take runs at menu open/close,
                    // and a mid-menu change diffs against nothing.
                    std::size_t taken = 0;
                    for (const auto& to : arriving) {
                        if (taken >= vacLeft.size()) break;
                        relabel(vacLeft[taken++], to, "order");
                    }
                }

                // std::map order puts the plain pool first (`base` < `base~XXXX`),
                // which is what the one-shot drop hint below wants: an ordinary
                // pickup still gets it.
                for (const auto& [poolKey, poolWant] : poolUnits) {
                // `carried` was already reduced above by whatever the worn count
                // is still accounting for -- subtracting the raw carry here took
                // a doll-lifted unit out twice.
                int placeUnits = poolKey == carryPool
                                     ? (std::max)(0, poolWant - carried) : poolWant;

                // this pool's slots, by index so the minting below can append to
                // `slots` without invalidating the walk
                // ★Grouped ONCE before the loop, not re-scanned per pool: this
                // used to be O(pools x slots) with a PoolOfKey parse on every
                // visit, for a walk that already runs behind a rebuild gate.
                auto  mineIt = byPool.find(poolKey);
                auto& mine   = mineIt != byPool.end() ? mineIt->second : kNoSlots;
                if (placeUnits <= 0 && mine.empty()) continue;

                int owned = 0;
                for (auto si : mine) owned += (std::max)(0, slots[si].le.count);

                int diff = placeUnits - owned;
                if (qTrace) {
                    std::string ks;
                    for (auto si : mine) {
                        ks += " '" + slots[si].key + "'x" +
                              std::to_string(slots[si].le.count);
                    }
                    SKSE::log::info("[QUIVER] pool='{}' place={} owned={} diff={} slots{}",
                                    poolKey, placeUnits, owned, diff,
                                    ks.empty() ? " (none)" : ks);
                }
                if (diff > 0) {
                    // ★★★A DROP HINT IS A PLACEMENT, NOT A LEFTOVER.
                    //
                    // The hint used to be spent down in the spill loop, which
                    // runs AFTER the fill below -- so anything the fill absorbed
                    // never reached the square the player aimed at. With a typed
                    // bag held that is not an edge case but the normal outcome:
                    // drag potions out of a chest onto an empty cell and the
                    // potion bag tops its own partial stack up first, leaving
                    // only the remainder to land where they were dropped
                    // (reported). The fill loop is right to prefer the bag for
                    // an arrival NOBODY AIMED -- loot off a corpse, a purchase,
                    // a reward. This one was aimed.
                    //
                    // So the aimed units are placed first and the rest goes on
                    // to the ordinary rules. A hinted tile is deliberately not
                    // pushed to g_freshTiles: routing must never override the
                    // player's own hand (same reason the spill path skips it).
                    if (g_dropHint.Wants(baseKey, poolKey)) {
                        const int want = g_dropHint.count > 0 ? g_dropHint.count : diff;
                        const int cnt = (std::min)({ want, cap, diff });
                        if (cnt > 0) {
                            Slot ns;
                            ns.key = NextTileKey(baseKey);
                            ns.le.uid = UidOf(poolKey);
                            ns.le.sig = SigOf(poolKey);
                            ns.le.col = g_dropHint.col;
                            ns.le.row = g_dropHint.row;
                            ns.le.bag = g_dropHint.bag;
                            ns.le.rot = g_dropHint.rot;   // GI62
                            ns.le.count = cnt;
                            g_dropHint = {};
                            g_layout[ns.key] = ns.le;   // reserve: NextTileKey advances
                            g_arrivedTiles.push_back(ns.key);
                            slots.push_back(std::move(ns));
                            diff -= cnt;
                        }
                    }
                    // ACQUIRE: fill partial tiles top-left, then spill into new tiles
                    for (auto si : mine) {
                        auto& s = slots[si];
                        if (diff <= 0) break;
                        // F2: never absorb fresh pickups into a tile parked in
                        // the trash (it's queued for deletion, not storage)
                        if (s.le.bag == kTrashKey) continue;
                        // ★Typed bags: do NOT top up a pile the player is
                        // keeping outside the item's own bag. Without this,
                        // picking up 5 ore with 6 already piled puts 4 into
                        // that pile and only the leftover into the bag — the
                        // feature appears to work while most of the haul still
                        // lands outside it. The pile stays exactly as the
                        // player left it (§4-1) — and that promise covers a
                        // pile parked in a GENERAL bag the same as one on the
                        // main board; only a pile already inside a bag of the
                        // item's own kind may absorb arrivals.
                        if (!g_typedBagsHeld.empty()) {
                            const auto& f = BagFilter::FilterOf(obj);
                            if (g_typedBagsHeld.contains(f) && !g_typedBagFull.contains(f)) {
                                bool intoOwnBag = false;
                                if (!s.le.bag.empty()) {
                                    const auto ba = g_bagAcceptByForm.find(BaseKey(s.le.bag));
                                    intoOwnBag = ba != g_bagAcceptByForm.end() &&
                                                 ba->second == f;
                                }
                                if (!intoOwnBag) continue;
                            }
                        }
                        const int room = cap - (std::max)(0, s.le.count);
                        if (room <= 0) continue;
                        const int add = (std::min)(room, diff);
                        s.le.count = (std::max)(0, s.le.count) + add;
                        diff -= add;
                    }
                    while (diff > 0) {
                        const int cnt = (std::min)(cap, diff);
                        Slot ns;
                        // ★★NextTileKey of the FORM. `base~SIG#n` is no longer
                        // born anywhere: the pool this tile belongs to lives in
                        // ns.le, where changing it costs a field write instead of
                        // a rename. (A signed STACK still gets its own tile --
                        // 1 stolen + 4 clean amethysts cannot share one -- but
                        // the tile is no longer NAMED after the split.)
                        ns.key = NextTileKey(baseKey);
                        ns.le.uid = UidOf(poolKey);
                        ns.le.sig = SigOf(poolKey);
                        // ★★A signed tile minted while the layout still holds
                        // plain keys for this form is the ONE-TIME migration off
                        // `base#n` -- the units were always in that pool, the
                        // keys just could not say so before 1.2.0. It is
                        // otherwise indistinguishable from a signature DRIFTING
                        // (a charge spent, a poison applied), which reshuffles a
                        // tile for real and is worth knowing about. Logged once
                        // per form so the two can be told apart in a report.
                        if (g_poolTrace && poolKey != baseKey) {
                            static std::set<std::string> s_said;
                            if (s_said.insert(poolKey).second) {
                                SKSE::log::info(
                                    "[POOL] first signed tile for '{}' -> '{}' "
                                    "(migration off base#n, or a signature drifted)",
                                    baseKey, ns.key);
                            }
                        }
                        ns.le.col = -1; ns.le.row = -1; ns.le.count = cnt;
                        // B2: partner-drop hint — the first NEW tile of this form
                        // lands at the drop cell (one-shot, then back to first-fit)
                        bool viaHint = false;
                        if (g_dropHint.Wants(baseKey, poolKey)) {
                            ns.le.col = g_dropHint.col;
                            ns.le.row = g_dropHint.row;
                            ns.le.bag = g_dropHint.bag;
                            ns.le.rot = g_dropHint.rot;   // GI62
                            g_dropHint = {};
                            viaHint = true;
                        }
                        g_layout[ns.key] = ns.le;   // reserve so NextTileKey advances
                        // ★Typed bags: the STACKABLE arrival point. Every one
                        // of the six filters (ore, ingredients, potions, soul
                        // gems, keys, hides) is stackable, so this — not the
                        // gear path above — is where their new tiles are born.
                        // Hooking only the gear path made the claim look
                        // completely dead: "0 fresh tiles" on every rebuild.
                        // ★A tile born from a drop HINT is the player's own
                        // hand choosing a cell — routing must not override
                        // that, so it never counts as fresh.
                        if (!viaHint) g_freshTiles.push_back(ns.key);
                        g_arrivedTiles.push_back(ns.key);   // (1.3.2) hint or not
                        slots.push_back(std::move(ns));
                        diff -= cnt;
                    }
                    // ★The hint is spent by the ARRIVAL, not only by a minted
                    // tile: when the fill loop absorbed everything into piles
                    // no tile was made, and a hint left armed here fires on the
                    // NEXT pickup of this form — teleporting it to a stale cell.
                    // ★...but only THIS pool's arrival spends it. A hint armed
                    // for a signed unit must survive the plain pool walking past
                    // with an acquisition of its own.
                    if (g_dropHint.Wants(baseKey, poolKey)) g_dropHint = {};
                } else if (diff < 0) {
                    // CONSUME: drain partial tiles first (bottom-right), then full
                    // ones; tiles PARKED in the trash (F2) drain dead last — an
                    // outside consumption must not eat the deletion buffer.
                    int deficit = -diff;
                    // The tile the player acted on gives up its unit FIRST (rule
                    // 2-B). Only then do the positional passes below run.
                    // ★SPENDING is a pool's business -- the hint names one key and
                    // that key lives in one pool, so the others must not throw it
                    // away on their way past. DISPOSAL is the form's, and happens
                    // once the pool loop is over (see below): a hint whose tile
                    // emptied, or whose pool never ran a deficit, would otherwise
                    // outlive the form entirely and be spent later by a brand-new
                    // tile that happened to inherit its key from NextTileKey.
                    if (deficit > 0 && g_drainHint.baseKey == baseKey) {
                        for (auto si : mine) {
                            auto& s = slots[si];
                            if (s.key != g_drainHint.key || s.le.count <= 0) continue;
                            const int take = (std::min)(s.le.count, deficit);
                            s.le.count -= take;
                            deficit -= take;
                            g_drainHint = {};
                            break;
                        }
                    }
                    // (explicit sells/stores never reach this drain: their tile's
                    // remembered quantity is decremented at confirm time in
                    // NotePendingRemove, so the reconciler sees no gap)
                    auto drain = [&](auto&& a_pick) {
                        for (auto it2 = mine.rbegin(); it2 != mine.rend() && deficit > 0; ++it2) {
                            auto& s = slots[*it2];
                            if (!a_pick(s)) continue;
                            const int take = (std::min)(s.le.count, deficit);
                            s.le.count -= take;
                            deficit -= take;
                        }
                    };
                    drain([&](const Slot& s) {
                        return s.le.bag != kTrashKey && s.le.count > 0 && s.le.count < cap;
                    });
                    drain([&](const Slot& s) { return s.le.bag != kTrashKey && s.le.count > 0; });
                    drain([&](const Slot& s) { return s.le.count > 0; });   // trash: last resort
                }
                }   // ---- end per-pool reconciliation ----

                // ★The hint dies with the FORM it named, spent or not. This is
                // the same ownership g_dropHint states one screen up ("the hint
                // is spent by the ARRIVAL"), and the old form-wide reconciler
                // had it for free by clearing at the end.
                if (g_drainHint.baseKey == baseKey) g_drainHint = {};

                // emit survivors; purge emptied tiles from the layout
                // ★Form-wide on purpose: the pools placed their own units, but
                // the board is one board and the conservation check below has to
                // see every tile of the form at once.
                for (auto& s : slots) {
                    if (s.le.count <= 0) { g_layout.erase(s.key); continue; }
                    g_layout[s.key].count = s.le.count;   // persist owned count now
                    makeTile(s.key, s.le.count, s.le);
                }

                // ---- self-check: STACKABLES ------------------------------------
                // This branch had no instrumentation at all, so a torch -- which
                // stacks, and therefore never reached the gear checks -- produced
                // no evidence whatsoever and had to be judged by eye.
                if (g_poolTrace) {
                    int drawn = 0;
                    std::string cells;
                    for (const auto& s : slots) {
                        if (s.le.count <= 0) continue;
                        drawn += s.le.count;
                        cells += std::format("{}x[{},{}] ", s.le.count, s.le.col, s.le.row);
                    }
                    const int wornN = wornUnits;
                    const int rsv = Loadout::ReservedCount(obj->GetFormID());
                    const int rm = Ledger::OpenOutgoingCount(obj->GetFormID());   // B4-3c
                    const int total = drawn + wornN + rsv + rm + carried;
                    if (total != count) {
                        SKSE::log::warn("[CHECK] {} STACK MISMATCH engine={} drawn={} "
                                        "worn={} reserved={} removing={} carried={} "
                                        "-> {} unaccounted",
                            baseKey, count, drawn, wornN, rsv, rm, carried,
                            count - total);
                    }
                    std::string sig = std::to_string(drawn) + " " + cells;
                    auto& prev = g_flickPrev[baseKey];
                    if (prev != sig) {
                        SKSE::log::info("[FLICK] {} stack {} -> {} (engine={} worn={} "
                                        "carried={})  {}",
                            baseKey, prev.empty() ? std::string("-")
                                                  : prev.substr(0, prev.find(' ')),
                            drawn, count, wornN, carried, sig);
                        prev = sig;
                    }
                }
            }

            // ---- ★S-G: coin tiles, straight out of the layout --------------
            // The slot is the book now: every coin tile is a (key, amount)
            // the player's gestures (or CoinIncome / CoinSpend) put there.
            // Drawn like the trash-parked tiles -- no inventory walk behind
            // them, the obj is the one coin form, the amount rides coinValue.
            if (auto* cform = GoldCoins::CoinForTier(0)) {
                const GridDef cdef = g_resolver ? g_resolver(cform) : GridDef{};
                for (const auto& [ck, cle] : g_layout) {
                    if (cle.coin < 0) continue;
                    if (g_held && ck == g_held->key) continue;   // cursor money
                    if (cle.bag == kTrashKey) continue;          // coins never park
                    Item cit;
                    cit.key = ck;
                    cit.obj = cform;
                    cit.count = 1;
                    cit.def = cdef;
                    cit.rot = 0;
                    cit.mask = MaskOf(cit.def);
                    cit.coinValue = cle.coin;
                    cit.col = cle.col;
                    cit.row = cle.row;
                    cit.inBag = cle.bag;
                    g_items.push_back(std::move(cit));
                }
            }
        }

        // stage 3: bag existence bookkeeping (E3/E4) — returns the map of
        // present bag tiles (key -> g_items index) for the view builder
        std::map<std::string, int> ReconcileBagBookkeeping()
        {
            // ---- bag bookkeeping ----
            // present bags (key -> index); stale open entries and orphaned inBag
            // assignments fall back gracefully (E4)
            std::map<std::string, int> bags;
            for (int i = 0; i < static_cast<int>(g_items.size()); ++i) {
                if (g_items[i].def.bag != 0) bags[g_items[i].key] = i;
            }
            for (auto it = g_openBags.begin(); it != g_openBags.end();) {
                // a carried bag is still in `bags` (it stays in g_items), so it
                // survives this prune on its own — the guard is belt and braces
                const bool carried = g_held && g_held->key == *it;
                if (!bags.contains(*it) && !carried) it = g_openBags.erase(it);
                else ++it;
            }
            for (auto& it : g_items) {
                if (it.inBag == kTrashKey) continue;   // F2: virtual bag, no tile
                if (!it.inBag.empty() && !bags.contains(it.inBag)) {
                    // A CARRIED bag isn't "gone" — keep its contents hidden inside
                    // it (inBag stays set -> excluded from main + no bag window),
                    // so they reflow to main only on a real drop/sell, not while
                    // the bag rides the cursor.
                    if (!(g_held && g_held->key == it.inBag)) {
                        // ★P2/3-1 instrumentation: THE moment contents spill.
                        // "The bag closed and everything fell out" has several
                        // possible causes -- the tile was re-keyed, the carry
                        // guard did not match, the bag really did leave -- and
                        // they are indistinguishable from outside. This names
                        // the bag key that went missing and what the cursor was
                        // holding at the time, which separates all three.
                        // ★★S-3-4: A LOST PARENT IS NOT A LOST HOME. With
                        // nesting, the bag that vanished may itself have been
                        // sitting inside another one -- and dumping its
                        // contents on the main board would empty a satchel out
                        // of the backpack it was in, which is not what losing
                        // the satchel means. Walk up instead: the nearest
                        // ancestor still on the board takes them, and only a
                        // chain that is gone all the way to the top reflows to
                        // main (E4). Bounded, because a corrupt save must not
                        // be able to spin this.
                        std::string up;
                        std::string probe = it.inBag;
                        for (int hop = 0; hop < 16; ++hop) {
                            const auto li = g_layout.find(probe);
                            if (li == g_layout.end() || li->second.bag.empty()) break;
                            probe = li->second.bag;
                            if (probe == kTrashKey) break;
                            if (bags.contains(probe)) { up = probe; break; }
                        }
                        SKSE::log::info("[BAG] '{}' left the board -- '{}' reflows "
                                        "to {} (held '{}')",
                            it.inBag, it.key,
                            up.empty() ? std::string("main") : "'" + up + "'",
                            g_held ? g_held->key : std::string("-"));
                        it.inBag = up;   // bag truly gone: contents reflow (E4)
                    }
                }
            }
            return bags;
        }

        // ★The bags that can actually hold something, derived from the board —
        //  never from `g_openBags`, which only says which bag WINDOWS are up.
        //  Every capacity question (the real placement, the accept-probe, the
        //  overload check, the take-all budget) goes through this one list, so
        //  a verdict of "it fits" can never disagree with where the item then
        //  lands. Key-sorted: the sims and the placement must walk candidate
        //  bags in the same order, or the same loot picks a different bag.
        struct BagSlot
        {
            std::string key;
            int         cols = 1;
            int         rows = 1;
            std::string accept;   // "" = general purpose (takes overflow only)
            int         col = -1;   // the bag TILE's own cell, for fill order
            int         row = -1;
            // NOTE deliberately no `carried` flag: a bag riding the cursor
            // never enters this list at all (see the g_held skip below) — the
            // held bag's window is a separate View built in BuildViewsAndSpill.
        };

        std::vector<BagSlot> CollectBagSlots(const std::vector<Item>& a_tiles)
        {
            std::vector<BagSlot> out;
            for (const auto& it : a_tiles) {
                if (it.def.bag == 0) continue;               // not a bag
                if (it.key == kTrashKey) continue;           // F2: never a target
                if (it.inBag == kTrashKey) continue;         // parked for deletion
                if (g_held && g_held->key == it.key) continue;   // riding the cursor
                out.push_back({ it.key, (std::max)(1, it.def.bw),
                                        (std::max)(1, it.def.bh),
                                it.def.accept, it.col, it.row });
            }
            // ★Fill order is the bag's own place on the board, top-left first,
            // so two bags of the same kind fill in the order the player sees
            // them. Key order (the old rule) is a FormID string — stable, but
            // it means the second satchel can fill before the first for no
            // reason the player can observe. Key breaks ties so an unplaced bag
            // still lands somewhere deterministic.
            std::sort(out.begin(), out.end(), [](const BagSlot& a, const BagSlot& b) {
                const int ar = a.row < 0 ? 9999 : a.row;
                const int br = b.row < 0 ? 9999 : b.row;
                if (ar != br) return ar < br;
                const int ac = a.col < 0 ? 9999 : a.col;
                const int bc = b.col < 0 ? 9999 : b.col;
                if (ac != bc) return ac < bc;
                return a.key < b.key;
            });
            return out;
        }

        // ★Typed bags, the claim (PLAN_TYPED_BAGS §3-2). Runs on tiles that
        // were minted THIS rebuild and nothing else: routing decides where a
        // new item lands, it does not hold items in place afterwards. Dragging
        // an ore back to the main board therefore sticks, which is what makes
        // rearranging a bag possible at all.
        // ★"모으기" (PLAN_TYPED_BAGS §4-1a). Routing only ever decides where a
        // NEW item lands; it deliberately never drags placed items around, or
        // rearranging a bag would be impossible. That leaves one real gap — a
        // bag you just acquired starts empty, and anything scattered while
        // tidying stays scattered. This is the answer to both, and it is a
        // BUTTON rather than an automatic sweep so nothing ever moves without
        // the player asking.
        //
        // Marks only; the normal placement pass seats them and bounces what
        // does not fit back to main, so a full bag partially collects for free.
        int CollectIntoBag(const std::string& a_bagKey, const std::string& a_accept)
        {
            if (a_bagKey.empty() || a_accept.empty()) return 0;
            auto* p = RE::PlayerCharacter::GetSingleton();
            int moved = 0;
            for (auto& it : g_items) {
                if (!it.obj) continue;
                if (it.inBag == a_bagKey) continue;      // already home
                if (it.inBag == kTrashKey) continue;     // queued for deletion
                if (it.def.bag) continue;                // E4: no bag inside a bag
                if (it.coinValue >= 0) continue;         // coins answer to the ledger
                if (g_held && g_held->key == it.key) continue;   // riding the cursor
                // ONLY from main and general-purpose bags. Pulling out of
                // another TYPED bag would let two bags fight over the same item
                // every time either button is pressed.
                if (!it.inBag.empty()) {
                    const auto src = std::find_if(g_views.begin(), g_views.end(),
                        [&](const View& v) { return v.bagKey == it.inBag; });
                    if (src != g_views.end() && !src->accept.empty()) continue;
                }
                if (BagFilter::FilterOf(it.obj) != a_accept) continue;
                // entry-level quest check, same deliberate choice as the claim:
                // one flagged unit keeps the whole (interchangeable) form out
                if (p) {
                    if (auto* e = LiveEntry(p, it.obj); e && e->IsQuestObject()) continue;
                }
                it.inBag = a_bagKey;
                it.col = -1;
                it.row = -1;
                auto& le = g_layout[it.key];
                le.bag = a_bagKey;
                le.col = -1;
                le.row = -1;
                ++moved;
            }
            if (moved > 0) {
                SKSE::log::info("[BAGCLAIM] collect '{}': {} tile(s)", a_accept, moved);
                RequestRebuild();
            }
            return moved;
        }

        // ★(1.3.0-D) a bag that just walked back in claims its bundled
        // contents: fresh tiles matching the manifest move INTO the bag.
        // Runs BEFORE the typed-bag claim so the bundle outranks filters.
        // A stack that merged into a pre-existing loose tile minted no
        // fresh tile and stays on the main board -- lossless, just not
        // re-bagged (the typed claim may still route it later).
        void ClaimIncomingBundles()
        {
            if (g_arrivedTiles.empty()) return;
            for (const auto& bagKey : g_arrivedTiles) {
                auto bt = std::find_if(g_items.begin(), g_items.end(),
                    [&](const Item& t) { return t.key == bagKey; });
                if (bt == g_items.end() || !bt->obj || bt->def.bag == 0) continue;
                const auto manifest =
                    LootBarter::TakeIncomingBundle(bt->obj->GetFormID());
                if (manifest.empty()) continue;
                // ★★S-3-4: the tile each manifest entry ended up as, keyed by
                // the entry's NAME, so a NESTED bag's own contents route into it
                // rather than into the bag that was stored. Entries are written
                // parent first, so by the time a child is read its parent's tile
                // is already known -- one forward pass, no sorting.
                std::map<std::uint32_t, std::string> tileOf;
                for (std::size_t mi = 0; mi < manifest.size(); ++mi) {
                    const auto& b = manifest[mi];
                    // which bag this entry belongs in: the stored bag, or a
                    // nested one that came home earlier in this same pass. A
                    // parent whose tile never arrived leaves its children
                    // loose on the board rather than guessing at a home.
                    const std::string into =
                        b.parent == 0 ? bagKey : tileOf[b.parent];
                    if (into.empty()) continue;
                    // ★(1.5.x) a GOLD entry has no engine tile to claim: the
                    // Septims merged into the ledger on arrival (announced by
                    // ExpectIncoming at the take, so no income mint doubled
                    // them). Mint its coin record straight into the bag at
                    // the anchor it was stored with.
                    if (auto* vg = GoldCoins::VanillaGold();
                        vg && b.form == vg->GetFormID()) {
                        if (b.count > 0) {
                            if (auto* cf = GoldCoins::CoinForTier(0)) {
                                // capfuls, the income mint's own granularity;
                                // the FIRST tile takes the stored anchor
                                int  left = b.count;
                                bool anchor = b.col >= 0 && b.row >= 0;
                                while (left > 0) {
                                    const int n =
                                        (std::min)(GoldCoins::kCoinCap, left);
                                    const std::string ck =
                                        NextTileKey(FormKey(cf));
                                    auto& le = g_layout[ck];
                                    le.bag = into;
                                    le.col = anchor ? b.col : -1;
                                    le.row = anchor ? b.row : -1;
                                    le.rot = anchor ? (b.rot & 3) : 0;
                                    le.coin = n;
                                    le.count = 1;
                                    anchor = false;
                                    left -= n;
                                }
                                MarkCapacityDirty();
                                ++g_boardVersion;
                                RequestRebuild();
                                SKSE::log::info(
                                    "[BAGCLAIM] {} G -> '{}' (gold entry)",
                                    b.count, into);
                            }
                        }
                        continue;
                    }
                    int remaining = b.count;
                    // ★★AND THE ANCHOR COMES HOME WITH IT. The manifest has
                    // carried col/row since v5 and this routed the contents
                    // into the bag without ever reading them, so a bag that
                    // came back out of a chest first-fit its own insides into
                    // a stranger's order (reported). The FIRST tile claimed for
                    // an entry takes the anchor -- an entry was one tile when it
                    // was stored, and if it comes back split, the extras have no
                    // remembered place of their own and first-fit as before.
                    bool anchorFree = b.col >= 0 && b.row >= 0;
                    // two passes: the matching sub-stack first, then any unit
                    for (int pass = 0; pass < 2 && remaining > 0; ++pass) {
                        for (const auto& key : g_arrivedTiles) {
                            if (remaining <= 0) break;
                            if (key == bagKey) continue;
                            auto it = std::find_if(g_items.begin(), g_items.end(),
                                [&](const Item& t) { return t.key == key; });
                            if (it == g_items.end() || !it->obj) continue;
                            if (it->obj->GetFormID() != b.form) continue;
                            // ★A nested BAG is claimable now -- it is an entry
                            // like any other. It was excluded when a bundle
                            // could not contain one.
                            if (!it->inBag.empty()) continue;
                            if (pass == 0 && it->sig != b.sig) continue;
                            it->inBag = into;
                            auto& le = g_layout[it->key];
                            le.bag = into;
                            if (tileOf[b.id].empty()) tileOf[b.id] = it->key;
                            // ★(1.5.x) a POUCH entry's parked amount goes to
                            // exactly THIS tile -- a wallet that travelled
                            // keeps its own money (parcel matched by form +
                            // amount; a miss falls back to the generic pass)
                            if (b.gold > 0 &&
                                GoldCoins::IsPouch(b.form)) {
                                GoldCoins::ClaimParcelForTile(it->key, b.form,
                                                              b.gold);
                            }
                            if (anchorFree) {
                                // ★★THE TILE, NOT ONLY THE LAYOUT. Writing the
                                // layout alone was too late to matter: Item::col
                                // is seeded from g_layout in an EARLIER stage,
                                // the view placement runs off the tile ("already
                                // placed? leave it alone"), and its first-fit
                                // result is then written BACK over the layout.
                                // So the anchor was recorded, ignored, and
                                // overwritten in the same pass -- the bag still
                                // came home shuffled. Both, or neither.
                                le.col = b.col;
                                le.row = b.row;
                                le.rot = b.rot & 3;
                                it->col = b.col;
                                it->row = b.row;
                                anchorFree = false;
                            }
                            remaining -= it->count;
                            SKSE::log::info("[BAGCLAIM] bundle: '{}' x{} -> '{}'",
                                it->obj->GetName() ? it->obj->GetName() : "?",
                                it->count, into);
                        }
                    }
                }
            }
        }

        void ClaimIntoTypedBags(const std::vector<BagSlot>& a_slots)
        {
            std::vector<const BagSlot*> typed;
            for (const auto& s : a_slots) {
                // a bag the player is holding never enters a_slots at all
                // (CollectBagSlots skips it), so everything here has a place
                // on the board and is safe to route into
                if (!s.accept.empty()) typed.push_back(&s);
            }
            if (g_freshTiles.empty() || typed.empty()) return;

            auto* p = RE::PlayerCharacter::GetSingleton();
            for (const auto& key : g_freshTiles) {
                auto it = std::find_if(g_items.begin(), g_items.end(),
                    [&](const Item& t) { return t.key == key; });
                if (it == g_items.end() || !it->obj) continue;
                // passing through on its way to being used -- see the note on
                // g_transientArrivals
                if (g_transientArrivals.contains(it->obj->GetFormID())) continue;
                if (!it->inBag.empty()) continue;      // already spoken for
                if (it->def.bag) continue;             // E4: no bag inside a bag
                if (it->coinValue >= 0) continue;      // coins answer to the ledger

                // Quest items stay on the main board. Hiding one inside a bag
                // makes it hard to find, and it cannot be dropped or sold, so
                // the bag buys the player nothing (PLAN §4-4).
                // ENTRY-level IsQuestObject on purpose (elsewhere quest state is
                // per sub-stack): stackable units are interchangeable, so when
                // ANY unit is quest-flagged the only safe call covers the form.
                if (p) {
                    if (auto* entry = LiveEntry(p, it->obj); entry && entry->IsQuestObject()) {
                        continue;
                    }
                }

                const auto& filter = BagFilter::FilterOf(it->obj);
                if (filter.empty()) continue;
                for (const auto* s : typed) {
                    if (s->accept != filter) continue;
                    if (s->key == it->key) continue;
                    it->inBag = s->key;
                    g_layout[it->key].bag = s->key;
                    SKSE::log::info("[BAGCLAIM] {} -> {} ({})",
                        it->obj->GetName() ? it->obj->GetName() : "?", s->key, filter);
                    // First bag of that kind in board order. If it turns out to
                    // be full, the placement pass hands the tile to the next one
                    // (see BuildViewsAndSpill) — capacity is not knowable here,
                    // so the fall-through lives where the seating happens.
                    break;
                }
            }
        }

        // ★S-G: MakePaidGoldDummies is gone. A payment debits NAMED coin
        // tiles now (CoinSpend), so there is no dissolved-cells guess for the
        // spill pass to re-fill -- the cells a payment frees really are free.

        // stage 4: bag views -> main list -> overflow spill into open bags
        // -> main view (placement is FINAL here)
        void BuildViewsAndSpill(std::map<std::string, int>& bags)
        {
            // ---- views: EVERY present bag (their overflow falls back to main).
            //      Closed ones get the same placement pass and are simply not
            //      drawn — see the View::open comment.
            std::vector<Item*> mainList;
            const auto slots = CollectBagSlots(g_items);
            // BEFORE the views place anything: the claim only sets inBag, and
            // the existing per-view pass below is what actually seats the tile
            // — including bouncing it back to main when the bag is full, which
            // is exactly decision 1 and needs no code of its own.
            ClaimIncomingBundles();   // (1.3.0-D) the bundle outranks the filter
            ClaimIntoTypedBags(slots);
            // re-derive "which typed bags are full" from THIS pass's placement;
            // the stackable fill loop read the previous pass's answer earlier
            g_typedBagFull.clear();
            // ★Fullness is per BAG, not per filter. Two ore bags are two
            // shelves: one being full says nothing about the other, and the
            // filter-level flag made a full first bag send everything to the
            // main pile while the second sat empty.
            std::set<std::string> bagFull;
            for (std::size_t si = 0; si < slots.size(); ++si) {
                const auto& slot = slots[si];
                const auto bi = bags.find(slot.key);
                if (bi == bags.end()) continue;
                const auto& bagItem = g_items[bi->second];
                View v;
                v.bagKey = slot.key;
                v.bagName = bagItem.obj ? bagItem.obj->GetName() : "";
                v.accept = slot.accept;
                v.cols = slot.cols;
                v.minRows = slot.rows;
                v.maxRows = v.minRows;   // fixed-height grid (B1/E5)
                v.open = g_openBags.contains(slot.key);
                std::vector<Item*> list;
                for (int i = 0; i < static_cast<int>(g_items.size()); ++i) {
                    if (g_items[i].inBag == slot.key) list.push_back(&g_items[i]);
                }
                v.rows = PlaceItems(list, v.cols, v.minRows, v.maxRows);
                for (auto* it : list) {
                    if (it->overflow) {   // bag full/shrunk: falls back to main (E4)
                        bagFull.insert(slot.key);
                        it->col = -1;
                        it->row = -1;
                        // ★Try the NEXT bag of the same kind before giving up on
                        // bags entirely (decision 4 promised board order, and
                        // order without fall-through means the second bag is
                        // never used). Slots are already in board order and
                        // later ones are built after this one, so handing the
                        // tile forward is enough — that view collects by inBag.
                        std::string next;
                        if (!slot.accept.empty()) {
                            for (std::size_t sj = si + 1; sj < slots.size(); ++sj) {
                                if (slots[sj].accept == slot.accept &&
                                    bags.contains(slots[sj].key)) {
                                    next = slots[sj].key;
                                    break;
                                }
                            }
                        }
                        it->inBag = next;   // "" = fall back to the main board
                        if (!next.empty()) g_layout[it->key].bag = next;
                        else               g_layout[it->key].bag.clear();
                    } else {
                        v.items.push_back(static_cast<int>(it - g_items.data()));
                    }
                }
                g_views.push_back(std::move(v));
            }

            // ★A bag on the cursor keeps its window open (user report: picking
            // up an open bag to move it closed the window and hid its contents,
            // which reads as "moving a bag empties it").
            //
            // It needs a view of its own because the carried tile is NOT in
            // g_items at all — the display collector excludes the carried unit,
            // so every earlier attempt to flag it while walking the tiles was
            // marking something that was never there. Its CONTENTS are still
            // in g_items with inBag set, which is what makes this possible.
            if (g_held && g_held->isBag && g_openBags.contains(g_held->key)) {
                const GridDef hd = g_resolver ? g_resolver(g_held->obj) : GridDef{};
                View v;
                v.bagKey = g_held->key;
                v.bagName = g_held->obj ? g_held->obj->GetName() : "";
                v.accept = hd.accept;
                v.carried = true;   // nothing routes into a bag with no place yet
                v.cols = (std::max)(1, hd.bw);
                v.minRows = (std::max)(1, hd.bh);
                v.maxRows = v.minRows;
                v.open = true;
                std::vector<Item*> list;
                for (int i = 0; i < static_cast<int>(g_items.size()); ++i) {
                    if (g_items[i].inBag == v.bagKey) list.push_back(&g_items[i]);
                }
                v.rows = PlaceItems(list, v.cols, v.minRows, v.maxRows);
                for (auto* it : list) {
                    if (it->overflow) continue;   // stays hidden; the drop reflows it
                    v.items.push_back(static_cast<int>(it - g_items.data()));
                }
                g_views.push_back(std::move(v));
            }

            // A filter only counts as full when EVERY bag that accepts it is.
            // Until then the stackable path must keep starting fresh tiles, or
            // arrivals would merge into the main pile with space still on the
            // second shelf.
            {
                std::map<std::string, std::pair<int, int>> perAccept;   // total, full
                for (const auto& slot : slots) {
                    if (slot.accept.empty() || !bags.contains(slot.key)) continue;
                    auto& t = perAccept[slot.accept];
                    ++t.first;
                    if (bagFull.contains(slot.key)) ++t.second;
                }
                for (const auto& [acc, t] : perAccept) {
                    if (t.first > 0 && t.first == t.second) g_typedBagFull.insert(acc);
                }
            }

            // F2: the trash is one more grid view — a fixed 6x4 virtual bag.
            // Parked tiles were assigned bag == kTrashKey at drop time.
            if (g_trashOpen) {
                View v;
                v.bagKey = kTrashKey;
                v.bagName = Lang::T(Lang::Str::TrashTitle);
                v.cols = kTrashCols;
                v.minRows = kTrashRows;
                v.maxRows = kTrashRows;
                std::vector<Item*> list;
                for (auto& it : g_items) {
                    if (it.inBag == kTrashKey) list.push_back(&it);
                }
                v.rows = PlaceItems(list, v.cols, v.minRows, v.maxRows);
                for (auto* it : list) {
                    if (it->overflow) {   // shouldn't happen (intake evicts) — reflow
                        it->col = -1;
                        it->row = -1;
                        it->inBag.clear();
                    } else {
                        v.items.push_back(static_cast<int>(it - g_items.data()));
                    }
                }
                g_views.push_back(std::move(v));
            }

            // main list: unassigned items + bag-overflow fallbacks. Contents of
            // CLOSED bags are fully hidden (E3) — they keep their entries.
            for (auto& it : g_items) {
                if (it.inBag.empty()) mainList.push_back(&it);
            }

            // ---- B: spill main-overflow items into the open bag views ----
            // The placement here is FINAL — committed straight into a bag view — so
            // the "does it fit a bag?" verdict can't disagree with a later
            // re-placement (the old split pre-assigned inBag, then the view loop
            // re-placed and bounced it back to main). Fresh buys/loot drain into
            // bag space; coins (gold ledger) and bag items (no nesting) never spill.
            const bool anyBag = std::any_of(g_views.begin(), g_views.end(),
                [](const View& v) { return !v.bagKey.empty() && v.bagKey != kTrashKey; });
            if (anyBag) {
                std::vector<Item*> probe;
                probe.reserve(mainList.size());
                for (auto* it : mainList) probe.push_back(it);
                PlaceItems(probe, kCols, kMinRows, kMinRows,
                           g_cwBonusCells);   // hard board + CW bonus (W3)
                for (auto* cand : probe) {
                    // real items only (dummies have no obj); coins keep the ledger
                    if (!(cand->obj && cand->overflow && cand->coinValue < 0)) continue;
                    // E4: a bag never auto-nests. The sims already refuse this
                    // (ComputeOverloaded checks def.bag, MaxAcceptUnits gates on
                    // it) — this pass silently allowed it, which was the one
                    // door out of three that disagreed (rule 97). Nesting is a
                    // MANUAL act: the player drops a bag into a general bag.
                    if (cand->def.bag != 0) continue;
                    // ★Open shelves first. Which bag takes the overflow does not
                    // change WHETHER it fits (same acceptance set, so the sims
                    // agree either way) — but an item the player can see land
                    // beats one that vanishes into a closed bag. Only when no
                    // open bag has room does it go somewhere closed, and that
                    // bag's tile is then marked NEW so the board says where.
                    bool placed = false;
                    for (int pass = 0; pass < 2 && !placed; ++pass) {
                        for (auto& v : g_views) {   // g_views holds only bag views here
                            if (v.bagKey.empty()) continue;
                            if (v.bagKey == kTrashKey) continue;   // F2: never spill INTO the trash
                            if (v.carried) continue;   // it is on the cursor, not on the board
                            if ((pass == 0) != v.open) continue;   // pass 0 = open bags
                            // ★A typed bag is not overflow space. Without this a
                            // general spill drops a sword into the ore bag the
                            // moment the board is full, and the bag stops meaning
                            // what its name says.
                            if (!v.accept.empty() &&
                                v.accept != BagFilter::FilterOf(cand->obj)) {
                                continue;
                            }
                            std::vector<Item*> test;
                            test.reserve(v.items.size() + 1);
                            for (int idx : v.items) test.push_back(&g_items[idx]);
                            cand->col = -1;
                            cand->row = -1;
                            test.push_back(cand);
                            const int rows = PlaceItems(test, v.cols, v.minRows, v.maxRows);
                            if (!cand->overflow) {
                                cand->inBag = v.bagKey;
                                v.items.push_back(static_cast<int>(cand - g_items.data()));
                                v.rows = rows;
                                // arrival into a CLOSED bag is invisible — light
                                // the bag's own tile (the NEW wash: clears on
                                // hover, exactly the "look in here" it means)
                                if (!v.open) g_newTiles.insert(v.bagKey);
                                placed = true;
                                break;   // committed into this bag
                            }
                        }
                    }
                }
                mainList.erase(std::remove_if(mainList.begin(), mainList.end(),
                    [](Item* it) { return !it->inBag.empty(); }), mainList.end());
            }

            View main;
            main.bagKey.clear();
            std::vector<Item*> mainPtrs = mainList;
            main.rows = PlaceItems(mainPtrs, kCols, kMinRows, 4096);
            // ★W3: a fresh unlock is a place to drop into, so the owned
            // region always shows even while empty
            main.rows = (std::max)(main.rows, OwnedRowSpan());
            for (auto* it : mainPtrs) {
                if (it->overflow || it->col < 0) continue;
                main.items.push_back(static_cast<int>(it - g_items.data()));
            }
            g_views.insert(g_views.begin(), std::move(main));
        }

        // GI1: an instance tile's key IS the engine's uniqueID, and the engine is
        // free to reassign that when an item changes container. If it does, every
        // move strands the old placement and g_layout -- which is serialised into
        // every single save -- grows for the rest of the playthrough.
        //
        // Keep a small stale budget per FORM, so the ordinary round trip (put the
        // sword in a chest, take it back, land on its own cell again) still works
        // and only genuine churn is collected. Equipped items are "stale" by this
        // test too, which is correct: one entry each, well inside the budget.
        //
        // The log line here is also the only DIRECT measurement of whether the
        // engine churns uniqueIDs at all (Phase 0 gate E7) -- if it never fires
        // across a playthrough, it doesn't.
        constexpr std::size_t kStaleInstanceBudget = 8;

        void PruneStaleInstanceLayouts()
        {
            std::unordered_map<std::string, std::vector<std::string>> staleByForm;
            for (const auto& [k, le] : g_layout) {
                if (!IsInstanceKey(k)) continue;        // ordinal tiles: untouched
                if (g_prevKeys.contains(k)) continue;   // on the board right now
                if (le.bag == kTrashKey) continue;      // parked for deletion, still ours
                staleByForm[BaseKey(k)].push_back(k);
            }
            int pruned = 0;
            for (auto& [form, keys] : staleByForm) {
                if (keys.size() <= kStaleInstanceBudget) continue;
                std::sort(keys.begin(), keys.end());   // deterministic: by uid hex
                for (std::size_t i = 0; i + kStaleInstanceBudget < keys.size(); ++i) {
                    g_layout.erase(keys[i]);
                    ++pruned;
                }
            }
            if (pruned > 0) {
                SKSE::log::info("[GRID] GI1: pruned {} stale instance placements "
                                "(uniqueID churn — see gate E7)", pruned);
            }
        }

        // stage 5: persist placements (cosave source of truth), prevKeys,
        // lazy icon captures, occupancy stat
        void FinalizeRebuild()
        {

            // ---- remember fresh placements (B4) — in-memory; cosave persists ----
            for (const auto& v : g_views) {
                for (int idx : v.items) {
                    const Item* itP = ItemAt(idx);   // guarded: see ItemAt
                    if (!itP) continue;
                    const auto& it = *itP;
                    auto& le = g_layout[it.key];
                    le.col = it.col;
                    le.row = it.row;
                    le.bag = it.inBag;
                    le.count = it.count;   // G4: keep owned count in sync with placement
                    le.rot = it.rot;       // GI62 (pass 2 may have stood it back up)
                    // ★The slot's coin amount too: a freshly minted auto tile
                    // (emitCoin with no pos) had no layout entry to stamp, so
                    // its slot said -1 until the NEXT partition -- one rebuild
                    // during which the spend allocator could not see it.
                    // -1 for every non-coin tile, which is the field's default.
                    le.coin = it.coinValue;
                }
            }

            // An applied entry has covered the one rebuild it existed for.
            // ★...and if any actually died here, this rebuild was still drawn
            // WITH their suppression -- one more pass shows the board as the
            // engine now has it. ProcessPending's unconditional rebuild used
            // to be that pass (removed in B3-c); worn equips still get theirs
            // released mid-walk by ReleaseWornPendingEquips, but a CONSUMABLE
            // never grows a worn list and never fires an unequip event, so its
            // entry could only die here -- and the board then stood one unit
            // short until the next unrelated rebuild: "one drink removed two"
            // (user report). The request coalesces; steady state stays quiet.
            // ★Ring session: NON-CONFIRMABLE entries only. Gear entries have
            // an equip event coming (landed-release, match-release, settle
            // sweep); erasing them here on the `applied` flag -- which flips
            // when OUR CALL returns, not when the engine wears the unit --
            // scheduled a rebuild per rebuild while a swap run was in flight
            // (measured: 11/s, alternating with the displaced-lift redraw).
            if (std::erase_if(g_pendingEquip,
                              [](const OffBoardUnit& u) {
                                  return u.applied && !u.confirmable;
                              }) > 0) {
                RequestRebuild();
            }

            // ---- GI65: mark tiles that are new since the last look ----------
            // Runs BEFORE prevKeys is rebuilt, because "did this key exist last
            // time" is the question. Gold is skipped: coin tiles are a mirror of
            // the ledger and split and merge on their own as you spend, so they
            // would light up constantly while meaning nothing.
            if (g_seenValid && !g_suppressNew) {
                std::unordered_map<RE::FormID, int> live;
                for (const auto& it : g_items) {
                    if (it.obj) live[it.obj->GetFormID()] += it.count;
                }
                for (const auto& v : g_views) {
                    for (int idx : v.items) {
                        const Item* itP = ItemAt(idx);   // guarded: see ItemAt
                        if (!itP) continue;
                        const auto& it = *itP;
                        if (!it.obj || g_prevKeys.contains(it.key)) continue;
                        const RE::FormID fid = it.obj->GetFormID();
                        if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) continue;
                        const auto seen = g_seenCount.find(fid);
                        const int had = seen == g_seenCount.end() ? 0 : seen->second;
                        if (live[fid] > had) g_newTiles.insert(it.key);
                    }
                }
            }
            g_suppressNew = false;

            g_prevKeys.clear();
            for (const auto& v : g_views) {
                for (int idx : v.items) g_prevKeys.insert(g_items[idx].key);
            }
            // a tile that left takes its mark with it
            std::erase_if(g_newTiles, [](const std::string& k) {
                return !g_prevKeys.contains(k);
            });

            PruneStaleInstanceLayouts();   // GI1

            for (const auto& it : g_items) {
                IconCache::GetSingleton()->QueueCapture(it.obj);
            }

            // S2: cells occupied / available across the whole carry — the main
            // board PLUS every bag the player owns (open or closed: a closed
            // bag still holds its contents, so leaving it out of the total made
            // the stats panel under-report the pack, user report). The trash is
            // excluded from both halves: it is a deletion queue, not storage.
            // Growth rows are included in `used`, so it can exceed the total
            // while overloaded — e.g. 147 / 140.
            g_spaceUsed = 0;
            g_spaceTotal = kCols * kMinRows + g_cwBonusCells;   // W3
            for (const auto& v : g_views) {
                if (v.bagKey == kTrashKey) continue;
                // ★Typed bags are excluded from BOTH halves. Their cells cannot
                // hold general loot, so counting them as free space answers the
                // question "can I pick more up" with a yes that is wrong — and
                // the overload verdict reads the same total, so a full board
                // would report itself fine while an empty ore bag padded the
                // number. Their contents leave `used` for the same reason: what
                // is not in the total must not be in the tally either, or the
                // panel drifts toward used > total for no visible cause.
                // Deliberately NOT shown anywhere else: a typed bag answers for
                // its own space inside its own window.
                if (!v.accept.empty()) continue;
                for (int idx : v.items) g_spaceUsed += MaskCells(g_items[idx].mask.rows);
                if (!v.bagKey.empty()) g_spaceTotal += v.cols * v.minRows;
            }

            // ★Now that the pouch tiles are known, hand over any gold that
            // was waiting without one -- a pre-1.3.0 save, or a pouch that
            // has just walked back into the inventory. (1.3.0: the helper
            // splits fresh tiles from known ones so the returner claims.)
            ClaimIncomingPouchGold();
            // ★Built from the FINISHED board. It used to be filled at ONE of
            // the two places a tile enters g_items, and the one it missed is
            // the coin/purse path -- so every gold tile failed the draw-time
            // membership test and was skipped, silently, for as long as the
            // test has existed. Deriving it from the finished list is the only
            // shape a future entry path cannot forget to feed.
            g_liveObjs.clear();
            for (const auto& liveIt : g_items) g_liveObjs.insert(liveIt.obj);
            SKSE::log::info("[GRID] rebuilt: {} items, {} views, gold {}",
                g_items.size(), g_views.size(), g_gold);
            // ★[FAV] tripwire (state and rationale at g_starMemo): diff the
            // starred forms against the last rebuild. Only forms still in
            // the inventory (g_values was just filled by this rebuild) can
            // accuse anyone -- a star that left WITH its item is rule 58's
            // ordinary business and its exit already filed a witness.
            {
                std::unordered_set<RE::FormID> now;
                auto* wp = RE::PlayerCharacter::GetSingleton();
                if (auto* ch = wp ? wp->GetInventoryChanges() : nullptr;
                    ch && ch->entryList) {
                    for (auto* e : *ch->entryList) {
                        if (!e || !e->object || !e->extraLists) continue;
                        for (auto* xl : *e->extraLists) {
                            if (xl && xl->HasType<RE::ExtraHotkey>()) {
                                now.insert(e->object->GetFormID());
                                break;
                            }
                        }
                    }
                }
                if (g_starMemoValid) {
                    for (const auto f : g_starMemo) {
                        if (now.contains(f) || g_starChangeOk.contains(f) ||
                            !g_values.contains(f)) {
                            continue;
                        }
                        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(f);
                        SKSE::log::warn(
                            "[FAV] ★star vanished off {:08X} '{}' with no toggle"
                            " and no exit -- outside witness (engine charge"
                            " writeback?)",
                            f, obj ? obj->GetName() : "?");
                    }
                }
                g_starMemo      = std::move(now);
                g_starMemoValid = true;
                g_starChangeOk.clear();
            }
            // ★B5: rule 13's missed enforcement -- the sweep for forms this
            // walk never visited. A form whose entire presence is WORN has
            // zero board units, so the walk skips it and the ordinary prune
            // never reaches its slots: a layout entry leaked by an equip that
            // bypassed ForgetTile (the engine's own conflict equips, a
            // loadout apply, a costume pass) survived for good. The
            // engine-walk sims never saw them; the board reader seated them
            // as phantom occupancy -- five of them refused pickups with room
            // in plain sight (measured: stranded lines with valid, colliding
            // coordinates). Runs at the END of the rebuild, against the
            // freshly collected board. Every keeper is named: displayed
            // forms belong to the ordinary prune, reserved cells are MEMORY
            // by design, the trash is parked, a queued drop is two-phase,
            // and a held or pending-equip cell is in transit. What remains
            // is a worn leak -- or an unowned ghost, the save-bloat class --
            // and both go.
            {
                std::set<std::string> keepBases;
                for (const auto& it : g_items) keepBases.insert(BaseKey(it.key));
                for (const auto& u : g_pendingEquip) keepBases.insert(u.base);
                if (g_held && g_held->obj) keepBases.insert(FormKey(g_held->obj));
                std::set<std::string> queuedKeys;
                for (const auto& [f, keys] : g_pendingSlotDrop) {
                    queuedKeys.insert(keys.begin(), keys.end());
                }
                for (auto li = g_layout.begin(); li != g_layout.end();) {
                    const std::string base = BaseKey(li->first);
                    bool keep = keepBases.contains(base) ||
                                li->second.bag == kTrashKey ||
                                queuedKeys.contains(li->first) ||
                                (g_held && li->first == g_held->key) ||
                                li->second.coin >= 0;   // ★S-G: coin records are live tiles
                    if (!keep) {
                        if (auto* obj = ObjFromBaseKey(base)) {
                            if (Loadout::ReservedCount(obj->GetFormID()) > 0) {
                                keep = true;
                            }
                        }
                    }
                    if (keep) {
                        ++li;
                    } else {
                        SKSE::log::info("[GRID] layout sweep: '{}' (unwalked "
                                        "form) -- rule 13 leak or ghost",
                                        li->first);
                        li = g_layout.erase(li);
                    }
                }
            }
            // ★DIAG: a tile with no column, or parked in the overflow zone, is
            // skipped by the occupancy shading AND by the sprite pass -- so
            // "the background is missing" and "the cell looks empty" are the
            // same state seen from two distances. Name them; a count alone
            // would not say WHICH item, and the report is about one item.
            for (const auto& it : g_items) {
                if (it.col >= 0 && !it.overflow) continue;
                SKSE::log::info("[GRID]   unplaced: '{}' col={} row={} overflow={} "
                                "bag='{}' {}x{} count={}",
                    it.obj ? it.obj->GetName() : "?", it.col, it.row,
                    it.overflow ? 1 : 0, it.inBag, it.mask.w, it.mask.h, it.count);
            }
            g_capacityDirty = true;   // occupancy changed (W2)
        }
    }

    void RebuildIfNeeded(const std::source_location& a_where)
    {
        // B4-1: consume the flag here -- this IS the rebuild it was asking
        // for. The empty-board test covers the openings no flag announces:
        // the first of the session, and the post-load B6 resets.
        // ★S-G: square the gold invariant at the opening -- migration of an
        // old save and any drift the events missed both settle here.
        CoinCensus("menu-open");
        if (g_needRebuild.exchange(false, std::memory_order_acq_rel) ||
            g_items.empty()) {
            Rebuild(a_where);
        }
    }

    void Rebuild(const std::source_location& a_where)
    {
        NoteRebuildRan(a_where);   // B3-c: provenance, drained at the entry
        // ★The search set is keyed by tile, so any rebuild can invalidate it.
        // Bumping a counter here and recomputing lazily beats calling into the
        // search from every one of this function's exits.
        ++g_boardVersion;
        // typed bags: "arrived this pass" is per-rebuild state and must not
        // survive into the next one, or a tile keeps being re-routed forever
        g_freshTiles.clear();
        g_arrivedTiles.clear();   // (1.3.2) same lifetime: one rebuild
        // ★A full pass makes the board authoritative again, so an unclaimed
        // optimistic removal has nothing left to say. Dropping them here is
        // what bounds the set: a click whose equip event never arrived would
        // otherwise leave a claim behind and swallow the NEXT decline for that
        // form. The cost of being wrong in this direction is one extra
        // rebuild; in the other it is a tile that stays on screen.
        g_optimisticGone.clear();
        // ★S1: a full pass supersedes the cursor stash and any queued view
        // moves -- the re-derivation IS their answer (the carry exclusion
        // still holds the held unit back, so nothing doubles). A later put
        // with no stash declines into another rebuild, which is correct.
        g_stash.reset();
        g_viewMoveQ.clear();
        // (transient arrivals sweep by FRAME in CapacityTick now -- see the
        // declaration)
        // ★g_typedBagFull is deliberately NOT cleared here. The fill loop reads
        // it EARLY in this rebuild and the bag placement writes it LATE, so
        // clearing at the top would guarantee the fill loop never sees a full
        // bag — the mitigation would be dead code that still compiles. It is
        // re-derived once per pass in BuildViewsAndSpill instead, which is what
        // makes the lag exactly one rebuild.

        // F2 safety net: with the trash CLOSED no layout entry may stay
        // assigned to it (a crash / mid-menu save could persist one via the
        // cosave; the item would render nowhere). Reflow them to first-fit.
        if (!g_trashOpen) {
            for (auto& [k, le] : g_layout) {
                if (le.bag == kTrashKey) { le.bag.clear(); le.col = -1; le.row = -1; }
            }
            g_trashOrder.clear();
            g_trashReturn.clear();
            g_trashXl.clear();
        } else {
            // FIFO bookkeeping stays in sync with the layout: drop keys that
            // left the trash (restored / deleted), append ones that appeared
            // (a carried tile returning after a cancelled re-drop).
            for (auto it = g_trashOrder.begin(); it != g_trashOrder.end();) {
                const auto li = g_layout.find(*it);
                if (li == g_layout.end() || li->second.bag != kTrashKey) {
                    g_trashReturn.erase(*it);
                    g_trashXl.erase(*it);
                    it = g_trashOrder.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto& [k, le] : g_layout) {
                if (le.bag == kTrashKey &&
                    std::find(g_trashOrder.begin(), g_trashOrder.end(), k) ==
                        g_trashOrder.end()) {
                    g_trashOrder.push_back(k);
                }
            }
        }

        g_items.clear();
        g_liveObjs.clear();   // rebuilt alongside g_items
        g_views.clear();
        g_gold = 0;
        g_values.clear();   // Phase 4: rebuilt below from live inventory entries

        // B3: expire pending removals whose engine transfer never landed —
        // without this a failed RemoveItem left the form permanently
        // under-counted (tile invisible until the menu closed)
        if (!g_pendingEquip.empty() &&
            std::chrono::steady_clock::now() - g_pendingEquipWhen > kPendingEquipTTL) {
            SKSE::log::warn("[GRID] pending equip expired ({} units) — releasing",
                            g_pendingEquip.size());
            g_pendingEquip.clear();
        }
        {
            // ★B4-3c: the counter TTL sweep that lived here is gone with the
            // counters -- the ledger's own frame-count expiry (OnRequestExpired)
            // is the one timeout, and it already drives the recovery.
            // ★B3-b housekeeping: a queued slot key whose layout entry is gone
            // is a dead letter -- the cell it named was already erased or
            // pruned, so neither Commit nor Cancel has anything left to do.
            // Slotless requests (world drop, use) leave these behind by
            // design; sweeping here keeps the queue from outliving the board.
            for (auto qi = g_pendingSlotDrop.begin(); qi != g_pendingSlotDrop.end();) {
                std::erase_if(qi->second, [](const std::string& k) {
                    return !g_layout.contains(k);
                });
                qi = qi->second.empty() ? g_pendingSlotDrop.erase(qi) : std::next(qi);
            }
        }
        // Lazy, not unconditional: the cosave is the layout authority now — an
        // every-rebuild ini re-read was the cross-save contamination (and would
        // clobber the freshly loaded record). Ini = legacy fallback only.
        if (!g_layoutLoaded) LoadLayout();

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        CollectDisplayTiles(player);            // stage 1+2 (collect + coin partition)
        auto bags = ReconcileBagBookkeeping();  // stage 3
        BuildViewsAndSpill(bags);               // stage 4 (views + spill)
        FinalizeRebuild();                      // stage 5
    }

    // ---- ★B3 partial-update helpers: main-view occupancy -----------------
    namespace
    {
        std::vector<std::vector<bool>> MainViewOcc()
        {
            const auto& mv = g_views[0];
            std::vector<std::vector<bool>> occ(
                static_cast<std::size_t>(mv.rows),
                std::vector<bool>(static_cast<std::size_t>(mv.cols), false));
            for (int idx : mv.items) {
                const auto& it = g_items[static_cast<std::size_t>(idx)];
                if (it.col < 0) continue;
                for (std::size_t r = 0; r < it.mask.rows.size(); ++r) {
                    for (std::size_t c = 0; c < it.mask.rows[r].size(); ++c) {
                        if (!it.mask.rows[r][c]) continue;
                        const int rr = it.row + static_cast<int>(r);
                        const int cc = it.col + static_cast<int>(c);
                        if (rr >= 0 && rr < mv.rows && cc >= 0 && cc < mv.cols) {
                            occ[static_cast<std::size_t>(rr)]
                               [static_cast<std::size_t>(cc)] = true;
                        }
                    }
                }
            }
            return occ;
        }

        bool OccFits(const std::vector<std::vector<bool>>& a_occ,
                     int a_c, int a_r, const Mask& a_m)
        {
            const int rows = static_cast<int>(a_occ.size());
            const int cols = rows > 0 ? static_cast<int>(a_occ[0].size()) : 0;
            for (std::size_t r = 0; r < a_m.rows.size(); ++r) {
                for (std::size_t c = 0; c < a_m.rows[r].size(); ++c) {
                    if (!a_m.rows[r][c]) continue;
                    const int rr = a_r + static_cast<int>(r);
                    const int cc = a_c + static_cast<int>(c);
                    if (rr < 0 || rr >= rows || cc < 0 || cc >= cols) return false;
                    if (a_occ[static_cast<std::size_t>(rr)]
                             [static_cast<std::size_t>(cc)]) return false;
                }
            }
            return true;
        }

        void OccMark(std::vector<std::vector<bool>>& a_occ,
                     int a_c, int a_r, const Mask& a_m)
        {
            for (std::size_t r = 0; r < a_m.rows.size(); ++r) {
                for (std::size_t c = 0; c < a_m.rows[r].size(); ++c) {
                    if (a_m.rows[r][c]) {
                        a_occ[static_cast<std::size_t>(a_r) + r]
                             [static_cast<std::size_t>(a_c) + c] = true;
                    }
                }
            }
        }

        bool OccFirstFit(const std::vector<std::vector<bool>>& a_occ,
                         const Mask& a_m, int& a_c, int& a_r)
        {
            const int rows = static_cast<int>(a_occ.size());
            const int cols = rows > 0 ? static_cast<int>(a_occ[0].size()) : 0;
            for (int r = 0; r + a_m.h <= rows; ++r) {
                for (int c = 0; c + a_m.w <= cols; ++c) {
                    if (OccFits(a_occ, c, r, a_m)) { a_c = c; a_r = r; return true; }
                }
            }
            return false;
        }

        // First-fit, both orientations -- the same pair the rebuild's placer
        // tries. Sets col/row/rot on a_le and marks the occupancy on success.
        bool OccPlace(std::vector<std::vector<bool>>& a_occ, const GridDef& a_def,
                      LayoutEntry& a_le)
        {
            Mask m = MaskOf(a_def);
            int  c = -1, r = -1;
            if (OccFirstFit(a_occ, m, c, r)) {
                a_le.rot = 0;
            } else if (CanRotate(a_def)) {
                m = MaskOf(a_def, 1);
                if (!OccFirstFit(a_occ, m, c, r)) return false;
                a_le.rot = 1;
            } else {
                return false;
            }
            a_le.col = c;
            a_le.row = r;
            OccMark(a_occ, c, r, m);
            return true;
        }
    }

    // ---- ★B3 BODY: the partial board update ------------------------------
    //
    // An unequip puts ONE unit back on the board, and until now the only way
    // to draw it was to re-derive the whole board from the engine. This is
    // the missing half §8-10 named: RemoveUnitFromBoard has existed since
    // NotePendingEquip (the optimistic hide); AddUnitToBoard is its return
    // leg. It reuses the two authorities the rebuild itself uses -- the
    // shared unit walk (identity, slot assignment, all off-board invariants)
    // and MakeDisplayTile (the one tile factory) -- scoped to ONE form.
    //
    // ★Fallback discipline: any situation this path is not CERTAIN about
    // returns false and the caller runs the full rebuild -- correctness is
    // exactly yesterday's, the fast path only covers what it can prove. Every
    // decline logs its reason, so the coverage is measured, not assumed
    // (the same bargain !rbdrop struck).
    bool OnFormDelta(std::uint32_t a_form)
    {
        const auto decline = [&](const char* a_why) {
            // ★NOT "-- full rebuild". That was written when every caller
            // escalated a decline, and one of them does not: the EQUIP side of
            // the event sink drops the return value on purpose (see the note
            // there). So this line promised a rebuild that, on that path, never
            // happened -- and a log that lies is worse than a quiet one,
            // because it is the first thing read when a stale tile is
            // reported. Every caller but that one escalates, which is the
            // documented default; the exception says so itself rather than
            // making this line repeat what is usually true.
            SKSE::log::info("[B3] partial add declined ({:08X}): {}",
                a_form, a_why);
            return false;
        };
        // ★(1.5.x) a unit that is only PASSING THROUGH (shelf use mode:
        // taken and consumed in the same breath) mints no tile and asks for
        // no rebuild -- the tome that blinked onto the board and died there
        // was this. Both deltas (the arrival and the spend) fall inside the
        // TTL and are swallowed as a pair; a unit the use REFUSED is
        // surfaced by the TTL-expiry rebuild instead (see the sweep).
        if (const auto ti = g_transientArrivals.find(a_form);
            ti != g_transientArrivals.end()) {
            ti->second.suppressed = true;
            SKSE::log::info("[B3] partial add ({:08X}): in transit -- no tile",
                            a_form);
            return true;
        }
        // ★★★THE HINT IS SPENT ON COMMIT, NOT WHILE PLANNING.
        //
        // This whole path is "all traces or none" -- it plans, and a single
        // decline anywhere throws the plan away and hands the delta to a full
        // rebuild. g_dropHint was the one thing that did not play by that
        // rule: it was cleared the moment a unit claimed it, so a decline two
        // units later left the rebuild to place everything with no hint at
        // all, and the cell the player actually aimed at was ignored. Track it
        // locally, and only write the global at the end.
        bool hintTaken = false;
        // A full rebuild is already on its way: let it cover this delta too.
        // Partials one-by-one against a pending full pass are pure waste, and
        // this line is what coalesces a take-all burst into ONE rebuild --
        // the first decline raises the flag, the rest short-circuit here.
        if (g_needRebuild.load(std::memory_order_relaxed)) return false;
        // Menu closed: the coalesced flag is already the cheap path -- the
        // next open (or a capacity gate) rebuilds once for the whole batch.
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen("GridInventoryMenu")) return false;   // quiet: normal
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return false;              // 원칙 4
        if (g_views.empty()) return false;   // board never built yet this session

        auto* form = RE::TESForm::LookupByID(a_form);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!obj) return decline("form not found");
        if (obj->IsGold() || GoldCoins::IsCoinForm(a_form)) {
            return decline("coin mirror");
        }
        // ★B4-4: a form the board NEVER SHOWS is a full answer, not a
        // decline. The ring carrier, the costume anchors and non-playable
        // scripting copies move containers and fire equip events like
        // anything else -- and every one of those used to fall through to
        // "decline -> full rebuild": three rebuilds per ring swap for a form
        // with no tile anywhere (log-measured; that window is the deferred
        // ring blink's habitat). SkipInventoryEntry is the board's own door
        // policy, so the same rules decide here.
        {
            const char* nm = obj->GetName();
            if (!obj->GetPlayable() || Costume::IsAnchor(obj) ||
                DualRing::IsCarrier(obj) || !nm || !*nm) {
                SKSE::log::info("[B3] partial add ({:08X}): board-invisible "
                                "form -- nothing to do", a_form);
                return true;
            }
        }
        const GridDef gdef = g_resolver ? g_resolver(obj) : GridDef{};
        if (gdef.bag != 0) return decline("bag form (window wiring)");
        const int cap = EffectiveCap(obj, gdef);
        // The rebuild would claim a fresh tile of a filtered form into a held
        // typed bag; first-fitting it onto the main board here would diverge
        // and the next rebuild would visibly move it.
        // ★S4: the STACKABLE branch seats its mints in the typed bag itself
        // now (below), so only GEAR still declines -- gear-shaped filtered
        // forms are rare and their claim order is the rebuild's to keep.
        const std::string typedFl = BagFilter::FilterOf(obj);
        const bool typedHeld = !typedFl.empty() && g_typedBagsHeld.contains(typedFl);
        if (typedHeld && cap <= 1) {
            return decline("typed bag would claim it (gear)");
        }
        // ★The entry comes from GetInventory -- the same source the rebuild
        // walks. LiveEntryOf answers from the CHANGES list, and a PLAIN item
        // that has just been unequipped often has no changes entry left (its
        // last extra list, ExtraWorn, went with the unequip) -- so the most
        // common case of all, a plain weapon displaced by a slot conflict,
        // always declined. The map OWNS the entry: it lives to the end of
        // this function, and nothing here outlives the call (원칙 2).
        auto inv = player->GetInventory(
            [&](RE::TESBoundObject& o) { return &o == obj; });
        int                     count = 0;
        RE::InventoryEntryData* entry = nullptr;
        for (auto& [o2, d2] : inv) {
            count = d2.first;
            entry = d2.second.get();
        }
        if (count <= 0) return decline("count 0");
        if (!entry) return decline("no entry");
        const std::string baseKey = FormKey(obj);
        std::uint8_t glow = 0;
        if (const auto* ef = obj->As<RE::TESEnchantableForm>();
            ef && ef->formEnchanting) {
            glow |= 1;
        }
        if (IsUniqueCached(obj)) glow |= 2;

        // ---- ★stackable coverage: the PROVABLE clean case ----------------
        // The rebuild's units computation for stackables is ~100 lines of
        // hard-won interplay (worn/carried/pendingEquip -- the quiver sagas),
        // and duplicating it here is exactly the drift this codebase keeps
        // paying for. So the fast path takes only the state it can prove:
        // no worn list left, nothing carried, nothing pending, nothing
        // reserved or parked. That IS the common unequip (ammo swap, torch
        // down, scroll away) -- everything else declines with a reason and
        // the [B3] lines keep measuring what the fallback still costs.
        //
        // Distribution safety: a tile OWNS its quantity and the reconciler
        // only closes the GAP between the saved sum and the engine count --
        // so any fill whose sum is right is STABLE under the next rebuild.
        if (cap > 1) {
            if (entry->IsWorn()) return decline("still worn");
            if (g_held && g_held->obj == obj) return decline("carried");
            for (const auto& u : g_pendingEquip) {
                if (u.base == baseKey) return decline("equip in flight");
            }
            if (Ledger::OpenOutgoingCount(a_form) > 0) return decline("removal in flight");   // B4-3c
            if (Loadout::ReservedCount(a_form) > 0) return decline("reserved");
            if (TrashedUnits(baseKey) > 0) return decline("trash parked");
            if (DualRing::Second() == obj) return decline("dual ring");

            // pools from the shared authority (nothing is queued out of them
            // -- the gates above proved it)
            int  signedUnits = 0;
            auto pools = SignedPools(entry, baseKey, signedUnits,
                                     /*countTrashed=*/false);
            pools[baseKey] += (std::max)(0, count - signedUnits);

            // the tiles standing for this form, grouped by pool
            std::map<std::string, std::vector<int>> tilesByPool;
            std::map<std::string, int>              sumByPool;
            for (std::size_t i = 0; i < g_items.size(); ++i) {
                const auto& it = g_items[i];
                if (it.obj != obj || it.inBag == kTrashKey) continue;
                const std::string pool = PoolPrefix(baseKey, it.uid, it.sig);
                tilesByPool[pool].push_back(static_cast<int>(i));
                sumByPool[pool] += it.count;
            }
            bool shrank = false, grew = false;
            for (const auto& [pool, sum] : sumByPool) {
                const auto pi = pools.find(pool);
                if ((pi == pools.end() ? 0 : pi->second) < sum) shrank = true;
            }
            for (const auto& [pool, n] : pools) {
                if (n > sumByPool[pool]) { grew = true; break; }
            }
            // ★S3: pools crossing (one down, one up) is a RELABEL -- the same
            // unit changed value, and which tile keeps which cell is census's
            // matching question (PLAN §3), not this fast path's.
            if (shrank && grew) return decline("pools crossed (relabel)");
            if (shrank) {
                // ---- ★S3: the REMOVE direction, pure-shrink only ----------
                // The gates above proved nothing of ours is in flight, so the
                // deficit is a genuine outside removal (script, follower,
                // vanilla favourites). Tiles pay REAR-first in position order
                // -- the same "the rear tiles absorb the spend" rule the gold
                // partition follows -- and a tile that empties leaves whole.
                struct Cut { std::string key; int take; };
                std::vector<Cut> plan;
                for (auto& [pool, sum] : sumByPool) {
                    const auto pi = pools.find(pool);
                    int deficit = sum - (pi == pools.end() ? 0 : pi->second);
                    if (deficit <= 0) continue;
                    auto idxs = tilesByPool[pool];
                    std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
                        const auto& x = g_items[static_cast<std::size_t>(a)];
                        const auto& y = g_items[static_cast<std::size_t>(b)];
                        if (x.inBag != y.inBag) return x.inBag > y.inBag;   // bags first (rear)
                        if (x.row != y.row) return x.row > y.row;
                        return x.col > y.col;
                    });
                    for (int i : idxs) {
                        if (deficit <= 0) break;
                        const auto& t = g_items[static_cast<std::size_t>(i)];
                        const int take = (std::min)(t.count, deficit);
                        plan.push_back({ t.key, take });
                        deficit -= take;
                    }
                    if (deficit > 0) return decline("shrink exceeds shown tiles");
                }
                for (const auto& c : plan) {
                    // by KEY, one at a time -- each erase re-finds, so the
                    // index shifts of a previous cut cannot mislead this one.
                    // The lean removal: no drain-hint spend, no optimistic
                    // claim -- those belong to CLICK paths, and this is an
                    // outside delta.
                    int idx = -1;
                    for (std::size_t i = 0; i < g_items.size(); ++i) {
                        if (g_items[i].key == c.key) { idx = static_cast<int>(i); break; }
                    }
                    if (idx < 0) { RequestRebuild(); return true; }
                    auto& t = g_items[static_cast<std::size_t>(idx)];
                    if (c.take < t.count) {
                        t.count -= c.take;
                        g_layout[c.key].count = t.count;
                        SKSE::log::info("[B3] ★outside shrink '{}' -{} -> {} in '{}'"
                                        " -- no rebuild",
                            obj->GetName(), c.take, t.count, c.key);
                        continue;
                    }
                    for (const auto& v : g_views) {
                        if (std::find(v.items.begin(), v.items.end(), idx) ==
                            v.items.end()) {
                            continue;
                        }
                        if (v.accept.empty() && v.bagKey != kTrashKey) {
                            g_spaceUsed -= MaskCells(t.mask.rows);
                        }
                        break;
                    }
                    g_items.erase(g_items.begin() + idx);
                    for (auto& v : g_views) {
                        std::erase(v.items, idx);
                        for (int& i2 : v.items) {
                            if (i2 > idx) --i2;
                        }
                    }
                    g_layout.erase(c.key);
                    g_prevKeys.erase(c.key);
                    g_newTiles.erase(c.key);
                    ++g_boardVersion;
                    SKSE::log::info("[B3] ★outside shrink '{}' tile '{}' off the "
                                    "board -- no rebuild", obj->GetName(), c.key);
                }
                MarkCapacityDirty();
                return true;
            }
            if (!grew) {
                SKSE::log::info("[B3] partial add ({:08X}): nothing fresh -- "
                                "no rebuild", a_form);
                return true;
            }

            // plan every fill and mint before touching anything (rule 5)
            auto occ = MainViewOcc();
            // ★S4: occupancy per HELD TYPED BAG, filled lazily -- a filtered
            // form's fresh tiles seat where the rebuild's claim would put
            // them, and a full bag bounces to main (decision 1).
            std::map<std::string, std::vector<std::vector<bool>>> typedOcc;
            struct Fill { int idx; int add; };
            struct Mint { LayoutEntry le; int units; };
            std::vector<Fill> fills;
            std::vector<Mint> mints;
            for (auto& [pool, want] : pools) {
                int delta = want - sumByPool[pool];
                if (delta <= 0) continue;
                // ★★★THE TILE THE PLAYER DROPPED ON IS TOPPED UP FIRST.
                //
                // Fills ran in board order (row, then col). That is right for
                // an arrival nobody aimed -- a pickup from the world has no
                // cell behind it. A DROP is the opposite: the whole gesture
                // names a cell, and topping up some OTHER tile instead is
                // exactly what "it went to the wrong stack" means.
                //
                // Reported as items scattering. Drop three arrows onto a stack
                // of forty-six and the three landed on whichever arrow tile
                // sat highest on the board; the tile under the cursor never
                // moved, and the rest of the pile appeared to jump elsewhere.
                //
                // The hint was already consulted a few lines below -- but only
                // when MINTING a new tile, never when filling an existing one,
                // which is the case a merge always takes.
                auto& idxs = tilesByPool[pool];
                const bool hinted = !hintTaken &&
                    g_dropHint.Wants(baseKey, pool) && g_dropHint.bag.empty();
                const int hc = hinted ? g_dropHint.col : -1;
                const int hr = hinted ? g_dropHint.row : -1;
                std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
                    const auto& x = g_items[static_cast<std::size_t>(a)];
                    const auto& y = g_items[static_cast<std::size_t>(b)];
                    const bool xh = hinted && x.inBag.empty() &&
                                    x.col == hc && x.row == hr;
                    const bool yh = hinted && y.inBag.empty() &&
                                    y.col == hc && y.row == hr;
                    if (xh != yh) return xh;   // ★the dropped-on tile leads
                    if (x.inBag != y.inBag) return x.inBag < y.inBag;
                    if (x.row != y.row) return x.row < y.row;
                    return x.col < y.col;
                });
                bool hintSpentByFill = false;
                for (int i : idxs) {
                    if (delta <= 0) break;
                    const auto& t = g_items[static_cast<std::size_t>(i)];
                    const int room = cap - t.count;
                    const int take = (std::min)(room, delta);
                    if (take > 0) {
                        if (hinted && t.inBag.empty() &&
                            t.col == hc && t.row == hr) {
                            hintSpentByFill = true;
                        }
                        fills.push_back({ i, take });
                        delta -= take;
                    }
                }
                // ★A MERGE SPENDS THE HINT TOO. The note further down already
                // says the hint belongs to the ARRIVAL rather than to a minted
                // tile -- but the code only cleared it when a tile was minted.
                // Left armed after a merge, it fires on the NEXT pickup of this
                // form and teleports it to a cell the player chose for
                // something else.
                if (hintSpentByFill) hintTaken = true;
                // whatever the saved tiles cannot hold becomes fresh tiles --
                // the pool prefix carries the identity the new slot records
                while (delta > 0) {
                    const int n = (std::min)(cap, delta);
                    Mint mp;
                    // ★★-1 MEANS UNPLACED. LayoutEntry defaults to col 0 row 0
                    // -- a real cell -- so the OccPlace guard below ("col < 0")
                    // never fired for a hint-less mint and every one of them
                    // sat down at [0,0], on top of whatever lived there (user
                    // report: a torch unequipped onto an occupied first cell).
                    // The reconciler's mint sets -1 for the same reason.
                    mp.le.col = -1;
                    mp.le.row = -1;
                    mp.le.uid = UidOf(pool);
                    mp.le.sig = SigOf(pool);
                    // partner-drop hint, same rules as the gear branch above
                    if (!hintTaken && g_dropHint.Wants(baseKey, pool)) {
                        if (!g_dropHint.bag.empty()) {
                            return decline("hinted into a bag");
                        }
                        const int  hrot = CanRotate(gdef) ? (g_dropHint.rot & 3) : 0;
                        const Mask hm = MaskOf(gdef, hrot);
                        if (OccFits(occ, g_dropHint.col, g_dropHint.row, hm)) {
                            mp.le.col = g_dropHint.col;
                            mp.le.row = g_dropHint.row;
                            mp.le.rot = hrot;
                            OccMark(occ, mp.le.col, mp.le.row, hm);
                            hintTaken = true;
                        }
                    }
                    // ★S4: the typed-bag seat comes FIRST for a filtered form
                    // -- that is where the rebuild's claim would put it. Each
                    // held bag of the filter is tried in view order; a full
                    // one falls through, and no bag at all bounces to main.
                    if (mp.le.col < 0 && typedHeld) {
                        for (const auto& v : g_views) {
                            if (v.accept != typedFl) continue;
                            auto to = typedOcc.find(v.bagKey);
                            if (to == typedOcc.end()) {
                                to = typedOcc.emplace(v.bagKey, ViewOccOf(v)).first;
                            }
                            LayoutEntry seat;
                            seat.uid = mp.le.uid;
                            seat.sig = mp.le.sig;
                            if (OccPlace(to->second, gdef, seat)) {
                                mp.le = seat;
                                mp.le.bag = v.bagKey;
                                break;
                            }
                        }
                    }
                    if (mp.le.col < 0 && !OccPlace(occ, gdef, mp.le)) {
                        return decline("no room (growth/spill)");
                    }
                    // ★★★A GROWTH ROW IS NOT A PLACE THE PARTIAL CAN LEAVE IT.
                    // MakeDisplayTile treats rows past kMinRows as temporary
                    // and blanks the coordinates -- correctly, because the
                    // rebuild's PlaceItems runs straight afterwards and seats
                    // them properly. The partial has no such second pass, so
                    // the tile kept col = -1 forever: counted in the space
                    // figure, skipped by every draw loop, invisible until
                    // something else forced a full rebuild. Hand it over
                    // instead; declining is what this path is for.
                    if (mp.le.bag.empty() &&
                        !OwnedFootprint(mp.le.col, mp.le.row,
                            MaskOf(gdef, CanRotate(gdef) ? (mp.le.rot & 3) : 0))) {
                        return decline("landed in the growth zone");
                    }
                    mp.units = n;
                    mints.push_back(std::move(mp));
                    delta -= n;
                }
            }

            // commit: counts first, then the minted tiles with every trace
            for (const auto& f : fills) {
                auto& it = g_items[static_cast<std::size_t>(f.idx)];
                it.count += f.add;
                g_layout[it.key].count = it.count;
                SKSE::log::info("[B3] ★stack fill '{}' +{} -> {} in '{}' -- no rebuild",
                    obj->GetName(), f.add, it.count, it.key);
            }
            auto& mv = g_views[0];
            for (auto& mp : mints) {
                const std::string key = NextTileKey(baseKey);
                mp.le.count = mp.units;
                g_layout[key] = mp.le;
                MakeDisplayTile(obj, entry, gdef, glow, key, mp.units, mp.le, -1);
                // ★S4: the seat's own view -- a typed-bag mint joins its bag
                int mintVi = 0;
                if (!mp.le.bag.empty()) {
                    mintVi = -1;
                    for (std::size_t vi2 = 0; vi2 < g_views.size(); ++vi2) {
                        if (g_views[vi2].bagKey == mp.le.bag) {
                            mintVi = static_cast<int>(vi2);
                            break;
                        }
                    }
                    if (mintVi < 0) { RequestRebuild(); return true; }
                }
                g_views[static_cast<std::size_t>(mintVi)].items.push_back(
                    static_cast<int>(g_items.size()) - 1);
                // ★★★AND IT IS NEW, SAID HERE. The mark used to be worked out
                // only inside a full rebuild, by asking which keys were absent
                // from g_prevKeys -- and this path mints a key without ever
                // touching that set. So a container emptied by right-click
                // (every take a partial add) showed no marks at all, and then
                // the first DRAG forced a rebuild that found the whole batch
                // missing from g_prevKeys and lit every one of them at once.
                // Reported exactly that way.
                //
                // ★The same two gates the rebuild uses, so the two paths
                // cannot disagree: nothing is marked before a snapshot exists,
                // nothing is marked on the rebuild a load asks for, and the
                // form has to hold MORE than it did when the player last
                // looked. `count` is that live total, read at the top.
                if (g_seenValid && !g_suppressNew) {
                    const auto sc = g_seenCount.find(a_form);
                    if (count > (sc == g_seenCount.end() ? 0 : sc->second)) {
                        g_newTiles.insert(key);
                    }
                }
                // ★...and the key is ON THE BOARD now. Without this the next
                // full rebuild finds it missing from g_prevKeys and marks it a
                // second time -- which is the batch above, arriving late.
                g_prevKeys.insert(key);
                // ★S4: typed-bag cells never count toward the space figure
                // (FinalizeRebuild's own rule)
                if (mp.le.bag.empty()) {
                    g_spaceUsed += MaskCells(g_items.back().mask.rows);
                }
                SKSE::log::info("[B3] ★stack mint '{}' x{} key '{}' at [{},{}]{} -- "
                                "no rebuild",
                    obj->GetName(), mp.units, key, mp.le.col, mp.le.row,
                    mp.le.bag.empty() ? "" : " (typed bag)");
            }
            g_liveObjs.insert(obj);
            MarkCapacityDirty();
            return true;
        }

        // The same walk the rebuild runs, scoped to this form: identity,
        // slot assignment, every off-board exclusion (held / trash / pending)
        // -- and mutate=true mints the GI21 placeholder for the fresh unit.
        std::vector<UnitTile> units_v;
        EnumerateUnitTiles(baseKey, count, (std::numeric_limits<int>::max)(),
                           entry, units_v, gdef.bag == 0, /*mutate=*/true);

        // Diff against what is standing on the boards. A tile WE show that
        // the walk no longer lists means shapes changed under us -- rebuild.
        std::set<std::string> have;
        for (const auto& it : g_items) {
            if (it.obj == obj && it.inBag != kTrashKey) have.insert(it.key);
        }
        for (const auto& k : have) {
            const bool listed = std::any_of(units_v.begin(), units_v.end(),
                [&](const UnitTile& u) { return u.key == k; });
            if (!listed) return decline("a shown tile left the set");
        }
        std::vector<const UnitTile*> fresh;
        for (const auto& u : units_v) {
            if (!have.contains(u.key)) fresh.push_back(&u);
        }
        // Nothing new to draw: the unit went to the cursor (doll lift), or the
        // event was an echo. The board is already right -- skip the rebuild.
        if (fresh.empty()) {
            SKSE::log::info("[B3] partial add ({:08X}): nothing fresh -- no rebuild",
                a_form);
            return true;
        }

        // Occupancy of the MAIN view, from the tiles standing on it.
        auto& mv = g_views[0];
        auto  occ = MainViewOcc();

        // Decide EVERY placement before touching g_items -- a fallback after a
        // partial commit would leave a half-updated board for the rebuild to
        // race (rule 5: all traces or none).
        // ★S3: seats span the MAIN view and the GENERAL bag views now -- a
        // fresh unit whose saved spot (or drop hint) names a bag is seated in
        // that bag's own occupancy instead of declining. Typed bags stay the
        // rebuild's business (their claims run in the fill pass), and so does
        // anything whose view cannot be found.
        std::map<std::string, std::vector<std::vector<bool>>> occByBag;
        const auto viewIdxOf = [&](const std::string& a_bag) -> int {
            for (std::size_t i = 0; i < g_views.size(); ++i) {
                if (g_views[i].bagKey == a_bag) return static_cast<int>(i);
            }
            return -1;
        };
        // nullptr = no seatable view for this bag (typed / trash / missing)
        const auto occOf = [&](const std::string& a_bag)
            -> std::vector<std::vector<bool>>* {
            if (a_bag.empty()) return &occ;
            if (a_bag == kTrashKey) return nullptr;
            const auto oi = occByBag.find(a_bag);
            if (oi != occByBag.end()) return &oi->second;
            const int vi = viewIdxOf(a_bag);
            if (vi < 0 || !g_views[static_cast<std::size_t>(vi)].accept.empty()) {
                return nullptr;
            }
            return &occByBag.emplace(a_bag,
                ViewOccOf(g_views[static_cast<std::size_t>(vi)])).first->second;
        };
        struct Planned { const UnitTile* u; LayoutEntry le; };
        std::vector<Planned> plan;
        for (const auto* u : fresh) {
            LayoutEntry le;
            if (const auto li = g_layout.find(u->key); li != g_layout.end()) le = li->second;
            auto* seatOcc = occOf(le.bag);
            if (!seatOcc) return decline("no seatable view for the saved bag");
            if (le.col >= 0) {
                // a surviving saved spot (parked star, cancel) -- honour it if free
                const Mask m = MaskOf(gdef, CanRotate(gdef) ? (le.rot & 3) : 0);
                if (!OccFits(*seatOcc, le.col, le.row, m)) {
                    return decline("saved spot occupied");
                }
                OccMark(*seatOcc, le.col, le.row, m);
            } else {
                // ★A partner-drop hint aims the fresh tile at the drop cell --
                // the same honour CollectDisplayTiles pays it (GI21/B2). ★S3:
                // a hint into a GENERAL bag seats there now; an occupied hint
                // falls through to first-fit, matching the placer.
                if (!hintTaken && g_dropHint.Wants(baseKey,
                        PoolPrefix(baseKey, u->uid, u->sig))) {
                    auto* hOcc = occOf(g_dropHint.bag);
                    const int  hrot = CanRotate(gdef) ? (g_dropHint.rot & 3) : 0;
                    const Mask hm = MaskOf(gdef, hrot);
                    if (hOcc && OccFits(*hOcc, g_dropHint.col, g_dropHint.row, hm)) {
                        le.col = g_dropHint.col;
                        le.row = g_dropHint.row;
                        le.rot = hrot;
                        le.bag = g_dropHint.bag;
                        OccMark(*hOcc, le.col, le.row, hm);
                        hintTaken = true;
                        plan.push_back({ u, le });
                        continue;
                    }
                }
                // rule 13 forgot the cell at equip time: first-fit, both
                // orientations, same as the rebuild's placer would
                if (!OccPlace(*seatOcc, gdef, le)) {
                    return decline("no room (growth/spill)");
                }
            }
            // ★see the mint path: MakeDisplayTile blanks growth-row coordinates
            // and only the full rebuild puts them back.
            if (le.bag.empty() &&
                !OwnedFootprint(le.col, le.row,
                    MaskOf(gdef, CanRotate(gdef) ? (le.rot & 3) : 0))) {
                return decline("landed in the growth zone");
            }
            plan.push_back({ u, le });
        }

        // Commit: the one tile factory, then every bookkeeping trace the
        // rebuild would have left for this tile (rule 5).
        for (const auto& p : plan) {
            g_layout[p.u->key] = p.le;   // persist BEFORE mint: the tile reads it
            MakeDisplayTile(obj, entry, gdef, glow, p.u->key, 1, p.le, p.u->xlIdx);
            const int idx = static_cast<int>(g_items.size()) - 1;
            const int tvi = viewIdxOf(p.le.bag);   // ★S3: the seat's own view
            if (tvi < 0) { RequestRebuild(); return true; }
            auto& tv = g_views[static_cast<std::size_t>(tvi)];
            tv.items.push_back(idx);
            if (tv.accept.empty() && tv.bagKey != kTrashKey) {
                g_spaceUsed += MaskCells(g_items.back().mask.rows);
            }
            g_liveObjs.insert(obj);
            // ★★★AND IT IS NEW, SAID HERE -- the GEAR half. The 1.4.4 fix
            // stamped the mark at the STACKABLE mint and never at this one,
            // so a chest emptied of weapons by right-click showed no marks
            // until a drag's rebuild lit the whole batch (user report, twice
            // -- the second time when S3 widened this path's coverage and
            // the rebuilds that had been papering over it stopped running).
            // Same two gates as the rebuild and the stack mint, so the three
            // paths cannot disagree; and the key goes into g_prevKeys, or
            // the NEXT rebuild marks it a second time.
            if (g_seenValid && !g_suppressNew) {
                const auto sc = g_seenCount.find(a_form);
                if (count > (sc == g_seenCount.end() ? 0 : sc->second)) {
                    g_newTiles.insert(p.u->key);
                }
            }
            g_prevKeys.insert(p.u->key);
            SKSE::log::info("[B3] ★partial add '{}' key '{}' at [{},{}]{} -- no rebuild",
                obj->GetName(), p.u->key, p.le.col, p.le.row,
                p.le.bag.empty() ? "" : " (bag)");
        }
        // ★NOW it is spent -- the plan committed, so the cell the player aimed
        // at has actually been used. A decline above returns without touching
        // the global and leaves the hint for the rebuild.
        if (hintTaken) g_dropHint = {};
        MarkCapacityDirty();
        return true;
    }

    // ---- ★B3 symmetric half: the use/equip click's partial REMOVE ----------
    namespace
    {
        // Runs at FinishFrame (never mid-draw). Takes the clicked tile's
        // departing units off the board the way the full rebuild would have
        // drawn it: gear and ammo lose the whole tile, a stackable counts
        // down -- and the drain hint is CONSUMED when this does its job, or
        // the next real rebuild would spend it a second time. False = the
        // caller rebuilds (tile gone, shapes changed -- anything unproven).
        bool TryUseClickPartialRemove(const std::string& a_key,
                                      RE::TESBoundObject* a_obj,
                                      int a_take, bool a_drained)
        {
            int idx = -1;
            for (std::size_t i = 0; i < g_items.size(); ++i) {
                if (g_items[i].key == a_key && g_items[i].obj == a_obj) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            if (idx < 0) return false;
            auto& it = g_items[static_cast<std::size_t>(idx)];
            const int take = a_take > 0 ? a_take
                                        : Equip::EquipCountFor(a_obj, it.count);

            if (take < it.count) {
                // stackable counting down: the tile keeps its cell, the
                // reconciler's own rule ("a tile owns its quantity") keeps
                // this stable under the next full rebuild
                it.count -= take;
                // ★store/sell already drained the LAYOUT in NotePendingRemove
                // (cancel-safe, confirm-time semantics) -- writing it again
                // here would drain the stack twice. Only the display syncs.
                if (!a_drained) {
                    if (const auto li = g_layout.find(a_key); li != g_layout.end()) {
                        li->second.count = it.count;
                    }
                }
                if (g_drainHint.key == a_key) g_drainHint = {};
                MarkCapacityDirty();
                SKSE::log::info("[B3] ★use click '{}' -{} -> {} in '{}' -- no rebuild",
                    a_obj->GetName(), take, it.count, a_key);
                g_optimisticGone.insert(a_obj->GetFormID());   // the sink asks
                return true;
            }

            // the whole tile leaves: find its view, keep the space stats in
            // step (typed bags and the trash never counted -- FinalizeRebuild's
            // own rule), then erase and re-point every index above it
            for (const auto& v : g_views) {
                if (std::find(v.items.begin(), v.items.end(), idx) == v.items.end()) {
                    continue;
                }
                if (v.accept.empty() && v.bagKey != kTrashKey) {
                    g_spaceUsed -= MaskCells(it.mask.rows);
                }
                break;
            }
            g_items.erase(g_items.begin() + idx);
            for (auto& v : g_views) {
                std::erase(v.items, idx);
                for (int& i : v.items) {
                    if (i > idx) --i;
                }
            }
            // ★The hint STAYS ARMED here (unlike the partial branch above,
            // whose layout write settles everything synchronously). A poison
            // goes through the vanilla apply-confirm, so the engine takes the
            // unit FRAMES after this runs -- the before/after read in
            // ProcessPending sees no change, rule 13 rightly keeps the cell,
            // and this tile's layout claim outlives its display. The rebuild
            // behind the eventual confirm event pays its deficit hint-first,
            // which is the only thing that stops it eating the potion BAG's
            // copy instead (user report). Spent or not, the hint dies at that
            // form walk, and a cancelled dialog just re-emits the tile in its
            // own cell. Store/sell settled the layout in NotePendingRemove,
            // so their hint has no job left.
            if (a_drained && g_drainHint.key == a_key) g_drainHint = {};
            MarkCapacityDirty();
            SKSE::log::info("[B3] ★use click '{}' tile '{}' off the board -- no rebuild",
                a_obj->GetName(), a_key);
            g_optimisticGone.insert(a_obj->GetFormID());   // the sink asks
            return true;
        }

        // ---- ★PLAN_SPACE_AUTHORITY S0: verb bodies -------------------------
        // (declared beside g_stash; the checklist they settle is written there)

        // occupancy of ONE view, from the tiles standing on it -- MainViewOcc
        // generalised, because Park/Restore fit into the trash and bag views
        // (default a_skipIdx lives on the forward declaration)
        std::vector<std::vector<bool>> ViewOccOf(const View& a_v, int a_skipIdx)
        {
            std::vector<std::vector<bool>> occ(
                static_cast<std::size_t>(a_v.rows),
                std::vector<bool>(static_cast<std::size_t>(a_v.cols), false));
            for (int idx : a_v.items) {
                if (idx == a_skipIdx) continue;
                const auto& it = g_items[static_cast<std::size_t>(idx)];
                if (it.col < 0) continue;
                for (std::size_t r = 0; r < it.mask.rows.size(); ++r) {
                    for (std::size_t c = 0; c < it.mask.rows[r].size(); ++c) {
                        if (!it.mask.rows[r][c]) continue;
                        const int rr = it.row + static_cast<int>(r);
                        const int cc = it.col + static_cast<int>(c);
                        if (rr >= 0 && rr < a_v.rows && cc >= 0 && cc < a_v.cols) {
                            occ[static_cast<std::size_t>(rr)]
                               [static_cast<std::size_t>(cc)] = true;
                        }
                    }
                }
            }
            return occ;
        }

        // does this view count toward the space figure? FinalizeRebuild's rule
        bool ViewCountsSpace(const View& a_v)
        {
            return a_v.accept.empty() && a_v.bagKey != kTrashKey;
        }

        bool StashTileForCarry(const std::string& a_key)
        {
            if (g_stash) return false;   // one cursor, one stash
            int idx = -1;
            for (std::size_t i = 0; i < g_items.size(); ++i) {
                if (g_items[i].key == a_key) { idx = static_cast<int>(i); break; }
            }
            if (idx < 0) return false;
            for (const auto& v : g_views) {
                if (std::find(v.items.begin(), v.items.end(), idx) == v.items.end()) {
                    continue;
                }
                if (ViewCountsSpace(v)) {
                    g_spaceUsed -= MaskCells(g_items[static_cast<std::size_t>(idx)].mask.rows);
                }
                break;
            }
            g_stash = std::move(g_items[static_cast<std::size_t>(idx)]);
            g_items.erase(g_items.begin() + idx);
            for (auto& v : g_views) {
                std::erase(v.items, idx);
                for (int& i : v.items) {
                    if (i > idx) --i;
                }
            }
            ++g_boardVersion;
            MarkCapacityDirty();
            SKSE::log::info("[SPACE] lift '{}' -> cursor -- no rebuild", a_key);
            return true;
        }

        bool UnstashTileTo(const std::string& a_bag, int a_col, int a_row,
                           int a_rot, int a_count)
        {
            if (!g_stash) return false;
            int vi = -1;
            for (std::size_t i = 0; i < g_views.size(); ++i) {
                if (g_views[i].bagKey == a_bag) { vi = static_cast<int>(i); break; }
            }
            if (vi < 0) return false;   // destination view missing: rebuild
            // a_col < 0 = no cell named (the trash park's whole-tile intake):
            // first-fit into the destination view, same as the placer would
            if (a_col < 0) {
                auto        occ = ViewOccOf(g_views[static_cast<std::size_t>(vi)]);
                LayoutEntry seat;
                seat.rot = g_stash->rot;
                if (!OccPlace(occ, g_stash->def, seat)) return false;
                a_col = seat.col;
                a_row = seat.row;
                a_rot = seat.rot;
                if (a_bag.empty() &&
                    !OwnedFootprint(a_col, a_row,
                        MaskOf(g_stash->def, a_rot & 3))) {
                    return false;   // growth zone (past the owned cells -- W3)
                }
                PlaceTile(g_stash->key, a_col, a_row, a_bag, a_count, a_rot);
            }
            Item it = std::move(*g_stash);
            g_stash.reset();
            it.col = a_col;
            it.row = a_row;
            it.inBag = a_bag;
            it.count = a_count;
            const int rot = CanRotate(it.def) ? (a_rot & 3) : 0;
            if (rot != it.rot) {
                it.rot = rot;
                it.mask = MaskOf(it.def, rot);
            }
            const std::string key = it.key;
            g_items.push_back(std::move(it));
            auto& v = g_views[static_cast<std::size_t>(vi)];
            v.items.push_back(static_cast<int>(g_items.size()) - 1);
            if (ViewCountsSpace(v)) {
                g_spaceUsed += MaskCells(g_items.back().mask.rows);
            }
            ++g_boardVersion;
            MarkCapacityDirty();
            SKSE::log::info("[SPACE] put '{}' at [{},{}]{} -- no rebuild",
                key, a_col, a_row, a_bag.empty() ? "" : " (bag)");
            return true;
        }

        bool UnstashTileHome()
        {
            if (!g_stash) return false;
            const auto li = g_layout.find(g_stash->key);
            if (li == g_layout.end() || li->second.col < 0) return false;
            // growth-zone spots are temporary (MakeDisplayTile's rule)
            if (li->second.bag.empty() &&
                !OwnedFootprint(li->second.col, li->second.row,
                    MaskOf(g_stash->def, li->second.rot & 3))) {
                return false;
            }
            return UnstashTileTo(li->second.bag, li->second.col, li->second.row,
                                 li->second.rot, g_stash->count);
        }

        void DiscardStash(const char* a_why)
        {
            if (!g_stash) return;
            SKSE::log::info("[SPACE] stash '{}' spent ({})", g_stash->key, a_why);
            g_stash.reset();
        }

        bool SetTileDisplayCount(const std::string& a_key, int a_count)
        {
            for (auto& it : g_items) {
                if (it.key != a_key) continue;
                it.count = a_count;
                MarkCapacityDirty();
                return true;
            }
            return false;
        }

        bool MoveTileToView(const std::string& a_key, const std::string& a_bag,
                            int a_col, int a_row)
        {
            int idx = -1;
            for (std::size_t i = 0; i < g_items.size(); ++i) {
                if (g_items[i].key == a_key) { idx = static_cast<int>(i); break; }
            }
            if (idx < 0) return false;
            int svi = -1, dvi = -1;
            for (std::size_t i = 0; i < g_views.size(); ++i) {
                if (svi < 0 && std::find(g_views[i].items.begin(), g_views[i].items.end(),
                                         idx) != g_views[i].items.end()) {
                    svi = static_cast<int>(i);
                }
                if (dvi < 0 && g_views[i].bagKey == a_bag) dvi = static_cast<int>(i);
            }
            if (svi < 0 || dvi < 0) return false;
            auto& it = g_items[static_cast<std::size_t>(idx)];
            auto& dst = g_views[static_cast<std::size_t>(dvi)];
            auto occ = ViewOccOf(dst, idx);
            LayoutEntry seat;
            seat.col = a_col;
            seat.row = a_row;
            seat.rot = it.rot;
            if (seat.col >= 0 && !OccFits(occ, seat.col, seat.row, it.mask)) {
                seat.col = -1;   // remembered spot taken: degrade to first-fit
                seat.row = -1;
            }
            if (seat.col < 0 && !OccPlace(occ, it.def, seat)) {
                return false;    // no room in the destination view: rebuild
            }
            // main-view growth rows are the rebuild's business (its own rule)
            if (a_bag.empty() &&
                !OwnedFootprint(seat.col, seat.row,
                    MaskOf(it.def, seat.rot & 3))) {
                return false;
            }
            auto& src = g_views[static_cast<std::size_t>(svi)];
            std::erase(src.items, idx);
            dst.items.push_back(idx);
            if (ViewCountsSpace(src) && !ViewCountsSpace(dst)) {
                g_spaceUsed -= MaskCells(it.mask.rows);
            } else if (!ViewCountsSpace(src) && ViewCountsSpace(dst)) {
                g_spaceUsed += MaskCells(it.mask.rows);
            }
            it.col = seat.col;
            it.row = seat.row;
            it.inBag = a_bag;
            if (CanRotate(it.def) && (seat.rot & 3) != it.rot) {
                it.rot = seat.rot & 3;
                it.mask = MaskOf(it.def, it.rot);
            }
            PlaceTile(a_key, seat.col, seat.row, a_bag, it.count, it.rot);
            ++g_boardVersion;
            MarkCapacityDirty();
            SKSE::log::info("[SPACE] move '{}' -> [{},{}]{} -- no rebuild",
                a_key, seat.col, seat.row,
                a_bag == kTrashKey ? " (trash)" : (a_bag.empty() ? "" : " (bag)"));
            return true;
        }

        void RunQueuedViewMoves()
        {
            if (g_viewMoveQ.empty()) return;
            for (const auto& r : g_viewMoveQ) {
                if (!MoveTileToView(r.key, r.bag, r.col, r.row)) {
                    SKSE::log::info("[SPACE] queued move '{}' declined -- rebuild",
                        r.key);
                    RequestRebuild();
                }
            }
            g_viewMoveQ.clear();
        }

        // ★S-G: one tile off the board, the lean way -- layout, marks, then
        // the display if it is up (a closed menu has no display to fix, and
        // the layout was the point). No drain-hint spend, no optimistic
        // claim: those belong to CLICK paths.
        bool RemoveTileLean(const std::string& a_key)
        {
            g_layout.erase(a_key);
            g_prevKeys.erase(a_key);
            g_newTiles.erase(a_key);
            int idx = -1;
            for (std::size_t i = 0; i < g_items.size(); ++i) {
                if (g_items[i].key == a_key) { idx = static_cast<int>(i); break; }
            }
            if (idx < 0) return false;
            auto& t = g_items[static_cast<std::size_t>(idx)];
            for (const auto& v : g_views) {
                if (std::find(v.items.begin(), v.items.end(), idx) == v.items.end()) {
                    continue;
                }
                if (ViewCountsSpace(v)) g_spaceUsed -= MaskCells(t.mask.rows);
                break;
            }
            g_items.erase(g_items.begin() + idx);
            for (auto& v : g_views) {
                std::erase(v.items, idx);
                for (int& i2 : v.items) {
                    if (i2 > idx) --i2;
                }
            }
            ++g_boardVersion;
            return true;
        }

        void RefreshPoolFlagsFor(RE::TESBoundObject* a_obj)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!a_obj || !player) return;
            auto* entry = LiveEntryOf(player, a_obj);
            for (auto& it : g_items) {
                if (it.obj != a_obj) continue;
                it.fav    = PoolHasStar(entry, it.uid, it.sig);
                it.stolen = PoolIsStolen(entry, it.uid, it.sig);
                it.quest  = PoolIsQuest(entry, it.uid, it.sig);
            }
        }

        // ---- ★S2: the expiry's recovery, one tile wide ---------------------
        // The two-phase drop kept the SLOT (CancelSlotDrop); this puts the
        // unit back on it without re-deriving the board. Everything unproven
        // -- a drained stackable (its layout entry died with the request), a
        // coin, a bag, an occupied seat -- returns false and the caller runs
        // the rebuild, exactly as before S2.
        bool ReEmitTileAt(std::uint32_t a_form, const std::string& a_key)
        {
            if (a_key.empty()) return false;
            const auto li = g_layout.find(a_key);
            if (li == g_layout.end() || li->second.col < 0) return false;
            // a stackable partial: the display counted down at request time
            // and the layout kept the full number -- resync upward, done
            for (auto& it : g_items) {
                if (it.key != a_key) continue;
                if (li->second.count > it.count) it.count = li->second.count;
                MarkCapacityDirty();
                return true;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) return false;
            auto* form = RE::TESForm::LookupByID(a_form);
            auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!obj) return false;
            if (obj->IsGold() || GoldCoins::IsCoinForm(a_form)) return false;   // §7
            const GridDef gdef = g_resolver ? g_resolver(obj) : GridDef{};
            if (gdef.bag != 0) return false;   // bag wiring: the rebuild's business
            auto inv = player->GetInventory(
                [&](RE::TESBoundObject& o) { return &o == obj; });
            int                     count = 0;
            RE::InventoryEntryData* entry = nullptr;
            for (auto& [o2, d2] : inv) {
                count = d2.first;
                entry = d2.second.get();
            }
            if (count <= 0 || !entry) return false;
            int vi = -1;
            for (std::size_t i = 0; i < g_views.size(); ++i) {
                if (g_views[i].bagKey == li->second.bag) { vi = static_cast<int>(i); break; }
            }
            if (vi < 0) return false;
            const Mask m = MaskOf(gdef, CanRotate(gdef) ? (li->second.rot & 3) : 0);
            if (!OccFits(ViewOccOf(g_views[static_cast<std::size_t>(vi)]),
                         li->second.col, li->second.row, m)) {
                return false;
            }
            if (li->second.bag.empty() &&
                !OwnedFootprint(li->second.col, li->second.row, m)) {
                return false;
            }
            std::uint8_t glow = 0;
            if (const auto* ef = obj->As<RE::TESEnchantableForm>();
                ef && ef->formEnchanting) {
                glow |= 1;
            }
            if (IsUniqueCached(obj)) glow |= 2;
            MakeDisplayTile(obj, entry, gdef, glow, a_key,
                            (std::max)(1, li->second.count), li->second,
                            li->second.xlIdx);
            auto& v = g_views[static_cast<std::size_t>(vi)];
            v.items.push_back(static_cast<int>(g_items.size()) - 1);
            if (ViewCountsSpace(v)) {
                g_spaceUsed += MaskCells(g_items.back().mask.rows);
            }
            g_prevKeys.insert(a_key);   // not NEW: it never really left
            ++g_boardVersion;
            MarkCapacityDirty();
            SKSE::log::info("[SPACE] re-emit '{}' after expiry -- no rebuild", a_key);
            return true;
        }
    }

    bool ClaimOptimisticRemove(RE::FormID a_form)
    {
        return g_optimisticGone.erase(a_form) > 0;   // see g_optimisticGone
    }

    void DropTileDisplay(const std::string& a_key, RE::TESBoundObject* a_obj)
    {
        // ★B4-4: one tile leaves the DISPLAY, nothing else -- the ring
        // router's replacement for its tail rebuild. Rule 13 (ForgetTile)
        // already handled the layout at the call site; a key that is not
        // displayed (the doll-drop path removed it at lift) is a no-op, which
        // is exactly what lets both ring paths share this line. Runs from
        // Tick, before the frame draws -- no deferral needed.
        (void)TryUseClickPartialRemove(a_key, a_obj,
                                       (std::numeric_limits<int>::max)(),
                                       /*a_drained=*/false);
    }

    bool CarrierCarryActive()
    {
        return g_held && g_held->fromCarrier;
    }

    int GoldAmount()
    {
        return g_gold;
    }

    // ---- ★S-G: gold's only mechanisms -----------------------------------
    // The ledger moved, so the TILES move. Income fills the rear-most
    // partial tile (the decomposition's remainder, now an owned slot) and
    // mints capfuls for the rest; a spend debits partials before full
    // thousands, rear board position first. The LAYOUT is the book -- the
    // display follows when it is up, and the col -1 mints are seated by the
    // rebuild the request flag buys. Main thread only (GoldCoins::Tick).

    void CoinIncome(int a_value)
    {
        if (a_value <= 0) return;
        auto* cform = GoldCoins::CoinForTier(0);
        if (!cform) return;
        int left = a_value;
        auto slots = CoinTilesByPosition();   // cursor money excluded by design
        for (auto it = slots.rbegin(); it != slots.rend() && left > 0; ++it) {
            if (it->value >= GoldCoins::kCoinCap) continue;
            const auto li = g_layout.find(it->key);
            if (li == g_layout.end()) continue;
            const int add = (std::min)(GoldCoins::kCoinCap - li->second.coin, left);
            if (add <= 0) continue;
            li->second.coin += add;
            left -= add;
            bool shown = false;
            for (auto& gi : g_items) {
                if (gi.key == it->key) {
                    gi.coinValue = li->second.coin;
                    shown = true;
                    break;
                }
            }
            if (!shown) RequestRebuild();   // closed menu: the board is stale
            SKSE::log::info("[GOLD] +{} G -> '{}' ({} G)", add, it->key,
                li->second.coin);
            break;   // one remainder fills; the rest arrives as fresh capfuls
        }
        bool minted = false;
        while (left > 0) {
            const int n = (std::min)(GoldCoins::kCoinCap, left);
            const std::string key = NextTileKey(FormKey(cform));
            auto& le = g_layout[key];
            le.col = -1;   // the placer seats it
            le.row = -1;
            le.coin = n;
            le.count = 1;
            left -= n;
            minted = true;
            SKSE::log::info("[GOLD] +{} G minted as '{}'", n, key);
        }
        MarkCapacityDirty();
        ++g_boardVersion;
        if (minted) RequestRebuild();
    }

    void CoinSpend(int a_value)
    {
        if (a_value <= 0) return;
        auto slots = CoinTilesByPosition();
        std::vector<const CoinSlot*> order;
        order.reserve(slots.size());
        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            if (it->value < GoldCoins::kCoinCap) order.push_back(&*it);
        }
        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            if (it->value >= GoldCoins::kCoinCap) order.push_back(&*it);
        }
        int left = a_value;
        for (const auto* s : order) {
            if (left <= 0) break;
            const auto li = g_layout.find(s->key);
            if (li == g_layout.end() || li->second.coin <= 0) continue;
            const int take = (std::min)(li->second.coin, left);
            left -= take;
            if (take < li->second.coin) {
                li->second.coin -= take;
                bool shown = false;
                for (auto& gi : g_items) {
                    if (gi.key == s->key) {
                        gi.coinValue = li->second.coin;
                        shown = true;
                        break;
                    }
                }
                if (!shown) RequestRebuild();   // closed menu: the board is stale
                SKSE::log::info("[GOLD] spend: '{}' pays {} -> {} G",
                    s->key, take, li->second.coin);
            } else {
                if (!RemoveTileLean(s->key)) RequestRebuild();
                SKSE::log::info("[GOLD] spend: '{}' emptied (-{} G)", s->key, take);
            }
        }
        MarkCapacityDirty();
        ++g_boardVersion;
        if (left > 0) {
            // Not a fault: a purchase priced past the tiles is PAID BY THE
            // POUCHES (user-confirmed rule) -- the pouch trim in the same
            // tick is that payment. Only a shortfall with no pouch to cover
            // it is census territory.
            SKSE::log::info("[GOLD] spend outruns the tiles by {} G -- the "
                            "pouches pay the rest", left);
        }
    }

    void CoinCensus(const char* a_why)
    {
        // one invariant replaces the mirror: Σ tile amounts == ledger − pouch.
        // Never squared while our own transfers are in flight -- their tile
        // half and ledger half land on different frames by design.
        if (GoldCoins::UnsettledDelta() != 0) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* gold = GoldCoins::VanillaGold();
        if (!p || !gold) return;
        int ledger = 0;
        {
            auto inv = p->GetInventory(
                [&](RE::TESBoundObject& o) { return &o == gold; });
            for (auto& [o2, d2] : inv) ledger = d2.first;
        }
        int tiles = 0;
        for (const auto& [k, le] : g_layout) {
            if (le.coin > 0) tiles += le.coin;
        }
        const int target = (std::max)(0, ledger - GoldCoins::PouchStored());
        const int diff = tiles - target;
        if (diff == 0) return;
        SKSE::log::warn("[GOLD] ★census ({}): tiles {} G vs ledger-share {} G "
                        "-- {} {} G", a_why, tiles, target,
            diff > 0 ? "trimming" : "minting", diff > 0 ? diff : -diff);
        if (diff > 0) CoinSpend(diff);
        else          CoinIncome(-diff);
    }

    // ★S-G: NotePaidGold is retired -- a payment debits named coin tiles
    // (CoinSpend), so nothing is guessed for the spill pass any more.

    void NotePendingEquip(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                          std::uint16_t a_sig, int a_hand, const std::string& a_srcKey,
                          int a_units, int a_xlIdx)
    {
        if (!a_obj) return;
        // mayBeWorn = TRUE: this unit is mid-transition. Before the engine
        // applies the equip it is still on the board and must come out of the
        // set; once applied it is already out via skipWorn, and removing it a
        // second time ate an innocent SPARE of the same pool. Matching a worn
        // list first is exactly the "has it landed yet?" test.
        // srcKey: hold the cell it left open for these frames. Without it the
        // pool re-packed the survivors into the front slots, so the tile that
        // vanished was whichever sorted last -- not the one the player clicked.
        // It came back one frame later, when the applied equip finally forgot
        // the real cell. That round trip is the "spare dagger blinks" report.
        // ★★★FIELD-WISE, NEVER BRACED. This record used to be built with a
        // nine-value aggregate initialiser while OffBoardUnit had ELEVEN fields
        // -- `xlIdx` was appended before `units`, and nobody re-counted. So the
        // unit COUNT was silently written into the list POSITION: every queued
        // equip claimed to live at extraLists[1], and the exclusion matched
        // whatever happened to sit there. Equipping a plain dagger removed the
        // TEMPERED one from the board, and its cell then showed the wrong item.
        // `units` meanwhile stayed 1, so a hundred-arrow quiver suppressed a
        // single arrow -- the very bug the field was added to fix.
        //
        // Nothing about that is visible at the call site, and the compiler
        // cannot see it either. Naming every field is the only form of this
        // code that cannot silently rot when the struct grows again.
        OffBoardUnit eq;
        eq.base      = FormKey(a_obj);
        eq.uid       = a_uid;
        eq.sig       = a_sig;
        eq.why       = "equipping";
        eq.mayBeWorn = true;
        eq.hand      = a_hand;
        eq.srcKey    = a_srcKey;
        eq.arriving  = true;
        eq.xlIdx     = a_xlIdx;
        eq.units     = (std::max)(1, a_units);
        // Same type set as WornLedger::Tracked: these forms get an equip
        // event, so they have real retirement paths and the applied-erase
        // must leave them alone.
        eq.confirmable = a_obj->Is(RE::FormType::Armor) ||
                         a_obj->Is(RE::FormType::Weapon) ||
                         a_obj->Is(RE::FormType::Light) ||
                         a_obj->Is(RE::FormType::Ammo);
        g_pendingEquip.push_back(std::move(eq));
        g_pendingEquipWhen = std::chrono::steady_clock::now();
        // B4-2b: the worn ledger hears the same request, with the same
        // identity, at the same moment (rule 2: this is when the unit is
        // known). Its own type filter drops the consumables that pass
        // through here on the use path.
        WornLedger::NotePending(a_obj->GetFormID(), a_uid, a_sig, a_hand,
                                (std::max)(1, a_units));
    }

    void ClearPendingEquips()
    {
        g_pendingEquip.clear();
        // same lifetime: a click queued in the frame the menu closed has no
        // board to act on any more
        g_clickAction.reset();
    }

    void ReleaseAppliedPendingEquip(std::uint32_t a_form)
    {
        auto* form = RE::TESForm::LookupByID(a_form);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!obj) return;
        const std::string base = FormKey(obj);
        // One event, one unit: erase the OLDEST applied entry of this base.
        // The surplus half of the consumable pair never matches a request
        // (Ledger), so it never reaches here -- one release per actual use.
        for (auto it = g_pendingEquip.begin(); it != g_pendingEquip.end(); ++it) {
            if (it->applied && it->base == base) {
                g_pendingEquip.erase(it);
                return;
            }
        }
    }

    void MarkEquipsApplied()
    {
        for (auto& u : g_pendingEquip) u.applied = true;
    }

    void ReleasePendingEquipFor(RE::FormID a_form)
    {
        auto* form = RE::TESForm::LookupByID(a_form);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!obj) return;
        const std::string base = FormKey(obj);
        for (auto it = g_pendingEquip.begin(); it != g_pendingEquip.end(); ++it) {
            if (it->base == base && it->arriving) {
                g_pendingEquip.erase(it);
                return;
            }
        }
    }

    void ReleaseLandedPendingEquip(RE::FormID a_form)
    {
        // ★An UNEQUIP event for a form whose equip already LANDED closes that
        // record's whole story: the unit arrived on the body and has now left
        // it, so the suppression has nothing more to suppress. Without this,
        // a rapid same-form swap run left landed records whose worn lists
        // were gone before the match-release saw them -- and the record then
        // hid an innocent unit until a sweep that (after B4-1) might never
        // run. Oldest landed entry of the form, one per event, same FIFO
        // discipline as everything else the events drive.
        auto* form = RE::TESForm::LookupByID(a_form);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!obj) return;
        const std::string base = FormKey(obj);
        for (auto it = g_pendingEquip.begin(); it != g_pendingEquip.end(); ++it) {
            if (it->base == base && it->landed) {
                g_pendingEquip.erase(it);
                return;
            }
        }
    }

    void NoteEquipLanded(RE::FormID a_form)
    {
        // B4-2c: the engine's own confirm, one event landing one entry --
        // oldest first, the order the engine ran them (the same discipline
        // rule 1 demands of the container ledger). Form-wide match: the event
        // names no unit (rule 2), and the queue holds our requests in order.
        auto* form = RE::TESForm::LookupByID(a_form);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!obj) return;
        const std::string base = FormKey(obj);
        for (auto& u : g_pendingEquip) {
            if (u.base == base && u.arriving && !u.landed) {
                u.landed = true;
                return;
            }
        }
    }


    // GI32: run queued favourite syncs on the GAME thread (UIRoot::Tick).
    //
    // GI34: and do the attach OURSELVES. The engine's SetFavorite works at ENTRY
    // scope, not unit scope: FavoritesMenu::Entry is { TESForm*,
    // InventoryEntryData* } with nowhere to record WHICH variant, so the engine
    // keeps at most one hotkey per entry and silently refuses a second. That is
    // also why vanilla cannot separate a plain dagger from a tempered one --
    // IsFavorited() is true when ANY list in the entry carries a hotkey, so
    // vanilla just draws the star on every row of that form. It looks like it
    // distinguishes them; it does not.
    //
    // A tile here resolves to its exact ExtraDataList, so we can be right where
    // vanilla is not: attach the ExtraHotkey to that one list. ExtraHotkey is a
    // stock, serialised extra with a public ctor and a game-heap new/delete, so
    // saves, the Q menu and other mods keep reading it exactly as before.
    //
    // The one thing only the engine can do is favourite a unit that owns NO
    // list, because that unit has to be split off the stack first -- and it
    // refuses to split while the entry already carries a hotkey. So lift the
    // other hotkeys for the duration of that one call and put them straight back.
    // ★The doll's and the drawer's way in. A board tile has a key and a list
    // index; a WORN unit has neither -- it owns no cell, and its position in
    // the entry shifts every time something is equipped. uid+sig names it
    // exactly, and ProcessFavorites resolves the pool from that.
    void ToggleFavoriteUnit(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                            std::uint16_t a_sig, int a_hand)
    {
        if (!a_obj) return;
        g_favSync.push_back({ a_obj, a_uid, -1, a_sig, /*worn=*/true, a_hand });
    }

    void ProcessFavorites()
    {
        if (g_favSync.empty()) return;
        auto q = std::move(g_favSync);
        g_favSync.clear();
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* changes = p ? p->GetInventoryChanges() : nullptr;
        if (!changes || !changes->entryList) return;
        for (const auto& f : q) {
            // tripwire witness: this form's star state is changing on purpose
            if (f.obj) g_starChangeOk.insert(f.obj->GetFormID());
            for (auto* entry : *changes->entryList) {
                if (!entry || entry->object != f.obj) continue;
                const std::string base = FormKey(f.obj);
                // The pool a list belongs to, by CONTENT -- never by pointer.
                // RemoveFavorite/SetFavorite create and destroy lists, so a
                // pointer captured before a call may be freed by it (this is
                // what crashed: RemoveByType deleting an ExtraHotkey the engine
                // had already reclaimed). Pool keys survive that.
                auto poolOf = [&base](RE::ExtraDataList* a_xl) {
                    std::uint16_t u = 0;
                    if (const auto* xu = a_xl->GetByType<RE::ExtraUniqueID>()) u = xu->uniqueID;
                    return PoolPrefix(base, u, InstanceSig(a_xl));
                };
                // Every hotkey removal goes through the engine. We only ever ADD
                // an ExtraHotkey ourselves -- deleting one we did not create is
                // how the crash happened.
                auto clearPools = [&](const std::vector<std::string>& a_pools) {
                    for (bool again = true; again; ) {
                        again = false;
                        if (!entry->extraLists) break;
                        for (auto* x : *entry->extraLists) {
                            if (!x || !x->HasType<RE::ExtraHotkey>()) continue;
                            if (std::find(a_pools.begin(), a_pools.end(), poolOf(x)) ==
                                a_pools.end()) continue;
                            changes->RemoveFavorite(entry, x);
                            again = true;   // the walk is invalid now, restart
                            break;
                        }
                    }
                };

                auto* xl = ExtraForTile(entry, f.uid, f.xlIdx);
                // ★★A DOLL REQUEST RESOLVES TO THE WORN LIST, and it must do
                // so BEFORE the pool ask below. ExtraForPool refuses worn
                // lists by design (that guard keeps a sale off the body), so
                // a worn unit whose signature no spare shares -- an enchanted
                // weapon with charge spent -- matched nothing, fell to
                // SetFavorite(entry, nullptr), and the engine minted a fresh
                // {Hotkey} list: a phantom unit with no charge extra, which
                // the board drew as a second, FULL copy of the weapon, and
                // dropping the pair could shed the real list's ExtraCharge
                // (user report: favorite-while-worn + unequip duplicates).
                // A hotkey ON the worn list is ordinary engine state -- the
                // engine itself carries hotkeys onto worn lists at equip --
                // and it is what vanilla's own menu does for an equipped item.
                if (!xl && f.worn) {
                    xl = WornExtraMatching(entry, f.uid, f.sig, f.hand);
                }
                // ★No position to resolve from (a doll or drawer slot): ask the
                // pool by signature, which is the grain the star works at.
                if (!xl && f.sig != 0) xl = ExtraForPool(entry, f.uid, f.sig);
                // GI40: read the POOL, the same unit the star is drawn from.
                // Asking this one list said "off" whenever the pool's hotkey was
                // sitting on a SIBLING list -- most often the worn one, after
                // equipping split the pool -- so F added a second hotkey and
                // nothing changed on screen.
                const std::uint16_t tsig = xl ? InstanceSig(xl) : 0;
                const bool on = PoolHasStar(entry, f.uid, tsig);
                // ★★Tell the wheel the moment the star comes off, not the next
                // time it happens to look. It re-reads the favourites only when
                // it opens, so unstar-and-restar inside one inventory visit was
                // invisible to it and the item returned to the slot it had been
                // dragged to -- while the same two clicks either side of opening
                // the wheel put it at the front. The act reports itself now.
                if (on) Wheeler::ForgetFavorite(f.obj->GetFormID());
                if (on && !xl) {
                    // ★Turning a pool off that has NO list of its own. Naming it
                    // would match nothing and the toggle would jam in the "on"
                    // position, so every star on the entry comes off instead.
                    // A toggle that cannot be untoggled is the one outcome worth
                    // avoiding here.
                    // ★This is now the rare path, not the plain pool's normal
                    // one: lifting the other stars before SetFavorite gets the
                    // engine to mint a list for a plain unit too, so it usually
                    // owns one and takes the precise branch below. Reached only
                    // when that failed -- and then we do not know which list
                    // holds this pool's mark, so coarse is the honest answer.
                    std::vector<std::string> all;
                    if (entry->extraLists) {
                        for (auto* x : *entry->extraLists) {
                            if (x && x->HasType<RE::ExtraHotkey>()) all.push_back(poolOf(x));
                        }
                    }
                    clearPools(all);
                } else if (on) {
                    clearPools({ PoolPrefix(base, f.uid, tsig) });
                } else if (xl) {
                    xl->Add(new RE::ExtraHotkey(RE::ExtraHotkey::Hotkey::kUnbound));
                } else {
                    // No list of its own, and the pool has no star anywhere else.
                    // Only the engine can split the unit off the stack, and it
                    // refuses while the entry already carries a hotkey -- so lift
                    // the others across the call.
                    std::vector<std::string> lifted;
                    if (entry->extraLists) {
                        for (auto* x : *entry->extraLists) {
                            if (x && x->HasType<RE::ExtraHotkey>()) lifted.push_back(poolOf(x));
                        }
                    }
                    clearPools(lifted);
                    // ★★SetFavorite's second parameter names the UNIT, and null
                    // is the only honest value for a plain unit: it has no list
                    // to point at, which is the whole reason this branch exists.
                    // The engine then picks for itself, and MEASUREMENT settled
                    // what it picks -- it mints a fresh list only when the entry
                    // has none at all; with even one variant present it writes
                    // into that variant's list instead. Calling again does not
                    // move it along either (verified: a second call is refused
                    // outright while any star exists).
                    //
                    // So there is no way to aim this call at a plain unit, and
                    // the star it produces is ACCEPTED where it lands rather
                    // than reverted. Reverting was tried first and it removed
                    // the wrong thing -- it left the player unable to favourite
                    // an ordinary dagger at all, which is worse than the star
                    // being coarse. PoolHasStar reads any entry star as the
                    // plain pool's, so the tile the player pointed at does light
                    // up; its variant sibling lights up with it. Same dagger,
                    // one mark between them.
                    changes->SetFavorite(entry, nullptr);
                    // Re-walk the CURRENT list and restore by POOL, so a list
                    // the split rebuilt is matched by what it holds, not by an
                    // address that may no longer mean anything.
                    if (entry->extraLists && !lifted.empty()) {
                        for (auto* x : *entry->extraLists) {
                            if (!x || x->HasType<RE::ExtraHotkey>()) continue;
                            if (std::find(lifted.begin(), lifted.end(), poolOf(x)) ==
                                lifted.end()) continue;
                            x->Add(new RE::ExtraHotkey(RE::ExtraHotkey::Hotkey::kUnbound));
                        }
                    }
                }
                // ★Back behind the trace switch. It was unconditional while the
                // question was open, and it answered it: the engine mints a list
                // only for an entry that has none, so "via=engine" on an entry
                // with variants always lands on a sibling. Nothing left to catch
                // here every press.
                if (g_poolTrace) {
                    std::string ls;
                    if (entry->extraLists) {
                        for (auto* x2 : *entry->extraLists) {
                            if (!x2) continue;
                            std::uint16_t u = 0;
                            if (const auto* xu = x2->GetByType<RE::ExtraUniqueID>()) {
                                u = xu->uniqueID;
                            }
                            ls += std::format("[{}{}] ",
                                PoolPrefix(FormKey(f.obj), u, InstanceSig(x2)),
                                x2->HasType<RE::ExtraHotkey>() ? " HOT" : "");
                        }
                    }
                    SKSE::log::info("[FAV] toggle uid {:04X} xl {} was={} via={}"
                                    " asked='{}' hit='{}' | {}",
                        f.uid, f.xlIdx, on ? "on" : "off",
                        xl ? "self" : "engine",
                        PoolPrefix(base, f.uid, tsig),
                        xl ? poolOf(xl) : std::string("-"),
                        ls.empty() ? "-" : ls);
                }
                break;
            }
        }
        // GI33: the star is read back OUT of the engine once the change has
        // actually landed. ★S1: read back IN PLACE -- the star (and the other
        // two pool-derived marks) refresh on this form's own tiles, and the
        // full rebuild this line used to ask for is gone. Runs on the Tick,
        // outside the draw, so mutating Item fields is safe.
        for (const auto& f : q) RefreshPoolFlagsFor(f.obj);
    }

    // ★A SAVE CAN ALREADY CARRY THE PHANTOM. Before the doll's favorite
    // request learned to name the worn list (FavSync::worn), starring an
    // equipped item whose signature no spare shared fell through to
    // SetFavorite(entry, nullptr), and the engine minted a {Hotkey}-only
    // list beside the real unit -- one item, two lists, two tiles, and a
    // drop path that could shed the real list's ExtraCharge for good.
    // The prevention above stops new ones; this retires the ones a save
    // brought along. The tell is arithmetic, the same one the GI2 clamp
    // warns on: the entry's lists claim more units than the entry holds,
    // and one of the lists carries NOTHING but the hotkey. Removal goes
    // through the engine (RemoveFavorite retires the emptied list), and
    // the star the player set is put back on a real unit -- the worn
    // list first, since favorite-while-worn is how the phantom was born.
    void HealPhantomHotkeyLists()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* changes = p ? p->GetInventoryChanges() : nullptr;
        if (!p || !changes || !changes->entryList) return;
        // counts come from the same walk the board trusts; the entries in
        // changes->entryList only know their delta against the container
        std::map<RE::TESBoundObject*, int> counts;
        for (auto& [obj, pair] : p->GetInventory()) {
            if (obj) counts[obj] = pair.first;
        }
        bool healed = false;
        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object || !entry->extraLists) continue;
            const auto ci = counts.find(entry->object);
            if (ci == counts.end() || ci->second <= 0) continue;
            int                 listed  = 0;
            RE::ExtraDataList*  phantom = nullptr;
            RE::ExtraDataList*  worn    = nullptr;
            RE::ExtraDataList*  spare   = nullptr;
            bool                starredElsewhere = false;
            for (auto* xl : *entry->extraLists) {
                if (!xl) continue;
                listed += (std::max)(1, xl->GetCount());
                // "nothing but the hotkey": every extra on the list is the
                // hotkey itself. A worn or renamed or counted list fails
                // this by carrying its other extra, which is the point --
                // those are real units.
                bool onlyHotkey = xl->HasType<RE::ExtraHotkey>();
                if (onlyHotkey) {
                    for (const auto& x : *xl) {
                        if (x.GetType() != RE::ExtraDataType::kHotkey) {
                            onlyHotkey = false;
                            break;
                        }
                    }
                }
                if (onlyHotkey) {
                    // one phantom per pass; a second hotkey-only list still
                    // holds a star but is no home for one -- never `spare`
                    if (!phantom) phantom = xl;
                    else starredElsewhere = true;
                    continue;
                }
                if (xl->HasType<RE::ExtraWorn>() ||
                    xl->HasType<RE::ExtraWornLeft>()) {
                    if (!worn) worn = xl;
                } else if (!spare) {
                    spare = xl;
                }
                starredElsewhere =
                    starredElsewhere || xl->HasType<RE::ExtraHotkey>();
            }
            // Real inconsistency only: a lone {Hotkey} list with the counts
            // in agreement is an ordinary favorited plain unit. And never
            // touch an entry whose only list IS the phantom -- with nothing
            // to carry the unit, removing it would orphan the star's owner.
            if (!phantom || listed <= ci->second || (!worn && !spare)) continue;
            changes->RemoveFavorite(entry, phantom);
            if (!starredElsewhere) {
                auto* home = worn ? worn : spare;
                home->Add(new RE::ExtraHotkey(RE::ExtraHotkey::Hotkey::kUnbound));
            }
            healed = true;
            g_starChangeOk.insert(entry->object->GetFormID());   // tripwire witness
            SKSE::log::info(
                "[FAV] healed phantom hotkey list on '{}' (count {} < listed {},"
                " star -> {})",
                entry->object->GetName(), ci->second, listed,
                starredElsewhere ? "already placed" : (worn ? "worn" : "spare"));
        }
        if (healed) RequestRebuild();
    }

    // GI36: resolve the sub-stack that is ACTUALLY leaving the bag, and drop its
    // star on the way out. Every outbound sink calls this instead of
    // ExtraForPool and hands the result straight to RemoveItem / the world drop.
    //
    // Rule 58: sold, stored, dropped, binned and planted all kill the star.
    // Equipping does not -- it comes back, so the star waits on the doll (55).
    //
    // Resolution and removal are ONE call on purpose. The previous attempt
    // cleared by POOL NAME from NotePendingRemove, and a pool name is a crowd:
    // three separately starred daggers all hash to the same pool, so storing one
    // stripped all three. A crowd cannot give up one member's star. Only the
    // list we are about to hand the engine can.
    //
    // NotePendingRemove is also the wrong MOMENT: it fires at request time, so a
    // failed pickpocket roll rolled the item back but not the star.
    //
    //   a_starred = how many of the a_count outgoing units wore a star. The
    //               caller always knows; the sink never can.
    // Returns nullptr for "let the engine pick" -- only ever within a pool whose
    // members are genuinely interchangeable.
    RE::ExtraDataList* ResolveExitUnit(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                       std::uint16_t a_sig, int a_count, int a_starred,
                                       int a_xlIdx)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !a_obj) return nullptr;
        // tripwire witness: rule 58 is about to take this form's star out
        if (a_starred > 0) g_starChangeOk.insert(a_obj->GetFormID());
        auto* changes = p->GetInventoryChanges();
        auto* entry   = LiveEntry(p, a_obj);
        if (!entry) return nullptr;
        auto* xl = ExtraForPoolImpl(entry, a_uid, a_sig);   // unchanged rules (worn excluded)
        // ★★AND THE ONE THE PLAYER ACTUALLY POINTED AT, when the caller knows
        // where it sits. A pool is a crowd: two identical daggers hash to one
        // signature, so the line above returns whichever list comes FIRST --
        // and that is why selling the front dagger sold the back one, and
        // binning one deleted the other. The cell the player clicked was never
        // part of the question.
        // ★The old rule ("never by list position, transfers run a frame later
        // and a captured position can be stale") was right about the risk and
        // wrong to answer it by giving up. The position is VERIFIED here
        // instead: it has to still hold a unit of the same pool, unworn. If
        // the list moved underneath us the check fails and this falls back to
        // exactly the old behaviour -- so a stale position can cost accuracy,
        // never correctness.
        if (a_xlIdx >= 0) {
            if (auto* byIdx = ExtraForTile(entry, a_uid, a_xlIdx)) {
                const bool wornX = byIdx->HasType<RE::ExtraWorn>() ||
                                   byIdx->HasType<RE::ExtraWornLeft>();
                if (!wornX && InstanceSig(byIdx) == a_sig) xl = byIdx;
            }
        }
        if (a_starred <= 0 || !changes) return xl;          // no star leaving: identical to before

        // GI37: pin a list open before stripping its star.
        //
        // A plain unit's list holds NOTHING but the ExtraHotkey, so removing the
        // star retires the list and takes our only handle with it. RemoveItem
        // then gets nullptr, picks for itself, and can walk a TEMPERED spare out
        // instead of the plain dagger that was clicked -- the two swap places on
        // the board. Targeting must survive the policy.
        //
        // ExtraCount is the anchor: InstanceSig does not hash it, so the pool key
        // (and the remembered cell with it) is unchanged, and the value we write
        // is the count the list already reports.
        auto anchor = [](RE::ExtraDataList* a_xl) {
            if (a_xl && !a_xl->HasType<RE::ExtraCount>()) {
                a_xl->Add(new RE::ExtraCount(
                    static_cast<std::int16_t>((std::max)(1, a_xl->GetCount()))));
            }
        };

        // (1) The pool NAMES one list => that is the unit going out, and its
        //     star is the only one we are entitled to touch.
        if (xl) {
            // Only part of a stack leaving? The survivors own that list, so the
            // star stays with them.
            const int listCount = (std::max)(1, xl->GetCount());
            if (!xl->HasType<RE::ExtraHotkey>() || a_count < listCount) return xl;
            anchor(xl);                        // GI37: keep the handle alive
            changes->RemoveFavorite(entry, xl);
            // RemoveFavorite can retire a list that just went empty: re-fetch the
            // entry and match by ADDRESS ONLY -- never dereference a dead pointer.
            entry = LiveEntry(p, a_obj);
            if (entry && entry->extraLists) {
                for (auto* x : *entry->extraLists) {
                    if (x == xl) return xl;
                }
            }
            return nullptr;   // merged back into the plain stack: any unit will do
        }

        // (2) PLAIN pool (uid 0, sig 0). Members are interchangeable by content
        //     and the star is the ONLY difference, so the only thing that has to
        //     come out right is how many stars remain. Clear at most a_starred,
        //     then hand the engine the list we just stripped so the unit that
        //     leaves is one that has already lost its star.
        int budget = a_starred;
        RE::ExtraDataList* freed = nullptr;
        RE::ExtraDataList* home = nullptr;   // starred list we may not strip
        while (budget > 0) {
            entry = LiveEntry(p, a_obj);
            if (!entry || !entry->extraLists) break;
            RE::ExtraDataList* hit = nullptr;
            for (auto* x : *entry->extraLists) {
                if (!x || !x->HasType<RE::ExtraHotkey>()) continue;
                if (x->HasType<RE::ExtraWorn>() ||
                    x->HasType<RE::ExtraWornLeft>()) continue;       // rule 55
                if (InstanceSig(x) != 0) continue;                   // a different pool
                if (const auto* xu = x->GetByType<RE::ExtraUniqueID>();
                    xu && xu->uniqueID != 0) continue;               // a uid pool
                if (!home) home = x;   // GI38: the unit lives HERE either way
                if ((std::max)(1, x->GetCount()) > budget) continue;  // survivors own it
                hit = x;
                break;
            }
            if (!hit) break;
            budget -= (std::max)(1, hit->GetCount());
            anchor(hit);                       // GI37: keep the handle alive
            changes->RemoveFavorite(entry, hit);
            freed = hit;
        }
        // Re-validate by ADDRESS against the current list before handing anything
        // back -- the calls above can retire a list.
        auto alive = [&](RE::ExtraDataList* a_x) -> RE::ExtraDataList* {
            if (!a_x) return nullptr;
            entry = LiveEntry(p, a_obj);
            if (!entry || !entry->extraLists) return nullptr;
            for (auto* x : *entry->extraLists) {
                if (x == a_x) return a_x;
            }
            return nullptr;
        };
        RE::ExtraDataList* out = alive(freed);
        // GI38: we may not be ALLOWED to strip the star -- several units share one
        // list, so taking its hotkey would rob the ones staying behind. That is a
        // reason to keep the star, NOT a reason to forget where the unit lives.
        // Returning nullptr here handed the choice to the engine, and it walked a
        // TEMPERED dagger out instead of the plain one that was clicked.
        // Correct targeting outranks the star policy: the star rides along.
        if (!out) out = alive(home);
        if (g_poolTrace) {
            std::string ls;
            if (entry && entry->extraLists) {
                for (auto* x : *entry->extraLists) {
                    if (!x) continue;
                    ls += std::format("[sig {:04X} n{}{}] ", InstanceSig(x),
                        (std::max)(1, x->GetCount()),
                        x->HasType<RE::ExtraHotkey>() ? " HOT" : "");
                }
            }
            SKSE::log::info(
                "[FAV] exit uid {:04X} sig {:04X} n={} starred={} cleared={} handed={} | {}",
                a_uid, a_sig, a_count, a_starred, a_starred - budget,
                out ? (out == freed ? "stripped" : "kept-star") : "engine",
                ls.empty() ? "-" : ls);
        }
        return out;
    }

    void NotePendingRemove(RE::TESBoundObject* a_obj, const std::string& a_key,
                           int a_count, int a_xlIdx)
    {
        if (!a_obj || a_count <= 0) return;
        const RE::FormID fid = a_obj->GetFormID();
        if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) return;   // coins: own system
        // ★(1.3.0-C) every UI exit path (store / sell / pick-store / trash)
        // funnels through here WITH its tile key -- name the leaving pouch
        // for the container sink, whose event only carries the form.
        if (GoldCoins::IsPouch(fid) && !a_key.empty()) GoldCoins::NotePouchLeaving(a_key);
        // ★B4-3c: the form/pool/position counters that used to arm here are
        // GONE -- the request ledger is the one set of books now. Every
        // caller submits its entry at the same commit (the Request funnels,
        // the trash confirm), measured to agree with the counters across two
        // phases (28 audits, zero mismatches) before the counters went.
        // What stays below is what was never theirs: the layout drain for a
        // stackable tile and the two-phase slot queue for gear.

        const GridDef gdef = g_resolver ? g_resolver(a_obj) : GridDef{};
        const int cap = EffectiveCap(a_obj, gdef);
        if (a_key.empty()) return;   // carried fragment: form-level only

        if (cap > 1) {
            // stackable: the sold/stored units leave THIS tile's remembered
            // quantity immediately — the reconciler then sees no gap, so no
            // other stack of the form is ever touched. Cancel-safe: this runs
            // on CONFIRM only. (The old key-mark scheme was cleared by
            // ProcessTransfers before the reconciler could consume it, which
            // let the generic drain order eat a different tile.)
            if (auto lt = g_layout.find(a_key); lt != g_layout.end()) {
                lt->second.count -= a_count;
                // 0 must ERASE, not persist — count 0 reads as "unspecified"
                // (legacy) and the tile would resurrect as a fresh pickup
                if (lt->second.count <= 0) g_layout.erase(lt);
            }
            return;
        }

        // ★★B3-b: DEFERRED, not done here. Dropping the slot at REQUEST time
        // is what made a refused store come back into the front gap: the cell
        // was already forgotten, so the rebuild minted a fresh slot and
        // first-fit it. The stackable branch above already knew this -- read its
        // comment, "Cancel-safe: this runs on CONFIRM only" -- and gear did not.
        //
        // The unit still disappears from the board the moment the player
        // commits: g_pendingRemovePool hides it. What survives is the CELL.
        //
        // What used to be here, for the record: g_layout.erase(a_key).
        //
        // This used to erase the key and then RE-KEY every surviving tile of the
        // form densely (#0..#n-1). That is what made the wrong cell empty: the
        // survivors were renumbered by their old ordinal while the next
        // enumeration handed out ordinals in walk order, and the two orders are
        // not the same thing. Pool assignment (see EnumerateUnitTiles) now maps
        // units to slots by POSITION, so removing a slot is the whole operation
        // -- the rest stay exactly where they are, and no key ever changes name.
        //
        // Losing the re-key also removes its bag hazard: a renamed bag key had to
        // drag g_openBags and every contents' inBag pointer along with it.
        g_pendingSlotDrop[fid].push_back(a_key);
    }

    // ★Both halves consume the same queue, and which half runs is the whole
    // difference between "the engine took it" and "the engine ignored us".
    // ★★By the request's OWN key, never by count off the front: two paths can
    // move the same form at once (a store pending while a stack is dropped to
    // the world), and popping "however many the confirmed delta said" handed
    // the drop's confirmation the store's key. A key that is not queued is a
    // no-op -- stackables and slotless requests never queued one.
    namespace
    {
        bool TakeQueuedSlot(std::uint32_t a_form, const std::string& a_slot)
        {
            if (a_slot.empty()) return false;
            const auto it = g_pendingSlotDrop.find(a_form);
            if (it == g_pendingSlotDrop.end()) return false;
            auto& v = it->second;
            const auto at = std::find(v.begin(), v.end(), a_slot);
            if (at == v.end()) return false;
            v.erase(at);
            if (v.empty()) g_pendingSlotDrop.erase(it);
            return true;
        }
    }

    void CommitSlotDrop(std::uint32_t a_form, const std::string& a_slot)
    {
        if (TakeQueuedSlot(a_form, a_slot)) g_layout.erase(a_slot);
    }

    void CancelSlotDrop(std::uint32_t a_form, const std::string& a_slot)
    {
        if (TakeQueuedSlot(a_form, a_slot)) {
            SKSE::log::warn("[GRID] slot '{}' KEPT -- its move was never confirmed",
                            a_slot);   // dropped from the queue, not from g_layout
        }
    }

    void ClearAllPendingRemoves()
    {
        // (B4-3c: the counters this function once cleared are gone -- the
        // ledger resets itself at the same boundaries)
        // ★B3-b: the slot queue is a trace this function forgot (rule 5: a
        // request leaves marks in MANY places, and a rollback that misses one
        // shows its symptom somewhere else). Keys that survived a load here
        // were consumed by the NEXT session's first confirmation of the same
        // form -- and if that stale key matched a living tile, the tile lost
        // its cell. The comment at the load-reset call site warns about
        // exactly this: "these carried PREVIOUS-session state across a load".
        g_pendingSlotDrop.clear();
    }

    void ClearDropHint()
    {
        g_dropHint = {};
    }

    namespace
    {
        // ★H1: the capacity gates answer while the MENU IS CLOSED, and nothing
        // rebuilds the board there -- Rebuild's only consumers are OnShow and
        // FinishFrame, both menu-side. So every pickup since the last open sat
        // with no layout entry, and the sim first-fit it onto the hard board
        // (no typed-bag claim, no growth rows), eating the very hole the
        // player remembered leaving: "I kept a 2x2 gap and it refused the
        // pickup". One real rebuild before answering makes g_layout the board
        // the next open would show -- placeholders, bag claims, growth. The
        // flag coalesces, so this costs one rebuild per burst of changes.
        void FreshenLayoutForGates()
        {
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen("GridInventoryMenu")) {
                return;   // menu open: FinishFrame owns the flag
            }
            // ★B5: OR the board was never built this session. The gates ran
            // fine on a stale flag alone while the sims re-derived everything
            // from the engine anyway -- but the board READER'S precondition
            // is a fresh board, and the first [CAP] observation round showed
            // exactly this: every pre-first-open capacity query compared the
            // engine against an empty g_items (104 -> 10 -> 1 divergences,
            // all engine-only, all before the first menu open).
            CoinCensus("gate-freshen");   // ★S-G: gates read coin cells too
            if (g_needRebuild.exchange(false, std::memory_order_acq_rel) ||
                g_items.empty()) {
                Rebuild();
            }
        }
    }

    int MaxAcceptUnits(RE::TESBoundObject* a_obj, int a_want)
    {
        // How many units (<= a_want) the inventory can ACCEPT right now:
        // partial-stack room first, then new tiles on the hard board, then new
        // tiles in open bags (same spill rules as Rebuild). Phase 7: stack
        // buy/take sliders clamp to this so a bulk purchase can't overflow.
        if (a_want <= 0) return 0;
        // ★★★A LEVELED ITEM IS A TABLE, NOT A THING, and measuring it for grid
        // space is measuring the menu instead of the meal. Vanilla's coin
        // purses are the case that found this: CoinPurseSmall is a FLOR whose
        // produceItem is CoinPurseGoldSmall, an LVLI -- so the harvest gate
        // asked "does a leveled list fit on the board", got a footprint out of
        // the fallback, and refused the pickup once the board filled. The list
        // resolves to GOLD, which takes no space at all.
        //
        // The gate exists to refuse what we KNOW cannot be taken. What a list
        // will hand over is not known until the engine opens it, so it is not
        // ours to refuse -- and an item that does arrive too big for the board
        // still lands in the growth rows and trips the overload, which is the
        // same treatment every other bypass (scripted AddItem, shop, console)
        // already gets.
        if (!a_obj || a_obj->IsGold() ||
            a_obj->Is(RE::FormType::LeveledItem)) {
            return a_want;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return a_want;

        if (!g_layoutLoaded) LoadLayout();
        FreshenLayoutForGates();   // ★H1

        // stack cap of the incoming form (Mabinogi tiles of up-to-cap units)
        const GridDef aDef = g_resolver ? g_resolver(a_obj) : GridDef{};
        const int aCap = EffectiveCap(a_obj, aDef);
        const std::string aKey = FormKey(a_obj);

        // units that merge into existing PARTIAL tiles (no new cells needed)
        int room = 0;
        if (aCap > 1) {
            for (auto& [k, v] : g_layout) {
                // ★not the deletion buffer: a stack parked in the trash is on
                // its way out, and "merging" into it green-lit pickups whose
                // units then had no cell to land on
                if (v.bag == kTrashKey) continue;
                if (BaseKey(k) == aKey && v.count > 0 && v.count < aCap) {
                    room += aCap - v.count;
                }
            }
        }
        if (room >= a_want) return a_want;
        const int tilesNeeded = (a_want - room + aCap - 1) / aCap;

        // shared headless collection (Phase 2) — one rule set for all sims
        CapTiles ct;
        CollectCapacityTiles(ct);
        auto& tmp = ct.tiles;

        // main occupants only: items inside a present (or form-anchored) bag
        // consume that bag's cells, not the board's
        std::vector<Item*> list;
        list.reserve(tmp.size() + tilesNeeded);
        for (auto& it : tmp) {
            if (it.inBag.empty()) list.push_back(&it);
        }

        // probes LAST: they only get what is left over on the hard board
        std::vector<Item> probes(tilesNeeded);
        for (int i = 0; i < tilesNeeded; ++i) {
            probes[i].key = "##probe" + std::to_string(i);
            probes[i].def = aDef;
            // ★Try BOTH orientations, the way the player can. A probe placed
            // upright-only refused a 1x3 staff for which a 3x1 gap was in
            // plain sight. Seeding rot=1 makes PlaceItems try the turned mask
            // first and its GI62 fallback then retries upright -- two
            // orientations for the price of the existing fallback. Square
            // masks are unchanged (the rotation is the identity).
            probes[i].rot = CanRotate(aDef) ? 1 : 0;
            probes[i].mask = MaskOf(aDef, probes[i].rot);
            list.push_back(&probes[i]);
        }
        PlaceItems(list, kCols, kMinRows, kMinRows,
               g_cwBonusCells);   // HARD board + CW bonus (W3)

        int fitTiles = 0;
        std::vector<Item*> leftover;
        for (auto& p : probes) {
            if (!p.overflow) ++fitTiles;
            else leftover.push_back(&p);
        }

        // B: spill leftover probes into bags with room, open or closed (a bag
        // item never nests inside another bag) — mirrors the Rebuild spill
        // pass, and must walk the SAME list in the SAME order as it does.
        if (aDef.bag == 0) {
            // ★Typed bags: the SAME accept rule as the real spill. Without it
            // this sim seated a sword probe in the empty ore bag, said "fits",
            // and the pickup it green-lit then overflowed for real — accept
            // verdicts must never disagree with where the item can land.
            const auto& fl = BagFilter::FilterOf(a_obj);
            for (const auto& slot : CollectBagSlots(tmp)) {
                if (leftover.empty()) break;
                if (!slot.accept.empty() && slot.accept != fl) continue;
                std::vector<Item*> blist;
                for (auto& it : tmp) {
                    if (it.inBag == slot.key) blist.push_back(&it);
                }
                for (auto* p : leftover) {   // reset from the main-board sim
                    p->col = -1;
                    p->row = -1;
                    blist.push_back(p);
                }
                PlaceItems(blist, slot.cols, slot.rows, slot.rows);
                std::vector<Item*> still;
                for (auto* p : leftover) {
                    if (!p->overflow) ++fitTiles;
                    else still.push_back(p);
                }
                leftover = std::move(still);
            }
        }

        const long long units = static_cast<long long>(room) +
                                static_cast<long long>(fitTiles) * aCap;
        return static_cast<int>((std::min)(static_cast<long long>(a_want), units));
    }

    bool CanFitNewItem(RE::TESBoundObject* a_obj)
    {
        // one-unit capacity gate: stacks onto a partial tile, or one new tile
        // first-fits on the hard board / an open bag (delegates to the
        // generalized counter so both gates share identical rules).
        return MaxAcceptUnits(a_obj, 1) >= 1;
    }

    bool WouldOverflow(RE::TESBoundObject* a_obj)
    {
        if (!a_obj || a_obj->IsGold()) return false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        if (!g_layoutLoaded) LoadLayout();
        FreshenLayoutForGates();   // ★H1

        const std::string targetKey = FormKey(a_obj);
        // shared headless collection (Phase 2) — one rule set for all sims
        CapTiles ct;
        CollectCapacityTiles(ct);

        std::vector<Item*> list;
        list.reserve(ct.tiles.size());
        for (auto& it : ct.tiles) {
            if (it.inBag.empty()) list.push_back(&it);
        }

        PlaceItems(list, kCols, kMinRows, kMinRows,
                   g_cwBonusCells);   // hard board + CW bonus (W3)

        for (const auto& it : ct.tiles) {
            if (it.key == targetKey) return it.overflow;
        }
        return false;   // worn/absent: nothing to bounce
    }

    // W2: does the CURRENT inventory overflow the hard board? Same headless
    // placement as WouldOverflow, but asks about everything at once.
    namespace
    {
        // ★Why the board is overloaded, not just that it is. The report that
        // prompted this said "the bar is red with plenty of space left", and
        // the verdict alone cannot answer it: the free cells the panel counts
        // may all sit inside bags, and the units that overflowed may be the two
        // kinds that can never go in one (coins, and bags themselves).
        struct OverloadWhy
        {
            int                      stranded = 0;
            std::vector<std::string> lines;
        };

        bool ComputeOverloaded(OverloadWhy* a_why = nullptr)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;

            if (!g_layoutLoaded) LoadLayout();

            // shared headless collection (Phase 2). NOTE two deliberate
            // unifications vs the old copy: ①unnamed COIN mirror tiles now
            // occupy capacity here too (they always did in the display and in
            // MaxAcceptUnits) ②saved overflow-zone spots reset to first-fit
            // (they are temporary everywhere else).
            CapTiles ct;
            CollectCapacityTiles(ct);
            auto& tmp = ct.tiles;

            std::vector<Item*> list;
            list.reserve(tmp.size());
            for (auto& it : tmp) {
                // main occupant only when NOT inside a bag that still exists —
                // by TILE (normal) or by FORM (tile transiently absent at load)
                if (it.inBag.empty()) {
                    list.push_back(&it);
                }
            }

            PlaceItems(list, kCols, kMinRows, kMinRows,
                       g_cwBonusCells);   // hard board + CW bonus (W3)

            // B: hard-board overflow drains into bag space, open or closed
            // (mirrors Rebuild's spill) — an item a bag can hold is NOT
            // overloaded. Coins and bag items can't spill: their overflow is a
            // genuine overload. This MUST agree with MaxAcceptUnits, or an item
            // it just accepted is judged overloaded the same frame (crimson
            // space + the forced-walk debuff).
            std::vector<Item*> spill;
            bool hardOverflow = false;
            for (auto& it : tmp) {
                if (!it.overflow) continue;
                if (it.inBag.empty() && it.def.bag == 0 && it.obj &&
                    !it.obj->IsGold() && !GoldCoins::IsCoinForm(it.obj->GetFormID())) {
                    spill.push_back(&it);
                } else {
                    hardOverflow = true;
                    if (a_why) {
                        const char* kind =
                            !it.obj                    ? "unknown" :
                            it.def.bag != 0            ? "a bag cannot be put inside a bag automatically" :
                            !it.inBag.empty()          ? "already inside a bag" :
                                                         "coins never spill into bags";
                        a_why->lines.push_back(std::format("'{}' -- {}",
                            it.obj ? it.obj->GetName() : "?", kind));
                    }
                }
            }
            for (const auto& slot : CollectBagSlots(tmp)) {
                if (spill.empty()) break;
                std::vector<Item*> occ;
                for (auto& it : tmp) {
                    if (it.inBag == slot.key) occ.push_back(&it);
                }
                for (auto sit = spill.begin(); sit != spill.end();) {
                    Item* cand = *sit;
                    // ★Typed bags: same accept rule as the real spill — an
                    // empty ore bag must not absolve a board overflowing with
                    // swords, or the debuff turns off while the overflow row
                    // is visibly full.
                    if (!slot.accept.empty() && cand->obj &&
                        slot.accept != BagFilter::FilterOf(cand->obj)) {
                        ++sit;
                        continue;
                    }
                    std::vector<Item*> test = occ;
                    cand->col = -1;
                    cand->row = -1;
                    test.push_back(cand);
                    PlaceItems(test, slot.cols, slot.rows, slot.rows);
                    if (!cand->overflow) {
                        cand->inBag = slot.key;
                        occ.push_back(cand);
                        sit = spill.erase(sit);
                    } else {
                        ++sit;
                    }
                }
            }
            if (a_why) {
                for (const Item* s : spill) {
                    // ★B5 diagnostics: the key and the COLLECTED coordinates.
                    // A stranded tile is either one that arrived placeless
                    // (col -1: reset, or a hidden cell that lost its spot) or
                    // one whose saved cell COLLIDED and was reflowed off the
                    // board -- the coordinates tell those apart, and the key
                    // says who it really was.
                    const auto li = g_layout.find(s->key);
                    a_why->lines.push_back(std::format(
                        "'{}' ({}x{}) key '{}' collected [{},{}] layout [{},{}] "
                        "bag '{}' -- no general bag slot has room for its shape",
                        s->obj ? s->obj->GetName() : "?", s->mask.w, s->mask.h,
                        s->key, s->col, s->row,
                        li != g_layout.end() ? li->second.col : -99,
                        li != g_layout.end() ? li->second.row : -99,
                        s->inBag));
                }
                a_why->stranded = static_cast<int>(a_why->lines.size());
            }
            return hardOverflow || !spill.empty();
        }
    }

    bool IsOverloaded() { return g_overloaded; }

    int SpaceUsed() { return g_spaceUsed; }

    // ⛔The old companion `BagFreeCells()` is gone. It existed because the
    //  total counted only the main board, so the take-all budget had to add
    //  bag room back by hand — two implementations of "how much room is
    //  there", and the one that mattered silently skipped closed bags AND
    //  counted the trash as storage. There is now one answer: total - used.
    int SpaceTotal() { return g_spaceTotal; }

    void PickupPartial(RE::TESBoundObject* a_obj, int a_count,
                       const std::string& a_srcKey, int a_srcTotal)
    {
        // shift+left-click split (G4): the chosen quantity leaves its source
        // tile NOW and rides the cursor as a carried fragment (preSplit, no key
        // yet). Rebuild counts the fragment via g_held->count, so the form's
        // total stays == engine count — a later cancel is auto-restored by the
        // ACQUIRE path (the source tile is now short by a_count).
        if (!a_obj || a_count <= 0) return;

        // G4 GOLD split: a_count is a VALUE. Create a pin for the fragment
        // (subtracts from walking gold), reduce the source if it was itself a
        // pin, and carry the fragment as a coin of the value's band.
        if (GoldCoins::IsCoinForm(a_obj->GetFormID()) &&
            !GoldCoins::IsPouch(a_obj->GetFormID())) {
            const int val = (std::min)(a_count, GoldCoins::kCoinCap);
            // ★S-G: the source is an OWNED SLOT either way (the pin/auto split
            // died with walking gold) -- shrink it, or erase it when the whole
            // amount leaves. One coin form, so the key never changes band.
            {
                const int sv = CoinRecordOf(a_srcKey);
                const int total = sv >= 0 ? sv : a_srcTotal;
                const int rem = total - val;
                if (rem <= 0) g_layout.erase(a_srcKey);
                else          SetCoinRecord(a_srcKey, rem);
            }
            auto* cform = GoldCoins::CoinForTier(GoldCoins::BandTier(val));
            if (!cform) return;
            const std::string pinKey = NextTileKey(FormKey(cform));
            SetCoinRecord(pinKey, val);   // the fragment's own record (cursor money)
            const GridDef gd = g_resolver ? g_resolver(cform) : GridDef{};
            Held g;
            g.key = pinKey;          // real pin key; position assigned on drop
            g.obj = cform;
            g.mask = MaskOf(gd);
            g.count = 1;
            g.coinValue = val;
            g.defScale = gd.scale;
            g.offX = g.mask.w * CellPx() * 0.5f;
            g.offY = g.mask.h * CellPx() * 0.5f;
            g.justPicked = true;
            g.preSplit = true;
            g_held = g;
            if (g_sound) g_sound(cform, true);   // the purse audibly lifts
            RequestRebuild();
            return;
        }

        int srcRot = 0;
        if (auto li = g_layout.find(a_srcKey); li != g_layout.end()) {
            srcRot = li->second.rot;   // GI62: a fragment leaves as its stack lies
            li->second.count -= a_count;
            if (li->second.count <= 0) g_layout.erase(li);   // took the whole tile
        }
        const GridDef d = g_resolver ? g_resolver(a_obj) : GridDef{};
        Held h;
        h.key.clear();            // assigned on drop (new tile) or absorbed (merge)
        h.srcPool = PoolOfSlot(a_srcKey);   // ...but the POOL is known now, and needed now
        h.obj = a_obj;
        h.SetRot(CanRotate(d) ? srcRot : 0);
        h.mask = MaskOf(d, h.rot);
        h.count = a_count;
        h.isBag = d.bag != 0;
        h.defScale = d.scale;
        HoldByPivot(h, d);   // GI62d
        h.justPicked = true;
        h.preSplit = true;
        for (const auto& si : g_items) {   // GI36: carry the source tile's star
            if (si.key != a_srcKey) continue;
            h.fav    = si.fav;
            h.stolen = si.stolen;   // (1.3.2) and its markers
            h.quest  = si.quest;
            break;
        }
        g_held = h;
        if (g_sound) g_sound(a_obj, true);   // split confirmed -> pickup sound
        RequestRebuild();
    }

    int CellSpanOf(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return 1;
        const GridDef d = g_resolver ? g_resolver(a_obj) : GridDef{};
        const Mask m = MaskOf(d);
        int n = 0;
        for (int y = 0; y < m.h; ++y) {
            for (int x = 0; x < m.w; ++x) {
                if (m.rows[y][x]) ++n;
            }
        }
        return (std::max)(1, n);
    }

    std::vector<std::string> PouchTiles()
    {
        std::vector<std::string> out;
        for (const auto& it : g_items) {
            if (!it.obj || it.key.empty()) continue;
            if (GoldCoins::IsPouch(it.obj->GetFormID())) out.push_back(it.key);
        }
        return out;
    }

    std::string AnyPouchTile()
    {
        const auto v = PouchTiles();
        return v.empty() ? std::string{} : v.front();
    }

    std::vector<std::string> OrderKeysByPosition(std::vector<std::string> a_keys)
    {
        // The same ordering the coin partition's byPos uses (main "" sorts
        // before any bag, then row-major) -- stated once more here because the
        // two must agree: a purse the partition calls "front" must not be one
        // the trim calls "rear". A key with no layout entry has no position to
        // defend, so it sorts last and is therefore the first thing a
        // back-to-front walk spends.
        std::sort(a_keys.begin(), a_keys.end(),
            [](const std::string& a, const std::string& b) {
                const auto ai = g_layout.find(a);
                const auto bi = g_layout.find(b);
                const bool ah = ai != g_layout.end();
                const bool bh = bi != g_layout.end();
                if (ah != bh) return ah;      // placed keys first
                if (!ah) return a < b;        // both unknown: stable by name
                const auto& x = ai->second;
                const auto& y = bi->second;
                if (x.bag != y.bag) return x.bag < y.bag;
                if (x.row != y.row) return x.row < y.row;
                if (x.col != y.col) return x.col < y.col;
                return a < b;
            });
        return a_keys;
    }

    std::vector<CoinSlot> CoinTilesByPosition()
    {
        // Coin tiles are keyed under whichever BAND form minted them -- a
        // shrunk pin keeps its old band key (see the partition's emitCoin
        // note) -- so all four tier bases are live even after the one-coin
        // migration. Filtering by base is what keeps the pouch out: it passes
        // IsCoinForm but is its own form, not a tier.
        std::set<std::string> bases;
        for (int t = 0; t < 4; ++t) {
            if (auto* f = GoldCoins::CoinForTier(t)) bases.insert(FormKey(f));
        }
        struct Row
        {
            CoinSlot           slot;
            const LayoutEntry* le = nullptr;
        };
        std::vector<Row> rows;
        for (const auto& [k, le] : g_layout) {
            if (le.coin < 0) continue;
            if (!bases.contains(BaseKey(k))) continue;
            if (g_held && k == g_held->key) continue;   // cursor money is spoken for
            rows.push_back({ { k, le.coin }, &le });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.le->bag != b.le->bag) return a.le->bag < b.le->bag;
            if (a.le->row != b.le->row) return a.le->row < b.le->row;
            if (a.le->col != b.le->col) return a.le->col < b.le->col;
            return a.slot.key < b.slot.key;
        });
        std::vector<CoinSlot> out;
        out.reserve(rows.size());
        for (auto& r : rows) out.push_back(std::move(r.slot));
        return out;
    }

    void MarkCapacityDirty() { g_capacityDirty = true; }

    bool PoolTrace() { return g_poolTrace; }
    bool SimDrift()  { return g_simDrift; }

    void SetSimDrift(bool a_on)
    {
        if (g_simDrift == a_on) return;
        g_simDrift = a_on;
        SKSE::log::info("[GRID] carry-identity drift simulation {}",
            a_on ? "ON (test)" : "off");
    }

    void SetPoolTrace(bool a_on)
    {
        if (g_poolTrace == a_on) return;
        g_poolTrace = a_on;
        SKSE::log::info("[GRID] pool trace {}", a_on ? "ON" : "off");
    }

    namespace
    {
        // ★(1.3.0) pouch tiles this board has already answered for. A tile
        // absent from here on the next claim pass was born THIS rebuild --
        // i.e. the pouch that just walked in, which is who returning gold
        // belongs to. Cleared on load (keys from another save are lies).
        std::set<std::string> g_knownPouchTiles;
    }

    // ★W3: settings + the live bonus (see CapacityTick for the measurement)
    void SetCwCells(int a_perCell, int a_base, int a_maxCells)
    {
        g_cwPerCell = (std::max)(0, a_perCell);
        g_cwBase = (std::max)(0, a_base);
        g_cwMaxCells = std::clamp(a_maxCells, 0, 200);
    }
    int CwPerCell() { return g_cwPerCell; }
    int CwBase() { return g_cwBase; }
    int CwMaxCells() { return g_cwMaxCells; }
    int CwBonusCells() { return g_cwBonusCells; }

    void ClaimIncomingPouchGold()
    {
        const auto tiles = PouchTiles();
        std::vector<std::string> fresh, known;
        for (const auto& k : tiles) {
            (g_knownPouchTiles.contains(k) ? known : fresh).push_back(k);
        }
        GoldCoins::ClaimReturned(fresh, known);
        g_knownPouchTiles.clear();
        g_knownPouchTiles.insert(tiles.begin(), tiles.end());
    }

    namespace
    {
        // W1v2: save-clean encumbrance via ESP ABILITIES. Ability effects are
        // stored in the save only as form references — deleting the mod makes
        // the engine purge them automatically (zero CarryWeight residue),
        // unlike the temporary-AV steering below (kept as fallback).
        //   0x807 GI_CarryBoost : ability, CarryWeight +50000
        //   0x808 GI_Overload   : ability, CarryWeight net negative
        RE::SpellItem* g_abBoost = nullptr;
        RE::SpellItem* g_abOver = nullptr;
        bool g_abTried = false;
        bool g_avResidueCleared = false;

        void ResolveAbilities()
        {
            if (g_abTried) return;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            g_abTried = true;
            g_abBoost = dh->LookupForm<RE::SpellItem>(0x807, "Grid Inventory.esp");
            g_abOver = dh->LookupForm<RE::SpellItem>(0x808, "Grid Inventory.esp");
            SKSE::log::info("[GRID] encumbrance mode: {}",
                (g_abBoost && g_abOver) ? "abilities (save-clean)" : "AV steering (fallback)");
        }
    }

    void CapacityTick()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return;

        // ★(1.5.x) transient arrivals age by frame; one whose deltas were
        // swallowed gets one rebuild at expiry, so a unit the use left
        // behind after all still surfaces (and a stale entry can never
        // swallow a later, real acquisition of the same form).
        for (auto ti = g_transientArrivals.begin();
             ti != g_transientArrivals.end();) {
            if (--ti->second.frames <= 0) {
                if (ti->second.suppressed) {
                    MarkCapacityDirty();
                    RequestRebuild();
                }
                ti = g_transientArrivals.erase(ti);
            } else {
                ++ti;
            }
        }

        if (g_capacityDirty) {
            g_capacityDirty = false;
            const bool was = g_overloaded;
            OverloadWhy why;
            g_overloaded = ComputeOverloaded(&why);
            if (g_overloaded && !was) {
                Sfx::FailNote(Lang::T(Lang::Str::Overloaded));
                // ★The panel shows main + GENERAL bags; the hard board is only
                // kCols x kMinRows of that. Printing both is what turns "why is
                // it red, I have room" into an answerable question -- the room
                // is real, it is just somewhere these units cannot go.
                SKSE::log::info("[GRID] capacity: OVERLOADED -- hard board {}x{}={}, "
                                "panel shows {}/{} (the free cells include general "
                                "bag space)",
                    kCols, kMinRows, kCols * kMinRows, g_spaceUsed, g_spaceTotal);
                for (const auto& l : why.lines) {
                    SKSE::log::info("[GRID]   stranded: {}", l);
                }
            } else if (!g_overloaded && was) {
                SKSE::log::info("[GRID] capacity: back to normal");
            }
        }

        // W1v2 preferred path: ESP abilities (no save residue). The boost is
        // always on; the overload debuff toggles with the capacity state.
        ResolveAbilities();
        auto* avo = player->AsActorValueOwner();
        if (g_abBoost && g_abOver) {
            // one-time per save: neutralise the OLD AV-steering residue
            // (ours was thousands; potion effects are tiny — leave those)
            if (!g_avResidueCleared) {
                g_avResidueCleared = true;
                const float t = player->GetActorValueModifier(
                    RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight);
                if (std::fabs(t) > 2000.0f) {
                    avo->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                        RE::ActorValue::kCarryWeight, -t);
                    SKSE::log::info("[GRID] cleared legacy CW modifier ({:+.0f})", t);
                }
            }
            if (!player->HasSpell(g_abBoost)) player->AddSpell(g_abBoost);
            const bool has = player->HasSpell(g_abOver);
            if (g_overloaded && !has) player->AddSpell(g_abOver);
            else if (!g_overloaded && has) player->RemoveSpell(g_abOver);

            // ★W3: carry weight -> owned cells. Subtract OUR two abilities'
            // own CarryWeight magnitudes (read from the forms once -- the
            // ESP may be edited) and the baseline; what remains is the
            // world's bonus: perks, stones, enchantments, potions, stamina
            // level-ups. Ability mode only -- the AV-steering fallback
            // rewrites the total every frame and leaves nothing to measure.
            if (g_cwPerCell > 0) {
                static float s_boostMag = -1.0f;
                static float s_overMag = 0.0f;
                if (s_boostMag < 0.0f) {
                    auto magOf = [](RE::SpellItem* a_sp) {
                        float t = 0.0f;
                        if (!a_sp) return t;
                        for (auto* eff : a_sp->effects) {
                            if (!eff || !eff->baseEffect) continue;
                            const auto& d = eff->baseEffect->data;
                            if (d.primaryAV != RE::ActorValue::kCarryWeight) {
                                continue;
                            }
                            const float m = eff->effectItem.magnitude;
                            t += d.flags.all(RE::EffectSetting::
                                     EffectSettingData::Flag::kDetrimental)
                                     ? -m : m;
                        }
                        return t;
                    };
                    s_boostMag = (std::max)(0.0f, magOf(g_abBoost));
                    s_overMag = magOf(g_abOver);
                }
                float cw = avo->GetActorValue(RE::ActorValue::kCarryWeight);
                if (player->HasSpell(g_abBoost)) cw -= s_boostMag;
                if (player->HasSpell(g_abOver)) cw -= s_overMag;
                // ★baseline 0 = AUTO: the race's own base, so overhauls that
                // rewrite it (race records) need no manual setting. Stamina
                // level-ups grow the AV past the racial base and so still
                // count as earned bonus.
                int base = g_cwBase;
                if (base <= 0) {
                    auto* race = player->GetRace();
                    base = race && race->data.baseCarryWeight > 0.0f
                               ? static_cast<int>(race->data.baseCarryWeight)
                               : 300;
                }
                const int ext = static_cast<int>(cw) - base;
                const int cells = std::clamp(
                    ext > 0 ? ext / g_cwPerCell : 0, 0, g_cwMaxCells);
                if (cells != g_cwBonusCells) {
                    SKSE::log::info(
                        "[GRID] carry-weight bonus: {} cell(s) ({:+} CW past "
                        "the baseline)", cells, ext);
                    g_cwBonusCells = cells;
                    MarkCapacityDirty();
                    RequestRebuild();
                }
            } else if (g_cwBonusCells != 0) {
                g_cwBonusCells = 0;
                MarkCapacityDirty();
                RequestRebuild();
            }
            return;
        }

        // W1: weight never limits — keep effective CarryWeight comfortably
        // above the inventory weight. W2: while overloaded, hold it just BELOW
        // so the vanilla encumbrance (forced walk, no fast travel) engages.
        // Steered every frame through the TEMPORARY AV modifier, so whatever
        // perks/spells/other mods do to CarryWeight, the net stays on target.
        constexpr float kBuffer = 10000.0f;
        const float invW = avo->GetActorValue(RE::ActorValue::kInventoryWeight);
        const float cw = avo->GetActorValue(RE::ActorValue::kCarryWeight);
        const float target = g_overloaded ? (std::max)(0.0f, invW - 5.0f)
                                          : invW + kBuffer;
        const float delta = target - cw;
        if (std::fabs(delta) > 0.5f) {
            avo->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                RE::ActorValue::kCarryWeight, delta);
        }
    }

    int ItemValue(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return 0;
        const auto it = g_values.find(a_obj->GetFormID());
        if (it != g_values.end()) return it->second;
        // ★★The cache is rebuilt from the PLAYER's inventory only, so every
        // item on the far side of a barter or container window misses it — and
        // the old -1 sentinel was not caught anywhere, it was PRINTED. Every
        // merchant tooltip read "Value -1" (measured: vanilla has no negative
        // book value at all — Spell Tome: Raise Zombie is 49, and 0 of the 821
        // BOOK records in Skyrim.esm are negative).
        // ★Asking the form is not a patch: the cache stores exactly this call's
        // result (see the GI44 note at the fill site), so player items are
        // unchanged and the other side simply gets the same answer.
        return a_obj->GetGoldValue();
    }

    int UnitValueWith(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_xl)
    {
        const int base = ItemValue(a_obj);
        if (!a_obj || !a_xl) return base;
        // Same throwaway-entry pattern the tooltip's damage/armor cards already
        // use in-game -- extended with the unit's real list for the value call,
        // then detached so the destructors see nothing engine-owned.
        RE::BSSimpleList<RE::ExtraDataList*> sl;
        // push_front on an EMPTY list constructs the item inside the embedded
        // head node -- no allocation. (front() here was a crash: begin() on an
        // empty list returns the null end() iterator, and release builds strip
        // the assert, so the assignment wrote through nullptr.)
        sl.push_front(a_xl);
        RE::InventoryEntryData e(a_obj, 1);
        e.extraLists = &sl;
        const int v = e.GetValue();
        e.extraLists = nullptr;         // detach BEFORE ~InventoryEntryData
        return v > 0 ? v : base;
    }

    // Public forwarders for the two instance resolvers (the implementations
    // live in the anonymous namespace above, next to the key grammar they
    // belong to). Callers outside Grid.cpp -- Equip, LootBarter -- need them to
    // name a sub-stack when they move or wear an item.
    RE::InventoryEntryData* LiveEntryOf(RE::TESObjectREFR* a_owner,
                                        RE::TESBoundObject* a_obj)
    {
        return LiveEntry(a_owner, a_obj);
    }

    RE::ExtraDataList* ExtraForInstance(RE::InventoryEntryData* a_entry,
                                        std::uint16_t a_uid, int a_xlIdx)
    {
        return ExtraForTile(a_entry, a_uid, a_xlIdx);
    }

    RE::ExtraDataList* ExtraForPool(RE::InventoryEntryData* a_entry,
                                    std::uint16_t a_uid, std::uint16_t a_sig)
    {
        return ExtraForPoolImpl(a_entry, a_uid, a_sig);
    }

    UnitChoice PoolChoice(RE::InventoryEntryData* a_entry, std::uint16_t a_uid,
                          std::uint16_t a_sig, bool a_nameWorn, bool a_wornLegal)
    {
        if (auto* xl = ExtraForPoolImpl(a_entry, a_uid, a_sig, a_nameWorn)) {
            return { PickKind::kNamed, xl };
        }
        // A named pool (uid or sig) that is ABSENT is a stale click, not a
        // licence for the engine to substitute something else.
        if (a_uid != 0 || a_sig != 0) return {};
        if (!a_entry || !a_entry->extraLists) return { PickKind::kAnyIsSafe, nullptr };
        bool ambiguous = false;
        for (auto* xl : *a_entry->extraLists) {
            if (!xl) continue;
            const bool wornHere = xl->HasType<RE::ExtraWorn>() ||
                                  xl->HasType<RE::ExtraWornLeft>();
            const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
            const bool plain = InstanceSig(xl) == 0 && !(xu && xu->uniqueID != 0);
            if (wornHere && !a_wornLegal) return {};   // the body's unit is grabbable
            if (!plain) ambiguous = true;
        }
        return { ambiguous ? PickKind::kFallback : PickKind::kAnyIsSafe, nullptr };
    }

    std::uint16_t InstanceSigOf(RE::ExtraDataList* a_xl) { return InstanceSig(a_xl); }

    bool IsBagForm(RE::TESBoundObject* a_obj)
    {
        return a_obj && g_resolver && g_resolver(a_obj).bag != 0;
    }

    int StackCap(RE::TESBoundObject* a_obj)
    {
        return a_obj ? EffectiveCap(a_obj) : 1;
    }

    void EnumerateUnits(RE::InventoryEntryData* a_entry, int a_count,
                        std::vector<UnitRef>& a_out, bool a_skipWorn)
    {
        EnumerateUnitRefs(a_count, a_count, a_entry, a_out, a_skipWorn);
    }

    // a_hand: 0 = either, 1 = RIGHT (ExtraWorn), 2 = LEFT (ExtraWornLeft).
    //
    // The hand matters as soon as two copies of ONE form are worn at once -- a
    // dagger in each hand. "First worn list of this form" then answers the same
    // list for both slots, so the doll showed the left item's stats on the right,
    // lifting one unequipped the other, and a swap saw the two as one unit.
    RE::ExtraDataList* WornExtraOf(RE::InventoryEntryData* a_entry, int a_hand)
    {
        if (!a_entry || !a_entry->extraLists) return nullptr;
        for (auto* xl : *a_entry->extraLists) {
            if (!xl) continue;
            const bool L = xl->HasType<RE::ExtraWornLeft>();
            const bool R = xl->HasType<RE::ExtraWorn>();
            if (!L && !R) continue;
            if (a_hand == 1 && !R) continue;
            if (a_hand == 2 && !L) continue;
            return xl;
        }
        return nullptr;
    }

    // The worn list belonging to THIS unit. a_sig 0 with a_uid 0 means "the
    // plain one", which is still unambiguous per hand.
    RE::ExtraDataList* WornExtraMatching(RE::InventoryEntryData* a_entry,
                                         std::uint16_t a_uid, std::uint16_t a_sig,
                                         int a_hand)
    {
        if (!a_entry || !a_entry->extraLists) return nullptr;
        for (auto* xl : *a_entry->extraLists) {
            if (!xl) continue;
            const bool L = xl->HasType<RE::ExtraWornLeft>();
            const bool R = xl->HasType<RE::ExtraWorn>();
            if (!L && !R) continue;
            if (a_hand == 1 && !R) continue;
            if (a_hand == 2 && !L) continue;
            std::uint16_t uid = 0;
            if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
            if (uid == a_uid && InstanceSig(xl) == a_sig) return xl;
        }
        return WornExtraOf(a_entry, a_hand);   // fall back rather than show nothing
    }

    // GI1/D2: this used to scan the WHOLE entry and glow if ANY sub-stack was
    // enchanted -- so one enchanted sword in a stack of three lit all three.
    // The caller now names the sub-stack this pixel belongs to:
    //   a_xl == nullptr -> a plain unit (or no entry at all): base form only.
    std::uint8_t GlowBits(RE::TESBoundObject* a_obj, RE::InventoryEntryData*,
                          RE::ExtraDataList* a_xl)
    {
        if (!a_obj) return 0;
        std::uint8_t glow = 0;
        if (const auto* ef = a_obj->As<RE::TESEnchantableForm>();
            ef && ef->formEnchanting) {
            glow |= 1;
        }
        if (!(glow & 1) && a_xl) {
            if (const auto* xe = a_xl->GetByType<RE::ExtraEnchantment>();
                xe && xe->enchantment) {
                glow |= 1;
            }
        }
        if (IsUniqueCached(a_obj)) glow |= 2;
        // GI66: per-unit STATUS bit. Poison is drawn as the top-right droplet,
        // NOT as a halo -- the switch above must never see this bit.
        if (a_xl) {
            if (const auto* xp = a_xl->GetByType<RE::ExtraPoison>();
                xp && xp->poison) {
                glow |= 4;
            }
        }
        return glow;
    }

    void DrawCountBadge(ImDrawList* a_dl, const ImVec2& a_tileMin, const char* a_text)
    {
        // Mabinogi-style: hugging the corner, full black outline so the count
        // reads on any icon underneath (all skins are dark-grounded)
        const ImVec2 tp(a_tileMin.x + 2.0f, a_tileMin.y - 1.0f);
        // ★Eight passes of black is a LOT of edge, and it is there because the
        // count sits on an item picture, not on the panel. But on a pale skin
        // the figure itself is dark, so the ring merges with it into a smudge
        // — the same trap the title fell into. Skins whose ink is dark get the
        // figure alone; the picture under it is what they contrast against.
        // ★★An INK skin takes the outline back, even though its panel is pale
        // and its ink is dark. The rule above is about the PANEL; this figure
        // sits on an item picture and, in this skin, is drawn in the clay
        // accent -- a mid-tone that has neither the panel's lightness nor the
        // ink's darkness to lean on. The ring is what gives it an edge on a
        // bright sack and on a black boot alike.
        if (!Theme::S().lightPanel || Theme::InkNeedsOutline() || Theme::InkChrome()) {
            const ImU32 oc = IM_COL32(0, 0, 0, 255);
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    a_dl->AddText(ImVec2(tp.x + ox, tp.y + oy), oc, a_text);
                }
            }
        }
        // the count already carries a full black outline, so the fill can be
        // the plain emphasis colour on any panel — light or dark
        // ★White on an ink skin, not the clay accent. The figure sits on an
        // item picture and carries a full black ring, so white is the one fill
        // that keeps its distance from every sprite underneath -- the clay is a
        // mid-tone and met the leather bags halfway.
        a_dl->AddText(tp, Theme::InkChrome() ? IM_COL32(255, 255, 255, 255)
                                             : Theme::Val(), a_text);
    }

    // ★See Grid.h. Drawn immediately before the sprite it belongs to, from the
    // same dw/dh/centre — a separate pass would have to recompute that sizing,
    // and two copies of it drifting apart puts the shadow off the item.
    void DrawItemShadow(ImDrawList* a_dl, void* a_srv, const ImVec2& a_centre,
                        float a_dw, float a_dh, float a_deg)
    {
        if (!a_dl || !a_srv) return;
        const float opac = Theme::ShadowOpacity();
        if (opac <= 0.002f) return;

        const float S    = Theme::Scale();
        const float blur = Theme::ShadowBlur() * S;
        // DISTANCE falls toward the lower right, the direction the capture rig
        // already lights from (az -37, el +34 — the lamp is up and to the LEFT,
        // so this is where the item's own shading says its shadow goes). At 0
        // the offset vanishes and the spread is ambient, which is also what
        // keeps the shadow symmetric under the 90-degree tile rotations.
        const float off = Theme::ShadowDist() * S * 0.70710678f;
        const ImVec2 c(a_centre.x + off, a_centre.y + off);
        const ImVec2 sz(a_dw, a_dh);

        // ★★A LIGHT shadow needs the sprite's ALPHA without its colour, which a
        // tint cannot give: black collapses the RGB and the alpha draws the
        // shape, white multiplies to the sprite itself and the "shadow" comes
        // out as offset copies of the item. UIRoot::BeginSilhouette swaps in a
        // pixel shader that reads the alpha alone, so on a dark board the halo
        // is finally a halo.
        // Which skins want it is the skin's own answer — Theme::LightItemShadow.
        const bool wantLight = Theme::LightItemShadow();
        const ImU32 ink = wantLight ? IM_COL32(255, 255, 255, 255)
                                    : IM_COL32(0, 0, 0, 255);
        const bool sil = wantLight && UIRoot::BeginSilhouette(a_dl);
        // ★If the shader is missing, fall back to BLACK rather than drawing
        // white through the ordinary path — that is exactly the smear.
        const ImU32 base = sil ? ink : IM_COL32(0, 0, 0, 255);

        if (blur < 0.05f) {
            const int a = static_cast<int>(opac * 255.0f + 0.5f);
            if (a > 0) {
                AddImageRot(a_dl, a_srv, c, sz, a_deg, (base & 0x00FFFFFFu) | (a << 24));
            }
            if (sil) UIRoot::EndSilhouette(a_dl);
            return;
        }

        // ★★The blur is N stamps of the sprite itself on a ring of radius
        // `blur`, not a sample of a pre-blurred texture. The baked silhouette
        // this used to read from went through a 96px canvas — 300px capture
        // DOWN to 96, blurred, then back UP to the tile — and that round trip
        // put a FLOOR under the softness. Four attempts at tuning its radius
        // all landed in the same place, because the number being tuned was
        // never what made it soft. Stamping the sprite runs at full capture
        // resolution: blur 0 is the exact outline, and every value above it
        // spreads by the pixels it says.
        //
        // Cheap despite the count: consecutive stamps share one texture, so
        // ImGui merges them into a single draw command — the cost is vertices
        // (17 quads instead of 1), not draw calls.
        //
        // ★Two rings, the inner one at 0.55r and rotated half a step, once the
        // radius is wide enough for a single ring to read as eight petals
        // rather than as a blur. Below that the inner ring would sit on top of
        // the centre stamp and buy nothing.
        constexpr int kSpokes = 8;
        const bool    twoRing = blur >= 1.5f;
        const int     taps    = 1 + kSpokes * (twoRing ? 2 : 1);

        // ★Stacked alpha is NOT additive: N layers of `a` come out at
        // 1-(1-a)^N. Dividing the target by N would leave the middle of the
        // shadow far too dark, so invert the compositing instead — then the
        // fully-covered interior lands exactly on OPACITY and the fringe, which
        // only some of the stamps reach, falls off on its own.
        const float per = 1.0f - std::pow(1.0f - opac, 1.0f / static_cast<float>(taps));
        const int   pa  = static_cast<int>(per * 255.0f + 0.5f);
        if (pa <= 0) {
            if (sil) UIRoot::EndSilhouette(a_dl);
            return;
        }
        const ImU32 col = (base & 0x00FFFFFFu) | (static_cast<ImU32>(pa) << 24);

        AddImageRot(a_dl, a_srv, c, sz, a_deg, col);
        constexpr float kStep = 6.28318531f / static_cast<float>(kSpokes);
        for (int i = 0; i < kSpokes; ++i) {
            const float t = static_cast<float>(i) * kStep;
            AddImageRot(a_dl, a_srv,
                ImVec2(c.x + std::cos(t) * blur, c.y + std::sin(t) * blur),
                sz, a_deg, col);
        }
        if (twoRing) {
            const float ir = blur * 0.55f;
            for (int i = 0; i < kSpokes; ++i) {
                const float t = (static_cast<float>(i) + 0.5f) * kStep;
                AddImageRot(a_dl, a_srv,
                    ImVec2(c.x + std::cos(t) * ir, c.y + std::sin(t) * ir),
                    sz, a_deg, col);
            }
        }
        // ★Every exit from this function has to pass through here — a shader
        // left bound would repaint the whole rest of the frame as silhouettes.
        if (sil) UIRoot::EndSilhouette(a_dl);
    }

    // ★★See Grid.h. ONE wedge per item, at the footprint's top-right.
    // Black underneath so the colour reads on a pale sheet as well as on a
    // dark panel — the same trick every marker on this tile already uses.
    void DrawRarityWedge(ImDrawList* a_dl, const ImVec2& a_boxMin,
                         const ImVec2& a_boxMax, std::uint8_t a_haloBits,
                         Lotd::Status a_relic)
    {
        const std::uint8_t bits = a_haloBits & 0x3;
        // ★An OWED relic earns the wedge with no rarity of its own; a donated
        // one does not, and gets nothing here that it would not have had
        // anyway. See the colour note below.
        if (!a_dl || (!bits && a_relic != Lotd::Status::kUndonated)) return;
        const float cell = CellPx();
        const float d    = cell * kWedgeFrac;
        const float rim  = RimPx();
        // ★★Pull in to the SHADED area, not to the tile rectangle. The occupied
        // cell's fill steps back from the hairline (DrawOccupancyPass: shadeIn),
        // so a wedge anchored to the raw box straddles the grid line and looks
        // pasted on top of the board rather than set into the item's own ground.
        // Same rule the fill uses, so the two edges land together.
        const float in  = Theme::S().engravedCells
                        ? Theme::kGrooveW * Theme::Scale() * 0.5f : 1.0f;
        const float x1  = a_boxMax.x - in;
        const float y0  = a_boxMin.y + in;
        // ★GI67: unique wins outright over enchanted — see DrawMarkerTray.
        // ★★1.4.4, AND THE ORDER IS THE WHOLE DESIGN. A relic still owed to the
        // museum takes the wedge from whatever rarity the item has, because
        // "carry this home" is the only urgent thing about it. Once it is
        // donated the wedge goes BACK to its rarity -- there is nothing urgent
        // left, and hiding "unique" on 1273 weapons and armours forever would
        // cost more than it buys.
        //
        // ★★★AND A DONATED RELIC WITH NO RARITY GETS NOTHING, which is a wedge
        // this feature shipped with and then lost on purpose.
        //
        // It was grey, and read as "already handed in, safe to sell". Two
        // things were wrong with that. A plain item that is NOT a relic is
        // equally safe to sell and carries no mark, so the grey separated two
        // states that lead to the same act; and a donated UNIQUE relic shows
        // gold, so the reading was not even available in the case a player
        // would most want it. The line above already says the real rule --
        // once donated, the museum has no claim on the wedge -- and the grey
        // was that rule failing to apply to the leftovers.
        //
        // It also got worse the better you played. Plain relics are the
        // NUMEROUS kind (books, ingredients, oddments), so a full collection
        // filled the board with a mark that asked for nothing, exactly when
        // the purple ones were hardest to pick out.
        //
        // The fact itself is never lost: the tooltip says it in every case.
        // Now the wedge says one thing only -- the museum still wants this.
        constexpr ImU32 kUnique   = IM_COL32(232, 182, 74, 255);
        constexpr ImU32 kEnchant  = IM_COL32(79, 143, 240, 255);
        constexpr ImU32 kRelicOwe = IM_COL32(169, 123, 232, 255);   // #A97BE8
        const ImU32 col = (a_relic == Lotd::Status::kUndonated) ? kRelicOwe
                        : (bits & 0x2)                          ? kUnique
                                                                : kEnchant;

        // outer: the full wedge, in black. Both legs are d, so the top and the
        // right side are the same length — it is a right ISOSCELES triangle.
        a_dl->AddTriangleFilled(ImVec2(x1 - d, y0), ImVec2(x1, y0),
                                ImVec2(x1, y0 + d), IM_COL32(11, 11, 11, 255));

        // ★★inner: inset on ALL THREE sides, not scaled from the shared corner.
        // The first cut drew a smaller triangle sharing the right-angle vertex,
        // which puts the whole difference on the hypotenuse — an outline on one
        // side only. Offsetting every edge inward by rim moves the right angle
        // by (rim, rim) and costs rim*(2 + sqrt2) of leg: rim off the top, rim
        // off the right, and rim*sqrt2 where the hypotenuse advances.
        const float di = d - rim * (2.0f + 1.41421356f);
        if (di > 0.5f) {
            const float ix = x1 - rim;
            const float iy = y0 + rim;
            a_dl->AddTriangleFilled(ImVec2(ix - di, iy), ImVec2(ix, iy),
                                    ImVec2(ix, iy + di), col);
        }
    }

    // ★See Grid.h. Shared by the grid, the equipment doll and the partner
    // window so one item cannot look like two different items.
    void DrawMarkerTray(ImDrawList* a_dl, const ImVec2& a_boxMin, const ImVec2& a_boxMax,
                        bool a_fav, bool a_stolen, bool a_poisoned, bool a_worn)
    {
        if (!a_dl || (!a_fav && !a_stolen && !a_poisoned && !a_worn)) return;
        const float cell  = CellPx();
        const float mw    = cell * kMarkFrac;
        const float r     = mw * 0.5f;
        const float rim   = RimPx();
        const float gap   = cell * kGapFrac;
        const float inset = cell * kInsetFrac;
        const ImU32 oc    = IM_COL32(11, 11, 11, 255);

        float       cx = a_boxMax.x - r - inset;   // rightmost marker centre
        const float cy = a_boxMax.y - r - inset;

        // ★★★"ON THE BODY" IS A TRAY MARKER, not a mark of its own. It was drawn
        // straight onto the partner cell at the same bottom-right corner the
        // tray starts from, so on a corpse wearing a POISONED weapon the two
        // landed on the same pixels and the torso -- drawn later -- hid the
        // droplet. It also carried a fixed 1px outline while everything in here
        // scales with RimPx(), which is the "one set, different shapes, same
        // weight" rule (RULES 63) read the other way round.
        // ★Rightmost, so the common case (worn, nothing else) looks exactly
        // where it always did; anything else steps left as it always has.
        if (a_worn) {
            const float s = r * 2.0f;
            const float x = cx - r, y = cy - r;
            const auto  torso = [&](float dx, float dy, ImU32 col) {
                a_dl->AddCircleFilled(
                    ImVec2(x + dx + s * 0.5f, y + dy + s * 0.26f), s * 0.23f, col);
                const ImVec2 t[4] = {
                    ImVec2(x + dx + s * 0.08f, y + dy + s),
                    ImVec2(x + dx + s * 0.24f, y + dy + s * 0.56f),
                    ImVec2(x + dx + s * 0.76f, y + dy + s * 0.56f),
                    ImVec2(x + dx + s * 0.92f, y + dy + s),
                };
                a_dl->AddConvexPolyFilled(t, 4, col);
            };
            // ★An outline in four directions rather than a fatter silhouette
            // underneath: at this size a grown shape closes the gap between head
            // and shoulders and the figure becomes a blob.
            torso(-rim, 0.0f, oc);
            torso(rim, 0.0f, oc);
            torso(0.0f, -rim, oc);
            torso(0.0f, rim, oc);
            torso(0.0f, 0.0f, IM_COL32(220, 200, 150, 235));
            cx -= mw + gap;
        }
        if (a_stolen) {
            a_dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(206, 64, 52, 255));
            a_dl->AddCircle(ImVec2(cx, cy), r, oc, 0, rim);
            cx -= mw + gap;
        }
        if (a_poisoned) {
            // ★Sized from the shared HEIGHT, not the shared width: the point
            // adds kDropTipD*r on top of the circle, so a droplet drawn at the
            // others' width would stand 30% taller than the row. Width follows
            // from that, which is why it is the narrow one.
            const float dw = mw / ((1.0f + kDropTipD) * 0.5f);
            // the drop's bounding centre sits 0.3*radius above its circle
            // centre — offset so all three share one centre line
            DrawPoisonDrop(a_dl, ImVec2(cx, cy + dw * 0.5f * (kDropTipD - 1.0f) * 0.5f), dw);
            cx -= mw + gap;
        }
        if (a_fav) {
            // ★★Fixed white, not sk.hi. The favourite mark used to take the
            // skin's bright accent, which is near-white on four skins, dark
            // ochre on the parchment one and teal on Simple — the same flag
            // reading as a different thing per skin. It is the player's own
            // mark; it should not change meaning with the wallpaper.
            constexpr ImU32 kFavCol = IM_COL32(245, 242, 234, 255);
            const ImVec2 q0(cx, cy - r), q1(cx + r, cy);
            const ImVec2 q2(cx, cy + r), q3(cx - r, cy);
            a_dl->AddQuadFilled(q0, q1, q2, q3, kFavCol);
            a_dl->AddQuad(q0, q1, q2, q3, oc, rim);
        }
    }

    void DrawGlow(ImDrawList* a_dl, RE::TESBoundObject* a_obj, std::uint8_t a_bits,
                  const ImVec2& a_iconMin, const ImVec2& a_iconMax,
                  const ImVec2& a_boxMin, const ImVec2& a_boxMax, int a_rot)
    {
        // ★★1.0.5: this used to paint the whole cell (and before that, a halo).
        // Both are gone — see Grid.h. What remains is the corner wedge, kept
        // behind the old name so the doll and the partner window keep their one
        // call site for "mark this item's rarity".
        // ★a_obj is read again as of 1.4.4 -- the museum status hangs off the
        // base form, and routing it through here is what gives the doll and the
        // partner window the same mark the board has, for free.
        (void)a_iconMin; (void)a_iconMax; (void)a_rot;
        DrawRarityWedge(a_dl, a_boxMin, a_boxMax, a_bits,
                        a_obj ? Lotd::Of(a_obj->GetFormID()) : Lotd::Status::kNotRelic);
    }

    namespace
    {
        // one queued read at a time — the Book Menu is modal anyway
        struct PendingRead
        {
            RE::FormID    form = 0;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
        };
        std::optional<PendingRead> g_pendingRead;
        // ★(1.5.x) a SHELF book's page (no owner involved) -- see
        // RequestShelfBookPage
        std::optional<PendingRead> g_pendingShelfPage;
        // ★A page still owed after the engine has read the book, settled one
        // tick later: a menu is raised through the UI queue, never
        // synchronously, so "did the engine open one?" cannot be asked now.
        std::optional<PendingRead> g_pageOwed;
        int g_pageOwedWait = 0;

        // ★★★IS THERE ANYTHING TO READ? Measured, side by side:
        //
        //   Shadowmarks           desc = "[pagebreak]..."   (4481 chars)
        //   Elder Scroll (Dragon) desc = "<Cool graphic>"   (14)
        //
        // `<Cool graphic>` is Bethesda's own note that this book's content is
        // a picture rather than text -- the scroll has no page, which is why
        // the one we raised was blank and why it stood in front of the unfurl
        // the engine had already started.
        //
        // The test is the meaning, not the length: a description that is
        // empty, or that is nothing but one bracketed marker, has no reader to
        // open. Real book text carries [pagebreak] or is simply prose, and
        // both pass.
        bool HasReadableText(const char* a_desc)
        {
            if (!a_desc) return false;
            const char* b = a_desc;
            while (*b && std::isspace(static_cast<unsigned char>(*b))) ++b;
            if (!*b) return false;
            std::string t(b);
            while (!t.empty() &&
                   std::isspace(static_cast<unsigned char>(t.back()))) {
                t.pop_back();
            }
            if (t.empty()) return false;
            // one bracketed marker and nothing else -- "<Cool graphic>"
            if (t.front() == '<' && t.back() == '>' &&
                t.find('>') == t.size() - 1) {
                return false;
            }
            return true;
        }
    }

    namespace
    {
        // ★★★HOW MANY OF THIS BOOK ARE HELD -- AND NOT FROM LiveEntryOf.
        //
        // LiveEntryOf answers from the CHANGES list, so a book nothing has
        // ever happened to has no entry there at all and reads as zero while
        // it sits plainly in the inventory. Grid.cpp:7301 already carries this
        // lesson for unequipped weapons; the book path walked into it again.
        //
        // Measured, one press apart:
        //   'The Book of the Dragonborn'  held 0->0   page suppressed
        //   'Line and Lure'               held 1->1   page raised
        // Nothing about the BOOKS differs. The fishing one was STOLEN, and
        // that theft flag is the change that gave it an entry to be found in.
        //
        // GetInventory is the source the rebuild itself walks, and it counts
        // the base container too. The returned map OWNS the entry, so it must
        // outlive every use of it and nothing may escape the call (원칙 2).
        [[nodiscard]] int HeldCountOf(RE::TESObjectREFR* a_owner,
                                      RE::TESBoundObject* a_obj)
        {
            if (!a_owner || !a_obj) return 0;
            auto inv = a_owner->GetInventory(
                [&](RE::TESBoundObject& o) { return &o == a_obj; });
            int n = 0;
            for (auto& [o, d] : inv) n = d.first;
            return n;
        }
    }

    void RequestBookRead(RE::TESObjectBOOK* a_book, std::uint16_t a_uid, std::uint16_t a_sig)
    {
        if (!a_book) return;
        g_pendingRead = PendingRead{ a_book->GetFormID(), a_uid, a_sig };
    }

    // ★(1.5.x) the SHELF read: a book the player does NOT own, read where it
    // lies. The inventory path above hands the book to the engine's Use --
    // which acts on the player's copy and, finding none, raised no page (the
    // 8/24 rework broke the 1.3.0 shelf read this way). The page itself
    // needs no owner: ShowBookPage raises TESBookReadEvent, so a skill book
    // still teaches exactly as reading it in the world does.
    void RequestShelfBookPage(RE::TESObjectBOOK* a_book, std::uint16_t a_uid,
                              std::uint16_t a_sig)
    {
        if (!a_book) return;
        g_pendingShelfPage = PendingRead{ a_book->GetFormID(), a_uid, a_sig };
    }

    // Raise the engine's page for a book we are only DISPLAYING. Handing it
    // the sub-stack keeps a per-instance name (quest alias / player rename) on
    // the page, and ref = nullptr is how a book held in the inventory is shown
    // rather than one lying in the world.
    //
    // ★★★THIS APPLIES THE BOOK, IT DOES NOT MERELY DRAW IT. Measured across
    // this one call: `spell 0 -> 1, held 2 -> 2` -- the tome is learned on the
    // spot and stays in the pack. That is why a tome was learned the instant it
    // was right-clicked, why nothing a skill gate hangs off ever ran, and why
    // the Elder Scroll opened to an empty page.
    //
    // ★★2026-08-24 CORRECTION -- this comment used to end "Display only; the
    // reading happens above", and that sent a whole day's hunt the wrong way.
    // It is NOT display only. Measured against a report that scripted books do
    // nothing when read from our board (The Dark Arts: Practical Necromancy):
    //
    //   [BOOK]    the engine raised no page -- showing it ourselves
    //   [BOOKEVT] ref=inventory uid=2                    <- 28ms after
    //   [MARKS]   perks 16->17 addedPerks 18->19 spells 20->21
    //
    // This call raises TESBookReadEvent, Papyrus' OnRead rides that same event
    // source, and the book's script ran to completion -- perk, scripted perk
    // and ability all arrived. The reading DOES happen here.
    //
    // What is true is that it is not the WHOLE reading: the engine's own menu
    // path spends a tome, and that spending is still done explicitly above.
    void ShowBookPage(RE::TESObjectBOOK* a_book, std::uint16_t a_uid,
                      std::uint16_t a_sig)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !a_book) return;
        auto* xl = ExtraForPool(LiveEntryOf(player, a_book), a_uid, a_sig);
        RE::BSString desc;
        a_book->GetDescription(desc, a_book);
        // ★★The NG line declares BookMenu::OpenBookMenu and never defines it,
        // so the call is made here through the same address-library id
        // CommonLib itself used. Same function, same arguments -- only the
        // hop through the library is ours now.
        using open_t = void(const RE::BSString&, const RE::ExtraDataList*,
                            RE::TESObjectREFR*, RE::TESObjectBOOK*,
                            const RE::NiPoint3&, const RE::NiMatrix3&, float, bool);
        static REL::Relocation<open_t> openBook{ RELOCATION_ID(50122, 51053) };
        openBook(desc, xl, nullptr, a_book,
                 RE::NiPoint3{}, RE::NiMatrix3{}, 1.0f, true);
    }

    void ProcessBookRead()
    {
        // ---- the page we may still owe, one tick after the engine read ----
        if (g_pageOwed) {
            // ★the engine's Use is QUEUED, so give it frames to land before
            // deciding it raised nothing
            if (--g_pageOwedWait > 0) return;
            const auto req = *g_pageOwed;
            g_pageOwed.reset();
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(req.form);
            auto* ui = RE::UI::GetSingleton();
            if (!player || !book || !ui) return;
            if (ui->IsMenuOpen(RE::BookMenu::MENU_NAME)) {
                SKSE::log::info("[BOOK] the engine raised its own page");
                return;
            }
            // consumed by the reading: there is nothing left to show.
            // ★The count comes from GetInventory, NOT the changes list -- see
            // HeldCountOf. A book with no changes entry is still a book the
            // player is holding, and suppressing its page was the whole bug.
            // The map is kept alive for `entry` below.
            auto inv = player->GetInventory(
                [&](RE::TESBoundObject& o) { return &o == book; });
            int                     held = 0;
            RE::InventoryEntryData* entry = nullptr;
            for (auto& [o2, d2] : inv) {
                held = d2.first;
                entry = d2.second.get();
            }
            if (held <= 0) {
                SKSE::log::info("[BOOK] read consumed it -- no page to raise");
                return;
            }
            // ★★★AND NOT IN FRONT OF ONE THE ENGINE TOOK OVER. The Elder Scroll
            // is EQUIPPED by the Use -- that is the unfurl, and the scroll's
            // script waits for the menus to be gone before it plays. Our page
            // is the thing standing in the way: the player had to press ESC
            // and close the inventory before anything happened.
            //
            // Being worn is the signal, and it is the honest one: a book the
            // engine has put in the player's hands is being used, not read,
            // and it never wanted a page.
            RE::BSString d;
            book->GetDescription(d, book);
            const bool worn = entry && entry->IsWorn();
            if (worn) {
                SKSE::log::info("[BOOK] the engine is using it -- no page");
                return;
            }
            if (!HasReadableText(d.c_str())) {
                // ★★AND WE GET OUT OF THE WAY. With no page to raise, the
                // engine's Use is the whole action -- for the Elder Scroll it
                // is the unfurl, and the script behind it waits for the menus
                // to be gone before it plays. Vanilla's inventory closes on a
                // Use like this; ours stayed open, so the player had to close
                // it by hand before anything happened.
                SKSE::log::info("[BOOK] nothing to read -- the engine has it");
                UIRoot::Close();
                return;
            }
            // a tome whose spell we now know has said everything it has to say
            if (auto* sp = book->GetSpell(); sp && player->HasSpell(sp)) {
                SKSE::log::info("[BOOK] spell already known -- no page to raise");
                return;
            }
            SKSE::log::info("[BOOK] the engine raised no page -- showing it ourselves");
            ShowBookPage(book, req.uid, req.sig);
            return;
        }

        // ---- ★(1.5.x) the shelf page: no owner, no engine Use -- the
        // page IS the read (TESBookReadEvent rides it), same as the world's
        if (g_pendingShelfPage) {
            const auto sreq = *g_pendingShelfPage;
            g_pendingShelfPage.reset();
            auto* sbook = RE::TESForm::LookupByID<RE::TESObjectBOOK>(sreq.form);
            auto* sui = RE::UI::GetSingleton();
            if (sbook && sui && !sui->IsMenuOpen(RE::BookMenu::MENU_NAME)) {
                SKSE::log::info("[BOOK] shelf read -- raising the page in place");
                ShowBookPage(sbook, sreq.uid, sreq.sig);
            }
            return;
        }

        if (!g_pendingRead) return;
        const auto req = *g_pendingRead;
        g_pendingRead.reset();

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(req.form);
        if (!player || !book) return;
        // already reading something — don't stack Book Menus
        if (auto* ui = RE::UI::GetSingleton();
            ui && ui->IsMenuOpen(RE::BookMenu::MENU_NAME)) {
            return;
        }
        // ★★★HAND IT TO THE ENGINE THE WAY "USE" DOES.
        //
        // Measured, in order, and each step ruled one thing out:
        //   OpenBookMenu   applies the book and keeps it -- neither read nor equip
        //   Read()         works for tomes only; 0 and no change on anything else
        //                  ★CORRECTION (2026-08-24): skill books too. Measured,
        //                  'The Importance of Where' -> `Read=true Use=false`
        //                  and the skill went up. Read() applies `teaches`,
        //                  whatever it teaches; it is anything with an EMPTY
        //                  teaches that it turns away.
        //   Activate       never fires, not even in the vanilla inventory
        //   BooksRead      fires from our path now, and the scroll still does
        //                  nothing -- so it is a tally, not the trigger
        //
        // What vanilla offers on a book is USE, and Use here already means
        // ActorEquipManager::EquipObject. For a book that is how Skyrim reads
        // one, and for the Elder Scroll it is the unfurl that was reported:
        // the inventory closes, the view goes first person, the scroll opens.
        auto* spell = book->GetSpell();
        const bool hadSpell = spell ? player->HasSpell(spell) : false;
        const int heldBefore = HeldCountOf(player, book);

        const bool took = book->Read(player);
        // Read settles a TOME (it teaches; the spending is below). Anything it
        // refuses is handed to the engine's Use instead, which is the door the
        // rest of the world's books go through.
        bool used = false;
        if (!took) used = Equip::UseItem(book, req.uid, -1, req.sig, {}, 1);

        const bool hasSpell = spell ? player->HasSpell(spell) : false;
        const int heldAfter = HeldCountOf(player, book);

        // ★★★AND THE TOME IS SPENT. Read() is the engine's door and it does
        // teach -- measured: `Read -> 1; spell 0 -> 1` on a spell the player
        // did not have. What it does NOT do is take the book: `held 2 -> 2`.
        // In the vanilla menu something downstream of the page spends it, and
        // that something is not reachable from here.
        //
        // So the spending is done explicitly, and only on the exact evidence
        // that it is owed: the read was accepted, the spell arrived across it,
        // and the count did not move. If a gate refuses the read -- vanilla's
        // or a mod's -- `took` is false, nothing was learned, and the book
        // stays, which is the whole point of asking the engine first.
        if (took && spell && hasSpell && !hadSpell && heldAfter == heldBefore &&
            heldAfter > 0) {
            player->RemoveItem(book, 1, RE::ITEM_REMOVE_REASON::kRemove,
                               nullptr, nullptr);
            SKSE::log::info("[BOOK] tome spent");
            NotePendingRemove(book, {}, 1, -1);
            RequestRebuild();
        }
        // the page is owed only if the engine raises none of its own
        g_pageOwed = req;
        g_pageOwedWait = 8;
        // ⓔⓖ PROBE. Two reports say our reading is not the game's reading: the
        // Dawnguard Elder Scroll does nothing at all, and a spell tome skips
        // the "you lack the skill" gate. Both would follow if OpenBookMenu
        // merely DRAWS THE PAGE while the engine's own door -- the one quest
        // fragments and third-party gates hang off -- is never opened.
        //
        // The engine offers two doors we do not use, TESObjectBOOK::Read and
        // ::Activate, and nothing local says which one raises the page. So
        // this measures the current path instead of guessing at theirs: if
        // IsRead() is still false after the page closes, our reading never
        // counted as one, and that is the whole bug.
        // ★★WHICH DOOR ANSWERED, and what moved. One report is "this one book
        // will not open, the others do" -- and the line above could not tell a
        // refusal from a success, because both reached it. `Read` is the tome
        // door, `Use` is the engine's Use that everything else goes through,
        // and the counts say whether the book or a spell actually moved. A
        // book that opens nowhere reads `Read=false Use=false` and names
        // itself; that is the whole diagnosis, from one press.
        SKSE::log::info(
            "[BOOK] read '{}' ({:08X}) -- Read={} Use={} held {}->{} spell {}->{}",
            DisplayNameOf(book, ExtraForPool(LiveEntryOf(player, book), req.uid, req.sig)),
            req.form, took, used, heldBefore, heldAfter, hadSpell, hasSpell);
    }

    std::string DefKeyOf(RE::TESForm* a_form)
    {
        return a_form ? FormKey(a_form) : std::string{};
    }

    const char* DisplayNameOf(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_xl,
                              RE::InventoryEntryData* a_entry)
    {
        if (!a_obj) return "";
        // ExtraTextDisplayData is where a per-instance name lives — a quest
        // alias substitution, or a name the player typed. Both are invisible
        // to TESForm::GetName(), which returns the raw record text.
        if (a_xl) {
            if (const char* n = a_xl->GetDisplayName(a_obj); n && *n) return n;
        } else if (a_entry) {
            if (const char* n = a_entry->GetDisplayName(); n && *n) return n;
        }
        const char* base = a_obj->GetName();
        return base ? base : "";
    }

    namespace
    {
        // Vanilla builds an effect line from the magic effect's DESCRIPTION
        // (MGEF > DNAM) with its <mag> / <dur> / <area> tags filled in. Replace
        // every occurrence; the tags are lowercase in the game data, but match
        // case-insensitively so hand-edited mod records still resolve.
        // ★One case-insensitive char compare for this file. Both tag fillers
        // carried their own identical copy of it -- the tags are lowercase in
        // the game data, and matching loosely is what lets a hand-edited mod
        // record resolve anyway.
        [[nodiscard]] bool IEq(char a_l, char a_r)
        {
            return std::tolower(static_cast<unsigned char>(a_l)) ==
                   std::tolower(static_cast<unsigned char>(a_r));
        }

        void FillTag(std::string& a_s, std::string_view a_tag, std::string_view a_val)
        {
            std::size_t i = 0;
            while (a_tag.size() <= a_s.size() && i <= a_s.size() - a_tag.size()) {
                if (std::equal(a_tag.begin(), a_tag.end(), a_s.begin() + i, IEq)) {
                    a_s.replace(i, a_tag.size(), a_val);
                    i += a_val.size();
                } else {
                    ++i;
                }
            }
        }

        // ★★★`[SURV=...]` is the ENGINE's conditional-description syntax, not a
        // mod's invention. Verified rather than assumed: the token "SURV" sits
        // in SkyrimSE.exe among the item card's own field names (`warmth`,
        // `warmthChange`, `numItemEffects`), USSEP ships a record that uses it,
        // and ccQDRSSE001-SurvivalMode.bsa holds 56 of them in every shipped
        // language. It marks text belonging to Creation Club Survival Mode:
        // shown while that mode is ON, dropped WHOLE while it is off.
        // Printing the DNAM verbatim put "[SURV=Restore <2> points of Hunger.]"
        // on a wine bottle's card, where vanilla shows nothing (user report).
        void StripSurvivalBlocks(std::string& a_s, bool a_keep)
        {
            static constexpr std::string_view kOpen = "[SURV=";
            // ★A cheap look before a copy. Effects carrying a SURV block are a
            // handful in the whole game, and this runs for every effect line of
            // a tooltip, every frame it is up -- so the 99% case should not be
            // paying for a rebuilt string. The '[' scan is the same work the
            // loop below would do anyway.
            if (a_s.find('[') == std::string::npos) return;
            std::string out;
            out.reserve(a_s.size());
            std::size_t i = 0;
            while (i < a_s.size()) {
                const bool hit = a_s.size() - i >= kOpen.size() &&
                                 std::equal(kOpen.begin(), kOpen.end(), a_s.begin() + i, IEq);
                if (!hit) {
                    out.push_back(a_s[i++]);
                    continue;
                }
                const std::size_t close = a_s.find(']', i + kOpen.size());
                if (close == std::string::npos) {
                    // ★Unterminated: copy the rest verbatim rather than eat it.
                    // A malformed record should look wrong, not go silent.
                    out.append(a_s, i, std::string::npos);
                    break;
                }
                if (a_keep) out.append(a_s, i + kOpen.size(), close - (i + kOpen.size()));
                i = close + 1;
            }
            a_s.swap(out);
        }

        // ★★An all-digit tag is a NUMBER, not a placeholder. Bethesda's own
        // Survival strings write the amount inside the brackets -- the archive
        // holds <2> / <18> / <220> / <380>, exactly the four hunger tiers, in
        // the same file that uses <mag> and <dur> correctly elsewhere. So there
        // is nothing to look up: the brackets are the only thing to remove.
        // Anything else in angle brackets is left alone -- an unknown tag
        // printed as-is is a visible fault, and inventing a value for it would
        // be a quiet one.
        void UnwrapNumericTags(std::string& a_s)
        {
            std::size_t i = 0;
            while (i < a_s.size()) {
                if (a_s[i] != '<') { ++i; continue; }
                std::size_t j = i + 1;
                while (j < a_s.size() && std::isdigit(static_cast<unsigned char>(a_s[j]))) ++j;
                if (j > i + 1 && j < a_s.size() && a_s[j] == '>') {
                    a_s.erase(j, 1);   // '>'
                    a_s.erase(i, 1);   // '<'
                    i = j - 1;         // one past the digits, both brackets gone
                } else {
                    ++i;
                }
            }
        }

        // The engine's OWN switch for the above -- the default object the game
        // itself reads, so this cannot disagree with it. Null (and false) when
        // the Creation Club content is not installed at all.
        // ★★★NEVER GetObject(DefaultObjectID) -- that overload is BROKEN, and it
        // took a CTD to find out. Its two lines look alike and are not:
        //     (&RelocateMember<bool>(this, 0xB80, 0xBA8))[idx]      <- correct
        //     &RelocateMember<TESForm**>(this, 0x20, 0x20)[idx]     <- wrong
        // The first takes the array's ADDRESS and then subscripts. The second
        // subscripts first, so it reads the VALUE at 0x20 -- objects[0], an
        // ordinary form pointer -- and uses that as the array base. 323 slots
        // past a TESForm is unmapped memory, and As<TESGlobal>() then read its
        // formType at +0x1A: an access violation on every tooltip with effects
        // on it, which is nearly every tooltip.
        // (Crash log signature, should it ever come back: `cmp byte ptr
        // [rcx+0x1A], 0x09` -- 0x1A is TESForm::formType and 9 is kGlobal.)
        // The size_t overload subscripts the real `objects` member instead, and
        // is what the SE/AE-only builds compile down to anyway.
        // ★The index comes from the enum, not from a literal. DefaultObjectID
        // packs (se | vr << 16); Survival Mode is SE/AE-only, so its high word
        // is zero and the low word IS the index -- which is also why VR has to
        // be turned away here rather than fed a 323 that means something else
        // entirely on that runtime.
        // ★Release-only caveat, stated so it is not a surprise: the size_t
        // overload asserts against DEFAULT_OBJECTS::kTotal, which a cross-VR
        // build compiles as 183. NDEBUG removes it; a Debug build would trip.
        [[nodiscard]] bool SurvivalModeOn()
        {
            if (REL::Module::IsVR()) return false;
            const auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
            if (!dom) return false;
            constexpr auto kIdx = static_cast<std::size_t>(
                std::to_underlying(RE::DefaultObjectID::kSurvivalModeEnabled) & 0xFFFF);
            const auto* g = dom->GetObject<RE::TESGlobal>(kIdx);
            return g && g->value != 0.0f;
        }

        // ★Both ends, in place. Leading/trailing space is what a dropped SURV
        // block leaves behind, and " " is not empty.
        void TrimInPlace(std::string& a_s)
        {
            const auto b = a_s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) { a_s.clear(); return; }
            a_s = a_s.substr(b, a_s.find_last_not_of(" \t\r\n") - b + 1);
        }
    }

    // ★ONE board for the whole screen. The partner window used to draw its
    // own hairline lattice, so a skin that carves or tiles its cells (SIMPLE,
    // the two Glass skins) got that treatment on the player's half and a bare
    // accent hairline on the merchant's — the two halves of the same screen
    // read as different UIs. The lattice is a SKIN decision, so it lives in
    // one function and every board asks for it.
    // ★★The ink skin's lattice, drawn AFTER the occupied ground instead of
    // under it. Every other skin draws its divider first and has the ground
    // step around it, which works because those dividers are one pixel wide on
    // a boundary the ground can dodge. A brush mark is three pixels and snaps
    // to the pixel grid; the ground does not, so "dodge half the width" was
    // right at some columns and wrong at others -- the grid showed through a
    // few lines and vanished at the rest. Drawing on top is the same answer the
    // open bag's tint already gives: the cells stay visible through whatever is
    // laid on them.
    void DrawInkLattice(ImDrawList* dl, const ImVec2& base, int a_cols, int a_rows)
    {
        if (!dl || !Theme::InkChrome()) return;
        const auto& sk = Theme::S();
        const float gridW = a_cols * CellPx();
        const float gridH = a_rows * CellPx();
        const float ith = Theme::InkRulePx();
        const float oth = Theme::InkEdgePx();
        const ImU32 ic = Theme::Col(sk.ink, 0.69f);
        const ImU32 oc = Theme::Col(sk.ink, 0.85f);
        for (int c = 1; c < a_cols; ++c) {
            Theme::InkRule(dl, ImVec2(base.x + c * CellPx(), base.y), gridH, ith, ic, true);
        }
        for (int r = 1; r < a_rows; ++r) {
            Theme::InkRule(dl, ImVec2(base.x, base.y + r * CellPx()), gridW, ith, ic);
        }
        Theme::InkStroke(dl, base, gridW, oth, oc);
        Theme::InkStroke(dl, ImVec2(base.x, base.y + gridH), gridW, oth, oc);
        Theme::InkStroke(dl, base, gridH, oth, oc, true);
        Theme::InkStroke(dl, ImVec2(base.x + gridW, base.y), gridH, oth, oc, true);
    }

    void ShadeCell(ImDrawList* dl, const ImVec2& base, int a_col, int a_row,
                   int a_cols, int a_rows, ImU32 a_col32)
    {
        const auto& sk = Theme::S();
        const float in0 = sk.engravedCells ? 0.0f
                        : Theme::InkChrome() ? 0.0f : 1.0f;
        const float in1 = sk.engravedCells
            ? Theme::kGrooveW * Theme::Scale() * 0.5f
            : Theme::InkChrome() ? 0.0f : 1.0f;
        const ImVec2 p0(base.x + a_col * CellPx(), base.y + a_row * CellPx());
        const ImVec2 p1(p0.x + CellPx(), p0.y + CellPx());
        dl->AddRectFilled(
            ImVec2(p0.x + (a_col > 0 ? in1 : in0),
                   p0.y + (a_row > 0 ? in1 : in0)),
            ImVec2(p1.x - (a_col + 1 < a_cols ? in1 : in0),
                   p1.y - (a_row + 1 < a_rows ? in1 : in0)),
            a_col32);
    }

    void DrawCellLattice(ImDrawList* dl, const ImVec2& base, int a_cols, int a_rows)
    {
        const auto& sk = Theme::S();
        const float gridW = a_cols * CellPx();
        const float gridH = a_rows * CellPx();
        if (Theme::InkChrome()) {
            // ★★Drawn LATE, from DrawInkLattice, not here. See its comment:
            // the marks go OVER the occupied ground rather than being cleared
            // around by it.
        } else if (sk.engravedCells) {
            // ★TILES ON THE PANEL, not a carved lattice. The divider is
            // the WINDOW itself: the gap between cells is simply left
            // unpainted, which is the only value that actually equals the
            // panel — painting the panel's own colour there would stack a
            // second coat and come out darker, not identical.
            // ★The cell face carries the COLOUR the old two-layer stack
            // composited to (groove .85 under face .85). Taking the groove
            // away without that would have lightened every cell, and only
            // the divider was meant to change. Its alpha is the skin's own
            // choice of how much room shows through -- not that arithmetic.
            const float g = Theme::kGrooveW * Theme::Scale();
            const ImU32 face  = Theme::Col(sk.cellBg);
            const ImU32 inner = Theme::Col(sk.cellGroove, sk.cellGroove.w * 0.85f);
            for (int r = 0; r < a_rows; ++r) {
                for (int c = 0; c < a_cols; ++c) {
                    // ★A groove exists BETWEEN cells, never outside the
                    // board, and each of the two neighbours gives HALF of
                    // it. Taking the whole groove off one side shifted
                    // every cell face up-left by g/2 while the item icon
                    // still centred on the true cell — so the whole grid
                    // looked offset and the icons looked pushed right.
                    const float l = (c > 0) ? g * 0.5f : 0.0f;
                    const float t = (r > 0) ? g * 0.5f : 0.0f;
                    const float rr = (c + 1 < a_cols) ? g * 0.5f : 0.0f;
                    const float bb = (r + 1 < a_rows) ? g * 0.5f : 0.0f;
                    const ImVec2 p0(base.x + c * CellPx() + l, base.y + r * CellPx() + t);
                    const ImVec2 p1(base.x + (c + 1) * CellPx() - rr,
                                    base.y + (r + 1) * CellPx() - bb);
                    dl->AddRectFilled(p0, p1, face);
                    dl->AddLine(ImVec2(p0.x, p0.y + 0.5f), ImVec2(p1.x, p0.y + 0.5f), inner);
                    dl->AddLine(ImVec2(p0.x + 0.5f, p0.y), ImVec2(p0.x + 0.5f, p1.y), inner);
                }
            }
        } else if (sk.translucent) {
            // ★★A TRANSLUCENT panel cannot say "cell" with colour. The
            // hairline below is the accent at 13%, and on Glass that
            // accent is a rust red — over a dark cave there is nothing
            // for it to differ from, so the board simply vanished
            // ("타일 색상이 구분이 안된다"). Anything painted here
            // composites with whatever the player is standing in front
            // of, and that changes every frame.
            // A CARVED line does not depend on the ground: a dark stroke
            // and a light one side by side means at least one of the two
            // is always unlike what is behind it. Same trick the SIMPLE
            // board uses, minus the filled face — the see-through panel
            // is the whole point of these skins.
            // Cell edges: the cell's top and left go DARK and its bottom
            // and right go LIGHT, which is the sunken read (light from
            // the top-left, rule 105).
            const float thin = 1.0f - sk.winBg.w;   // .25 Dark / .40 Clear
            // ★Strength follows the panel's own alpha: the line is drawn
            // ON the panel, and the less panel there is the harder it has
            // to work. Derived, so a future translucent skin needs no new
            // number of its own.
            const ImU32 dk = IM_COL32(0, 0, 0,
                static_cast<int>(255.0f * (0.34f + 0.50f * thin) + 0.5f));
            const ImU32 lt = IM_COL32(255, 255, 255,
                static_cast<int>(255.0f * (0.07f + 0.31f * thin) + 0.5f));
            // 0..cols inclusive: the outermost cells need their carve too,
            // or the top row and left column read as half-finished
            for (int c = 0; c <= a_cols; ++c) {
                const float x = base.x + c * CellPx();
                dl->AddLine(ImVec2(x - 0.5f, base.y), ImVec2(x - 0.5f, base.y + gridH), lt);
                dl->AddLine(ImVec2(x + 0.5f, base.y), ImVec2(x + 0.5f, base.y + gridH), dk);
            }
            for (int r = 0; r <= a_rows; ++r) {
                const float y = base.y + r * CellPx();
                dl->AddLine(ImVec2(base.x, y - 0.5f), ImVec2(base.x + gridW, y - 0.5f), lt);
                dl->AddLine(ImVec2(base.x, y + 0.5f), ImVec2(base.x + gridW, y + 0.5f), dk);
            }
        } else {
        // ★A faint ground under every cell, on light panels only. Without it a
        // pale sheet and a pale item picture have nothing between them — the
        // white sacks in the screenshot sat ON the paper with no cell to sit
        // IN. Dark skins never needed it: their panel already is the ground.
        if (sk.cellBg.w > 0.0f) {
            const ImU32 face = Theme::Col(sk.cellBg);
            for (int r = 0; r < a_rows; ++r) {
                for (int c = 0; c < a_cols; ++c) {
                    const ImVec2 p0(base.x + c * CellPx(), base.y + r * CellPx());
                    dl->AddRectFilled(p0,
                        ImVec2(p0.x + CellPx() - 1.0f, p0.y + CellPx() - 1.0f), face);
                }
            }
        }
        // v9: hairline cell grid inside an acc 20% outer border
        // ★Alpha .13 is tuned for a bright accent on a dark panel. On a light
        // panel acc IS the dark colour, but it is being laid over a pale sheet
        // where 13% of anything is invisible — the board had no cells at all.
        const ImU32 lineCol = Theme::Acc(sk.lightPanel ? 0.30f : 0.13f);
        for (int c = 1; c < a_cols; ++c) {
            dl->AddLine(ImVec2(base.x + c * CellPx(), base.y),
                ImVec2(base.x + c * CellPx(), base.y + gridH), lineCol);
        }
        for (int r = 1; r < a_rows; ++r) {
            dl->AddLine(ImVec2(base.x, base.y + r * CellPx()),
                ImVec2(base.x + gridW, base.y + r * CellPx()), lineCol);
        }
        }
    }

    namespace
    {
        // (1.3.1) defined with the recharge window further down; the tooltip
        // needs it to decide whether T is worth offering on this unit.
        bool UnitCharge(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_xl,
                        bool a_worn, int a_hand, float& a_cur, float& a_max);
    }

    void DrawItemTooltip(RE::TESBoundObject* a_obj, int a_count, int a_coinValue,
                         int a_price, bool a_isBuy, RE::TESObjectREFR* a_owner,
                         ExtraScope a_scope, std::uint16_t a_uid, int a_xlIdx,
                         std::uint16_t a_sig, int a_hand, const TileContext& a_tile)
    {
        if (!a_obj) return;

        // The OWNER's inventory entry: poison/charge/soul/crafted-enchant extras
        // all live there, not on the base form.
        // D1: this used to read the player unconditionally, so a merchant's
        // ordinary sword displayed the player's own sword's extras.
        // The map OWNS the entry copies (unique_ptr) — it must outlive every
        // extraOf call below, or `entry` dangles (was a real CTD: freed
        // entry->extraLists reused by the heap mid-tooltip).
        auto* player = RE::PlayerCharacter::GetSingleton();   // SHIFT-compare / spell tome
        RE::TESObjectREFR* owner = a_owner;
        if (!owner) owner = player;
        RE::TESObjectREFR::InventoryItemMap inv;
        RE::InventoryEntryData* entry = nullptr;
        if (owner) {
            inv = owner->GetInventory(
                [&](RE::TESBoundObject& o) { return &o == a_obj; });
            for (auto& [o2, d2] : inv) {
                entry = d2.second.get();
                break;
            }
        }
        // GI1: and WHICH sub-stack of that entry. kAny keeps the historical
        // "first list carrying the trait" behaviour for aggregate cells.
        RE::ExtraDataList* scoped = nullptr;
        switch (a_scope) {
        case ExtraScope::kUnit:
            scoped = ExtraForTile(entry, a_uid, a_xlIdx);
            // ★★★...AND BY POOL WHEN THE TILE CANNOT BE NAMED. A unit sitting
            // in a container on our side of a transfer has no uid (the engine
            // assigns none) and no recorded position (xlIdx is -1), so the
            // lookup above misses and a tempered dagger reads as plain --
            // measured, two of them in one barrel both lost their name while
            // their signature matched the held list exactly.
            //
            // ★This is NOT the entry fallback the note below forbids, and the
            // difference is the whole point: the entry's name is its FIRST
            // sub-stack's, borrowed by units that are nothing like it, whereas
            // a signature match means the same contents -- same temper, same
            // enchantment, same name. Reading either is reading this unit's.
            if (!scoped && a_sig != 0) scoped = ExtraForPool(entry, a_uid, a_sig);
            break;
        case ExtraScope::kWorn:
            scoped = WornExtraMatching(entry, a_uid, a_sig, a_hand);
            break;
        case ExtraScope::kAny:  break;
        }

        // ★★THE TOOLTIP SITS WHERE ImGui PUTS IT, and that is deliberate.
        //
        // 1.4.1 tried pushing the box clear of the game's cursor, because the
        // arrow was covering the item's NAME (reported). It was the wrong fix:
        // the clearance has to be guessed from a cursor size we do not know,
        // every cursor replacer ships a different one, and at any guess large
        // enough to work the box floats far away from the tile it describes.
        // Reverted on that reasoning -- a number tuned to one machine's cursor
        // is wrong on everyone else's.
        Theme::PushTipStyle();
        ImGui::BeginTooltip();
        // ★GI61: a UNIT-scoped tile with no extra list means this unit HAS
        // none — NOT "look the name up somewhere else". An entry's display
        // name is its FIRST sub-stack's, so handing the entry over here made
        // every plain dagger borrow the tempered one's name ("Fine Dagger"
        // x3) while the temper badge and the rest of the tooltip, which read
        // the unit's own extra data, correctly showed only one. Only an
        // AGGREGATE cell may fall back to the entry.
        const char* nm = DisplayNameOf(a_obj, scoped,
            a_scope == ExtraScope::kAny ? entry : nullptr);
        if (a_count > 1) {
            ImGui::TextColored(Theme::TipBody(), "%s  x%d", nm, a_count);
        } else {
            ImGui::TextColored(Theme::TipBody(), "%s", nm);
        }
        // ★★Directly under the NAME, as a subtitle: "Iron Greatsword / Greatsword"
        // is how the eye expects a kind to be told, and it is the one fact here
        // that qualifies the name rather than measuring the item. Printed bare --
        // a caption in front would be the longer half of the line.
        // ★Asked of Equip so the tooltip and the doll give ONE answer. Everything
        // this UI has no slot for reads "Accessory (47)": the number, because that
        // is what mod pages quote and what two cloaks fighting over one slot have
        // in common. Empty for anything not worn, which is most of the grid.
        if (const std::string slot = Equip::SlotLabel(a_obj); !slot.empty()) {
            ImGui::TextColored(Theme::TipHead(), "%s", slot.c_str());
        }
        // ★A BOOK YOU HAVE READ SAYS SO, which is what vanilla's list does and
        // what a shelf of two hundred titles needs to be usable at all. The
        // flag lives on the BASE FORM, so this is per TITLE rather than per
        // copy -- the same grain vanilla marks at, and the honest one: having
        // read one copy is having read the book.
        if (const auto* bk = a_obj->As<RE::TESObjectBOOK>();
            bk && bk->IsRead() && !bk->TeachesSpell()) {
            ImGui::TextColored(Theme::TipState(), "%s", Lang::T(Lang::Str::BookRead));
        }
        // ★★THE MUSEUM LINE, and it is said in EVERY case -- which is the half
        // of the design the wedge cannot carry. A donated relic hands its wedge
        // back to its own rarity, so on the board a donated unique is
        // indistinguishable from an ordinary unique; here is where that fact
        // still lives. "Safe to sell" is the question this answers, and it is a
        // question asked of one item at a time, not of a bag being skimmed.
        switch (Lotd::Of(a_obj->GetFormID())) {
        case Lotd::Status::kUndonated:
            ImGui::TextColored(Theme::TipState(), "%s", Lang::T(Lang::Str::MuseumOwed));
            break;
        case Lotd::Status::kDonated:
            ImGui::TextColored(Theme::TipState(), "%s", Lang::T(Lang::Str::MuseumDone));
            break;
        default: break;
        }
        // ★The armour CLASS reads as a second qualifier of the same kind as the
        // slot -- "Body", then "Heavy Armor" -- so it belongs on its own line
        // directly under it, not appended to the rating below (user's layout
        // call, and it is the better one: the rating is a measurement, the
        // class is what the thing IS).
        if (const auto* armoCls = a_obj->As<RE::TESObjectARMO>()) {
            switch (armoCls->GetArmorType()) {
            case RE::BIPED_MODEL::ArmorType::kLightArmor:
                ImGui::TextColored(Theme::TipHead(), "%s", Lang::T(Lang::Str::ArmorLight));
                break;
            case RE::BIPED_MODEL::ArmorType::kHeavyArmor:
                ImGui::TextColored(Theme::TipHead(), "%s", Lang::T(Lang::Str::ArmorHeavy));
                break;
            default:
                ImGui::TextColored(Theme::TipHead(), "%s", Lang::T(Lang::Str::ArmorClothing));
                break;
            }
        }
        const bool isPouch = GoldCoins::IsPouch(a_obj->GetFormID());
        if (a_coinValue >= 0) {   // G2: represented / stored gold
            // GI64: the pouch prints "stored / cap". Without the cap there was
            // no way to learn the limit short of filling it.
            if (isPouch) {
                ImGui::TextColored(Theme::TipHead(), "%s / %s G", Commas(a_coinValue).c_str(),
                    Commas(GoldCoins::PouchCapOfForm(a_obj->GetFormID())).c_str());
            } else {
                ImGui::TextColored(Theme::TipHead(), "%dG", a_coinValue);
            }
        }

        // GI64: what this thing is FOR. Only the two items whose behaviour is
        // ours rather than the game's -- everything else explains itself through
        // its own stats.
        //
        // ★★Bag-ness is the one entry in TileContext that IS derivable from the
        // object, and the wares list passed `false` because it has no tile def
        // to read -- so a bag on a merchant's shelf described nothing at all,
        // when its capacity is the only thing you need before buying one.
        // Ask the def instead of trusting the caller.
        // ★Deliberately NOT folded into a_tile.isBag: that flag also picks the
        // right-click verb, and it is tested BEFORE `partner` below -- a bag on
        // a shelf must stay "buy", not become "open bag".
        const bool describesBag = a_tile.isBag || ResolveDef(a_obj).bag != 0;
        const char* rmb = UIRoot::KeyLabel(UIRoot::Act::kSecondary);
        // ★The last line of each block is an ACTION ("right-click to open" /
        // "to withdraw"), and right-click does something else entirely on the
        // other side of a barter window (buy) or in the trash (restore).
        // Everything above it describes the thing and is still true while
        // deciding, so only the action line drops out.
        // ★This mirrors the verb chain below: the bag/pouch verb is only
        // reached when none of equipSlot / parked / partner claimed it. If that
        // order ever changes, this has to change with it.
        const bool ownAction = !a_tile.partner && !a_tile.parked && !a_tile.equipSlot;
        if (isPouch) {
            ImGui::Separator();
            ImGui::TextColored(Theme::TipSub(), "%s", Lang::T(Lang::Str::PouchLine1));
            ImGui::TextColored(Theme::TipSub(), "%s", Lang::T(Lang::Str::PouchLine2));
            if (ownAction) {
                ImGui::TextColored(Theme::TipSub(), Lang::T(Lang::Str::PouchLine3), rmb);
            }
        } else if (describesBag) {
            const auto bd = ResolveDef(a_obj);
            const int bw = (std::max)(1, bd.bw);
            const int bh = (std::max)(1, bd.bh);
            ImGui::TextColored(Theme::TipHead(), "%s · ", Lang::T(Lang::Str::BagLabel));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(Theme::TipVal(), Lang::T(Lang::Str::BagCells), bw, bh, bw * bh);
            ImGui::Separator();
            // ★A typed bag says so BEFORE the player tries (rule 75). The red
            // ghost on a bad drop is the answer at the moment of failure; this
            // is the answer while they are still deciding.
            if (!bd.accept.empty()) {
                ImGui::TextColored(Theme::TipBad(), Lang::T(Lang::Str::BagOnly), BagFilter::DisplayName(bd.accept));
            } else {
                ImGui::TextColored(Theme::TipSub(), "%s", Lang::T(Lang::Str::BagLine1));
            }
            if (ownAction) {
                ImGui::TextColored(Theme::TipSub(), Lang::T(Lang::Str::BagLine2), rmb);
            }
        }

        // One effect per line — shared by potions/ingredients and enchantments.
        // Matching vanilla means matching BOTH halves of what its item card
        // does: WHICH effects are shown, and WHAT each line says. This used to
        // key off the effect NAME, which is the opposite set from vanilla's:
        // enchantment effects mostly have no name and only a description, so
        // an enchanted robe printed a hidden helper effect ("Fortify Health
        // 25") and none of the two lines the game itself shows.
        // Once per tooltip, not once per effect line — the answer cannot change
        // between two lines of the same card.
        const bool survivalOn = SurvivalModeOn();
        auto effectLine = [&](RE::Effect* a_e, const ImVec4& a_col) {
            auto* base = a_e ? a_e->baseEffect : nullptr;
            if (!base) return;
            // The engine hides these from every item card and the magic menu:
            // enchantments carry helper effects not meant to be read.
            using EFlag = RE::EffectSetting::EffectSettingData::Flag;
            if (base->data.flags.all(EFlag::kHideInUI)) return;

            // GetMagnitude()/GetDuration()/GetArea() honour the kNoMagnitude /
            // kNoDuration / kNoArea flags, so a magnitude-less effect can't
            // print a stray 0.
            const float         mag  = a_e->GetMagnitude();
            const std::uint32_t dur  = a_e->GetDuration();
            const std::uint32_t area = a_e->GetArea();

            std::string line;
            // ★★HAD a description is a different question from what is LEFT of
            // one, and the two branches keep them apart. A Survival-only
            // description resolves to nothing once its block is dropped, and
            // vanilla then prints no line at all -- so this branch returns
            // rather than falling through and inventing "Restore Hunger 2".
            // The name form below is for effects that never had a description.
            // ★if/else, not a flag: a `hadDesc` bool mirrored which branch had
            // been taken, and a mirror is a second thing to keep true.
            const char* const desc = base->magicItemDescription.c_str();
            if (desc && *desc) {
                line = desc;
                char v[32];
                std::snprintf(v, sizeof(v), "%.0f", mag);
                FillTag(line, "<mag>", v);
                std::snprintf(v, sizeof(v), "%u", dur);
                FillTag(line, "<dur>", v);
                std::snprintf(v, sizeof(v), "%u", area);
                FillTag(line, "<area>", v);
                // ★After the tags, so a <mag> INSIDE a kept block is already a
                // number by the time the block is unwrapped.
                StripSurvivalBlocks(line, survivalOn);
                UnwrapNumericTags(line);
                TrimInPlace(line);
                if (line.empty()) return;   // resolved to nothing: vanilla shows none
            } else {
                // No description (common on crafted and mod-added effects):
                // fall back to the old "Name 50 (10s)" form.
                const char* n = base->GetName();
                if (!n || !*n) return;
                char b[160];
                if (mag > 0.0f && dur > 0) {
                    std::snprintf(b, sizeof(b), "%s %.0f (%us)", n, mag, dur);
                } else if (mag > 0.0f) {
                    std::snprintf(b, sizeof(b), "%s %.0f", n, mag);
                } else if (dur > 0) {
                    std::snprintf(b, sizeof(b), "%s (%us)", n, dur);
                } else {
                    std::snprintf(b, sizeof(b), "%s", n);
                }
                line = b;
            }
            // Descriptions are sentences — wrap them at the same width as the
            // flavour text below rather than stretching the tooltip.
            ImGui::PushTextWrapPos(300.0f * Theme::Scale());
            ImGui::TextColored(a_col, "%s", line.c_str());
            ImGui::PopTextWrapPos();
        };

        auto extraOf = [&]<class T>() -> T* {
            if (a_scope != ExtraScope::kAny) return scoped ? scoped->GetByType<T>() : nullptr;
            if (!entry || !entry->extraLists) return nullptr;
            for (auto* xl : *entry->extraLists) {
                if (!xl) continue;
                if (auto* x = xl->GetByType<T>()) return x;
            }
            return nullptr;
        };

        // Card values match vanilla only via the engine functions that fold in
        // the armor/weapon SKILL multiplier + perks + temper — GetArmorRating()/
        // GetAttackDamage() return only the base form value (Steel cuirass base
        // 31 vs card 31*(1+0.4*15/100)=32.86->round->33). PlayerCharacter::
        // GetArmorValue/GetDamage take an InventoryEntryData; a throwaway entry
        // (no extraLists = no temper) still applies skill+perks — which is what
        // the player asked to match. Round, don't truncate.
        // Diablo-style SHIFT compare: while shift is held over a weapon/armor,
        // find the equipped counterpart (same hand / overlapping biped slot),
        // append a signed diff to the stat line and show an "Equipped" card
        // beside this tooltip (rendered after EndTooltip below).
        RE::TESBoundObject* cmpObj = nullptr;
        int  cmpVal = 0;
        bool cmpIsWeap = false;
        const bool wantCmp = ImGui::GetIO().KeyShift;
        auto diffText = [&](int a_mine) {
            if (!cmpObj) return;
            const int d = a_mine - cmpVal;
            const ImVec4 c = d > 0 ? ImVec4(0.47f, 0.78f, 0.47f, 1.0f)
                           : d < 0 ? ImVec4(0.8f, 0.32f, 0.28f, 1.0f)
                                   : Theme::TipSub();
            ImGui::SameLine();
            ImGui::TextColored(c, "(%+d)", d);
        };

        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) {
            int dmg = static_cast<int>(weap->GetAttackDamage());
            if (pc) {
                RE::InventoryEntryData e(a_obj, 1);
                dmg = static_cast<int>(std::lroundf(pc->GetDamage(&e)));
            }
            if (wantCmp && pc) {
                RE::TESForm* eq = pc->GetEquippedObject(false);
                if (!eq || !eq->As<RE::TESObjectWEAP>()) eq = pc->GetEquippedObject(true);
                if (auto* ew = eq ? eq->As<RE::TESObjectWEAP>() : nullptr) {
                    RE::InventoryEntryData ee(ew, 1);
                    cmpObj = ew;
                    cmpVal = static_cast<int>(std::lroundf(pc->GetDamage(&ee)));
                    cmpIsWeap = true;
                }
            }
            // ★DIAG: "all weapon tooltips show damage as 0" (reported, not
            // reproducible here). We do not compute this -- GetDamage is the
            // engine's own routine, reached by address, so a 0 means the engine
            // returned one. Logging the RECORD's damage beside it says which:
            // base 0 too means the weapon record itself is empty, base non-zero
            // means the engine's adjusted value collapsed. Once per form, so a
            // hover cannot flood the log.
            if (dmg == 0) {
                static std::unordered_set<RE::FormID> s_said;
                if (s_said.insert(a_obj->GetFormID()).second) {
                    SKSE::log::warn("[TIP] weapon damage 0: '{}' ({:08X}) base={} pc={}",
                        a_obj->GetName(), a_obj->GetFormID(),
                        weap->GetAttackDamage(), pc ? "y" : "n");
                }
            }
            ImGui::TextColored(Theme::TipVal(), "%s %d", Lang::T(Lang::Str::Damage), dmg);
            diffText(dmg);
        } else if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            int arm = static_cast<int>(armo->GetArmorRating());
            if (pc) {
                RE::InventoryEntryData e(a_obj, 1);
                arm = static_cast<int>(std::lroundf(pc->GetArmorValue(&e)));
            }
            if (wantCmp && pc && player) {
                const auto hmask = static_cast<std::uint32_t>(armo->GetSlotMask().get());
                // ★★FILTER IT. GetInventory copies every entry it accepts --
                // a new InventoryEntryData and a new BSSimpleList each -- and
                // this asked for the WHOLE pack, every frame Shift was held
                // over an armour tooltip. The loop then threw away all but the
                // worn armour that shares a biped slot, which is a test the
                // filter can make before anything is copied. (The weapon
                // comparison a few lines up already passes one.)
                auto winv = player->GetInventory([&](RE::TESBoundObject& o) {
                    auto* wa = o.As<RE::TESObjectARMO>();
                    return wa && (static_cast<std::uint32_t>(wa->GetSlotMask().get())
                                  & hmask) != 0;
                });
                for (auto& [o2, d2] : winv) {
                    auto* e2 = d2.second.get();
                    if (!o2 || !e2 || !e2->IsWorn()) continue;
                    // ★Never compare against the costume anchor. It is a 0-rating
                    // placeholder, so every helmet would tout "+N over what you
                    // are wearing" against a slot the player considers empty.
                    if (Costume::IsAnchor(o2)) continue;
                    auto* wa = o2->As<RE::TESObjectARMO>();
                    if (!wa) continue;
                    if ((static_cast<std::uint32_t>(wa->GetSlotMask().get()) & hmask) == 0) continue;
                    RE::InventoryEntryData ee(wa, 1);
                    cmpObj = wa;
                    cmpVal = static_cast<int>(std::lroundf(pc->GetArmorValue(&ee)));
                    break;
                }
            }
            // (the CLASS is printed up beside the slot, where it reads as what
            // the piece IS rather than as part of its measurement)
            ImGui::TextColored(Theme::TipVal(), "%s %d", Lang::T(Lang::Str::Armor), arm);
            diffText(arm);
        } else {
            RE::MagicItem* magic = a_obj->As<RE::AlchemyItem>();
            // ★An INGREDIENT only tells you what you have LEARNED (user report
            // ②: we listed all four from the start, which is the alchemy game
            // given away). The engine keeps the discovery per ingredient FORM
            // in gamedata.knownEffectFlags -- one bit per effect, indexed into
            // the same effects array -- so eating one or brewing with it turns
            // its line on by itself, with nothing for us to track. A potion is
            // not gated: its label says what it does.
            std::uint16_t known = 0xFFFF;
            // ★★A SCROLL IS A MagicItem TOO, and its card was blank. The same
            // omission the spell tome had: this block only ever asked about
            // potions and ingredients, so a scroll -- an item whose entire
            // point is the spell inside it -- listed nothing at all. Not
            // gated like an ingredient: a scroll's label says what it does.
            if (!magic) magic = a_obj->As<RE::ScrollItem>();
            if (!magic) {
                if (auto* ingr = a_obj->As<RE::IngredientItem>()) {
                    magic = ingr;
                    known = ingr->gamedata.knownEffectFlags;
                }
            }
            if (magic) {
                std::uint16_t bit = 1;
                for (auto* e : magic->effects) {
                    if (known & bit) {
                        effectLine(e, Theme::TipBody());
                    } else {
                        // ★An unknown effect keeps its PLACE. Vanilla's item
                        // card simply omits it, which left a freshly picked
                        // flower with a blank card -- indistinguishable from
                        // an item we failed to read (user call, and the right
                        // one). The alchemy menu's own grammar is a row of
                        // question marks, so the card borrows it: four lines
                        // that fill in one at a time as the ingredient is
                        // learned, and the count itself tells you how much of
                        // this plant you have left to discover.
                        ImGui::TextColored(Theme::TipSub(), "???");
                    }
                    bit = static_cast<std::uint16_t>(bit << 1);
                }
            }
        }

        // temper (grindstone / workbench). ExtraHealth::health is a damage/armour
        // MULTIPLIER: 1.0 = untempered, 1.25 = +25%. It gets its own line because
        // the stat line above is computed from a throwaway entry that carries no
        // extras by design -- and because tempering is the cheapest per-instance
        // difference a player can create, so this is what makes the GI1 tile
        // binding visible at all (before this, tempering showed NOTHING).
        if (const auto* xh = extraOf.operator()<RE::ExtraHealth>();
            xh && xh->health > 1.0f) {
            ImGui::TextColored(Theme::TipGood(), "%s +%d%%", Lang::T(Lang::Str::TemperLabel),
                static_cast<int>(std::lroundf((xh->health - 1.0f) * 100.0f)));
        }

        // spell tome: what it teaches (right-click learns it), known marker
        if (auto* book = const_cast<RE::TESObjectBOOK*>(a_obj->As<RE::TESObjectBOOK>());
            book && book->TeachesSpell()) {
            if (auto* spell = book->GetSpell()) {
                const bool known = player && player->HasSpell(spell);
                ImGui::TextColored(Theme::TipGood(), "%s: %s%s%s%s",
                    Lang::T(Lang::Str::Teaches), spell->GetName(),
                    known ? " (" : "", known ? Lang::T(Lang::Str::Known) : "",
                    known ? ")" : "");
                // ★★★AND WHAT THE SPELL ACTUALLY DOES.
                //
                // The card named the spell and stopped there, while vanilla
                // spells out the effect -- "Targets take 20 points of frost
                // damage for 10 seconds, plus Stamina damage" (reported, with
                // the vanilla card beside ours). Deciding whether a 1350-gold
                // tome is worth buying is exactly what that line is for.
                //
                // A tome's DESC is not this: books are excluded from the
                // description block below because their DESC is the book's
                // text. The effect belongs to the SPELL, and effectLine
                // already renders one the way vanilla does -- tags filled,
                // hidden helpers skipped.
                for (auto* e : spell->effects) effectLine(e, Theme::TipBody());
            }
        }

        // enchantment — base record (EITM) or player-crafted (ExtraEnchantment)
        {
            RE::EnchantmentItem* ench = nullptr;
            std::uint16_t maxCharge = 0;
            if (const auto* ef = a_obj->As<RE::TESEnchantableForm>()) {
                ench = ef->formEnchanting;
                maxCharge = ef->amountofEnchantment;
            }
            // ★An enchantment stuck on the ITEM is used only when the record
            // carries none. The engine refuses to re-enchant something that is
            // already enchanted, so the two do not normally coexist — and where
            // they do, the record is what the thing IS. Vanilla's own item card
            // reads it that way (measured on a robe carrying both).
            //
            // Letting the attached one win is not a cosmetic choice: any mod
            // that rides an enchantment slot as a MARKER — our own socket
            // extension does exactly that on already-enchanted gear — would
            // otherwise blank out the item's real description, since a marker
            // effect is deliberately hidden from the UI. One robe in a test save
            // showed "Increases your Health by 25 points." in place of both of
            // its actual effects.
            if (!ench) {
                if (auto* xe = extraOf.operator()<RE::ExtraEnchantment>();
                    xe && xe->enchantment) {
                    ench = xe->enchantment;
                    maxCharge = xe->charge;
                }
            }
            if (ench) {
                for (auto* e : ench->effects) effectLine(e, Theme::TipGood());
                // charge (weapons drain per hit; armour enchants don't)
                if (a_obj->Is(RE::FormType::Weapon) && maxCharge > 0) {
                    float cur = static_cast<float>(maxCharge);
                    if (auto* xc = extraOf.operator()<RE::ExtraCharge>()) {
                        cur = xc->charge;
                    }
                    ImGui::TextColored(Theme::TipVal(), "%s %d / %d",
                        Lang::T(Lang::Str::ChargeLabel),
                        static_cast<int>(cur), static_cast<int>(maxCharge));
                }
            }
        }

        // applied poison (weapons)
        if (auto* xp = extraOf.operator()<RE::ExtraPoison>(); xp && xp->poison) {
            ImGui::TextColored(Theme::TipGood(), "%s: %s (x%u)",
                Lang::T(Lang::Str::PoisonLabel), xp->poison->GetName(), xp->count);
        }

        // soul gem fill state (entry override wins over the base record)
        if (const auto* gem = a_obj->As<RE::TESSoulGem>()) {
            RE::SOUL_LEVEL lvl = gem->GetContainedSoul();
            if (auto* xs = extraOf.operator()<RE::ExtraSoul>()) {
                lvl = xs->GetContainedSoul();
            }
            if (lvl != RE::SOUL_LEVEL::kNone) {
                static constexpr Lang::Str kSoulNames[] = {
                    Lang::Str::SoulPetty, Lang::Str::SoulPetty, Lang::Str::SoulLesser,
                    Lang::Str::SoulCommon, Lang::Str::SoulGreater, Lang::Str::SoulGrand,
                };
                const auto idx = (std::min)(static_cast<size_t>(lvl),
                    std::size(kSoulNames) - 1);
                ImGui::TextColored(Theme::TipState(), "%s: %s",
                    Lang::T(Lang::Str::SoulLabel), Lang::T(kSoulNames[idx]));
            }
        }

        // flavour/effect description (artifacts, uniques) — books excluded,
        // their DESC is the whole book text
        if (!a_obj->Is(RE::FormType::Book)) {
            if (auto* desc = a_obj->As<RE::TESDescription>()) {
                RE::BSString out;
                desc->GetDescription(out, a_obj->As<RE::TESForm>());
                if (out.size() > 0 && out.c_str() && *out.c_str()) {
                    // ★★THE SAME TIDYING THE EFFECT LINES GET, and it was
                    // missing here. GetDescription resolves the magnitude but
                    // leaves it WRAPPED -- the Gauldur Amulet came out reading
                    // "by <30> points", brackets and all, in a screenshot.
                    // A description and an effect line are the same kind of
                    // sentence from the same records; only one of them was
                    // being finished.
                    // ★Survival blocks too: a description written for a mode
                    // that is switched off must not be half-printed.
                    std::string line = out.c_str();
                    StripSurvivalBlocks(line, SurvivalModeOn());
                    UnwrapNumericTags(line);
                    TrimInPlace(line);
                    if (!line.empty()) {
                        ImGui::PushTextWrapPos(300.0f * Theme::Scale());
                        ImGui::TextColored(Theme::TipBody(), "%s", line.c_str());
                        ImGui::PopTextWrapPos();
                    }
                }
            }
        }

        // weight is meaningless under the space system (W1) — value only.
        // GI43: THIS unit's value (temper folded in, vanilla parity) -- the
        // scoped list is already resolved above; kAny falls back to the base.
        // ★Not for coins: the "1,000G" line above IS the value, and the coin
        // forms carry 0 in the record (they are icons for a ledger, not goods),
        // so this line could only ever add a contradictory second number.
        if (a_coinValue < 0) {
            ImGui::TextColored(Theme::TipHead(), "%s %d",
                Lang::T(Lang::Str::Value), UnitValueWith(a_obj, scoped));
        }

        // Phase 4: barter price (buy on the merchant side, sell on the player
        // side) — crimson when the payer can't afford it (design pass E)
        if (a_price >= 0) {
            ImGui::Separator();
            const bool broke = a_isBuy
                ? g_gold < a_price
                : (a_price > 0 && LootBarter::MerchantGold() < a_price);
            ImGui::TextColored(broke ? ImVec4(0.8f, 0.32f, 0.28f, 1.0f) : Theme::TipVal(),
                "%s: %d",
                Lang::T(a_isBuy ? Lang::Str::BuyLabel : Lang::Str::SellLabel), a_price);
        }
        // ── GI50 → GI64: what this tile answers to ──────────────────────────
        // All of it was undiscoverable, so it used to be printed right here.
        // It is now RESOLVED here and DRAWN by the screen-bottom prompt bar:
        // the right-click verb changes with the mode and the tile kind (a dozen
        // actions), splitting is a modifier nobody would try, compare had no
        // reason to ever be pressed -- but none of that is a property of the
        // item, and printing it beside every tooltip put the same six lines on
        // screen no matter what was hovered.
        {
            using Act = UIRoot::Act;
            const RE::FormID fid = a_obj->GetFormID();
            const bool isCoin  = GoldCoins::IsCoinForm(fid) && !isPouch;
            const auto mode    = LootBarter::CurrentMode();
            const bool quest  = a_tile.quest;
            const bool stolen = a_tile.stolen;

            ImGui::Separator();

            // Restrictions first — without these the only feedback is a fail
            // sound AFTER the action has already been refused.
            if (quest) {
                ImGui::TextColored(ImVec4(0.8f, 0.32f, 0.28f, 1.0f), "%s",
                    Lang::T(Lang::Str::BadgeQuest));
            }
            if (stolen) {
                ImGui::TextColored(ImVec4(0.8f, 0.32f, 0.28f, 1.0f), "%s",
                    Lang::T(Lang::Str::BadgeStolen));
            }
            if (mode == LootBarter::Mode::kBarter && !a_tile.partner &&
                !isCoin && !LootBarter::MerchantBuys(a_obj, stolen)) {
                ImGui::TextColored(ImVec4(0.8f, 0.32f, 0.28f, 1.0f), "%s",
                    Lang::T(Lang::Str::BadgeWontBuy));
            }

            // Line 1: the two primary actions. Branch order mirrors the click
            // handlers exactly — if one moves, this must move with it.
            bool hasVerb = true;
            Lang::Str verb = Lang::Str::ActEquip;
            if (a_tile.equipSlot) {
                verb = Lang::Str::ActUnequip;
            } else if (a_tile.parked) {
                verb = Lang::Str::ActRestore;
            } else if (a_tile.isBag) {
                // ★(1.5.0 audit) a SHELF bag's window book is LootBarter's,
                // not g_openBags -- asking the wrong book said "open" over a
                // bag whose window already was.
                const bool open = a_tile.partner
                    ? LootBarter::IsShelfBagOpen(a_tile.key)
                    : g_openBags.contains(std::string(a_tile.key));
                verb = open ? Lang::Str::ActCloseBag : Lang::Str::ActOpenBag;
            } else if (isPouch) {
                verb = Lang::Str::ActWithdraw;
            } else if (g_trashOpen && !a_tile.partner && !a_tile.equipSlot &&
                       !a_tile.parked) {
                // ★The bin is open, so this is what the button does now. It
                // sits in the same place here as in the click handler -- below
                // bag and pouch, above every mode -- because the two lists are
                // one decision written twice, and a bar that promises "sell"
                // while the click bins the item is worse than no bar at all.
                // ★partner / equipSlot never reach the handler this mirrors
                // (they have their own), so they must not claim the verb here.
                verb = Lang::Str::ActTrash;
            } else if (a_tile.partner) {
                verb = mode == LootBarter::Mode::kBarter     ? Lang::Str::ActBuy
                     : mode == LootBarter::Mode::kPickpocket ? Lang::Str::ActSteal
                                                             : Lang::Str::ActTakeIt;
            } else if (isCoin) {
                // With a pouch on you, a coin tile has a verb at last: bank it.
                // Without one there is still nothing a click can do, and the
                // bar goes back to saying so rather than promising a deposit
                // that would be refused.
                if (GoldCoins::PouchHeld()) verb = Lang::Str::ActDeposit;
                else                        hasVerb = false;
            } else if (LootBarter::IsLootMode(mode)) {
                verb = Lang::Str::ActStoreIn;
            } else if (mode == LootBarter::Mode::kPickpocket) {
                verb = Lang::Str::ActPlant;
            } else if (mode == LootBarter::Mode::kBarter) {
                verb = Lang::Str::ActSell;
            } else if (auto* bk = a_obj->As<RE::TESObjectBOOK>()) {
                verb = bk->TeachesSpell() ? Lang::Str::ActLearn
                     : bk->IsRead()       ? Lang::Str::ActReread
                                          : Lang::Str::ActRead;
            } else if (a_obj->Is(RE::FormType::AlchemyItem) ||
                       a_obj->Is(RE::FormType::Ingredient)) {
                verb = Lang::Str::ActUse;   // drunk / eaten, not worn
            } else if (!(a_obj->Is(RE::FormType::Weapon) ||
                         a_obj->Is(RE::FormType::Armor) ||
                         a_obj->Is(RE::FormType::Ammo) ||
                         a_obj->Is(RE::FormType::Light) ||
                         a_obj->Is(RE::FormType::Scroll))) {
                // ★Misc, keys, soul gems: nothing here is WORN, so the bar
                // promising "equip" was wrong on every one of them. The click
                // now reaches the engine (Equip::UseItem), and a mod is free to
                // make any of these do something — so offer the word vanilla
                // uses and let the item answer.
                verb = Lang::Str::ActUse;
            }

            // ★GI64: nothing is PRINTED here any more. The verb is still worked
            // out, because the prompt bar needs it -- but the bar is where it is
            // shown. Keeping a copy in the tooltip meant "RMB equip" appeared
            // twice on screen at once, once beside the item and once along the
            // bottom, which is exactly the duplication the bar was built to end.
            //
            // The tooltip is now purely what the item IS; every key that does
            // something lives in one place.

            // ★GI63: EVERY key line left this tooltip for the bottom bar. What
            // remains here is only what differs per item -- name, effects,
            // poison, temper, weight, price -- and those lines now start right
            // under the name instead of below two rows of keys that read the
            // same on every tile in the game.
            //
            // The conditions still have to be computed HERE, because they are
            // things only the tooltip's context knows: a quest item cannot be
            // dropped, a coin cannot be starred, the doll and the partner board
            // do not handle those keys at all.
            const bool canSplit = !a_tile.equipSlot &&
                (a_count > 1 || isPouch || (isCoin && a_coinValue > 1));
            const bool canCompare = a_obj->Is(RE::FormType::Weapon) ||
                                    a_obj->Is(RE::FormType::Armor);
            const bool sideBoard = a_tile.partner || a_tile.equipSlot;
            // ★T RECHARGES, and until now the only way to find that out was to
            // read the changelog. The same test OpenRecharge runs, minus the
            // sound: an enchanted WEAPON (armour carries no charge) that is not
            // already full. Never on the partner's side -- their sword is not
            // ours to feed a soul gem to.
            float rcCur = 0.0f, rcMax = 0.0f;
            const bool canRecharge =
                !a_tile.partner && a_obj->Is(RE::FormType::Weapon) &&
                UnitCharge(a_obj, scoped, a_tile.equipSlot, a_hand, rcCur, rcMax) &&
                (rcMax - rcCur) >= 0.5f;
            // ★(1.5.0) the shelf USE MODE hint: a container's book reads
            // (a tome learns) in place on Shift+right-click -- the one verb
            // of this board that no click could discover.
            const bool shelfUse =
                a_tile.partner && LootBarter::IsLootMode(mode) &&
                a_obj->As<RE::TESObjectBOOK>() != nullptr;
            const Lang::Str useVerb =
                shelfUse && a_obj->As<RE::TESObjectBOOK>()->TeachesSpell()
                    ? Lang::Str::ActLearn
                    : Lang::Str::ActRead;
            g_hoverPrompt = { ImGui::GetFrameCount(), canSplit, canCompare,
                              !sideBoard && !quest,                    // canDrop
                              // ★the doll and the drawer star things now too;
                              // only a coin or the partner's shelf cannot.
                              !a_tile.partner && !isCoin && !isPouch,  // canFav
                              hasVerb, verb, canRecharge,
                              shelfUse, useVerb };
        }
        const ImVec2 tipPos = ImGui::GetWindowPos();
        const ImVec2 tipSize = ImGui::GetWindowSize();
        ImGui::EndTooltip();
        Theme::PopTipStyle();

        // SHIFT compare: the equipped counterpart's card beside the tooltip.
        // Drawn on the FOREGROUND draw list — always above every window (a
        // plain window sank behind the partner window, and a second
        // BeginTooltip APPENDS to the same tooltip in this ImGui version,
        // which stretched the main box instead of making a card). Flips to
        // the LEFT of the tooltip when the right side would leave the screen.
        if (cmpObj) {
            const char* en = cmpObj->GetName();
            if (!en || !*en) en = "?";
            char statBuf[64];
            std::snprintf(statBuf, sizeof(statBuf), "%s %d",
                Lang::T(cmpIsWeap ? Lang::Str::Damage : Lang::Str::Armor), cmpVal);
            char valBuf[64];
            std::snprintf(valBuf, sizeof(valBuf), "%s %d",
                Lang::T(Lang::Str::Value), cmpObj->GetGoldValue());

            // ★★THE CARD IS A SECOND TOOLTIP, so it wears the tooltip's chrome
            // and the tooltip's palette — not the skin's. It used to paint
            // sk.winBg / sk.acc / sk.ink, which put a parchment panel with
            // skin-coloured ink flush against the dark tooltip it belongs to,
            // on every skin. The padding came from the WINDOW style for the
            // same reason, so even the text inset disagreed.
            const ImVec2 pad = Theme::TipPadding();
            const float lh = ImGui::GetTextLineHeightWithSpacing();
            const float cardW = (std::max)({
                ImGui::CalcTextSize(Lang::T(Lang::Str::EquippedLabel)).x,
                ImGui::CalcTextSize(en).x,
                ImGui::CalcTextSize(statBuf).x,
                ImGui::CalcTextSize(valBuf).x }) + pad.x * 2.0f;
            const float cardH = 4.0f * lh + pad.y * 2.0f;

            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            float cx = tipPos.x + tipSize.x + 6.0f;
            if (cx + cardW > disp.x) cx = tipPos.x - cardW - 6.0f;   // flip left
            if (cx < 0.0f) cx = 0.0f;
            float cy = tipPos.y;
            if (cy + cardH > disp.y) cy = (std::max)(0.0f, disp.y - cardH);

            auto* fdl = ImGui::GetForegroundDrawList();
            const ImVec2 c0(cx, cy);
            const ImVec2 c1(cx + cardW, cy + cardH);
            const float rnd = Theme::TipRounding();
            fdl->AddRectFilled(c0, c1, Theme::TipBg(), rnd);
            fdl->AddRect(c0, c1, Theme::TipBorder(), rnd, 0, 1.0f);
            float ty = cy + pad.y;
            auto line = [&](const char* a_txt, const ImVec4& a_col) {
                fdl->AddText(ImVec2(cx + pad.x, ty), ImGui::GetColorU32(a_col), a_txt);
                ty += lh;
            };
            // ★Same roles the tooltip uses for the same kinds of fact: a muted
            // caption, the name in the value colour, the stat as running text,
            // the price as a sub-line.
            line(Lang::T(Lang::Str::EquippedLabel), Theme::TipSub());
            line(en, Theme::TipVal());
            line(statBuf, Theme::TipBody());
            line(valBuf, Theme::TipSub());
        }
    }

    void Draw()
    {
        RotateHeldItem();   // GI62: A / D, before any grid reads the footprint
        if (g_views.empty()) return;
        const float gridW = g_views[0].cols * CellPx();
        // exact width; overflow rows (scripted adds) still wheel-scroll, the
        // bar itself is hidden so it never eats into the right margin.
        // Height is PINNED to the hard board: a fill-height child let the
        // overflow rows spill OVER the GOLD bar strip below (user-reported) —
        // now they clip + scroll inside instead.
        const ImVec2 clipTop = ImGui::GetCursorScreenPos();   // grid origin
        // ★GI80: EXACTLY the cells — no slack. The +1 that used to be here is
        // where the overflow bleed came from: the child clips content to its
        // own rect, so one extra pixel of height is one pixel in which a
        // scrolled sprite may legally draw past the last cell row. The border
        // rings were then added to paint over that sliver, which made the
        // frame load-bearing: move it for looks and the leak comes straight
        // back (it just did). The pixel cannot leak if it does not exist, and
        // the frame is free to sit wherever it looks right — it draws on its
        // own clip now (GI79) and no longer needs room inside the child.
        const float boardH = kMinRows * CellPx();
        ImGui::BeginChild("fablerim_grid", ImVec2(gridW, boardH), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar);
        // ROOT CAUSE of the "items spill past the frame" saga: the grid's
        // draw commands were NOT clipped to this child (overflow rows — and,
        // once wheel-scrolled, even the top rows — rendered right through
        // the window). Clip every grid pass to the 10x14 board explicitly;
        // a small margin keeps the rarity glow's soft bleed from cutting
        // square at the edges.
        {
            auto* cdl = ImGui::GetWindowDrawList();
            const float m = 10.0f * Theme::Scale();
            cdl->PushClipRect(ImVec2(clipTop.x - m, clipTop.y - m),
                ImVec2(clipTop.x + gridW + m, clipTop.y + boardH + m), true);
            DrawGridView(g_views[0], 0);
            cdl->PopClipRect();

            // Border ON TOP of the item sprites (they z-cover the pass-1
            // chrome outline): fixed to the 10x14 board, so it does not move
            // when the overflow rows scroll. Must be on THIS child's list
            // AFTER the passes: the child renders above its parent, so a
            // parent-list border sat under the items (user-reported). Other
            // windows / the carried cursor icon still draw above.
            //
            // ★GI79: its own clip, and NOT intersected with the current one.
            // The board rings live OUTSIDE the child's rect now, and the
            // child's clip is exactly that rect — intersecting would silently
            // shave them off. (That is what had been happening to the outward
            // ring all along: it was drawn at -1 and never survived the clip,
            // which is why the border measured 1px and not the 2px the code
            // reads like.) Content keeps the strict clip above; only these two
            // lines are allowed out.
            const float b = 8.0f * Theme::Scale();
            cdl->PushClipRect(ImVec2(clipTop.x - b, clipTop.y - b),
                ImVec2(clipTop.x + gridW + b, clipTop.y + boardH + b), false);
            // 1px OUTWARD of the cells (user request): the frame stands just
            // off the board instead of sitting on the outermost cell edge.
            // Purely a look — the leak it used to cover is gone (GI80), so
            // this offset is free to be whatever reads best.
            constexpr float kOff = 1.0f;
            // 0.36 = what two stacked 20% layers composited to at scroll 0
            // (1 - 0.8*0.8), the weight the border was judged right at
            // ★Skipped on a tiles-on-panel board — see the note at the
            // per-view edge above. It also never agreed with itself here: the
            // rings are drawn inside the scroll clip, so each side was cut by
            // a different amount and the four edges came out unequal.
            if (!Theme::S().engravedCells) {
                cdl->AddRect(ImVec2(clipTop.x - kOff, clipTop.y - kOff),
                    ImVec2(clipTop.x + gridW + kOff, clipTop.y + boardH + kOff),
                    Theme::Acc(0.36f));
                // second ring 1px further out — 2px of frame, growing outward
                cdl->AddRect(ImVec2(clipTop.x - kOff - 1.0f, clipTop.y - kOff - 1.0f),
                    ImVec2(clipTop.x + gridW + kOff + 1.0f, clipTop.y + boardH + kOff + 1.0f),
                    Theme::Acc(0.20f));
            }
            cdl->PopClipRect();
        }
        ImGui::EndChild();
    }

    // G2: coin-pouch withdraw window — same construction as the settings /
    // loadout confirm windows (fixed size + TitleBar + centred content).
    bool ClosePouch()
    {
        if (!g_pouchOpen) return false;
        g_pouchOpen = false;
        return true;
    }

    void DrawPouchWindow()
    {
        if (!g_pouchOpen) return;
        if (!GoldCoins::Ready()) { g_pouchOpen = false; return; }

        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;
        const int stored = GoldCoins::PouchStoredOf(g_pouchTile);
        if (g_pouchSlider > stored) g_pouchSlider = stored;

        char line[64];
        std::snprintf(line, sizeof(line), "%s: %d / %dG",
            Lang::T(Lang::Str::StoredLabel), stored,
            GoldCoins::PouchCapOfKey(g_pouchTile));
        const float sliderW = 220.0f * S;
        const float contentW = (std::max)({ btnRow, sliderW,
            ImGui::CalcTextSize(line).x });
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float topPad = Theme::TitleTopPad();   // paid for in the height below
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + ImGui::GetFrameHeight() + 6.0f * S +
                2.0f * sp + ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        wm->ApplyNext("pouch",
            ImVec2((disp.x - size.x) * 0.5f, (disp.y - size.y) * 0.5f), size,
            WinManager::Anchor::kTopLeft, topPad);
        ImGui::Begin("##grid_pouch", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        auto* pouch = GoldCoins::PouchForm();
        wm->TitleBar("pouch", pouch && pouch->GetName() ? pouch->GetName() : "?",
            0.0f, true);

        if (!ImGui::IsWindowAppearing() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered()) {
            g_pouchOpen = false;
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(line).x);
        ImGui::TextColored(sk.ink, "%s", line);
        center(sliderW);
        const int drawBefore = g_pouchSlider;
        if (Theme::ChromeSliderInt("##pouchdraw", &g_pouchSlider, 0, stored, sliderW, "%dG") &&
            g_pouchSlider != drawBefore) {
            static double s_lastTick = 0.0;
            const double now = ImGui::GetTime();
            if (now - s_lastTick > 0.06) {
                s_lastTick = now;
                if (g_pouchSlider > drawBefore) Sfx::SelectOn();
                else                            Sfx::SelectOff();
            }
        }
        // GI51: LEFT/RIGHT nudge by 1G (hold repeats; A/D arrive as arrows).
        // GI52: keys stand down while a text field owns the keyboard.
        const bool typing = ImGui::GetIO().WantTextInput;
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && g_pouchSlider > 0) {
            --g_pouchSlider;
            Sfx::SelectOff();
        }
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && g_pouchSlider < stored) {
            ++g_pouchSlider;
            Sfx::SelectOn();
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        const bool can = g_pouchSlider > 0;
        // GI51: Enter/Space confirm (ESC already closes via CloseTopWindow)
        const bool keyOk = !typing &&
                           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_Space, false));
        ImGui::BeginDisabled(!can);
        if (Sfx::Button(Lang::T(Lang::Str::Withdraw), ImVec2(btnW, 0)) || (can && keyOk)) {
            // the withdrawn amount rides the CURSOR as a pinned purse (same
            // flow as a stack split) instead of dropping straight into the
            // inventory. Carry caps at one purse (kCoinCap); any excess of a
            // larger withdrawal lands in the inventory as walking gold.
            const int v = g_pouchSlider;
            GoldCoins::WithdrawFrom(g_pouchTile, v, false);   // pickup sound plays instead
            const int carry = (std::min)(v, GoldCoins::kCoinCap);
            if (auto* cform = GoldCoins::CoinForTier(GoldCoins::BandTier(carry))) {
                PickupPartial(cform, carry, {}, 0);   // the purse rides the cursor
            }
            // ★S-G: whatever exceeds one purse becomes board tiles at once --
            // the mirror that used to materialise "walking" gold is gone
            if (v > carry) CoinIncome(v - carry);
            g_pouchOpen = false;             // window closes; the purse rides
            g_pouchSlider = 0;
            g_pouchTile.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 8.0f * S);
        if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
            g_pouchOpen = false;
        }
        ImGui::End();
    }

    // ---- (1.3.1) soul-gem recharge (hover + T) ----------------------------
    namespace
    {
        struct RechargeUI
        {
            bool          open = false;
            RE::FormID    obj = 0;    // the enchanted weapon's form
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            bool          worn = false;
            int           hand = 0;   // 1 = right (ExtraWorn), 2 = left
        };
        RechargeUI g_rechargeUI;

        // a clicked row, applied on the Tick (engine mutations off the render
        // pass, same rule as every other transfer)
        struct RechargePick
        {
            bool           want = false;
            RE::FormID     gem = 0;
            RE::SOUL_LEVEL soul = RE::SOUL_LEVEL::kNone;
            bool           fromBase = false;   // prefilled form, no ExtraSoul
        };
        RechargePick g_rechargePick;

        // charge points one soul restores -- the engine's own GMSTs, vanilla
        // table as fallback when a setting is missing
        [[nodiscard]] int SoulChargePoints(RE::SOUL_LEVEL a_lv)
        {
            static const char* kNames[5] = {
                "iSoulLevelValuePetty", "iSoulLevelValueLesser",
                "iSoulLevelValueCommon", "iSoulLevelValueGreater",
                "iSoulLevelValueGrand"
            };
            static constexpr int kFallback[5] = { 250, 500, 1000, 2000, 3000 };
            const int i = static_cast<int>(a_lv) - 1;
            if (i < 0 || i > 4) return 0;
            if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
                if (auto* s = gs->GetSetting(kNames[i]); s && s->GetInteger() > 0) {
                    return s->GetInteger();
                }
            }
            return kFallback[i];
        }

        // the unit's current / max charge. false = not a chargeable unit.
        // ★Worn charge lives in the HAND's actor value while equipped (the
        // engine drains it there and writes ExtraCharge back on unequip), so
        // a worn unit is read from the AV, never from the list.
        bool UnitCharge(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_xl,
                        bool a_worn, int a_hand, float& a_cur, float& a_max)
        {
            auto* enchBase = a_obj ? a_obj->As<RE::TESEnchantableForm>() : nullptr;
            const RE::ExtraEnchantment* xEnch =
                a_xl ? a_xl->GetByType<RE::ExtraEnchantment>() : nullptr;
            if (xEnch && xEnch->enchantment && xEnch->charge != 0) {
                a_max = static_cast<float>(xEnch->charge);
            } else if (enchBase && enchBase->formEnchanting &&
                       enchBase->amountofEnchantment != 0) {
                a_max = static_cast<float>(enchBase->amountofEnchantment);
            } else {
                return false;   // not enchanted (or a cost-free enchant)
            }
            if (a_worn) {
                auto* p = RE::PlayerCharacter::GetSingleton();
                if (!p) return false;
                a_cur = p->AsActorValueOwner()->GetActorValue(
                    a_hand == 2 ? RE::ActorValue::kLeftItemCharge
                                : RE::ActorValue::kRightItemCharge);
            } else if (const auto* xc =
                           a_xl ? a_xl->GetByType<RE::ExtraCharge>() : nullptr) {
                a_cur = xc->charge;
            } else {
                a_cur = a_max;   // no ExtraCharge = never fired = full
            }
            return true;
        }

        // the target unit's list, resolved FRESH (never cached across frames)
        [[nodiscard]] RE::ExtraDataList* RechargeUnitList(RE::PlayerCharacter* a_p,
                                                          RE::TESBoundObject* a_obj)
        {
            auto* entry = LiveEntry(a_p, a_obj);
            if (!entry) return nullptr;
            if (g_rechargeUI.worn) {
                return WornExtraMatching(entry, g_rechargeUI.uid, g_rechargeUI.sig,
                                         g_rechargeUI.hand);
            }
            return ExtraForPoolImpl(entry, g_rechargeUI.uid, g_rechargeUI.sig);
        }

        struct GemRow
        {
            RE::TESBoundObject* obj = nullptr;
            RE::SOUL_LEVEL      soul = RE::SOUL_LEVEL::kNone;
            int                 count = 0;
            bool                fromBase = false;
        };

        [[nodiscard]] std::vector<GemRow> CollectFilledGems()
        {
            std::vector<GemRow> out;
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return out;
            auto inv = p->GetInventory([](RE::TESBoundObject& a_o) {
                return a_o.Is(RE::FormType::SoulGem);
            });
            for (auto& [obj, data] : inv) {
                const int total = data.first;
                if (total <= 0) continue;
                auto* gem = obj->As<RE::TESSoulGem>();
                if (!gem) continue;
                int listed = 0;
                if (auto* entry = data.second.get(); entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (!xl) continue;
                        const int n = (std::max)(1, static_cast<int>(xl->GetCount()));
                        listed += n;
                        const auto lv = xl->GetSoulLevel();
                        if (lv == RE::SOUL_LEVEL::kNone) continue;
                        const auto it = std::find_if(out.begin(), out.end(),
                            [&](const GemRow& r) {
                                return r.obj == obj && r.soul == lv && !r.fromBase;
                            });
                        if (it != out.end()) it->count += n;
                        else out.push_back({ obj, lv, n, false });
                    }
                }
                // prefilled forms (soul on the record, no list)
                const int plain = total - listed;
                if (plain > 0 && gem->GetContainedSoul() != RE::SOUL_LEVEL::kNone) {
                    out.push_back({ obj, gem->GetContainedSoul(), plain, true });
                }
            }
            // smallest soul first -- the thrifty pick sits on top
            std::sort(out.begin(), out.end(), [](const GemRow& a_x, const GemRow& a_y) {
                return a_x.soul < a_y.soul;
            });
            return out;
        }
    }

    void OpenRecharge(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                      std::uint16_t a_sig, bool a_worn, int a_hand)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        // vanilla scope: enchanted WEAPONS carry a charge; armor does not
        if (!p || !a_obj || !a_obj->Is(RE::FormType::Weapon)) return;
        g_rechargeUI = { false, a_obj->GetFormID(), a_uid, a_sig, a_worn, a_hand };
        auto* xl = RechargeUnitList(p, a_obj);
        float cur = 0.0f, max = 0.0f;
        if (!UnitCharge(a_obj, xl, a_worn, a_hand, cur, max)) return;   // not enchanted: T is silent
        if (max - cur < 0.5f) {
            Sfx::FailNote(Lang::T(Lang::Str::RechargeFull));
            return;
        }
        g_rechargeUI.open = true;
        g_rechargePick = {};
        Sfx::SelectOn();
    }

    bool IsRechargeOpen() { return g_rechargeUI.open; }

    bool CloseRecharge()
    {
        if (!g_rechargeUI.open) return false;
        g_rechargeUI.open = false;
        return true;
    }

    void DrawRechargeWindow()
    {
        if (!g_rechargeUI.open) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(g_rechargeUI.obj);
        if (!p || !obj) { g_rechargeUI.open = false; return; }
        auto* xl = RechargeUnitList(p, obj);
        float cur = 0.0f, max = 0.0f;
        if (!UnitCharge(obj, xl, g_rechargeUI.worn, g_rechargeUI.hand, cur, max)) {
            g_rechargeUI.open = false;   // the unit left (sold, dropped)
            return;
        }
        const auto rows = CollectFilledGems();

        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float rowW = 280.0f * S;

        char line[64];
        std::snprintf(line, sizeof(line), "%s: %s / %s",
            Lang::T(Lang::Str::ChargeLabel),
            Commas(static_cast<int>(cur + 0.5f)).c_str(),
            Commas(static_cast<int>(max + 0.5f)).c_str());

        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float topPad = Theme::TitleTopPad();
        const int   nRows = rows.empty() ? 1 : static_cast<int>(rows.size());
        const ImVec2 size(
            rowW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + sp +
                nRows * (ImGui::GetFrameHeight() + sp) + 18.0f * S + 2.0f * insY);
        wm->ApplyNext("recharge",
            ImVec2((disp.x - size.x) * 0.5f, (disp.y - size.y) * 0.5f), size,
            WinManager::Anchor::kTopLeft, topPad);
        ImGui::Begin("##grid_recharge", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        wm->TitleBar("recharge",
            obj->GetName() && *obj->GetName() ? obj->GetName() : "?", 0.0f, true);

        if (!ImGui::IsWindowAppearing() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered()) {
            g_rechargeUI.open = false;
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(line).x);
        ImGui::TextColored(sk.ink, "%s", line);

        if (rows.empty()) {
            const char* none = Lang::T(Lang::Str::RechargeNoGems);
            center(ImGui::CalcTextSize(none).x);
            ImGui::TextColored(sk.inkDim, "%s", none);
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            const int pts = SoulChargePoints(r.soul);
            char label[128];
            std::snprintf(label, sizeof(label), "%s x%d   +%s##rc%zu",
                r.obj->GetName() ? r.obj->GetName() : "?", r.count,
                Commas(pts).c_str(), i);
            center(rowW);
            if (Sfx::Button(label, ImVec2(rowW, 0))) {
                g_rechargePick = { true, r.obj->GetFormID(), r.soul, r.fromBase };
                g_rechargeUI.open = false;   // identity fields stay for the apply
            }
        }
        ImGui::End();
    }

    void ProcessRecharge()
    {
        if (!g_rechargePick.want) return;
        const auto pick = g_rechargePick;
        g_rechargePick = {};
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(g_rechargeUI.obj);
        auto* gemObj = RE::TESForm::LookupByID<RE::TESBoundObject>(pick.gem);
        if (!p || !obj || !gemObj) return;

        auto* xl = RechargeUnitList(p, obj);
        float cur = 0.0f, max = 0.0f;
        if (!UnitCharge(obj, xl, g_rechargeUI.worn, g_rechargeUI.hand, cur, max)) {
            return;
        }
        float amount = static_cast<float>(SoulChargePoints(pick.soul));
        // Soul Squeezer et al. -- the engine's own perk entry point (add ops)
        RE::BGSEntryPoint::HandleEntryPoint(
            RE::BGSEntryPoint::ENTRY_POINT::kModSoulGemRecharge, p, gemObj, &amount);
        const float next = (std::min)(max, cur + amount);
        if (next - cur < 0.5f) {
            Sfx::FailNote(Lang::T(Lang::Str::RechargeFull));
            return;
        }

        // write BOTH homes: the list (survives unequip / the unworn case) and,
        // for a worn unit, the hand's AV -- the drain while equipped is DAMAGE
        // on that AV, so restoring the damage is the exact inverse (caps at
        // the base value = full charge).
        if (auto* xc = xl ? xl->GetByType<RE::ExtraCharge>() : nullptr) {
            xc->charge = next;
        }
        if (g_rechargeUI.worn) {
            p->AsActorValueOwner()->ModActorValue(
                RE::ACTOR_VALUE_MODIFIER::kDamage,
                g_rechargeUI.hand == 2 ? RE::ActorValue::kLeftItemCharge
                                       : RE::ActorValue::kRightItemCharge,
                next - cur);
        }

        // consume the gem: reusable (Azura's Star) empties, the rest shatter
        // (vanilla behaviour -- no empty gem comes back)
        const auto* kwf = gemObj->As<RE::BGSKeywordForm>();
        const bool reusable = kwf && kwf->HasKeywordString("ReusableSoulGem");
        auto ginv = p->GetInventory([&](RE::TESBoundObject& a_o) {
            return &a_o == gemObj;
        });
        RE::InventoryEntryData* gentry = nullptr;
        for (auto& [o2, d2] : ginv) {
            gentry = d2.second.get();
            break;
        }
        RE::ExtraDataList* gxl = nullptr;
        if (!pick.fromBase && gentry && gentry->extraLists) {
            for (auto* x : *gentry->extraLists) {
                if (x && x->GetSoulLevel() == pick.soul) { gxl = x; break; }
            }
        }
        if (reusable) {
            if (gxl) {
                if (auto* xs = gxl->GetByType<RE::ExtraSoul>()) {
                    xs->soul = RE::SOUL_LEVEL::kNone;   // emptied, gem kept
                }
            } else if (auto* sg = gemObj->As<RE::TESSoulGem>();
                       sg && sg->linkedSoulGem) {
                // prefilled reusable (modded): swap to its linked empty form
                p->RemoveItem(gemObj, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                p->AddObjectToContainer(sg->linkedSoulGem, nullptr, 1, nullptr);
            }
        } else {
            p->RemoveItem(gemObj, 1, RE::ITEM_REMOVE_REASON::kRemove, gxl, nullptr);
        }

        // ★XP: vanilla grants Enchanting XP by SOUL SIZE, regardless of the
        // charge actually restored (UESP). What it grants PER soul was the
        // open question, and 1.3.x answered it with an invented 5% of the
        // soul's charge points -- 150 for a grand gem, which a user reported
        // as levelling Enchanting far too fast (feedback ①). The comment there
        // said "tune on feedback", so this is that.
        //
        // ★The number is no longer ours. Every skill carries the engine's own
        // use formula in its AVSK record -- xp = useMult * magnitude +
        // offsetMult, the shape Bethesda has used since Oblivion -- and the
        // magnitude for a recharge is the soul's charge points. Reading it at
        // runtime means we grant what the GAME says a recharge is worth, and a
        // rebalance mod that edits the Enchanting AVIF (Ordinator and friends
        // all do) moves our grant with it instead of fighting it.
        //
        // The old approximation survives only as the fallback for a record
        // with no AVSK block at all, where there is nothing to read.
        {
            // ★★What AddSkillExperience actually wants was the whole question,
            // and the ESM answered it: Enchanting's AVSK carries useMult 900,
            // offsetMult 0 (read straight out of Skyrim.esm; Alchemy is 0.75,
            // Smithing 160). A skill "use" is useMult * MAGNITUDE, so the
            // number this call takes is a magnitude on that scale -- NOT a
            // finished XP figure. 1.3.x passed 12.5 for a petty soul and 150
            // for a grand one, which on that scale is thousands of points:
            // one recharge of a barely-drained weapon levelled Enchanting on
            // the spot (measured in game, 12.50 -> a level).
            //
            // So: pass a magnitude. One full ENCHANTMENT is the anchor the
            // record is calibrated for (magnitude 1.0 = 900 xp). A recharge is
            // a fraction of that, scaled by the soul spent -- a grand soul is
            // a tenth of an enchantment, a petty soul a twelfth of that again.
            // The ratio is ours; the scale it rides on is the game's, so a
            // rebalance mod that edits the Enchanting AVIF moves this with it.
            constexpr float kRechargeShareOfAnEnchant = 0.10f;
            const float grand = static_cast<float>(
                (std::max)(1, SoulChargePoints(RE::SOUL_LEVEL::kGrand)));
            const float soul = static_cast<float>(SoulChargePoints(pick.soul));
            const float mag = kRechargeShareOfAnEnchant *
                              std::clamp(soul / grand, 0.0f, 1.0f);

            // ★Before/after, because this is the one number in the plugin we
            // cannot read off a record: whether the engine scales what we hand
            // it. The delta says so outright, and one recharge settles it.
            const auto readXp = [&]() -> float {
                auto* sk = p->GetInfoRuntimeData().skills;
                return (sk && sk->data)
                    ? sk->data->skills[RE::PlayerCharacter::PlayerSkills::Data::
                                           Skills::kEnchanting].xp
                    : -1.0f;
            };
            const float before = readXp();
            p->AddSkillExperience(RE::ActorValue::kEnchanting, mag);
            const float after = readXp();
            // ★ANSWERED, in game: mag 0.0083 produced a delta of 7.50, which
            // is exactly useMult 900 applied to what we passed. The engine
            // DOES scale it -- so 1.3.x's 12.5 was eleven thousand points,
            // some thirty levels for one petty gem, and the whole of report ①.
            SKSE::log::info("[RECHARGE] enchanting +{:.1f} xp (soul {:.0f}/{:.0f})",
                after - before, soul, grand);
            (void)before;
        }
        p->PlayPickUpSound(gemObj, false, false);
        SKSE::log::info("[RECHARGE] '{}' {:.0f} -> {:.0f} (+{:.0f}, soul {})",
            obj->GetName() ? obj->GetName() : "?", cur, next, next - cur,
            static_cast<int>(pick.soul));
        RequestRebuild();
        MarkCapacityDirty();
    }

    void DrawBagWindows()
    {
        auto* wm = WinManager::GetSingleton();

        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        for (int vi = 1; vi < static_cast<int>(g_views.size()); ++vi) {
            auto& v = g_views[vi];
            if (!v.open) continue;   // closed bag: holds items, draws no window
            // symmetric 12px margins around the bag grid (scale-aware) +
            // 2x frame inset for tornFrame skins (breathing room)
            // ★+ the skin's title clearance, and the height pays for it (see
            // Theme::TitleTopPad). A bag docks to the main window's edge, so
            // its title has to sit on the same line as the one beside it.
            const float topPad = Theme::TitleTopPad();
            const ImVec2 size(v.cols * CellPx() + 2.0f * Theme::PadX() * S +
                                  2.0f * Theme::FrameInsetX(),
                              v.rows * CellPx() + 54.0f * S + 2.0f * Theme::FrameInsetY());

            // default: flow to the right of the main window (E5).
            // ★Wrapped cascade: one straight diagonal walked the 18th bag
            // clean off the bottom of the display — the ApplyNext clamp keeps
            // it reachable now, but a fresh spawn should not need rescuing.
            ImVec2 defPos(200.0f + vi * 60.0f, 200.0f);
            if (auto* mw = wm->Find("main")) {
                const int step = (vi - 1) % 8;
                const int band = (vi - 1) / 8;
                defPos = ImVec2(mw->pos.x + mw->size.x + 8.0f + band * 48.0f,
                                mw->pos.y + step * 60.0f);
            }
            wm->ApplyNext(v.bagKey, defPos, size, WinManager::Anchor::kTopLeft, topPad);

            // typed bag: the COLLECT control sits in the titlebar, so reserve
            // its width before the title is laid out (same contract the main
            // window's EDIT / SETTINGS use)
            const bool typedBag = !v.accept.empty();
            const char* colLbl = Lang::T(Lang::Str::BagCollect);
            const float colW = typedBag ? ImGui::CalcTextSize(colLbl).x + 14.0f * S : 0.0f;
            // ★The reserve has to cover the MARGIN as well as the button. It
            // was colW alone, so the drag strip ran under the button's right
            // edge and claimed ActiveId there first -- the very failure the
            // reserve exists to prevent, just narrow enough (6px) to pass for
            // a missed click. Widening the margin would have widened it too.
            const float colRes = typedBag ? colW + Theme::TopControlRightPad() : 0.0f;

            ImGui::Begin(("##bag_" + v.bagKey).c_str(), nullptr, kManagedWinFlags);
            wm->TitleBar(v.bagKey, v.bagName.c_str(), colRes);
            if (typedBag) {
                const ImVec2 keep = ImGui::GetCursorScreenPos();
                // ★On the TITLE's own line — the same centring the main
                // window's EDIT / SETTINGS use. The old anchor was the
                // content cursor (window padding), which sits well above the
                // title text, so COLLECT floated over the bag's name.
                // ★★HALF the frame inset, which is what that comment above
                // always claimed and what the code did not do. The inset is how
                // far the FRAME eats inward; the title clears half of it and
                // stays centred in its own bar (WinManager::TitleBar), and the
                // main window's EDIT / SETTINGS / x take the same half. Taking
                // the WHOLE of it put this button below the name it is aligned
                // to on 16 of the 19 skins -- 3px under a translucent SIMPLE,
                // 12px under a torn PARCHMENT, and invisible only on Sumi and
                // Fable, where the inset happens to be zero. The top pad is a
                // different quantity and is still taken whole: the caller paid
                // for it in the window's height (Theme::TitleTopPad).
                const float lineH = ImGui::GetTextLineHeight();
                const float btnH = lineH + 6.0f * S;
                const float textTop = WinManager::TitleTextY(v.bagKey, lineH);
                // ★Same right margin as the main window's x and the FIND box
                // (Theme::TopControlRightPad). This was the odd one out at 6px
                // -- with bags docked along the main window's edge, three
                // different margins on one vertical line were all visible at
                // once.
                ImGui::SetCursorScreenPos(ImVec2(
                    ImGui::GetWindowPos().x + size.x - colW - Theme::TopControlRightPad(),
                    textTop - (btnH - lineH) * 0.5f));   // button centres the label
                if (Sfx::Button(("##collect_" + v.bagKey).c_str(),
                                ImVec2(colW, btnH))) {
                    CollectIntoBag(v.bagKey, v.accept);
                }
                const bool hov = ImGui::IsItemHovered();
                auto* dl = ImGui::GetWindowDrawList();
                const ImVec2 bp = ImGui::GetItemRectMin();
                const ImVec2 bs = ImGui::GetItemRectSize();
                const ImVec2 ts = ImGui::CalcTextSize(colLbl);
                // ★★Val(), not sk.hi — the third call site to learn this (see
                // LootBarter's figure and the favourite mark). On a LIGHT panel
                // `hi` is a structural token, and worse: this button's own hover
                // fill is lifted TOWARD it (ButtonHovered = btnFace + (hi -
                // btnFace) * 0.28). Painting the label in `hi` therefore puts
                // 63,65,66 text on a 39,41,42 ground — the word disappears at
                // exactly the moment the cursor arrives, and only here, because
                // every other emphasised text already asks Val().
                dl->AddText(ImVec2(bp.x + (bs.x - ts.x) * 0.5f,
                                   bp.y + (bs.y - ts.y) * 0.5f),
                    hov ? Theme::Val() : ImGui::GetColorU32(sk.inkDim), colLbl);
                if (hov) {
                    // bottom bar, not a floating card under the cursor: the
                    // card landed on the grid the player is aiming at
                    char hint[192];
                    std::snprintf(hint, sizeof(hint), Lang::T(Lang::Str::BagCollectTip),
                        BagFilter::DisplayName(v.accept));
                    UIRoot::NoteHoverHint(hint);
                }
                ImGui::SetCursorScreenPos(keep);
            }
            // F2: dim crimson border marks the trash apart from real bags
            if (v.bagKey == kTrashKey) {
                auto* wdl = ImGui::GetWindowDrawList();
                const ImVec2 wp = ImGui::GetWindowPos();
                const ImVec2 we(wp.x + size.x, wp.y + size.y);
                wdl->PushClipRect(wp, we, false);
                wdl->AddRect(wp, we, IM_COL32(140, 40, 30, 150), Theme::S().rounding,
                    0, 2.0f);
                wdl->PopClipRect();
            }
            DrawGridView(v, vi);
            ImGui::End();
        }
    }

    namespace
    {
        // G4: pin a gold value onto a grid position, choosing the coin form by
        // the value's band (auto-repins to the right form when a merge crosses
        // a band). Returns the new tile key.
        std::string PlacePin(int a_value, int a_col, int a_row, const std::string& a_bag)
        {
            auto* f = GoldCoins::CoinForTier(GoldCoins::BandTier(a_value));
            if (!f) return {};
            const std::string key = NextTileKey(FormKey(f));
            SetCoinRecord(key, a_value);
            PlaceTile(key, a_col, a_row, a_bag, 1);
            return key;
        }

        // Phase 2: the ONE gold-on-gold merge (formerly two drifting copies —
        // fragment-onto-pin vs whole-tile-onto-tile). Both tiles are consumed,
        // up-to-cap lands as a pinned purse at the TARGET's cell (fallback:
        // the drop cell), the remainder keeps riding the cursor as a fresh
        // pin. Unpinning both first keeps the walking-gold ledger exact, so
        // sibling auto coins never reshuffle. a_held must be g_held's value.
        void MergeGoldInto(Held& a_held, const std::string& a_tgtKey, int a_tgtValue,
                           const LayoutEntry& a_fallbackPos)
        {
            const int combined = a_tgtValue + a_held.coinValue;
            const int placed = (std::min)(combined, GoldCoins::kCoinCap);
            const int leftover = combined - placed;
            LayoutEntry pos = a_fallbackPos;
            if (auto li = g_layout.find(a_tgtKey); li != g_layout.end()) pos = li->second;
            g_layout.erase(a_tgtKey);
            g_layout.erase(a_held.key);
            PlacePin(placed, pos.col, pos.row, pos.bag);
            if (g_sound) g_sound(a_held.obj, false);
            if (leftover > 0) {   // remainder keeps riding as a pin
                auto* lf = GoldCoins::CoinForTier(GoldCoins::BandTier(leftover));
                const std::string lk = NextTileKey(FormKey(lf));
                SetCoinRecord(lk, leftover);
                a_held.key = lk;
                a_held.obj = lf;
                a_held.coinValue = leftover;
                a_held.preSplit = true;   // fragment rules from here on
                a_held.mask = MaskOf(g_resolver ? g_resolver(lf) : GridDef{});
            } else {
                g_held.reset();
            }
        }
    }

    // ================= Phase 3: drop-target dispatch table =================
    // FinishFrame's 6~7-deep else-if drop state machine, re-expressed as
    // (held kind -> ordered route list). A route row = WHERE the drop landed
    // + a handler; the FIRST matching row whose handler returns true resolves
    // the drop, false falls through to the next row, and no consuming row =
    // keep carrying. Adding a new drop TARGET (e.g. the F2 trash window) is
    // one DropWhere case + one row per held kind that accepts it. Handler
    // bodies moved VERBATIM from the old chain — behaviour unchanged.
    namespace
    {
        // held icon rides the cursor above every window
        void DrawHeldCursorIcon(Held& a_held)
        {
            const auto& io = ImGui::GetIO();
            auto* fg = ImGui::GetForegroundDrawList();
            const float w = a_held.mask.w * CellPx();
            const float h = a_held.mask.h * CellPx();
            const ImVec2 a(io.MousePos.x - a_held.offX, io.MousePos.y - a_held.offY);
            RE::TESBoundObject* heldIconObj = a_held.obj;
            if (GoldCoins::IsPouch(a_held.obj->GetFormID())) {
                // ★(1.3.0) a pouch lifted OFF THE SHELF keeps its amount on
                // its reserved container slot, not in g_pouchStored -- asking
                // only the player table drew the empty band for the whole
                // carry (and the right icon snapped back on the drop).
                const int shelf = LootBarter::HeldShelfGold();
                const int amount =
                    shelf >= 0 ? shelf : GoldCoins::PouchStoredOf(a_held.key);
                if (auto* v = GoldCoins::PouchIconObjectFor(amount,
                        GoldCoins::PouchCapOfKey(a_held.key),
                        a_held.obj ? a_held.obj->GetFormID() : 0)) {
                    heldIconObj = v;
                }
            }
            auto* hc = IconCache::GetSingleton();
            const IconCache::Icon* heldIcon = hc->Get(heldIconObj);
            if (!heldIcon && heldIconObj != a_held.obj) {
                hc->QueueCapture(heldIconObj);
                heldIcon = hc->Get(a_held.obj);   // base icon until captured
            }
            // ★The carried sprite has to follow the SAME rule as the tile it
            // came from, both in what it draws and how big. Without this,
            // picking up a category-icon item put a bare rectangle on the
            // cursor — the item looked like it had vanished from the board
            // without arriving anywhere.
            bool heldFallback = false;
            Fallback::KeyXform heldX;
            if (!heldIcon) {
                hc->QueueCapture(heldIconObj);
                const auto fb = Fallback::GetDrawn(a_held.obj);
                heldIcon = fb.icon;
                heldX = fb.x;
                heldFallback = heldIcon != nullptr;
            }
            if (const auto* icon = heldIcon) {
                float dw, dh;
                const auto hdef = Grid::ResolveDef(a_held.obj);
                // same precedence as the tile it came from
                const float hSc = hdef.fscale != 1.0f ? hdef.fscale : heldX.scale;
                const float hRot = hdef.frot != 0.0f ? hdef.frot : heldX.rot;
                const float hOfs = hdef.fx != 0.0f ? hdef.fx : heldX.x;
                if (heldFallback) {
                    const float sc = (std::min)(w / static_cast<float>(icon->w),
                                                h / static_cast<float>(icon->h)) *
                                     0.85f * hSc;
                    dw = icon->w * sc;
                    dh = icon->h * sc;
                } else {
                    const float target = (std::max)(w, h) * 0.95f * a_held.defScale;
                    const float ms = static_cast<float>((std::max)(icon->w, icon->h));
                    dw = icon->w / ms * target;
                    dh = icon->h / ms * target;
                }
                // both styles, like the board tile -- a carried item must look
                // exactly like the tile it was lifted from
                const ImVec2 nudge = RotatedOffset(hOfs, hdef.fy, a_held.rot);
                // ★GI62e: the sprite must revolve about THE PIVOT, not about the
                // middle of its footprint. On a cell-pivot item those are two
                // different points, and spinning about the middle swings the
                // grip away from the cursor and back -- on screen the axis looks
                // like it slides from the haft into the blade and returns
                // (user-reported on an axe).
                //
                // The footprint centre sits at `v` relative to the cursor once
                // the turn has settled. Winding `v` BACK by however much of the
                // turn is still outstanding traces exactly the arc it travelled,
                // so the pivot cell stays nailed under the pointer the whole way.
                float vx = w * 0.5f - a_held.offX;
                float vy = h * 0.5f - a_held.offY;
                const float spin = a_held.rotDeg - a_held.rotAim;   // 0 once settled
                if (std::fabs(spin) > 0.01f) {
                    const float r = spin * 3.14159265f / 180.0f;
                    const float cs = std::cos(r);
                    const float sn = std::sin(r);
                    const float nx = vx * cs - vy * sn;
                    const float ny = vx * sn + vy * cs;
                    vx = nx;
                    vy = ny;
                }
                const ImVec2 c(io.MousePos.x + vx + nudge.x,
                               io.MousePos.y + vy + nudge.y);
                // rotDeg, not rot*90 -- the sprite is mid-turn for ~0.15s
                UIRoot::DrawItemIconRot(fg, icon->srv, c, ImVec2(dw, dh),
                    (heldFallback ? hRot : 0.0f) + a_held.rotDeg);
            } else {
                fg->AddRect(a, ImVec2(a.x + w, a.y + h), IM_COL32(220, 200, 140, 200), 3.0f);
            }

            // GI63: the rotate hint used to ride here in a black box. It moved
            // to the screen-bottom prompt bar (UIRoot::DrawPromptBar) -- a hint
            // that chases the cursor competes with the very thing it is telling
            // you to aim, and a hard-coded black plate matched none of the six
            // skins.
        }

        enum class DropWhere : std::uint8_t
        {
            kEquipSlot,       // hovered doll slot (g_slotTarget)
            kTrashArea,       // F2: any cell of the trash view (park intake)
            kEmptyCell,       // grid cell, empty & item fits
            kBlockerSingle,   // grid cell occupied by exactly one item
            kCellArea,        // anywhere over a grid (valid or not) — TERMINAL
                              // for whole tiles (no fallthrough to partner)
            kPartnerLoot,     // container window hovered (loot mode)
            kPartnerBarter,   // merchant window hovered (barter mode)
            kPartnerPickpocket,   // F6b: mark's window hovered (reverse lift)
            kVoid,            // outside every window
            kAlways,
        };

        bool DropWhereMatches(DropWhere a_where)
        {
            const bool cell = g_target.has &&
                              g_target.view < static_cast<int>(g_views.size());
            switch (a_where) {
            case DropWhere::kEquipSlot:
                return !g_slotTarget.empty();
            case DropWhere::kTrashArea:
                return cell && g_views[g_target.view].bagKey == kTrashKey;
            case DropWhere::kEmptyCell:
                return cell && g_target.valid;
            case DropWhere::kBlockerSingle:
                return cell && !g_target.valid && g_target.blockers.size() == 1;
            case DropWhere::kCellArea:
                return cell;
            case DropWhere::kPartnerLoot:
                return LootBarter::IsPartnerHovered() &&
                       LootBarter::IsLootMode(LootBarter::CurrentMode());
            case DropWhere::kPartnerBarter:
                return LootBarter::IsPartnerHovered() &&
                       LootBarter::CurrentMode() == LootBarter::Mode::kBarter;
            case DropWhere::kPartnerPickpocket:
                return LootBarter::IsPartnerHovered() &&
                       LootBarter::CurrentMode() == LootBarter::Mode::kPickpocket;
            case DropWhere::kVoid:
                // AllowWhenBlockedByActiveItem: the drop CLICK activates the
                // tile button under the cursor — plain IsWindowHovered then
                // reported "no window" and DISCARDED in-window drops
                return !ImGui::IsWindowHovered(
                    ImGuiHoveredFlags_AnyWindow |
                    ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            case DropWhere::kAlways:
                return true;
            }
            return false;
        }

        // ---- PARTNER item (loot take / barter buy with direct placement) ----
        // Phase 5-B: lands exactly on the dropped cell (main grid OR a bag
        // window), not first-fit. Only a valid (empty, fits) cell accepts it;
        // a blocked cell, the partner window, or outside cancels (nothing
        // moved). Bag intake: dropping onto a bag's free cell routes the item
        // into that bag. Buys still gold-check.
        // P2/3-5d: both defined further down; the partner-drop route needs
        // them before either exists.
        void ResolveDrop(Held& a_held);

        bool DropPartnerHeld(Held& a_held)
        {
            // ★(1.5.x) shelf gold dropped on a POUCH INSIDE AN OPEN SHELF-BAG
            // WINDOW deposits into that entry -- the bag window is not the
            // partner grid, so this has to be asked before the hover gate
            // below. Whatever the pouch cannot hold keeps riding (coin-route
            // grammar); a full pouch consumes the drop rather than falling
            // through to the void.
            if (a_held.obj && a_held.obj->IsGold() &&
                !LootBarter::IsBundleCarry() &&
                LootBarter::IsBundlePouchHovered()) {
                const int moved = LootBarter::DepositHeldGoldIntoBundlePouch();
                if (moved >= a_held.count) {
                    if (g_sound) g_sound(a_held.obj, false);
                    g_held.reset();
                } else if (moved > 0) {
                    a_held.count -= moved;
                    if (g_sound) g_sound(a_held.obj, false);
                } else {
                    Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                }
                return true;
            }
            // F7 (kLoot/kSteal): dropping a partner-carried item back ON the
            // partner grid REARRANGES the container — empty cell = move, on
            // another item = swap (mirrors the player-grid grammar). Chrome
            // (titlebar / bottom bar) cancels back to its spot.
            if (LootBarter::IsPartnerHovered() &&
                LootBarter::IsLootMode(LootBarter::CurrentMode())) {
                // ★★MOVING SOMETHING INSIDE A CHEST MOVES NOTHING. The engine
                // total does not change, the item does not travel, and the only
                // thing that happens is that a cell is somewhere else -- which
                // is now what the code does, instead of pushing a claim onto a
                // transfer queue and hoping a matcher re-attached it to the
                // right cell two frames later.
                const auto sd = LootBarter::QueryStoreDrop();
                if (sd.onCell && sd.freeSpot) {
                    // ★★(1.3.1) A CARRY LIFTED OUT OF A SHELF BAG HAS NO CELL
                    // TO MOVE. The engine item never went anywhere -- it was
                    // hidden inside the bundle, not stored -- so releasing it
                    // from the bundle is what makes a cell exist at all, and
                    // asking to MOVE one lands it wherever the reconcile
                    // happens to put a newly visible unit: the front gap.
                    // A plain partner carry does own a cell, and moves it.
                    const bool fromBundle = LootBarter::IsBundleCarry();
                    // ★staying in the container: the branch goes to the cell
                    // this drop is creating, not to the player
                    LootBarter::ConsumeBundleCarry(a_held.obj, a_held.count,
                                                   /*toPlayer=*/false);
                    if (fromBundle) {
                        LootBarter::PlaceStoredCell(a_held.obj, a_held.count,
                                                    sd.col, sd.row, a_held.rot,
                                                    a_held.uid, a_held.sig);
                    } else {
                        LootBarter::MoveHeldCell(sd.col, sd.row, a_held.rot);
                    }
                    if (g_sound) g_sound(a_held.obj, false);
                    g_held.reset();
                } else if (sd.onCell && sd.occ && LootBarter::IsBundleCarry()) {
                    // (1.3.1) no swap with a bundled carry -- the occupant's
                    // slot machinery has nothing to hand it. Keep carrying.
                } else if (sd.onCell && sd.occ && a_held.obj->IsGold() &&
                           GoldCoins::IsPouch(sd.occ->GetFormID())) {
                    // ★(1.4.4) SHELF GOLD ONTO A SHELF POUCH DEPOSITS, the
                    // same promise the player-side coin routes already keep
                    // (GoldOnPartnerPouch). Without this row the pair fell
                    // through to the rearrange grammar below -- different
                    // pools cannot merge, so the two cells simply traded
                    // places (user report). Whatever the pouch cannot hold
                    // keeps riding the cursor, coin-route grammar.
                    const int moved =
                        LootBarter::DepositHeldGoldIntoShelfPouch(sd.occSpotKey);
                    if (moved >= a_held.count) {
                        if (g_sound) g_sound(a_held.obj, false);
                        g_held.reset();
                    } else if (moved > 0) {
                        a_held.count -= moved;
                        if (g_sound) g_sound(a_held.obj, false);
                    } else {
                        Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                    }
                } else if (sd.onCell && sd.occ) {
                    // ★Same thing on an occupied square, and the same grammar
                    // the player's own board uses: the same pool MERGES up to
                    // the cap (what will not fit keeps riding the cursor);
                    // anything else trades places.
                    const int left = LootBarter::MergeHeldCellInto(sd.occSpotKey);
                    if (left == 0) {
                        if (g_sound) g_sound(a_held.obj, false);
                        g_held.reset();
                    } else if (left > 0) {
                        a_held.count = left;   // the remainder stays in hand
                        if (g_sound) g_sound(a_held.obj, false);
                    } else {
                        // not a merge: swap. The cells exchange positions and
                        // the occupant comes to the cursor.
                        const auto occ = sd;   // copy: reset() invalidates it
                        LootBarter::SwapHeldCellWith(occ.occSpotKey);
                        if (g_sound) g_sound(a_held.obj, false);
                        g_held.reset();
                        // GI24: the displaced occupant keeps its OWN identity
                        // and its own cell -- picking it up anonymously used to
                        // leave that cell unclaimed, and position order then
                        // handed it to a sibling.
                        BeginPartnerCarry(occ.occ, occ.occCount, occ.occValue,
                                          -1.0f, -1.0f,
                                          occ.occUid, occ.occXlIdx, occ.occOrd, occ.occRot);
                        LootBarter::NoteCarriedSpot(occ.occSpotKey);
                    }
                } else if (sd.onCell) {
                    // 2+ blockers: keep carrying (player-grid parity)
                } else {
                    g_held.reset();   // window chrome: cancel back
                }
                // ★S4: no tail rebuild. Every arm above moves PARTNER cells
                // (or only the cursor), and the partner board re-derives from
                // its own cell book every frame -- the player board did not
                // change in any of them. Measured as the #3 rebuild source of
                // the gate-1 session (8 of 85), repainting a board that was
                // already right.
                return true;
            }
            // Is the cursor over the player's own board at all? Everything
            // below needs the same answer, and the gold route needs it BEFORE
            // the swap question is even asked.
            const bool onOwnBoard = g_target.has &&
                                    !LootBarter::IsPartnerHovered() &&
                                    g_target.view < static_cast<int>(g_views.size());
            // F2: a partner item can't be taken INTO the trash — cancel.
            // ★Hoisted out of the placement block below so it is asked of EVERY
            // drop on the board, not only the ones that block reaches. It used
            // to sit inside, which was harmless only because everything it did
            // not cover fell through to the same cancel at the bottom.
            if (onOwnBoard && g_views[g_target.view].bagKey == kTrashKey) {
                g_held.reset();
                RequestRebuild();
                return true;
            }
            // ★★P2/3-5d: GOLD LANDS WHERE IT WAS DROPPED, at its own amount.
            //
            // Right-clicking a chest's gold pours it into the purse and lets
            // the mirror sort out the tiles -- correct, and unchanged. A DRAG
            // is a different promise: this many coins, in that square.
            // Everything needed for it already exists on this side, because a
            // withdrawn pouch amount has always ridden the cursor as a pinned
            // purse and the gold-fragment routes already say what happens when
            // one lands: an empty cell takes it, a coin or a pouch merges with
            // it, and whatever will not fit stays on the cursor.
            //
            // So the take happens, the amount becomes a pinned purse, and that
            // purse is handed straight to those routes -- the same ones a
            // shift-split fragment uses. No new grammar, and the slider is
            // skipped: a drag has already said how much.
            //
            // ★★AND IT SITS ABOVE THE SWAP QUESTION, which is where the first
            // version got it wrong. It lived inside the placement block below,
            // whose gate is "an empty cell, or a single occupant we may swap
            // with" -- and that swap test excludes coin forms by design. So the
            // route was reachable ONLY over an empty cell: dropping a chest's
            // gold onto a coin tile or a pouch matched nothing, fell through to
            // the cancel at the bottom, and the money went back in the chest
            // (reported: cases 3 and 4). The two destinations the fragment
            // routes exist to serve were the two they could not be reached for.
            //
            // Landing on the board is the whole condition. Which square, and
            // what is already sitting there, is the routes' question to answer
            // -- including "2+ blockers, keep carrying", which is why there is
            // no target-validity test here either.
            //
            // ★★S-0: AND THE LEDGER IS TOLD FIRST. The take moves engine coins
            // through LootBarter's queue, which the gold book cannot see, so
            // without the announcement below it read this arrival as loot off
            // the floor. Two reports came out of that: the pouch's auto-store
            // swallowed the dragged amount while the pin drew its value from
            // the coins already laid out, and the pin landing a tick before the
            // credit blinked a tile. GoldCoins::ExpectIncoming says both of
            // those out of one line -- and it is said ONLY on a queued request,
            // because a promise made for a refused transfer is a lie the ledger
            // would carry.
            if (onOwnBoard && a_held.obj->IsGold() &&
                LootBarter::IsLootMode(LootBarter::CurrentMode())) {
                const int take = a_held.count;
                if (LootBarter::RequestTake(a_held.obj, take, a_held.uid, a_held.sig)) {
                    GoldCoins::ExpectIncoming(take);
                    g_held.reset();
                    CarryWithdrawnGold(take);   // nets to zero against the above
                    if (g_held) ResolveDrop(*g_held);
                } else {
                    g_held.reset();   // refused: the carry goes home
                }
                RequestRebuild();
                return true;
            }
            // F7-swap: a partner item dropped on a SINGLE occupied player tile
            // takes/buys into that spot and the displaced tile rides the
            // cursor (player-grid C4 grammar). Coins/pouch keep the old no-op.
            const Item* swapDisp = nullptr;
            if (g_target.has && !g_target.valid && g_target.blockers.size() == 1 &&
                !LootBarter::IsPartnerHovered() &&
                g_target.view < static_cast<int>(g_views.size())) {
                const Item& cand = g_items[g_target.blockers.front()];
                if (!GoldCoins::IsCoinForm(cand.obj->GetFormID()) &&
                    cand.coinValue < 0) {
                    swapDisp = &cand;
                }
            }
            if (g_target.has && (g_target.valid || swapDisp) &&
                !LootBarter::IsPartnerHovered() &&
                g_target.view < static_cast<int>(g_views.size())) {
                const auto& v = g_views[g_target.view];   // (trash: asked above)
                const int cnt = a_held.count;
                bool ok = true;
                if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter && cnt <= 1) {
                    const int total = LootBarter::BuyPrice(a_held.obj, a_held.partnerValue);
                    if (GoldAmount() < total) {
                        Sfx::FailNote(Lang::T(Lang::Str::NotEnoughGold));
                        ok = false;
                    }
                }
                if (ok) {
                    if (LootBarter::IsLootMode(LootBarter::CurrentMode())) {
                        if (cnt > 1) LootBarter::OpenSlider(a_held.obj, cnt,
                            LootBarter::XferDir::kTake, {}, 0, a_held.uid, a_held.sig);
                        else LootBarter::RequestTake(a_held.obj, cnt,
                                                     a_held.uid, a_held.sig);
                    } else if (LootBarter::CurrentMode() ==
                               LootBarter::Mode::kPickpocket) {
                        // F6b: dragging out of a mark's pockets rolls too
                        if (cnt > 1) LootBarter::OpenSlider(a_held.obj, cnt,
                            LootBarter::XferDir::kPickTake, {}, 0, a_held.uid, a_held.sig);
                        else LootBarter::RequestPickTake(a_held.obj, cnt, a_held.uid, a_held.sig);
                    } else {   // kBarter
                        if (cnt > 1) LootBarter::OpenSlider(a_held.obj, cnt,
                            LootBarter::XferDir::kBuy, {}, a_held.partnerValue,
                            a_held.uid, a_held.sig);
                        else {
                            const int total = LootBarter::BuyPrice(a_held.obj, a_held.partnerValue);
                            LootBarter::RequestBuy(a_held.obj, 1, total, a_held.partnerValue,
                                                   a_held.uid, a_held.sig);
                        }
                    }
                    // B2: drop-cell placement as a one-shot HINT for the
                    // ACQUIRE pass, NOT a premature layout write — the old
                    // direct write clobbered an existing stack's saved
                    // position and leaked a full-count reservation when the
                    // quantity slider was cancelled.
                    const std::string hintBase = FormKey(a_held.obj);
                    // stale layout entries of this form (no live tile — e.g.
                    // the stack was sold out earlier this session) would win
                    // over the hint on rebuild and pull the purchase back to
                    // its OLD remembered cell — the explicit drop position
                    // must decide, so purge them
                    if (!GoldCoins::IsCoinForm(a_held.obj->GetFormID())) {
                        std::set<std::string> liveKeys;
                        for (const auto& gi : g_items) {
                            if (BaseKey(gi.key) == hintBase) liveKeys.insert(gi.key);
                        }
                        for (auto lt = g_layout.begin(); lt != g_layout.end();) {
                            if (BaseKey(lt->first) == hintBase &&
                                !liveKeys.contains(lt->first)) {
                                lt = g_layout.erase(lt);
                            } else {
                                ++lt;
                            }
                        }
                    }
                    // ★The pool the ARRIVING unit belongs to, so a signed tile
                    // cannot have its cell taken by a plain one landing in the
                    // same rebuild. A carry with no signature arms FORM-WIDE
                    // (empty pool) -- PoolPrefix(base,0,0) returns the base,
                    // which is NOT form-wide under Wants(), and that mismatch
                    // is why the fix below exists.
                    // ★(1.3.1) STEAL / PICKPOCKET DRIFT THE SIGNATURE: the
                    // engine's kSteal removal attaches the stolen-ownership
                    // extra IN TRANSIT, so the unit that was plain (or sig S)
                    // in the chest arrives as `base~S'` -- a pool the hint
                    // never named. Every steal/pickpocket drag therefore
                    // first-fit into the front gap. Those modes arm form-wide;
                    // one item rides the cursor at a time, so the guard the
                    // pool bought (two same-form arrivals in one rebuild)
                    // cannot trigger here anyway.
                    const bool sigDrifts =
                        LootBarter::CurrentMode() == LootBarter::Mode::kSteal ||
                        LootBarter::CurrentMode() == LootBarter::Mode::kPickpocket;
                    const std::string hintPool =
                        (!sigDrifts && (a_held.uid != 0 || a_held.sig != 0))
                            ? PoolPrefix(hintBase, a_held.uid, a_held.sig)
                            : std::string{};
                    // ★...and how many. A drag has already said the amount;
                    // the hint carries it so the aimed units are PLACED rather
                    // than left to whatever the fill pass did not want.
                    //
                    // ★★★BUT NOT WHEN THE SLIDER IS STILL OPEN. The line above
                    // used to claim "a stack that opened the quantity slider
                    // commits its own number on confirm, so cnt is right here
                    // too". It is not: OpenSlider only ASKS, and this runs
                    // immediately with cnt still holding the whole dragged
                    // stack. Confirming a smaller number never reached the
                    // hint.
                    //
                    // Measured -- 15 on a corpse, 50 in the pack, 7 confirmed:
                    //
                    //   [FLICK] ...01397D stack 0 -> 50   15x[6,5] 35x[-1,-1]
                    //
                    // The hint reserved FIFTEEN units of the dropped-on cell,
                    // so fifteen of the player's own fifty sat there and the
                    // other thirty-five were pushed out to a free square. That
                    // is the "it scattered" report, and the 15 is the corpse's
                    // count leaking into a board it never belonged to.
                    //
                    // 0 means "whatever arrives", which is exactly right for an
                    // amount nobody has chosen yet.
                    const int hintCount = (cnt > 1) ? 0 : cnt;
                    g_dropHint = { hintBase, hintPool,
                                   g_target.col, g_target.row, v.bagKey, a_held.rot,
                                   hintCount };
                    if (swapDisp) {
                        // free the displaced tile's spot for the incoming item
                        // and put it on the cursor (same as the C4 swap)
                        const Item d = *swapDisp;   // copy: reset invalidates it
                        g_layout.erase(d.key);
                        g_held.reset();
                        g_held = Held{ d.key, d.obj, d.mask, d.count,
                                       d.def.bag != 0, d.def.scale,
                                       d.mask.w * CellPx() * 0.5f,
                                       d.mask.h * CellPx() * 0.5f, true };
                        g_held->coinValue = d.coinValue;
                        g_held->xlIdx = d.xlIdx;   // GI1
                        g_held->uid = d.uid;
                        g_held->sig = d.sig;       // GI25
                        g_held->fav = d.fav;       // GI36
                        g_held->stolen = d.stolen;
                        g_held->quest  = d.quest;
                        g_held->SetRot(d.rot);       // GI62
                        HoldByPivot(*g_held, d.def);
                        if (g_sound) g_sound(d.obj, true);
                        RequestRebuild();
                        return true;
                    }
                }
            }
            g_held.reset();
            RequestRebuild();
            return true;
        }

        // ---- GOLD fragment (G4; held.key is its pin) ----
        bool GoldFragOnEmptyCell(Held& a_held)
        {
            // (1) empty cell -> anchor the pin here
            const auto& v = g_views[g_target.view];
            PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, 1, a_held.rot);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            return true;
        }

        bool GoldFragOnBlocker(Held& a_held)
        {
            const auto& v = g_views[g_target.view];
            const Item tgt = g_items[g_target.blockers.front()];
            const RE::FormID tf = tgt.obj->GetFormID();
            const bool tgtGold = GoldCoins::IsCoinForm(tf) &&
                                 !GoldCoins::IsPouch(tf) && tgt.coinValue >= 0;
            if (GoldCoins::IsPouch(tf)) {
                // (2c) dropped ON the pouch -> store the value (mirrors the
                // whole-tile rule: the pin returns to walking first so the
                // pouch can draw from it; no room -> the pin restores)
                const int v2 = a_held.coinValue;
                const int stored = GoldCoins::StoreToPouch(tgt.key, v2);
                if (stored > 0 && stored < v2) {   // ★S-G: partial keeps riding
                    SetCoinRecord(a_held.key, v2 - stored);
                    a_held.coinValue = v2 - stored;
                    if (g_sound) g_sound(a_held.obj, false);
                }
                if (stored >= v2) {
                    g_layout.erase(a_held.key);
                    if (g_sound) g_sound(a_held.obj, false);
                    g_held.reset();
                }
                // refused outright: the record never moved -- keep riding
            } else if (tgtGold) {
                // (2) merge onto ANY gold tile (pin or auto) — the value
                // lands as a pinned purse at the target's cell
                MergeGoldInto(a_held, tgt.key, tgt.coinValue,
                    { g_target.col, g_target.row, v.bagKey, 1 });
            } else {
                // (2d) non-gold item -> SWAP (same rule as a whole tile):
                // the pin anchors at the drop cell, the displaced item
                // rides the cursor
                g_layout.erase(tgt.key);
                PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, 1, a_held.rot);
                if (g_sound) {
                    g_sound(a_held.obj, false);
                    g_sound(tgt.obj, true);
                }
                g_held = Held{ tgt.key, tgt.obj, tgt.mask, tgt.count,
                               tgt.def.bag != 0, tgt.def.scale,
                               tgt.mask.w * CellPx() * 0.5f,
                               tgt.mask.h * CellPx() * 0.5f, true };
                g_held->coinValue = tgt.coinValue;
                g_held->xlIdx = tgt.xlIdx;   // GI1
                g_held->uid = tgt.uid;
                g_held->sig = tgt.sig;       // GI25
                g_held->fav = tgt.fav;       // GI36
                g_held->stolen = tgt.stolen;
                g_held->quest  = tgt.quest;
                g_held->SetRot(tgt.rot);       // GI62
                HoldByPivot(*g_held, tgt.def);
            }
            return true;
        }

        bool GoldFragToVoid(Held& a_held)
        {
            if (LootBarter::CurrentMode() == LootBarter::Mode::kNormal) {
                GoldCoins::DropAsGold(a_held.coinValue);   // (4) discard the gold
            }
            // loot/barter: (5) cancel — the value is already back in walking
            g_layout.erase(a_held.key);
            g_held.reset();
            return true;
        }

        // Same FORM is not the same POOL. A tempered dagger and a plain one
        // share a base form yet can never stack, so testing the form alone sent
        // them down the MERGE path -- which, for gear with a stack cap of 1,
        // absorbs nothing and returns false. The drop simply did nothing and the
        // two would not swap. Pools answer it correctly: same pool -> merge,
        // different pool -> swap, exactly as with two unrelated items.
        std::string HeldPool(const Held& a_held)
        {
            return PoolPrefix(FormKey(a_held.obj), a_held.uid, a_held.sig);
        }

        // ★★A NEW TILE FOR A CARRY THAT IS LANDING. Named after the FORM and
        // stamped with what it is showing -- which is the whole 1.3.2 grammar in
        // one place.
        //
        // These three landing paths were the last ones still calling
        // NextTileKey(HeldPool(...)), i.e. still minting `base~SIG#n`. A tile
        // born that way was named after the carry's signature AT THE MOMENT IT
        // LANDED, so the first thing that changed the item afterwards -- a
        // poison, a spent charge -- left it answering to a name nothing matched,
        // and it moved. Naming it after the form and recording the binding
        // separately means nothing can ever rename it again.
        std::string NewTileForHeld(const Held& a_held)
        {
            const std::string nk = NextTileKey(FormKey(a_held.obj));
            auto& le = g_layout[nk];
            le.uid   = a_held.uid;
            le.sig   = a_held.sig;
            le.xlIdx = a_held.xlIdx;
            // ★★A SPLIT FRAGMENT KNOWS ITS POOL, NOT ITS uid/sig.
            //
            // PickupPartial records the source's pool string and leaves uid and
            // sig at zero -- reasonably, since the fragment is a QUANTITY out of
            // a pool rather than a named unit. But this is where a new tile's
            // pool gets decided, and reading the zeroes made every fragment a
            // PLAIN tile.
            //
            // For an ordinary stack nothing showed: the units were plain, so the
            // plain pool was the right answer by accident. Split a STOLEN stack
            // and the accident ends -- ownership lives on an extra list, which
            // gives those units a signature. The fragment landed in a pool with
            // no units in it, so the reconcile erased it and refilled the tile
            // it came from: "I split it, put it down, and it merged back"
            // (reported for stolen goods; the same was true of anything signed
            // -- tempered, enchanted, poisoned).
            if (le.uid == 0 && le.sig == 0 && !a_held.srcPool.empty()) {
                le.uid = UidOf(a_held.srcPool);
                le.sig = SigOf(a_held.srcPool);
            }
            return nk;
        }

        // ---- STACK fragment (G4 split; no key yet) ----
        bool StackFragOnEmptyCell(Held& a_held)
        {
            // (1) empty cell -> new tile owning this quantity
            const auto& v = g_views[g_target.view];
            const std::string nk = NewTileForHeld(a_held);
            PlaceTile(nk, g_target.col, g_target.row, v.bagKey, a_held.count, a_held.rot);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            return true;
        }

        bool StackFragOnBlocker(Held& a_held)
        {
            const auto& v = g_views[g_target.view];
            const Item tgt = g_items[g_target.blockers.front()];
            // Same pool AND stackable -> merge. Same pool but NOT stackable (two
            // identical daggers) has no merge to do: the old code took the merge
            // branch, computed room = cap(1) - 1 = 0, and returned false, so the
            // drop did nothing at all and the item stayed stuck on the cursor.
            // Interchangeable or not, the player asked for these two cells to
            // trade places -- fall through to the swap.
            if (PoolOfSlot(tgt.key) == HeldPool(a_held) && EffectiveCap(a_held.obj) > 1) {
                // (2) merge into the same-form stack up to cap (full target:
                // fall through — later rows won't match over the grid, so the
                // fragment keeps carrying instead of a pointless swap)
                const int cap = EffectiveCap(a_held.obj);
                const int room = (std::max)(0, cap - tgt.count);
                const int absorbed = (std::min)(room, a_held.count);
                if (absorbed <= 0) return false;
                g_layout[tgt.key].count = tgt.count + absorbed;
                a_held.count -= absorbed;
                if (g_sound) g_sound(a_held.obj, false);
                if (a_held.count <= 0) g_held.reset();
                return true;   // leftover (if any) keeps carrying
            }
            // (2b) different-form tile -> SWAP (same rule as a whole tile):
            // the fragment becomes a NEW tile at the drop cell, the
            // displaced item rides the cursor
            const std::string nk = NewTileForHeld(a_held);
            ParkTile(tgt.key);   // survives on the cursor -- keep its flags
            PlaceTile(nk, g_target.col, g_target.row, v.bagKey, a_held.count, a_held.rot);
            if (g_sound) {
                g_sound(a_held.obj, false);
                g_sound(tgt.obj, true);
            }
            g_held = Held{ tgt.key, tgt.obj, tgt.mask, tgt.count,
                           tgt.def.bag != 0, tgt.def.scale,
                           tgt.mask.w * CellPx() * 0.5f,
                           tgt.mask.h * CellPx() * 0.5f, true };
            g_held->coinValue = tgt.coinValue;
            g_held->xlIdx = tgt.xlIdx;   // GI1
            g_held->uid = tgt.uid;
            g_held->sig = tgt.sig;       // GI25
            g_held->fav = tgt.fav;       // GI36
            g_held->stolen = tgt.stolen;
            g_held->quest  = tgt.quest;
            g_held->SetRot(tgt.rot);       // GI62
            HoldByPivot(*g_held, tgt.def);
            return true;
        }

        // Phase 7 / 1.4.0: one line, used by every fragment row that would
        // LOSE a quest object (sell, world drop, plant). The fragment cancels
        // back to its spot -- the source tile is short by N since the split and
        // the rebuild's reconciler restores it (ACQUIRE), same as a refusal.
        bool QuestFragBlocked(Held& a_held)
        {
            if (!a_held.quest) return false;
            Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
            g_held.reset();
            RequestRebuild();
            return true;
        }

        bool StackFragStore(Held& a_held)
        {
            if (!LootBarter::PartnerHasRoomFor(a_held.obj, a_held.count)) {
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));   // (1.3.3)
                return true;   // the fragment keeps riding the cursor
            }
            // ★★THE FRAGMENT AIMS AT A SQUARE TOO, and it was the only store
            // path that never said so: a whole tile has always honoured its drop
            // cell, while five potions carefully split off and placed landed in
            // the front gap. Nothing makes a fragment less aimed than a whole
            // tile -- it is MORE aimed, since the player already chose how many.
            const auto sd = LootBarter::QueryStoreDrop();
            LootBarter::RequestStore(a_held.obj, a_held.count,
                                     HeldUidOf(a_held.key, a_held.uid), a_held.sig,
                                     a_held.fav, -1, a_held.key);   // (3) store
            if (sd.onCell && sd.freeSpot) {
                LootBarter::PlaceStoredCell(a_held.obj, a_held.count,
                                            sd.col, sd.row, a_held.rot,
                                            HeldUidOf(a_held.key, a_held.uid),
                                            a_held.sig);
            }
            // fragment (empty key) = form-level pending only
            NotePendingRemove(a_held.obj, a_held.key, a_held.count, a_held.xlIdx);
            g_held.reset();
            return true;
        }

        bool StackFragSell(Held& a_held)
        {
            // (3b) SELL the fragment by dropping it on the merchant window —
            // the quantity was already chosen at the split slider, so no
            // second slider. The source tile is short by N since the split,
            // so a refusal (reset) restores it via ACQUIRE.
            const RE::FormID fid = a_held.obj->GetFormID();
            if (QuestFragBlocked(a_held)) return true;   // never sold
            const int val = TileValue(a_held.obj,                          // GI43
                HeldUidOf(a_held.key, a_held.uid), a_held.sig);
            if (!LootBarter::MerchantBuys(a_held.obj, a_held.stolen)) {
                Sfx::FailNote(Lang::T(Lang::Str::MerchantWontBuy));
            } else {
                const int total = val > 0
                    ? LootBarter::SellPriceTotal(a_held.obj, val, a_held.count) : 0;
                if (total > 0 && LootBarter::MerchantGold() < total) {
                    Sfx::FailNote(Lang::T(Lang::Str::MerchantNoGold));
                } else {
                    LootBarter::RequestSell(a_held.obj, a_held.count,
                        total, val * a_held.count, a_held.uid, a_held.sig, a_held.fav,
                        a_held.xlIdx, a_held.key);
                    // GI25: the split fragment still belongs to a POOL, and the
                    // pending bookkeeping has to say which one -- an empty key
                    // fell back to "deduct from the plain pool", the same
                    // mis-attribution that made a stored tempered dagger take a
                    // plain one's slot on its way out.
                    NotePendingRemove(a_held.obj, a_held.key, a_held.count, a_held.xlIdx);
                }
            }
            g_held.reset();
            return true;
        }

        bool StackFragToVoid(Held& a_held)
        {
            if (LootBarter::CurrentMode() == LootBarter::Mode::kNormal) {
                if (QuestFragBlocked(a_held)) return true;   // never discarded
                if (g_dropWorld) {
                    g_dropWorld(a_held.obj, a_held.count,   // GI36
                        ResolveExitUnit(a_held.obj, a_held.uid, a_held.sig, a_held.count,
                                        a_held.fav ? a_held.count : 0, a_held.xlIdx));
                }   // (4) discard N
            }
            g_held.reset();   // (5) loot/barter: cancel (Rebuild restores N)
            return true;
        }

        // ---- WHOLE tile ----
        bool WholeOnEquipSlot(Held& a_held)
        {
            // Dropping onto an OCCUPIED slot should read as a swap. The engine
            // unequips the old item for us, but nothing told the grid where to
            // put it, so it re-entered as a fresh pickup and took the first free
            // gap. Hand it the cell the carried item is vacating.
            // Rule 26: a drop onto an OCCUPIED slot is a swap, and a swap hands
            // the displaced item to the CURSOR -- exactly as dropping onto an
            // item in the grid does (rule 20). Sending it back to a cell instead
            // made the doll the one place where a swap behaved differently.
            //
            // Identify the occupant BEFORE the equip: comparing the form alone
            // said "same item" for a tempered sword dropped over a plain one, so
            // the swap was skipped in exactly the case pools exist for.
            RE::TESBoundObject* worn = Equip::WornObjectAt(g_slotTarget);
            auto*               wxl = worn ? Equip::WornExtraAt(g_slotTarget) : nullptr;
            // GI54: the hand is how the ENGINE marks the worn list -- weapons
            // take the slot's hand, a torch is left, and ARMOUR (a shield on
            // the shieldL slot included) is biped-worn with no hand mark.
            // Slot-based hand 2 made the landed-test look for an ExtraWornLeft
            // a shield never gets: the pending entry double-counted with the
            // applied worn list and the SPARE blinked out meanwhile.
            const auto engineHand = [&](RE::TESBoundObject* a_o) {
                if (!a_o) return 0;
                if (a_o->Is(RE::FormType::Weapon)) {
                    return g_slotTarget == "shieldL" ? 2 : 1;
                }
                if (a_o->Is(RE::FormType::Light)) return 2;
                return 0;
            };
            const int           wornHand = engineHand(worn);
            const std::uint16_t wsig = InstanceSig(wxl);
            std::uint16_t       wuid = 0;
            if (wxl) {
                if (const auto* xu = wxl->GetByType<RE::ExtraUniqueID>()) wuid = xu->uniqueID;
            }
            // The carried unit came off the board (hand 0) or off the OTHER hand;
            // either way it is not the unit standing in this slot. Comparing only
            // form+signature called two identical daggers "the same unit", so the
            // swap was skipped and the displaced one silently went to the pack.
            // ...but a carry that was DISPLACED by a swap can never be "the unit
            // already standing in this slot": something else took its place, and
            // that something has to come off. With two plain daggers the identity
            // test said "same unit" (uid 0, sig 0, same form, same hand), the
            // displaced one was never handed to the cursor, and it fell back into
            // the pack -- taking the parked star's slot on the way.
            const bool swapping = worn && !(worn == a_held.obj && wsig == a_held.sig &&
                                            a_held.fromDoll && !a_held.swappedOut &&
                                            a_held.hand == wornHand);

            // C6: dropped on an equip slot — the gate decides; a reject
            // snaps the item back (its layout entry is intact).
            // UidOf(key) alone lost the signature, so a tempered weapon dropped
            // on a slot equipped an arbitrary copy: pass the carried unit's own
            // identity instead of re-deriving a partial one from its key.
            const bool accepted = Equip::EquipItem(a_held.obj, g_slotTarget, a_held.uid,
                                                   a_held.xlIdx, a_held.sig, a_held.key,
                                                   a_held.count);   // ammo: the whole carry
            // Same interim gap as the right-click path: the carry ends NOW but
            // the engine equips later, so the tile would flicker back into the
            // cell it just left.
            if (g_poolTrace) {
                // ★The COUNTS earn their place here: "swap=true accepted=true"
                // looked healthy through three wrong diagnoses, and it was the
                // quantities either side of the swap that finally told the story.
                SKSE::log::info("[SWAP] slot '{}' <- '{}' x{} key '{}' | occupant '{}' x{} "
                                "wxl={} swap={} accepted={}",
                    g_slotTarget, a_held.obj ? a_held.obj->GetName() : "?", a_held.count,
                    a_held.key, worn ? worn->GetName() : "(empty)",
                    Equip::WornCountAt(g_slotTarget),
                    wxl ? "yes" : "no", swapping, accepted);
            }
            // ★Held BEFORE the reset below, which invalidates `a_held`.
            RE::TESBoundObject* const swapInObj = a_held.obj;
            // ★Ring session: the whole carry, for the REJECT path's origin
            // return -- a doll-lifted piece refused at another slot goes back
            // where it was worn, not to the grid (user spec).
            const Held heldSnap = a_held;
            if (accepted) {
                // GI54: the INCOMING item's engine hand, not the occupant's --
                // a shield replacing a left-hand sword is still hand 0.
                const int equipUnits = Equip::EquipCountFor(a_held.obj, a_held.count);
                NotePendingEquip(a_held.obj, a_held.uid, a_held.sig,
                                 engineHand(a_held.obj), a_held.key,
                                 equipUnits, a_held.xlIdx);
                g_drainHint = { FormKey(a_held.obj), a_held.key };
                // ★A carry BIGGER than what one equip takes -- a torch stack
                // dropped on the hand; ammo is exempt because EquipCountFor
                // takes its whole tile. The engine wears one; the surplus is
                // back in the pack, but equipping moves no containers, so no
                // event will ever redraw it -- the stack just vanished from
                // the board (user report). The "no tail rebuild for accepted
                // drops" verdict was measured on shapes where the carry and
                // the equip agree; this shape breaks that premise, and the
                // rebuild it pays for is exactly one. The drain hint above
                // bills the in-flight unit to the lifted tile, so the
                // remainder re-emits in its own cell.
                if (a_held.count > equipUnits) {
                    RequestRebuild();
                }
                // ★S1: the carried tile went to the DOLL -- its stash copy is
                // spent. (The surplus rebuild above re-derives the remainder;
                // Rebuild clears the stash itself, so the order is safe.)
                DiscardStash("equipped");
            }
            g_held.reset();
            // Only an ACCEPTED equip that actually displaces something starts the
            // return carry. A potion or spell tome dropped on a slot is drunk or
            // read -- nothing comes off, and there is nothing to hand back.
            // ★★The SECOND RING'S displaced occupant rides the cursor like
            // every other swap -- its carry is marked fromCarrier, so the
            // worn-backed accounting cannot let it claim the FIRST ring's
            // list (the leak that made the first cursor attempt draw the ring
            // twice). The ONE exception: first slot empty, where Wear moves
            // the displaced ring onto the FIRST slot instead of the pack --
            // nothing leaves the body, and a cursor copy really would be a
            // duplicate.
            const bool ringL = g_slotTarget == "ringL";
            SKSE::log::info("[RING] drop-swap slot '{}': in='{}' occupant='{}' "
                            "second='{}' accepted={}",
                g_slotTarget, swapInObj ? swapInObj->GetName() : "?",
                worn ? worn->GetName() : "-",
                DualRing::Second() ? DualRing::Second()->GetName() : "-",
                accepted ? 1 : 0);
            const bool ringLToFirst = ringL &&
                                      Equip::WornObjectAt("ringR") == nullptr;
            if (accepted && swapping && !ringLToFirst) {
                // The engine has not unequipped it yet, so this is exactly a doll
                // pickup -- and it must name the HAND, or the worn-unit match can
                // consume the copy we just put IN and leave the displaced one
                // counted on the board as well as on the cursor.
                // ★...and the displaced quiver comes back WHOLE, same as any
                // other unequip. ★Asked of the SLOT, not of wxl: ammo can be
                // worn with no ExtraDataList at all, and `wxl ? GetCount() : 1`
                // then answered 1 for a hundred-arrow quiver -- ninety-nine of
                // them went to the pack instead of onto the cursor.
                BeginCarry(worn, wuid, wsig, wornHand, /*swappedOut=*/true,
                           Equip::EquipCountFor(worn,
                               Equip::WornCountAt(g_slotTarget)),
                           /*fromCarrier=*/ringL);
            }
            // ★No tail rebuild for ACCEPTED drops. The !rbdrop interrogation
            // measured every accepted shape without it: plain equips, swaps,
            // same-form swaps, stackables, potions and tomes were all fine --
            // the carry had already left the board at lift, so there was
            // nothing for a full rebuild to draw. The carrier's stand-down
            // moved into DualRing::TakeOff itself (rule 6).
            // ★★A REJECTED drop is the case the interrogation never ran: the
            // carry is consumed either way (g_held.reset above), the layout
            // entry is intact, and the only thing that ever put the tile back
            // on screen was this rebuild -- without it a refused ring vanished
            // until the next reopen (user report). The reject path keeps it --
            // unless the carry's origin is a SLOT, where the piece goes back
            // on the body instead and the board has nothing to redraw.
            // ★S1: a rejected BOARD-origin carry has a stash and a live
            // layout entry -- it goes home as one tile. Doll-origin keeps its
            // re-wear return; anything unproven re-derives as before.
            if (!accepted && !ReturnCarryToOrigin(heldSnap) &&
                !UnstashTileHome()) {
                RequestRebuild();
            }
            return true;
        }

        bool WholeOnCellArea(Held& a_held)
        {
            const auto& v = g_views[g_target.view];
            if (g_target.valid) {
                // C3: place (in-memory; the cosave persists on game save)
                if (g_poolTrace) {
                    SKSE::log::info("[ACT] drop-on-cell '{}' key '{}' -> [{},{}]",
                        a_held.obj ? a_held.obj->GetName() : "?", a_held.key,
                        g_target.col, g_target.row);
                }
                PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, a_held.count, a_held.rot);
                if (g_sound) g_sound(a_held.obj, false);
                // ★S1: one tile lands, one tile is drawn. Captured before the
                // reset invalidates a_held; a carry that never stashed (doll
                // origin, post-rebuild carry) declines into the old rebuild.
                const std::string dstBag = v.bagKey;
                const int dstCol = g_target.col, dstRow = g_target.row;
                const int dstRot = a_held.rot, dstCnt = a_held.count;
                g_held.reset();
                if (!UnstashTileTo(dstBag, dstCol, dstRow, dstRot, dstCnt)) {
                    RequestRebuild();
                }
            // ★The swap/merge branch runs when the cell is NOT valid, so the
            // filter check above does not cover it — a sword dropped onto an
            // ore tile would trade places with it and end up inside the ore
            // bag. Same rule, stated again where the second door is.
            } else if (g_target.blockers.size() == 1 &&
                       // E4b: same narrowed rule as the ghost — a bag may swap
                       // into a GENERAL bag, never the trash / a typed bag / a
                       // spot that would loop its own containment chain
                       !(a_held.isBag && !v.bagKey.empty() &&
                         (v.bagKey == kTrashKey || !v.accept.empty() ||
                          NestsWithin(a_held.key, v.bagKey))) &&
                       !(!v.accept.empty() && a_held.obj &&
                         BagFilter::FilterOf(a_held.obj) != v.accept)) {
                const Item disp = g_items[g_target.blockers.front()];
                const RE::FormID hfid = a_held.obj->GetFormID();
                const RE::FormID dfid = disp.obj->GetFormID();
                const bool heldCoin = GoldCoins::IsCoinForm(hfid) &&
                                      !GoldCoins::IsPouch(hfid);
                const bool dispCoin = GoldCoins::IsCoinForm(dfid) &&
                                      !GoldCoins::IsPouch(dfid);
                bool doSwap = false;
                if (GoldCoins::IsPouch(dfid) && heldCoin) {
                    // G2: a coin dropped ON the pouch stores its value (up to
                    // the cap; no room -> keep carrying). G4: a pinned purse
                    // must return to walking first so the pouch can draw.
                    const int v2 = a_held.coinValue;
                    // ★S-G: the tile's record is the amount; partials shrink
                    const int stored = GoldCoins::StoreToPouch(disp.key, v2);
                    if (stored > 0 && stored < v2) {
                        SetCoinRecord(a_held.key, v2 - stored);
                        a_held.coinValue = v2 - stored;
                        if (g_sound) g_sound(a_held.obj, false);
                        RequestRebuild();
                    } else if (stored >= v2) {
                        g_layout.erase(a_held.key);
                        if (g_sound) g_sound(a_held.obj, false);
                        g_held.reset();
                        RequestRebuild();
                    }
                } else if (heldCoin && dispCoin &&
                           a_held.coinValue >= 0 && disp.coinValue >= 0) {
                    // C4-G: a WHOLE gold tile dropped on another gold tile
                    // merges — shared MergeGoldInto (pouch mechanism,
                    // remainder rides the cursor as a pin)
                    MergeGoldInto(a_held, disp.key, disp.coinValue,
                        { g_target.col, g_target.row, v.bagKey, 1 });
                    RequestRebuild();
                } else if (!heldCoin && !dispCoin && disp.key != a_held.key &&
                           !a_held.isBag && disp.def.bag == 0 &&
                           PoolOfSlot(disp.key) == HeldPool(a_held) &&
                           EffectiveCap(a_held.obj) > 1) {
                    // C4-S: a WHOLE stack tile dropped on a same-form tile
                    // merges up to the stack cap; the overflow stays on the
                    // cursor (its layout entry keeps the reduced count, so
                    // cancel restores it in place).
                    const int cap = EffectiveCap(a_held.obj);
                    const int room = (std::max)(0, cap - disp.count);
                    const int absorbed = (std::min)(room, a_held.count);
                    if (absorbed > 0) {
                        g_layout[disp.key].count = disp.count + absorbed;
                        a_held.count -= absorbed;
                        if (g_sound) g_sound(a_held.obj, false);
                        // ★S1: the target tile's number changes, the carry's
                        // number changes -- two counts, no rebuild. The stash
                        // rides the remainder; a carry that never stashed
                        // re-derives as before.
                        const bool shown = SetTileDisplayCount(disp.key,
                                                               disp.count + absorbed);
                        if (a_held.count <= 0) {
                            g_layout.erase(a_held.key);
                            g_held.reset();
                            DiscardStash("merged whole");
                            if (!shown) RequestRebuild();
                        } else {
                            g_layout[a_held.key].count = a_held.count;
                            if (g_stash) g_stash->count = a_held.count;
                            if (!shown || !g_stash) RequestRebuild();
                        }
                    }
                    if (g_poolTrace) {
                        SKSE::log::info("[SWAP] merge '{}' into '{}' absorbed={} "
                                        "(cap {}), carrying {}",
                            a_held.key, disp.key, absorbed, cap, a_held.count);
                    }
                    // target full: swapping two same-form tiles only
                    // exchanged their positions (pointless churn) — keep
                    // carrying instead
                } else {
                    doSwap = true;
                }
                if (doSwap) {
                    if (g_poolTrace) {
                        SKSE::log::info("[SWAP] held '{}' (uid {:04X} sig {:04X}) onto "
                                        "'{}' (uid {:04X} sig {:04X}) at [{},{}] -> "
                                        "displaced rides the cursor",
                            a_held.key, a_held.uid, a_held.sig,
                            disp.key, disp.uid, disp.sig, g_target.col, g_target.row);
                    }
                    // C4: swap — free the displaced item's cell FIRST, then
                    // save mine; it snaps to the cursor immediately. PARK, not
                    // erase: the item survives, so its flags must too.
                    ParkTile(disp.key);
                    PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, a_held.count, a_held.rot);
                    if (g_sound) {
                        g_sound(a_held.obj, false);
                        g_sound(disp.obj, true);
                    }
                    // ★S1 captures BEFORE the held slot is overwritten below
                    const std::string myBag = v.bagKey;
                    const int myCol = g_target.col, myRow = g_target.row;
                    const int myRot = a_held.rot, myCnt = a_held.count;
                    g_held = Held{ disp.key, disp.obj, disp.mask, disp.count,
                                   disp.def.bag != 0, disp.def.scale,
                                   disp.mask.w * CellPx() * 0.5f,
                                   disp.mask.h * CellPx() * 0.5f, true };
                    g_held->coinValue = disp.coinValue;   // G4
                    g_held->xlIdx = disp.xlIdx;           // GI1
                    g_held->uid = disp.uid;
                    g_held->sig = disp.sig;               // GI25
                    g_held->fav = disp.fav;               // GI36
                    g_held->stolen = disp.stolen;
                    g_held->quest  = disp.quest;
                    g_held->SetRot(disp.rot);               // GI62
                    HoldByPivot(*g_held, disp.def);
                    // ★S1: my tile seats, the displaced one comes off -- two
                    // tiles, no rebuild. Any miss re-derives exactly as before
                    // (the enumeration's carry exclusion is untouched).
                    if (!UnstashTileTo(myBag, myCol, myRow, myRot, myCnt) ||
                        !StashTileForCarry(disp.key)) {
                        RequestRebuild();
                    }
                }
            }
            // other invalid targets (2+ blockers, bag-in-bag): keep carrying.
            // TERMINAL either way — a whole tile over a grid never falls
            // through to the partner/void rows (matches the old else-if).
            return true;
        }

        // ---- ★ONE PATH / O-1: USE, as a route -----------------------------
        //
        // The one destination a drag cannot reach -- the player's own mouth, or
        // the reading of a book -- and until now the only transfer that never
        // left the draw pass. The right-click branch called the engine from
        // inside the tile loop and left three notes to itself about what the
        // board should look like afterwards; this is that branch, moved whole
        // into the shape every other destination already has.
        //
        // What the move actually buys, stated honestly:
        //   - The board changes at the CLICK'S OWN MOMENT, in the same place a
        //     drag's drop changes it. The ring router's reach-backs into the
        //     board from the engine tick (ForgetTile + DropTileDisplay, one or
        //     more frames later) stop being necessary -- that is O-1b.
        //   - A refusal comes back INSTANTLY. It used to be a two-second ghost:
        //     nothing put the tile back, so the pending-equip TTL did, long
        //     after the player had stopped believing the click worked.
        //   - The guards sit with the destination instead of beside the click.
        //
        // What it does NOT buy, and the plan claimed it would: NotePendingEquip,
        // the drain hint and the partial removal all survive. They are not
        // stand-ins for a cursor -- they bridge the ENGINE'S LATENCY, which no
        // amount of restructuring on our side shortens. The carry ends at the
        // click; the engine equips or drinks a frame or more later; something
        // has to hold the unit off the board in between, and these are it.
        bool WholeUse(Held& a_held)
        {
            if (!a_held.obj) return false;
            if (g_poolTrace) {
                const auto le = g_layout.count(a_held.key) ? g_layout[a_held.key]
                                                           : LayoutEntry{};
                SKSE::log::info("[ACT] rclick-equip '{}' key '{}' at [{},{}]",
                    a_held.obj->GetName(), a_held.key, le.col, le.row);
            }
            // ★Guard first, and it is the DESTINATION'S guard now: vanilla
            // refuses this in so many words ("You can not eat quest items."),
            // and wearing a quest item stays allowed. Declining here returns
            // the tile to its cell -- see RunSyntheticRoute.
            if (a_held.quest && ConsumingWouldEatIt(a_held.obj)) {
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                return false;
            }
            // ★★UseItem, not EquipItem: a click is not a request to WEAR. The
            // old call went through the doll's type gate, so anything outside
            // WEAP/ARMO/AMMO/LIGH/ALCH/SCRL was refused before the engine ever
            // saw it -- which is why mod items whose whole purpose is "click me
            // in the inventory" did nothing at all (AddItemMenu).
            // count: what THIS tile holds. Ammo goes on by the tileful
            // (EquipCountFor); everything else ignores it and takes one.
            // ★O-1b: a ring bound for the SECOND slot is AIMED, not left to a
            // router downstream. Aiming puts the click in the same targeted
            // branch a drag onto that slot uses -- and that branch names the
            // exact unit (it resolves srcList) where the router hands the
            // engine a null list and lets it pick. One road, and the better of
            // the two. A refusal there falls through to the first slot, so
            // nothing is lost by aiming.
            const bool queued =
                Equip::RingWantsSecondSlot(a_held.obj)
                    ? Equip::EquipItem(a_held.obj, "ringL", a_held.uid,
                                       a_held.xlIdx, a_held.sig, a_held.key,
                                       a_held.count)
                    : Equip::UseItem(a_held.obj, a_held.uid, a_held.xlIdx,
                                     a_held.sig, a_held.key, a_held.count);
            if (!queued) {
                return false;   // refused at our own gate: straight back
            }
            RE::TESBoundObject* const obj = a_held.obj;
            const std::string         key = a_held.key;
            const std::uint16_t       uid = a_held.uid;
            const std::uint16_t       sig = a_held.sig;
            const int                 xl = a_held.xlIdx;
            const int                 count = a_held.count;
            g_held.reset();   // the carry is spent

            // ...and the board bookkeeping runs only for a unit that is
            // actually LEAVING. A scripted item stays put; vacating its cell
            // would make it hop.
            if (!Equip::IsWearOrConsume(obj)) {
                RequestRebuild();
                return true;
            }
            // right-click: weapons go to the right hand, armour has only one
            // place to go -- and a LIGHT (torch) goes to the LEFT. Recording it
            // as right made the "has it landed?" test look for a worn list in
            // the wrong hand, never find one, and the tile flickered until the
            // entry expired. [DOLL] shieldL='Torch'(h2) is the proof.
            int hand = 0;
            if (obj->Is(RE::FormType::Weapon)) hand = 1;
            else if (obj->Is(RE::FormType::Light)) hand = 2;
            NotePendingEquip(obj, uid, sig, hand, key,
                             Equip::EquipCountFor(obj, count), xl);
            g_drainHint = { FormKey(obj), key };   // stackables: who gives one up
            // The optimistic exit, drawn without a full rebuild -- the same
            // partial the deferred click-remove used to run, called straight
            // out because this already runs outside the draw pass.
            if (!TryUseClickPartialRemove(key, obj, /*take=*/0, /*drained=*/false)) {
                SKSE::log::info("[ONEPATH] use partial declined ('{}') -- full rebuild",
                    key);
                RequestRebuild();
            }
            return true;
        }

        // defined with the gold routes below; the whole-tile store needs it
        // for the same reason the fragment route does
        int StoreCoinValueTo(RE::TESObjectREFR* a_dst, Held& a_held);

        bool WholeStore(Held& a_held)
        {
            // dropped on the container window = STORE (coins excluded —
            // mirror artefacts). A stack (>1) opens the quantity slider
            // first. The pouch stores fine (gold travels via the sink).
            // F7 (kLoot/kSteal): the drop CELL is honoured — empty cell =
            // stored right there; occupied cell = swap (the stored item
            // takes the occupant's spot, the occupant jumps to the cursor).
            const RE::FormID fid = a_held.obj->GetFormID();
            // ★(1.3.2a) a whole COIN TILE dropped on a shelf POUCH cell
            // deposits into its slot (the pinned fragment's grammar, see
            // GoldOnPartnerPouch); anywhere else coins keep their old cancel
            if (a_held.coinValue > 0 &&
                GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) {
                // P2/3-5: same two destinations as the fragment's route above --
                // a pouch cell under the cursor, else the container itself.
                // ★The pin comes home first -- see StoreCoinValueTo, and the
                // report it exists for. A whole COIN TILE can be a pinned purse
                // just as a fragment can (dragging gold out of a chest makes
                // one), so this branch was losing the store for the same reason.
                int moved = LootBarter::DepositOnHoveredPouch(a_held.coinValue);
                if (moved > 0) {
                    GoldCoins::DebitLedger(moved);
                } else if (auto* dst = LootBarter::Partner()) {
                    moved = StoreCoinValueTo(dst, a_held);
                    // ★S-1: gold is a stack with a big cap on the shelf side, so
                    // it takes the square it was dropped on exactly as an ingot
                    // does. The form that lands over there is Gold001, not our
                    // coin tile -- the mirror is this side's business only.
                    if (moved > 0) {
                        const auto sd = LootBarter::QueryStoreDrop();
                        if (sd.onCell && sd.freeSpot) {
                            if (auto* vg = GoldCoins::VanillaGold()) {
                                LootBarter::NoteStoredUnits(vg, moved);
                                LootBarter::PlaceStoredCell(vg, moved,
                                                            sd.col, sd.row, 0);
                            }
                        }
                    }
                }
                if (moved > 0 && moved < a_held.coinValue) {
                    // ★S-G: a partial store keeps the remainder riding (the
                    // record already shrank in StoreCoinValueTo)
                    a_held.coinValue -= moved;
                    if (g_sound) g_sound(a_held.obj, false);
                    return true;
                }
                if (moved > 0) {
                    g_layout.erase(a_held.key);
                    if (g_sound) g_sound(a_held.obj, false);
                    g_held.reset();
                    RequestRebuild();
                    return true;
                }
                // ★Nothing moved. Consume the drop but KEEP the carry: falling
                // through would reach the tail below, which resets the carry --
                // and a coin tile whose store was refused would vanish off the
                // cursor with its cell already gone.
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                return true;
            }
            // (no quest guard: storing CHANGES CONTAINERS -- see PoolIsQuest)
            bool queued = false;   // O-2: did anything actually leave?
            if (!LootBarter::PartnerHasRoomFor(a_held.obj, a_held.count)) {
                // (1.3.3) a follower's pack is 10 x 8 -- keep carrying
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
            } else if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                const auto sd = LootBarter::QueryStoreDrop();   // F7 (dead outside kLoot/kSteal)
                if (a_held.count > 1) {
                    // srcKey rides along: pending-remove fires on CONFIRM (an
                    // immediate erase made the tile jump to the front while
                    // the engine removal was still queued).
                    // F7: an empty drop cell rides the slider as a spot hint
                    if (sd.onCell && sd.freeSpot) {
                        LootBarter::AimStoreAt(a_held.obj, sd.col, sd.row,
                                               HeldInstanceSig(), a_held.rot);
                    }
                    LootBarter::OpenSlider(a_held.obj, a_held.count,
                        LootBarter::XferDir::kStore, a_held.key, 0, a_held.uid, a_held.sig,
                        false, a_held.fav, a_held.xlIdx);
                } else {
                    LootBarter::RequestStore(a_held.obj, a_held.count,
                                             HeldUidOf(a_held.key, a_held.uid), a_held.sig,
                                             a_held.fav, a_held.xlIdx, a_held.key);
                    NotePendingRemove(a_held.obj, a_held.key, a_held.count, a_held.xlIdx);
                    queued = true;   // O-2: this tile really is leaving
                    if (a_held.isBag) {
                        // (1.3.0-D) contents FOLLOW the bag into the chest
                        // (the old E4 reflow spilled them onto the main
                        // board). A tile whose store is refused re-seats via
                        // the rebuild's "bag truly gone" reflow, so nothing
                        // can point at a bag that left without it.
                        StoreBagContents(a_held.key, a_held.obj);
                        g_openBags.erase(a_held.key);
                    }
                    if (sd.onCell) {
                        // `sd.occ != a_held.obj` was a FORM comparison: storing a
                        // dagger onto the container's dagger read as "the same
                        // thing", the swap was skipped, and the stored one
                        // first-fit into some other cell. Two units of a
                        // non-stackable form are DIFFERENT units, so that is a
                        // swap (rule 20) -- exactly what the partner's own
                        // rearrange path already does. Only a genuine stack
                        // merges.
                        const bool merging = sd.occ == a_held.obj &&
                                             EffectiveCap(a_held.obj) > 1;
                        if (sd.occ && !merging) {
                            // rule 4: swap — the stored item takes the
                            // occupant's square, the occupant rides the cursor
                            const auto occ = sd;   // copy: reset invalidates it
                            LootBarter::PlaceStoredCell(a_held.obj, a_held.count,
                                occ.occCol, occ.occRow, a_held.rot,
                                HeldUidOf(a_held.key, a_held.uid), HeldInstanceSig());
                            g_held.reset();
                            // GI24: same as the rearrange swap — the occupant
                            // keeps its identity and its own cell
                            BeginPartnerCarry(occ.occ, occ.occCount, occ.occValue,
                                              -1.0f, -1.0f,
                                              occ.occUid, occ.occXlIdx, occ.occOrd, occ.occRot);
                            LootBarter::NoteCarriedSpot(occ.occSpotKey);
                            RequestRebuild();
                            return true;
                        }
                        // rule 3: a FREE square = stored right there. A MERGE
                        // needs no square of its own -- the reconcile pours the
                        // arriving units into the cell that is already showing
                        // them, which is what merging means.
                        if (sd.freeSpot) {
                            LootBarter::PlaceStoredCell(a_held.obj, a_held.count,
                                sd.col, sd.row, a_held.rot,
                                HeldUidOf(a_held.key, a_held.uid), HeldInstanceSig());
                        }
                    }
                }
            }
            // ★ONE PATH / O-2: a SYNTHETIC carry never reached the screen,
            // so the board is still showing this tile -- the opposite of a
            // drag, which stopped drawing it at the lift. Two consequences
            // meet here. A store that really happened has to take the tile off
            // the display itself, by the same partial the click has always
            // used (the layout was already drained by NotePendingRemove, hence
            // drained=true). And a store that did NOT happen -- no room, or a
            // slider still waiting for its number -- needs no repaint at all,
            // because nothing changed. That second half is why routing the
            // click here costs no rebuilds it did not already cost.
            const bool                transient = a_held.transient;
            const std::string         key = a_held.key;
            RE::TESBoundObject* const obj = a_held.obj;
            const int                 count = a_held.count;
            const bool hadStash = g_stash && g_stash->key == a_held.key;   // ★S2
            g_held.reset();
            if (!transient) {
                // ★S2: a drag's tile left the board at the lift; the stash is
                // all that remains. A store that queued spends it (confirm /
                // expiry own the CELL from here -- the two-phase drop); one
                // that did not puts the tile straight back. One tile either
                // way; unproven shapes re-derive exactly as before.
                if (queued && hadStash) {
                    DiscardStash("stored");
                } else if (!queued && UnstashTileHome()) {
                    // refused or slider pending: back home, nothing else moved
                } else {
                    RequestRebuild();
                }
            } else if (queued &&
                       !TryUseClickPartialRemove(key, obj, count, /*drained=*/true)) {
                RequestRebuild();
            }
            return true;
        }

        bool WholeSell(Held& a_held)
        {
            // SELL by dragging onto the merchant window. Coins excluded;
            // stack -> slider; single sells if the merchant can afford.
            // Bags/pouch sell only this way (right-click = manage). The
            // bag's contents reflow to main only here, on the real sale.
            const RE::FormID fid = a_held.obj->GetFormID();
            int queued = 0;   // O-3: how many units actually left
            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                const int val = TileValue(a_held.obj,                          // GI43
                    HeldUidOf(a_held.key, a_held.uid), a_held.sig);
                if (a_held.quest) {   // Phase 7: can't sell
                    Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                } else if (!LootBarter::MerchantBuys(a_held.obj, a_held.stolen)) {
                    Sfx::FailNote(Lang::T(Lang::Str::MerchantWontBuy));   // Phase 6: category / stolen
                } else if (a_held.count > 1) {
                    // srcKey rides along: pending-remove fires on CONFIRM
                    LootBarter::OpenSlider(a_held.obj, a_held.count,
                        LootBarter::XferDir::kSell, a_held.key, val, a_held.uid, a_held.sig,
                        false, a_held.fav);
                } else {
                    const int total = val > 0 ? LootBarter::SellPrice(a_held.obj, val) : 0;
                    if (total > 0 && LootBarter::MerchantGold() < total) {
                        Sfx::FailNote(Lang::T(Lang::Str::MerchantNoGold));
                    } else if (a_held.fav) {
                        // ★ONE PATH / O-3: THE STAR ASKS FIRST, on both roads.
                        // This popup lived only on the right-click, so dragging
                        // a favourite onto the merchant sold it outright while
                        // clicking the same item asked -- one item, two answers,
                        // and the silent one was the dangerous half. Unifying
                        // the paths is what made the difference visible; the
                        // safer answer wins.
                        LootBarter::AskSellConfirm(a_held.obj, 1, total, val, a_held.key,
                                                   a_held.uid, a_held.sig,
                                                   a_held.fav, a_held.xlIdx);
                    } else {
                        LootBarter::RequestSell(a_held.obj, 1, total, val, a_held.uid, a_held.sig,
                                                a_held.fav, a_held.xlIdx, a_held.key);
                        NotePendingRemove(a_held.obj, a_held.key, 1, a_held.xlIdx);
                        queued = 1;   // O-3: one unit really is leaving
                        if (a_held.isBag) {   // contents reflow to main on sale (E4)
                            g_openBags.erase(a_held.key);
                            for (auto& [k, le] : g_layout) {
                                if (le.bag == a_held.key) le.bag.clear();
                            }
                        }
                    }
                }
            }
            // ★ONE PATH / O-3: same tail as the store's -- a synthetic carry
            // never reached the screen, so a sale that happened takes its own
            // unit off the display, and a sale that did not (no gold, a slider,
            // a star waiting on its popup) leaves a board that never changed.
            const bool                transient = a_held.transient;
            const std::string         key = a_held.key;
            RE::TESBoundObject* const obj = a_held.obj;
            const int                 held = a_held.count;
            const bool hadStash = g_stash && g_stash->key == a_held.key;   // ★S2
            g_held.reset();
            if (!transient) {
                // ★S2: same shape as the store tail -- a WHOLE sale spends the
                // stash, a refusal (or a slider / the star's popup) puts the
                // tile back; a partial sale re-derives as before.
                if (queued >= held && queued > 0 && hadStash) {
                    DiscardStash("sold");
                } else if (queued <= 0 && UnstashTileHome()) {
                    // back home, nothing else moved
                } else {
                    RequestRebuild();
                }
            } else if (queued > 0 &&
                       !TryUseClickPartialRemove(key, obj, queued, /*drained=*/true)) {
                RequestRebuild();
            }
            return true;
        }

        // ---- F6b: reverse pickpocket (planting items on the mark) ----
        bool WholePickStore(Held& a_held)
        {
            const RE::FormID fid = a_held.obj->GetFormID();
            if (a_held.quest) {   // planted = lost on a wandering mark
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
            } else if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                // coins are mirror artefacts (gold planting isn't supported);
                // everything else rolls the engine attempt on the Tick.
                // Pending-remove is noted on the WIN inside the Tick.
                if (a_held.count > 1) {
                    LootBarter::OpenSlider(a_held.obj, a_held.count,
                        LootBarter::XferDir::kPickStore, a_held.key, 0, a_held.uid, a_held.sig,
                        false, a_held.fav);
                } else {
                    LootBarter::RequestPickStore(a_held.obj, 1, a_held.uid, a_held.sig, a_held.key,
                                                 a_held.fav, a_held.xlIdx);
                }
            }
            // ★ONE PATH / O-3: NOTHING has left yet on this road -- the plant
            // is a ROLL, and the pending-remove is noted on the win, inside the
            // Tick. So a synthetic carry leaves a board that has not changed,
            // and repainting it would be a rebuild spent on nothing. (The old
            // click paid exactly that one, every plant.)
            const bool transient = a_held.transient;
            g_held.reset();
            if (!transient) RequestRebuild();
            return true;
        }

        bool FragPickStore(Held& a_held)
        {
            // a split fragment plants its quantity (no tile key: the source
            // count already dropped at split time; a lost roll force-closes
            // and the reconciler restores the units)
            if (QuestFragBlocked(a_held)) return true;   // the mark walks off with it
            LootBarter::RequestPickStore(a_held.obj, a_held.count, a_held.uid, a_held.sig, {},
                                         a_held.fav, a_held.xlIdx);
            g_held.reset();
            return true;
        }

        bool WholeCancelNonNormal(Held&)
        {
            // loot/barter mode: dropping into empty space must NOT discard
            // (accidental loss during looting) — cancel back to the spot.
            if (LootBarter::CurrentMode() == LootBarter::Mode::kNormal) return false;
            g_held.reset();
            RequestRebuild();
            return true;
        }

        bool WholeToVoid(Held& a_held)
        {
            // C5: dropped outside every window -> discard to the world. The
            // pouch drops like any item; its stored gold travels with it
            // (container sink handles the ledger).
            const RE::FormID fid = a_held.obj->GetFormID();
            if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) {
                // G2/G4: the coin's VALUE drops as a real Gold001 world ref;
                // a pinned purse returns to walking first so the debit lands.
                // The mirror then removes the coin tile.
                GoldCoins::DropAsGold(a_held.coinValue);
                g_layout.erase(a_held.key);   // free this coin's slot
                g_held.reset();
                RequestRebuild();
            } else if (a_held.quest) {
                // Phase 7: quest items can't be discarded — cancel back to
                // their spot (★S2: one tile home; unproven re-derives).
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                g_held.reset();
                if (!UnstashTileHome()) RequestRebuild();
            } else {
                g_layout.erase(a_held.key);
                if (a_held.isBag) {
                    // dropping a bag releases its contents to main (E4)
                    g_openBags.erase(a_held.key);
                    for (auto& [k, le] : g_layout) {
                        if (le.bag == a_held.key) le.bag.clear();
                    }
                }
                // G3: a carried tile drops ITS units, not the whole form
                // stack (arrows 250 = tiles of 100/100/50)
                if (g_dropWorld) {
                    g_dropWorld(a_held.obj, a_held.count,   // GI36
                        ResolveExitUnit(a_held.obj, a_held.uid, a_held.sig, a_held.count,
                                        a_held.fav ? a_held.count : 0, a_held.xlIdx));
                }
                // ★S2: the whole carry left for the world; the stash was its
                // last trace. A bag drop reflows its contents -- that stays a
                // rebuild (the contents' tiles all move); anything unstashed
                // re-derives as before.
                const bool wasBag = a_held.isBag;
                const bool hadStash = g_stash && g_stash->key == a_held.key;
                g_held.reset();
                if (!wasBag && hadStash) {
                    DiscardStash("dropped to world");
                } else {
                    RequestRebuild();
                }
            }
            return true;
        }

        // ---- F2: trash intake / eviction helpers ----

        // live favorite state straight from the engine entry (Held carries
        // no fav flag) — same walk as ToggleFavorite
        // protection rules — false blocks the intake (note played)
        bool TrashIntakeAllowed(RE::TESBoundObject* a_obj, bool a_quest,
                                const std::string& a_bagKey, bool a_isBag)
        {
            const RE::FormID fid = a_obj->GetFormID();
            if (a_quest) {
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                return false;
            }
            // gold coins / pouch / raw gold: the discard path stays R / void
            // drop only (two delete paths for gold invite accidents)
            if (GoldCoins::IsCoinForm(fid) || a_obj->IsGold()) {
                Sfx::FailNote(Lang::T(Lang::Str::TrashGoldBlocked));
                return false;
            }
            // ★P2/3-1 creates this hazard and closes it in the same breath.
            // A worn bag now HAS a tile, and a tile can be dragged into the
            // trash -- which would delete something the doll is still wearing.
            // Nothing else on the board can be worn, so nothing else needed
            // this guard before.
            if (a_isBag) {
                if (auto* p2 = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* e2 = LiveEntry(p2, a_obj); e2 && e2->IsWorn()) {
                        Sfx::FailNote(Lang::T(Lang::Str::TrashWornBlocked));
                        return false;
                    }
                }
            }
            if (a_isBag) {   // only an EMPTY bag may be trashed
                for (const auto& [k, le] : g_layout) {
                    if (le.bag == a_bagKey) {
                        Sfx::FailNote(Lang::T(Lang::Str::TrashBagBlocked));
                        return false;
                    }
                }
            }
            return true;
        }

        // can a_mask first-fit the 6x4 trash board with the CURRENT parked
        // layout (g_layout-based so an eviction frees space immediately)?
        bool TrashHasRoomFor(const Mask& a_mask)
        {
            bool occ[kTrashRows][kTrashCols] = {};
            for (const auto& [k, le] : g_layout) {
                if (le.bag != kTrashKey || le.col < 0) continue;
                if (g_held && k == g_held->key) continue;   // carried: cell free
                // parked tiles are always live g_items (rebuilt while open)
                const Mask* m = nullptr;
                for (const auto& it : g_items) {
                    if (it.key == k) { m = &it.mask; break; }
                }
                const int mw = m ? m->w : 1, mh = m ? m->h : 1;
                for (int y = 0; y < mh; ++y) {
                    for (int x = 0; x < mw; ++x) {
                        if (m && !m->rows[y][x]) continue;
                        const int r = le.row + y, c = le.col + x;
                        if (r >= 0 && r < kTrashRows && c >= 0 && c < kTrashCols) {
                            occ[r][c] = true;
                        }
                    }
                }
            }
            for (int r = 0; r + a_mask.h <= kTrashRows; ++r) {
                for (int c = 0; c + a_mask.w <= kTrashCols; ++c) {
                    bool ok = true;
                    for (int y = 0; ok && y < a_mask.h; ++y) {
                        for (int x = 0; ok && x < a_mask.w; ++x) {
                            if (a_mask.rows[y][x] && occ[r + y][c + x]) ok = false;
                        }
                    }
                    if (ok) return true;
                }
            }
            return false;
        }

        // CONFIRM a parked tile's deletion: the layout entry drains via the
        // shared pending-remove bridge and the engine RemoveItem is queued
        // for the Tick (never mid-frame). FIFO order is caller-managed.
        void ConfirmTrashDelete(const std::string& a_key)
        {
            const auto li = g_layout.find(a_key);
            if (li == g_layout.end() || li->second.bag != kTrashKey) return;
            // ★Read the binding BEFORE NotePendingRemove drains the entry --
            // and read it from the SLOT, not from the key, which no longer says.
            const std::uint16_t delUid = li->second.uid;
            const std::uint16_t delSig = li->second.sig;
            RE::TESBoundObject* obj = nullptr;
            int count = (std::max)(1, li->second.count);
            bool fav = false;   // GI36
            for (const auto& it : g_items) {
                if (it.key == a_key) {
                    obj = it.obj; count = it.count; fav = it.fav;
                    break;
                }
            }
            if (obj) {
                // ★The park recorded WHICH unit this tile stands for; the
                // removal has to carry it too, or the exclusion falls back
                // to matching by signature -- and a signature that moved
                // since the park leaves the walk with nothing, so the unit
                // is never taken off the board and its sibling is reborn
                // in the first free cell. (Measured: pool ~B825 asked for
                // ~1D81, members=2 slots=1, one tile NEW at first-fit.)
                const auto txi = g_trashXl.find(a_key);
                const int  txl = txi == g_trashXl.end() ? -1 : txi->second;
                NotePendingRemove(obj, a_key, count, txl);   // drains + erases the entry
                // B4-3c: the ledger entry opens HERE, at the commit -- the
                // engine call runs on the Tick (ProcessTrashDeletes), and the
                // window between the two is the ledger's to cover now.
                Ledger::Submit(obj->GetFormID(), -count, "trash", delUid, delSig,
                               a_key);
                // Field-wise on purpose (§10-6): this struct has grown once.
                TrashDelete td;
                td.form = obj->GetFormID();
                td.count = count;
                td.uid = delUid;
                td.sig = delSig;
                td.fav = fav;
                td.xlIdx = txl;
                td.key = a_key;
                g_trashDeleteQ.push_back(std::move(td));
            } else {
                g_layout.erase(a_key);   // unresolvable tile: just free the cell
            }
            g_trashReturn.erase(a_key);
            g_trashXl.erase(a_key);
            RequestRebuild();
        }

        // evict oldest parked tiles until a_mask fits (FIFO, per spec)
        void TrashMakeRoomFor(const Mask& a_mask)
        {
            bool evicted = false;
            while (!TrashHasRoomFor(a_mask) && !g_trashOrder.empty()) {
                const std::string victim = g_trashOrder.front();
                g_trashOrder.pop_front();
                ConfirmTrashDelete(victim);
                evicted = true;
            }
            // ★S1 decline: an eviction takes a parked TILE off the display,
            // and this can run mid-draw (the right-click park) -- the rebuild
            // stays its display path. Rare: only a FULL bin evicts.
            if (evicted) RequestRebuild();
        }

        // park a KEYED tile (whole-tile intake + the favorite-ask resume).
        // a_col/a_row -1 = first-fit inside the trash board.
        // ★a_rot: the quarter-turn the item was carrying when it was binned.
        // Omitted, PlaceTile leaves the entry's rotation alone -- so a tile
        // dropped sideways was checked as 4x1 and then drawn as 1x4, straight
        // over its neighbours, and the two room tests disagreed from then on.
        void ParkKeyInTrash(const std::string& a_key, RE::TESBoundObject* a_obj,
                            int a_count, int a_col, int a_row, int a_xlIdx = -1,
                            std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                            int a_rot = -1)
        {
            LayoutEntry prev;   // pre-park spot -> right-click restore target
            prev.col = -1;
            prev.row = -1;
            if (const auto li = g_layout.find(a_key); li != g_layout.end()) prev = li->second;
            g_trashReturn[a_key] = prev;
            // remember which unit this park refers to (see g_trashXl)
            g_trashXl[a_key] = a_xlIdx;
            PlaceTile(a_key, a_col, a_row, kTrashKey, a_count, a_rot);
            // ★★★STAMP THE IDENTITY. A carry lifted OFF THE DOLL has no layout
            // entry to inherit from -- a worn unit owns no cell -- so PlaceTile
            // minted a blank one and the parked slot claimed uid 0 / sig 0: a
            // plain unit, whatever was actually binned. Everything downstream
            // believed it. The off-board record said "plain", so the exclusion
            // took a plain dagger off the board; the tile drew as a plain
            // dagger, because that is what its slot said it was holding.
            //
            // A carry off the GRID was unaffected -- its key already existed
            // with the right hints and PlaceTile leaves them alone. That is
            // exactly the split the report described: "from the inventory it
            // was fine, straight from the doll it was not".
            {
                auto& le = g_layout[a_key];
                le.uid   = a_uid;
                le.sig   = a_sig;
                le.xlIdx = a_xlIdx;
            }
            g_trashOrder.push_back(a_key);
            g_openBags.erase(a_key);   // a parked (empty) bag closes its window
            if (g_sound && a_obj) g_sound(a_obj, false);
            // ★S1: one tile moves into the trash view. A DRAG brings a stash
            // (the tile left the board at lift) and seats it directly; a
            // right-click's tile is still standing on the board mid-draw, so
            // its move is queued for FinishFrame. Any miss rebuilds.
            if (g_stash && g_stash->key == a_key) {
                const auto& le = g_layout[a_key];
                if (!UnstashTileTo(kTrashKey, le.col, le.row, le.rot, a_count)) {
                    RequestRebuild();
                }
            } else {
                g_viewMoveQ.push_back({ a_key, kTrashKey, a_col, a_row });
            }
        }

        // ---- F2: drop handlers ----
        bool WholeIntoTrash(Held& a_held)
        {
            // a tile already parked = in-trash reposition: normal grid grammar
            const auto cur = g_layout.find(a_held.key);
            if (cur != g_layout.end() && cur->second.bag == kTrashKey) return false;

            if (!TrashIntakeAllowed(a_held.obj, a_held.quest, a_held.key, a_held.isBag)) {
                g_held.reset();   // blocked: snaps back (layout entry intact)
                if (!UnstashTileHome()) RequestRebuild();   // ★S1: one tile home
                return true;
            }
            const int col = g_target.valid ? g_target.col : -1;
            const int row = g_target.valid ? g_target.row : -1;
            if (a_held.fav) {   // GI36: the carry brought the answer with it
                // favorite: confirm first — the tile snaps back while asking
                g_trashAsk = { true, a_held.obj, a_held.key, a_held.count, col, row,
                               a_held.xlIdx, a_held.uid, a_held.sig, a_held.rot };
                Sfx::SelectOn();
                g_held.reset();
                if (!UnstashTileHome()) RequestRebuild();   // ★S1: one tile home
                return true;
            }
            TrashMakeRoomFor(a_held.mask);
            ParkKeyInTrash(a_held.key, a_held.obj, a_held.count, col, row,
                           a_held.xlIdx, a_held.uid, a_held.sig, a_held.rot);
            g_held.reset();
            return true;
        }

        // ★★★THE OPEN BIN CHANGES WHAT A RIGHT-CLICK MEANS, and only on the
        // player's own surfaces. Binning one item at a time by dragging is the
        // slowest thing this UI asks of anyone -- lift, travel to the window,
        // drop, repeat -- and the trash being OPEN is already the player
        // saying "I am throwing things away now". So while it is up, a
        // right-click on the board or in a bag sends the tile straight in.
        //
        // ★It reuses the drag's own guards rather than restating them: quest
        // items, gold and coins, a worn or non-empty bag all refuse here
        // exactly as they refuse a drop, with the same note. A favourite still
        // asks first -- the confirmation exists because the mark means "I chose
        // this on purpose", and a faster gesture is a better reason to keep it,
        // not a reason to drop it.
        //
        // ★★The PARTNER window and the equipment doll are untouched, and not
        // by a condition: they own their own right-click handlers
        // (LootBarter.cpp, Equip.cpp) and never reach this code. Nothing about
        // a container's or a merchant's board is ours to bin.
        bool RightClickIntoTrash(const Item& a_it)
        {
            if (!g_trashOpen || !a_it.obj) return false;
            if (!TrashIntakeAllowed(a_it.obj, a_it.quest, a_it.key, a_it.def.bag != 0)) {
                return true;   // refused, and it has already said why
            }
            const LayoutEntry le = g_layout.count(a_it.key) ? g_layout[a_it.key]
                                                            : LayoutEntry{};
            if (a_it.fav) {
                // col/row -1: the bin first-fits it, there being no drop point
                g_trashAsk = { true, a_it.obj, a_it.key, a_it.count, -1, -1,
                               le.xlIdx, a_it.uid, a_it.sig, a_it.rot };
                Sfx::SelectOn();
                // ★S1: only a popup opened -- no tile moved, nothing to repaint
                return true;
            }
            TrashMakeRoomFor(a_it.mask);
            ParkKeyInTrash(a_it.key, a_it.obj, a_it.count, -1, -1,
                           le.xlIdx, a_it.uid, a_it.sig, a_it.rot);
            return true;
        }

        bool StackFragIntoTrash(Held& a_held)
        {
            // a split fragment parks as its own NEW tile (no favorite ask:
            // the fragment can't survive the popup round-trip — reset would
            // re-absorb it into the source stack)
            if (!TrashIntakeAllowed(a_held.obj, a_held.quest, {}, false)) {
                g_held.reset();
                return true;
            }
            TrashMakeRoomFor(a_held.mask);
            const std::string nk = NewTileForHeld(a_held);
            g_trashReturn[nk] = LayoutEntry{ -1, -1, {}, 0 };   // restore = first-fit
            g_trashXl[nk] = a_held.xlIdx;
            {   // field-wise: a whole-struct init here would wipe the binding
                // NewTileForHeld just recorded, and the trash queue needs it to
                // name the right unit when the deletion is confirmed.
                auto& le = g_layout[nk];
                le.col   = g_target.valid ? g_target.col : -1;
                le.row   = g_target.valid ? g_target.row : -1;
                le.bag   = kTrashKey;
                le.count = a_held.count;
            }
            g_trashOrder.push_back(nk);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            return true;
        }

        bool GoldFragTrashBlock(Held&)
        {
            Sfx::FailNote(Lang::T(Lang::Str::TrashGoldBlocked));
            return true;   // note played; the pin keeps riding the cursor
        }

        // ★★P2/3-5: GOLD DOES NOT REMEMBER A CELL, and trying to make it
        // was a mistake worth writing down.
        //
        // Storing an item is a promise about a THING: this dagger goes in that
        // square. Gold in a container is not a thing, it is an AMOUNT -- one
        // Gold001 stack the board bands into capfuls purely so it can be read.
        // Those bands have no identity: deposit 300 more and the last one is
        // worth 800 instead of 500, because the total moved.
        //
        // Noting a drop cell on top of that made the first band jump to
        // wherever the newest coins landed, so every extra deposit rewrote the
        // amounts AND the positions of gold that was already in the chest (user
        // report). The bands sit in ordinal order instead, which is stable: the
        // early ones stay full and stay put, and only the last one changes --
        // what a growing pile should look like.
        // ★P2/3-5: gold dropped on a partner window. Two destinations, in
        // order: a POUCH cell under the cursor takes it into that pouch's slot
        // (the older grammar), and anything else hands it to the CONTAINER
        // itself -- a chest, a follower's pack -- which is what "I want to keep
        // my money in the warehouse" has always meant and never had a road to.
        //
        // ★The merchant is not on this list and needs no test for it: the row
        // that reaches here is kPartnerLoot, which only matches a container or
        // a follower. Selling gold to a shopkeeper is not a thing.
        // ★★A PINNED PURSE HAS TO COME HOME BEFORE IT CAN BE SPENT, and this is
        // the one line the whole gold-into-a-chest feature turned on.
        //
        // Pinning SUBTRACTS a value from walking gold -- that is what a pin is
        // -- so a purse's own value is, by definition, not in the pool
        // StoreToContainer measures against. With 10,000 G banked in a pouch
        // and 663 G riding as a pin, walking gold is exactly 0, the store
        // clamps to 0 and moves nothing, and the caller then unpins the purse
        // and erases its cell for a transfer that never happened: the 663
        // reappears as an ordinary coin tile in the first free square. That is
        // the "sometimes it does not store, and it jumps to an empty cell"
        // report, and it needed the gold to have been DRAGGED OUT of a
        // container first -- which is what makes it a pin.
        //
        // The pouch route next door has always known this ("the pin returns to
        // walking first so the pouch can draw from it"). Same move here, with
        // the same restore when the destination refuses.
        int StoreCoinValueTo(RE::TESObjectREFR* a_dst, Held& a_held)
        {
            // ★S-G: the record IS the amount. A refusal leaves it untouched;
            // a partial store shrinks it (the caller keeps the carry when it
            // sees moved < value); a full one is erased by the caller.
            const int moved = GoldCoins::StoreToContainer(a_dst, a_held.coinValue);
            if (moved > 0 && moved < a_held.coinValue) {
                SetCoinRecord(a_held.key, a_held.coinValue - moved);
            }
            return moved;
        }

        bool GoldOnPartnerPouch(Held& a_held)
        {
            if (a_held.coinValue <= 0) return false;
            int moved = LootBarter::DepositOnHoveredPouch(a_held.coinValue);
            if (moved > 0) {
                GoldCoins::DebitLedger(moved);   // the pouch route debits here
            } else if (auto* dst = LootBarter::Partner()) {
                moved = StoreCoinValueTo(dst, a_held);
            } else {
                return false;
            }
            if (moved <= 0) {
                // refused: the purse is intact and keeps riding the cursor
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                return true;
            }
            const int rem = a_held.coinValue - moved;
            if (rem > 0) {   // ★S-G: a partial deposit keeps riding
                SetCoinRecord(a_held.key, rem);
                a_held.coinValue = rem;
                if (g_sound) g_sound(a_held.obj, false);
                return true;
            }
            g_layout.erase(a_held.key);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            RequestRebuild();
            return true;
        }

        // ★(1.5.x) a coin dropped on a POUCH INSIDE AN OPEN SHELF-BAG WINDOW
        // deposits into that entry -- the bundled twin of GoldOnPartnerPouch.
        // The bag window records the hovered pouch entry per frame; this row
        // self-gates on that record, so it sits at the head of both coin
        // tables and is inert everywhere else. A full pouch keeps the carry
        // riding (consuming the drop): falling through would reach the void
        // row and drop the purse on the floor.
        bool GoldOnBundlePouch(Held& a_held)
        {
            if (a_held.coinValue <= 0) return false;
            if (!LootBarter::IsBundlePouchHovered()) return false;
            const int moved =
                LootBarter::DepositOnHoveredBundlePouch(a_held.coinValue);
            if (moved <= 0) {
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                return true;
            }
            GoldCoins::DebitLedger(moved);   // same settle as the cell route
            const int rem = a_held.coinValue - moved;
            if (rem > 0) {   // partial deposit keeps riding (S-G record shrinks)
                SetCoinRecord(a_held.key, rem);
                a_held.coinValue = rem;
                if (g_sound) g_sound(a_held.obj, false);
                return true;
            }
            g_layout.erase(a_held.key);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            RequestRebuild();
            return true;
        }

        // ★(1.5.x) a player coin dropped on an open shelf-bag window (not on
        // a pouch entry -- that row runs first): the physical Septims store
        // into the container (StoreCoinValueTo, pin-home and record shrink
        // included), and the bag window books them as a gold ENTRY so they
        // arrive inside the bag instead of surfacing as a loose shelf cell.
        bool GoldIntoShelfBag(Held& a_held)
        {
            if (a_held.coinValue <= 0) return false;
            if (LootBarter::IsBundlePouchHovered()) return false;   // deposit row
            if (!LootBarter::IsShelfBagHovered()) return false;
            auto* dst = LootBarter::Partner();
            if (!dst) return false;
            const int moved = StoreCoinValueTo(dst, a_held);
            if (moved <= 0) {
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                return true;
            }
            LootBarter::IntakeGoldEntry(moved);
            if (moved < a_held.coinValue) {
                a_held.coinValue -= moved;
                if (g_sound) g_sound(a_held.obj, false);
                return true;
            }
            g_layout.erase(a_held.key);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            RequestRebuild();
            return true;
        }

        struct DropRoute
        {
            DropWhere where;
            bool (*handler)(Held&);
        };

        constexpr DropRoute kPartnerHeldRoutes[] = {
            { DropWhere::kAlways, DropPartnerHeld },
        };
        // gold fragment: priority mirrors the stack case; (6) chrome /
        // unresolved = keep carrying (no consuming row)
        constexpr DropRoute kGoldFragRoutes[] = {
            // (1.5.x) hovered bundled pouch wins outright -- self-gating
            { DropWhere::kAlways, GoldOnBundlePouch },
            { DropWhere::kAlways, GoldIntoShelfBag },   // (1.5.x) into an open bag
            { DropWhere::kTrashArea, GoldFragTrashBlock },   // F2: gold never parks
            { DropWhere::kEmptyCell, GoldFragOnEmptyCell },
            { DropWhere::kBlockerSingle, GoldFragOnBlocker },
            { DropWhere::kPartnerLoot, GoldOnPartnerPouch },   // (1.3.2a) deposit
            { DropWhere::kVoid, GoldFragToVoid },
        };
        constexpr DropRoute kStackFragRoutes[] = {
            { DropWhere::kTrashArea, StackFragIntoTrash },   // F2 (before kEmptyCell)
            { DropWhere::kEmptyCell, StackFragOnEmptyCell },
            { DropWhere::kBlockerSingle, StackFragOnBlocker },
            // ★No blanket quest cancel here any more. A kAlways row fires for
            // EVERY destination, so a split quest fragment could not reach the
            // store row below -- the fragment half of feedback ⑤⑭. The guard
            // now sits in the rows that actually lose the item (sell, void,
            // plant); the trash row has always asked TrashIntakeAllowed.
            { DropWhere::kPartnerLoot, StackFragStore },
            { DropWhere::kPartnerBarter, StackFragSell },
            { DropWhere::kPartnerPickpocket, FragPickStore },   // F6b
            { DropWhere::kVoid, StackFragToVoid },
        };
        constexpr DropRoute kWholeTileRoutes[] = {
            // (1.5.x) a whole coin tile on a bundled pouch: same head row as
            // the fragment table (coinValue gates it -- inert for gear)
            { DropWhere::kAlways, GoldOnBundlePouch },
            { DropWhere::kAlways, GoldIntoShelfBag },   // (1.5.x) into an open bag
            { DropWhere::kEquipSlot, WholeOnEquipSlot },
            { DropWhere::kTrashArea, WholeIntoTrash },   // F2 (falls through when
                                                         // repositioning INSIDE)
            { DropWhere::kCellArea, WholeOnCellArea },
            { DropWhere::kPartnerLoot, WholeStore },
            { DropWhere::kPartnerBarter, WholeSell },
            { DropWhere::kPartnerPickpocket, WholePickStore },   // F6b
            { DropWhere::kAlways, WholeCancelNonNormal },
            { DropWhere::kVoid, WholeToVoid },
        };

        void ResolveDrop(Held& a_held)
        {
            const DropRoute* rows = nullptr;
            size_t n = 0;
            bool alwaysRebuild = false;   // fragments rebuild on every attempt
            if (a_held.fromPartner) {
                rows = kPartnerHeldRoutes;
                n = std::size(kPartnerHeldRoutes);
            } else if (a_held.preSplit && a_held.coinValue >= 0) {
                rows = kGoldFragRoutes;
                n = std::size(kGoldFragRoutes);
                alwaysRebuild = true;
            } else if (a_held.preSplit) {
                rows = kStackFragRoutes;
                n = std::size(kStackFragRoutes);
                alwaysRebuild = true;
            } else {
                rows = kWholeTileRoutes;
                n = std::size(kWholeTileRoutes);
            }
            for (size_t i = 0; i < n; ++i) {
                if (!DropWhereMatches(rows[i].where)) continue;
                if (rows[i].handler(a_held)) break;
            }
            // no consuming route: keep carrying (window chrome etc.)
            if (alwaysRebuild) RequestRebuild();
        }

        // ---- ★ONE PATH / O-0: the synthetic carry -------------------------
        //
        // A right-click is a QUICK transfer, not a DIFFERENT kind of transfer.
        // The drag path earns its correctness from one property -- at every
        // instant exactly one place owns the unit, and a failure hands it back
        // to where it came from -- and the click path has never had it: each
        // branch improvises the same job with a ledger of its own (the drain
        // hint, the click-remove, the acting spot). This is the plumbing that
        // lets a click borrow the drag's road instead.
        //
        // O-0 builds it and moves NOTHING. Nothing calls the two functions
        // below yet; the branches arrive one at a time in O-1..O-4, so each
        // move is a change small enough to judge on its own.

        // Lift a displayed tile into a carry the player never sees. Silent by
        // design: the route that consumes it plays whatever sound belongs to
        // the destination, and a pickup clink for a carry that lasted one call
        // would be a sound with no gesture behind it.
        [[nodiscard]] bool BeginSyntheticCarry(const Item& a_it)
        {
            if (g_held || !a_it.obj) return false;
            g_held = Held{ a_it.key, a_it.obj, a_it.mask, a_it.count,
                           a_it.def.bag != 0, a_it.def.scale, 0.0f, 0.0f,
                           /*justPicked=*/false };
            g_held->coinValue = a_it.coinValue;
            g_held->xlIdx = a_it.xlIdx;
            g_held->uid = a_it.uid;
            g_held->sig = a_it.sig;
            g_held->fav = a_it.fav;
            g_held->stolen = a_it.stolen;
            g_held->quest = a_it.quest;
            g_held->SetRot(a_it.rot);
            g_held->transient = true;
            return true;
        }

        // Run one route over a synthetic carry and guarantee the carry is gone
        // afterwards, whatever the route decided.
        //
        // ★A route that returns WITHOUT consuming the carry means "keep
        // carrying" -- the right answer for a drag over window chrome, and no
        // answer at all here, because there is no cursor to keep it on. For a
        // synthetic carry that is a FAILURE, and a failure goes back where it
        // came from: the tile's layout entry was never erased (a carry does not
        // erase, the commit does), so the board redraws it exactly where it was.
        bool RunSyntheticRoute(bool (*a_route)(Held&))
        {
            if (!g_held || !g_held->transient) return false;
            const bool consumed = a_route(*g_held);
            // ★TEST THE FLAG, not merely "is something held". A route is
            // allowed to consume our carry and start a REAL one in its place --
            // a drop onto an occupied slot hands the displaced item to the
            // cursor, and that carry is the player's to put down. Asking only
            // "is g_held set" would call that success story a decline and tear
            // the new carry back off the cursor.
            if (g_held && g_held->transient) {
                // still ours: the route declined. Put it back.
                const std::string key = g_held->key;
                g_held.reset();
                RequestRebuild();
                SKSE::log::info("[ONEPATH] route declined '{}' -- carry returned "
                                "to its cell", key);
                return false;
            }
            return consumed;
        }
    }

    void FinishFrame()
    {
        // ★The pending-equip TTL, freed from Rebuild. Both sweeps for a stuck
        // entry (this TTL and the applied-erase) lived inside Rebuild -- and
        // B4-1's quiet open means an idle board may never rebuild again, so a
        // record orphaned by a rapid swap run (its equip landed, its unit was
        // unequipped again, its worn list vanished before the match-release
        // could fire) hid a unit for the rest of the session (measured:
        // engine=2 worn=0 off=[equipping] drawn=1, held for good). The sweep
        // belongs to the frame, not to the rebuild that may never come.
        // ★S4: a sweep releases suppression entries, and the units they hid
        // need drawing again -- but the sweep KNOWS which forms those are, so
        // it asks the per-form delta first and only an undeliverable form
        // costs the full rebuild. Measured as the #2 rebuild source of the
        // gate-1 session (12 of 85), each repainting the board for one form.
        const auto releaseByForm = [](std::vector<std::string> a_bases) {
            std::sort(a_bases.begin(), a_bases.end());
            a_bases.erase(std::unique(a_bases.begin(), a_bases.end()),
                          a_bases.end());
            for (const auto& b : a_bases) {
                auto* obj = ObjFromBaseKey(b);
                if (!obj || !OnFormDelta(obj->GetFormID())) {
                    RequestRebuild();
                    return;
                }
            }
        };
        if (!g_pendingEquip.empty()) {
            const auto quiet = std::chrono::steady_clock::now() - g_pendingEquipWhen;
            if (quiet > kPendingEquipTTL) {
                SKSE::log::warn("[GRID] pending equip expired ({} units) -- releasing"
                                " (frame sweep)", g_pendingEquip.size());
                std::vector<std::string> bases;
                bases.reserve(g_pendingEquip.size());
                for (const auto& u : g_pendingEquip) bases.push_back(u.base);
                g_pendingEquip.clear();
                releaseByForm(std::move(bases));
            } else if (quiet > kPendingEquipSettle && !g_held) {
                // ★Prompt retirement for the TAIL of a click run. A LANDED
                // record's job -- bridging the frames between the request and
                // the engine's apply -- ended when the equip event arrived;
                // after that only a rebuild's match-release could retire it,
                // and a quiet board (B4-1) may never rebuild. So the last
                // equips of a spam sat suppressing their units until the TTL
                // (measured: 'expired (2 units)' exactly 2s after the last
                // click -- the "ring appears late" report). Landed + a quiet
                // request stream = settled; the worn list is the engine's own
                // and skipWorn covers the unit from here. Unlanded records
                // keep the full TTL -- their confirmation may still be coming.
                // And no releases while the cursor carries: the rebuild this
                // schedules would land mid-swap-window, the old blink.
                std::vector<std::string> bases;
                for (const auto& u : g_pendingEquip) {
                    if (u.landed) bases.push_back(u.base);
                }
                if (std::erase_if(g_pendingEquip,
                                  [](const OffBoardUnit& u) { return u.landed; }) > 0) {
                    releaseByForm(std::move(bases));   // ★S4: one form, not the board
                }
            }
        }
        // ★ONE PATH / O-0: THE LEAK GUARD. A synthetic carry is born and
        // consumed inside one call, so one reaching here at all is a bug --
        // some route kept it without saying so. Insurance rather than
        // decoration: without this the player would be left holding an
        // invisible item, unable to click anything else, with no gesture that
        // could put it down. Return it to its cell and say so loudly.
        if (g_held && g_held->transient) {
            const std::string key = g_held->key;
            g_held.reset();
            RequestRebuild();
            SKSE::log::error("[ONEPATH] a synthetic carry survived its call "
                             "('{}') -- returned to its cell", key);
        }
        // ★S1: the deferred lift-detach and the queued park/restore moves --
        // BEFORE the drop resolution reads the board, so this frame's drop
        // sees the same freed cells the rebuild used to free.
        if (g_held && g_held->needsDetach) {
            g_held->needsDetach = false;
            if (!StashTileForCarry(g_held->key)) RequestRebuild();
        }
        if (g_wantTrashView) {
            g_wantTrashView = false;
            const bool have = std::any_of(g_views.begin(), g_views.end(),
                [](const View& v) { return v.bagKey == kTrashKey; });
            if (!have && g_trashOpen && !g_views.empty()) {
                View tv;
                tv.bagKey = kTrashKey;
                tv.bagName = Lang::T(Lang::Str::TrashTitle);
                tv.cols = kTrashCols;
                tv.minRows = kTrashRows;
                tv.maxRows = kTrashRows;
                tv.rows = kTrashRows;
                g_views.push_back(std::move(tv));
                SKSE::log::info("[SPACE] trash view opened -- no rebuild");
            }
        }
        RunQueuedViewMoves();
        if (g_held) {
            auto& held = *g_held;
            DrawHeldCursorIcon(held);

            if (held.justPicked) {
                held.justPicked = false;   // the pickup click must not drop (C1)
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                CancelHold();              // C7: cancel -> the origin space
                Sfx::SelectOff();
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Phase 3: the old 6~7-deep else-if chain lives in the
                // drop-route tables above (ResolveDrop) — no consuming
                // route (window chrome etc.) keeps the item carried
                ResolveDrop(held);
            }
        }

        // an item was just dropped/cancelled: the tile that materialises
        // under the cursor must not fire the hover blip
        static bool s_wasHeld = false;
        const bool nowHeld = g_held.has_value();
        if (s_wasHeld && !nowHeld) Sfx::HoverMute(0.35);
        s_wasHeld = nowHeld;

        g_target = {};        // recomputed by next frame's draws
        g_slotTarget.clear();

        // ★B3: the use/equip click's deferred board-side work, BEFORE the
        // rebuild gate -- a failed partial (tile already gone, shapes moved)
        // downgrades to the flag and the very next line runs the rebuild.
        // ★ONE PATH / O-1: the right-click's transfer, run where a drag's is.
        // Lift the named tile into a carry the player never sees and hand it to
        // the USE route; a decline puts it straight back (RunSyntheticRoute).
        // The tile is looked up FRESH -- anything between the click and here
        // (an engine event, a partner transfer) may have taken it, and a click
        // on a tile that no longer exists is simply a click on nothing.
        if (g_clickAction) {
            const ClickAction ca = *g_clickAction;
            g_clickAction.reset();
            const Item* tile = nullptr;
            for (const auto& it : g_items) {
                if (it.key == ca.key) { tile = &it; break; }
            }
            if (tile && BeginSyntheticCarry(*tile)) {
                switch (ca.route) {
                case ClickRoute::kUse:   RunSyntheticRoute(WholeUse); break;
                case ClickRoute::kStore: RunSyntheticRoute(WholeStore); break;
                case ClickRoute::kSell:  RunSyntheticRoute(WholeSell); break;
                case ClickRoute::kPlant: RunSyntheticRoute(WholePickStore); break;
                }
            } else {
                SKSE::log::info("[ONEPATH] click '{}' -- tile gone, nothing to do",
                    ca.key);
            }
        }

        if (g_needRebuild.exchange(false, std::memory_order_acq_rel)) {
            Rebuild();
        }
    }

    // ---- F2: trash window public surface ----

    bool IsTrashOpen() { return g_trashOpen; }

    void ToggleTrash()
    {
        if (g_trashOpen) {
            CloseTrash();
        } else {
            g_trashOpen = true;
            // ★S1: opening the bin ALWAYS starts it empty (the closed-state
            // reflow guarantees no parked entries survive a close), so the
            // view is a fresh empty board -- appended at FinishFrame via the
            // deferral flag, never mid-draw. One frame, same as the deferred
            // rebuild this replaces.
            g_wantTrashView = true;
            Sfx::BagOpen();
        }
    }

    bool CloseTrash()
    {
        if (!g_trashOpen) return false;
        // closing CONFIRMS every parked deletion (spec) — engine removals
        // run on the Tick via the delete queue
        std::vector<std::string> keys;
        for (const auto& [k, le] : g_layout) {
            if (le.bag == kTrashKey) keys.push_back(k);
        }
        for (const auto& k : keys) ConfirmTrashDelete(k);
        g_trashOrder.clear();
        g_trashReturn.clear();
        g_trashXl.clear();
        g_trashAsk = {};
        g_trashOpen = false;
        RequestRebuild();
        Sfx::BagClose();
        return true;
    }

    bool SearchMatches(const char* a_name)
    {
        if (g_search.empty()) return true;          // no term: everything matches
        if (!a_name || !*a_name) return false;
        std::string low(a_name);
        // ASCII fold only — and that is enough. Korean, Japanese and Chinese
        // have no case to fold, so those names compare exactly as typed; Latin
        // names get the case-insensitive match players expect from a search box.
        std::transform(low.begin(), low.end(), low.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return low.find(g_search) != std::string::npos;
    }

    namespace
    {
        void RecomputeSearch()
        {
            g_searchHit.clear();
            g_searchVersion = g_boardVersion;
            if (g_search.empty()) return;
            for (const auto& it : g_items) {
                if (SearchMatches(it.obj ? it.obj->GetName() : nullptr)) {
                    g_searchHit.insert(it.key);
                }
            }
        }
    }

    bool SearchActive() { return !g_search.empty(); }
    const std::string& SearchTerm() { return g_search; }

    void SetSearch(const char* a_term)
    {
        std::string low = a_term ? a_term : "";
        std::transform(low.begin(), low.end(), low.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (low == g_search) return;
        g_search = std::move(low);
        RecomputeSearch();
    }

    bool ClearSearch()
    {
        if (g_search.empty()) return false;
        g_search.clear();
        g_searchHit.clear();
        return true;
    }

    bool SearchMisses(const std::string& a_key)
    {
        if (g_search.empty()) return false;
        // a rebuild since the set was built (looted, sold, rearranged)
        if (g_searchVersion != g_boardVersion) RecomputeSearch();
        return !g_searchHit.contains(a_key);
    }

    bool IsTrashConfirmOpen() { return g_trashAsk.active; }

    bool CloseTrashConfirm()
    {
        if (!g_trashAsk.active) return false;
        g_trashAsk = {};
        return true;
    }

    void DrawTrashConfirm()
    {
        if (!g_trashAsk.active) return;
        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;

        const char* name = g_trashAsk.obj ? g_trashAsk.obj->GetName() : "?";
        const char* msg = Lang::T(Lang::Str::TrashFavConfirm);
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float contentW = (std::max)({ btnRow,
            ImGui::CalcTextSize(msg).x,
            ImGui::CalcTextSize(name).x * 1.35f });
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + sp + 6.0f * S +
                ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        if (wm->BeginConfirmPopup("trashask", "##gi_trashask", name, size)) {
            g_trashAsk = {};   // outside click cancels (tile already snapped back)
            Sfx::SelectOff();
        }
        if (!g_trashAsk.active) { ImGui::End(); return; }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };
        center(ImGui::CalcTextSize(msg).x);
        ImGui::TextColored(sk.inkDim, "%s", msg);
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
        if (ok && g_trashAsk.obj) {
            // ★the TURNED footprint, the same one the drop was validated with
            const Mask m = MaskOf(g_resolver ? g_resolver(g_trashAsk.obj) : GridDef{},
                                  g_trashAsk.rot);
            TrashMakeRoomFor(m);
            ParkKeyInTrash(g_trashAsk.key, g_trashAsk.obj, g_trashAsk.count,
                g_trashAsk.col, g_trashAsk.row, g_trashAsk.xlIdx,
                g_trashAsk.uid, g_trashAsk.sig, g_trashAsk.rot);
            g_trashAsk = {};
        } else if (cancel) {
            g_trashAsk = {};   // tile stays where it snapped back to
        }
        ImGui::End();
    }

    void ProcessTrashDeletes()
    {
        if (g_trashDeleteQ.empty()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { g_trashDeleteQ.clear(); return; }
        int soundBudget = 2;   // a close-all burst must not machine-gun sounds
        for (const auto& d : g_trashDeleteQ) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(d.form);
            if (!obj) continue;
            const int count = d.count;
            // B4-3c: the submit moved to ConfirmTrashDelete -- the commit,
            // where the suppression used to arm -- so the ledger covers the
            // confirm-to-Tick window the counters once covered. The event
            // this call fires confirms the entry; nothing drains by hand.
            player->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kRemove,
                ResolveExitUnit(obj, d.uid, d.sig, count, d.fav ? count : 0,
                                d.xlIdx), nullptr);
            if (g_sound && soundBudget-- > 0) g_sound(obj, false);
        }
        g_trashDeleteQ.clear();
        RequestRebuild();
        MarkCapacityDirty();
    }

    // ---- (1.3.1) shelf-bag intake --------------------------------------
    RE::TESBoundObject* HeldShelfStorable()
    {
        if (!g_held || g_held->fromPartner || !g_held->obj) return nullptr;
        // ★★A BAG MAY GO IN. This read "no bag inside a bag", which was a
        // statement about the old flat bundle rather than a rule anyone chose --
        // the player's own boards have nested bags since E4b. A bundle is a tree
        // now, and the commit below sends the bag's contents along with it.
        if (g_held->coinValue >= 0) return nullptr;   // coin tiles mirror the ledger
        const RE::FormID fid = g_held->obj->GetFormID();
        // ★(1.5.x) A POUCH MAY GO IN NOW. It was refused because a bundle
        // entry had nothing to hold a number, so its gold had nowhere to be
        // written down. BundleItem::gold exists (cosave v15) and the shelf
        // reconcile claims the parcel into it, so a pouch banks inside a
        // bag exactly as it does on a bare shelf cell. Plain coins stay
        // out: a coin tile mirrors the ledger and never travels as an item.
        if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) return nullptr;
        if (g_held->quest) return nullptr;   // Phase 7
        return g_held->obj;
    }

    bool CommitHeldToShelfBag(RE::FormID& a_form, int& a_count,
                              std::uint16_t& a_sig, int& a_rot,
                              std::uint8_t& a_glow, bool& a_stolen)
    {
        auto* obj = HeldShelfStorable();
        if (!obj) return false;
        a_stolen = g_held->stolen;
        a_form = obj->GetFormID();
        a_count = g_held->count;
        a_sig = g_held->sig;
        a_rot = g_held->rot & 3;
        // ★(1.3.2) the markers this unit carries, read from its live entry
        // ONCE here (a commit is a click, not a frame) -- the bundle records
        // them so the bag window can draw them without an inventory walk.
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* entry = LiveEntry(p, obj);
            auto* xl = ExtraForPoolImpl(entry, g_held->uid, g_held->sig);
            a_glow = GlowBits(obj, entry, xl);
        }
        // ★★A BAG TRAVELS WITH ITS INSIDES, here as everywhere else. The
        // manifest is parked for the caller to splice under the entry this
        // commit is about to become -- the same door a bag stored onto the shelf
        // GRID comes through, one step earlier.
        if (g_held->isBag) StoreBagContents(g_held->key, obj);
        LootBarter::RequestStore(obj, g_held->count,
            HeldUidOf(g_held->key, g_held->uid), g_held->sig, g_held->fav,
            -1, g_held->key);
        NotePendingRemove(obj, g_held->key, g_held->count, g_held->xlIdx);
        if (g_sound) g_sound(obj, false);
        g_held.reset();
        RequestRebuild();
        return true;
    }

    int HeldRot() { return g_held ? (g_held->rot & 3) : 0; }

    // ★(1.3.2a) the withdrawn shelf-pouch amount rides the cursor as a
    // pinned purse, exactly like the player pouch's withdraw; the excess
    // over one purse stays as walking gold. The ledger credit is already
    // queued (WalkingGold reflects it this same frame), so the pin's
    // subtraction balances.
    void CarryWithdrawnGold(int a_value)
    {
        if (a_value <= 0) return;
        const int carry = (std::min)(a_value, GoldCoins::kCoinCap);
        if (auto* cform = GoldCoins::CoinForTier(GoldCoins::BandTier(carry))) {
            PickupPartial(cform, carry, {}, 0);
        }
        // ★S-G: the excess beyond one purse becomes board tiles at once --
        // "stays walking" stopped meaning anything when walking gold died
        if (a_value > carry) CoinIncome(a_value - carry);
    }

    bool PeekHeldForShelf(RE::TESBoundObject*& a_obj, int& a_count,
                          std::uint16_t& a_sig, int& a_rot, bool& a_fromPartner)
    {
        if (!g_held || !g_held->obj) return false;
        a_obj = g_held->obj;
        a_count = g_held->count;
        a_sig = g_held->sig;
        a_rot = g_held->rot & 3;
        a_fromPartner = g_held->fromPartner;
        return true;
    }

    void DropHeldForShelf()
    {
        if (!g_held) return;
        if (g_sound) g_sound(g_held->obj, false);
        g_held.reset();
        RequestRebuild();
    }

    // ---- cosave persistence ('GLAY' v5) ----
    // v5 == v2 layout (v3/v4 experiments retired; v4's trailing fav byte is
    // read-and-discarded on load).
    // [u32 bagCount]{str} [u32 entryCount]{ str key, i32 col, i32 row, str bag,
    // i32 count (v2, G4 tile-owned quantity; v1 records load with count=0 =
    // "unspecified", which the reconciler fills like a fresh pickup) }
    // Keys are "Plugin.esp|0xLocalID" strings — load-order independent, no
    // ResolveFormID needed. main.cpp owns the record loop.

    // A RUNTIME-CREATED form (potion brewed at an alchemy lab, weapon the player
    // enchanted) has no source plugin, so FormKey names it "Dynamic|0x<FormID>".
    // Those FormIDs are handed out per session and are NOT stable across a
    // save/load, which makes such a key useless to persist and mildly harmful:
    //   - it can never match its own item again, so it is dead weight that grows
    //     with every distinct recipe the player ever brews, and
    //   - a DIFFERENT runtime form can be handed the same id after a load and
    //     inherit the old item's grid slot.
    // The stale-instance pruner does not collect these (it only walks '@'/'~'
    // suffixed keys), so filter them at the cosave boundary instead. In-memory
    // they stay live and behave normally for the rest of the session.
    bool IsPersistableKey(const std::string& a_key)
    {
        return !a_key.starts_with("Dynamic|");
    }

    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kRecordType, kCosaveVersion)) {
            SKSE::log::error("[GRID] cosave: OpenRecord failed");
            return;
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_openBags.size()));
        for (const auto& b : g_openBags) WriteStr(a_intfc, b);
        // F2: parked-in-trash entries never persist (a mid-menu F5 save would
        // otherwise strand items in a view that doesn't exist after load) —
        // they save at their PRE-park spot so a load simply restores them.
        std::uint32_t persisted = 0;
        for (const auto& [key, le] : g_layout) {
            if (!IsPersistableKey(key)) continue;
            ++persisted;
        }
        a_intfc->WriteRecordData(persisted);
        for (const auto& [key, le] : g_layout) {
            if (!IsPersistableKey(key)) continue;
            const LayoutEntry* out = &le;
            LayoutEntry back;
            if (le.bag == kTrashKey) {
                back.col = -1;
                back.row = -1;
                back.count = le.count;
                if (const auto ri = g_trashReturn.find(key); ri != g_trashReturn.end()) {
                    back = ri->second;
                    back.count = le.count;
                }
                out = &back;
            }
            WriteStr(a_intfc, key);
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->col));
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->row));
            WriteStr(a_intfc, out->bag);
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->count));   // v2
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->rot));     // v6
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->coin));    // v8
            // ★v9: WHICH unit this slot was showing. A hint, not a name -- a
            // stale one only weakens the next match, and the fallback keeps the
            // cell regardless. Persisted because without it every slot would
            // load blank and the first rebuild after a load would match purely
            // by position: harmless for placement, but a stolen stack could bind
            // to a clean one and show the wrong marker until something moved.
            // ★xlIdx is deliberately NOT saved: a list POSITION means nothing
            // across sessions, and writing it down would invite trusting it.
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(out->uid));
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(out->sig));
        }
        // v7: the "already seen" baseline. Without it, loading a save would make
        // the entire inventory read as new the first time the menu opens.
        // ★Dynamic FormIDs are dropped rather than written: SKSE resolves 0xFF
        // ids by passing them through unchanged, so a stale one could land on
        // whatever occupies that slot in the new session.
        std::uint32_t seenN = 0;
        for (const auto& [fid, n] : g_seenCount) {
            if ((fid >> 24) != 0xFF) ++seenN;
        }
        a_intfc->WriteRecordData(seenN);
        for (const auto& [fid, n] : g_seenCount) {
            if ((fid >> 24) == 0xFF) continue;
            a_intfc->WriteRecordData(fid);
            a_intfc->WriteRecordData(static_cast<std::int32_t>(n));
        }
        SKSE::log::info("[GRID] cosave: saved {} placements, {} open bags, {} seen counts",
            g_layout.size(), g_openBags.size(), seenN);
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        // v1 (no per-tile count) still loads: count stays 0 and the reconciler
        // fills it on the first Rebuild (batch absorb, layout preserved).
        //
        // GI1: this used to be a WHITELIST ("!= 1 && != kCosaveVersion") that
        // returned in SILENCE. Bumping kCosaveVersion with that in place would
        // have dropped every v2 save's entire layout without a single log line.
        // Range-check instead, and say so when a record is refused.
        if (a_version < 1 || a_version > kCosaveVersion) {
            SKSE::log::warn("[GRID] cosave: unsupported layout record v{} (max v{}) — skipped",
                            a_version, kCosaveVersion);
            return;
        }

        std::map<std::string, LayoutEntry> layout;
        std::set<std::string> bags;

        std::uint32_t bagCount = 0;
        if (!a_intfc->ReadRecordData(bagCount) || bagCount > kMaxEntries) return;
        for (std::uint32_t i = 0; i < bagCount; ++i) {
            std::string b;
            if (!ReadStr(a_intfc, b)) return;
            bags.insert(std::move(b));
        }
        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count) || count > kMaxEntries) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string key;
            LayoutEntry le;
            std::int32_t col = 0, row = 0;
            if (!ReadStr(a_intfc, key) || !a_intfc->ReadRecordData(col) ||
                !a_intfc->ReadRecordData(row) || !ReadStr(a_intfc, le.bag)) {
                return;   // truncated: keep whatever parsed? no — bail clean
            }
            le.col = col;
            le.row = row;
            if (a_version >= 2) {   // v2: per-tile count (0 in v1 -> reconciled)
                std::int32_t cnt = 0;
                if (!a_intfc->ReadRecordData(cnt)) return;
                le.count = cnt;
            }
            if (a_version == 4) {
                // v4 ONLY (GI27 era): a per-tile favourite byte. The feature
                // was retired in GI33 (favourites are vanilla's), but saves
                // written by those builds still carry the byte -- read it so
                // the stream stays aligned, and throw it away.
                std::int32_t f = 0;
                if (!a_intfc->ReadRecordData(f)) return;
            }
            if (a_version >= 6) {   // GI62: quarter-turns (older saves = upright)
                std::int32_t rot = 0;
                if (!a_intfc->ReadRecordData(rot)) return;
                le.rot = rot & 3;
            }
            // ★v8: the coin amount this slot holds. An older save carries none,
            // and -1 is exactly right for that -- the rebuild hands amounts to
            // unmarked slots in position order, which is what every save did
            // before this field existed.
            if (a_version >= 8) {
                std::int32_t coin = 0;
                if (!a_intfc->ReadRecordData(coin)) return;
                le.coin = coin;
            }
            if (a_version >= 9) {
                std::uint32_t u = 0, s = 0;
                if (!a_intfc->ReadRecordData(u)) return;
                if (!a_intfc->ReadRecordData(s)) return;
                le.uid = static_cast<std::uint16_t>(u);
                le.sig = static_cast<std::uint16_t>(s);
            } else {
                // ★★THE MIGRATION, and it is one line of intent: before v9 a
                // slot's binding lived INSIDE its key ("base~B825#2"). Read it
                // back out and put it where it belongs.
                //
                // The key itself is left exactly as it is. It never has to
                // change -- from here on nothing recomputes it, so an old
                // suffixed key is simply an opaque slot name that happens to
                // look like the old grammar. Renaming every entry would risk
                // orphaning the things that POINT at keys (a bag's contents, a
                // pinned coin purse) for no gain at all.
                le.uid = UidOf(key);
                le.sig = SigOf(key);
            }
            // Saves written before the filter above carry dynamic keys; drop
            // them on the way in so an existing playthrough gets cleaned too.
            if (!IsPersistableKey(key)) continue;
            layout[std::move(key)] = std::move(le);
        }

        // v7: the seen-counts baseline (GI65). Absent in older saves -- those
        // load with no baseline, and the first menu opening marks nothing
        // rather than marking everything.
        std::unordered_map<RE::FormID, int> seen;
        if (a_version >= 7) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n) || n > kMaxEntries) return;
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::FormID raw = 0;
                std::int32_t cnt = 0;
                if (!a_intfc->ReadRecordData(raw) || !a_intfc->ReadRecordData(cnt)) return;
                RE::FormID fid = 0;
                if (a_intfc->ResolveFormID(raw, fid) && fid != 0) seen[fid] = cnt;
            }
        }

        g_layout = std::move(layout);
        g_openBags = std::move(bags);
        g_seenCount = std::move(seen);
        g_seenValid = !g_seenCount.empty();
        g_layoutLoaded = true;   // do NOT fall back to the legacy ini
        g_capacityDirty = true;
        SKSE::log::info("[GRID] cosave: loaded {} placements, {} open bags, {} seen counts",
            g_layout.size(), g_openBags.size(), g_seenCount.size());
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        // clean slate before every load / new game. g_layoutLoaded=false keeps
        // the legacy ini as a one-shot migration source for saves that predate
        // the 'GLAY' record (LoadRecord overrides it when the record exists).
        g_layout.clear();
        g_openBags.clear();
        g_pendingEquip.clear();   // cross-frame set: must never outlive a load
        // ★S1: the stash holds an Item with a live TESBoundObject* -- a load
        // replaces the world under it (원칙 2), and a queued move names keys
        // from the save being left. Both die at the boundary.
        g_stash.reset();
        g_viewMoveQ.clear();
        g_wantTrashView = false;
        g_layoutLoaded = false;
        g_prevKeys.clear();
        // GI65: prevKeys is empty after a load, so the very next rebuild would
        // see EVERY tile as brand new. Suppress that one pass; the baseline
        // itself is restored by LoadRecord (or stays absent on an old save).
        g_newTiles.clear();
        g_seenCount.clear();
        g_seenValid = false;
        g_suppressNew = true;
        // [FAV] tripwire: the memo names the save being left
        g_starMemo.clear();
        g_starChangeOk.clear();
        g_starMemoValid = false;
        g_capacityDirty = true;
        g_avResidueCleared = false;   // legacy CW cleanup is per-save
        ClearAllPendingRemoves();
        // B6: defensive resets — these carried PREVIOUS-session state across
        // a load (g_held even held a stale TESBoundObject*). The menu-close
        // path usually cleans them, but a load with the menu open must not
        // rely on it.
        g_held.reset();
        g_items.clear();
        g_liveObjs.clear();   // rebuilt alongside g_items
        g_views.clear();
        g_target = {};
        g_slotTarget.clear();
        g_dropHint = {};
        // ★Its twin, which this list was missing. A hint that survives a load
        // names a key from the PREVIOUS save, and NextTileKey hands out the
        // lowest free ordinal -- so the first tile of that form in the new game
        // inherits the name and is drained ahead of the positional rules.
        g_drainHint = {};
        g_pouchOpen = false;
        g_pouchSlider = 0;
        g_pouchTile.clear();
        g_knownPouchTiles.clear();   // (1.3.0) tile keys from another save are lies
        g_overloaded = false;
        g_spaceUsed = 0;
        g_spaceTotal = kCols * kMinRows + g_cwBonusCells;   // W3
        // F2: trash state is per-session — parked items simply reappear on
        // their boards after a load (the engine inventory was never touched)
        g_trashOpen = false;
        g_trashOrder.clear();
        g_trashReturn.clear();
        g_trashXl.clear();
        g_trashDeleteQ.clear();
        g_trashAsk = {};
        // ★★★THE WINDOWS THAT OUTLIVE THEIR SAVE. g_pouchOpen above has been
        // on this list since it was written; the recharge window arrived in
        // 1.3.1 and never joined it. Left standing, it comes back after a load
        // still naming the PREVIOUS save's weapon by uid/sig/worn/hand -- and
        // when that form is absent from the new one, the worn branch reads the
        // charge off whatever is in the player's hand now and spends a real
        // soul gem into it.
        g_rechargeUI = {};
        g_rechargePick = {};
        // ★Book reading is worse than it looks: ProcessBookRead runs from
        // UIRoot::Tick every frame, menu open or not. An owed page eight ticks
        // from a load fires INTO THE NEW SESSION -- a page opening by itself,
        // or a UIRoot::Close nobody asked for.
        g_pendingRead.reset();
        g_pendingShelfPage.reset();
        g_pageOwed.reset();
        g_pageOwedWait = 0;
        // ★And the two quiet ones: a click owed to a tile that no longer
        // exists, and arrival marks whose FormIDs belonged to another game
        // (they decay on their own, but not before poisoning a rebuild or three).
        g_clickAction.reset();
        g_transientArrivals.clear();
        g_optimisticGone.clear();   // claims from the previous session
        RequestRebuild();
    }

    void NoteFormSeen(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return;
        const RE::FormID fid = a_obj->GetFormID();
        // ★The baseline first. Without it the mark returns the moment the item
        // comes back off the body: unequipping rebuilds a tile the new-item
        // test has not seen before, and that test asks the baseline, not the
        // mark. Read the count from the ENGINE, the same source the baseline
        // is taken from, so the two cannot disagree.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            auto inv = player->GetInventory(
                [&](RE::TESBoundObject& o) { return &o == a_obj; });
            int n = 0;
            for (const auto& [o, d] : inv) n = d.first;
            if (n > 0) g_seenCount[fid] = n;
        }
        // ...then the marks on whatever tiles this form is showing as. A form
        // can hold several (split stacks, named units), and equipping one is
        // still "I have seen this item".
        std::erase_if(g_newTiles, [fid](const std::string& k) {
            for (const auto& it : g_items) {
                if (it.key == k) return it.obj && it.obj->GetFormID() == fid;
            }
            return false;
        });
    }

    void NoteTransientArrival(RE::FormID a_form)
    {
        if (a_form) g_transientArrivals[a_form] = TransientArrival{};
    }

    void NoteInventorySeen()
    {
        // ★Snapshot the ENGINE's counts, not the board's. A tile can be missing
        // from the grid while the item is genuinely held (parked in the trash,
        // queued for an equip), and using board totals would make those look
        // like fresh gains the next time the menu opens.
        g_newTiles.clear();
        g_seenCount.clear();
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            auto inv = player->GetInventory();
            for (const auto& [obj, data] : inv) {
                if (obj && data.first > 0) g_seenCount[obj->GetFormID()] = data.first;
            }
        }
        g_seenValid = true;
    }

    void MarkLayoutFresh()
    {
        // new game (kNewGame arrives after revert, and no load callback runs):
        // start EMPTY — the legacy-ini migration is for old saves only
        g_layout.clear();
        g_openBags.clear();
        g_layoutLoaded = true;
        g_capacityDirty = true;
    }
}
