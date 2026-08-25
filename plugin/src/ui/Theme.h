#pragma once

#include <imgui.h>

namespace FUI::Theme
{
    // B-7b skin system (mockup v9): a skin swaps colour tokens AND structural
    // grammar (rounding, title treatment, frame style, translucency). Layout
    // metrics never change between skins.
    struct Skin
    {
        const char* name;
        // colour tokens (from the v9 mockup CSS)
        ImVec4 acc;        // accent (lines, titles) — alpha applied per use
        ImVec4 hi;         // bright accent (values, badges)
        ImVec4 ink;        // body text
        ImVec4 inkDim;     // secondary text
        ImVec4 winBg;      // window background (kept fully opaque: parking)
        ImVec4 glyph;      // item/silhouette tint
        // ★Hand-tuned per skin, and it stays that way — see OccupiedGround().
        ImVec4 shade;      // occupied-cell overlay
        ImVec4 sel;        // selection / highlight
        ImVec4 filled;     // equipped-slot border accent
        // structural grammar
        // (the VELLUM/NORDIC grammar block — titleCentered / cornerBrackets /
        // slotBevel / paperTexture / serifTitle / darkText — was removed with
        // those skins; no surviving skin ever set any of them)
        float rounding;        // window/frame rounding
        float titleSpacing;    // extra px between title glyphs (at scale 1)
        // OATHVEIN UNTARNISHED (v10.4) grammar — default false for other skins
        bool  cornerFade = false;      // borders drawn as corner-fade lines (no full border)
        bool  topStrip = false;        // 2px crimson strip across the window top
        bool  titleGlow = false;       // glowing title (the underline was removed in 1.0.5)
        bool  diamondLabels = false;   // section labels: "◇ LABEL" in sel colour
        // ★The frame is DRAWN now, not sampled — see Theme::TornPanel. Its two
        // texture-era companions (tornGlowB, tornTex) were removed with the
        // four .fic sheets they used to select between: nothing picks a torn
        // variant any more because there is only one, and its silhouette comes
        // from noise rather than from a bitmap.
        bool  tornFrame = false;       // procedural torn-paper panel (window bg)
        // translucent windows (skins 5/6) are safe because the capture path
        // saves/restores the FULL backbuffer frame (ItemPreview Render step
        // 1/5), so the parked model never survives onto the visible frame
        bool  translucent = false;   // respect winBg alpha (others force 1.0)
        bool  bevelChrome = false;   // gradient titlebar + dark/light bevel border

        // ★The panel is LIGHT, and that inverts a rule this UI was built on.
        // Every "emphasis" here is a BRIGHTER colour (hi) over a dark ground —
        // gauge fills, slider values, stat numbers. Over a light panel there is
        // no headroom above it, so those all fade into the panel instead of
        // standing out: the sliders read as empty tracks and the numbers as
        // disabled text. Emphasis has to go the other way, toward acc.
        // `translucent` cannot carry this — skins 5/6 are translucent AND dark.
        bool  lightPanel = false;

        // ── SIMPLE grammar (skin 7) ────────────────────────────────────────
        // ★Cells are separated by a carved GROOVE, not a hairline: a dark gap
        // with a 1px light line down its middle, plus a 1px dark shadow inside
        // each cell's top-left. The light centre line is what makes it read as
        // carved — a single flat line cannot, at any colour.
        bool   engravedCells = false;
        // Every token below defaults to ALPHA 0 = "this skin does not use it",
        // and each draw site falls back to exactly what it did before. That is
        // why six existing skins need no edits at all.
        ImVec4 cellBg{ 0.0f, 0.0f, 0.0f, 0.0f };        // empty-cell fill
        ImVec4 cellGroove{ 0.0f, 0.0f, 0.0f, 0.0f };    // groove, dark
        // ★A `cellGrooveLt` (the groove's light centre line) sat here until
        // 1.0.5. The carved look above is drawn WITHOUT it — kGrooveW says the
        // panel simply shows through — so seventeen skins were each carrying a
        // colour nothing sampled. Removing it shifted every following token up
        // one slot, which is exactly the hazard the note below describes.
        ImVec4 btnFace{ 0.0f, 0.0f, 0.0f, 0.0f };       // button face
        // 규칙 96: the open bag's tile. Was a file-local constant in Grid.cpp;
        // the default IS that constant, so nothing moves for the other skins.
        ImVec4 bagOpen{ 122.0f / 255.0f, 154.0f / 255.0f, 122.0f / 255.0f, 64.0f / 255.0f };
        // The money figure. Every other number in this UI is "a value"; this
        // one is money, and the reference gives it its own colour rather than
        // the shared emphasis. Alpha 0 = follow Val() like everything else.
        ImVec4 goldNum{ 0.0f, 0.0f, 0.0f, 0.0f };
        // Window title size. 15 was picked against the atlas's 17px body
        // text; a skin may want the two closer together.
        // ★Defaulted fields go at the END. Every skin above is a POSITIONAL
        // initialiser, so a new member in the middle silently feeds the next
        // value into it and shifts all six of them by one.
        // ★24 for every skin. The bar is 34px tall and the title is centred in
        // it (WinManager), so one size gives them all the same 5px above and
        // below — the older 15 left the dark skins with a caption-sized title
        // floating in a bar built for something bigger.
        float  titleSize = 24.0f;

        // ── light-panel palette ────────────────────────────────────────────
        // ★Read only when lightPanel is set, and ALPHA 0 means "use the
        // built-in value" — so SIMPLE names none of these and is unchanged.
        // They exist because the light-panel machinery was written for ONE
        // skin and had its blues compiled in; a second light skin (the
        // parchment one) shares every rule and none of the colours.
        ImVec4 lpBtn{};        // idle button face
        ImVec4 lpBtnHov{};
        ImVec4 lpBtnAct{};
        ImVec4 lpBorder{};     // button + frame border
        ImVec4 lpBtnOnFace{};  // a toggled-ON button
        ImVec4 lpBtnOnInk{};   // ink on that face
        ImVec4 lpRule{};       // dividers

        // ★★INK SKINS: the panel is a photographed SHEET, so the skin names a
        // file instead of carrying another colour. Grain and a vignette are
        // pixels; no token can say them.
        //
        // ★This is the only thing the two ink skins do not share. Every mark
        // that goes on the paper -- the frame, the rules, the washes -- is a
        // white+alpha sprite tinted from the tokens above, so one sprite set
        // serves both, and would serve a dark ink skin too. Only the paper had
        // to be baked twice, because the tint IS the paper.
        //
        // Relative to Data/SKSE/Plugins/GridInventory_ink/. Null on every
        // other skin, which is what keeps them out of this draw path entirely.
        const char* paper = nullptr;
    };

