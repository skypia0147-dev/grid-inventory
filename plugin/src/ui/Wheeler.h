#pragma once

#include <cstdint>

namespace SKSE
{
    class SerializationInterface;
}

// WHEELER — a quick menu for switching equipment presets and costumes without
// opening the inventory. Two fans of slanted bars, above and below a divider.
//
// ★★★IT OWNS NO DECISION OF ITS OWN. Which preset is active lives in Loadout,
// and which tab supplies the costume lives in Costume. This READS those to
// decide what to show and CALLS those to change them -- so picking here and
// clicking a tab in the inventory are the same act, and neither can drift from
// the other. It does keep a CURSOR (which row is highlighted), but that is
// cleared on every open and discarded on close, so it can never become a
// second answer to "which preset is on".
//
// ★★EVERY WHEEL OPENS AT CENTRE, POINTING AT NOTHING -- every group, not just
// the item ones. What is currently active still SHOWS (the tick draws it);
// preselecting it is a different thing and was never what that was for. Open
// on the active row and letting go without aiming applies something, so "none
// of these" stops being reachable from a full wheel. Bringing the aim back to
// the middle takes the choice back for the same reason.
//
//   PRESET    the loadout tabs           -> Loadout::RequestSwitch
//   COSTUME   the same tabs + one more   -> Costume::SetTab
//   GEAR      the starred items          -> Equip
//   MAGIC     the starred spells/shouts  -> EquipSpell / EquipShout
//
// ★★EQUIP IS "NO COSTUME" on the costume wheel. There was briefly a crossed
// circle for clearing, sitting beside an EQUIP tab the wheel drew and then
// refused -- two slots for one idea. A costume is read out of a tab's stored
// list and EQUIP has none, so it cannot be worn as one; choosing it can only
// mean "show my own gear", which is exactly what clearing does.
// ★It does not switch you to the EQUIP set -- that is the PRESET wheel's job.
// It takes the costume off whatever you are wearing.
//
// ★★★WHAT EXISTS IS THE GAME'S, WHERE IT SITS IS THE WHEEL'S -- for all four.
// The engine says which items are starred and Loadout says which tabs exist;
// the wheel keeps its own arrangement of slots over them, and the player drags
// it into whatever shape their hand wants. Nothing on screen is at a computed
// position: a slot HOLDS something, and the holding is stored.
//
// This is why the costume group's extra row has no fixed home, and why EQUIP
// need not sit at twelve o'clock. Both were once consequences of a slot's
// meaning being arithmetic on its number -- which is the same statement as
// "the wheel cannot be arranged".
//
// ---- input ------------------------------------------------------------------
// The hotkey is HELD. While it is down:
//
//   mouse    WHEEL steps the slot · A/D switches group · release applies
//   gamepad  left stick aims · D-pad left/right switches group · release
//
// ★★The fan axis RESETS ON REVERSAL and never accumulates. A running total
// means a long push upward has to be paid back before the fan can change --
// shove the mouse up, bring it down, and nothing happens until the debt is
// cleared. Zeroing the instant the direction flips is what makes "the other
// group, immediately" true, and it makes the axis jitter-proof for free.
//
// ★★★NOTHING ELSE REACHES THE GAME while it is up -- not movement, not sprint,
// not attacks, not the console. See Mute() below. It used to leave movement
// alone, on the reasoning that a quick menu should not interrupt a fight; what
// that actually bought was a wheel whose most reachable keys (A/D) were
// unusable, and a player steering blind behind a full-screen decision.
//
// The camera hooks (LookLock and friends in the cpp) stay: they close the same
// door from the other side, and a predicate that costs nothing is worth keeping
// against the day an input reaches the camera by a road this sink never sees.
//
// ---- layout (settled; see the design mockups) -------------------------------
//   bar 210x34 · pitch 42 · lean 12° · each step away from the divider indents
//   13 · the selected bar grows 1.34x ABOUT ITS OWN CENTRE and pushes its
//   neighbours clear -- it does not slide sideways, so the vertical edge the
//   eye tracks while stepping stays put
//   open   "slide", 260ms, from off-left, nearest the divider first
//   close  "keep the pick", 220ms -- the chosen bar stays while the rest go
//   seven rows per fan; longer lists window around the cursor and print the
//   count that fell off each end

namespace RE
{
    class ButtonEvent;
    class ThumbstickEvent;
    class MouseMoveEvent;
    class InputEvent;
}

namespace FUI::Wheeler
{
    // ---- lifetime --------------------------------------------------------
    // Registers the overlay menu. Call once at kDataLoaded, beside the other
    // menu registrations.
    void RegisterMenu();

    // ---- state -----------------------------------------------------------
    [[nodiscard]] bool IsOpen();

