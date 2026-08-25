#pragma once

#include <functional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImFont;
struct ImVec2;
struct ImVec4;

// Plan B core: owns the ImGui context (init from the game's D3D11 device),
// runs the per-frame ImGui pass from GridInventoryMenu::PostDisplay, and
// opens/closes the menu via the UI message queue.
namespace FUI::UIRoot
{
    // ---- the bottom prompt bar --------------------------------------------
    // ★Public so surfaces OUTSIDE the inventory can use the real bar instead of
    // drawing a lookalike. The quick menu is the first of those, and a
    // hand-rolled row there would be a second thing to keep in step with the
    // theme, the scale, the keycap chrome and the vignette that lets any of it
    // read over a snowfield.
    struct PromptBit
    {
        std::string key;     // "" = plain text (drag / wheel / a warning)
        std::string label;
        bool        sep = false;   // draw a divider before this bit
    };
    void DrawPromptRow(const std::vector<PromptBit>& a_bits, bool a_warn, float a_fade);

    // ★★The section label over a board -- "ITEMS" on the player's side,
    // "CONTENTS" / "WARES" on the partner's. ONE place because the two windows
    // have to agree: the skin chooses between a crimson diamond and outlined
    // chrome, and the partner window only ever drew the diamond. On the ink
    // skins its label was then the one piece of the old style left on screen,
    // which is exactly the kind of split a shared window frame is meant to
    // prevent.
    // ★a_col overrides only the COLOUR -- the style (crimson diamond vs
    // outlined chrome) still comes from the skin. A warning label needs its own
    // colour, but it does not get to wear a different typeface from the label
    // beside it.
    void SectionLabel(const char* a_text, const ImVec4* a_col = nullptr);

    // One line of help for the BOTTOM prompt bar, good for this frame only.
    // Call it while a control is hovered; the bar shows it in place of its
    // ambient hints. Keeps help out from under the cursor.
    void NoteHoverHint(const char* a_text);

    // A real bold face for the few places that want weight (the quick menu's
    // hub). ★It takes the string because the bold face is baked with Latin and
    // hangul only -- a japanese or cyrillic name has no glyph there and would
    // draw tofu, so anything it cannot spell comes back as the main font
    // instead: not bold, but readable, which is the right way round.
    ImFont* BoldFont(const char* a_utf8);

    void RegisterMenu();  // RE::UI::Register (call at kDataLoaded)
    bool TryInitD3D();    // idempotent ImGui init from renderer data

    void Open();   // queue kShow for GridInventoryMenu
    void Close();  // queue kHide

    // ★DIAG, wired but not called (same policy as g_poolTrace). SkyUI-style
    // widgets -- SunHelm's among them -- show only while the TOP of the HUD's
    // mode stack is in their allowed list, so a mode pushed and never popped
    // hides them PERMANENTLY. Call this around a suspect open/close to dump
    // the stack ("[HUDMODE] tag: [...]") and the culprit names itself.
    // (Reported once against 1.2.x, never reproduced here.) UI thread only.
    void LogHudModes(const char* a_tag);

    // I/ESC close staging: an open settings popup or EDIT mode closes FIRST
    // (returns true); only when nothing was closed does the caller close the
    // whole inventory.
    bool CloseTopWindow();

    // overlay hover blocking: popups/settings/EDIT draw chrome WIDER than
    // their ImGui window rect, so the grid underneath still hover-reacts in
    // the chrome margin. Overlay windows register their (margin-padded) rect
    // each frame; the grids skip hover/clicks while the mouse is inside one.
    void NoteOverlayRect();   // call INSIDE the overlay window's Begin scope
    bool MouseInOverlay();    // previous frame's rects (draw-order safe)