    // Gap between cells (px at scale 1). ★It is no longer a carved groove —
    // nothing is painted there, the panel simply shows through — so the width
    // is now about how far apart the tiles sit, not how deep the cut looks.
    // Each of the two neighbours gives up half of it.
    inline constexpr float kGrooveW = 2.0f;

    // 9-slice source margins for the torn-paper frame texture, as a fraction
    // of the image. Corners fixed, edges stretch one way, centre both.
    //
    // ★★These MUST match kTornDest below in real pixels, or the corner and
    // edge slices are squeezed on their way to the screen. The previous pair
    // (0.1663 / 0.2901 = 106 x 222 px of a 640x766 image, drawn into 30 px)
    // crushed them 3.55x across and 7.41x down — and because the two factors
    // differ by 2.09x, the four corners were sheared as well as flattened.
    // That is what made the torn edge read as a smudge rather than as paper.
    //
    // ★The old numbers came from the 99.5th percentile of the alpha fringe,
    // i.e. the single longest whisker of torn paper. Measured properly, the
    // tearing is over by 27-38 px on every side; everything past that was
    // FLAT SHEET being folded into the corner slice. 48 px covers the ragged
    // band with room to spare and leaves the flat middle to stretch, which is
    // the one part that can stretch without showing.
    inline constexpr float kTornPx = 48.0f;    // the band, in source pixels
    inline constexpr float kTornUM = kTornPx / 640.0f;   // = 0.0750
    inline constexpr float kTornVM = kTornPx / 766.0f;   // = 0.0627
    // ★★Head-room OUTSIDE the window rect for the drawn tearing to live in.
    // The sheet itself is the window rect exactly — the teeth stick out past
    // it — so this only has to clear the deepest bite (TornDepth peaks at
    // ~9.2px) plus a margin.
    // ★It is NOT the sheet's size. Making the clip and the sheet the same
    // rectangle is what produced a perfectly straight edge on the first try:
    // every tooth was drawn and then clipped away by the very box it was
    // supposed to escape.
    inline constexpr float kTornOut = 14.0f;

    // ★★THE UI SCALE, and since 1.2.1 it follows the SCREEN. It was fixed at
    // 1.0 after the old 0.5~1.6 slider was removed (that slider moved type and
    // chrome while CellScale moved the board — two scales for one question).
    // Fixed meant type was a fixed number of PIXELS, so on a 4K screen every
    // label was half the size it is on 1440p and the SCALE slider could not
    // help: that slider moves the board, not the type.
    //
    // ★Derived from the display HEIGHT, not the width ★ an ultrawide is not a
    // bigger screen, it is a wider one: 3440x1440 gets the same 1.00 as
    // 2560x1440 and nothing about it changes. Width would have inflated it by
    // a third and pushed the layout off the bottom.
    //
    // ★★1440 is the BASELINE because that is the screen every one of these
    // sizes was chosen by eye against. Change it and you have not "fixed a
    // constant", you have resized the whole UI for everybody.
    inline constexpr float kScaleBaseH = 1440.0f;
    inline constexpr float kScaleMin   = 0.75f;   // 1080p
    inline constexpr float kScaleMax   = 2.00f;   // beyond 4K
    [[nodiscard]] float Scale();
    // Resolve Scale() from a real backbuffer height. Called ONCE, from
    // TryInitD3D, before the font atlas is baked — the bake reads Scale().
    void ResolveScale(float a_displayH);

    // ★★THE scale — what the SCALE slider moves, persisted as "!scale".
    // Measured on the main window: the item grid and the equipment doll are
    // both built out of cell-sized blocks (10x14 cells; every slot is 2x2 of
    // them, weapons 2x4), and together they are 97% of the window's width.
    // Padding is 4%. So "make the window smaller" has exactly one lever and
    // this is it — which is why it inherited the name.
    //
    // ★★TWO NUMBERS, deliberately. The board's shipped size was 0.90 of the
    // cell constants, and a slider that reads 0.90 at the default invites the
    // question "0.90 of WHAT" — the answer being an internal constant nobody
    // outside this file can see. So the SETTING is 1.00 at the shipped size
    // and the 0.90 moved here, where it is the definition of 1.00 rather than
    // a number the player has to interpret.
    //   ScaleSetting()  what the slider shows and the ini stores — 1.00 default
    //   CellScale()     what the board is drawn at — ScaleSetting() * kScaleBase
    // Render code wants the second; settings and persistence want the first.
    inline constexpr float kScaleBase = 0.90f;   // the board size "1.00" means
    [[nodiscard]] float CellScale();
    [[nodiscard]] float ScaleSetting();
    void SetScaleSetting(float a_scale);

    // ── text size, chosen by the player ────────────────────────────────────
    // ★★★WHY THIS EXISTS SEPARATELY FROM Scale(). Scale() is decided by the
    // display height and nothing else -- there has never been a way for a
    // player to say "this text is too small for me". On a high-resolution
    // screen that is a real complaint even after the automatic 1.5x, because
    // how big 17px reads depends on the panel and how far away it is, not on
    // its pixel count. Reported by more than one 4K user.
    //
    // It multiplies Scale() rather than replacing it, so the automatic sizing
    // still does its job and this rides on top: 1.00 keeps exactly what
    // shipped. It reaches every string in two places -- style.FontScaleMain
    // for the ones ImGui sizes (tooltips, running text, stack counts) and
    // SnapPx for the ones we size ourselves (values, captions) -- and needs no
    // atlas rebuild at all: ImGui 1.92 rasterises each size on demand.
    //
    // ★Layout is NOT scaled by this. Rows grow because the text in them is
    // taller, and windows measure their own content now, so they follow. The
    // board's cell size has its own slider and stays where the player put it.
    //
    // ★★The value is NOT applied while the slider is held -- see RowFontScale.
    // The settings panel is built out of the text this setting sizes, so
    // applying it live re-laid that panel out and slid the slider out from
    // under the cursor holding it. Deferring to the release is not a nicety
    // here; it is the difference between a control and a moving target.
    // ★kFontScaleStep is the ARROW step only -- the track stays continuous, so
    // the size can land on any whole pixel. A hundredth (what the range would
    // otherwise derive) does not move the rendered size at all, which is why
    // the arrows read as dead before.
    inline constexpr float kMinFontScale  = 0.85f;
    inline constexpr float kMaxFontScale  = 1.60f;
    inline constexpr float kFontScaleStep = 0.05f;
    [[nodiscard]] float FontScale();
    // Clamped to the range above. Returns true when the value actually moved --
    // nothing has to be rebuilt when it does, so this is only worth asking to
    // decide whether the change is worth PERSISTING.
    bool SetFontScale(float a_scale);

