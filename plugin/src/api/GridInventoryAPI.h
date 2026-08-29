#pragma once
// =============================================================================
//  Grid Inventory -- extension ABI v1
// =============================================================================
//  This header is the ONLY thing shared between Grid Inventory (the host, which
//  owns the grid, the tiles and every pixel drawn) and an extension plugin (a
//  "provider", which owns gameplay data the host knows nothing about).
//
//  ABI DISCIPLINE -- the rules below are not style, they are the contract:
//
//   1. <cstdint> is the sole dependency. No CommonLibSSE-NG type, no Dear ImGui
//      type, no STL type ever crosses the boundary. A provider built against a
//      different CommonLib revision, a different ImGui version or a different
//      MSVC runtime must still link and run.
//   2. Every struct is POD with explicit padding and a static_assert on its
//      size. Adding a field to a released struct is a breaking change: bump
//      kABIVersion instead.
//   3. Strings are fixed char[] buffers, UTF-8, NUL-terminated. Never a pointer
//      the other side has to free.
//   4. Pointer arguments are borrowed for the duration of the call only.
//   5. The provider NEVER calls ImGui. It returns data; the host draws it with
//      its own draw list, its own atlas and its own theme. Sharing an ImGui
//      context across a DLL boundary would require sharing the context pointer
//      AND the allocator, and any version drift between the two plugins would
//      be an undiagnosable CTD.
//
//  THREADING -- GetOverlay / GetTooltipLines / OfferDrop are all called from the
//  host's render thread inside an active ImGui frame. Do not block, do not
//  allocate, and do not call a game API that can open or close a menu.
//
//  HANDSHAKE (SKSE messaging, both plugins register a listener in
//  SKSEPluginLoad so ordering does not matter):
//
//      kPostLoad :  host     --[ kMsgHostReady, const HostReady* ]--> everyone
//      kPostLoad :  provider --[ kMsgRegisterProvider, const Provider* ]--> "GridInventory"
//
//  ★AND ONE SIGNAL THAT RUNS THE OTHER WAY (1.4.1):
//
//      any time  :  host     --[ kMsgCostumeState, const CostumeState* ]--> everyone
//
//  Everything above is the outside EXTENDING our UI: we call a Provider and it
//  answers. The costume signal is the opposite direction -- we announce, nobody
//  answers -- so it needs no Provider and no registration. A plugin that does
//  not know this message simply never sees it, which is why adding it did NOT
//  require an ABI bump: message types are separate namespaces, and existing
//  extensions are untouched.
//
//  ★REGISTER TWO LISTENERS, SPLIT BY SENDER (learned the hard way, 2026-07-30):
//
//      RegisterListener(OnLifecycle);            // == RegisterListener("SKSE", ..)
//      RegisterListener(nullptr, OnApiMessage);  // no filter
//
//   Why both:
//    - The one-argument shorthand is RegisterListener("SKSE", cb), which filters
//      out everything a *plugin* sends. With only that, the handshake fails
//      silently in BOTH directions: the host's broadcast never arrives, and the
//      provider's Dispatch finds no matching listener and returns false.
//    - A single unfiltered listener cannot replace the pair, because message
//      type numbers live in the SENDER's namespace. Another plugin's "type 1"
//      is indistinguishable from kPostLoad and its "type 4" from kPostLoadGame,
//      so lifecycle handling would fire at random. Do NOT try to sort it out by
//      inspecting msg->sender -- what that string holds for SKSE's own messages
//      is not something to guess at, and guessing wrong silently disables every
//      lifecycle branch you own.
//    - The unfiltered listener must therefore touch ONLY the 4CC types below.
//      Registering both works because the two use different sender keys.
//
//  The host refuses any Provider whose abiVersion != kABIVersion or whose
//  structSize != sizeof(Provider). Refusal is a log line plus an in-game
//  notification -- never a CTD, never a silent no-op.
//
//  Vendor this file verbatim into a consumer project. Do not edit the copy.
// =============================================================================

#include <cstdint>

namespace GridInvAPI
{
    // ---- versioning -------------------------------------------------------

    inline constexpr std::uint32_t kABIVersion = 1;

    // SKSE plugin name the provider dispatches to, and the host's own name.
    inline constexpr const char* kHostPluginName = "GridInventory";

