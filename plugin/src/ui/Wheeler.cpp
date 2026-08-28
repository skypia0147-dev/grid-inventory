#include "ui/Wheeler.h"

#include "game/Costume.h"
#include "game/DualRing.h"
#include "ui/Equip.h"
#include "ui/Fallback.h"
#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/Loadout.h"
#include "ui/Sfx.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <RE/B/BSTimer.h>
#include <RE/L/LookHandler.h>
#include <RE/P/PlayerControls.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace FUI::Wheeler
{
    namespace
    {
        // ---- the painted wheel -----------------------------------------------
        // Ten slots, fixed. ★The count is fixed because the ARTWORK is: the
        // wheel was painted with ten segments and one of them already sits on
        // twelve o'clock, so nothing here divides a circle at runtime. That is
        // also what makes the ragged ink possible -- the edges are a picture,
        // and a picture cannot be re-cut every frame.
        constexpr int   kSlots = 10;
        constexpr float kStep = 360.0f / kSlots;

        // ★★The wheel cannot grow -- the ring is a painting cut into ten -- so
        // the LIST has to fit it. EQUIP occupies one place and is not a preset,
        // which is why the preset cap is one less than the slot count.
        // ★A comment saying "keep these in step" is a comment nobody reads on
        // the day they raise the cap. This fails the build instead.
        static_assert(Loadout::kMaxPresets + 1 <= kSlots,
                      "preset cap exceeds the wheel: EQUIP takes a slot, so the "
                      "cap must be kSlots - 1 (raise kSlots only if the artwork "
                      "is re-cut, and see the COSTUME group too)");

        // Radii in the TEXTURE's own space (the bake is a 512 square centred on
        // the wheel; 256 is its half-size and also its outermost droplet).
        constexpr float kTexHalf = 256.0f;
        constexpr float kRSegIn = 111.0f;    // segments start
        constexpr float kRSegOut = 225.0f;   // segments end
        constexpr float kWheelPx = 560.0f;   // design px the square is drawn at
        constexpr float kDesignH = 1080.0f;

        // ★400, not 300. The open is a hand travelling once round the ring --
        // laying the ground and dropping the slots onto it -- and at 300 that
        // journey was over before it read as a journey. Nothing here is waited
        // on: the wheel is usable from the first frame and the aim is what
        // gates acting, so the only thing a longer open costs is time the
        // player was going to spend looking anyway.
        constexpr float kOpenMs = 400.0f;
        // ★★420, not 220. "Keep the pick" is the whole point of the close --
        // the others leave, the chosen one stays a moment longer -- and at 220
        // the unchosen slots were gone in about 120ms. There was nothing to
        // read: the wheel simply vanished. Closing may take longer than
        // opening; the hand has already let go and is not waiting on it.
        constexpr float kCloseMs = 420.0f;
        constexpr float kStaggerSpan = 0.55f;
        // Switching lists re-inks the ring, in the same order it was first laid
        // down. ★Same grammar as the open on purpose -- a second kind of motion
        // for "the same ten slots, different contents" would read as a
        // different wheel rather than the same one turning over.
        // ★★DERIVED, not a second 400. The two were written as equal numbers
        // and immediately drifted the first time one of them was tuned: the
        // open went to 400 and the switch stayed at 300, which is the same
        // stagger played at two speeds -- exactly the "different wheel" the
        // line above exists to prevent. If the switch ever needs its own pace
        // it can have its own constant, and that will then be a decision
        // someone made rather than one that happened.
        constexpr float kGroupMs = kOpenMs;
        // ★340, not 170. At 170 the stroke was over before the eye caught it
        // moving -- the mark read as appearing rather than being drawn, which
        // is the same as having no animation at all. The stroke is the one
        // moment the wheel shows a HAND, so it has to be slow enough to watch.
        // It is still shorter than a slot step is likely to be, so an aim that
        // settles anywhere sees a finished mark.
        constexpr float kArcMs = 340.0f;     // how long the brush takes to draw
        constexpr float kDeadzone = 46.0f;   // raw mouse counts before a slot is picked
        constexpr float kMouseSens = 0.75f;

        constexpr ImU32 kInk = IM_COL32(0x0a, 0x0a, 0x0b, 255);
        constexpr ImU32 kRed = IM_COL32(0xb5, 0x31, 0x2a, 255);
        constexpr ImU32 kPaper = IM_COL32(0xd8, 0xc9, 0xa0, 255);
        constexpr ImU32 kDim = IM_COL32(0xa2, 0xa2, 0x9f, 255);
        constexpr ImU32 kInkText = IM_COL32(0xe6, 0xe3, 0xdd, 255);

        // ---- state ------------------------------------------------------------
        // ★What is ACTIVE lives in Loadout and Costume. What lives here is a
        // pointer: which slot the mouse is aimed at. It is seeded from those two
        // on every open and thrown away on close.
        bool  g_open = false;
        float g_t = 0.0f;
        int   g_dir = 0;
        // ★Diagnostic only: armed on open, spent by the first fully-open frame
        // of the medallion pass. Per OPEN rather than per session, because the
        // question it answers ("did the pak answer this time") can change when
        // something else reads the settings file behind us.
        bool  g_diagIcons = false;
        // ★How much of the ring the open actually drew. Written while opening,
        // READ while closing -- the close contracts whatever is on screen, and
        // a wheel released mid-sweep has less than a full circle of it.
        float g_ringP = 0.0f;
        // ★Groups are a LIST now, not a pair. Stage 1 of replacing a separate
        // quick-menu mod: the third group will be item quick-slots, and it has
        // to be able to arrive as one table row rather than as a third branch
        // in every function that asks "which group".
        constexpr int kPreset  = 0;
        constexpr int kCostume = 1;
        constexpr int kItems   = 2;
        constexpr int kMagic   = 3;
        constexpr int kGroups  = 4;
        int   g_group = kPreset;
        // ★On by default: the wheel is what this feature IS, and a player who
        // wants the vanilla screen back can say so once. See Wheeler.h.
        bool  g_enabled = true;
        // ★Said once per stand-down -- see the probe in OnButton.
        bool  g_saidPassing = false;

        // ---- the item group's contents -----------------------------------------
        // ★★★NO REGISTRATION STEP. The wheel reads the game's OWN favourites --
        // the star you put on an item in the inventory is the whole binding.
        // The mod this replaces needs the vanilla inventory open to bind, which
        // is exactly why it cannot work alongside a replacement inventory; a
        // wheel built from a list the player already curates has no such
        // dependency and nothing extra to learn.
        // ★Spells and shouts are favourited too, and they live somewhere else
        // entirely -- MagicFavorites, not an ExtraHotkey on an inventory entry.
        // Same star to the player, two different lists to the engine, so the
        // wheel reads both and keeps the kind alongside: what a slot DOES on
        // click depends on it, and so does what it draws.
        enum class FavKind : std::uint8_t { kItem, kSpell, kShout };

        struct FavItem
        {
            RE::TESForm*        form = nullptr;   // the thing itself
            RE::TESBoundObject* obj = nullptr;    // items only -- for the icon
            FavKind             kind = FavKind::kItem;
            std::uint16_t       sig = 0;    // the tempered/enchanted unit
            int                 xl = -1;    // extraLists index, -1 = plain
            int                 count = 0;
            bool                worn = false;   // snapshot; see Items::current
            // ★★★THE ENGINE'S OWN NAME FOR THE UNIT, and its absence was the
            // whole bug. The wheel asked for its starred unit with uid 0, and a
            // pool request with uid 0 EXPLICITLY SKIPS every list that carries a
            // uniqueID -- "a uid unit is the sole member of its own pool", by
            // design. The engine hands uniqueIDs to the things it tracks, which
            // is exactly what gets worn: weapons and armour came back "named
            // unit gone -- equip skipped" every time, while potions and rings
            // (which never earn one) worked fine. That is the split the reports
            // described, down to the item kinds.
            // ★Appended LAST so the seven-value initialisers below stay valid --
            // the magic ring has no unit to name and leaves it 0.
            std::uint16_t       uid = 0;
        };
        // ★Two lists, not one shared ten. Gear and magic are starred from
        // different screens and reached for at different moments, and sharing a
        // ring meant a full quiver of potions could push every spell off it.
        // Ten each is enough; ten between them was not.
        FavItem g_fav[kSlots]{};
        int     g_favN = 0;
        FavItem g_mag[kSlots]{};
        int     g_magN = 0;

        // defined with UseFav, needed by the snapshot above it
        [[nodiscard]] bool UsesVoiceSlot(RE::TESForm* a_form);

        // ★★The player's own order, kept as FormIDs rather than slot numbers.
        // The list is rebuilt from the game's favourites several times a second
        // and its length changes as stars come and go, so "slot 3" means
        // nothing across two rebuilds -- but "this sword" does.
        // ★Anything not in here sorts after everything that is, in the order
        // the engine hands it over. A newly starred item therefore lands at the
        // end and stays put once moved, which is the only behaviour that does
        // not shuffle the wheel under a hand that has learned it.
        std::vector<RE::FormID> g_order[2];   // [0] gear, [1] magic

        // ★★★EVERY STAR, NOT JUST THE TEN THAT FIT.
        //
        // ApplyOrder gives a place up when its form is no longer starred, and
        // it asked the DISPLAYED list -- which stops at kSlots. Past ten
        // favourites the eleventh is starred, absent from that list, and read
        // as unstarred: its place was zeroed on every rebuild (each open, and
        // again every 330ms) and the zero went into the cosave.
        //
        // Worse, WHICH ten survive is not stable. GetInventory returns a
        // std::map keyed by TESBoundObject*, so the walk runs in pointer-
        // address order -- load order, allocation order, and different between
        // sessions. The arrangement decayed differently every restart.
        //
        // So the walks below record every starred form here and stop only
        // FILLING at kSlots, and this is what ApplyOrder asks.
        std::set<RE::FormID> g_starredAll[2];

        // ★The saved order names a form PER PLACE, with 0 for "left empty".
        // Keeping the empty places is what makes a wheel worth arranging: the
        // hand remembers where a thing sits, and closing the gaps every time a
        // potion runs out would move everything else.
        void ApplyOrder(FavItem* a_list, int a_n, int a_which)
        {
            auto& want = g_order[a_which];
            if (want.empty()) return;
            // ★★★A PLACE IS HELD BY A STAR, NOT RESERVED BY A NAME. Anything in
            // the table that is not starred right now gives its place up, so
            // "newly starred" means the same thing whether the item is new to
            // the wheel or was on it yesterday: it lands in the first free
            // place from the front.
            //
            // ★It used to keep the name and hand the place back the moment that
            // exact item was starred again. Unstar everything, star one thing,
            // and it went to wherever that item had been dragged months ago --
            // an empty wheel that still remembers is a wheel the player cannot
            // reset by any means they can see.
            //
            // ★This is what RememberOrder already believed: it writes the
            // arrangement AS DRAWN on every drag, so a slot whose item was
            // missing at that moment was zeroed anyway. Only the reader was
            // still treating the table as a booking. The two agree now.
            //
            // ★The cost, accepted: put a starred item in a chest and take it
            // out again and it comes back at the front rather than where it
            // was. Its star went into the chest with it, and a place kept for
            // something that is not starred is the very thing above.
            for (auto& id : want) {
                if (!id) continue;
                // ★asked of every star, not of the ten on screen -- see
                // g_starredAll for what the narrower test cost.
                if (!g_starredAll[a_which].contains(id)) id = 0;
            }
            FavItem out[kSlots]{};
            bool taken[kSlots]{};
            for (int slot = 0; slot < kSlots && slot < static_cast<int>(want.size()); ++slot) {
                if (!want[slot]) continue;
                for (int i = 0; i < a_n; ++i) {
                    if (taken[i] || !a_list[i].form) continue;
                    if (a_list[i].form->GetFormID() != want[slot]) continue;
                    out[slot] = a_list[i];
                    taken[i] = true;
                    break;
                }
            }
            // whatever the saved order did not mention goes in the first free
            // place -- a newly starred thing appears without disturbing anything
            for (int i = 0; i < a_n; ++i) {
                if (taken[i] || !a_list[i].form) continue;
                for (int slot = 0; slot < kSlots; ++slot) {
                    if (out[slot].form) continue;
                    out[slot] = a_list[i];
                    break;
                }
            }
            for (int i = 0; i < kSlots; ++i) a_list[i] = out[i];
        }

        void RememberOrder(const FavItem* a_list, int a_n, int a_which)
        {
            (void)a_n;
            auto& out = g_order[a_which];
            out.assign(kSlots, 0);
            for (int i = 0; i < kSlots; ++i) {
                if (a_list[i].form) out[i] = a_list[i].form->GetFormID();
            }
        }

        // ---- the set wheels' own arrangement ---------------------------------
        // ★★★THE WHEEL OWNS WHERE THINGS SIT, the game owns what exists. That is
        // the same split the item wheels have always had -- the engine says
        // which items are starred, the wheel says where they go -- and applying
        // it here is what makes all four groups one mechanism.
        //
        // ★It was briefly the other way: dragging a preset moved the INVENTORY
        // TAB, on the reasoning that two orders for one list must drift. But
        // they are not one value. "Which tab is third" is the data's order;
        // "what sits at three o'clock" is a preference about a hand. Tying them
        // together also leaked a KIND constraint into a PLACE one -- EQUIP is
        // index 0 because it is not a preset, not because it is first, and that
        // is why it could not be moved off twelve o'clock.
        //
        // ★A slot holds a tab index, or kEmptySlot.
        constexpr int kEmptySlot = -100;   // nothing sits here
        // ★★kClearSlot is GONE, and reading a save that has one is the only
        // reason it is still named. The costume wheel used to carry a "no
        // costume" row of its own beside an EQUIP tab it drew and then refused
        // -- two slots for one idea, since EQUIP cannot be worn as a costume
        // and choosing it can only mean "wear no costume". Now EQUIP IS that
        // answer, and the wheel is a slot shorter for it.
        constexpr int kClearSlot = -1;     // legacy: retired to kEmptySlot on load
        int  g_setOrder[2][kSlots]{};      // indexed by kPreset / kCostume
        bool g_setInit = false;

        void ResetSetOrder()
        {
            for (auto& w : g_setOrder) {
                for (auto& s : w) s = kEmptySlot;
            }
            g_setInit = true;
        }

        // ★Bring the arrangement in step with the tabs that actually exist --
        // called on every open, exactly as CollectFavorites is. Doing it by
        // rebuild rather than by notification is what removes the bookkeeping a
        // purchase or a delete would otherwise owe: a tab that went away simply
        // stops being found, and a new one lands in the first free place.
        // ★★The one thing a rebuild CANNOT see is a deletion, because deleting
        // tab 2 renumbers tab 3 into a 2 that still exists. That is what
        // OnTabRemoved below is for, and it is the only notification needed.
        void RebuildSetOrder()
        {
            if (!g_setInit) ResetSetOrder();
            const int n = Loadout::Count();
            for (int w = 0; w < 2; ++w) {
                int* ord = g_setOrder[w];
                // ★★A retired clear row is REMOVED, not emptied. Everywhere
                // else an empty place is deliberate -- the hand remembers where
                // a thing sits, so a potion running out must not slide the rest
                // along -- but this place was never chosen by the player, and
                // leaving a hole where it stood would push EQUIP off twelve
                // o'clock in every save made before the row went away.
                for (int i = 0; i < kSlots; ++i) {
                    if (ord[i] != kClearSlot) continue;
                    for (int j = i; j < kSlots - 1; ++j) ord[j] = ord[j + 1];
                    ord[kSlots - 1] = kEmptySlot;
                    --i;   // whatever slid into this place is unexamined
                }
                for (int i = 0; i < kSlots; ++i) {
                    const int t = ord[i];
                    if (t == kEmptySlot) continue;
                    if (t < 0 || t >= n) ord[i] = kEmptySlot;   // tab is gone
                }
                // ★★A TAB SITS IN ONE PLACE. Nothing here can seat one twice --
                // the pass below places each tab once, a drag swaps, and the
                // remove-notice renumbers -- so a repeat means the arrangement
                // arrived damaged, from a save or from a bug not yet found.
                //
                // It is worth healing rather than trusting, because of what a
                // repeat DOES: the pass below seats a tab only if no slot
                // mentions it and only where a slot is empty, so ten copies of
                // tab 0 leave no empty place and every preset the player owns
                // becomes unreachable. That is the reported shape exactly -- a
                // wheel full of EQUIP with the bought preset nowhere on it, and
                // no way out of it, because a wheel you cannot see a preset on
                // is a wheel you cannot drag one off.
                // ★The FIRST of each wins: whatever the player arranged, the
                // earliest place is the one they are likeliest to recognise.
                for (int i = 0; i < kSlots; ++i) {
                    if (ord[i] == kEmptySlot) continue;
                    for (int j = i + 1; j < kSlots; ++j) {
                        if (ord[j] != ord[i]) continue;
                        SKSE::log::warn("[WHEEL] tab {} sat in slots {} and {} -- "
                                        "the later place is cleared", ord[i], i, j);
                        ord[j] = kEmptySlot;
                    }
                }
                // ...and seat whatever the arrangement does not mention yet, in
                // the first free place, so a newly bought preset appears
                // without disturbing anything already placed.
                for (int t = 0; t < n; ++t) {
                    bool seen = false;
                    for (int i = 0; i < kSlots; ++i) seen |= (ord[i] == t);
                    if (seen) continue;
                    for (int i = 0; i < kSlots; ++i) {
                        if (ord[i] == kEmptySlot) { ord[i] = t; break; }
                    }
                }
            }
        }
        float   g_favMs = 0.0f;   // ms since the last favourites re-read
        bool    g_itemActed = false;      // a click already equipped this visit
        // ★★A VISIT THAT ARRANGED DOES NOT ALSO APPLY. Letting go of the hotkey
        // acts on whatever the aim is resting on, and after a drag that is by
        // definition the thing just moved -- so tidying the wheel ended by
        // using the last piece you tidied. Dragging is an act in itself and a
        // complete one; the release has nothing left to do.
        // ★Set only when something actually TRAVELLED, so a press that merely
        // failed to become a drag still applies the way a plain pick does.
        // ★It matters most on the set wheels, where "apply" means stripping the
        // character and re-dressing them: rearranging presets must never be
        // able to change which one you are wearing.
        bool    g_arranged = false;
        int     g_dragFrom = -1;          // slot the left button went down on
        // ★★Did it actually travel? g_dragFrom cannot answer that any more: a
        // drag rearranges as it goes and moves that mark along with the carried
        // piece, so by the time the button comes up "from" always equals
        // "where the cursor is" -- and every drop read as a click. Arranging
        // the wheel would equip whatever you had just finished moving.
        bool    g_dragMoved = false;
        // ★★Where each slot is DRAWN, chasing where it belongs. Reordering
        // moves an entry between indices instantly -- that is bookkeeping -- but
        // a wheel that teleports its contents reads as a glitch. Each slot keeps
        // its own displayed angle and eases toward its index, so passing a
        // neighbour makes THAT neighbour slide aside while the rest hold still.
        float g_slotAngle[kSlots]{};
        bool  g_angleInit = false;
        constexpr float kSlideMs = 130.0f;   // a slot's own travel time

        void ResetSlotAngles()
        {
            for (int i = 0; i < kSlots; ++i) g_slotAngle[i] = i * kStep;
            g_angleInit = true;
        }

        // ---- the cursor: a trail of ink -------------------------------------
        // ★★THE POINTER HAS NOWHERE TO BE SEEN. The game hides and pins the
        // real cursor while the wheel is up, and the wheel answers with a
        // highlight on whatever slot you are pointing AT -- which says nothing
        // in the middle, where you are pointing at nothing. So the middle had
        // no feedback at all: the hand moved and the screen did not.
        // ★A trail rather than a dot, because a dot answers "where" and the
        // middle's real question is "which way am I going". The marks are laid
        // where the pointer HAS BEEN and dry out behind it, so the shape of the
        // last half-second is on screen.
        struct InkDrop
        {
            float x = 0.0f, y = 0.0f;   // in the pointer's own space (g_mx/g_my)
            float age = 1.0f;           // 0 = just fell, 1 = gone
            float rot = 0.0f;
            float scale = 1.0f;
            int   cell = 0;             // which drop, 0..kDropAtlas^2-1
            bool  mirror = false;       // ...and its reflection
        };
        // ★★The atlas is CUT FROM THE WHEEL'S OWN PAINTING -- every cell is a
        // real fleck the painter threw off the brush, harvested by
        // tools/make_ink_drops.py. Drawn ones were tried first and read as
        // smooth blobs on a surface made of brushwork: a formula can wobble an
        // outline but it does not know about paper.
        // ★Sixteen of them, and mirrored, so thirty-two silhouettes. Rotation
        // alone does not disguise a repeat -- the eye is very good at rotated
        // shapes, and a drop with a tail stays "that one with the tail" at any
        // angle. A reflection is a different silhouette and costs no texture.
        constexpr int kDropAtlas = 4;   // cells per side
        constexpr int   kDropCount = 20;
        constexpr float kDropLifeMs = 520.0f;
        // ★★Wider than a mark is, or the marks touch and the trail becomes one
        // sausage -- which is what the first attempt drew. A trail is read as a
        // COUNT of separate landings; overlapping them takes that away and
        // leaves a stroke, and the wheel already has a stroke that means
        // something else.
        constexpr float kDropGap = 16.0f;   // pointer units between marks
        InkDrop g_drop[kDropCount]{};
        int     g_dropHead = 0;
        float   g_dropLastX = 0.0f, g_dropLastY = 0.0f;
        std::uint32_t g_dropRand = 0x9E3779B9u;

        void ClearDrops()
        {
            for (auto& d : g_drop) d.age = 1.0f;
            g_dropLastX = g_dropLastY = 0.0f;
        }

        // ★★★EVERYTHING THAT LIVES FOR ONE VISIT, CLEARED IN ONE PLACE. These
        // are the flags that describe what the hand is doing RIGHT NOW -- what
        // it picked up, whether it moved anything, whether a click already
        // acted. None of them mean anything across two visits.
        //
        // ★They were reset piecemeal and g_dragFrom was reset in exactly ONE
        // place: the mouse-up handler. Let go of the hotkey while still holding
        // a slot and that handler never runs -- the wheel is closed by then and
        // the mouse branch returns early -- so the next open began with a slot
        // already "carried". Aiming alone then rearranged the ring and wrote it
        // to the save, with no button held. A list of flags that must all be
        // cleared together is a list that will eventually be cleared apart.
        void ResetVisitState()
        {
            g_dragFrom = -1;
            g_dragMoved = false;
            g_itemActed = false;
            g_arranged = false;
        }

        // ★Lay one down only once the hand has actually travelled. Spawning per
        // FRAME instead piles the whole trail onto one spot the moment the
        // player holds still, which turns a trail into a blot -- and makes the
        // trail's length depend on the frame rate rather than on the movement.
        void MaybeDropInk(float a_x, float a_y)
        {
            const float dx = a_x - g_dropLastX, dy = a_y - g_dropLastY;
            if (dx * dx + dy * dy < kDropGap * kDropGap) return;
            g_dropLastX = a_x;
            g_dropLastY = a_y;
            g_dropRand = g_dropRand * 1664525u + 1013904223u;
            const std::uint32_t r = g_dropRand >> 8;
            auto& d = g_drop[g_dropHead];
            g_dropHead = (g_dropHead + 1) % kDropCount;
            d.x = a_x;
            d.y = a_y;
            d.age = 0.0f;
            d.cell = static_cast<int>(r % (kDropAtlas * kDropAtlas));
            d.mirror = ((r >> 20) & 1u) != 0u;
            d.rot = static_cast<float>((r >> 2) & 0xFFu) * (6.28318531f / 256.0f);
            // ★A WIDE spread, not a nudge. Marks of nearly one size read as a
            // dotted line however irregular each one is; it is the difference
            // between them that says a hand did this.
            d.scale = 0.58f + static_cast<float>((r >> 10) & 0x7Fu) * (0.80f / 127.0f);
        }

        void AdvanceDrops(float a_dt)
        {
            const float k = a_dt / kDropLifeMs;
            for (auto& d : g_drop) {
                if (d.age < 1.0f) d.age = (std::min)(1.0f, d.age + k);
            }
        }

        // ---- sound ------------------------------------------------------------
        // ★★The step blip needs a THROTTLE. Aiming crosses slots as fast as the
        // hand moves, so one sweep across the ring fires ten of these inside a
        // few frames and they arrive as a rattle rather than as clicks. The
        // inventory hit this first and answered it the same way -- Sfx::HoverNote
        // carries a 60ms floor -- but that helper keys off an ImGui widget id
        // and there are no widgets here, so the rule is repeated rather than
        // reused. Same number on purpose: a wheel notch and a tile hover should
        // not tick at different rates.
        constexpr float kStepBlipMs = 60.0f;
        float g_blipCool = 0.0f;

        void StepBlip()
        {
            if (g_blipCool > 0.0f) return;
            g_blipCool = kStepBlipMs;
            Sfx::Focus();
        }

        // ★Angles travel WITH their entry. The list swap moves an entry between
        // indices; without the same swap here each slot would keep the angle of
        // whoever used to sit at its index, and the ring would shuffle rather
        // than the one carried piece moving.
        void SwapAngles(int a_a, int a_b)
        {
            if (!g_angleInit) ResetSlotAngles();
            std::swap(g_slotAngle[a_a], g_slotAngle[a_b]);
        }

        void AdvanceSlotAngles(float a_dt)
        {
            if (!g_angleInit) { ResetSlotAngles(); return; }
            const float k = std::clamp(a_dt / kSlideMs, 0.0f, 1.0f);
            for (int i = 0; i < kSlots; ++i) {
                const float want = i * kStep;
                // ★Shortest way round: 350 degrees to 10 is twenty degrees, not
                // three hundred and forty. Without this a slot crossing twelve
                // o'clock takes the long way and sweeps the whole wheel.
                float d = want - g_slotAngle[i];
                while (d > 180.0f) d -= 360.0f;
                while (d < -180.0f) d += 360.0f;
                g_slotAngle[i] += d * k;
            }
        }
        RE::TESForm* g_castForm = nullptr;   // spell/shout waiting for the game thread
        bool         g_castLeft = false;
        // ★"...and take it OFF instead", decided at click time off the snapshot
        // rather than re-derived on the game thread, so the two cannot disagree
        // about what was on screen when the player pressed.
        bool         g_castOff = false;
        // ★★MILLISECONDS, not frames. Every one of these was a frame count
        // written for 60fps -- at 144Hz the inventory re-scan ran seven
        // times a second instead of three, and the "once a second" key poll
        // fired every 0.4s. A period in frames is a period that changes with
        // the machine.
        constexpr float kFavRefreshMs = 330.0f;   // ~3 snapshots a second
        int   g_sel = -1;           // slot under the pointer, -1 = none
        int   g_pick = -1;          // latched at release, for the close
        int   g_pickGroup = -1;
        float g_arcT = 0.0f;        // how much of the brush stroke is laid down
        float g_groupT = 1.0f;      // list-switch re-ink; 1 = settled
        int   g_warm = 0;           // costume slot to preload this frame
        float g_greyIdleMs = 0.0f;  // ms the wheel has been fully down
        constexpr float kGreyFreeMs = 500.0f;   // before the capture is freed
        float g_mx = 0.0f, g_my = 0.0f;
        bool  g_padDriving = false;

        float g_savedTimeMult = 1.0f;
        bool  g_slowed = false;
        float g_keyMs = 0.0f;   // ms to the next Favorites-binding re-read

        // ---- the hotkey, as a combination --------------------------------------
        // Index 0 = keyboard, 1 = gamepad. A set of one is the ordinary case.
        std::uint32_t g_combo[2][kMaxCombo] = { { 0x2B }, { 0x0100 } };  // \ , LB
        int           g_comboN[2] = { 1, 1 };
        float         g_slowFactor = 0.25f;

        // What is physically down right now. ★Needed because a combination has
        // to answer "is everything else already held?" at the moment its last
        // key arrives, and an InputEvent only ever tells us about ONE key.
        bool          g_kbDown[256] = {};
        // ★A LIST, not a bitfield. The pad's face buttons are bit masks, but
        // the triggers are not -- kLeftTrigger is 0x0009, which as bits reads
        // as D-pad up + right. OR them together and holding the two D-pad
        // directions becomes indistinguishable from squeezing the trigger.
        std::uint32_t g_padDown[8] = {};
        int           g_padDownN = 0;
        // The code that opened the wheel -- the only one whose release is ours
        // to eat. See the header for why that distinction is not cosmetic.
        std::uint32_t g_openedBy = 0;

        // rebinding
        int           g_capDev = -1;          // -1 = off, 0 = keyboard, 1 = pad
        std::uint32_t g_capCode[kMaxCombo] = {};
        int           g_capN = 0;
        int           g_capHeld = 0;          // still-down count; 0 = commit

        [[nodiscard]] float Scale()
        {
            const auto& io = ImGui::GetIO();
            return io.DisplaySize.y > 1.0f ? io.DisplaySize.y / kDesignH : 1.0f;
        }

        // ---- textures ---------------------------------------------------------
        constexpr const char* kDir = "Data/SKSE/Plugins/GridInventory_wheel/";
        IconCache::Icon g_ring{}, g_arc{}, g_tab{}, g_tick{}, g_drops{}, g_slot[kSlots]{};
        bool g_texTried = false;
        bool g_texOk = false;

        void LoadTextures()
        {
            if (g_texTried) return;
            g_texTried = true;
            IconCache::LoadPngTexture(std::string(kDir) + "ring.png", g_ring);
            IconCache::LoadPngTexture(std::string(kDir) + "arc.png", g_arc);
            IconCache::LoadPngTexture(std::string(kDir) + "tab.png", g_tab);
            IconCache::LoadPngTexture(std::string(kDir) + "tick.png", g_tick);
            IconCache::LoadPngTexture(std::string(kDir) + "drops.png", g_drops);
            int n = 0;
            for (int i = 0; i < kSlots; ++i) {
                if (IconCache::LoadPngTexture(
                        std::string(kDir) + "slot" + std::to_string(i) + ".png", g_slot[i])) {
                    ++n;
                }
            }
            g_texOk = g_ring.srv && g_arc.srv && n == kSlots;
            SKSE::log::info("[WHEEL] textures {} (ring={} arc={} slots={}/{})",
                g_texOk ? "loaded" : "MISSING", g_ring.srv != nullptr,
                g_arc.srv != nullptr, n, kSlots);
        }

        // ---- magic symbols: school sigils and the dragon alphabet --------------
        // ★★Loaded on demand and kept, like the weapon medallions -- but from
        // the WHEEL's own folder and as white+alpha, so they take the wheel's
        // paper colour the way every other mark on it does. The weapon
        // medallions are painted art on a torn disc and are drawn as-is; these
        // are marks, not pictures, and a mark that ignored the palette would be
        // the one thing on the wheel wearing its own colour.
        std::unordered_map<std::string, IconCache::Icon> g_symbols;

        [[nodiscard]] const IconCache::Icon* Symbol(const std::string& a_file)
        {
            auto it = g_symbols.find(a_file);
            if (it == g_symbols.end()) {
                IconCache::Icon ic{};
                IconCache::LoadPngTexture(kDir + a_file + ".png", ic);
                if (!ic.srv) SKSE::log::warn("[WHEEL] symbol missing: {}{}.png",
                                             kDir, a_file);
                it = g_symbols.emplace(a_file, ic).first;
            }
            return it->second.srv ? &it->second : nullptr;
        }

        // A spell's school, straight off the engine — MagicItem::GetAssociatedSkill.
        [[nodiscard]] const char* SchoolOf(RE::TESForm* a_form)
        {
            auto* spell = a_form ? a_form->As<RE::SpellItem>() : nullptr;
            if (!spell) return nullptr;
            switch (spell->GetAssociatedSkill()) {
            case RE::ActorValue::kIllusion:   return "sym_illusion";
            case RE::ActorValue::kConjuration:return "sym_conjuration";
            case RE::ActorValue::kDestruction:return "sym_destruction";
            case RE::ActorValue::kRestoration:return "sym_restoration";
            case RE::ActorValue::kAlteration: return "sym_alteration";
            default: return nullptr;   // powers, abilities -> the old medallion
            }
        }

        // ★★The dragon alphabet is NOT one letter per roman letter: nine of its
        // 34 signs stand for a pair (aa ah ei ey ii ir oo ur uu). Matching the
        // two-letter ones FIRST is the whole rule -- read left to right taking
        // the longest sign that fits, "Feim" is f+ei+m and not f+e+i+m.
        // Longest-first also has to be a LIST, not a length test: "ir" is a sign
        // and "ib" is not, so only these nine may consume two letters.
        [[nodiscard]] std::vector<std::string> Transliterate(std::string_view a_word)
        {
            static constexpr const char* kPairs[] = {
                "aa", "ah", "ei", "ey", "ii", "ir", "oo", "ur", "uu"
            };
            std::vector<std::string> out;
            std::string w;
            w.reserve(a_word.size());
            for (const char ch : a_word) {
                w.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(ch))));
            }
            for (std::size_t i = 0; i < w.size();) {
                bool pair = false;
                if (i + 1 < w.size()) {
                    const std::string two = w.substr(i, 2);
                    for (const char* p : kPairs) {
                        if (two == p) { out.push_back(two); i += 2; pair = true; break; }
                    }
                }
                if (!pair) {
                    if (std::isalpha(static_cast<unsigned char>(w[i]))) {
                        out.emplace_back(1, w[i]);
                    }
                    ++i;
                }
            }
            return out;
        }

        // A shout's FIRST word, in dragon script. Its name is the dragon word
        // itself ("Fus"); `translation` carries the English ("Force").
        [[nodiscard]] std::vector<std::string> ShoutWord(RE::TESForm* a_form)
        {
            auto* shout = a_form ? a_form->As<RE::TESShout>() : nullptr;
            if (!shout) return {};
            auto* word = shout->variations[RE::TESShout::VariationIDs::kOne].word;
            const char* nm = word ? word->GetName() : nullptr;
            if (!nm || !*nm) return {};
            return Transliterate(nm);
        }

        // Draws a magic slot's mark. false = this slot has none, use the
        // medallion. Colour is the wheel's paper, like every other mark here.
        [[nodiscard]] bool DrawMagicMark(ImDrawList* a_dl, ImVec2 a_c, float a_sz,
                                         int a_group, int a_slot, int a_alpha,
                                         float a_sigilScale, float a_wordScale)
        {
            if (a_group != kMagic || a_slot < 0 || a_slot >= kSlots) return false;
            if (a_alpha <= 2) return false;
            const auto& fav = g_mag[a_slot];
            if (!fav.form) return false;
            const ImU32 col = (kPaper & 0x00FFFFFFu) |
                              (static_cast<ImU32>(a_alpha) << IM_COL32_A_SHIFT);

            if (fav.kind == FavKind::kSpell) {
                const char* key = SchoolOf(fav.form);
                // ★A POWER has no school -- GetAssociatedSkill returns kNone --
                // so it fell through to the old painted medallion and sat in a
                // ring of brush marks wearing a picture frame. It cannot borrow
                // the shout's dragon word either: a power has no word. Its own
                // mark, then, and every power shares it -- there is nothing
                // about a lesser power that a sigil could tell apart.
                if (!key && UsesVoiceSlot(fav.form)) key = "sym_power";
                const auto* ic = key ? Symbol(key) : nullptr;
                if (!ic) return false;
                const float h = a_sz * a_sigilScale;
                a_dl->AddImage(reinterpret_cast<ImTextureID>(ic->srv),
                    ImVec2(a_c.x - h, a_c.y - h), ImVec2(a_c.x + h, a_c.y + h),
                    ImVec2(0, 0), ImVec2(1, 1), col);
                return true;
            }
            if (fav.kind != FavKind::kShout) return false;

            // ★The signs are stored at a COMMON HEIGHT and their own widths, so
            // laying them out is a running x — no per-sign metrics table, and a
            // redrawn alphabet needs nothing changed here.
            const auto signs = ShoutWord(fav.form);
            if (signs.empty()) return false;
            float total = 0.0f;
            int   n = 0;
            for (const auto& s : signs) {
                if (const auto* ic = Symbol("glyph_" + s)) {
                    total += static_cast<float>(ic->w) / static_cast<float>((std::max)(ic->h, 1));
                    ++n;
                }
            }
            if (n == 0) return false;
            constexpr float kGap = 0.14f;      // of a sign's height
            total += kGap * static_cast<float>(n - 1);
            // fit the WHOLE word across the medallion's width
            const float hh = (a_sz * a_wordScale) / (total * 0.5f) * 0.5f;
            float x = a_c.x - total * hh;
            for (const auto& s : signs) {
                const auto* ic = Symbol("glyph_" + s);
                if (!ic) continue;
                const float w = static_cast<float>(ic->w) /
                                static_cast<float>((std::max)(ic->h, 1)) * hh * 2.0f;
                a_dl->AddImage(reinterpret_cast<ImTextureID>(ic->srv),
                    ImVec2(x, a_c.y - hh), ImVec2(x + w, a_c.y + hh),
                    ImVec2(0, 0), ImVec2(1, 1), col);
                x += w + kGap * hh * 2.0f;
            }
            return true;
        }

        // ---- what each slot holds ---------------------------------------------
        // ★★★ONE TABLE, ONE ROW PER GROUP. This used to be `if (a_group)` inside
        // eight separate functions, which is a two-group shape written eight
        // times -- adding a third meant finding and editing all eight, and
        // missing one is a bug that only shows on the new group. A descriptor
        // makes a group a THING you add rather than a branch you thread.
        //
        // ★What a slot HOLDS: a tab index, or kEmptySlot. It is a lookup, not
        // arithmetic -- the arrangement is the player's and it is stored (see
        // g_setOrder).
        // ★It was `slot - 1` for costumes and `slot` for presets, which was the
        // same statement as "the wheel cannot be arranged": a slot's meaning
        // was computed from its position, so its position could not be chosen.
        // ★kClearSlot appears here ONLY when reading an old save. The costume
        // wheel has no clear row any more -- EQUIP is that answer -- and
        // RebuildSetOrder removes any it finds. Do not reintroduce one on the
        // strength of this type: a value nothing writes is a value nothing
        // maintains.
        [[nodiscard]] int TabOf(int a_group, int a_slot)
        {
            if (a_slot < 0 || a_slot >= kSlots) return kEmptySlot;
            if (a_group != kPreset && a_group != kCostume) return kEmptySlot;
            if (!g_setInit) ResetSetOrder();
            return g_setOrder[a_group][a_slot];
        }

        struct GroupDesc
        {
            const char* title;                     // shown on the brush banner
            bool (*filled)(int slot);              // is there anything here at all
            bool (*eligible)(int slot);            // ...and may it be chosen now
            const char* (*name)(int slot);         // hub text
            bool (*current)(int slot);             // draws the tick
            void (*apply)(int slot);               // what letting go does
            // ★There is no "slot to open on". Every wheel opens with nothing
            // chosen -- see SeedCursor. This used to be a per-group function
            // and it stopped saying anything the day the last group answered
            // -1: a table of function pointers is for what differs BETWEEN
            // groups, and a row that reads the same in every column is a
            // second place for the rule to live.
            // what fills the slot's circle -- see namespace Art below
            const char* (*medallion)(int slot);    // weapon-type key, or null
            RE::TESBoundObject* (*face)(int slot); // item whose icon to draw
            // ★No "is this the nothing slot" here any more. It existed for the
            // costume wheel's clear row, which was drawn as a crossed circle
            // and stood beside an EQUIP tab meaning the same thing; EQUIP took
            // the job and the row went, leaving every group answering no. The
            // same fate as seed above, for the same reason.
            // ★A click, with the hand it came from. Every group answers now;
            // the hand is only meaningful for gear and magic, and PRESET /
            // COSTUME ignore it -- they have one answer, not two hands.
            void (*click)(int slot, bool leftHand);
            // ★Move a slot's contents to another slot. Null where there is
            // nothing to arrange at all.
            //
            // ★★from == to ASKS "may this slot be lifted?" and changes nothing.
            // The alternative was a second predicate beside this one, which is
            // the same rule written twice and one of them eventually wrong.
            //
            // ★It MAY refuse, and every caller still checks -- but as of the
            // arrangement move nothing does: all four wheels keep their own
            // layout over ten equal places, so every slot accepts. The return
            // value is kept because the question is real (a wheel whose order
            // is not its own would decline) and because the callers reading it
            // cost nothing. Do NOT read this as "the set wheels refuse" -- that
            // was true only while dragging moved real inventory tabs.
            bool (*reorder)(int from, int to);
        };

        // ★Declared here, defined further down: the art helpers need SlotKey and
        // FaceOf, which need Loadout, which reads better after the group logic.
        // A function's address is a link-time constant, so the table below can
        // be built from declarations alone.
        namespace Art
        {
            const char* presetMedallion(int s);
            const char* noMedallion(int s);
            RE::TESBoundObject* noFace(int s);
            RE::TESBoundObject* costumeFace(int s);
            RE::TESBoundObject* itemFace(int s);
            const char* itemMedallion(int s);
            const char* magicMedallion(int s);
        }

        namespace Preset
        {
            bool filled(int s) { return TabOf(kPreset, s) >= 0; }
            bool eligible(int s) { return filled(s); }
            const char* name(int s)
            {
                const int t = TabOf(kPreset, s);
                return t >= 0 ? Loadout::Name(t) : "";
            }
            bool current(int s)
            {
                const int t = TabOf(kPreset, s);
                return t >= 0 && Loadout::Active() == t;
            }
            void apply(int s)
            {
                const int t = TabOf(kPreset, s);
                if (t >= 0 && t != Loadout::Active()) Loadout::RequestSwitch(t);
            }
            // ★A click puts it on NOW, instead of waiting for the wheel to
            // close. Gear and magic have always worked that way; a preset had
            // to be selected and then released, which reads as the click having
            // done nothing. `apply` already refuses a preset that is current,
            // so clicking and then releasing on the same slot is one switch.
            // The hand is meaningless here -- a preset has one answer.
            void click(int s, bool) { apply(s); }
            bool reorder(int from, int to);
        }

        namespace Costume_
        {
            bool filled(int s) { return TabOf(kCostume, s) >= 0; }
            // ★★EQUIP is choosable HERE and nowhere else. Costume::CanBeTab
            // says no to it because a costume is read out of a tab's stored
            // list and EQUIP has none -- true, and beside the point: picking
            // EQUIP on this wheel does not read a list, it says "wear no
            // costume". The tab that means "your own gear" and the answer
            // "show my own gear" are the same answer.
            bool eligible(int s)
            {
                const int t = TabOf(kCostume, s);
                return t == 0 || (t > 0 && Costume::CanBeTab(t));
            }
            const char* name(int s)
            {
                const int t = TabOf(kCostume, s);
                return t >= 0 ? Loadout::Name(t) : "";
            }
            bool current(int s)
            {
                const int t = TabOf(kCostume, s);
                if (t < 0) return false;
                // ★EQUIP is on precisely when no costume is.
                return t == 0 ? (Costume::Tab() < 0) : Costume::IsTab(t);
            }
            void apply(int s)
            {
                const int t = TabOf(kCostume, s);
                if (t < 0) return;
                // ★Tab 0 -> -1: "wear EQUIP" is spelt "wear nothing over it".
                const int want = (t == 0) ? -1 : t;
                if (want != Costume::Tab()) Costume::SetTab(want);
            }
            void click(int s, bool) { apply(s); }   // same as PRESET above
            // ★PRESET and COSTUME arrange SEPARATELY, though they list the same
            // tabs. The set you reach for in a hurry and the look you reach for
            // are different preferences about the same things, and one wheel is
            // not evidence about the other.
            bool reorder(int from, int to);
        }

        namespace Items
        {
            // ★★A SLOT IS A PLACE, not a position in a packed list. Four
            // favourites used to mean four slots existed and the other six were
            // out of bounds -- so a drag toward one of them moved the ink and
            // left the icon behind, because the reorder quietly refused. The
            // ring always has ten places; some are empty.
            bool filled(int s) { return s >= 0 && s < kSlots && g_fav[s].form; }
            bool eligible(int s) { return filled(s); }
            const char* name(int s)
            {
                return filled(s) && g_fav[s].form ? g_fav[s].form->GetName() : "";
            }
            // ★"Currently on" means WORN here, not "the set that is active".
            // A potion is never current; a sword is while it is in a hand.
            bool current(int s);
            // ★Releasing does the ordinary thing: right hand, or use it if it
            // is not held in one. The two hands still need clicks -- a release
            // is one gesture and cannot say which hand -- but making the plain
            // case need a click too would have been a rule with no reason:
            // every other group applies on release, and the common item is one
            // you just want in your hand.
            // ★★Unless a click already acted. Clicking equips immediately, so
            // without this the release would equip a SECOND time -- and for a
            // potion that is two potions drunk for one visit to the wheel.
            void apply(int slot);
            void click(int slot, bool leftHand);
            bool reorder(int from, int to);
        }

        namespace Magic
        {
            bool filled(int s) { return s >= 0 && s < kSlots && g_mag[s].form; }
            bool eligible(int s) { return filled(s); }
            const char* name(int s)
            {
                return filled(s) && g_mag[s].form ? g_mag[s].form->GetName() : "";
            }
            bool current(int s);
            void apply(int slot);
            void click(int slot, bool leftHand);
            bool reorder(int from, int to);
        }

        constexpr GroupDesc kGroup[kGroups] = {
            // ★PRESET gets the drawn weapon medallion -- a sword captured in 3D
            // and squeezed into this circle is a hairline. COSTUME gets the real
            // ITEM icon, because a costume IS armour and the body piece is what
            // the player recognises; there is no weapon in it to name.
            { "PRESET", Preset::filled, Preset::eligible, Preset::name,
              Preset::current, Preset::apply,
              Art::presetMedallion, Art::noFace, Preset::click, Preset::reorder },
            { "COSTUME", Costume_::filled, Costume_::eligible, Costume_::name,
              Costume_::current, Costume_::apply,
              Art::noMedallion, Art::costumeFace, Costume_::click, Costume_::reorder },
            { "GEAR", Items::filled, Items::eligible, Items::name,
              Items::current, Items::apply,
              Art::itemMedallion, Art::itemFace, Items::click, Items::reorder },
            { "MAGIC", Magic::filled, Magic::eligible, Magic::name,
              Magic::current, Magic::apply,
              Art::magicMedallion, Art::noFace, Magic::click, Magic::reorder },
        };

        [[nodiscard]] const GroupDesc& G(int a_group)
        {
            return kGroup[std::clamp(a_group, 0, kGroups - 1)];
        }

        [[nodiscard]] bool Filled(int a_group, int a_slot)
        {
            if (a_slot < 0 || a_slot >= kSlots) return false;
            return G(a_group).filled(a_slot);
        }

        [[nodiscard]] bool Eligible(int a_group, int a_slot)
        {
            if (a_slot < 0 || a_slot >= kSlots) return false;
            return G(a_group).eligible(a_slot);
        }

        // Is there anything in this group at all? ★"filled", not "eligible":
        // a costume tab that cannot be chosen right now (it is the one you are
        // wearing) still makes the list worth showing.
        [[nodiscard]] bool AnyFilled(int a_group)
        {
            for (int i = 0; i < kSlots; ++i) {
                if (G(a_group).filled(i)) return true;
            }
            return false;
        }

        [[nodiscard]] const char* NameOf(int a_group, int a_slot)
        {
            return (a_slot >= 0 && a_slot < kSlots) ? G(a_group).name(a_slot) : "";
        }

        [[nodiscard]] bool IsCurrent(int a_group, int a_slot)
        {
            return (a_slot >= 0 && a_slot < kSlots) && G(a_group).current(a_slot);
        }

        // ---- weapon-type medallions -------------------------------------------
        constexpr const char* kMedDir = "Data/SKSE/Plugins/GridInventory_fallback/";

        // ★File-scope, not function-local: the reload has to be able to reach
        // it. As a local static there was no way to empty it from anywhere,
        // which is exactly what happened -- see Wheeler.h ReloadMedallions.
        std::unordered_map<std::string, IconCache::Icon> g_medallions;

        [[nodiscard]] const IconCache::Icon* Medallion(const char* a_key)
        {
            if (!a_key || !*a_key) return nullptr;
            auto it = g_medallions.find(a_key);
            if (it == g_medallions.end()) {
                IconCache::Icon ic{};
                IconCache::LoadPngTexture(
                    std::string(kMedDir) + "wt_" + a_key + ".png", ic);
                it = g_medallions.emplace(a_key, ic).first;
            }
            return it->second.srv ? &it->second : nullptr;
        }

        // ★★Keywords FIRST. GetWeaponType() is an ANIMATION type and it files
        // battleaxes and warhammers under the same value, so it can never tell
        // apart the pair a player is most likely to own together.
        [[nodiscard]] const char* WeaponKey(RE::TESObjectWEAP* a_w)
        {
            if (!a_w) return nullptr;
            if (const auto* kf = skyrim_cast<const RE::BGSKeywordForm*>(a_w)) {
                for (std::uint32_t i = 0; i < kf->numKeywords; ++i) {
                    auto* kw = kf->keywords ? kf->keywords[i] : nullptr;
                    const char* id = kw ? kw->GetFormEditorID() : nullptr;
                    if (!id) continue;
                    struct { const char* kw; const char* key; } kMap[] = {
                        { "WeapTypeGreatsword", "greatsword" },
                        { "WeapTypeBattleaxe", "battleaxe" },
                        { "WeapTypeWarhammer", "warhammer" },
                        { "WeapTypeWarAxe", "waraxe" },
                        { "WeapTypeDagger", "dagger" },
                        { "WeapTypeSword", "sword" },
                        { "WeapTypeMace", "mace" },
                        { "WeapTypeCrossbow", "crossbow" },
                        { "WeapTypeBow", "bow" },
                        { "WeapTypeStaff", "staff" },
                    };
                    for (const auto& m : kMap) {
                        if (std::strcmp(id, m.kw) == 0) return m.key;
                    }
                }
            }
            using T = RE::WEAPON_TYPE;
            switch (a_w->GetWeaponType()) {
            case T::kOneHandSword:  return "sword";
            case T::kOneHandDagger: return "dagger";
            case T::kOneHandAxe:    return "waraxe";
            case T::kOneHandMace:   return "mace";
            case T::kTwoHandSword:  return "greatsword";
            case T::kTwoHandAxe:    return "battleaxe";
            case T::kBow:           return "bow";
            case T::kCrossbow:      return "crossbow";
            case T::kStaff:         return "staff";
            default:                return "unarmed";
            }
        }

        // The item whose captured icon stands for a whole COSTUME. ★A costume is
        // armour, so a weapon-type medallion says nothing about it -- the body
        // piece is what the player recognises. Never null-by-assumption: a set
        // can be anything, so this falls through to the first entry and the
        // caller still handles "nothing".
        [[nodiscard]] RE::TESBoundObject* FaceOf(int a_tab)
        {
            RE::TESBoundObject* first = nullptr;
            RE::TESBoundObject* body = nullptr;
            RE::TESBoundObject* head = nullptr;
            for (const auto id : Loadout::FormsOf(a_tab)) {
                auto* f = RE::TESForm::LookupByID(id);
                auto* o = f ? f->As<RE::TESBoundObject>() : nullptr;
                if (!o) continue;
                if (!first) first = o;
                if (auto* a = o->As<RE::TESObjectARMO>()) {
                    using S = RE::BGSBipedObjectForm::BipedObjectSlot;
                    if (!body && a->HasPartOf(S::kBody)) body = a;
                    if (!head && (a->HasPartOf(S::kHair) || a->HasPartOf(S::kHead) ||
                                  a->HasPartOf(S::kCirclet))) head = a;
                }
            }
            return body ? body : (head ? head : first);
        }

        // Gather the starred items, in inventory order, up to the wheel's size.
        // ★Called once per open, on the game thread. It walks the inventory and
        // reads ExtraHotkey, which is where the engine keeps the star -- the
        // same mark the vanilla favourites menu shows.
        void CollectFavorites()
        {
            // ★★★CLEAR THE ARRAY, not just the count. This wrote over the first
            // N entries and left the rest of the previous visit in place --
            // and nothing downstream reads the count: `filled` asks whether
            // g_fav[s].form is set, UseFav indexes straight in. Unstar three of
            // five items and the two that scrolled off were still drawn, still
            // clickable, still handed to Equip as items no longer owned.
            // ★It hid behind ApplyOrder: once the player has dragged anything,
            // that function rebuilds the array from scratch and the stale tail
            // disappears. So it only ever showed for someone who had never
            // rearranged their wheel -- a self-healing bug, invisible to
            // whoever tests it after using the feature once.
            for (auto& f : g_fav) f = {};
            for (auto& f : g_mag) f = {};
            g_favN = 0;
            g_starredAll[0].clear();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) return;   // see ProcessCast
            auto inv = player->GetInventory();
            for (auto& [obj, data] : inv) {
                // ★no early break: the walk has to SEE every star even when it
                // can only show ten of them (see g_starredAll).
                if (!obj || data.first <= 0 || !data.second) continue;
                // ★Skip what the costume system wears on the player's behalf.
                // An anchor is not the player's property anywhere else either.
                if (Costume::IsAnchor(obj)) continue;
                auto* entry = data.second.get();
                if (!entry || !entry->extraLists) continue;
                // ★★★PREFER A SPARE'S SIGNATURE, NEVER THE WORN ONE.
                //
                // This signature exists to be handed to EQUIP -- and the equip
                // resolver refuses worn lists by design (a worn list belongs to
                // the copy already on the body). The star, however, MOVES onto
                // the worn list the moment the item is put on: the engine
                // carries ExtraHotkey across, which PoolHasStar relies on
                // elsewhere.
                //
                // So the first starred list is the WORN one for anything the
                // player has ever worn, and every later equip from the wheel was
                // refused -- "[EQUIP] named unit gone -- equip skipped", a dozen
                // times in one reporter's session, while unequipping kept
                // working because that path resolves through WornExtraMatching
                // instead. "Only equip is broken" was the shape of it.
                //
                // Take the first NON-WORN starred list; fall back to the worn
                // one only when there is nothing else, where the click means
                // unequip anyway.
                RE::ExtraDataList* star = nullptr;
                RE::ExtraDataList* wornStar = nullptr;
                for (auto* xl : *entry->extraLists) {
                    if (!xl || !xl->HasType<RE::ExtraHotkey>()) continue;
                    if (xl->HasType<RE::ExtraWorn>() ||
                        xl->HasType<RE::ExtraWornLeft>()) {
                        if (!wornStar) wornStar = xl;
                        continue;
                    }
                    star = xl;
                    break;
                }
                if (!star) star = wornStar;
                if (star) g_starredAll[0].insert(obj->GetFormID());
                if (star && g_favN < kSlots) {
                    // ★★★KEEP THE SIGNATURE OF THE UNIT THAT CARRIES THE STAR.
                    // The star sits on one ExtraDataList and we are holding it
                    // -- but sig was written as 0, which means "no instance" all
                    // the way down to ActorEquipManager, and there the ENGINE
                    // picks a copy. Star the tempered sword of two and the wheel
                    // could hand you the plain one. Equip's own header warns
                    // about this coin flip; the warning is for callers who have
                    // no instance in hand, and this one does.
                    // ★A pool, not a list: the sig is what distinguishes the
                    // tempered pool from the plain one, which is exactly the
                    // grain the favourites system works at.
                    std::uint16_t starUid = 0;
                    if (const auto* xu = star->GetByType<RE::ExtraUniqueID>()) {
                        starUid = xu->uniqueID;
                    }
                    // ★"worn" includes the SECOND RING: a carrier wears its
                    // effect while the ring stands in the pack, so IsWorn says
                    // no -- and the wheel then showed no check mark and
                    // offered equip instead of unequip for a ring plainly on
                    // the doll (user report).
                    const bool wornHere = entry->IsWorn() ||
                                          DualRing::Second() == obj;
                    g_fav[g_favN] = { obj, obj, FavKind::kItem,
                                      Grid::InstanceSigOf(star), -1,
                                      data.first, wornHere, starUid };
                    ++g_favN;   // one tile per form: a starred pool is one thing
                }
            }
            // ★...and the magic side of the same star. The engine keeps spells
            // and shouts in their own list; to the player it was the same
            // gesture on the same screen, so the wheel does not distinguish.
            g_magN = 0;
            g_starredAll[1].clear();
            if (auto* mf = RE::MagicFavorites::GetSingleton()) {
                for (auto* form : mf->spells) {
                    if (!form) continue;   // ★no early break -- see g_starredAll
                    FavKind k;
                    if (form->Is(RE::FormType::Spell))      k = FavKind::kSpell;
                    else if (form->Is(RE::FormType::Shout)) k = FavKind::kShout;
                    else continue;   // scrolls and the like arrive as items above
                    g_starredAll[1].insert(form->GetFormID());
                    if (g_magN >= kSlots) continue;   // seen, but no place to show it
                    // ★"Currently on" for magic is the hand, not the pack. A
                    // shout sits in the voice slot; a spell in either hand, and
                    // both count -- the tick says "this is what you have out",
                    // and which hand is the player's business.
                    bool on = false;
                    auto& rt = player->GetActorRuntimeData();
                    if (UsesVoiceSlot(form)) {
                        on = rt.selectedPower == form;
                    } else {
                        for (auto* sel : rt.selectedSpells) {
                            if (sel == form) { on = true; break; }
                        }
                    }
                    g_mag[g_magN] = { form, nullptr, k, 0, -1, 1, on };
                    ++g_magN;
                }
            }
            ApplyOrder(g_fav, g_favN, 0);
            ApplyOrder(g_mag, g_magN, 1);
        }

        [[nodiscard]] const char* SlotKey(int a_group, int a_slot)
        {
            // ★< 0 covers both an empty place and the "no costume" row, neither
            // of which has gear to name. It used to test the slot NUMBER for
            // the clear row, which stopped being true the moment that row could
            // be dragged somewhere else.
            const int tab = TabOf(a_group, a_slot);
            if (tab < 0 || tab >= Loadout::Count()) return nullptr;
            for (const auto id : Loadout::FormsOf(tab)) {
                auto* f = RE::TESForm::LookupByID(id);
                if (!f) continue;
                if (f->Is(RE::FormType::Spell)) return "spell";
                if (auto* w = f->As<RE::TESObjectWEAP>()) {
                    if (const char* k = WeaponKey(w)) return k;
                }
            }
            return "unarmed";
        }

        // ---- what a slot DRAWS, per group --------------------------------------
        // ★The last branch on the group index. A slot's circle holds exactly one
        // of three things and each group answers differently: PRESET names a
        // weapon medallion and COSTUME hands over an item to draw its captured
        // icon. (There was a third: a crossed circle for the costume wheel's
        // clear row. EQUIP took that job and the row went with it.)
        // Kept as separate questions rather than one tagged union because
        // the draw code already had three separate paths -- this only moves the
        // decision to the table.
        namespace Art
        {
            const char* presetMedallion(int s) { return SlotKey(kPreset, s); }
            const char* noMedallion(int) { return nullptr; }
            RE::TESBoundObject* noFace(int) { return nullptr; }
            RE::TESBoundObject* costumeFace(int s)
            {
                const int t = TabOf(kCostume, s);
                return t >= 0 ? FaceOf(t) : nullptr;
            }
            RE::TESBoundObject* itemFace(int s)
            {
                // ★Only real items have a captured sprite. A spell is not an
                // object the engine can stand in a menu and photograph, so it
                // falls through to the medallion below.
                return (s >= 0 && s < kSlots) ? g_fav[s].obj : nullptr;
            }
            const char* itemMedallion(int) { return nullptr; }
            // ★A shout borrows the spell medallion. Wrong drawing beats no
            // drawing: an empty circle says "this slot is broken", and a shout
            // IS magic to anyone reading the wheel at speed.
            const char* magicMedallion(int) { return "spell"; }
        }

        // ★★READ, never ask. "Is it worn" is answered by the snapshot, not by
        // walking the inventory: this is called for every slot of every frame,
        // and GetInventory walks the whole pack each time. Ten slots at sixty
        // frames is six hundred inventory walks a second to draw ten ticks.
        // The snapshot is refreshed on a timer instead -- see Tick.
        bool Items::current(int s) { return filled(s) && g_fav[s].worn; }


        // ---- slow motion --------------------------------------------------------
        void EnterSlowMotion()
        {
            if (g_slowed) return;
            g_savedTimeMult = RE::BSTimer::QGlobalTimeMultiplier();
            if (auto* t = RE::BSTimer::GetSingleton()) {
                t->SetGlobalTimeMultiplier(g_slowFactor, true);
                g_slowed = true;
            }
        }

        void LeaveSlowMotion()
        {
            if (!g_slowed) return;
            if (auto* t = RE::BSTimer::GetSingleton()) {
                // ★Restore what was there, not 1.0 -- another mod may own this.
                t->SetGlobalTimeMultiplier(g_savedTimeMult, true);
            }
            g_slowed = false;
        }

        // ---- camera lock ---------------------------------------------------------
        // ★★NO ENGINE STATE IS FLIPPED. ControlMap::ToggleControls(kLooking) is
        // the obvious call and it failed in both directions at once: the camera
        // still turned while the wheel was up, AND a run that missed the restore
        // left looking dead afterwards. enabledControls is a bitfield the whole
        // game shares. A predicate on the look handler owns nothing, so nothing
        // can be left behind.
        // ★★★WHERE THE GAME ITSELF READS INPUT. Zeroing events in our own sink
        // does nothing for movement, and the reason is ordering: sinks are
        // called in registration order, the game's PlayerControls registered at
        // startup, ours much later -- by the time we blank an event the player
        // has already taken a step. Hooking PlayerControls' own entry point
        // removes the race entirely: whatever we do here happens before the
        // game has looked, every time, because it IS the game looking.
        // ★It mutes rather than skipping the original. Skipping would leave the
        // engine holding whatever was pressed when the wheel opened -- walk,
        // open, and the character walks forever. Muted events still reach it,
        // and a held key reads as released, so it tidies itself up.
        struct InputLock
        {
            // ★★Blank, call, put back -- the same discipline as MenuLock and for
            // the same reason. The event is ONE object shared down the whole
            // sink chain: leaving it zeroed would silence every listener after
            // this one, and one of those is us reading the wheel's own
            // controls. Only the call in the middle is meant to go deaf.
            static RE::BSEventNotifyControl ProcessEvent(
                RE::BSTEventSink<RE::InputEvent*>* a_this,
                RE::InputEvent* const*             a_event,
                RE::BSTEventSource<RE::InputEvent*>* a_src)
            {
                struct Saved { RE::ButtonEvent* b; float v; };
                struct SavedT { RE::ThumbstickEvent* t; float x, y; };
                Saved  saved[16]{};
                SavedT savedT[4]{};
                int    n = 0, nt = 0;
                if (g_open && a_event) {
                    for (auto* e = *a_event; e; e = e->next) {
                        // ★★★BUTTONS, plus the LEFT stick. Mouse and RIGHT
                        // stick MOTION is left alone, because that motion is
                        // how the wheel is aimed -- blanking it here (and it
                        // was blanked, without a restore) left the player able
                        // to step slots with the scroll wheel and nothing
                        // else, which is why dragging did not feel like
                        // dragging.
                        // ★(1.3.2) The LEFT stick is MOVEMENT, and movement is
                        // blocked while the wheel is up (header contract). It
                        // used to be the aim stick, which is why stick motion
                        // was historically exempt wholesale; the aim lives on
                        // the RIGHT stick now, and the camera hooks already
                        // refuse that one further in. Blank-call-restore, so
                        // our own sink (later in the chain) still reads it.
                        if (auto* t = e->AsThumbstickEvent()) {
                            if (!t->IsRight() && nt < 4) {
                                savedT[nt++] = { t, t->xValue, t->yValue };
                                t->xValue = 0.0f;
                                t->yValue = 0.0f;
                            }
                            continue;
                        }
                        auto* b = e->AsButtonEvent();
                        if (!b) continue;
                        // ★Never blank what cannot be put back. If the save
                        // slots run out, this event is left alone -- a button
                        // muted without a restore stays muted for every sink
                        // after us, and if it is the hotkey the wheel can never
                        // open again.
                        if (n >= 16) continue;
                        saved[n++] = { b, b->Value() };
                        b->GetRuntimeData().value = 0.0f;
                    }
                }
                const auto r = _ProcessEvent(a_this, a_event, a_src);
                for (int i = 0; i < n; ++i) saved[i].b->GetRuntimeData().value = saved[i].v;
                for (int i = 0; i < nt; ++i) {
                    savedT[i].t->xValue = savedT[i].x;
                    savedT[i].t->yValue = savedT[i].y;
                }
                return r;
            }
            static void Install()
            {
                // [0] is the BSTEventSink<InputEvent*> base -- PlayerControls
                // inherits four sinks and this is the first. Slot 1 is
                // ProcessEvent (0 is the destructor).
                REL::Relocation<std::uintptr_t> v{ RE::VTABLE_PlayerControls[0] };
                _ProcessEvent = v.write_vfunc(0x1, ProcessEvent);
            }
            static inline REL::Relocation<decltype(ProcessEvent)> _ProcessEvent;
        };

        // ★★★AND THE MENU SIDE OF THE SAME DOOR. PlayerControls drives the
        // world; MenuControls is what turns a key into "open the favourites
        // menu". Closing that menu after it opened was not enough: opening it
        // switches the input context, and the switch reaches us as a key-UP --
        // so the wheel closed itself the instant it appeared. The menu has to
        // never open, not open-and-go.
        // ★It mutes the hotkey even while the wheel is CLOSED. That is the
        // whole point: the press that opens the wheel is the same press that
        // would open the menu, and by the time the wheel is up it is too late.
        [[nodiscard]] bool InCombo(bool a_pad, std::uint32_t a_code);   // defined below

        // ★Is a menu already holding input? Both the hotkey path and the
        // pre-MenuControls blanking ask this, and they have to agree: one
        // refusing to open while the other still eats the key is precisely
        // the state that swallowed the favourite toggle.
        [[nodiscard]] bool AnotherMenuOwnsInput()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return false;
            if (ui->GameIsPaused()) return true;
            for (const auto& m : ui->menuStack) {
                if (m && m->menuFlags.all(RE::UI_MENU_FLAGS::kUsesMenuContext)) {
                    return true;
                }
            }
            return false;
        }

        struct MenuLock
        {
            // ★★★BLANK, CALL, PUT IT BACK. Muting outright was wrong and the
            // symptom was total: the hotkey did nothing at all. An event is one
            // object shared by every sink in the chain, so zeroing it here
            // zeroes it for whoever comes after -- including us. The menu must
            // not see the key; everyone else still must.
            static RE::BSEventNotifyControl ProcessEvent(
                RE::BSTEventSink<RE::InputEvent*>*   a_this,
                RE::InputEvent* const*               a_event,
                RE::BSTEventSource<RE::InputEvent*>* a_src)
            {
                struct Saved { RE::ButtonEvent* b; float v; };
                Saved saved[16]{};
                int   n = 0;
                // ★★★AND NOTHING AT ALL WHEN THE WHEEL IS OFF. This is the one
                // hook that acts while the wheel is CLOSED -- it is what stops
                // the favourites menu from ever appearing, rather than letting
                // it open and shutting it again -- so it is also the one that
                // has to be told the wheel is gone. The others all key off
                // g_open and go quiet by themselves.
                // ★This was missed when the switch went in, and the symptom was
                // exactly what you would expect: turning the wheel off restored
                // the vanilla menu's right to open, while the key that opens it
                // was still being erased one layer earlier.
                // ★★★...AND NOTHING WHILE THE PLAYER IS TRANSFORMED, for the
                // same reason and one layer deeper than anyone looked.
                //
                // This hook is where the beast-form yield was quietly being
                // undone. The wheel's OnButton stood aside, the menu intercept
                // in main.cpp stopped closing the menu -- and the key still did
                // nothing, because the menu never got to SEE it: this blanks
                // the hotkey at MenuControls whether the wheel is open or not,
                // which is precisely its job the rest of the time. Measured
                // either way: with the wheel off, Q in vampire-lord form opens
                // the vanilla menu and it stays up (four seconds in the log);
                // with the wheel on, nothing at all.
                //
                // So the stand-down has to reach here too. Three layers, one
                // question -- ask it in all three or the answer never arrives.
                // ★★★...AND NOTHING WHILE A MENU ALREADY OWNS INPUT.
                //
                // This blanks the key BEFORE MenuControls sees it, and an open
                // menu is fed from AFTER MenuControls -- our own inventory
                // takes its keys as Scaleform events, not from the window
                // proc. A key erased here therefore never reaches the menu at
                // all.
                //
                // The wheel's hotkey follows the game's FAVORITES binding
                // (AdoptFavoritesKey). At the vanilla default that is Q and
                // nothing collides. Rebind favourites to F -- which plenty of
                // players do, since F is what stars an item in the inventory
                // -- and the wheel starts eating the star key: with the wheel
                // ON you could favourite nothing, with it OFF everything
                // worked. Reported, then reproduced exactly by making that one
                // rebind.
                //
                // Nothing is lost by standing down here: the wheel cannot open
                // over a menu that owns input (SomethingElseOwnsTheKey already
                // refuses), and the vanilla favourites menu -- the thing this
                // blanking exists to suppress -- cannot appear over one
                // either. The key was being erased for nobody.
                if (g_enabled && !YieldingToVanilla() && !AnotherMenuOwnsInput() &&
                    a_event) {
                    for (auto* e = *a_event; e && n < 16; e = e->next) {
                        auto* b = e->AsButtonEvent();
                        if (!b) continue;
                        const auto dev = b->GetDevice();
                        const bool pad = dev == RE::INPUT_DEVICE::kGamepad;
                        // The hotkey always; everything else once the wheel is
                        // up, so no menu can open behind it.
                        // ★★★A MOUSE BUTTON IS NOT A KEYBOARD SCAN CODE. The
                        // pad/not-pad split sent mouse events into the keyboard
                        // combo test, where IDCode 9 (wheel-down) matched a
                        // combo bound to '8' (scan code 9) -- and this hook runs
                        // with the wheel CLOSED, so the collision killed
                        // wheel-scrolling in every vanilla menu. Only ask the
                        // combo about the device the combo is written in.
                        if (!g_open) {
                            if (dev != RE::INPUT_DEVICE::kKeyboard &&
                                dev != RE::INPUT_DEVICE::kGamepad) continue;
                            if (!InCombo(pad, b->GetIDCode())) continue;
                        }
                        if (n >= 16) continue;   // see InputLock: no blank without a restore
                        saved[n++] = { b, b->Value() };
                        b->GetRuntimeData().value = 0.0f;
                    }
                }
                const auto r = _ProcessEvent(a_this, a_event, a_src);
                for (int i = 0; i < n; ++i) saved[i].b->GetRuntimeData().value = saved[i].v;
                return r;
            }
            static void Install()
            {
                REL::Relocation<std::uintptr_t> v{ RE::VTABLE_MenuControls[0] };
                _ProcessEvent = v.write_vfunc(0x1, ProcessEvent);
            }
            static inline REL::Relocation<decltype(ProcessEvent)> _ProcessEvent;
        };

        struct LookLock
        {
            static bool CanProcess(RE::LookHandler* a_this, RE::InputEvent* a_e)
            {
                return g_open ? false : _CanProcess(a_this, a_e);
            }
            static void ProcessThumbstick(RE::LookHandler* a_this,
                RE::ThumbstickEvent* a_e, RE::PlayerControlsData* a_d)
            {
                if (!g_open) _ProcessThumbstick(a_this, a_e, a_d);
            }
            static void ProcessMouseMove(RE::LookHandler* a_this,
                RE::MouseMoveEvent* a_e, RE::PlayerControlsData* a_d)
            {
                if (!g_open) _ProcessMouseMove(a_this, a_e, a_d);
            }
            static void Install()
            {
                REL::Relocation<std::uintptr_t> v{ RE::VTABLE_LookHandler[0] };
                _CanProcess = v.write_vfunc(0x1, CanProcess);
                _ProcessThumbstick = v.write_vfunc(0x2, ProcessThumbstick);
                _ProcessMouseMove = v.write_vfunc(0x3, ProcessMouseMove);
            }
            static inline REL::Relocation<decltype(CanProcess)>        _CanProcess;
            static inline REL::Relocation<decltype(ProcessThumbstick)> _ProcessThumbstick;
            static inline REL::Relocation<decltype(ProcessMouseMove)>  _ProcessMouseMove;
        };

        // ★The wheel scroll steps slots, so the camera must not also zoom.
        // ★HorseCameraState has its OWN vtable even though it derives from
        // ThirdPersonState -- hook one and the other keeps zooming.
        struct ZoomLock
        {
            using Fn = void(RE::ThirdPersonState*, RE::ButtonEvent*, RE::PlayerControlsData*);
            static void Third(RE::ThirdPersonState* t, RE::ButtonEvent* e, RE::PlayerControlsData* d)
            {
                if (!g_open) _third(t, e, d);
            }
            static void Horse(RE::ThirdPersonState* t, RE::ButtonEvent* e, RE::PlayerControlsData* d)
            {
                if (!g_open) _horse(t, e, d);
            }
            static void Install()
            {
                REL::Relocation<std::uintptr_t> a{ RE::VTABLE_ThirdPersonState[1] };
                _third = a.write_vfunc(0x4, Third);
                REL::Relocation<std::uintptr_t> b{ RE::VTABLE_HorseCameraState[1] };
                _horse = b.write_vfunc(0x4, Horse);
            }
            static inline REL::Relocation<Fn> _third;
            static inline REL::Relocation<Fn> _horse;
        };

        // ★★Left click swaps the list -- and it ALSO swung the sword, because a
        // sink returning "handled" does not stop the game. It never did: that is
        // the same reason the camera needed a hook rather than a return value.
        // Attack and block both live on this one handler.
        struct AttackLock
        {
            static bool CanProcess(RE::AttackBlockHandler* a_this, RE::InputEvent* a_e)
            {
                return g_open ? false : _CanProcess(a_this, a_e);
            }
            static void ProcessButton(RE::AttackBlockHandler* a_this,
                RE::ButtonEvent* a_e, RE::PlayerControlsData* a_d)
            {
                if (!g_open) _ProcessButton(a_this, a_e, a_d);
            }
            static void Install()
            {
                REL::Relocation<std::uintptr_t> v{ RE::VTABLE_AttackBlockHandler[0] };
                _CanProcess = v.write_vfunc(0x1, CanProcess);
                _ProcessButton = v.write_vfunc(0x4, ProcessButton);
            }
            static inline REL::Relocation<decltype(CanProcess)>   _CanProcess;
            static inline REL::Relocation<decltype(ProcessButton)> _ProcessButton;
        };

        // ---- the desaturate pass ---------------------------------------------
        // ★★The screen is copied and redrawn through a luminance shader. There
        // is no blend state that computes a per-pixel luminance, so a translucent
        // grey plate is the closest fixed-function can get -- and that washes the
        // picture out rather than draining it. The machinery for a custom shader
        // was already here (the grid's silhouette pass compiles one at runtime),
        // so this is one more of the same and not a new dependency.
        //
        // ★If any step fails -- no render target, a format we cannot match, the
        // shader refusing to compile -- the flag drops and Draw() falls back to a
        // plain dark scrim. A menu that will not open is worse than a menu that
        // does not drain the colour.
        struct Grey
        {
            static inline ID3D11PixelShader* ps = nullptr;
            static inline ID3D11PixelShader* prevPs = nullptr;
            static inline ID3D11Texture2D* tex = nullptr;
            static inline ID3D11ShaderResourceView* srv = nullptr;
            static inline DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            static inline UINT w = 0, h = 0;
            static inline bool failed = false;

            static void MakeShader(ID3D11Device* dev)
            {
                if (ps || failed) return;
                static const char kSrc[] =
                    "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR0;"
                    "               float2 uv : TEXCOORD0; };\n"
                    "sampler sampler0;\n"
                    "Texture2D texture0;\n"
                    "float4 main(PS_IN i) : SV_Target\n"
                    "{\n"
                    "    float3 c = texture0.Sample(sampler0, i.uv).rgb;\n"
                    "    float  y = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
                    // ★★LIFTED toward mid grey, not dimmed. Draining the colour
                    // out of a night scene leaves DARK grey, and the wheel is
                    // black ink -- so the first pass made the very thing it was
                    // meant to reveal harder to see. What the wheel needs is not
                    // less colour, it is a PALE FLOOR to stand on. This maps the
                    // whole range into 0.38..0.80: a cave comes up, a snowfield
                    // comes down, and black reads against either.
                    "    y = saturate(y) * 0.42 + 0.38;\n"
                    "    return float4(y, y, y, i.col.a);\n"
                    "}\n";
                ID3DBlob* blob = nullptr;
                ID3DBlob* err = nullptr;
                if (SUCCEEDED(D3DCompile(kSrc, sizeof(kSrc) - 1, nullptr, nullptr,
                                         nullptr, "main", "ps_4_0", 0, 0, &blob, &err))
                    && blob) {
                    dev->CreatePixelShader(blob->GetBufferPointer(),
                                           blob->GetBufferSize(), nullptr, &ps);
                } else {
                    failed = true;
                    SKSE::log::warn("[WHEEL] greyscale PS failed: {}",
                        err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                }
                if (blob) blob->Release();
                if (err) err->Release();
            }

            static void Release()
            {
                if (srv) { srv->Release(); srv = nullptr; }
                if (tex) { tex->Release(); tex = nullptr; }
            }

            // Runs as a draw callback, just before the full-screen image.
            static void CaptureCB(const ImDrawList*, const ImDrawCmd*)
            {
                auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
                auto* dev = data ? reinterpret_cast<ID3D11Device*>(data->forwarder) : nullptr;
                auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
                if (!dev || !ctx) { failed = true; return; }
                MakeShader(dev);
                if (!ps) return;

                // ★★NOT the currently bound render target. At the point an
                // IMenu's PostDisplay runs, whatever is bound is some
                // intermediate the UI pass is using -- copying it gave a BLACK
                // screen, because there is no screen in it. The engine keeps
                // the swap chain's own texture in renderTargets[], indexed by
                // the render window, and that is the picture the player sees.
                // ★★Ask the SWAP CHAIN, not the render-target table. The table
                // was the obvious place and it came back empty on this runtime
                // (logged: "grey source = none"), because the entry the window
                // points at is not where the presented image lives. The swap
                // chain always has buffer 0, by contract, and it is the thing
                // the player is about to see.
                auto& win = data->renderWindows[0];
                ID3D11Texture2D* src = nullptr;
                bool ownsSrc = false;
                const char* how = "none";

                if (auto* sc = reinterpret_cast<IDXGISwapChain*>(win.swapChain)) {
                    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                reinterpret_cast<void**>(&src))) && src) {
                        ownsSrc = true;          // GetBuffer AddRefs
                        how = "swapchain";
                    }
                }
                if (!src) {
                    if (auto* rv = reinterpret_cast<ID3D11RenderTargetView*>(win.renderView)) {
                        ID3D11Resource* r = nullptr;
                        rv->GetResource(&r);
                        if (r) {
                            r->QueryInterface(__uuidof(ID3D11Texture2D),
                                              reinterpret_cast<void**>(&src));
                            r->Release();
                            if (src) { ownsSrc = true; how = "renderView"; }
                        }
                    }
                }
                if (!src) {
                    const auto idx = static_cast<std::size_t>(win.swapChainRenderTarget);
                    if (idx < RE::RENDER_TARGET::kTOTAL) {
                        src = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[idx].texture);
                        if (src) how = "rtTable";
                    }
                }

                static bool said = false;
                if (!said) {
                    said = true;
                    SKSE::log::info("[WHEEL] grey source = {} (sc={} rv={} rtIdx={})",
                        how, win.swapChain != nullptr, win.renderView != nullptr,
                        static_cast<int>(win.swapChainRenderTarget));
                }
                if (!src) { failed = true; SKSE::log::warn("[WHEEL] no swap chain RT"); return; }

                ID3D11RenderTargetView* rtv = nullptr;
                ID3D11DepthStencilView* dsv = nullptr;
                ctx->OMGetRenderTargets(1, &rtv, &dsv);
                {
                    D3D11_TEXTURE2D_DESC sd{};
                    src->GetDesc(&sd);
                    // ★Match the source EXACTLY. CopyResource refuses anything
                    // else, and with frame generation in the chain the back
                    // buffer is R10G10B10A2 rather than the 8-bit anyone
                    // assumes -- the icon capture already learned that one.
                    if (!tex || w != sd.Width || h != sd.Height || fmt != sd.Format) {
                        Release();
                        D3D11_TEXTURE2D_DESC td = sd;
                        td.Usage = D3D11_USAGE_DEFAULT;
                        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        td.CPUAccessFlags = 0;
                        td.MiscFlags = 0;
                        td.MipLevels = 1;
                        td.SampleDesc.Count = 1;
                        td.SampleDesc.Quality = 0;
                        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) &&
                            SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &srv))) {
                            w = sd.Width; h = sd.Height; fmt = sd.Format;
                        } else {
                            Release();
                            failed = true;
                            SKSE::log::warn("[WHEEL] grey target {}x{} fmt {} refused",
                                sd.Width, sd.Height, static_cast<int>(sd.Format));
                        }
                    }
                    if (sd.SampleDesc.Count != 1) {
                        // ★Multisampled: CopyResource will not take it, and a
                        // silent skip leaves a black texture on screen. Say so
                        // and drop to the plain plate instead.
                        failed = true;
                        SKSE::log::warn("[WHEEL] swap chain is {}x MSAA -- no grey pass",
                            sd.SampleDesc.Count);
                    }
                    if (tex && sd.SampleDesc.Count == 1) {
                        // ★Unbind first. Copying out of a resource that is still
                        // bound as the render target is a read/write hazard.
                        ctx->OMSetRenderTargets(0, nullptr, nullptr);
                        ctx->CopyResource(tex, src);
                        if (rtv) ctx->OMSetRenderTargets(1, &rtv, dsv);
                    }
                }
                if (ownsSrc && src) src->Release();
                if (rtv) rtv->Release();
                if (dsv) dsv->Release();

                ctx->PSGetShader(&prevPs, nullptr, nullptr);
                ctx->PSSetShader(ps, nullptr, 0);
            }

            static void RestoreCB(const ImDrawList*, const ImDrawCmd*)
            {
                auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
                auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
                if (ctx) ctx->PSSetShader(prevPs, nullptr, 0);
                if (prevPs) { prevPs->Release(); prevPs = nullptr; }
            }
        };

        // ---- the overlay menu --------------------------------------------------
        class WheelerMenu : public RE::IMenu
        {
        public:
            static constexpr std::string_view MENU_NAME = "GridWheelerMenu";

            static void RegisterMenu()
            {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->Register(MENU_NAME, Creator);
                    SKSE::log::info("[WHEEL] {} registered", MENU_NAME);
                }
            }

            void PostDisplay() override
            {
                if (!UIRoot::TryInitD3D()) return;
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                // ★★BETWEEN THE TWO NewFrames, and it has to be exactly here:
                // the Win32 backend has just written the WINDOW's size, and
                // ImGui::NewFrame is about to lay the frame out against it. A
                // window is not the picture -- borderless upscale renders
                // 1920x1080 into a 3840x2160 window -- and this wheel takes its
                // whole scale from DisplaySize.y, so the window's answer drew
                // it at double size, off the bottom right. Reported.
                UIRoot::SyncDisplaySize();
                ImGui::NewFrame();
                // ★★This menu draws its OWN ImGui frame, so the mip sampler the
                // inventory binds at the head of its background list never
                // reached the wheel: every sprite here was sampled at mip 0.
                // A 256px sigil shown at ~48px is a 5x reduction, and the fine
                // curves of the power mark came out as a staircase. The mips
                // were already built (IconCache) -- nothing was using them.
                UIRoot::UseMipSampler(ImGui::GetBackgroundDrawList());
                Draw();
                ImGui::Render();
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }

            void AdvanceMovie(float, std::uint32_t) override {}

        private:
            static RE::IMenu* Creator()
            {
                using F = RE::UI_MENU_FLAGS;
                auto* m = new WheelerMenu();
                m->menuFlags.set(F::kCustomRendering);
                // ★kAllowSaving or the engine blocks EVERY save while this is up.
                m->menuFlags.set(F::kAllowSaving);
                // ★Deliberately NOT kPausesGame -- this runs in slow motion, and
                // a paused game would stop the animation with it.
                m->depthPriority = 12;
                return m;
            }

            // ★VR's RE::IMenu is 0x40, ours compiles to 0x30 — the engine
            // writes unk30/unk34/menuName into the sixteen bytes past our
            // allocation. Same tail, same reason, as GridMenu.h; the full
            // account of the crash that found it lives there.
            REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> _vrUnk30{ RE::UI_MENU_Unk09::kNone };
            std::byte                                     _vrUnk34{ std::byte{ 1 } };
            RE::BSFixedString                             _vrMenuName{};
        };

        static_assert(sizeof(WheelerMenu) >= 0x40,
            "VR's IMenu is 0x40 -- the engine writes past a 0x30 allocation");

        void ShowMenu(bool a_show)
        {
            auto* mq = RE::UIMessageQueue::GetSingleton();
            if (!mq) return;
            mq->AddMessage(WheelerMenu::MENU_NAME,
                a_show ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }

        void Apply()
        {
            if (g_arranged) return;   // see g_arranged: tidying is the whole act
            if (g_sel < 0 || !Eligible(g_group, g_sel)) return;
            G(g_group).apply(g_sel);
        }

        // ★★★ONE WAY OUT. Three things closed the wheel -- the hotkey release,
        // the settings switch, and a game load -- and each did its own subset
        // of the work. The switch's version even carried a comment claiming it
        // went "through the same path a key release does", which it did not.
        // Anything a visit leaves behind has to be undone here, so that adding
        // a fourth exit cannot forget a step that the other three remember.
        // ★The close ANIMATION is not part of this: g_dir = -1 starts it and
        // Tick finishes it, including taking the overlay down. This is about
        // state, not about pixels.
        void CloseWheel()
        {
            if (!g_open) return;
            g_open = false;
            g_dir = -1;
            // ★★AND IT DOES NOT TOUCH THE PICK. The release path latches
            // g_pick one line before calling this, precisely so the close
            // knows which slot to hold on to -- and this used to wipe it on
            // the very next statement. `shown` then read -1 for the whole
            // close, no slot ever matched, and every one of them took the
            // leave-early ramp: "keep the pick" was dead code that looked
            // present in both the constant and the comment.
            // ★Clearing belongs where the close ENDS (Tick) and where state is
            // deliberately forgotten (ResetOnLoad) -- not in the middle of a
            // sequence that just finished writing it.
            ResetVisitState();
            LeaveSlowMotion();
        }

        // Put a starred item to use. ★Both paths go through Equip's QUEUE, which
        // performs on the game thread outside the draw -- clicking here happens
        // in the render pass, and equipping from there is how a menu crashes.
        // ★One body for both lists: a starred thing is used the same way
        // wherever it was starred from. The caller passes the list because the
        // groups own separate ones, not because they behave differently.
        // ★★"Does this live in the VOICE slot rather than a hand?" -- and the
        // answer is not the form type. A shout does, obviously; so does a
        // POWER, and a power is a FormType::Spell, so the wheel filed modded
        // shouts under kSpell and then looked for them in selectedSpells[],
        // where they never are. They equipped and unequipped correctly and
        // simply never showed the tick that says "this one is on".
        // Greater, lesser and voice powers all land in selectedPower.
        [[nodiscard]] bool UsesVoiceSlot(RE::TESForm* a_form)
        {
            if (!a_form) return false;
            if (a_form->Is(RE::FormType::Shout)) return true;
            if (auto* sp = a_form->As<RE::SpellItem>()) {
                using T = RE::MagicSystem::SpellType;
                const auto t = sp->GetSpellType();
                return t == T::kPower || t == T::kLesserPower || t == T::kVoicePower;
            }
            return false;
        }

        void UseFav(const FavItem* a_list, int a_n, int a_slot, bool a_leftHand)
        {
            // ★A place, not an index into a packed list -- and an empty place
            // does nothing rather than acting on whatever used to be there.
            (void)a_n;
            if (a_slot < 0 || a_slot >= kSlots || !a_list[a_slot].form) return;
            // ★The wheel logged nothing at all about what a click DID, so a
            // report of "it does not work" had no way to say where it stopped.
            SKSE::log::info("[WHEEL] use slot={} '{}' kind={} uid {:04X} sig {:04X} "
                            "worn={} hand={}",
                a_slot, a_list[a_slot].form->GetName(),
                static_cast<int>(a_list[a_slot].kind), a_list[a_slot].uid,
                a_list[a_slot].sig, a_list[a_slot].worn, a_leftHand ? "L" : "R");
            g_itemActed = true;   // the release must not fire on top of this
            // ★The gear tick comes off a 330ms snapshot, because the LIST comes
            // off the inventory and that is what costs (see Items::current) --
            // so an equip made from the wheel could sit a third of a second
            // before it showed. A click is the one moment worth re-reading for,
            // and this asks Tick to do it next frame instead of at leisure.
            g_favMs = kFavRefreshMs;
            // ★★Magic goes through a queue of our own. Equip's queue speaks in
            // TESBoundObject and a spell is not one; EquipSpell/EquipShout are
            // engine calls that belong on the game thread, and this runs in the
            // render pass. One pointer and a flag, performed in Tick.
            if (a_list[a_slot].kind != FavKind::kItem) {
                g_castForm = a_list[a_slot].form;
                g_castLeft = a_leftHand;
                // ★★Same rule as the gear below: clicking what is ALREADY OUT
                // puts it away. Magic had no way back at all -- a spell taken
                // in hand could only be dropped by opening the magic menu, and
                // the wheel exists so that menu does not have to be opened.
                //
                // ★★★PER HAND, not "is it out anywhere". `worn` is true when the
                // spell is in EITHER hand, which is right for the tick that
                // marks it as active and wrong for this: with Flames in the
                // right hand, right-clicking to put it in the LEFT hand read as
                // "already out" and took it off instead. One spell in both
                // hands -- what dual casting is -- became unreachable.
                // Read at the click rather than off the snapshot: the snapshot
                // refreshes on a timer, and a second click can easily land
                // inside that window.
                g_castOff = false;
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    auto& rt = pc->GetActorRuntimeData();
                    if (UsesVoiceSlot(a_list[a_slot].form)) {
                        g_castOff = (rt.selectedPower == a_list[a_slot].form);
                    } else {
                        const int hand = a_leftHand ? RE::Actor::SlotTypes::kLeftHand
                                                    : RE::Actor::SlotTypes::kRightHand;
                        g_castOff = (rt.selectedSpells[hand] == a_list[a_slot].form);
                    }
                }
                return;
            }
            auto* obj = a_list[a_slot].obj;
            if (!obj) return;
            // ★★★CLICKING WHAT YOU ARE WEARING TAKES IT OFF. The wheel had no
            // way back: every click was an equip, so a shield put on by mistake
            // could only be removed by opening the inventory -- which is the
            // one thing the wheel exists to avoid. The doll has had this on
            // right-click all along; it was simply never reachable from here.
            // ★Reads the snapshot, so it lags a rebuild at most -- a third of a
            // second, and the alternative is walking the inventory per click.
            if (a_list[a_slot].worn) {
                // ★The second ring is not ENGINE-worn -- a carrier stands in
                // for it -- so the engine unequip below is a no-op for it.
                // The carrier's own gate does the whole job (sound, redraw).
                if (auto* armo = obj->As<RE::TESObjectARMO>();
                    armo && DualRing::Second() == armo) {
                    DualRing::RequestTakeOff();
                    return;
                }
                Equip::UnequipItem(obj, a_list[a_slot].uid, a_list[a_slot].sig,
                                   a_leftHand ? 2 : 0, a_list[a_slot].count);
                return;
            }
            // ★★A potion has no hand. Anything that is not held in one goes to
            // UseItem, which hands the object to the engine and lets IT decide
            // what a click means -- the same rule the inventory follows, and the
            // reason a scripted mod item still works here.
            const bool twoHanded = obj->Is(RE::FormType::Weapon) || obj->Is(RE::FormType::Light);
            if (!twoHanded && !obj->Is(RE::FormType::Armor)) {
                Equip::UseItem(obj, a_list[a_slot].uid, -1, a_list[a_slot].sig, {},
                               a_list[a_slot].count);
                return;
            }
            // ★Armour ignores the hand: there is one place a cuirass goes. The
            // slot id decides the hand only for what can be held.
            const char* slot = obj->Is(RE::FormType::Armor)
                                   ? ""                        // let Equip resolve it
                                   : (a_leftHand ? "shieldL" : "weapon");
            Equip::EquipItem(obj, slot, a_list[a_slot].uid, -1, a_list[a_slot].sig, {},
                             a_list[a_slot].count);
        }

        // ★★ONE ROTATION FOR EVERY WHEEL. What travels differs -- the item
        // wheels carry a FavItem, the set wheels carry a tab index -- but how a
        // ring rearranges does not, so it is written once and the element type
        // is the only thing the caller supplies. Two copies of this walk is two
        // chances for the wrap arithmetic to disagree with what the eye sees.
        //
        // ★A rotation, not a swap: dragging the third thing onto the first
        // should push the other two along, the way a hand moving a card in a
        // fan does -- swapping would fling whatever was there to where this
        // came from, which is a second change nobody asked for.
        //
        // ★kSlots, not a count of entries: dropping something into an empty
        // place is the whole point of being able to arrange them.
        template <typename T>
        bool RotateSlots(T* a_list, int a_from, int a_to)
        {
            if (a_from < 0 || a_to < 0 || a_from >= kSlots || a_to >= kSlots) return false;
            // ★true, not false: same-place is the "may this be lifted?" question
            // (see GroupDesc::reorder), and every place on every wheel may.
            if (a_from == a_to) return true;
            // ★★★THE RING WRAPS. Slot 0 and slot 9 are neighbours on screen but
            // opposite ends of the array, so carrying a piece anticlockwise
            // past twelve o'clock used to rotate all ten -- one step by hand,
            // eight steps in the list. Walking round in single swaps makes the
            // array agree with what the eye sees: crossing the top exchanges
            // two slots, nothing more.
            // ★The direction is the SHORTER way round, which is also the way
            // the hand actually travelled.
            int steps = ((a_to - a_from) % kSlots + kSlots) % kSlots;
            const int dir = (steps > kSlots / 2) ? -1 : 1;
            if (dir < 0) steps = kSlots - steps;
            int cur = a_from;
            for (int n = 0; n < steps; ++n) {
                const int nxt = ((cur + dir) % kSlots + kSlots) % kSlots;
                std::swap(a_list[cur], a_list[nxt]);
                SwapAngles(cur, nxt);
                cur = nxt;
            }
            return true;
        }

        // The item wheels: rotate, then write the arrangement down as FormIDs.
        bool MoveSlot(FavItem* a_list, int a_n, int a_which, int a_from, int a_to)
        {
            (void)a_n;
            if (!RotateSlots(a_list, a_from, a_to)) return false;
            RememberOrder(a_list, kSlots, a_which);
            return true;
        }

        // The set wheels: the arrangement IS the array, so there is nothing to
        // write down afterwards -- it is saved whole (see SaveGame).
        bool MoveSet(int a_which, int a_from, int a_to)
        {
            if (!g_setInit) ResetSetOrder();
            return RotateSlots(g_setOrder[a_which], a_from, a_to);
        }

        bool Preset::reorder(int from, int to) { return MoveSet(kPreset, from, to); }
        bool Costume_::reorder(int from, int to) { return MoveSet(kCostume, from, to); }

        // ---- the two magic-and-gear groups, wired to that one body ----------
        void Items::click(int slot, bool leftHand) { UseFav(g_fav, g_favN, slot, leftHand); }
        void Magic::click(int slot, bool leftHand) { UseFav(g_mag, g_magN, slot, leftHand); }
        void Items::apply(int slot)
        {
            if (g_itemActed) return;   // a click in this visit already did it
            UseFav(g_fav, g_favN, slot, /*leftHand*/ false);
        }
        void Magic::apply(int slot)
        {
            if (g_itemActed) return;
            UseFav(g_mag, g_magN, slot, /*leftHand*/ false);
        }
        // ★★Asked LIVE, unlike the gear beside it. Items::current reads the
        // snapshot because "am I wearing this" costs a walk of the whole pack,
        // and ten slots at sixty frames is six hundred walks a second -- so it
        // is refreshed on a 330ms timer and the tick can lag a third of a
        // second behind. Magic has no such cost: what is in a hand or the voice
        // is three pointers, and comparing them every frame is free. Paying the
        // snapshot's latency for it bought nothing and showed as a visible
        // delay before the tick appeared on a spell that was already out.
        bool Magic::current(int s)
        {
            if (!filled(s)) return false;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc || !pc->Is3DLoaded()) return g_mag[s].worn;
            auto* form = g_mag[s].form;
            auto& rt = pc->GetActorRuntimeData();
            if (UsesVoiceSlot(form)) return rt.selectedPower == form;
            for (auto* sel : rt.selectedSpells) {
                if (sel == form) return true;
            }
            return false;
        }
        bool Items::reorder(int from, int to) { return MoveSlot(g_fav, g_favN, 0, from, to); }
        bool Magic::reorder(int from, int to) { return MoveSlot(g_mag, g_magN, 1, from, to); }

        // ★★EVERY WHEEL OPENS AT CENTRE, WITH NOTHING CHOSEN. Opening on what
        // is already active reads as helpful and is not: it makes the wheel
        // answer a question the player has not asked yet, and it makes letting
        // go without aiming DO something. "None of these" has to be reachable,
        // and the cheapest way to reach it is to already be there.
        //
        // ★What is currently on is still visible -- the tick draws it. Showing
        // the state and preselecting it are different things, and only the
        // first was ever wanted.
        //
        // This used to be per-group, seeded from Loadout::Active / Costume::Tab
        // for the two set-pickers. The gear wheel opened at centre from the
        // start and it is the one that feels right, so the rule is now the
        // wheel's, not the group's.
        void SeedCursor()
        {
            g_sel = -1;
            g_mx = g_my = 0.0f;
            g_arcT = 0.0f;
        }

        void StepSlot(int a_delta)
        {
            int s;
            if (g_sel < 0) {
                // ★★FROM CENTRE THE FIRST NOTCH ENTERS AT TWELVE O'CLOCK, which
                // way it turns. Stepping from "nothing" the way we step from a
                // slot lands on the SECOND one -- the arithmetic starts at 0
                // and then moves -- so one notch skipped the first preset
                // entirely. Centre is not a place on the ring, so entering it
                // is not a move; the ring has a first slot and that is where
                // you arrive. Turning again moves the ordinary way from there.
                s = 0;
                for (int guard = 0; guard < kSlots && !Eligible(g_group, s); ++guard) {
                    s = (s + a_delta + kSlots) % kSlots;
                }
            } else {
                s = g_sel;
                for (int guard = 0; guard < kSlots; ++guard) {
                    s = (s + a_delta + kSlots) % kSlots;
                    if (Eligible(g_group, s)) break;
                }
            }
            // ★Lay the stroke down again when the CHOICE changes. This is not
            // the reset that was taken out of the aim path: there a sweep of
            // the hand re-triggered it every frame, so it never got past its
            // first. The wheel steps one slot per notch, discretely, and each
            // notch is a new mark -- which is what the highlight is.
            if (s != g_sel) { g_arcT = 0.0f; StepBlip(); }
            g_sel = s;
            // keep the pointer in step so a later mouse move does not jump
            const float a = (-90.0f + s * kStep) * 0.01745329f;
            g_mx = std::cos(a) * 140.0f;
            g_my = std::sin(a) * 140.0f;
        }

        // ---- drawing helpers ----------------------------------------------------
        // A rotated copy of one wheel texture. ★AddImageQuad, not AddImage: the
        // slot art is baked once at twelve o'clock and every other slot is the
        // same picture turned a multiple of 36 degrees.
        void DrawWheelTex(ImDrawList* dl, const IconCache::Icon& t, ImVec2 c,
                          float side, float deg, ImU32 col)
        {
            if (!t.srv) return;
            const float r = deg * 0.01745329f;
            const float s = std::sin(r), co = std::cos(r);
            const float h = side * 0.5f;
            const ImVec2 q[4] = { { -h, -h }, { h, -h }, { h, h }, { -h, h } };
            ImVec2 p[4];
            for (int i = 0; i < 4; ++i) {
                p[i] = ImVec2(c.x + q[i].x * co - q[i].y * s,
                              c.y + q[i].x * s + q[i].y * co);
            }
            dl->AddImageQuad(reinterpret_cast<ImTextureID>(t.srv),
                p[0], p[1], p[2], p[3],
                ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), col);
        }

        // The same texture, but REVEALED clockwise from twelve -- a wedge at a
        // time, the way the slots arrive.
        //
        // ★★The ring used to simply fade in, over e 0.35..0.70. That window
        // sounds generous and is not: `e` is already eased, so it lands at
        // roughly 40..99ms of a 300ms open -- a 60ms blip beside ten slots
        // taking the full 300 to come round. It read as the ring being there
        // from the start, because to the eye it was.
        //
        // ★A WEDGE FAN, not a clip. Clipping is rectangles only in ImGui, and
        // even given an arbitrary one it would leave a guillotine edge across
        // the bristles -- the same reason DrawArc lays its stroke down in
        // columns instead. So the leading edge is a RAMP over kRingFeather
        // degrees, and the ring arrives drawn rather than switched on.
        //
        // ★Each wedge reaches the texture's own SQUARE boundary, not a circle,
        // so the uv mapping is exact and needs no correction. The corners cost
        // nothing to carry: measured, the ring's ink stops at r=117 of a 512
        // square, so nothing lives out there to be clipped by the chords.
        // ★★...and the leading edge TAPERS, because a wedge that only fades is
        // still a straight cut across the bristles -- the exact thing this
        // function exists to avoid, moved from the alpha channel into the
        // shape. Fading alone gave a pale but perfectly square end, and square
        // is what the eye reads first.
        // The fix is to fade the ring's OUTER and INNER edges sooner than its
        // middle, so the stroke narrows to its centre line before it stops.
        // That needs the wedge split ACROSS the ring as well as along it --
        // hence a band loop inside the angle loop, `edgeness` being 0 on the
        // centre line and 1 at either rim.
        void DrawRingSweep(ImDrawList* dl, ImVec2 c, float side, float p, int alpha)
        {
            if (!g_ring.srv || alpha <= 2 || p <= 0.0f) return;
            constexpr float kRingStepDeg = 2.0f;
            // ★16 degrees was 4% of the circumference — a brush leaves a tail
            // an order of magnitude longer than that, and at 16 the taper had
            // no room to be seen as one.
            constexpr float kRingFeather = 42.0f;   // the centre line's tail
            constexpr float kRimLead     = 2.4f;    // the rims' tail, x the above
            // ★12, not 5. Each band is ONE flat alpha across its whole width
            // (AddImageQuad carries a single colour), so the band count is the
            // number of steps the taper is drawn in -- at 5 the rendered tail
            // came out in visible vertical stripes. 12 puts the step below what
            // the eye picks out at this size. The cost is quads, not draw
            // calls: consecutive ones share the texture and ImGui merges them.
            constexpr int   kBands       = 12;      // across the ring
            // ★★MEASURED off ring.png: its ink lives between r=79 and r=117 of
            // a 256 half-square, i.e. RADIUS 0.31..0.46 of h (see `at`, which
            // takes a radius — these are not fractions of the way to a corner).
            // The bands have to span THAT, not the whole square -- spread from
            // the centre to the corner instead, four of the five would sit on
            // empty texture and the ring's real centre line would land on a
            // band BOUNDARY, which is the one place the taper cannot act.
            // Padded a little either way so a redrawn ring with a wider stroke
            // or a few flecks does not get its edges clipped off.
            constexpr float kInkIn  = 0.26f;
            constexpr float kInkOut = 0.52f;
            // ★★THE STROKE RUNS PAST ITS OWN START. Drawing exactly 360 degrees
            // means the two tails meet nose to nose at twelve o'clock, and two
            // tapers meeting is a notch -- the ring reads as cut there however
            // softly each end is drawn. Which is why the first fix (fading the
            // start tail back in as the circle closed) never looked right: it
            // was hiding a seam instead of not making one.
            // A brush closing a circle does not stop where it started; it
            // carries on over the opening stroke and lifts. So the sweep is
            // longer than the circle, and the last stretch lies on top of the
            // first -- which is also why the overlap has to be longer than the
            // start's tail, or the tail would still be showing underneath.
            // ★The length is not a taste: at angle x inside the overlap the
            // second pass is `kOverlap - x` behind its own front, so it is
            // solid only while that exceeds the front's tail. To bury a start
            // tail of 30 degrees under a front tail of 42, the overlap has to
            // clear 72 -- measured at 52 the seam still sat at 0.70 of a solid
            // ring, which is a visible pale band exactly where the seam is.
            constexpr float kOverlap = 80.0f;
            const float h = side * 0.5f;
            // ★p carries the overlap as its second unit: 1 = the circle closed,
            // 2 = the stroke lifted. Keeping 1.0 meaning "360 degrees" is what
            // holds the ring's front on the slot-start front (see the caller);
            // the overlap then runs in the stagger's own tail, which is time
            // the ring already had spare.
            const float sweep = (std::min)(p, 1.0f) * 360.0f
                              + std::clamp(p - 1.0f, 0.0f, 1.0f) * kOverlap;
            const ImVec2 uvc(0.5f, 0.5f);
            // ★`stretch` is how much further the SQUARE's edge is than the
            // circle's along this ray: 1 at the axes, 1.414 into a corner.
            struct Edge { ImVec2 pos, uv; float stretch; };
            const auto edge = [&](float deg) {
                const float r = deg * 0.01745329f;
                float dx = std::cos(r), dy = std::sin(r);
                // push the point out to the square's edge along its own ray
                const float s = 1.0f / (std::max)(std::fabs(dx), std::fabs(dy));
                dx *= s;
                dy *= s;
                return Edge{ ImVec2(c.x + dx * h, c.y + dy * h),
                             ImVec2(0.5f + dx * 0.5f, 0.5f + dy * 0.5f), s };
            };
            // ★★A point at RADIUS t (of h), not at fraction t of the way to the
            // square's edge. The distinction is the whole bug: interpolating
            // straight to that edge makes a band boundary trace a SQUARE, so
            // the inner boundary bulged out into the corners -- at 45 degrees a
            // t of 0.26 lands at 0.368h against ink that starts at 0.309h, and
            // the band cut the ring open along a diagonal. On screen it read as
            // a transparent rectangle sitting inside the circle, with its
            // corners slicing the stroke.
            // Dividing by `stretch` cancels the square: radius is then t*h in
            // every direction, and the uv follows the same point, so the
            // texture still lines up exactly.
            const auto at = [&](const Edge& e, float tRadius) {
                const float t = tRadius / e.stretch;
                return Edge{ ImVec2(c.x + (e.pos.x - c.x) * t, c.y + (e.pos.y - c.y) * t),
                             ImVec2(uvc.x + (e.uv.x - uvc.x) * t,
                                    uvc.y + (e.uv.y - uvc.y) * t), e.stretch };
            };
            const int n = (std::max)(1,
                static_cast<int>(std::ceil(sweep / kRingStepDeg)));
            // ★★BOTH ENDS taper. The front is the one that moves, but the START
            // sits still at twelve o'clock for the whole sweep -- a square cut
            // there is on screen far longer than one on the front ever is, and
            // it is the end the eye has time to find. A brush entering the
            // paper leaves a tail exactly as it leaves one on the way out.
            // ★It is capped against the sweep. A fixed 30 degrees is a third of
            // the stroke while the stroke is still 90 long, so the opening
            // frames were more tail than mark; a brush's entry tail scales with
            // the stroke it starts.
            constexpr float kStartFeather = 30.0f;
            const float startFeather = (std::min)(kStartFeather, sweep * 0.35f);
            for (int i = 0; i < n; ++i) {
                const float d0 = sweep * i / n;
                const float d1 = sweep * (i + 1) / n;
                const float behind = sweep - d1;   // how far behind the front
                const Edge e0 = edge(-90.0f + d0);
                const Edge e1 = edge(-90.0f + d1);
                for (int b = 0; b < kBands; ++b) {
                    const float f0 = static_cast<float>(b) / kBands;
                    const float f1 = static_cast<float>(b + 1) / kBands;
                    const float t0 = kInkIn + (kInkOut - kInkIn) * f0;
                    const float t1 = kInkIn + (kInkOut - kInkIn) * f1;
                    // 0 on the ring's centre line, 1 at either rim
                    const float edgeness = std::fabs((f0 + f1) - 1.0f);
                    const float feather = kRingFeather * (1.0f + kRimLead * edgeness);
                    float m = std::clamp(behind / feather, 0.0f, 1.0f);
                    const float sf = startFeather * (1.0f + kRimLead * edgeness);
                    m = (std::min)(m, std::clamp(d0 / sf, 0.0f, 1.0f));
                    // ★Squared, not linear: a brush tail thins fast at the tip
                    // and holds its weight at the root. Linear read as a long
                    // grey smear rather than a stroke running out of ink.
                    const int a = static_cast<int>(alpha * m * m);
                    if (a <= 2) continue;
                    const Edge a0 = at(e0, t0), a1 = at(e1, t0);
                    const Edge b0 = at(e0, t1), b1 = at(e1, t1);
                    dl->AddImageQuad(reinterpret_cast<ImTextureID>(g_ring.srv),
                        a0.pos, a1.pos, b1.pos, b0.pos,
                        a0.uv, a1.uv, b1.uv, b0.uv,
                        (kInk & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24));
                }
            }
        }

        // The hover stroke, laid down left to right.
        // ★★The leading edge is a RAMP across several quads, not a clip. A clip
        // gives the brush a guillotine edge -- a straight cut across the
        // bristles, the one thing a brush never leaves. And the lanes across its
        // width run slightly out of step, so the tip frays instead of arriving
        // as one clean front.
        void DrawArc(ImDrawList* dl, ImVec2 c, float side, float deg, float t, int alpha)
        {
            if (!g_arc.srv || alpha <= 2) return;
            constexpr int kCols = 22;
            constexpr int kLanes = 4;
            constexpr float kFeather = 0.22f;
            constexpr float kFray = 0.045f;

            const float rad = deg * 0.01745329f;
            const float sn = std::sin(rad), cs = std::cos(rad);
            const float k = side / (kTexHalf * 2.0f);      // texture px -> screen px
            const auto put = [&](float tx, float ty) {     // texture space -> screen
                const float x = (tx - kTexHalf) * k, y = (ty - kTexHalf) * k;
                return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
            };
            // the arc's own box in texture space, generous enough to cover it
            const float x0 = kTexHalf - 96.0f, x1 = kTexHalf + 96.0f;
            const float y0 = 8.0f, y1 = 70.0f;
            const float head = t * (1.0f + kFeather);

            for (int L = 0; L < kLanes; ++L) {
                const float v0 = y0 + (y1 - y0) * L / kLanes;
                const float v1 = y0 + (y1 - y0) * (L + 1) / kLanes;
                const float lag = (L % 2 ? -1.0f : 1.0f) * kFray * (0.5f + 0.5f * L);
                for (int i = 0; i < kCols; ++i) {
                    const float u0 = static_cast<float>(i) / kCols;
                    const float u1 = static_cast<float>(i + 1) / kCols;
                    // ★Laid down RIGHT TO LEFT, and the texture is mirrored to
                    // match. The wheel counts clockwise, so at a slot the hand
                    // is travelling that way -- a stroke running the other way
                    // fights the shape it is marking. Both had to flip together:
                    // reversing only the sweep would start the brush at its dry
                    // tail, and a brush stroke begins where the ink is.
                    const float um = 1.0f - ((u0 + u1) * 0.5f) + lag;
                    const float m = std::clamp((head - um) / kFeather, 0.0f, 1.0f);
                    if (m <= 0.004f) continue;
                    const int a = static_cast<int>(alpha * m);
                    if (a <= 2) continue;
                    const float tx0 = x0 + (x1 - x0) * u0, tx1 = x0 + (x1 - x0) * u1;
                    dl->AddImageQuad(reinterpret_cast<ImTextureID>(g_arc.srv),
                        put(tx0, v0), put(tx1, v0), put(tx1, v1), put(tx0, v1),
                        ImVec2(tx0 / (kTexHalf * 2), v0 / (kTexHalf * 2)),
                        ImVec2(tx1 / (kTexHalf * 2), v0 / (kTexHalf * 2)),
                        ImVec2(tx1 / (kTexHalf * 2), v1 / (kTexHalf * 2)),
                        ImVec2(tx0 / (kTexHalf * 2), v1 / (kTexHalf * 2)),
                        (kRed & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24));
                }
            }
        }

        // ★Text has no ground of its own, and the wheel floats on the world.
        // Eight dark copies give every letter its own edge -- the same answer the
        // parchment gives the medallions, applied to type.
        // ★The halo colour is a PARAMETER because the ground flipped. The world
        // is washed pale now, so the wheel's own lettering is dark ink with a
        // pale edge -- the opposite of what it needed when the backdrop was the
        // night sky. Anything sitting on the wheel's own black plate keeps the
        // old way round.
        void Halo(ImDrawList* dl, ImVec2 c, const char* s, float sz, ImU32 col,
                  int alpha, ImU32 a_shadow = IM_COL32(0, 0, 0, 255))
        {
            if (!s || !*s || alpha <= 2) return;
            auto* fnt = ImGui::GetFont();
            const ImVec2 ts = fnt->CalcTextSizeA(sz, FLT_MAX, 0.0f, s);
            const ImVec2 p(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f);
            const ImU32 sh = (a_shadow & 0x00FFFFFF) |
                             (static_cast<ImU32>(alpha * 0.70f) << 24);
            const float d = (std::max)(1.0f, sz * 0.085f);
            for (int i = 0; i < 8; ++i) {
                const float ax = (i % 3 == 0) ? -d : (i % 3 == 1 ? d : 0.0f);
                const float ay = (i / 3 == 0) ? -d : (i / 3 == 1 ? d : 0.0f);
                dl->AddText(fnt, sz, ImVec2(p.x + ax, p.y + ay), sh, s);
            }
            dl->AddText(fnt, sz, p,
                (col & 0x00FFFFFF) | (static_cast<ImU32>(alpha) << 24), s);
        }
    }

    // ---- hotkey plumbing -----------------------------------------------------
    namespace
    {
        // DirectInput scan codes. Sparse on purpose: anything not named here
        // prints as its number, which is still a thing the player can tell two
        // bindings apart by.
        struct NamedKey { std::uint32_t code; const char* name; };
        constexpr NamedKey kKbNames[] = {
            { 1, "Esc" }, { 14, "Backspace" }, { 15, "Tab" }, { 28, "Enter" },
            { 29, "LCtrl" }, { 42, "LShift" }, { 43, "\\" }, { 54, "RShift" },
            { 55, "Num*" }, { 56, "LAlt" }, { 57, "Space" }, { 58, "CapsLock" },
            { 12, "-" }, { 13, "=" }, { 26, "[" }, { 27, "]" }, { 39, ";" },
            { 40, "'" }, { 41, "`" }, { 51, "," }, { 52, "." }, { 53, "/" },
            { 69, "NumLock" }, { 70, "ScrollLock" }, { 74, "Num-" }, { 78, "Num+" },
            { 83, "Num." }, { 156, "NumEnter" }, { 157, "RCtrl" }, { 181, "Num/" },
            { 183, "PrtScr" }, { 184, "RAlt" }, { 197, "Pause" }, { 199, "Home" },
            { 200, "Up" }, { 201, "PgUp" }, { 203, "Left" }, { 205, "Right" },
            { 207, "End" }, { 208, "Down" }, { 209, "PgDn" }, { 210, "Insert" },
            { 211, "Delete" },
        };
        constexpr NamedKey kPadNames[] = {
            { 0x0001, "D-Up" }, { 0x0002, "D-Down" }, { 0x0004, "D-Left" },
            { 0x0008, "D-Right" }, { 0x0010, "Start" }, { 0x0020, "Back" },
            { 0x0040, "LStick" }, { 0x0080, "RStick" }, { 0x0100, "LB" },
            { 0x0200, "RB" }, { 0x1000, "A" }, { 0x2000, "B" }, { 0x4000, "X" },
            { 0x8000, "Y" }, { 0x0009, "LT" }, { 0x000A, "RT" },
        };

        void KeyName(bool a_pad, std::uint32_t a_code, char* a_out, std::size_t a_n)
        {
            if (a_pad) {
                for (const auto& k : kPadNames) {
                    if (k.code == a_code) { std::snprintf(a_out, a_n, "%s", k.name); return; }
                }
            } else {
                // the letter and digit blocks are contiguous, so they are rows
                // of the keyboard rather than rows of a table
                constexpr const char* kRow1 = "1234567890";
                constexpr const char* kRow2 = "QWERTYUIOP";
                constexpr const char* kRow3 = "ASDFGHJKL";
                constexpr const char* kRow4 = "ZXCVBNM";
                if (a_code >= 2 && a_code <= 11) {
                    std::snprintf(a_out, a_n, "%c", kRow1[a_code - 2]); return;
                }
                if (a_code >= 16 && a_code <= 25) {
                    std::snprintf(a_out, a_n, "%c", kRow2[a_code - 16]); return;
                }
                if (a_code >= 30 && a_code <= 38) {
                    std::snprintf(a_out, a_n, "%c", kRow3[a_code - 30]); return;
                }
                if (a_code >= 44 && a_code <= 50) {
                    std::snprintf(a_out, a_n, "%c", kRow4[a_code - 44]); return;
                }
                if (a_code >= 59 && a_code <= 68) {
                    std::snprintf(a_out, a_n, "F%u", a_code - 58); return;
                }
                if (a_code == 87 || a_code == 88) {
                    std::snprintf(a_out, a_n, "F%u", a_code - 76); return;
                }
                for (const auto& k : kKbNames) {
                    if (k.code == a_code) { std::snprintf(a_out, a_n, "%s", k.name); return; }
                }
            }
            std::snprintf(a_out, a_n, "#%u", a_code);
        }

        void ComboToText(bool a_pad, const std::uint32_t* a_codes, int a_n,
                         char* a_out, std::size_t a_cap)
        {
            a_out[0] = '\0';
            for (int i = 0; i < a_n; ++i) {
                char one[24];
                KeyName(a_pad, a_codes[i], one, sizeof(one));
                if (i) std::strncat(a_out, " + ", a_cap - std::strlen(a_out) - 1);
                std::strncat(a_out, one, a_cap - std::strlen(a_out) - 1);
            }
        }

        [[nodiscard]] bool PadIsDown(std::uint32_t a_code)
        {
            for (int i = 0; i < g_padDownN; ++i) {
                if (g_padDown[i] == a_code) return true;
            }
            return false;
        }

        void PadSetDown(std::uint32_t a_code, bool a_down)
        {
            for (int i = 0; i < g_padDownN; ++i) {
                if (g_padDown[i] != a_code) continue;
                if (!a_down) g_padDown[i] = g_padDown[--g_padDownN];
                return;
            }
            if (a_down && g_padDownN < static_cast<int>(std::size(g_padDown))) {
                g_padDown[g_padDownN++] = a_code;
            }
        }

        [[nodiscard]] bool IsDown(bool a_pad, std::uint32_t a_code)
        {
            return a_pad ? PadIsDown(a_code) : (a_code < 256 && g_kbDown[a_code]);
        }

        [[nodiscard]] bool InCombo(bool a_pad, std::uint32_t a_code)
        {
            const int d = a_pad ? 1 : 0;
            for (int i = 0; i < g_comboN[d]; ++i) {
                if (g_combo[d][i] == a_code) return true;
            }
            return false;
        }

        // Every member down, counting the one that just arrived.
        [[nodiscard]] bool ComboComplete(bool a_pad)
        {
            const int d = a_pad ? 1 : 0;
            if (g_comboN[d] <= 0) return false;
            for (int i = 0; i < g_comboN[d]; ++i) {
                if (!IsDown(a_pad, g_combo[d][i])) return false;
            }
            return true;
        }
    }

    int ComboSize(bool a_pad) { return g_comboN[a_pad ? 1 : 0]; }

    std::uint32_t ComboAt(bool a_pad, int a_i)
    {
        const int d = a_pad ? 1 : 0;
        return (a_i >= 0 && a_i < g_comboN[d]) ? g_combo[d][a_i] : 0;
    }

    void SetCombo(bool a_pad, const std::uint32_t* a_codes, int a_n)
    {
        const int d = a_pad ? 1 : 0;
        g_comboN[d] = std::clamp(a_n, 0, kMaxCombo);
        for (int i = 0; i < g_comboN[d]; ++i) g_combo[d][i] = a_codes[i];
    }

    const char* ComboText(bool a_pad)
    {
        static char s_kb[96], s_pad[96];
        char* out = a_pad ? s_pad : s_kb;
        ComboToText(a_pad, g_combo[a_pad ? 1 : 0], g_comboN[a_pad ? 1 : 0], out, 96);
        return out;
    }

    // ★NO CALLER as of the Favorites-key move: the settings row that armed this
    // is gone, and AdoptFavoritesKey overwrites whatever the ini or a capture
    // produced. Kept because the machinery is correct and the day someone wants
    // the wheel on its own key -- or on a combination, which the game's own
    // controls cannot express -- this is what that costs. Delete it if that day
    // does not come.
    void BeginCapture(bool a_pad)
    {
        g_capDev = a_pad ? 1 : 0;
        g_capN = 0;
        g_capHeld = 0;
    }

    void CancelCapture() { g_capDev = -1; }
    bool Capturing() { return g_capDev >= 0; }

    const char* CaptureText()
    {
        static char s_buf[96];
        ComboToText(g_capDev == 1, g_capCode, g_capN, s_buf, sizeof(s_buf));
        return s_buf;
    }

    // ---- lifetime ------------------------------------------------------------
    void RegisterMenu()
    {
        WheelerMenu::RegisterMenu();
        InputLock::Install();
        MenuLock::Install();
        LookLock::Install();
        ZoomLock::Install();
        AttackLock::Install();
        LoadTextures();
        LoadSettings();   // the hotkey has to be right before the first press
    }

    bool IsOpen() { return g_open; }

    // ★★★THE HOTKEY IS THE GAME'S OWN FAVOURITES KEY, read from ControlMap.
    //
    // The wheel now IS the favourites menu -- it is built from the same stars
    // and it does the same job better -- so it should answer to the same key,
    // and it should follow that key when the player rebinds it in the game's
    // own options. That is one less setting to own, and it comes with the pad
    // binding for free.
    // ★Re-read on every open rather than cached: the control map is the
    // player's to change at any moment, and a wheel bound to a key they have
    // since moved is indistinguishable from a broken wheel.
    void AdoptFavoritesKey()
    {
        auto* cm = RE::ControlMap::GetSingleton();
        auto* ue = RE::UserEvents::GetSingleton();
        if (!cm || !ue) return;
        const auto kb = cm->GetMappedKey(ue->favorites, RE::INPUT_DEVICE::kKeyboard);
        const auto pad = cm->GetMappedKey(ue->favorites, RE::INPUT_DEVICE::kGamepad);
        // 0xFF is the engine's "not bound". Leave the previous answer standing
        // rather than binding the wheel to nothing.
        if (kb != 0xFF) { const std::uint32_t c = kb; SetCombo(false, &c, 1); }
        if (pad != 0xFF) { const std::uint32_t c = pad; SetCombo(true, &c, 1); }
    }

    // ---- persistence ---------------------------------------------------------
    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        // ★★INITIALISE BEFORE WRITING. g_setOrder is a plain array: until
        // ResetSetOrder has run it is all ZEROS, and zero is not "empty" here,
        // it is TAB 0. Writing that state records ten copies of EQUIP into the
        // save, and every read of that save afterwards believes it.
        // ★Nothing can reach this today -- the revert callback resets before
        // any save can happen -- and it is written down anyway, because the
        // cost is one line and the failure it prevents is a save file that
        // stays wrong. Every other reader of this array asks the same question
        // first (TabOf, RebuildSetOrder, MoveSet); the writer was the one place
        // that did not.
        if (!g_setInit) ResetSetOrder();
        if (!a_intfc->OpenRecord(kRecordType, kVersion)) return;
        for (int w = 0; w < 2; ++w) {
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_order[w].size()));
            for (const auto id : g_order[w]) a_intfc->WriteRecordData(id);
        }
        // ★The set wheels' arrangement, as TAB INDICES rather than FormIDs. A
        // preset exists only inside this save, so its index means exactly one
        // thing here and needs no resolving -- the opposite of the item wheels
        // above, where the number is a form and the load order may have moved
        // it. Same shape, different kind of name.
        for (int w = 0; w < 2; ++w) {
            for (int i = 0; i < kSlots; ++i) {
                a_intfc->WriteRecordData(static_cast<std::int32_t>(g_setOrder[w][i]));
            }
        }
        // ★Which group was last open. Per SAVE rather than per install: two
        // characters reach for different things, and a spellsword's habit is
        // not a stealth archer's.
        a_intfc->WriteRecordData(static_cast<std::int32_t>(g_group));
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        // ★Older saves are read as far as they go. Version 1 has the item
        // wheels and no set arrangement; refusing the whole record would throw
        // away an order the player had already arranged, to avoid guessing at
        // one they never had. The rebuild on open supplies the missing half.
        if (a_version > kVersion) return;
        for (int w = 0; w < 2; ++w) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            // ★★A COUNT READ FROM A FILE IS NOT A COUNT YET. ReadRecordData
            // succeeds on any four bytes, and reserve() runs before a single
            // element has been validated -- so a corrupted field asks for that
            // many FormIDs of memory and throws out of a load callback with no
            // catch above it. The real value is always kSlots; anything else
            // is a damaged record and there is nothing to salvage.
            if (n > static_cast<std::uint32_t>(kSlots)) {
                SKSE::log::warn("[WHEEL] cosave order count {} is impossible -- record ignored", n);
                return;
            }
            g_order[w].clear();
            g_order[w].reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::FormID id = 0;
                if (!a_intfc->ReadRecordData(id)) return;
                // ★Resolve, do not trust. A save made with a different load
                // order stores the same item under a different number, and a
                // stale id would silently reorder the wheel around an item
                // that no longer exists.
                // ★★★A DROPPED ENTRY SHIFTS EVERY LATER ONE. This vector is
                // POSITIONAL -- ApplyOrder reads want[slot], and 0 means "this
                // place was left empty" -- so skipping an id that no longer
                // resolves does not remove one thing, it renumbers the rest.
                // Uninstall one plugin and the whole arrangement the hand had
                // learned rotates by a slot.
                RE::FormID resolved = 0;
                if (!a_intfc->ResolveFormID(id, resolved)) resolved = 0;
                g_order[w].push_back(resolved);
            }
        }
        if (a_version < 2) return;   // no set arrangement in that save
        ResetSetOrder();
        for (int w = 0; w < 2; ++w) {
            for (int i = 0; i < kSlots; ++i) {
                std::int32_t v = 0;
                if (!a_intfc->ReadRecordData(v)) return;
                g_setOrder[w][i] = static_cast<int>(v);
            }
        }
        if (a_version < 3) return;   // no remembered group in that save
        std::int32_t grp = 0;
        if (a_intfc->ReadRecordData(grp)) {
            g_group = std::clamp(static_cast<int>(grp), 0, kGroups - 1);
        }
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        g_order[0].clear();
        g_order[1].clear();
        // ★★Cleared, not left alone. Revert fires before every load, and a tab
        // index from the outgoing save names a different set in the incoming
        // one -- the arrangement is the only thing here that is written in a
        // language the next save does not speak.
        ResetSetOrder();
        g_group = kPreset;   // ...and the remembered group, for the same reason
        // ★★★A LOAD IS AN EXIT TOO. The wheel is held open by a key, and it
        // deliberately does not pause the game -- so a load that lands while it
        // is up leaves g_open true with nobody left to release the key. Every
        // hook then keeps blanking buttons (no movement, no attack, not even
        // Esc), the camera stays locked, and the 0.25x time multiplier is a
        // GLOBAL the engine does not reset on load.
        // ★It self-heals if the player presses the favourites key again, which
        // is exactly why it would never be reported clearly.
        CloseWheel();
        g_t = 0.0f;
        g_dir = 0;
        // ★A load forgets the pick outright. CloseWheel deliberately keeps it
        // (the close animation is still holding that slot), but there is no
        // close here -- g_t goes straight to 0 -- so nobody would ever come
        // back to clear it.
        g_pick = g_pickGroup = -1;
        // ★And the queued spell: it is a raw form pointer from the outgoing
        // save, and casting it after the load would use whatever now lives at
        // that address (§4-3 rule 2).
        g_castForm = nullptr;
        g_castOff = false;   // ...and the flag that rides with it
        // ★The starred lists are pointers into the OTHER character's inventory.
        // Clearing the counts left the entries themselves behind.
        for (auto& f : g_fav) f = {};
        for (auto& f : g_mag) f = {};
        g_favN = 0;
        g_magN = 0;
    }

    void OnTabRemoved(int a_index)
    {
        if (!g_setInit) return;   // nothing arranged yet: the rebuild will do
        for (auto& w : g_setOrder) {
            for (auto& s : w) {
                if (s < 0) continue;              // empty, or the clear row
                if (s == a_index) s = kEmptySlot; // ★the place stays, emptied
                else if (s > a_index) --s;        // the tabs above it slid down
            }
        }
        SKSE::log::info("[WHEEL] tab {} removed -- arrangement updated", a_index);
    }

    bool Enabled() { return g_enabled; }

    void SetEnabled(bool a_on)
    {
        if (g_enabled == a_on) return;
        g_enabled = a_on;
        g_saidPassing = false;   // a fresh stand-down says its line again
        // ★Shut it if it happens to be up, through the SAME close every other
        // exit uses. This used to be a partial copy of that path -- with a
        // comment saying it was not -- and the copy was missing the drag state.
        if (!a_on) CloseWheel();
        SKSE::log::info("[WHEEL] {}", a_on ? "enabled" : "disabled -- vanilla favourites restored");
    }

    void ForgetFavorite(RE::FormID a_form)
    {
        if (!a_form) return;
        // ★Both lists: an item and a spell can never share a FormID, so asking
        // each is cheaper than deciding which one to ask.
        for (auto& order : g_order) {
            for (auto& id : order) {
                if (id == a_form) id = 0;
            }
        }
    }

    void ReloadMedallions()
    {
        for (auto& [k, ic] : g_medallions) {
            if (ic.srv) ic.srv->Release();
            if (ic.tex) ic.tex->Release();
        }
        const size_t n = g_medallions.size();
        g_medallions.clear();
        SKSE::log::info("[WHEEL] medallion cache dropped ({} entries)", n);
    }

    void LoadSettings()
    {
        // ★★Read the on/off switch HERE, at kDataLoaded, rather than waiting
        // for the inventory's first draw to pull in the whole ui ini. The wheel
        // answers a key during ordinary play; "off" that only takes effect once
        // you open your bag is off in name only -- and until then the wheel
        // opens AND suppresses the vanilla menu it was meant to have restored.
        g_enabled = WinManager::ReadWheelEnabled(g_enabled);
        if (!g_enabled) {
            SKSE::log::info("[WHEEL] disabled by settings");
            return;
        }
        // ★★★ONE SOURCE. The ini used to carry the binding too, and that
        // second copy is what broke the wheel the moment the inventory was
        // opened: WinManager re-reads that file on every open and put the stale
        // key back. The game's own Favorites binding is the answer, and nothing
        // else is consulted.
        AdoptFavoritesKey();
        SKSE::log::info("[WHEEL] hotkey: {} / pad {}", ComboText(false), ComboText(true));
    }

    // ---- input ---------------------------------------------------------------
    // ★★THE WHEEL YIELDS. One question, asked of every press that would open
    // it: is this key even MINE right now?
    //
    // MENU. The favourites key belongs to whatever UI owns the keyboard. The
    // old gate asked GameIsPaused() and then named two menus -- and a list of
    // names is only as good as its last edit. Quest Journal Overhaul binds Q
    // inside the journal and the wheel was still listening (user report ⑯):
    // a custom menu need not pause the game, so nothing on the list matched.
    // kUsesMenuContext is the ENGINE'S OWN word for "this menu has taken the
    // input context", which is exactly the question -- and it covers the two
    // named menus (ours sets the same flag, see GridMenu) for free.
    // ★Name the BIT, never the accessor: CommonLibSSE-NG has IMenu::UsesCursor()
    // returning kUsesMenuContext and UsesMenuContext() returning
    // kUsesMovementToDirection -- the two are swapped in the header, so the
    // readable-looking call is the wrong one.
    //
    // BEAST FORM. A werewolf or vampire lord reverts through the VANILLA
    // favourites menu, where the revert power lives. Our wheel is built from
    // starred items, worn gear and loadout presets -- a beast has none of the
    // three -- so eating the key offered nothing and locked the player in the
    // form (user report ⑬).
    //
    // ★★★ASKED OF THE ENGINE, NOT OF THE RACE, and that is the whole fix.
    //
    // This used to be "the race is unplayable", on the reasoning that an
    // unplayable race is what a transformation IS. It is not. The vanilla
    // VAMPIRE races are unplayable too -- measured in Skyrim.esm, all twelve
    // of them, NordRaceVampire through BretonRaceChildVampire, Playable
    // clear. So the moment a player caught vampirism the wheel stood down for
    // good and the vanilla menu answered the key forever after. Reported as a
    // conflict with a vampire overhaul; it was nothing of the kind, and a
    // Live Another Life vampire start reproduced it with every vampire mod
    // disabled.
    //
    // MenuControls carries the engine's own beast-form flag, and it exists for
    // exactly this reason: to change what the menus do while transformed. It
    // is the question this function was always trying to ask.
    bool YieldingToVanilla()
    {
        if (auto* mc = RE::MenuControls::GetSingleton(); mc && mc->InBeastForm()) {
            return true;
        }
        // ★A modded form that swaps the race without going through the engine's
        // beast form still has to be caught, and the mark that separates one
        // from a vampire is a FACE. A vampire keeps yours -- FaceGenHead is set
        // on every vanilla vampire race -- while a beast wears something that
        // is not a face at all, and the flag is clear on WerewolfBeastRace.
        // Unplayable AND faceless is a transformation; unplayable with your own
        // face is just you, changed.
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* race = p ? p->GetRace() : nullptr;
        if (!race || race->GetPlayable()) return false;
        return !race->data.flags.any(RE::RACE_DATA::Flag::kFaceGenHead);
    }

    bool SomethingElseOwnsTheKey(std::uint32_t a_pressed)
    {
        if (AnotherMenuOwnsInput()) return true;
        if (YieldingToVanilla()) {
            // Rare and worth measuring: which races actually land here
            // (the plan asks whether modded transformations do).
            auto* race = RE::PlayerCharacter::GetSingleton()->GetRace();
            // ★And WHAT THE KEY IS at that moment. "Q does nothing in beast
            // form" may be nothing to do with us: if the engine no longer maps
            // the favourites event to this key while transformed, the press was
            // never going to raise that menu however politely we stand aside.
            // 0xFF is the engine's own "not bound".
            std::uint32_t fav = 0xFF;
            if (auto* cm = RE::ControlMap::GetSingleton()) {
                if (auto* ue = RE::UserEvents::GetSingleton()) {
                    fav = cm->GetMappedKey(ue->favorites, RE::INPUT_DEVICE::kKeyboard);
                }
            }
            // ★WHICH HALF fired, because the two mean different things: the
            // engine's own flag is the answer for a vanilla transformation,
            // and the faceless-race test is the guess made for a modded one.
            // A yield reported by the guess on a race nobody expects is the
            // line that will explain the next report of this.
            auto* mc = RE::MenuControls::GetSingleton();
            SKSE::log::info("[WHEEL] yielding the favourites key -- "
                            "transformed into '{}' (engine beastForm={}); "
                            "favourites is bound to {:#04x}, pressed {:#04x}",
                race->GetName() ? race->GetName() : "?",
                (mc && mc->InBeastForm()) ? 1 : 0, fav, a_pressed);
            return true;
        }
        return false;
    }

    bool OnButton(const RE::ButtonEvent* a_event)
    {
        if (!a_event) return false;
        // ★★Off means CLAIMING NOTHING -- not "open but hidden". Every route in
        // has to fail here, including the capture path used to rebind, or the
        // wheel would still be eating keys for a menu that cannot appear.
        // ★PROBE: "the wheel is off and the vanilla menu does not come back"
        // has two causes that look identical from outside -- the press never
        // reached the game, or it did and the game declined to raise a menu.
        // This line is the first half: it fires from OUR sink, before any
        // decision, so seeing it with no [FAV] line beside it says the key
        // arrived, we passed it on untouched, and the engine chose nothing.
        // ★ONCE per stand-down, not once per press: a player who turns the
        // wheel off stays off, and a line every time they reach for their
        // favourites is a log that buries the rest of itself.
        if (!g_enabled) {
            if (!g_saidPassing && a_event->IsDown() && InCombo(
                    a_event->GetDevice() == RE::INPUT_DEVICE::kGamepad,
                    a_event->GetIDCode())) {
                g_saidPassing = true;
                SKSE::log::info("[WHEEL] favourites key {:#04x} seen while DISABLED "
                                "-- passed to the engine untouched",
                    a_event->GetIDCode());
            }
            return false;
        }
        const auto dev = a_event->GetDevice();
        const auto id = a_event->GetIDCode();

        if (dev == RE::INPUT_DEVICE::kMouse) {
            if (!g_open) return false;
            if (a_event->IsDown()) {
                // ★The wheel is a BUTTON in this engine: 8 up, 9 down.
                if (id == 8) { g_padDriving = false; StepSlot(-1); return true; }
                if (id == 9) { g_padDriving = false; StepSlot(1); return true; }
                // ★★Left = right hand, right = left hand -- what every quick
                // menu in this game means by a click. This is why switching
                // groups had to leave the mouse buttons: a toggle there would
                // have cost the wheel its second hand.
                // ★The group answers whether a click means anything, so this
                // stays true when a fourth and fifth group arrive.
                // ★★★ASK Eligible, LIKE EVERY OTHER PATH INTO click().
                //
                // The pad path and the left-release path below both check it;
                // the two mouse paths did not. A slot can be drawn, filled and
                // aimable while its group refuses to act on it -- the costume
                // group does exactly that for the preset tab you are already
                // wearing, and StepSlot skips it, so only a player aiming with
                // the mouse could reach it. Costume::SetTab then folded the
                // ineligible index to -1, which does not mean "nothing" but
                // "no costume": the click took off what the player had on.
                if (id == 1 && G(g_group).click && g_sel >= 0 &&
                    Eligible(g_group, g_sel)) {
                    G(g_group).click(g_sel, /*leftHand*/ true);
                    return true;
                }
                // ★★LEFT BUTTON ACTS ON RELEASE, not on press -- that one move
                // buys dragging for free. Press marks where the hand started;
                // aim carries the selection to another slot as it always does;
                // release asks whether it moved. Nothing was taken away from
                // the controls to make room, because a click IS a drag of
                // length zero.
                // ★Ask before lifting. A slot that cannot travel should not
                // look lifted either -- it grows and lightens the moment the
                // button goes down, and then refusing to move reads as the
                // drag being broken rather than the slot being fixed.
                // ★Nothing refuses today (see GroupDesc::reorder); the question
                // is asked so that a wheel which does cannot slip past.
                if (id == 0 && G(g_group).reorder && g_sel >= 0 &&
                    G(g_group).reorder(g_sel, g_sel)) {
                    g_dragFrom = g_sel;
                    g_dragMoved = false;
                    return true;
                }
            } else if (a_event->IsUp() && id == 0) {
                const int from = g_dragFrom;
                const bool moved = g_dragMoved;
                g_dragFrom = -1;
                g_dragMoved = false;
                // ★A click is a drag that went nowhere. Having moved something
                // is an act in itself -- finishing it must not also put the
                // thing on.
                if (from >= 0 && !moved && G(g_group).click &&
                    g_sel >= 0 && Eligible(g_group, g_sel)) {   // ★see the right-click note
                    G(g_group).click(g_sel, /*leftHand*/ false);
                }
                if (moved) {
                    g_arcT = 0.0f;   // the mark redraws on its new home
                    // ★At the DROP, not on every neighbour passed. A drag
                    // rearranges as it travels, so the crossings already have
                    // their own sound (the step blip); this one says the
                    // arrangement is now the arrangement.
                    Sfx::Favorite();
                }
                return true;
            }
            return true;   // swallow the rest; swinging at the menu is never meant
        }

        const bool pad = dev == RE::INPUT_DEVICE::kGamepad;
        if (!pad && dev != RE::INPUT_DEVICE::kKeyboard) return false;

        // ★Record FIRST, for every key, bound or not. A combination has to ask
        // "is everything else already held?" and an InputEvent only ever
        // reports one key -- so the answer has to have been kept.
        if (a_event->IsDown()) {
            if (pad) PadSetDown(id, true);
            else if (id < 256) g_kbDown[id] = true;
        } else if (a_event->IsUp()) {
            if (pad) PadSetDown(id, false);
            else if (id < 256) g_kbDown[id] = false;
        }

        // ---- rebinding ----------------------------------------------------
        if (g_capDev >= 0) {
            if ((g_capDev == 1) != pad) return false;   // other device: not ours
            if (!pad && id == 1 && a_event->IsDown()) { // Esc cancels
                g_capDev = -1;
                return true;
            }
            if (a_event->IsDown()) {
                bool seen = false;
                for (int i = 0; i < g_capN; ++i) seen |= g_capCode[i] == id;
                if (!seen && g_capN < kMaxCombo) g_capCode[g_capN++] = id;
                if (!seen) ++g_capHeld;
            } else if (a_event->IsUp()) {
                // ★Commit when the LAST one comes up. Committing on the first
                // release would take "Shift" out of Shift+X the instant a hand
                // came off the modifier -- and a hand comes off the modifier
                // first about half the time.
                if (g_capHeld > 0 && --g_capHeld == 0 && g_capN > 0) {
                    SetCombo(pad, g_capCode, g_capN);
                    g_capDev = -1;
                    WinManager::GetSingleton()->Save();
                }
            }
            return true;   // nothing else in the game sees these
        }

        // ---- cycling the groups while the wheel is up ----------------------
        // ★★A/D -- the keys the hand is already on. They are the movement keys,
        // which is exactly why they were unusable before: the wheel used to let
        // the player keep walking. It does not any more (every unclaimed input
        // is muted while it is open), so the most reachable pair on the
        // keyboard is free, and "left/right" is what switching pages means.
        // ★It cycles both ways rather than toggling: a toggle works for two
        // groups and stops working at three.
        // ★The pad uses the D-pad's left/right. Not the shoulders: the left one
        // is the default hold key, and a group switch that needs the key you
        // are already holding is not a control.
        if (g_open && a_event->IsDown()) {
            constexpr std::uint32_t kA = 30, kD = 32;              // scan codes
            constexpr std::uint32_t kDLeft = 0x0004, kDRight = 0x0008;
            // ★(1.3.2) the pad hotkey resolves to a D-PAD direction in this
            // layout, so "hold it AND press D-pad left/right" was physically
            // impossible. The triggers are the pad's natural left/right pair
            // (LT back, RT forward), and Back cycles forward for pads where
            // the triggers are busy. D-pad left/right stays for layouts
            // whose hotkey leaves it free.
            constexpr std::uint32_t kBack = 0x0020, kLT = 0x0009, kRT = 0x000A;

            // ★★(1.3.2) THE PAD'S TWO HANDS. The mouse has had them since the
            // wheel shipped -- left button = right hand, right button = left
            // hand -- and the pad had neither: letting go of the hotkey was
            // the only way to act, so it could only ever equip to the right
            // hand and there was no way to take anything off.
            // ★A and X, NOT A and B. X is what "equip" already means on this
            // pad: our own inventory routes the item screens' Equip binding
            // (ue->equip / xButton) to the same click the mouse's right
            // button makes, so the button the player already presses to put
            // something on is the button that puts it in the other hand here.
            // B is the engine's Cancel and is left alone.
            // ★UseFav is a TOGGLE (worn -> unequip), so one button covers
            // equip and unequip for its hand, and g_itemActed keeps the
            // release from acting a second time on top of it.
            constexpr std::uint32_t kPadA = 0x1000, kPadX = 0x4000;
            if (pad && (id == kPadA || id == kPadX) && G(g_group).click) {
                if (g_sel >= 0 && Eligible(g_group, g_sel)) {
                    G(g_group).click(g_sel, /*leftHand*/ id == kPadX);
                    Sfx::Favorite();
                } else {
                    Sfx::SelectOff();   // nothing under the aim
                }
                return true;
            }

            int dir = 0;
            if (!pad && id == kA) dir = -1;
            else if (!pad && id == kD) dir = 1;
            else if (pad && id == kDLeft) dir = -1;
            else if (pad && id == kDRight) dir = 1;
            else if (pad && id == kLT) dir = -1;
            else if (pad && id == kRT) dir = 1;
            else if (pad && id == kBack) dir = 1;
            if (dir) {
                g_group = (g_group + dir + kGroups) % kGroups;
                g_groupT = 0.0f;   // re-ink the ring for the new list
                // ★★★A DRAG DOES NOT SURVIVE A GROUP CHANGE. Slot 2 of PRESET
                // and slot 2 of GEAR are unrelated, so carrying an index across
                // the boundary made the new list rearrange itself around a
                // number that meant something else -- and write the result to
                // the save. The group switch is a keyboard/pad path and never
                // saw the mouse button, so nothing else could have caught it.
                g_dragFrom = -1;
                g_dragMoved = false;
                // ★Back to centre, not "the same slot in the new list". Slot 3
                // of GEAR and slot 3 of MAGIC have nothing to do with each
                // other, and carrying an aim across that boundary would arm a
                // choice the player never made in the list they are now in.
                SeedCursor();
                g_padDriving = pad;
                Sfx::SelectOn();
                return true;
            }

        }

        if (!InCombo(pad, id)) return false;

        // ★★A member that is NOT the one that opened the wheel is passed
        // through, both ways. The game already saw it go down (we did not open
        // until the last key arrived), so eating its release would leave the
        // game holding a key forever -- a Shift stuck down is a player stuck
        // sprinting. Only the opening code is ours to take.
        if (g_open && id != g_openedBy && !a_event->IsUp()) return false;

        if (a_event->IsDown() && !g_open && ComboComplete(pad)) {
            if (SomethingElseOwnsTheKey(id)) return false;
            LoadTextures();
            // ★Ask for a re-read rather than trusting the event stream. This
            // costs one inventory scan per open and removes a whole class of
            // "the icon is one equip behind" -- gear can move without a
            // TESEquipEvent the sink sees, and the wheel is the surface where
            // that shows.
            Loadout::MarkActiveStale();
            CollectFavorites();   // the item group is whatever is starred right now
            RebuildSetOrder();    // ...and the set groups, whatever tabs exist
            // ★PROBE (1.4.4): a report of the preset wheel coming up FULL of
            // the EQUIP tab. Ten slots drawn means ten slots answered "filled",
            // and every one of them wore the current-tick -- which can only
            // mean every slot holds the same tab index. Nothing that writes
            // this array can put a value in it twice: the rebuild seats each
            // tab once, the drag swaps, and the remove-notice renumbers. So
            // the array is asked what it actually holds, rather than reasoned
            // about further.
            {
                std::string pre, cos;
                for (int i = 0; i < kSlots; ++i) {
                    const int p = g_setOrder[kPreset][i];
                    const int c = g_setOrder[kCostume][i];
                    pre += (p == kEmptySlot ? std::string("-") : std::to_string(p)) + " ";
                    cos += (c == kEmptySlot ? std::string("-") : std::to_string(c)) + " ";
                }
                std::string names;
                for (int t = 0; t < Loadout::Count(); ++t) {
                    names += "[" + std::to_string(t) + "]'" + Loadout::Name(t) + "' ";
                }
                SKSE::log::info("[WHEEL] open: tabs={} active={} {}", Loadout::Count(),
                                Loadout::Active(), names);
                SKSE::log::info("[WHEEL] order preset = {}", pre);
                SKSE::log::info("[WHEEL] order costume= {}", cos);
            }
            // ★Belt and braces: CloseWheel already cleared these on the way
            // out, but an open that assumes the last close ran is an open that
            // trusts every future exit path to have been written correctly.
            ResetVisitState();
            ResetSlotAngles();    // every open starts square
            ClearDrops();         // ...and with no trail from last time
            g_diagIcons = true;   // report where this open's pictures came from
            g_open = true;
            g_dir = 1;
            // ★★Opens on the group it CLOSED on. It used to reset to PRESET
            // every time, which charges the player two keystrokes for a habit:
            // someone who lives in GEAR paid them on every single open. The
            // group is a place in the menu, and a menu that forgets where you
            // were is a menu you re-navigate rather than use.
            // ★Unless nothing is there. A group can empty out between visits --
            // unstar every item and GEAR has ten blank slots -- and opening
            // onto that reads as the wheel being broken rather than as the list
            // being empty. Falling back to the sets is safe: they always hold
            // at least EQUIP.
            if (!AnyFilled(g_group)) g_group = kPreset;
            g_padDriving = (dev == RE::INPUT_DEVICE::kGamepad);
            g_pick = g_pickGroup = -1;
            g_openedBy = id;
            SeedCursor();
            EnterSlowMotion();
            ShowMenu(true);
            Sfx::BagOpen();
            return true;
        }
        if (a_event->IsUp() && g_open) {
            // ★Latch before clearing g_open -- the close animation has to know
            // which slot to hold on to, and the pointer stops existing here.
            g_pick = g_sel;
            g_pickGroup = g_group;
            // ★★ONE sound, not both. Closing is always a close, but a close
            // that ACTED and a close that came away empty are different events
            // to the player, and stacking a cancel blip on top of the shut
            // sound would report the second while the first is still playing.
            // The empty case is the one that needs saying out loud -- nothing
            // else on screen confirms "you let go and nothing happened".
            const bool acted = g_sel >= 0 && !g_arranged && Eligible(g_group, g_sel);
            Apply();
            if (acted) Sfx::BagClose();
            else       Sfx::SelectOff();
            // ★After Apply, which reads g_arranged. CloseWheel clears it.
            CloseWheel();
            // ...and only the opening key's release is eaten; a modifier's goes
            // back to the game that is still holding it down.
            return id == g_openedBy;
        }
        return g_open && id == g_openedBy;
    }

    void Mute(RE::InputEvent* a_event)
    {
        if (!a_event || !g_open) return;
        if (auto* b = a_event->AsButtonEvent()) {
            // See the header: zeroing the value is what lets the engine
            // release a key it was already holding instead of holding it
            // forever. heldDownSecs is deliberately left alone -- it is the
            // thing that tells those two cases apart.
            b->GetRuntimeData().value = 0.0f;
            return;
        }
        if (auto* m = a_event->AsMouseMoveEvent()) {
            m->mouseInputX = 0;
            m->mouseInputY = 0;
            return;
        }
        if (auto* t = a_event->AsThumbstickEvent()) {
            t->xValue = 0.0f;
            t->yValue = 0.0f;
        }
    }

    bool OnThumbstick(const RE::ThumbstickEvent* a_event)
    {
        // ★(1.3.2) the RIGHT stick aims the wheel now (user request); the
        // LEFT stick is muted with the rest of movement while the wheel is
        // up (InputLock -- the header contract: nothing else reaches the
        // game). The camera is not fighting for the right stick either:
        // LookLock refuses its motion while the wheel is open.
        if (!g_open || !a_event || !a_event->IsRight()) return false;
        g_padDriving = true;
        const float x = a_event->xValue, y = a_event->yValue;
        if (x * x + y * y > 0.36f) {
            g_mx = x * 160.0f;
            g_my = -y * 160.0f;
        }
        return true;
    }

    bool OnMouseMove(const RE::MouseMoveEvent* a_event)
    {
        if (!g_open || !a_event) return false;
        g_padDriving = false;
        // ★Accumulated DELTA, not the cursor. The game keeps the pointer hidden
        // and pinned, so its screen position never moves; and deltas make every
        // open start dead centre, which is the only way "flick a direction"
        // feels right on a wheel.
        g_mx += static_cast<float>(a_event->mouseInputX) * kMouseSens;
        g_my += static_cast<float>(a_event->mouseInputY) * kMouseSens;
        const float d = std::sqrt(g_mx * g_mx + g_my * g_my);
        if (d > 260.0f) { g_mx *= 260.0f / d; g_my *= 260.0f / d; }
        // ★Marked HERE, from the pointer's own movement, not from the frame
        // loop: the trail is a record of where the hand went, and only this
        // path knows that. Clamped first, so a mark never lands outside the
        // reach the pointer actually has.
        MaybeDropInk(g_mx, g_my);
        return true;
    }

    // ---- the engine's own unequip, for magic ---------------------------------
    // ★★★THE CALLS THE FAVOURITES MENU USES. CommonLibSSE-NG wraps EquipSpell
    // (37939) and EquipShout (37941) and stops there, so the two that take
    // magic back OFF are invisible from the headers -- which is why three
    // attempts at this went through the wrong door:
    //   · Actor::DeselectSpell cleared the selection and told the animation
    //     graph nothing, leaving the player in a casting idle holding nothing;
    //   · ActorEquipManager::UnequipObject is the WEAPON path, and given one
    //     hand it empties BOTH when the same spell is in each -- measured
    //     against vanilla, which empties exactly the hand you clicked.
    // The ids come from D7ry's Wheeler (github.com/D7ry/wheeler), whose
    // Utils.cpp declares both; its call sites also pin the hand argument --
    // it passes 0 before equipping the LEFT hand and 1 before the RIGHT, which
    // is RE::Actor::SlotTypes (kLeftHand = 0, kRightHand = 1).
    // ★No addresses, only ids: this stays version-independent like everything
    // else here (CLAUDE.md rule 4-1.2).
    void EngineUnequipSpell(RE::ActorEquipManager* a_em, RE::Actor* a_actor,
                            RE::SpellItem* a_spell, int a_hand)
    {
        if (!a_em || !a_actor || !a_spell) return;
        using func_t = void (RE::ActorEquipManager::*)(RE::Actor*, RE::SpellItem*, int);
        REL::Relocation<func_t> func{ RELOCATION_ID(37947, 38903) };
        func(a_em, a_actor, a_spell, a_hand);
    }
    void EngineUnequipShout(RE::ActorEquipManager* a_em, RE::Actor* a_actor,
                            RE::TESShout* a_shout)
    {
        if (!a_em || !a_actor || !a_shout) return;
        using func_t = void (RE::ActorEquipManager::*)(RE::Actor*, RE::TESShout*);
        REL::Relocation<func_t> func{ RELOCATION_ID(37948, 38904) };
        func(a_em, a_actor, a_shout);
    }

    // ---- frame ----------------------------------------------------------------
    // ★Perform a queued spell or shout. Game thread only -- see UseFav.
    void ProcessCast()
    {
        if (!g_castForm) return;
        auto* form = g_castForm;
        const bool off = g_castOff;
        g_castForm = nullptr;
        g_castOff = false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* em = RE::ActorEquipManager::GetSingleton();
        // ★Not merely non-null: a player without 3D is one mid-load or in the
        // main menu, and equipping onto that is asking the engine to dress a
        // body that is not there (CLAUDE.md rule 4).
        if (!player || !em || !player->Is3DLoaded()) return;
        // ★A POWER is a Spell that lives in the voice slot, so putting one away
        // is the shout's operation and not the hand's -- there is no hand to
        // take it out of. Handled before the SpellItem branch below, which
        // would otherwise go looking for it in selectedSpells[].
        if (off && UsesVoiceSlot(form) && !form->Is(RE::FormType::Shout)) {
            // ★★A POWER is not taken off by naming the voice slot. Passing
            // kPowerOrShout (3) did nothing at all -- modded shouts, which are
            // powers, equipped fine and would not come back off. Wheeler walks
            // 2, 1, 0 instead and that is what works: the engine files a power
            // under a hand slot even though it reads back from selectedPower,
            // and which one is not something the caller gets to know. Sweeping
            // all three is the same answer the favourites menu arrives at.
            auto* sp = form->As<RE::SpellItem>();
            constexpr int kSweep[3] = { RE::Actor::SlotTypes::kUnknown,
                                        RE::Actor::SlotTypes::kRightHand,
                                        RE::Actor::SlotTypes::kLeftHand };
            for (const int hand : kSweep) EngineUnequipSpell(em, player, sp, hand);
            return;
        }
        if (auto* shout = form->As<RE::TESShout>()) {
            // ★A shout has no hand. It goes to the voice, and asking for a
            // left-hand shout is asking a question the game has no answer to.
            // ★Taking one off has its own engine call, next door to EquipShout
            // -- see EngineUnequipShout. This used to blank selectedPower by
            // hand, which is the same mistake DeselectSpell made for spells:
            // the slot empties and nothing else is told.
            if (off) {
                EngineUnequipShout(em, player, shout);
                return;
            }
            em->EquipShout(player, shout);
            return;
        }
        if (auto* spell = form->As<RE::SpellItem>()) {
            // ★★ONE HAND -- the one that was clicked, and the engine's own
            // unequip is what makes that possible (EngineUnequipSpell). The
            // weapon path emptied both hands when the same spell was in each,
            // so a dual cast could not be undone by halves; this takes the hand
            // it is given, which is what the favourites menu does.
            if (off) {
                EngineUnequipSpell(em, player, spell,
                                   g_castLeft ? RE::Actor::SlotTypes::kLeftHand
                                              : RE::Actor::SlotTypes::kRightHand);
                return;
            }
            const RE::FormID slotId = g_castLeft ? 0x13F43 : 0x13F42;   // Left / Right hand
            auto* slot = RE::TESForm::LookupByID<RE::BGSEquipSlot>(slotId);
            em->EquipSpell(player, spell, slot);
        }
    }

    void Tick()
    {
        ProcessCast();
        // ★★★REAL time, not game time, and not a constant.
        //
        // It was a hard 16ms, which assumed 60fps: at 144Hz every animation ran
        // 2.4x fast (a 420ms close finished in 175ms) and the 60ms sound
        // throttle became 25ms, bringing back the rattle it was added to stop.
        //
        // ★★But NOT the frame delta the Update hook is handed, and NOT
        // BSTimer::realTimeDelta either -- both were tried and both are wrong
        // here. The wheel puts the game into slow motion while it is up
        // (EnterSlowMotion, 0.25x), and that multiplier reaches BOTH of the
        // engine's clocks: "realTimeDelta" survives a pause, not a time scale.
        // Measured in game: every wheel animation ran at a quarter speed.
        //
        // ★★★So the clock is the one the engine cannot reach. A menu animates
        // in the PLAYER'S seconds -- that is what makes it feel the same
        // whatever the world is doing -- and the only way to be sure of that is
        // to ask something outside the world. Tick runs once per frame, so the
        // gap between two calls is the frame.
        static auto s_last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        // ★Clamped. A hitch or a loading screen leaves a gap of whole seconds,
        // which would jump an animation to its end and let the periodic pollers
        // fire in a burst the moment play resumes.
        const float dt = std::clamp(
            std::chrono::duration<float, std::milli>(now - s_last).count(),
            1.0f, 100.0f);
        s_last = now;

        // ★★Follow the game's Favorites binding as it changes. Not on open --
        // by then the key press has already been matched against the old
        // answer, so a player who just rebound the key would find that neither
        // the old key nor the new one works. Once a second, while closed, is
        // early enough to be invisible and cheap enough to ignore.
        if (!g_open && (g_keyMs += dt) >= 1000.0f) {
            g_keyMs = 0.0f;
            AdoptFavoritesKey();
        }
        // ★★Disarm capture the moment the panel it belongs to is gone. While it
        // is armed EVERY key is swallowed, so a menu closed by Esc, by a load,
        // or by anything else that does not route through the settings row
        // would leave the game deaf to the keyboard with nothing on screen to
        // explain it. Checked here because Tick is the one thing that runs
        // whatever else happens.
        if (g_capDev >= 0) {
            // ★...and the settings row is equally gone when the menu is merely
            // suppressed, so the same disarm applies: an armed capture eats
            // every key, and it must never outlive the panel it belongs to.
            if (!UIRoot::IsBoardLive()) g_capDev = -1;
        }
        // ★★The brush advances BEFORE the "nothing is moving" early-out. It used
        // to sit after it, so the stroke could only ever be laid down while the
        // wheel was still opening -- once open, g_dir goes to 0 and Tick returns
        // at the first line. Switching lists resets the stroke to nothing, and
        // nothing is where it stayed for the rest of the session.
        // ★★The brush only advances while there is something to mark. Gear and
        // magic now open with NOTHING chosen, and this ran regardless -- so the
        // stroke finished drawing itself against an empty wheel, and by the
        // time the hand aimed at a slot there was nothing left to animate: the
        // mark simply appeared, complete. That is the "the highlight stopped
        // animating" report.
        if (g_open && g_sel >= 0 && g_arcT < 1.0f) {
            g_arcT = (std::min)(1.0f, g_arcT + dt / kArcMs);
        }
        // ★Advanced with the brush, ahead of the early-out below, and for the
        // same reason: once the wheel is open g_dir is 0 and Tick returns at
        // that line, so anything left after it can never run while the thing
        // it animates is on screen.
        if (g_open && g_groupT < 1.0f) {
            g_groupT = (std::min)(1.0f, g_groupT + dt / kGroupMs);
        }
        AdvanceSlotAngles(dt);
        if (g_blipCool > 0.0f) g_blipCool = (std::max)(0.0f, g_blipCool - dt);
        // ★Dries whether the wheel is open or not, and above the early-out for
        // the same reason the brush is: a trail left on screen at the moment
        // the wheel is dismissed has to finish fading, and by then g_dir has
        // already sent Tick home.
        AdvanceDrops(dt);
        // ★Re-read the starred items on a slow timer while the wheel is up, so
        // a click that equips something shows its tick. Three times a second is
        // faster than a hand can notice and a two-hundredth of what asking per
        // slot per frame would cost. Game thread, which is where Tick runs and
        // where reading the inventory is safe.
        if (g_open && (g_favMs += dt) >= kFavRefreshMs) {
            g_favMs = 0.0f;
            CollectFavorites();
        }
        // ★★The screen-sized capture is the single biggest thing this menu owns
        // -- one full backbuffer, 15 MB at 1440p and 33 MB at 4K -- and it used
        // to be held for the rest of the session after one open. Rebuilding it
        // costs one CreateTexture2D, which is the frame the wheel spends fading
        // in anyway.
        // ★NOT the moment the hotkey is released: the close animation still
        // draws over a grey world. Only once the menu is actually down.
        // ★And not on the very tick it goes down either. ImGui hands raw
        // pointers to the draw list, so the frame that just referenced this SRV
        // may still be in flight; a few ticks of daylight cost nothing and mean
        // the release can never land under a live draw.
        if (g_dir == 0 && !g_open) {
            if (Grey::srv && g_greyIdleMs < kGreyFreeMs) {
                if ((g_greyIdleMs += dt) >= kGreyFreeMs) Grey::Release();
            }
        } else {
            g_greyIdleMs = 0.0f;
        }
        if (g_dir == 0) return;
        if (g_dir > 0) {
            g_t = (std::min)(1.0f, g_t + dt / kOpenMs);
            if (g_t >= 1.0f) g_dir = 0;
        } else {
            g_t = (std::max)(0.0f, g_t - dt / kCloseMs);
            if (g_t <= 0.0f) {
                g_dir = 0;
                ShowMenu(false);          // only once the close has played out
                g_pick = g_pickGroup = -1;
                g_sel = -1;
            }
        }
    }

    void Draw()
    {
        if (g_t <= 0.001f) return;
        const auto& io = ImGui::GetIO();
        const float S = Scale();
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const float side = kWheelPx * S;
        const float k = side / (kTexHalf * 2.0f);
        // ★The icons and the hub ride the SMALLER of the two envelopes. The
        // ring re-inks slot by slot on a list switch; if the things standing on
        // it did not go with it, the old list's icons would hang in the air
        // over a ring that is being repainted underneath them.
        const float ge = (std::min)(g_t, g_open ? g_groupT : 1.0f);
        const float e = 1.0f - std::pow(1.0f - ge, 3.0f);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##wheeler", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        auto* dl = ImGui::GetWindowDrawList();

        // ---- drain the colour out of the world ------------------------------
        const int fade = static_cast<int>(255 * std::clamp(g_t * 1.6f, 0.0f, 1.0f));
        if (!Grey::failed) {
            dl->AddCallback(&Grey::CaptureCB, nullptr);
            if (Grey::srv) {
                dl->AddImage(reinterpret_cast<ImTextureID>(Grey::srv),
                    ImVec2(0, 0), io.DisplaySize, ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, fade));
            }
            dl->AddCallback(&Grey::RestoreCB, nullptr);
            dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        } else {
            // fallback: no shader, so the best available is a plain dark plate
            dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                IM_COL32(10, 11, 12, static_cast<int>(fade * 0.55f)));
        }

        // ---- where the pointer is -------------------------------------------
        if (g_open) {
            const float d = std::sqrt(g_mx * g_mx + g_my * g_my);
            if (d > kDeadzone) {
                float a = std::atan2(g_my, g_mx) * 57.29578f;   // screen degrees
                // slot 0 is centred on twelve o'clock (-90 in screen degrees)
                float rel = std::fmod(a + 90.0f + kStep * 0.5f + 720.0f, 360.0f);
                const int s = std::clamp(static_cast<int>(rel / kStep), 0, kSlots - 1);
                // ★★The stroke is NOT redrawn on every slot change. It was, and
                // that is why the highlight vanished the moment the wheel was
                // used: sweeping across slots reset the draw-on faster than it
                // could play, so it never got past its first frame. The brush
                // lays down ONCE when the wheel opens and after that the stroke
                // simply moves.
                // ★★★A DRAG REARRANGES AS IT TRAVELS. Carry a slot past its
                // neighbour and that neighbour steps aside NOW, not when the
                // button comes up: the list is the thing being edited, and it
                // should look edited while the hand is still on it. Doing it
                // all at the drop would make the wheel jump at the moment the
                // player has already stopped paying attention to it.
                // ★★★EVERY NEW SLOT IS A NEW MARK, aim included. The reset used
                // to be kept out of this path on the reasoning that a sweeping
                // hand would re-trigger it every frame and never get past the
                // first -- true, but it only matters WHILE the hand is
                // sweeping, and nobody is reading the highlight then. The
                // moment it comes to rest the stroke draws in full. Leaving it
                // out is why the animation was invisible in ordinary use: the
                // wheel opens, the brush finishes against the seeded slot, and
                // from then on aiming only slid a finished mark around.
                if (s != g_sel) { g_arcT = 0.0f; StepBlip(); }
                // ★★Only follow the aim if the list ACCEPTED the move. On the
                // set wheels it can decline -- a tab cannot be dropped onto
                // EQUIP, nor onto a place where no tab exists -- and moving
                // the mark anyway left the carried piece sitting where it was
                // while the drag believed it was somewhere else. Declined, the
                // hand simply passes over: the highlight goes, the piece stays.
                if (s != g_sel && g_dragFrom >= 0 && G(g_group).reorder &&
                    G(g_group).reorder(g_dragFrom, s)) {
                    g_dragFrom = s;   // the carried thing is at the new place now
                    g_dragMoved = true;
                    g_arranged = true;   // ...and the release must not act
                }
                g_sel = s;
            }
            // ★★★...and BRINGING IT BACK TO THE MIDDLE takes the choice back.
            // That is the way out of a full wheel: aim at something, change
            // your mind, return to centre, let go, and nothing happens.
            // ★It once flickered, and the cause was the OTHER half of this
            // rule rather than this line: while the set-pickers still opened
            // on the active row, that seeded mark was cleared the instant the
            // wheel appeared, because the hand had not moved yet and the aim
            // was already at centre. Now that nothing is ever preselected the
            // two halves agree, and the exception they needed is gone.
            // ★Except mid-drag: the carried slot has to stay carried while the
            // hand crosses the middle, or the drop loses what it was holding.
            else if (g_dragFrom < 0) {
                g_sel = -1;
            }
        }

        const int shown = g_open ? g_sel : g_pick;
        const int shownGroup = g_open ? g_group : (g_pickGroup >= 0 ? g_pickGroup : g_group);

        if (!g_texOk) {
            Halo(dl, c, "GridInventory_wheel textures missing", 20.0f * S, kInkText, 255);
            ImGui::End();
            return;
        }

        // ★Warm the costume icons while the PRESET list is on screen. The miss
        // handler below only runs for the group being drawn, so without this a
        // click would land on a wheel of blanks and fill in a frame later. One
        // slot per frame: the whole list is ready inside ten frames, and no
        // single frame pays for nine texture uploads.
        if (g_open) {
            g_warm = (g_warm + 1) % kSlots;
            if (Filled(kCostume, g_warm)) {
                if (auto* face = G(kCostume).face(g_warm)) {
                    auto* cache = IconCache::GetSingleton();
                    if (!cache->Get(face)) cache->QueueCapture(face);
                }
            }
        }

        // ★★★HOW FAR ALONG ONE SLOT IS, and the only place that answers. The ink
        // and the picture in it are ONE THING arriving or leaving, so asking
        // separately is how they came apart: the ink knew about the stagger and
        // about keeping the pick, the icons knew only the wheel's overall
        // easing, and on the way out every icon faded together while the ink
        // under them left one at a time. What you saw was pictures hanging in
        // the air over slots that were no longer there.
        const auto slotProgress = [&](int i) {
            // clockwise from twelve, each slot a little after the one before it
            const float st = (static_cast<float>(i) / (kSlots - 1)) * (1.0f - kStaggerSpan);
            if (g_dir >= 0) {
                float lt = std::clamp((g_t - st) / kStaggerSpan, 0.0f, 1.0f);
                // ★A list switch runs the SAME stagger again, and takes the
                // lower of the two. Lower, not replaced: mid-open the wheel is
                // still arriving, and a switch must not push a slot further
                // along than the open has actually got.
                if (g_groupT < 1.0f) {
                    lt = (std::min)(lt,
                        std::clamp((g_groupT - st) / kStaggerSpan, 0.0f, 1.0f));
                }
                return lt;
            }
            // ★Keep the pick: the others leave FIRST and the chosen slot
            // holds on. 2.2/-1.2 empties the ring by 45% of the close,
            // leaving the rest of it for the one slot that matters -- at
            // 1.8/-0.8 the two halves overlapped enough to read as one
            // fade, which is not what "keep the pick" means.
            return (i == shown) ? g_t : std::clamp(g_t * 2.2f - 1.2f, 0.0f, 1.0f);
        };

        // ---- the ink, slot by slot from twelve o'clock -----------------------
        // The carried slot is held back and drawn after the loop, over the rest.
        int   carriedIdx = -1;
        float carriedLt = 0.0f;
        int   carriedA = 0;
        for (int i = 0; i < kSlots; ++i) {
            const float lt = slotProgress(i);
            if (lt <= 0.005f) continue;
            const float le = 1.0f - std::pow(1.0f - lt, 3.0f);
            const bool has = Filled(shownGroup, i);
            // ★An empty slot is still a PLACE -- the wheel's shape is what the
            // hand learns, so it has to be there. 150 made it a rumour.
            const int a = static_cast<int>((has ? 255.0f : 215.0f) *
                                           (std::min)(1.0f, lt * 2.4f));
            // ★★ROTATION ZERO. Each slot texture was cut from the painting
            // WHERE IT ALREADY SAT, so it arrives in place -- turning it by
            // i*36 turned it a second time and the ten slots landed on
            // 72i mod 360, which is five positions with two textures stacked on
            // each and five bare. That is the "every other one missing" pattern.
            // (The plan had been one texture rotated ten times; the bake did
            // something better, and the draw code was still following the plan.)
            // ★ROTATION IS THE OFFSET FROM HOME, not the slot's angle. Each
            // texture was cut from the painting where it already sat, so it
            // arrives in place at rotation zero; turning it by (displayed -
            // home) carries the ink along with a slot being pushed aside and
            // leaves a settled ring untouched.
            // ★★And the carried slot is drawn LAST and LIT -- black ink sliding
            // over black ink is invisible however large it grows, so the size
            // change alone cannot say which one is travelling.
            const float home = i * kStep;
            const float shown = g_angleInit ? g_slotAngle[i] : home;
            const bool carried = g_open && i == g_dragFrom && shownGroup == g_group;
            if (carried) { carriedIdx = i; carriedLt = le; carriedA = a; continue; }
            DrawWheelTex(dl, g_slot[i], c, side * (0.62f + 0.38f * le), shown - home,
                (kInk & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24));
        }
        // ★The one being carried: bigger, lighter, and over everything else.
        // Lighter is not decoration -- see above.
        if (carriedIdx >= 0) {
            constexpr ImU32 kLift = IM_COL32(0x2b, 0x28, 0x26, 255);
            const float le = 1.0f - std::pow(1.0f - carriedLt, 3.0f);
            const float home = carriedIdx * kStep;
            const float shown = g_angleInit ? g_slotAngle[carriedIdx] : home;
            DrawWheelTex(dl, g_slot[carriedIdx], c,
                side * (0.62f + 0.38f * le) * 1.10f, shown - home,
                (kLift & 0x00FFFFFF) | (static_cast<ImU32>(carriedA) << 24));
        }

        // ★The ring's leading edge IS the slot-start front. A slot begins at
        // g_t = (i/9) * (1 - kStaggerSpan), so the front reaches the last slot
        // at 1 - kStaggerSpan and the ring closes its circle on the same beat:
        // one clockwise motion that lays the ground and drops the slots onto
        // it, rather than two things starting at once.
        // ★Raw g_t, not the eased e, because the stagger it is matching is on
        // raw g_t as well -- easing only this one would make the ring drift
        // ahead of the slots in the middle of the sweep and catch up at the end.
        if (g_dir >= 0) {
            // ★Kept, not recomputed, once the close begins -- see below.
            // ★★Runs to 2, not 1. 1 is the closed circle -- the beat the slot
            // stagger is tied to -- and the second unit is the overlap the
            // stroke lays over its own start (DrawRingSweep). It fits in the
            // time the ring already had spare: the front reaches the last slot
            // at 1 - kStaggerSpan, and the stagger's own tail is still running.
            g_ringP = std::clamp(g_t / (1.0f - kStaggerSpan) , 0.0f, 2.0f);
            // ★★A list switch re-inks the RING as well, on the same rule the
            // slots use (slotProgress): run the sweep again and take the LOWER
            // of the two. The ring's leading edge is the slot-start front, so
            // leaving it whole while the slots re-staggered underneath broke
            // the one thing this animation is built on -- the front laying
            // ground and the slots dropping onto it. Lower, not replaced,
            // because a switch mid-open must not push the ring further round
            // than the open has actually got.
            if (g_groupT < 1.0f) {
                g_ringP = (std::min)(g_ringP,
                    std::clamp(g_groupT / (1.0f - kStaggerSpan), 0.0f, 2.0f));
            }
            if (g_ringP >= 1.999f) {
                DrawWheelTex(dl, g_ring, c, side, 0.0f,
                    (kInk & 0x00FFFFFF) | (static_cast<ImU32>(245) << 24));
            } else {
                DrawRingSweep(dl, c, side, g_ringP, 245);
            }
        } else {
            // ★★Closing DRAWS IN, toward the hub. Not backwards along its own
            // sweep: the hand in this wheel only ever travels clockwise -- the
            // slots ink that way and the brush stroke was deliberately flipped
            // to match -- so an anticlockwise un-draw would be the one motion
            // on screen running against everything else.
            // ★Scale is the SAME easing the open uses, read at the closing
            // value. That gives the shape for free: near-full for the first
            // half of the close, while the unchosen slots clear, then in
            // quickly -- so the contraction happens against the one slot left
            // standing rather than competing with nine that are leaving.
            const float sc = e;
            // ★Fade only once it is already small. Shrinking alone would end
            // on a hard dot; fading from the start would make the shape
            // pointless because there would be nothing left to see move.
            const int a = static_cast<int>(
                245 * std::clamp(e / 0.25f, 0.0f, 1.0f));
            if (a > 2 && sc > 0.002f) {
                // ★A wheel let go of DURING the open never finished its sweep,
                // and must contract as the partial ring it actually is. This
                // is why g_ringP is latched: recomputing it here would shrink
                // with one hand and un-draw anticlockwise with the other.
                if (g_ringP >= 1.999f) {
                    DrawWheelTex(dl, g_ring, c, side * sc, 0.0f,
                        (kInk & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24));
                } else {
                    DrawRingSweep(dl, c, side * sc, g_ringP, a);
                }
            }
        }

        // ---- the cursor's trail ----------------------------------------------
        // ★★Drawn AFTER the ring and before the hub, so the marks lie on the
        // pale wash in the middle rather than under it. Under the ring they
        // were invisible for exactly the region they exist to serve.
        // ★They fade out toward the deadzone edge, where the highlight stroke
        // takes over: two answers to "what am I pointing at" must not be up at
        // once, and handing off across the boundary is what makes them read as
        // one control rather than two.
        if (g_drops.srv && e > 0.5f) {
            const float wheelA = (std::min)(1.0f, (e - 0.5f) / 0.35f);
            for (const auto& d : g_drop) {
                if (d.age >= 1.0f) continue;
                // ★Drying, not fading evenly: a mark holds its colour for the
                // first third and then goes quickly. A linear fade reads as ten
                // grey dots of graded value -- a gradient, not ink.
                const float dry = std::clamp((d.age - 0.30f) / 0.70f, 0.0f, 1.0f);
                const float life = 1.0f - dry * dry;
                // ...and out at the deadzone edge, by where the MARK sits.
                // ★★It starts LATE, at 0.70 of the way out. Two fades multiply:
                // a mark is already dimmed by drying, and beginning the second
                // one near the middle left the trail faint over most of the
                // ground it covers -- the handover to the highlight has to
                // happen AT the boundary, not all the way to it.
                const float dist = std::sqrt(d.x * d.x + d.y * d.y);
                const float edge = 1.0f - std::clamp(
                    (dist - kDeadzone * 0.70f) / (kDeadzone * 0.30f), 0.0f, 1.0f);
                const float av = life * edge * wheelA;
                if (av <= 0.01f) continue;

                // ★★The trail's reach is DERIVED, not chosen: at the edge of
                // the deadzone a mark should sit just inside the ring's inner
                // rim, because the deadzone IS the middle and the trail is what
                // draws it. Picking a pixel figure instead gave a trail that
                // used a quarter of the hole it lives in, and would have come
                // adrift the first time either number moved.
                const float px = ((kRSegIn * k) * 0.72f) / kDeadzone;
                const ImVec2 m(c.x + d.x * px, c.y + d.y * px);
                // ★A mark spreads a little as it dries.
                // ★★This is the QUAD's half-extent, not the ink's. The painted
                // drop fills about a sixth of its cell -- it has to leave room
                // for a tail and specks -- so a quad sized to look right on
                // paper puts six pixels of ink on screen, which reads as dust
                // rather than as a mark. Size the quad for the ink inside it.
                const float sz = 24.0f * S * d.scale * (1.0f + 0.22f * d.age);
                const float cs = std::cos(d.rot), sn = std::sin(d.rot);
                const ImVec2 q[4] = {
                    ImVec2(m.x + (-sz * cs - -sz * sn), m.y + (-sz * sn + -sz * cs)),
                    ImVec2(m.x + (sz * cs - -sz * sn),  m.y + (sz * sn + -sz * cs)),
                    ImVec2(m.x + (sz * cs - sz * sn),   m.y + (sz * sn + sz * cs)),
                    ImVec2(m.x + (-sz * cs - sz * sn),  m.y + (-sz * sn + sz * cs)),
                };
                constexpr float uv = 1.0f / static_cast<float>(kDropAtlas);
                const float u = static_cast<float>(d.cell % kDropAtlas) * uv;
                const float v = static_cast<float>(d.cell / kDropAtlas) * uv;
                // ★Mirroring is a SWAP OF THE TWO U COLUMNS, not a negative
                // width -- the quad's corners are already rotated, so flipping
                // the geometry would turn the drop as well as reflect it.
                const float uL = d.mirror ? u + uv : u;
                const float uR = d.mirror ? u : u + uv;
                const int ia = static_cast<int>(av * 235.0f);
                dl->AddImageQuad(reinterpret_cast<ImTextureID>(g_drops.srv),
                    q[0], q[1], q[2], q[3],
                    ImVec2(uL, v), ImVec2(uR, v), ImVec2(uR, v + uv), ImVec2(uL, v + uv),
                    (kInk & 0x00FFFFFF) | (static_cast<ImU32>(ia) << 24));
            }
        }

        // ---- the hover stroke -------------------------------------------------
        // ★It marks WHERE THE POINTER IS, so it lands on empty slots too. Gating
        // it on "this slot has something in it" meant that with two presets the
        // wheel gave no answer to eight directions out of ten -- it read as the
        // highlight being broken. Releasing on an empty slot still does nothing;
        // that is Apply()'s job, not the pointer's.
        if (e > 0.55f && shown >= 0) {
            const int a = static_cast<int>(255 * (std::min)(1.0f, (e - 0.55f) / 0.3f));
            DrawArc(dl, c, side, shown * kStep,
                    g_open ? g_arcT : 1.0f, a);
        }

        // ---- medallions -------------------------------------------------------
        // ★Each rides its OWN slot, from slotProgress above -- same stagger on
        // the way in, same keep-the-pick on the way out.
        {
            // ★★DIAGNOSTIC. "Every icon is a drawing" and "the pak has no
            // sprite for these items" look identical on screen and identical
            // in the log -- a key that misses queues a capture, and the queue
            // only turns while the INVENTORY is open, so neither case ever
            // prints a capture line. That ambiguity cost a wrong diagnosis
            // (read as a coverage gap; it was the capture lamp defaulting to
            // 0,0 because the ui ini had not been read yet), so the two are
            // told apart here, once per open, by counting what the draw
            // actually resolved. The LAMP is printed beside the counts because
            // it is half of every cache key: a whole-wheel miss with a
            // non-zero pak is that number being wrong.
            int diagHit = 0, diagMiss = 0;
            for (int i = 0; i < kSlots; ++i) {
                if (!Filled(shownGroup, i)) continue;
                const float lt = slotProgress(i);
                // ★The picture comes in behind its ink rather than with it: the
                // slot is a surface and you paint on a surface once it is there.
                // Remapped from the slot's own progress, not from the wheel's,
                // so "behind its ink" stays true for every slot separately.
                const float it = std::clamp((lt - 0.42f) / 0.58f, 0.0f, 1.0f);
                if (it <= 0.005f) continue;
                const float ie = 1.0f - std::pow(1.0f - it, 3.0f);
                const int a = static_cast<int>(255 * (std::min)(1.0f, it * 2.4f));
                const float ang = (-90.0f + (g_angleInit ? g_slotAngle[i] : i * kStep))
                                  * 0.01745329f;
                // ★Out from the middle with the ink, on the same 0.62..1 ramp
                // the slot texture uses -- a picture that held its final radius
                // while the slot grew under it was sliding across its own slot.
                const float grow = 0.62f + 0.38f * ie;
                // ...and up with the carried slot, which is lifted 1.10x.
                const float lift =
                    (g_open && i == g_dragFrom && shownGroup == g_group) ? 1.10f : 1.0f;
                const float r = (kRSegIn + kRSegOut) * 0.5f * k * grow * lift;
                const ImVec2 m(c.x + r * std::cos(ang), c.y + r * std::sin(ang));
                // ★Size follows more gently than position: the ink's full 0.62
                // applied to a 26px picture is a smudge for the first half of
                // the open, and a picture too small to recognise is worse than
                // one that is merely early.
                const float sz = 26.0f * S * (0.80f + 0.20f * ie) * lift;
                // ★★A spell reads SMALLER than a shout at the same nominal size,
                // and deliberately. A sigil is a solid shape filling its square;
                // a shout's word is two to five thin signs strung out sideways,
                // so the same box holds far less ink. Matching the boxes made
                // the sigils shout over the words -- these are the numbers that
                // make the two weigh the same on the ring.
                // ★0.92, up from 0.80. The schools are mostly open shapes -- a
                // bird, a tree, three rings -- so they carry far less ink than
                // their box suggests and read small beside a filled mark. The
                // power sigil keeps its size by being drawn smaller INSIDE its
                // file (82% of the tile against the schools' 94%), which is
                // also where its edges got softened; scaling it here instead
                // would have enlarged the aliasing along with the shape.
                constexpr float kSigilScale = 0.92f;   // spell, of the medallion box
                constexpr float kWordScale  = 1.00f;   // shout, ...along its length
                // Which of the two the group wants is the table's answer now.
                if (auto* face = G(shownGroup).face(i)) {
                    auto* cache = IconCache::GetSingleton();
                    const auto* icon = face ? cache->Get(face) : nullptr;
                    // counted BEFORE the fallback, which is the only point at
                    // which the two answers are still distinguishable
                    if (icon) ++diagHit; else ++diagMiss;
                    // ★★A miss here is "not loaded YET", not "no icon". The
                    // sprite pak survives a restart; the D3D texture does not,
                    // and nothing uploads an item's sprite until something
                    // DRAWS it. Until now the only thing that ever drew these
                    // was the inventory -- which is exactly why the wheel came
                    // up blank after a restart and filled in once each costume
                    // had been opened by hand. Asking loads it from the pak on
                    // the spot; a costume never captured before joins the
                    // normal queue instead of staying invisible forever.
                    if (!icon && face) {
                        cache->QueueCapture(face);
                        // ★★★AND FALL BACK TO THE DRAWN ICON, exactly as the
                        // grid does. The capture queue only turns while the
                        // INVENTORY is open -- photographing an item means
                        // standing its model in a 3D scene, and the wheel has
                        // no such scene -- so an item never yet captured could
                        // not be made here however politely we asked. It was
                        // simply blank until the player opened their bag.
                        // ★A category drawing is not a placeholder for the
                        // capture; it is the same answer the board gives, so
                        // the two surfaces agree instead of one of them being
                        // empty.
                        icon = Fallback::GetDrawn(face).icon;
                    }
                    if (icon && icon->srv) {
                        const auto tex = reinterpret_cast<ImTextureID>(icon->srv);
                        // ★Fit the LONGER side to the box and let the other keep
                        // its proportion. Item captures are not square -- a 2x3
                        // sprite forced into a square comes out squashed, and a
                        // squashed cuirass is a different-looking cuirass.
                        const float aw = icon->w > 0 ? static_cast<float>(icon->w) : 1.0f;
                        const float ah = icon->h > 0 ? static_cast<float>(icon->h) : 1.0f;
                        // ★Fitting the long side to the SAME box a medallion uses
                        // makes the captured item come out visibly smaller: the
                        // medallion fills its square in both directions, a 2x3
                        // sprite only fills one. Match what the eye sees, not the
                        // box -- and a capture carries its own margin on top.
                        const float fit = (sz * 1.34f) / (std::max)(aw, ah);
                        const float hw = aw * fit, hh = ah * fit;
                        const ImVec2 q0(m.x - hw, m.y - hh), q1(m.x + hw, m.y + hh);
                        // ★★A menu-captured item is a DARK object and it lands on
                        // BLACK ink here. The grid already solved this: a shader
                        // that draws the sprite's alpha alone, stamped in white
                        // around it, gives a halo no tint can (black collapses
                        // the colour, white multiplies to the sprite itself).
                        constexpr int kSpokes = 8;
                        if (UIRoot::BeginSilhouette(dl)) {
                            // stacked alpha is not additive: solve per stamp or
                            // the middle of the halo goes solid white
                            const float want = 0.66f * (a / 255.0f);
                            const float per = 1.0f - std::pow(1.0f - want,
                                1.0f / static_cast<float>(kSpokes));
                            const int pa = static_cast<int>(per * 255.0f + 0.5f);
                            const float rr = 2.0f * S;
                            if (pa > 0) {
                                const ImU32 hc = IM_COL32(255, 255, 255, pa);
                                for (int sp = 0; sp < kSpokes; ++sp) {
                                    const float th = sp * (6.28318531f / kSpokes);
                                    const ImVec2 o(std::cos(th) * rr, std::sin(th) * rr);
                                    dl->AddImage(tex, ImVec2(q0.x + o.x, q0.y + o.y),
                                        ImVec2(q1.x + o.x, q1.y + o.y),
                                        ImVec2(0, 0), ImVec2(1, 1), hc);
                                }
                            }
                            UIRoot::EndSilhouette(dl);
                        }
                        dl->AddImage(tex, q0, q1, ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32(255, 255, 255, a));
                    }
                } else if (DrawMagicMark(dl, m, sz, shownGroup, i, a,
                                         kSigilScale, kWordScale)) {
                    // a spell's school sigil, or a shout's word in dragon script
                } else if (const auto* med = Medallion(G(shownGroup).medallion(i))) {
                    dl->AddImage(reinterpret_cast<ImTextureID>(med->srv),
                        ImVec2(m.x - sz, m.y - sz), ImVec2(m.x + sz, m.y + sz),
                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
                }
                // ★"This one is on" -- a painted check, not a dot. A 4px circle
                // was a bullet hole in a wheel made entirely of brush work, and
                // at that size it read as dirt on the ring rather than a mark
                // anyone put there.
                // ★It keeps the DOT'S anchor (upper right of the medallion) at
                // a brush stroke's size. A stroke needs room to be recognised
                // as one: shrunk to the dot's footprint it is just a red smear,
                // and the tail that makes a check a check disappears first.
                if (IsCurrent(shownGroup, i) && g_tick.srv) {
                    const float ts = sz * 2.0f;   // half-extent, so 4x the medallion radius
                    const ImVec2 tc(m.x + sz * 0.72f, m.y - sz * 0.72f);
                    dl->AddImage(reinterpret_cast<ImTextureID>(g_tick.srv),
                        ImVec2(tc.x - ts, tc.y - ts), ImVec2(tc.x + ts, tc.y + ts),
                        ImVec2(0, 0), ImVec2(1, 1),
                        (kRed & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24));
                }
            }
            // ★Held until the wheel is FULLY open. Slots fade in one at a
            // time, so an early frame has visited only the first few and would
            // report a partial wheel as the whole answer -- the same "counted
            // what happened to be there" mistake this line exists to end.
            // ★Only when something MISSED. It used to report every open, which
            // is a line per wheel for a number that is almost always "all of
            // them" -- the counts existed to catch a pak keyed on the wrong
            // lamp, and that is a fault, not a status. Silence means correct.
            if (g_diagIcons && ge >= 1.0f) {
                g_diagIcons = false;
                if (diagMiss > 0) {
                    SKSE::log::warn(
                        "[WHEEL] icons: {} from cache, {} fell back to a drawing "
                        "(group {}, caplight {:.0f},{:.0f})",
                        diagHit, diagMiss, G(shownGroup).title,
                        Theme::CaptureLightAz(), Theme::CaptureLightEl());
                }
            }
        }

        // ---- the middle: a name and a position, nothing else -------------------
        if (e > 0.5f) {
            const int a = static_cast<int>(255 * (std::min)(1.0f, (e - 0.5f) / 0.4f));
            // ★Plain ink, no edging. The pale wash under the hub is even and
            // light enough that black needs no help standing on it -- an
            // outline there only fattened the letters and read as a sticker.
            // ★A real bold FACE, drawn once. Weight from redrawing the glyph at
            // small offsets looked heavy in a mockup and smeared in the game:
            // every copy lands on its own subpixel phase, so the counters
            // inside the letters fill with half-tone and the word grows a
            // ghost. One pass of a bold cut has none of that.
            auto plain = [&](float dy, const char* s, float sz, int al) {
                if (!s || !*s || al <= 2) return;
                auto* fnt = UIRoot::BoldFont(s);
                const ImVec2 ts = fnt->CalcTextSizeA(sz, FLT_MAX, 0.0f, s);
                dl->AddText(fnt, sz, ImVec2(c.x - ts.x * 0.5f, c.y + dy - ts.y * 0.5f),
                    (kInk & 0x00FFFFFF) | (static_cast<ImU32>(al) << 24), s);
            };
            // The same, for one SPAN of a string -- a wrapped line is not
            // null-terminated at its end.
            // ★The face is chosen once by the caller and passed in, not asked
            // per line: BoldFont answers for a whole string, so asking it about
            // each half could set one line in bold and the other in the main
            // face when only the second half has a glyph it cannot spell.
            auto span = [&](ImFont* fnt, float dy, const char* b, const char* e,
                            float sz, int al) {
                if (!fnt || !b || b >= e || al <= 2) return;
                const ImVec2 ts = fnt->CalcTextSizeA(sz, FLT_MAX, 0.0f, b, e);
                dl->AddText(fnt, sz, ImVec2(c.x - ts.x * 0.5f, c.y + dy - ts.y * 0.5f),
                    (kInk & 0x00FFFFFF) | (static_cast<ImU32>(al) << 24), b, e);
            };
            // ★★Nothing aimed at means an EMPTY middle. There used to be a
            // "pick" prompt here, from when the centre had no other way to say
            // it was the neutral place; the ink trail says it now, and says it
            // better -- it moves with the hand, so it is feedback rather than
            // an instruction. A word left sitting under the trail would also
            // be the one piece of the wheel that never moves.
            if (shown >= 0) {
                const bool has = Filled(shownGroup, shown);
                const char* nm = has ? NameOf(shownGroup, shown) : "\xE2\x80\x94";
                const int   na = has ? a : static_cast<int>(a * 0.75f);
                const float nsz = 25.0f * S;
                // ★★The hub is a CIRCLE, so a long name does not merely
                // overflow its box -- it runs out over the ring and into the
                // slots. Names are the player's own ("Heavy Armour Set"), so
                // there is no length to design for; the hub folds instead.
                // ★The width is the ring's inner rim, not a chosen pixel
                // figure, so it stays right at any resolution. 1.62 rather than
                // 2.0 leaves the margin a circle needs -- a chord at the line's
                // height is shorter than the diameter, and the second line sits
                // further from the middle than the first.
                const float wrapW = (kRSegIn * k) * 1.62f;
                auto* fnt = UIRoot::BoldFont(nm);
                const char* end = nm + std::strlen(nm);
                // ★★Ask ImGui where to break. It breaks at the last SPACE that
                // fits and only falls to mid-word when one word is wider than
                // the line -- which is the behaviour a name wants. It also
                // knows UTF-8 and CJK, and both matter here: a Korean preset
                // name has no space to find and its characters are three bytes
                // each, so anything cutting by hand either never folds or
                // splits a glyph in half.
                //
                // ★★MEASURE THE RESULT, do not trust the request. What comes
                // back is wider than what was asked for -- ImGui allows itself
                // some slack before deciding a line is full -- and the exact
                // amount is an implementation detail this build does not ship
                // the source for. So it is not assumed: ask, measure what came
                // back, and pull the request in until the answer is true.
                // ★It matters most on the second line, which sits further from
                // the middle and so has less of the circle to work with.
                const char* brk = nullptr;
                for (float ask = wrapW; ask > wrapW * 0.5f; ask -= nsz * 0.5f) {
                    brk = fnt->CalcWordWrapPosition(nsz, nm, end, ask);
                    if (!brk || brk >= end || brk <= nm) break;
                    if (fnt->CalcTextSizeA(nsz, FLT_MAX, 0.0f, nm, brk).x <= wrapW) break;
                }
                // ★Trim the break from BOTH sides. Whether ImGui hands back the
                // position of the space or the one after it is an
                // implementation detail, and a trailing space left on line one
                // shifts that line off centre by its own width -- a
                // wrong-looking result from a correct break.
                const char* firstEnd = brk;
                const char* second = brk;
                if (brk && brk > nm && brk < end) {
                    while (firstEnd > nm && (firstEnd[-1] == ' ' || firstEnd[-1] == '\t')) {
                        --firstEnd;
                    }
                    while (second < end && (*second == ' ' || *second == '\t')) ++second;
                }
                // Trailing blanks can leave nothing for the second line; that
                // is a one-line name with untidy spacing, not a two-line one.
                const bool folded = brk && firstEnd > nm && second < end;

                float posY = 20.0f * S;
                if (folded) {
                    // ★Two lines are set about the SAME centre the one line
                    // used, not stacked downward from it -- the hub's middle is
                    // where the eye already is, and a name that grew a line
                    // should not appear to slide.
                    const float lh = fnt->CalcTextSizeA(nsz, FLT_MAX, 0.0f, "A").y;
                    // ★★TWO lines, not as many as it takes. A third would be
                    // taller than the hub is wide at that height, so the name
                    // would leave the circle by the top and bottom instead of
                    // by the sides -- the same fault, turned ninety degrees.
                    // What does not fit is cut and marked, which at least says
                    // "there is more of this name" instead of drawing it over
                    // the ring.
                    // Does the rest fit on one line? Same measure-the-result
                    // rule as above -- the slack applies here too.
                    const bool tail =
                        fnt->CalcTextSizeA(nsz, FLT_MAX, 0.0f, second, end).x > wrapW;
                    span(fnt, -6.0f * S - lh * 0.5f, nm, firstEnd, nsz, na);
                    if (tail) {
                        // ★Cut to the width MINUS the ellipsis, measured, so the
                        // mark that says "there is more" does not itself become
                        // the thing that overflows.
                        const float room =
                            wrapW - fnt->CalcTextSizeA(nsz, FLT_MAX, 0.0f, "\xE2\x80\xA6").x;
                        const char* cut = second;
                        for (float ask = room; ask > nsz; ask -= nsz * 0.5f) {
                            cut = fnt->CalcWordWrapPosition(nsz, second, end, ask);
                            if (!cut || cut <= second) { cut = second; break; }
                            if (fnt->CalcTextSizeA(nsz, FLT_MAX, 0.0f, second, cut).x <= room) break;
                        }
                        if (cut <= second) cut = end;   // nothing fits: draw what we have
                        char buf[96];
                        const size_t n = (std::min)(sizeof(buf) - 4,
                                                    static_cast<size_t>(cut - second));
                        std::memcpy(buf, second, n);
                        std::memcpy(buf + n, "\xE2\x80\xA6", 4);   // "…" + NUL
                        span(fnt, -6.0f * S + lh * 0.5f, buf, buf + n + 3, nsz, na);
                    } else {
                        span(fnt, -6.0f * S + lh * 0.5f, second, end, nsz, na);
                    }
                    // ...and the counter steps out of the way of the second one
                    posY = 20.0f * S + lh * 0.6f;
                } else {
                    plain(-6.0f * S, nm, nsz, na);
                }
                char pos[24];
                std::snprintf(pos, sizeof(pos), "%d / %d", shown + 1, kSlots);
                plain(posY, pos, 14.0f * S, static_cast<int>(a * 0.8f));
            }
            // ★The title rides a painted brush banner, not a rectangle. A hard
            // black box was the one straight edge left on a wheel made entirely
            // of ink, and it read as a placeholder because it was one.
            // ★And no key cap here. The bottom prompt bar already says LMB;
            // saying it twice on one screen is one more thing to read.
            const float ty = c.y - side * 0.5f - 22.0f * S;
            if (g_tab.srv && g_tab.w > 0) {
                const float bw = 124.0f * S;
                const float bh = bw * static_cast<float>(g_tab.h) / static_cast<float>(g_tab.w);
                dl->AddImage(reinterpret_cast<ImTextureID>(g_tab.srv),
                    ImVec2(c.x - bw, ty - bh), ImVec2(c.x + bw, ty + bh),
                    ImVec2(0, 0), ImVec2(1, 1),
                    (kInk & 0x00FFFFFF) | (static_cast<ImU32>(a * 0.95f) << 24));
            }
            Halo(dl, ImVec2(c.x, ty), G(shownGroup).title, 16.0f * S, kPaper, a);
        }

        ImGui::End();

        // ---- the inventory's own prompt bar ------------------------------------
        using LS = Lang::Str;
        std::vector<UIRoot::PromptBit> bits;
        if (g_padDriving) {
            bits = { { "STICK", Lang::T(LS::WheelPick) },
                     { "D-PAD", Lang::T(LS::WheelGroup), true },
                     { "", Lang::T(LS::WheelApply), true } };
        } else {
            // ★"MOUSE" named the device, not the control -- and the mouse does
            // two different things here. The wheel picks, the button switches
            // group; a cap that says MOUSE tells you neither.
            bits = { { "WHEEL", Lang::T(LS::WheelPick) },
                     { "A/D", Lang::T(LS::WheelGroup), true },
                     { "", Lang::T(LS::WheelApply), true } };
        }
        UIRoot::DrawPromptRow(bits, false, std::clamp(g_t * 1.6f, 0.0f, 1.0f));
    }
}