    // ── GI59: glow + icon light are stored PER ICON STYLE ──────────────────
    // Slot 0 = realistic (3D captures), 1 = drawn (flat art). They need
    // different light, so each keeps its own numbers.
    //
    // ★The no-argument forms act on the ACTIVE style. PERSISTENCE MUST NOT USE
    // THEM: the ini is read line by line and "!glowstyle" comes before
    // "!iconstyle", so at load time the active style is not known yet and the
    // value would land in the wrong slot. Save/load through the *Of / *At
    // forms, which name the slot outright.
    [[nodiscard]] int IconStyleSlot();   // 0 realistic / 1 drawn (active)

    // rarity glow style: 1 = silhouette halo (blurred icon alpha) — default,
    // 0 = stretched radial across the footprint (previous look, kept for revert)
    // ★★1.0.5: every setting below is stored per SKIN as well. The no-argument
    // forms act on the live skin and live icon style — UI call sites use those
    // and none of their signatures changed. The *Of / *At forms take a 1-BASED
    // skin index first and are what persistence walks.
    // ★★1.0.5 ITEM SHADOW — three plain numbers, because that is what a drop
    // shadow is everywhere else: how far it falls, how soft it is, how dark.
    //   axis 0  DISTANCE px  — offset toward the lower right; 0 = ambient
    //   axis 1  BLUR px      — how far the edge spreads; 0 = a hard silhouette
    //   axis 2  OPACITY 0..1 — the black's alpha
    // Stored per skin AND per icon style, like every other DISPLAY row.
    //
    // ★These replaced a SHADOW + SHADOW STYLE pair in which "Soft" and "Sharp"
    // were an implementation detail wearing a setting's clothes — one path
    // sampled a baked 96px silhouette, the other stamped the sprite. The blur
    // is drawn at full sprite resolution now, so the shape follows the number
    // instead of the number picking between two shapes.
    [[nodiscard]] float ShadowDist();
    [[nodiscard]] float ShadowBlur();
    [[nodiscard]] float ShadowOpacity();
    [[nodiscard]] float ShadowAxis(int a_axis);         // the same three, by index
    void SetShadowAxis(int a_axis, float a_v);          // acts on the live pair
    [[nodiscard]] float ShadowAt(int a_skin, int a_slot, int a_axis);
    void SetShadowAt(int a_skin, int a_slot, int a_axis, float a_v);

    // ---- legacy, kept for the ini format only -------------------------------
    // ★The rarity halo these drove is gone (rarity is a corner wedge) and so is
    // the shadow style that briefly borrowed the slot. Nothing READS them any
    // more; they stay so the "!disp<skin>" line keeps its 13 fields and an ini
    // written by any 1.0.x build still parses. Do not add call sites.
    // ★Only the *Of pair survives, because only it earns the keep: persistence
    // names the skin and slot explicitly. The active-skin conveniences
    // (GlowStyle/SetGlowStyle) served the settings UI that is gone, so they
    // were dead in the ini sense too — removed in 1.0.5.
    [[nodiscard]] int  GlowStyleOf(int a_skin, int a_slot);
    void SetGlowStyleOf(int a_skin, int a_slot, int a_style);

    // rarity glow brightness (0.2~2.5). Kept per GLOW style as well, so the
    // full key is [icon style][glow style]. GlowGain()/SetGlowGain() act on the
    // active pair; *At names both and is what persistence uses. Scales
    // per-rarity tint alphas.
    // (The *Of middle pair — glow style within the active icon style — went
    // with the settings UI that called it; see the legacy note above.)
    [[nodiscard]] float GlowGain();
    void SetGlowGain(float a_gain);
    [[nodiscard]] float GlowGainAt(int a_skin, int a_slot, int a_style);
    void SetGlowGainAt(int a_skin, int a_slot, int a_style, float a_gain);

    // item icon brightness (0.4~1.6). Applied LIVE at draw time by
    // UIRoot::DrawItemIcon: <=1 darkens via tint, >1 brightens via an extra
    // additive-blend pass (a tint alone cannot go up).
    [[nodiscard]] float IconGain();
    void SetIconGain(float a_gain);
    [[nodiscard]] float IconGainOf(int a_skin, int a_slot);
    void SetIconGainOf(int a_skin, int a_slot, float a_gain);

    // ★★1.0.5 CAPTURE LIGHT — where the menu scene's single lamp sits while
    // an icon is being photographed, as an OFFSET in degrees from the shipped
    // rig. NOT per skin: the capture is a photograph of the model, and the
    // skin it is later drawn under does not change it. Storing it per skin
    // would invalidate every icon on a skin switch, which is the one thing a
    // skin switch must stay cheap enough to do freely.
    //
    // The SAME units and origin as an item def's own lightAz/lightEl, so the
    // final angle is base + this + the item's. Move this and every untuned
    // item follows; the handful that were tuned keep their relative offset.
    [[nodiscard]] float CaptureLightAz();
    [[nodiscard]] float CaptureLightEl();
    void SetCaptureLight(float a_azDeg, float a_elDeg);

    // ★The ICON STYLE is the skin's too, and Theme owns the number: it hands
    // the choice to IconCache whenever it changes or a skin is entered. Call
    // SetIconStyle instead of IconCache::SetStyle, or the pick is not saved.
    // Raw values are IconCache::Style's (0 realistic / 2 drawn / 3 pixel).
    [[nodiscard]] int  IconStyleOf(int a_skin);
    void SetIconStyleOf(int a_skin, int a_style);
    void SetIconStyle(int a_style);   // the live skin

    // active skin (1-based), persisted BY NAME as "!skin3"
    [[nodiscard]] int  SkinIndex();
    void SetSkin(int a_index);            // re-applies the ImGui style

    // ★★A saved POSITION is only readable by the build that wrote it. Twice now
    // the array has been reordered — "Fable Amber" removed, then the ink pair
    // moved to the front and the Glass pair dropped — and each time every
    // saved number pointed at a different skin. That is not a visible break: a
    // player sees their own skin and their own icon gain, just belonging to
    // somebody else. So a name is what gets written, and a name is what these
    // take. Position survives nothing; a name survives everything except a
    // rename, which is a thing we can choose not to do.
    [[nodiscard]] int  SkinIndexByName(const char* a_name);   // 0 = no such skin
    [[nodiscard]] const char* SkinNameAt(int a_index);        // "" out of range
    bool SetSkinByName(const char* a_name);                   // false = unknown