    inline constexpr std::uint32_t kMsgHostReady        = 0x47494852;  // 'GIHR'
    inline constexpr std::uint32_t kMsgRegisterProvider = 0x47495250;  // 'GIRP'
    inline constexpr std::uint32_t kMsgCostumeState     = 0x47494353;  // 'GICS'
    // ★(1.5.x) SUPPRESS THE GRID'S OWN WINDOW while yours sits over it.
    //
    // Sending UI_MESSAGE_TYPE::kHide to "GridInventoryMenu" does the same
    // thing and needs no header -- that is the standard courtesy and it is
    // answered. This message exists for the case where the intent should be
    // unambiguous: the engine also sends kHide, so a host that wants to know
    // the request came from a MOD rather than from the game reads this one.
    //
    // The grid stays OPEN throughout: its board, the item on the cursor and
    // every sub-window survive, and IsMenuOpen() keeps answering true --
    // it reports the SESSION, not whether the board is on screen, so it does
    // not move when you suppress. (1.5.1 answered liveness there by mistake,
    // which told a client its own suppression was the player closing the
    // inventory; fixed in 1.5.2.)
    //
    // ★★YOU OWN THE HOLD. This message is not the same as kHide in one way
    // that matters: the host's safety net recovers a kHide by watching the
    // MENU STACK, and your window may not be on it at all -- an overlay
    // drawn outside the menu system is invisible to any such test, and the
    // net used to revoke those suppressions about a fifth of a second in.
    // A hold taken with this message is not second-guessed that way, and the
    // host's own kShow will not break it either.
    //
    // ★★AND THERE IS NO TIMER BEHIND IT. The hold does not expire. What that
    // buys costs one obligation, and it is absolute:
    //
    //   RELEASE IT (suppress = 0) ON EVERY PATH THAT CLOSES YOUR WINDOW.
    //
    // Not just the normal one. The cancel, the error return, the hotkey that
    // closes it, the load that happens while it is up -- every exit. While
    // you hold this the player cannot see the inventory and cannot reach it,
    // so a path that forgets is a soft lock, and no timer is coming: a build
    // of this host did carry a ten-minute backstop and it was removed,
    // because nobody sits in front of a frozen game for ten minutes. They
    // kill the process at two.
    //
    // The only other things that take the hold back are the ones that end the
    // session your window was living over anyway: our own close, a save load,
    // and a new game.
    //
    // ★SEND IT WHILE THE INVENTORY IS OPEN. There is nothing to step aside
    // from otherwise, and a hold banked against a session that has not started
    // would surface at the next open as a board that never draws. So one taken
    // with the inventory closed is refused, with a line in our log saying so.
    // Check IsMenuOpen() first, or just send it when your window opens over us.
    //
    // ★DISPATCH FROM ANY THREAD. It is parked and applied on the next game
    // frame, so the grid goes quiet a frame after you ask rather than inside
    // your Dispatch call. Nothing here touches the engine on your thread.
    inline constexpr std::uint32_t kMsgSuppressUI      = 0x47495355;  // 'GISU'

    // ---- limits -----------------------------------------------------------

    inline constexpr std::uint32_t kMaxBadges       = 8;    // D2 caps at 6; 8 is headroom
    inline constexpr std::uint32_t kTooltipTextLen  = 128;  // bytes incl. NUL
    inline constexpr std::uint32_t kMaxTooltipLines = 16;

    // ---- item identity ----------------------------------------------------

    // The only identifier the two sides share. `uid` is ExtraUniqueID, which the
    // engine rewrites on every container move -- so this key is valid for the
    // current frame and must never be persisted by either side.
    //
    // uid == 0 means "a unit of the plain stack": the entry has no ExtraDataList
    // of its own. Such a unit can never carry provider data, and the host still
    // draws a tile for it (see the collection arithmetic in PLAN_INSTANCE 2-B).
    struct ItemKey
    {
        std::uint32_t owner;   // container REFR FormID; the player is 0x00000014
        std::uint32_t base;    // TESBoundObject FormID
        std::uint16_t uid;     // ExtraUniqueID, 0 = plain stack unit
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };
    static_assert(sizeof(ItemKey) == 16, "ItemKey is part of the ABI");

    // ---- badge overlay ----------------------------------------------------

    enum BadgeState : std::uint8_t
    {
        kBadgeEmpty  = 0,   // draw the empty well only
        kBadgeFilled = 1,   // draw iconId inside the well
        kBadgeLocked = 2    // well is present but cannot be used right now
    };