    // ★★★"Does the cursor belong to the window being drawn right now?" -- the
    // two-term question every board has to answer before it reacts, and it must
    // be ONE term because the two halves protect against different things and
    // both are easy to forget:
    //   IsWindowHovered  z-order. Geometry alone cannot see the window in
    //                    front, which is how a drop landed in the bag hidden
    //                    behind a merchant.
    //   MouseInOverlay   the chrome a popup / settings / EDIT draws OUTSIDE its
    //                    ImGui rect. z-order does not cover that band, so a
    //                    pickup was refused there while a drop went through.
    // ★AllowWhenBlockedByActiveItem is not optional: the click that resolves a
    // drop activates whatever item is under the cursor, and a plain hover test
    // then reports "no window" on the one frame the answer is needed.
    // It was written out by hand in three places and the partner window's copy
    // had only the first half.
    // a_extra: ImGuiHoveredFlags. RootAndChildWindows for a window whose
    // content lives in a scrolling child (the partner's goods do) -- the claim
    // is the window's either way, but ImGui hands the hover to the child.
    // ★Typed as int: this header is included by files that do not pull in
    // imgui.h, and ImGuiHoveredFlags IS an int there.
    [[nodiscard]] bool CursorOwnsWindow(int a_extra = 0);

    // INSPECT overlay — C key, matching vanilla's "Item Zoom" binding
    // (controlmap Item Zoom = 0x2E). Rotatable full-size view of the engine's
    // own model render: the ONLY way to read detail that lives on the model
    // (dragon-claw glyphs drive the Bleak Falls Barrow door puzzle).
    void OpenInspect(RE::TESBoundObject* a_obj, const std::string& a_key);
    [[nodiscard]] bool IsInspectOpen();
    bool CloseInspect();      // false when it wasn't open (ESC chain)

    void OnShow();   // menu received kShow
    void OnClose();  // menu received kHide
    void Render();   // full ImGui frame; called from PostDisplay only
    void Tick();     // game-update hook (pre-render): def apply + model parking

    void AddScrollEvent(float a_x, float a_y);

    // true while an ImGui text field owns the keyboard (preset name etc.) —
    // the raw-input I-key close must not fire while the user is typing
    [[nodiscard]] bool IsTextInputActive();

    // ---- gamepad -----------------------------------------------------------
    // The engine hides its Cursor Menu (and stops advancing MenuCursor) when a
    // controller is driving, which left this UI with no pointer at all. These
    // feed a cursor we own and draw ourselves; the input sink in main.cpp calls
    // them for pad events only while our menu is open. A real mouse movement
    // hands control straight back.
    //   a_right : right stick (scroll) vs left stick (pointer)
    //   a_idCode: RE::BSWin32GamepadDevice::Keys value
    void NotePadStick(bool a_right, float a_x, float a_y);
    void NotePadButton(std::uint32_t a_idCode, bool a_pressed);
    // A genuine mouse event hands the pointer back. Must come from a real
    // device event: the OS cursor's POSITION is useless as a signal here,
    // because the game parks and re-warps it while a pad is driving.
    void NoteMouseInput();
    // Should GridMenu keep asking for the vanilla Cursor Menu? False only when
    // we have taken over the pointer ourselves (see PadCursorMode).
    [[nodiscard]] bool WantsGameCursor();

    // ---- control hints -----------------------------------------------------
    // What to PRINT for an action in the tooltip's hint lines. While a pad is
    // driving this is the button actually bound to it ("A" / "X" / "LT"), which
    // is only knowable by asking the engine — the player may have rebound it,
    // and a non-Xbox pad reports its own layout. Otherwise it is the keyboard /
    // mouse key. The pad side is resolved once per menu open and cached: the
    // lookup does string work, and this is called every frame a tooltip is up.
    enum class Act : std::uint8_t
    {
        kPrimary,     // pick up / place
        kSecondary,   // equip / read / bag ...
        kDrop,
        kFavorite,
        kInspect,
        kSplit,       // split a stack / hold to compare
        // GI63: rotation is OURS, not a rebindable game action, so these have no
        // ControlMap entry and always resolve to their keyboard labels. Routing
        // them through KeyLabel anyway keeps every prompt on one code path -- and
        // the day a pad binding exists, only the table below changes.
        kRotateCCW,
        kRotateCW,
        // ★Ours as well, and with no pad binding at all -- KeyLabel answers
        // for it without consulting the controller table (it sits past the
        // end of that array on purpose).
        kRecharge,
    };
    [[nodiscard]] const char* KeyLabel(Act a_act);