    // The two legacy numberings, oldest first. Both convert a position written
    // by an older build into a NAME in this one, which is why the removed
    // skins have to name a substitute (see kSkins' header).
    //   "!skin"  — before Fable Amber was removed
    //   "!skin2" — after that, before the Glass pair went and Sumi moved up
    void SetSkinLegacy(int a_oldIndex);
    void SetSkinLegacy2(int a_oldIndex);
    // ...and the same conversion for a bare index, so !dispN / !shadN written
    // under the old numbering land on the right skin. 0 = that skin is gone.
    [[nodiscard]] int  MigrateSkin2Index(int a_oldIndex);
    [[nodiscard]] const Skin& S();        // active skin tokens
    [[nodiscard]] int  SkinCount();
    [[nodiscard]] const Skin& SkinAt(int a_index);   // 1-based

    // apply the active skin to the ImGui style (call at init + on change)
    void Apply();

    // tornFrame breathing room: extra content inset (px, scaled) so the title
    // and body clear the ragged frame edge. 0 for every non-torn skin — each
    // managed window grows by 2x this and shifts its content in by it.
    [[nodiscard]] float FrameInsetX();
    [[nodiscard]] float FrameInsetY();

    // ★TOP-ONLY breathing room above a window title, and the reason it is not
    // just a bigger FrameInsetY: that one is symmetric (a window grows by 2x it
    // and the bottom strip measures up from it), so spending it on the TOP
    // would have opened the same gap under the gold bar.
    // ★Skin-independent. It was born as clearance for Sumi's brush frame and
    // Fable's crimson strip, both of which put ink where the title wants to be,
    // and is now applied to every skin so they all read alike.
    // ★A caller must add it to its OWN height as well as pass it to TitleBar --
    // see WinManager::TitleBar.
    [[nodiscard]] float TitleTopPad();

    // ★★ONE right margin for every control that hangs off the top-right of a
    // window -- the FIND box, the main window's EDIT / SETTINGS / x, a bag's
    // COLLECT. They stack vertically down the same edge, so nothing but a
    // shared number can keep them flush; measured, they were at 14 / 12 / 6.
    // ★The number is the BOARD's right edge (one PadX inside the frame) plus
    // the 2px the find box holds back from its child's clip boundary. Read
    // from the window's right edge, so a call site subtracts it and its own
    // width and is done -- no per-site frame-inset arithmetic.
    [[nodiscard]] float TopControlRightPad();

    // helpers: token -> ImU32 with alpha override
    [[nodiscard]] ImU32 Acc(float a_alpha);
    // The EMPHASIS colour for values, numbers and active chrome text. `hi` on
    // a dark panel, `acc` on a light one — see Skin::lightPanel. Call sites
    // that mean "make this stand out" use this instead of naming hi directly.
    [[nodiscard]] ImU32 Val(float a_alpha = 1.0f);
    [[nodiscard]] const ImVec4& ValVec();   // the same choice, unconverted
    // The money figure — Skin::goldNum when the skin sets one, Val() otherwise.
    [[nodiscard]] ImU32 GoldCol();
    // Chrome text (window titles, section headings, title-bar controls). `acc`
    // over a dark panel; over a LIGHT one acc is the darkest thing on screen
    // and these stop reading as headings — they read as borders.
    [[nodiscard]] ImU32 Chrome(float a_alpha = 1.0f);
    [[nodiscard]] ImU32 Col(const ImVec4& a_c, float a_alpha = -1.0f);

    // Window side padding, at scale 1. ★Every window has to ask HERE, not
    // hard-code 12: the managed windows size themselves from the padding
    // (width = pad + content + pad) and three of them push their own
    // WindowPadding on top. Changing only the style value moved nothing —
    // the four sites disagreed and the hard-coded ones won.
    [[nodiscard]] float PadX();
    [[nodiscard]] float PadY();

    // ── rounding ───────────────────────────────────────────────────────────
    // Windows and frames round by DIFFERENT amounts: 6px on a 700px window is
    // a soft corner, 6px on a 17px button is a lozenge. Skin::rounding stays
    // the single knob for skins that want one value everywhere.
    [[nodiscard]] float WinRounding();
    [[nodiscard]] float FrameRounding();

    // The window frame, and how far a snapped window overlaps its neighbour.
    // ★The frame is OPAQUE on purpose. Two translucent frames stacked on one
    // pixel row blend to a darker line, which is exactly the doubled seam the
    // overlap is meant to remove — opaque, drawing the same colour twice
    // lands on the same value, so a docked pair reads as one line.
    [[nodiscard]] ImU32 WinBorder();
    [[nodiscard]] float BorderPx();        // stroke width at the current scale
    [[nodiscard]] float BorderOverlap();   // snap overlap = one stroke, 0 if unframed

    // Ink for a button that is ON — its face is the brightest thing in the
    // skin, so white would sit on white. Dark on light, the way round the
    // rest of this panel now works.
    [[nodiscard]] ImU32 BtnOnInk();
    [[nodiscard]] ImVec4 BtnOnInkVec();

    // Window TITLE ink. Split from Chrome(): the title keeps white + a black
    // outline (21:1 whatever the panel does), while every other piece of
    // chrome went to dark ink. They used to share one colour.
    [[nodiscard]] ImU32 TitleInk();

    // The face of a button that is ON. Skins without a light panel keep the
    // accent wash they always had.
    [[nodiscard]] ImU32 BtnOn(float a_alpha = 1.0f);

    // ── item tooltip ───────────────────────────────────────────────────────
    // ★The tooltip stops being "the panel, floating". Over a light panel a
    // tooltip painted in the same blue has no edge at all — it lands on the
    // window it describes and dissolves. It goes DARK instead, and then each
    // kind of fact gets its own colour: heading, value, benefit, restriction.
    // Skins without a light panel keep the tokens they always used.
    [[nodiscard]] const ImVec4& TipHead();   // section label
    [[nodiscard]] const ImVec4& TipVal();    // name, numbers
    [[nodiscard]] const ImVec4& TipGood();   // temper, enchantment
    [[nodiscard]] const ImVec4& TipBad();    // "cannot", overload
    // ★A fact about THIS copy that is neither a number nor a rarity:
    // read, exhibited, the soul in a gem. Shares the museum wedge's
    // purple so the tile and the card say one thing in one colour.
    [[nodiscard]] const ImVec4& TipState();  // read / exhibited / soul
    [[nodiscard]] const ImVec4& TipSub();    // hints, shop price
    [[nodiscard]] const ImVec4& TipBody();   // running text
    // push/pop around BeginTooltip: dark ground + pale hairline + body ink
    void PushTipStyle();
    void PopTipStyle();