    // ---- input -----------------------------------------------------------
    // Fed from the existing InputSink. ★The hotkey is HELD: down opens, up
    // applies-and-closes. Returning true means the wheel consumed the event and
    // the game should not also act on it.
    bool OnButton(const RE::ButtonEvent* a_event);
    bool OnThumbstick(const RE::ThumbstickEvent* a_event);

    // ★Mouse DELTA, not cursor position. While the wheel is up the game still
    // owns the pointer -- it is hidden and pinned, so its screen position never
    // moves and reading it would leave the pick stuck wherever the cursor
    // happened to be. Accumulating deltas also makes every open start dead
    // centre, which is the only way "flick a direction" feels right.
    bool OnMouseMove(const RE::MouseMoveEvent* a_event);

    // ★★SILENCING IS NOT DONE HERE, and this note is the second half of the one
    // in main.cpp's input sink. A Mute() lived at this spot and blanked events
    // inside OUR sink -- but an event is one object shared by the whole chain,
    // so blanking it there also blanked it for every listener downstream, and
    // which side of us the game sits on is not ours to decide.
    //
    // It is done at PlayerControls' and MenuControls' entry points instead
    // (InputLock / MenuLock in the cpp), which blank, call the original, and
    // put the value back -- no blank without a restore. That discipline is what
    // keeps a key from sticking: the engine reads a button through predicates
    // built from value and held-duration, so a blank that is never restored
    // strands whatever was held when the wheel opened, and the character walks
    // forever. Blanking a copy the game has already been shown does not.

    // ---- frame -----------------------------------------------------------
    // Game-update hook: advances the open/close animation and owns the
    // slow-motion window. ★Runs every tick, including while closed, because the
    // close animation has to finish after the menu has already been asked to go.
    void Tick();

    // ImGui pass, called only from the overlay menu's PostDisplay.
    void Draw();

    // ---- hotkey ----------------------------------------------------------
    // ★The hotkey is a COMBINATION: every code in the set must be held for the
    // wheel to open, and letting go of any one of them closes it. One code is
    // the ordinary case and behaves exactly as a plain key always did.
    //
    // ★★A combination cannot swallow its own modifiers. The wheel opens on the
    // LAST code to go down, and only that code was ever taken from the game --
    // the others reached it as normal presses, so their releases must reach it
    // too. Eating the release of a Shift the game saw go down leaves the player
    // sprinting forever. The opening code's release is ours; every other
    // member's release is passed straight through.
    inline constexpr int kMaxCombo = 4;

    [[nodiscard]] int           ComboSize(bool a_pad);
    [[nodiscard]] std::uint32_t ComboAt(bool a_pad, int a_i);
    void SetCombo(bool a_pad, const std::uint32_t* a_codes, int a_n);

    // "LShift + Backslash" — for the settings row and for the ini's comment.
    // Points at a static buffer, good until the next call for that device.
    [[nodiscard]] const char* ComboText(bool a_pad);

    // ---- rebinding -------------------------------------------------------
    // ★IN-GAME KEY CAPTURE IS GONE, deliberately. It recorded a held set and
    // bound it when the last key came up -- correct machinery that nothing
    // could start: the settings row that armed it went away with the
    // Favorites-key move, and AdoptFavoritesKey overwrote whatever it produced.
    // Its own note said to delete it if that day did not come, and it did not.
    // Rebinding lives in GridInventory_ui.ini (`!wheelkey` / `!wheelkeypad`);
    // git history has the capture code if a combination is ever wanted.

    // ★★A tab was deleted, so every tab above it just changed number. The
    // wheel's own arrangement stores those numbers, and this is the ONE change
    // it cannot notice by rebuilding: deleting tab 2 leaves a tab 2 in place,
    // so "does it still exist" answers yes about a different set. Everything
    // else -- a purchase, a load, a tab that was never placed -- the rebuild on
    // open handles by itself.
    // Called by Loadout, beside the notice the costume system already gets.
    void OnTabRemoved(int a_index);

    // ---- on / off --------------------------------------------------------
    // ★★★TURNING IT OFF GIVES THE GAME ITS SCREEN BACK. The wheel does not sit
    // alongside the vanilla favourites menu, it REPLACES it -- it answers the
    // same key and suppresses the menu that key used to open. Some players
    // would rather have the original, so the honest control is one switch that
    // decides which of the two exists, not a pile of options that make the
    // wheel gradually more like the thing it replaced.
    //
    // ★While off: the hotkey is not claimed, the favourites menu is not
    // intercepted, and the wheel cannot be opened by any route. Everything the
    // wheel stores (the arrangement, the last group) is left alone, so turning
    // it back on returns to exactly where the player left it.
    //
    // ★It is NOT save state. Which UI you prefer is a property of the install,
    // like the skin and the language, so it lives in the ui ini beside them.
    [[nodiscard]] bool Enabled();