    // ★Hand the engine's OWN Cursor Menu the real thumbstick event so it moves
    // its own arrow — `CursorMenu` is a MenuEventHandler with a ProcessThumbstick
    // of its own (that is how the world map's pad cursor works). Our menu is not
    // in MenuControls' handler list, so the event never reaches it by itself.
    // Poking MenuCursor::cursorPos* instead does NOT work: the arrow only
    // re-reads that pair when an input event arrives, so it just teleported on
    // the next button press. We pass the live event straight through — no
    // synthesised event, no vtable games.
    void FeedEngineCursor(RE::ThumbstickEvent* a_event);

    // ★True while the game's own Book Menu is up on top of us (right-click on
    // a book / note). Our overlay renders LAST, so it would paint straight
    // over the book; we stand down entirely instead — no draw, and every
    // input channel passes through, or the book could not even be closed.
    [[nodiscard]] bool IsBookOpen();

    // ★★True while the game's console is up. Unlike the book it does NOT hide
    // us — it draws over the bottom of the screen and takes the keyboard —
    // so we keep rendering and only stand down from KEY input. The engine
    // delivers the same keystrokes to both menus, which is how typing a
    // console command also filled the item search box.
    [[nodiscard]] bool IsConsoleOpen();

    // ★★THE DIAGNOSTIC KEY THAT SHIPS UNASSIGNED.
    //
    // DirectInput scancode of the key that hands the next inventory, container
    // or shop to the engine and the one after that back to us. 0 -- the
    // shipped value -- matches no key, so the mechanism is whole but inert.
    // Set from GridInventory_ui.ini beside the other test switches:
    //
    //     !vanillakey = 87        (87 = 0x57 = F11)
    //
    // Kept here rather than in main.cpp so the ini parser and the input sink
    // can both reach it without either owning the other.
    void               SetVanillaKey(int a_scancode);
    [[nodiscard]] int  VanillaKey();

    // ★★Draw the next images as SILHOUETTES: the vertex tint supplies the
    // colour, the texture supplies only its ALPHA. Needed because an ImGui tint
    // multiplies — black collapses a sprite to its shape, white leaves the
    // sprite untouched — so a LIGHT silhouette is not expressible as a tint at
    // all. Pair the two calls on the same draw list; Begin returns false when
    // the shader is unavailable, and the caller must then not change its tint.
    [[nodiscard]] bool BeginSilhouette(ImDrawList* a_dl);
    void EndSilhouette(ImDrawList* a_dl);

    // ★★Binds the mip-enabled sampler for the rest of the draw list. The DX11
    // backend's own sampler is MaxLOD = 0, so a texture's mip chain is ignored
    // and a 256px sprite shown at ~48px aliases -- visible as a staircase along
    // any fine curve. The inventory gets this at the head of its background
    // list once a frame; the WHEEL runs its own ImGui frame (its PostDisplay
    // calls NewFrame/Render itself) and so was never covered by that one.
    // Call it first in any independent frame.
    void UseMipSampler(ImDrawList* a_dl);

    // ★Point ImGui at the RENDER TARGET rather than at the window. Call from
    // every frame we build, between the backend's NewFrame and ImGui's -- see
    // the note in UIRoot.cpp for what the window's own answer costs.
    void SyncDisplaySize();

    // main.cpp installs these to keep legacy state (attack-input block) in sync
    void SetVisibilityCallbacks(std::function<void()> a_onShow, std::function<void()> a_onHide);

    // item icon draw with the ICON LIGHT setting applied live: <=1 darkens
    // via tint, >1 brightens via an extra additive pass (Theme::IconGain).
    // Every item-sprite AddImage must go through this for a consistent look.
    void DrawItemIcon(ImDrawList* a_dl, void* a_srv, const ImVec2& a_min, const ImVec2& a_max);
    // GI52: same passes, drawn around a centre at an angle. Drawn (category)
    // icons carry their own rotation, which a 3D capture never needed — the
    // capture bakes its orientation into the pixels.
    void DrawItemIconRot(ImDrawList* a_dl, void* a_srv, const ImVec2& a_centre,
                         const ImVec2& a_size, float a_deg);
}
