#pragma once

#include <cstdint>

namespace FUI
{
    // ★★★WHICH UNIT. Not which FORM, and not which POSITION.
    //
    // A player carrying three Iron Daggers -- one tempered, two plain -- is not
    // carrying "three Iron Daggers": the engine keeps them in different
    // ExtraDataLists and they have different names, damage and prices. Every
    // question this plugin asks about an item ("equip THAT one", "sell THAT
    // one", "what is THAT one called") is a question about a unit, and the
    // answer has to travel with the request that asked it.
    //
    // ★It used to travel as four loose parameters, and four loose parameters
    // are four chances to drop one. Measured 2026-09-01, in one day: a tooltip
    // that passed a literal `0` where it held the signature; an equip that
    // resolved by list position and re-equipped the unit already on the body; a
    // transfer that handed the engine a null list and got a tempered bow back
    // as the plain copy. Three different symptoms, one sentence -- the caller
    // knew, and asked anyway.
    //
    // ★★AND THE POSITION IS NOT THE IDENTITY. `xlIdx` is a HINT: the engine
    // reorders extraLists behind us, and a PLAIN unit (uid 0, sig 0) is
    // listless by definition, so any index recorded for one is a leftover from
    // a moment it briefly had a list -- a worn unit carries one holding only
    // ExtraWorn, and the instant that goes away every later index slides down
    // and names somebody else. Resolve by pool where the pool is known; the
    // index is the last resort, never the first.
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
}