    // ★★The same chrome, for a panel that must LOOK like a tooltip without
    // being an ImGui popup. The shift-compare card paints on the foreground
    // draw list -- it cannot inherit a pushed style, so it used to carry the
    // SKIN's parchment ground and accent border and sat beside a dark tooltip
    // looking like a different mod, on every skin. One source for both, or
    // the two drift again the next time either is touched.
    [[nodiscard]] ImU32  TipBg();
    [[nodiscard]] ImU32  TipBorder();
    [[nodiscard]] float  TipRounding();
    [[nodiscard]] ImVec2 TipPadding();

    // Draw text with the 4-way 1px black outline the window title uses.
    // ★For the strings the eye SEEKS — values, totals, money. White on this
    // panel measures 3.30:1 on its own; against its own black edge it is 21:1
    // whatever the panel does. Reading strings do NOT get this: an outline
    // thickens the stroke, and a settings panel full of 9px labels goes heavy.
    // a_size 0 = the current font size; a_spacing adds tracking, which needs
    // the glyphs drawn one at a time (section labels use it).
    void TextOutlined(ImDrawList* a_dl, ImVec2 a_pos, ImU32 a_col, const char* a_text,
                      float a_size = 0.0f, float a_spacing = 0.0f);
    [[nodiscard]] ImVec2 TrackedSize(const char* a_text, float a_size, float a_spacing);

    // ── one lighting model ─────────────────────────────────────────────────
    // ★The light comes from the TOP-LEFT, everywhere: buttons rise (lit top
    // and left, shaded bottom and right) and grid cells sink — the same light
    // with the sign flipped. One rule, so the skin reads as a material rather
    // than as coloured shapes.
    // ★The WINDOW is the exception, and deliberately: it takes the lit line on
    // ALL FOUR sides. A dark line down a large face's bottom and right reads
    // as a second frame, not as depth — the window wants an edge, not a
    // direction. So BevelShd(true) exists for symmetry and is not used.
    // ★a_window also drops the strength to about two thirds. The same alpha
    // reads FAR stronger there — the eye follows a longer edge, and a
    // translucent panel sets its light line against the dark game world
    // instead of against its own face.
    [[nodiscard]] ImU32 BevelLit(bool a_window = false);
    [[nodiscard]] ImU32 BevelShd(bool a_window = false);


    // ★★Centre a whole LABEL in a rect by its ink. ImGui centres the line
    // BOX, which reserves descender room whether or not the string has one —
    // so "EDIT" (all caps, no descender) rides high inside its button and "+"
    // (x-height, no ascender either) sits low. Every label is off by a
    // different amount, which is why no single nudge can straighten them: the
    // amount is a property of the STRING, not of the button.
    // Measures the tallest ascent and deepest descent actually present.
    void TextInkCentered(ImDrawList* a_dl, const ImVec2& a_p0, const ImVec2& a_p1,
                         ImU32 a_col, const char* a_text, float a_size = 0.0f);

    // ★★Does text on this skin want a black edge behind it?
    // The outline exists to lift LIGHT ink off a mid-tone panel — SIMPLE's
    // white numbers measure 3.30:1 on their own and 21:1 against their own
    // edge. Dark ink on a PALE panel is already the strongest thing there
    // (parchment: 8.10:1), so an edge in the same value range as the ink just
    // fattens every stroke until the word reads as a blob.
    // Asked of the ink itself, so a new light skin needs no flag — and all
    // three places that outline text (body, window title, count badge) give
    // the same answer.
    [[nodiscard]] bool InkNeedsOutline();

    // ★The ground under an occupied cell — the board, the equipment doll and
    // the partner window must all ask HERE. They used to each reach for
    // sk.shade with their own alpha (doll 1.0, board the token's own), so one
    // colour said "occupied" loudly on one half of the window and almost
    // nothing on the other.
    // ★Deriving this from the panel was tried twice and reverted twice — the
    // reasoning, the measurements and why they were misleading are recorded at
    // the implementation in Theme.cpp. Read that before trying a third time.
    [[nodiscard]] ImU32 OccupiedGround();

    // ★Does this skin want the item shadow drawn LIGHT instead of black? A
    // black shadow separates a sprite from a pale board; on a dark one it is
    // the same colour as the problem. Which skins want which is a per-skin
    // judgement — see the implementation for why it is not derived.
    [[nodiscard]] bool LightItemShadow();

    // ── type scale ─────────────────────────────────────────────────────────
    // ★Four steps, and the title is the only maximum. They were all 20 —
    // heading, label, value and button — so nothing had a size of its own and
    // the close x, whose multiplier still assumed a 17px body, came out at 31
    // and outgrew the title it sat beside.
    [[nodiscard]] float SnapPx(float a_size);   // whole-pixel: see Theme.cpp
    // ★Already in PIXELS. SnapPx takes design units and SCALES them; this
    // one only rounds — for a value that has been through the scale once
    // already (ImGui::GetFontSize(), or a size derived from one).
    [[nodiscard]] float SnapAbs(float a_px);
    [[nodiscard]] float FontValue();    // numbers the eye seeks
    [[nodiscard]] float FontBody();     // labels, buttons, running text
    [[nodiscard]] float FontCaption();  // section headings, family names
    // ★The window name. Display scale ONLY — see the note in Theme.cpp: its
    // bar is layout, and the text-size setting moves type, not layout.
    [[nodiscard]] float FontTitle();

    // Horizontal rules between stat blocks (and ImGui's own separators).
    // acc over a dark panel; over a LIGHT one acc is the darkest thing on
    // screen, so the rule stops reading as a divider and reads as a crack.
    [[nodiscard]] ImU32 Rule();

    // ★The Plain()/kPlainAlpha tagging that used to live here is gone with the
    // frame-wide outline pass it existed for. It marked widget text by setting
    // alpha 254 so the pass would skip it; once every string that wants an edge
    // asks for one directly (Theme::TextOutlined), nothing reads the tag and
    // the alpha is simply 255 like every other skin's already was.

    // gauge/slider track chrome: translucent skins fill in sel (the EDIT
    // painter red) — a grey acc fill reads as dead space over the world
    // ★And on a LIGHT panel the gauge fills UPWARD in brightness instead: the
    // accent is the darkest token there, so an accent fill in a dark well
    // reads as a groove cut into the panel with no visible level in it.
    [[nodiscard]] ImU32 GaugeTrack();   // the empty part
    [[nodiscard]] ImU32 GaugeFill();
    [[nodiscard]] ImU32 GaugeBorder();