    // ★The wheel stands down entirely while the player is TRANSFORMED (an
    // unplayable race -- werewolf, vampire lord, whatever a mod adds): a beast
    // has no stars, no worn gear and no presets, and reverting runs through the
    // vanilla favourites menu. Both halves of the takeover have to ask this --
    // the hotkey (so the press passes through) AND the menu intercept in
    // main.cpp (so what the press opens is allowed to stay open). Gating only
    // the first leaves the key working and the menu closing on sight, which is
    // the same locked-in-beast-form the report described.
    [[nodiscard]] bool YieldingToVanilla();
    void SetEnabled(bool a_on);

    // ★★A star has just been taken off this form, so it gives up its place on
    // the wheel. Called by whatever removes the star, at the moment it does.
    //
    // ★Why not just notice at the next open: the wheel only re-reads the
    // favourites when it opens, so unstarring and re-starring inside one visit
    // to the inventory left no trace for it to see -- the item came back to
    // the slot it had been dragged to, while doing the same thing either side
    // of opening the wheel put it at the front. Same two actions, two answers,
    // and the difference was whether an unrelated menu happened to be shown in
    // between. The act has to report itself.
    void ForgetFavorite(RE::FormID a_form);

    // ★★★...AND THE MOMENT ONE GOES ON, which is the only moment the ORDER of
    // starring is knowable at all.
    //
    // The engine records that a thing is starred and never when. Asked later,
    // the inventory answers in the order of the forms' ADDRESSES -- so a wheel
    // filled from scratch came out shuffled however carefully the player had
    // starred things one at a time. Spells escaped it by accident:
    // MagicFavorites is an array the engine appends to, so it already carries
    // the history that items never had.
    //
    // Called as the star is made, this gives items the same history. Stars that
    // predate it -- or that were made in the vanilla favourites menu, which
    // does not pass through here -- are still seated in whatever order they are
    // first seen; there is nothing left to ask by then.
    void NoteStarred(RE::FormID a_form);

    // ---- assets ----------------------------------------------------------
    // ★★Drop the wheel's own drawn-icon cache. The medallions come out of the
    // same fallback folder the grid draws from -- "the folder IS the surface
    // you customise" -- but the wheel caches them separately, so the settings
    // panel's reload refreshed the grid and left the wheel showing the old
    // picture until the game was restarted. Worse, a failed load was cached
    // too, so a file ADDED later stayed invisible forever.
    // Called by whatever reloads the drawn icons, beside Fallback::ReloadAssets.
    void ReloadMedallions();

    // ---- settings --------------------------------------------------------
    // Slow-motion factor comes from the plugin ini alongside the rest.
    void LoadSettings();

    // ★Re-read the game's Favorites binding into the wheel's hotkey. The wheel
    // has taken that menu's place, so it answers to that key and follows the
    // player when they rebind it in the game's own controls.
    void AdoptFavoritesKey();
    // ★...unless the player has put the wheel somewhere of its own (!wheelkey
    // in the ui ini). 0 restores "follow the game". Set before the first
    // AdoptFavoritesKey and re-applied by every later one, so a rebind of the
    // game's Favourites key can no longer drag the wheel back on top of it.
    // Exists because Inventory rebound onto that key became unopenable: the
    // wheel blanks its hotkey before any menu can see it.
    // ★★A TAP HOLDS THE WHEEL OPEN. Under this many milliseconds a press is a
    // tap and the wheel stays up until the key is pressed again; over it the
    // press is the hold it always was and letting go applies. The two coexist,
    // so nobody has to choose and nobody's habit breaks.
    void          SetTapMs(int a_ms);
    [[nodiscard]] int TapMs();

    void          SetKeyOverride(bool a_pad, std::uint32_t a_code);
    [[nodiscard]] std::uint32_t KeyOverride(bool a_pad);

    // ---- persistence -----------------------------------------------------
    // ★The arrangement the player dragged every wheel into. Per save, like the
    // presets and the costume: which things are starred is save state, so where
    // those things sit is too.
    // ★Two kinds of name in one record, deliberately. GEAR and MAGIC are stored
    // as FormIDs and resolved on load -- a preference that survives the load
    // order changing under it, and drops silently what a removed plugin took
    // with it. PRESET and COSTUME are stored as tab indices, because a preset
    // exists nowhere but inside this save and has no other name.
    // v2 added the second half; v3 added the last-open group. Older records
    // still load -- each version reads as far as it goes and the rest is
    // rebuilt or defaulted on the next open.
    inline constexpr std::uint32_t kRecordType = 'GWHL';
    // 4: the player's own wheels. A slot is written as a LIST so that
    //    several things in one place can arrive later without a second
    //    migration -- see SaveGame.
    inline constexpr std::uint32_t kVersion = 4;

    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame(SKSE::SerializationInterface* a_intfc);
}
