#pragma once

// Legacy of the Dragonborn — the museum index.
//
// ★★★TWO SOURCES, and the second one is why an add-on's relics show up at
// all. Read (1) for what the museum itself knows and (2) for what the rest of
// the load order knows; Of() consults them in that order.
//
//
// (1) LOTD ITSELF. It pairs TWO FormLists per section, at matching indices:
//
//   DBM_Section<X>        the display slots -- one ObjectReference per spot
//                         in the museum
//   DBM_Section<X>Items   the item that belongs in each slot, same index
//
// A slot the player has filled is ENABLED; an empty spot is disabled. That is
// how the museum shows a donation -- the model appears on the pedestal.
//
// ★MEASURED, not assumed. Probed against a live game: with nothing donated the
// index read `0 DISPLAYED`; after donating exactly one dragon priest mask it
// read `1 DISPLAYED`, and the increment landed in the mask section. That is the
// whole basis for Status below.
//
// Also measured, from the plugin itself: 47 sections, 2074 entries, and every
// pair agrees on size. Two shapes have to be handled and are counted here so
// nobody rediscovers them -- 77 nested FormLists (one slot accepting several
// variants) and 19 slots that are not references at all.
//
//
// (2) ★THE CURATOR'S COMPANION (DBM_RelicNotifications.esp), when installed.
//
// Source (1) sees only what ships in LegacyoftheDragonborn.esm. A museum
// add-on -- JaySuS Swords was the one reported -- puts its relics somewhere
// else entirely, so the inventory called them ordinary items while the game's
// own museum UI called them displayable. Reported 2026-09-02.
//
// The Curator's Companion keeps FOUR FormLists that answer exactly the
// question this file exists to answer, and it is already the thing moreHUD
// reads to draw its relic tag:
//
//   dbmMaster  0x609634   every relic the museum knows
//   dbmNew     0x558285   not seen yet
//   dbmFound   0x558286   seen, not donated
//   dbmDisp    0x558287   donated
//
// ★MEASURED from the plugin: all four are EMPTY on disk. Papyrus fills them
// at runtime, which is why they are re-read on Refresh() and not just once in
// Rebuild(). Its own MCM shows their sizes as the moreHUD counts and holds
// them to New + Found + Disp == Master, and its support table names 127 mods.
//
// ★WHY (1) STILL WINS where both answer: a section slot is an
// ObjectReference whose enabled state IS the donation, read straight from the
// game. dbmDisp is a Papyrus-maintained copy of that fact and can lag it. The
// add-on lists are the reason to consult (2) at all -- not a reason to stop
// trusting (1).
namespace FUI::Lotd
{
    enum class Status : std::uint8_t
    {
        kNotRelic  = 0,   // nothing to do with the museum
        kUndonated = 1,   // a relic the player has not handed in yet
        kDonated   = 2,   // already on a pedestal
    };

    // Once per load. Walks the 47 sections and builds FormID -> slot handle,
    // resolving nested lists and dropping slots that are not references.
    void Rebuild();

    // Once per inventory open. Re-reads only the slots' enabled state.
    //
    // ★Why this is separate from Rebuild: the map is fixed for the session but
    // donations happen DURING play, and asking IsDisabled() per tile would mean
    // resolving a handle on every frame of every draw. Donating requires
    // closing the inventory, so a sweep taken as the menu opens cannot go stale
    // while the menu is up.
    //
    // ★This is also where source (2) is picked up, for a different reason: its
    // lists start empty and Papyrus fills them somewhere after load, so a read
    // taken once in Rebuild() could hold nothing at all. Rebuilt only when a
    // list's SIZE has moved, so the usual open costs two size reads.
    void Refresh();

    // O(1) lookup against the cache. This is what a tile calls.
    [[nodiscard]] Status Of(RE::FormID a_base);

    // ★Session boundary. Handles from the previous game resolve to nothing at
    // best and to another game's references at worst.
    void Clear();

    // For the log line only -- how much the index is holding.
    [[nodiscard]] std::size_t Size();

    // Likewise, and kept apart from Size() on purpose: these two count
    // different things (pedestals we resolved vs forms another mod listed),
    // and adding them would report a total that is true of neither. 0 when the
    // Curator's Companion is not installed.
    [[nodiscard]] std::size_t CuratorSize();
}