    // ★★The number centred on a gauge, drawn by US so it can carry the black
    // edge ImGui cannot give it — on a light panel the fill is white and so is
    // the ink, and a filled track otherwise swallows its own value.
    // ★It is PUBLIC because two windows draw this control. The settings
    // sliders go through ChromeSliderInt/Float below; the EDIT panel builds
    // its rows by hand (its fill fractions are not the widget's own range), so
    // it needs the value half without the rest. While this was private to
    // Theme.cpp, every fix that landed on the settings sliders stopped at the
    // editor's door — which is exactly how nine EDIT rows kept ImGui's
    // unoutlined text after the settings ones were fixed.
    // a_isInt picks which type a_fmt is fed.
    void GaugeValue(ImDrawList* a_dl, const ImVec2& a_p, float a_w, float a_h,
                    const char* a_fmt, float a_v, bool a_isInt);

    // ---- gauge step buttons ------------------------------------------------
    //
    // Two arrows sunk into the ENDS of a gauge track, each nudging the value by
    // one step. Held down they repeat (ImGui's own key-repeat timing).
    //
    // ★Inside the track, not beside it. Flanking buttons would have cost the
    // track 36 of its 158px — a quarter of the drag range — and the drag is
    // still how a value is found; the buttons only settle it. The trade is that
    // the outer 16px of each end no longer starts a drag, which matters least
    // exactly where the arrows are: at the ends, where a drag is already
    // finished.
    //
    // Call AFTER the widget so the buttons sit on top of it. Returns true on
    // any nudge, and clamps to [a_lo, a_hi].
    [[nodiscard]] bool GaugeStep(const ImVec2& a_p, float a_w, float a_h,
                                 const char* a_id, float& a_val, float a_step,
                                 float a_lo, float a_hi);
    [[nodiscard]] bool GaugeStepInt(const ImVec2& a_p, float a_w, float a_h,
                                    const char* a_id, int& a_val, int a_step,
                                    int a_lo, int a_hi);
    // width of one arrow, so a caller can keep its own hit tests clear of them
    [[nodiscard]] float GaugeStepW();

    // ---- typing a value straight into a gauge --------------------------------
    //
    // ★★A gauge draws its own number so the number can carry a black edge,
    // which means the WIDGET's text is pushed transparent. Double-click to type
    // and that transparency lands on the characters being typed: the old value
    // stays put, the new one is invisible, and only the caret shows. So the row
    // has to know it is being edited BEFORE it submits the widget.
    //
    // Ask with the same id string the widget is given.
    [[nodiscard]] bool GaugeEditing(const char* a_id);
    // The track wearing its input-field clothes: fill removed, well darkened,
    // border in the skin's accent. Drawn INSTEAD of GaugeBar while editing, so
    // a gauge and a text field are never mistaken for each other.
    void GaugeInputFrame(ImDrawList* a_dl, const ImVec2& a_p, float a_w, float a_h);
    // ★The gauge centres its number; ImGui's text field does not, so the figure
    // JUMPED to the left edge the instant it was double-clicked. Push a frame
    // padding sized to hold the text in the middle, submit the widget, pop.
    // Pair them, and keep the pair OUTSIDE PushChromeStyle/PopChromeStyle --
    // style vars are a stack, and PopChromeStyle takes whichever one is on top.
    void GaugeInputPushAlign(const char* a_id, float a_w);
    void GaugeInputPopAlign();
    // What to put in the row's note column while typing.
    [[nodiscard]] const char* GaugeInputHint();

    // The well, its fill and its border, in one call.
    // ★The fill stops at the ARROWS rather than at the track edge. Running it
    // under them made a full gauge look like it had swallowed its own controls
    // — and worse, hid which end was at its limit. The arrows are chrome that
    // happens to sit inside the frame; the bar is what the value fills.
    void GaugeBar(ImDrawList* a_dl, const ImVec2& a_p, float a_w, float a_h,
                  float a_frac);

    // TextOutlined, but it ADVANCES the cursor like ImGui::Text does, so it
    // drops into a normal layout. ★Same reason GaugeValue is public: the
    // settings rows and the EDIT rows are the same kind of label, and while
    // this lived inside UIRoot the editor's labels stayed unoutlined
    // TextColored calls after the settings ones were fixed.
    void TextOutlinedFlow(ImU32 a_col, const char* a_text,
                          float a_size = 0.0f, float a_spacing = 0.0f);

    // v10.4: border drawn as 8 gradient segments — bright at the corners,
    // fading out toward each edge's middle (vertical runs are inset 1px so
    // the corner pixel never double-blends). a_col carries the peak alpha.
    void CornerFade(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col,
                    float a_frac = 0.32f);
    // GI49b: same 8 segments with per-edge colors (status rings split
    // temper/poison across left+top vs right+bottom)
    void CornerFadeEdges(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max,
                         ImU32 a_top, ImU32 a_left, ImU32 a_right, ImU32 a_bottom,
                         float a_frac);

    // ★★A torn-paper panel, DRAWN — no texture, so the tearing is the same
    // size in a 2x2 bag as in the main window. The nine-slice it replaces had
    // no way to be: it stretches its edge strips, which carry the torn
    // silhouette, so a small window compressed them (0.16x on an Ore Sack) and
    // the paper turned into a row of needles. Cropping instead only moved the
    // problem to the seam.
    // The edge is a 1-D function of distance travelled — three octaves of
    // value noise plus a rare deeper bite — and each 3px step becomes one
    // trapezoid, which is convex and therefore drawable as-is.
    // a_seed: derive it from the window key so each window keeps its own
    // silhouette and it never changes as the window moves or resizes.
    void TornPanel(ImDrawList* a_dl, const ImVec2& a_min, const ImVec2& a_max,
                   ImU32 a_col, unsigned int a_seed);

    // ---- ink skins -----------------------------------------------------------
    // The active skin's paper sheet, or false when it has none. ★A LOOKUP, not
    // a draw: the caller already owns the window rect and the draw order, and
    // the two ink skins differ from the torn ones in what they paint, not in
    // where. Loads on first ask and caches.
    //
    // ★★CUT TO FIT, NOT STRETCHED, which is why it hands back uvs. Stretching
    // one 16:9 sheet over every window distorts the grain by however much the
    // two aspects differ -- measured, 1.10x on the main window (invisible) but
    // 2.64x on a tall bag, where the fibres smear into vertical streaks and the
    // paper stops looking like paper. So the uvs name the largest piece of the
    // sheet WITH THE WINDOW'S OWN ASPECT, and the grain keeps its true scale at
    // every size. A tall window is a tall piece cut off the same sheet, which
    // is also what it physically is.
    // ★Never tiled: the sheet's opposite edges differ by 8.3/8.5 luma, so a
    // seam would show.
    // a_key: offsets which part of the sheet is cut, so two windows open side
    // by side are not the same square inch of paper. Derive it from the window
    // key, as TornPanel's seed is.
    [[nodiscard]] bool PaperTexture(ImTextureID& a_out, ImVec2 a_size,
                                    unsigned int a_key,
                                    ImVec2& a_uv0, ImVec2& a_uv1);

