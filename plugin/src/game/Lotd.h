#pragma once

// Legacy of the Dragonborn — the museum index.
//
// LOTD pairs TWO FormLists per section, at matching indices:
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
    void Refresh();

    // O(1) lookup against the cache. This is what a tile calls.
    [[nodiscard]] Status Of(RE::FormID a_base);

    // ★Session boundary. Handles from the previous game resolve to nothing at
    // best and to another game's references at worst.
    void Clear();

    // For the log line only -- how much the index is holding.
    [[nodiscard]] std::size_t Size();
}