    // `iconId` is a TESBoundObject FormID, NOT an atlas index. The host already
    // renders an icon for any bound object through its own icon cache (model-path
    // hashed, both art styles, capture on demand), so a provider hands over the
    // form of the thing sitting in the well and gets the host's art pipeline for
    // free -- no shared texture handle, no shared atlas, nothing to version.
    //
    //   iconId == 0                      -> host draws its default empty well
    //   iconId != 0, state == kBadgeEmpty -> provider-supplied well art
    //   iconId != 0, state == kBadgeFilled -> that form's icon inside the well
    struct Badge
    {
        std::uint32_t iconId;    // TESBoundObject FormID; 0 = host default well
        std::uint32_t tintRGBA;  // 0 = host default tint
        std::uint8_t  state;     // BadgeState
        std::uint8_t  _pad[3];
    };
    static_assert(sizeof(Badge) == 12, "Badge is part of the ABI");

    // One instance's overlay. `count` is the total number of wells, which is what
    // the host feeds into its layout table together with the tile's width and
    // height -- the geometry is the host's business, the contents are not.
    struct Overlay
    {
        std::uint8_t count;      // 0 = this instance has no overlay
        std::uint8_t _pad[3];
        Badge        badges[kMaxBadges];
    };
    static_assert(sizeof(Overlay) == 100, "Overlay is part of the ABI");

    // ---- tooltip ----------------------------------------------------------

    struct TooltipLine
    {
        char          text[kTooltipTextLen];  // UTF-8, NUL-terminated
        std::uint32_t rgba;                   // 0 = host default colour
        std::uint8_t  indent;                 // 0..3
        std::uint8_t  separatorBefore;        // non-zero = rule above this line
        std::uint8_t  _pad[2];
    };
    static_assert(sizeof(TooltipLine) == 136, "TooltipLine is part of the ABI");

    // ---- drop routing -----------------------------------------------------

    enum DropVerdict : std::uint32_t
    {
        kDropReject  = 0,   // not ours -- host continues its own routing table
        kDropAccept  = 1,   // ours, and legal (commit != 0 means it happened)
        kDropBlocked = 2    // ours, but refused now -- host stops routing, shows refusal
    };

    struct DropQuery
    {
        ItemKey      target;        // instance the cursor is over
        ItemKey      dragged;       // instance being carried
        std::int32_t draggedCount;  // carried quantity (a split fragment may be < stack)
        std::int32_t badgeIndex;    // nearest badge, or -1 if the cursor is outside every well
        std::uint8_t commit;        // 0 = hover query (highlight only), 1 = perform
        std::uint8_t _pad[3];
    };
    static_assert(sizeof(DropQuery) == 44, "DropQuery is part of the ABI");

    // ---- host -> provider services ---------------------------------------

    // What the host's partner window currently is. A provider that must treat a
    // PURCHASE differently from LOOT cannot work this out for itself: the host
    // replaces the vanilla container and barter menus, so RE::BarterMenu is
    // never open during a merchant transaction and the transfer arrives looking
    // exactly like ordinary container loot.
    enum PartnerKind : std::uint32_t
    {
        kPartnerNone       = 0,   // plain inventory -- no partner window
        kPartnerContainer  = 1,   // chest / corpse / companion (incl. stealing)
        kPartnerMerchant   = 2,   // barter: money changes hands
        kPartnerPickpocket = 3    // living target
    };

    struct HostServices
    {
        std::uint32_t structSize;   // = sizeof(HostServices)
        std::uint32_t abiVersion;   // = kABIVersion

        // Ask the host to re-collect the grid on its next frame. Safe to call from
        // any thread; the host only sets a flag.
        void (*RequestRebuild)();

        // True while the grid MENU SESSION is open: its board, the item on the
        // cursor and every sub-window are alive. Suppression (kMsgSuppressUI)
        // does NOT move this -- a hidden grid is still an open one, and a
        // client reading its own suppression back as a close is what this
        // answering liveness caused in 1.5.1. A provider that mutates
        // inventory should check this first; the session is what makes a
        // mutation dangerous, not whether pixels are on screen.
        // Main/game thread only (reads RE::UI's menu map, which is unlocked).
        bool (*IsMenuOpen)();

        // Grant-time tile snapshot: how many grid cells `base` occupies RIGHT NOW
        // under the host's resolved definition (per-form override > model-shared >
        // category preset), counting only the true cells of a polyomino mask --
        // an L-shaped item in a 2x3 bounding box is 5, not 6. The value follows
        // the user's live EDIT-mode tile changes, so a provider that rolls
        // per-instance state from it must call it AT THE MOMENT of the roll and
        // freeze the result; never re-derive persisted state from a later call.
        // Returns 0 when the form is unknown to the host. Main/game thread only
        // (reads the host's def maps without a lock).
        std::uint32_t (*CellSpanOf)(std::uint32_t base);