    // ★★The multiplier that turns the sheet on disk into the skin's paper:
    // Skin::winBg divided by the file's own average. So winBg is not a colour
    // behind the sheet -- on a paper skin ImGui's WindowBg is transparent and
    // nothing is behind it -- it is the colour the sheet must READ as, and the
    // SETTINGS chip is painted from that same value. Before this, the chip came
    // from the token and the window came from the file, and the only thing
    // keeping them together was that nobody had changed either one.
    // Returns white when the skin has no paper. Cannot brighten (a tint
    // multiplies); a winBg above the sheet's average clamps and says so once.
    [[nodiscard]] ImU32 PaperTint();

    // ★★Paints the sheet AND dissolves its edge. A photographed sheet ends in a
    // razor-straight cut, and no amount of brushwork on top hides that: the
    // frame's dry gaps are exactly where a hard edge shows through, and its
    // corners never cover the sheet's.
    // ★The fade CANNOT be baked into the file. The sheet is cut to the window's
    // aspect, so what reaches the screen is an interior slice -- a soft border
    // painted into the png is cropped away before anyone sees it. It has to be
    // made at draw time, which is why this is a draw call and PaperTexture is
    // only a lookup.
    // Returns false when the skin has no paper, so the caller falls through.
    bool PaperPanel(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max,
                    unsigned int a_key, float a_fade);

    // ★How far the sheet and the frame reach PAST the window rect. The window
    // rect is where the content stops; paper and brush both carry on, or the
    // frame reads as a mark floating inside a rectangle -- which is exactly
    // what it looked like on the first pass.
    inline constexpr float kInkBleed = 7.0f;
    // ...and how wide the sheet's edge takes to dissolve. Wider than the bleed
    // on purpose: the fade has to finish OUTSIDE the brush, or its own gradient
    // becomes the straight edge it was added to remove.
    inline constexpr float kInkFade = 16.0f;

    // Does this skin draw its chrome with brush marks? Asked as a question
    // rather than tested as `paper != nullptr` at every call site, so a future
    // skin can have one without the other.
    [[nodiscard]] bool InkChrome();

    // The item grid's inside line, in px at the current window. ★Asked for
    // rather than recomputed: the LATTICE draws it and the occupied ground has
    // to clear it, and those two living in different files is exactly how a
    // 1px clearance survived next to a 2.9px line -- the leftover paper read as
    // a bright gap running through every multi-cell item.
    // ★All three are px at scale 1, multiplied by the UI scale -- NOT a
    // fraction of the current window. See the note in the cpp: every panel is
    // its own ImGui child, so "a fraction of the window" is a different number
    // in each of them and the same line came out three weights.
    [[nodiscard]] float InkRulePx();    // the item grid's inside line
    [[nodiscard]] float InkEdgePx();    // a board's outer border
    [[nodiscard]] float InkHeavyPx();   // the doll's cells

    // ---- one mark ------------------------------------------------------------
    // A brush line, stretched along its length. a_th is its thickness in px.
    // ★The sprite's own aspect is NOT preserved, and must not be: the strokes
    // chosen for this carry their character ALONG the run (dry split bristles),
    // which survives stretching. A stroke with a blob or a tip in it would put
    // that one feature in the middle of the line -- the first assembly failed
    // exactly that way.
    // a_whole: use the sprite END TO END instead of a section of it. A section
    // is cut square at both ends, which is right for a mark whose ends are
    // covered and wrong for one that stands alone.
    // ★a_fade: taper both ends to nothing over this FRACTION of the length,
    // via per-vertex alpha. "The corner covers the cut" was never true -- the
    // corner is itself thinning where it meets the side, so a square end either
    // doubles the ink with it or is left exposed. Two marks that both fade lie
    // on top of one another and read as one stroke, which is what the corner
    // sprite was painted expecting. 0 keeps the hard-ended behaviour.
    void InkStroke(ImDrawList* a_dl, ImVec2 a_from, float a_len, float a_th,
                   ImU32 a_col, bool a_vert = false, bool a_whole = false,
                   float a_fade = 0.0f);

    // ★A DIVIDER, drawn the way the skin draws dividers. Every caller used to
    // AddLine with Theme::Rule(), which is a colour and cannot answer "is this
    // skin's divider a hairline or a brush mark" -- so an ink window kept
    // hairlines everywhere a colour was enough to look right.
    void RuleLine(ImDrawList* a_dl, ImVec2 a_from, ImVec2 a_to);

    // The fine ruled line. Same call shape as InkStroke on purpose: which of
    // the two a surface uses IS the rule -- a border is a brush, an inside is
    // a ruler -- so the difference stays one word at the call site.
    void InkRule(ImDrawList* a_dl, ImVec2 a_from, float a_len, float a_th,
                 ImU32 a_col, bool a_vert = false);

    // ---- assemblies ----------------------------------------------------------
    // The window's frame: four sides and four corners.
    // ★★The sides take their thickness and centreline FROM THE CORNER'S OWN
    // ARM. Measured, the corner sprites differ by 4.5x in arm thickness, so a
    // side drawn at any constant lines up with at most one of them -- which is
    // how the first comparison came out with a single usable pairing and looked
    // like a preference.
    // ★Corners are drawn LAST, over the sides. That covers the join, which in
    // turn means a side may be cut anywhere and no 9-slice is needed: the frame
    // fits any window without a stretched middle.
    // a_corner: the corner mark's drawn size in px. It is CLAMPED to the box
    // rather than abandoned when the box is small -- a frame without its
    // corners is four cut brush-ends meeting at four square notches, which is
    // the one thing this assembly exists to avoid.
    void InkFrame(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col,
                  float a_corner);

    // The corner size a mark of thickness a_th needs, so the sides it meets
    // come out at that thickness. ★Derived from the sprite's own arm, never
    // guessed: the six corners differ by 4.5x in arm thickness.
    [[nodiscard]] float InkCornerFor(float a_th);

    // The plate behind a window title -- the QUICK WHEEL's own banner sprite,
    // tinted. ★Reused rather than remade: the wheel already paints a plate
    // behind its group name, and a second mark meaning the same thing is how a
    // UI stops looking like one hand made it.
    void InkTitleMark(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col);

    // A stamped seal, centred on a_centre and a_size across. ★Drawn in its own
    // vermilion, not the skin's accent: on this paper the accent is a clay
    // SURFACE tone, and a seal printed in it reads as a faded copy of itself.
    void InkSeal(ImDrawList* a_dl, ImVec2 a_centre, float a_size, float a_alpha);

    // The wash behind a worn item. a_key chooses the blob and its rotation, so
    // a slot keeps one across frames while its neighbours differ.
    // ★Drawn PAST the cell on every side. The bleed is what stops a column of
    // slots reading as a column of stamps.
    void InkWash(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col,
                 unsigned int a_key);

    // Drop the ink art cache so a repainted sheet is picked up without a
    // restart. Releases SRVs -> call from UIRoot::Tick only, beside
    // Fallback::ReloadAssets and Wheeler::ReloadMedallions.
    void ReloadInkArt();

    // Design-pass A: the ONE quantity-slider look (settings track chrome —
    // black .2 ground, acc .20 fill, acc .25 border, centred hi value) shared
    // by the quantity popup, the pouch withdraw and any future int slider.
    // Draws at the current cursor, a_w wide, frame height tall.
    bool ChromeSliderInt(const char* a_id, int* a_v, int a_min, int a_max,
                         float a_w, const char* a_fmt = "%d");

    // float twin, absolute position tracking (the settings rows previously
    // used DragFloat under slider-looking chrome — dragging felt broken
    // because the value crawled by drag-speed instead of following the mouse)
    // ★a_def: right-click restores it. Pass a negative to opt out. ImGui has
    // no such gesture of its own, and the settings rows have no room for the
    // EDIT panel's "(def 1.00)" column — the caller puts the wording in the
    // bottom bar instead, so the affordance costs no pixels.
    // ★a_step: what one press of the ◀ ▶ arrows moves the value by. 0 derives
    // it from the range, which is right for every slider whose range is wide
    // enough for a hundredth to mean something. The track itself is always
    // continuous; this only sizes the arrows.
    bool ChromeSliderFloat(const char* a_id, float* a_v, float a_min, float a_max,
                           float a_w, const char* a_fmt = "%.2f",
                           float a_def = -1.0f, float a_step = 0.0f);

    // ★The defaults live HERE and the runtime values are initialised FROM
    // them, so "restore the default" and "what a fresh install had" cannot
    // drift apart. Glow and icon gain differ per style/slot, which is exactly
    // why a hard-coded 1.0 in the reset path would have been wrong.
    inline constexpr float kDefScale     = 1.00f;   // fixed; Scale() returns it
    // ★★The SCALE slider's three numbers, in one place. A right-click reset,
    // the slider's own ends and a fresh install all read from here, so they
    // cannot say different things.
    // ★These are SETTING values (kScaleBase multiplies them). The ends were
    // asked for as 0.75~1.10 while the default still read 0.90; carried across
    // at the same BOARD SIZE — which is what the numbers were chosen by eye
    // against — that span is 0.83~1.22, rounded here to slider-friendly ends.
    // Board size still runs 0.765~1.08 against the old 0.75~1.10.
    //
    // ★★1.2.1: THE TOP END IS NOT WHAT IT READS. kScaleBase is 0.90, so the
    // old maximum of 1.20 drew the board at 1.08 -- eight percent over default,
    // which on an ultrawide at 1440p or above is no bigger at all. Reported as
    // "even the maximum is too small", and it was.
    // ★2.00 (board 1.80, cell 86px) and not higher, for a reason that is not
    // taste: the pak stores each icon at what a TILE can show -- 160px per
    // footprint cell (IconCache's `limit`). At 86px a 1x1 still has room to
    // spare; past ~1.85 the cell outgrows its own sprite and every small icon
    // starts being upscaled. Raising this further means re-baking 427MB of
    // icons, so the ceiling belongs where the art already reaches.
    inline constexpr float kDefCellScale = 1.00f;
    // ★The bottom end moves with the top for the same reason: 0.85 was only
    // 0.765 of the board, a shade under default, so "smaller" barely was. 0.60
    // (board 0.54, cell 26px) is where the FLOOR actually is -- every marker
    // holds a one-pixel rim by clamp (RimPx) rather than by scale, so the
    // shapes survive, and the icons are downscaled from a supersampled capture
    // rather than stretched. Below that the count badges stop being readable.
    inline constexpr float kMinCellScale = 0.60f;
    inline constexpr float kMaxCellScale = 2.00f;
    // ★0/0 means "the shipped rig", not "no light" — the offset origin. That
    // is what makes a right-click reset here identical to a fresh install, and
    // what lets an item def's 0/0 mean "whatever the global says".
    inline constexpr float kDefCapLightAz = 0.0f;
    inline constexpr float kDefCapLightEl = 0.0f;
    [[nodiscard]] constexpr float DefGlowGain(int a_style) { return a_style == 0 ? 1.55f : 0.75f; }
    [[nodiscard]] constexpr float DefIconGain(int a_slot)  { return a_slot  == 0 ? 1.35f : 1.02f; }
    // ★★1.0.5 ITEM SHADOW defaults — the ambient-65% mockup the author picked,
    // stated in the same three numbers the sliders now expose.
    inline constexpr float kDefShadowDist = 0.0f;    // px, toward lower-right
    inline constexpr float kDefShadowBlur = 2.5f;    // px of edge spread
    inline constexpr float kDefShadowOpac = 0.65f;   // 0..1
    [[nodiscard]] constexpr float DefShadow(int a_axis)
    {
        return a_axis == 0 ? kDefShadowDist
             : a_axis == 1 ? kDefShadowBlur
                           : kDefShadowOpac;
    }
    // ...and the same for whatever slot is live right now, which is what a
    // reset on the visible slider needs. Keeps the slot arithmetic in one file
    // instead of at every call site. (The glow twin went with the glow style
    // itself — the reset it served no longer has a slider to sit on.)
    [[nodiscard]] float DefaultIconGain();

    // Phase 2: the invisible-widget style for any Drag/Slider drawn OVER
    // custom track chrome (transparent frame + hi-coloured value text) —
    // formerly five hand copies across Editor and the Chrome sliders.
    // a_sliderGrab also hides the Slider* grab (Drag* widgets have none).
    void PushChromeStyle(bool a_sliderGrab);
    void PopChromeStyle(bool a_sliderGrab);
}