        // One of PartnerKind, valid for as long as the grid menu is open.
        // Read it while handling an inventory-transfer event to learn HOW the
        // item was acquired -- see the PartnerKind comment above for why the
        // engine's own event cannot answer that.
        std::uint32_t (*PartnerKind)();

        // FormID of the container / merchant / pickpocket target whose window is
        // open, or 0 in kPartnerNone. A provider that wants to act on what is
        // LYING IN a container (rather than on what reaches the player) needs
        // the reference itself, and only the host knows which one it opened.
        std::uint32_t (*PartnerRef)();
    };
    static_assert(sizeof(HostServices) == 48, "HostServices is part of the ABI");

    struct HostReady
    {
        std::uint32_t       structSize;  // = sizeof(HostReady)
        std::uint32_t       abiVersion;  // = kABIVersion
        const HostServices* services;    // owned by the host, valid for the process lifetime
    };
    static_assert(sizeof(HostReady) == 16, "HostReady is part of the ABI");

    // ---- costume state ----------------------------------------------------
    //
    //  A COSTUME is an appearance-only outfit: the body shows the set held by
    //  one loadout tab while the stats keep coming from what is really worn.
    //  Sent whenever that changes -- put on, switched to another tab, or taken
    //  off -- so a companion mod can follow what the player LOOKS like without
    //  reading equipment, which would tell it the wrong thing.
    //
    //  ★SENT ON EVERY TRANSITION, not only the ones a player causes. Loading a
    //  save that had a costume, reverting to a new game, and deleting the tab a
    //  costume pointed at all move this state, and all announce here. A listener
    //  that only handled the button presses would drift out of sync silently.
    //
    //  ★`pieces` IS BORROWED. It points into the host's own buffer and is valid
    //  for the duration of the callback and no longer -- copy what you need
    //  before returning. Length is `pieceCount`, which is 0 when `tab` is -1.
    //
    //  ★A PIECE IS A FORM, not a stack unit: `base` is the armour's FormID,
    //  `owner` is the player, and `uid` is 0. A costume names what to LOOK
    //  like; it does not point at one particular copy in the inventory.
    //
    //  ★ARMOUR ONLY. The loadout tab behind a costume may also hold weapons, a
    //  shield and a quiver -- a costume leaves all of those alone, because they
    //  are held rather than worn. Only the pieces that actually reach the body
    //  are listed here, so every entry is something the player is now seen in.
    struct SuppressUI
    {
        std::uint32_t structSize;   // = sizeof(SuppressUI)
        std::uint32_t abiVersion;   // = kABIVersion
        std::uint32_t suppress;     // 1 = hide the grid, 0 = give it back
    };
    static_assert(sizeof(SuppressUI) == 12, "SuppressUI is part of the ABI");

    struct CostumeState
    {
        std::uint32_t  structSize;   // = sizeof(CostumeState)
        std::uint32_t  abiVersion;   // = kABIVersion
        std::int32_t   tab;          // loadout tab supplying the look; -1 = none
        std::uint32_t  pieceCount;   // 0 when tab is -1
        const ItemKey* pieces;       // BORROWED: valid only during the callback
    };
    static_assert(sizeof(CostumeState) == 24, "CostumeState is part of the ABI");

    // ---- provider -> host ------------------------------------------------

    struct Provider
    {
        std::uint32_t structSize;   // = sizeof(Provider)
        std::uint32_t abiVersion;   // = kABIVersion
        const char*   name;         // static string, diagnostics only
        void*         self;         // opaque; handed back as the first argument

        // HOT PATH -- called once per visible tile per frame, and again per
        // equipment-doll slot. Must be a hash lookup and nothing more.
        // Return false to say "nothing to draw"; *out is then untouched.
        bool (*GetOverlay)(void* self, const ItemKey* key, Overlay* out);

        // Fill up to `capacity` lines into the host-owned buffer, return how many
        // were written. Called only while the tooltip for `key` is being built.
        std::uint32_t (*GetTooltipLines)(void* self, const ItemKey* key,
                                         TooltipLine* out, std::uint32_t capacity);

        // Consulted before the host's own drop routing tables, for both whole-tile
        // and split-fragment drops. Returns a DropVerdict.
        std::uint32_t (*OfferDrop)(void* self, const DropQuery* query);
    };
    static_assert(sizeof(Provider) == 48, "Provider is part of the ABI");
}
