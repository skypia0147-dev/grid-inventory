#include "game/GoldCoins.h"
#include "ui/Editor.h"
#include "ui/Fallback.h"
#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace FUI::Editor
{
    namespace
    {
        Hooks               g_hooks;
        bool                g_editMode = false;
        RE::TESBoundObject* g_sel = nullptr;
        // ★★★THE STACK CAP THE BOARD IS CURRENTLY BUILT FOR.
        //
        // Every other row in this editor applies as you drag, and that is the
        // point of them -- you watch the footprint change under the cursor.
        // The stack cap cannot work that way. It starts at 0 ("auto"), so the
        // first value a drag enters is 1, and a cap of 1 gives every single
        // unit its own tile: a stack of sixty potions explodes across the
        // board the instant you touch the control, before you have said what
        // you actually want (reported).
        //
        // So this row alone reaches the board at SAVE and nowhere else. The
        // number moves under the cursor as usual and the board does not stir
        // until the edit is committed -- which is also the only moment the
        // player has actually said what they meant.
        //
        // Every LIVE apply therefore has to send the stack the board is built
        // for rather than the one being edited (see LiveDef), or adjusting any
        // other row would drag a half-chosen cap along with it.
        int g_stackApplied = 0;
        std::string         g_selKey;

        FullDef g_cur;                 // live-edited values
        // ★★1.0.5: editing is a SESSION. Selecting an item takes a baseline;
        // every change applies to memory at once (so the icon updates as you
        // drag) but touches no file until SAVE. Leaving without saving puts
        // the baseline back.
        // The old model auto-wrote 0.5s after any change, which meant there
        // was no moment the player could point at as "committed" — and the
        // only way back was "Reset to default", which threw away every other
        // adjustment along with the one mistake.
        FullDef g_base;                // values this session started from
        bool    g_baseOverride = false;   // did the item HAVE an ini line then?
        bool    g_dirty = false;       // unsaved changes exist
        double  g_revertNote = 0.0;    // bottom-bar "discarded" until this time

        // ★★1.0.5 — 8, not 6. The socket overlay packs a footprint into a
        // uint64 (Badges::TileShape.cells = 8x8), so 8 is the ceiling the rest
        // of the code already lives with; the painter was the only thing
        // stopping at 6. Anything past 8 would mean widening that mask, and no
        // single item has business covering more than 64 cells.
        // Keep in step with IconStudio's painter (app.js shapeToCells).
        inline constexpr int kPaintN = 8;
        bool g_paint[kPaintN][kPaintN] = {};   // footprint painter cells
        bool g_painting = false;       // drag-paint state
        bool g_paintValue = true;      // painting or erasing this drag
        // ★Which slider block the orientation section shows: 0 = rotation,
        // 1 = position. A tab rather than two more rows -- the panel was
        // already at six sliders. Kept across items on purpose: someone lining
        // up a shelf of weapons stays on the same axis for all of them.
        int g_footTab = 0;

        // load the current def into the painter cells
        void DefToPainter()
        {
            std::memset(g_paint, 0, sizeof(g_paint));
            if (!g_cur.shape.empty()) {
                int r = 0, c = 0;
                for (char ch : g_cur.shape) {
                    if (ch == '|') { ++r; c = 0; continue; }
                    if (r < kPaintN && c < kPaintN) g_paint[r][c] = (ch == '1');
                    ++c;
                }
            } else {
                for (int r = 0; r < kPaintN && r < g_cur.h; ++r)
                    for (int c = 0; c < kPaintN && c < g_cur.w; ++c)
                        g_paint[r][c] = true;
            }
        }

        // painter -> trimmed shape/w/h (H2: full rectangle collapses to w/h)
        void PainterToDef()
        {
            int minR = kPaintN, maxR = -1, minC = kPaintN, maxC = -1;
            for (int r = 0; r < kPaintN; ++r)
                for (int c = 0; c < kPaintN; ++c)
                    if (g_paint[r][c]) {
                        minR = (std::min)(minR, r); maxR = (std::max)(maxR, r);
                        minC = (std::min)(minC, c); maxC = (std::max)(maxC, c);
                    }
            if (maxR < 0) return;   // empty painter: don't save (H2)

            const int w = maxC - minC + 1, h = maxR - minR + 1;
            bool full = true;
            std::string shape;
            for (int r = minR; r <= maxR; ++r) {
                if (r > minR) shape += '|';
                for (int c = minC; c <= maxC; ++c) {
                    shape += g_paint[r][c] ? '1' : '0';
                    if (!g_paint[r][c]) full = false;
                }
            }
            g_cur.w = w;
            g_cur.h = h;
            g_cur.shape = full ? std::string() : shape;
        }

        // Values apply LIVE (memory-only, every change); the dirty machinery
        // only debounces the ini FILE write — a slider drag would otherwise
        // rewrite the file every frame.
        void MarkDirty() { g_dirty = true; }

        // What a LIVE apply is allowed to say: everything being edited, except
        // the stack cap, which stays at whatever the board is already built
        // for until Save.
        FullDef LiveDef()
        {
            FullDef d = g_cur;
            d.stack = g_stackApplied;
            return d;
        }

        // SAVE: the one place a change reaches the file. The baseline moves up
        // to here, so from now on "revert" means back to what was just saved.
        void SaveSession()
        {
            if (!g_dirty || !g_sel || !g_hooks.setOverride) return;
            const bool stackMoved = g_cur.stack != g_stackApplied;
            g_hooks.setOverride(g_sel, g_cur, true);   // persist to ini
            g_stackApplied = g_cur.stack;              // ★the cap lands HERE
            if (stackMoved) Grid::RequestRebuild();
            g_base = g_cur;
            g_baseOverride = true;                     // the line exists now
            g_dirty = false;
        }

        // LEAVE WITHOUT SAVING: put the item back exactly as it was found.
        // ★The two cases are not the same. If the item already had an ini
        // line, restoring means writing the baseline back to MEMORY (the file
        // still holds it, untouched). If it had none, the live-apply during
        // this session CREATED a memory override, and the way to undo that is
        // to drop it — writing the baseline back would leave the item carrying
        // an override it never had.
        void RevertSession()
        {
            if (!g_dirty || !g_sel) return;
            g_cur = g_base;
            g_stackApplied = g_base.stack;
            if (g_baseOverride) {
                if (g_hooks.setOverride) g_hooks.setOverride(g_sel, g_base, false);
            } else if (g_hooks.resetOverride) {
                g_hooks.resetOverride(g_sel);
            }
            g_dirty = false;
            g_revertNote = ImGui::GetTime() + 2.5;
            Grid::RequestRebuild();
        }

        // mockup .drow track: [label][accent-filled value bar][defnote].
        // The fill/border draw first; the DragFloat sits on top with a
        // transparent frame so only the centred value text shows.
        void TrackChrome(ImVec2 a_p, float a_w, float a_h, float a_frac)
        {
            auto* dl = ImGui::GetWindowDrawList();
            // ★Theme::FrameRounding(), not Skin::rounding. A rounding of 0
            // means the skin NAMES none, not that it wants square corners —
            // reading the raw field gave every Simple-family track hard 90°
            // corners while the settings window, which goes through the
            // accessor, rounded them.
            // ★ONE definition, shared with the settings sliders. The fill stops
            // at the step arrows there, and a second copy of that rule here
            // would be a second chance to disagree with it.
            Theme::GaugeBar(dl, a_p, a_w, a_h, a_frac);
        }

        constexpr float kLabelW = 46.0f;   // * scale
        constexpr float kTrackW = 158.0f;  // * scale

        // ★★★THE TRACK, CLIPPED TO WHAT THE ROW ACTUALLY HAS LEFT.
        //
        // kTrackW is a FIXED width, so the moment a scrollbar appeared the
        // track carried on underneath it and took its value label with it --
        // reported as "raising the scale makes the settings and edit windows
        // unusable": the body gets taller, ImGui adds a vertical scrollbar,
        // every row loses ScrollbarSize of width, and nothing in here noticed.
        //
        // Asking for the remaining width costs nothing while there is room --
        // min() returns kTrackW untouched -- and is the whole fix when there is
        // not. The floor keeps a squeezed track grabbable rather than letting
        // it collapse to a hairline.
        //
        // ★Call it AFTER the label's SameLine: the value it reads is measured
        // from the current cursor to the content edge, which is exactly the
        // space the track is allowed to use.
        [[nodiscard]] float TrackW(float a_scale)
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            return (std::min)(kTrackW * a_scale, (std::max)(24.0f, avail));
        }

        // ★The name beside a gauge, drawn the way the settings rows draw
        // theirs: the skin's own ink, with the black edge that keeps a light
        // string legible over a translucent panel. These were TextColored in
        // inkDim, which is the token for SECONDARY text — a field you can edit
        // is not secondary, and on the derived skins it left the editor's
        // labels a shade dimmer than every other label in the UI.
        // The wording is NOT upper-cased here: that is a settings-panel rule
        // (see UIRoot's SettingLabel), and these labels are already terse.
        void RowLabel(const char* a_text)
        {
            Theme::TextOutlinedFlow(Theme::Chrome(1.0f), a_text);
        }

        // ★★A gauge row is ONE control: track, widget, and the number on top.
        // This window used to assemble those three by hand at every row, and
        // kept ImGui's own text — so when the settings sliders learned to draw
        // their value themselves (ImGui cannot outline what it draws, and on a
        // light panel a white fill under white ink erases it), all nine rows
        // here were left behind. Everything that draws a gauge now runs through
        // the same two functions: TrackChrome for the well, Theme::GaugeValue
        // for the figure.
        // a_widget runs with the widget's own text made INVISIBLE; whatever it
        // leaves in a_shown is what gets printed, so the number is always the
        // post-drag one.
        template <class W>
        bool GaugeRow(ImVec2 a_p, float a_w, float a_frac, bool a_grab,
                      const char* a_id, const char* a_fmt, const float& a_shown,
                      W&& a_widget)
        {
            auto*       dl = ImGui::GetWindowDrawList();
            const float h  = ImGui::GetFrameHeight();
            // ★Asked BEFORE the widget, because the answer decides whether the
            // text gets pushed transparent -- and that cannot be undone after
            // the widget is submitted.
            const bool typing = Theme::GaugeEditing(a_id);
            if (typing) Theme::GaugeInputFrame(dl, a_p, a_w, h);
            else        TrackChrome(a_p, a_w, h, a_frac);
            ImGui::SetCursorScreenPos(a_p);
            // ★Outside PushChromeStyle: both push style VARS, and PopChromeStyle
            // pops whichever is on top of the stack.
            if (typing) Theme::GaugeInputPushAlign(a_id, a_w);
            Theme::PushChromeStyle(a_grab);
            // ★★NOT while typing: the transparency we use to hide the widget's
            // own number would hide the characters being typed as well.
            if (!typing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
            // ★★Without this the step arrows are DEAD. ImGui hands hover to
            // the item submitted FIRST, and this widget covers the whole track,
            // so a click on an arrow reached the slider instead — and two quick
            // clicks put it into text entry, which is what "the cursor blinks
            // and the slider stops responding" was.
            ImGui::SetNextItemAllowOverlap();
            ImGui::SetNextItemWidth(a_w);
            const bool ch = a_widget();
            if (!typing) ImGui::PopStyleColor();
            Theme::PopChromeStyle(a_grab);
            if (typing) Theme::GaugeInputPopAlign();
            else        Theme::GaugeValue(dl, a_p, a_w, h, a_fmt, a_shown, false);
            return ch;
        }

        // int flavour — same contract, %d formatting
        template <class W>
        bool GaugeRowI(ImVec2 a_p, float a_w, float a_frac, bool a_grab,
                       const char* a_id, const char* a_fmt, const int& a_shown,
                       W&& a_widget)
        {
            auto*       dl = ImGui::GetWindowDrawList();
            const float h  = ImGui::GetFrameHeight();
            const bool typing = Theme::GaugeEditing(a_id);   // see the float version
            if (typing) Theme::GaugeInputFrame(dl, a_p, a_w, h);
            else        TrackChrome(a_p, a_w, h, a_frac);
            ImGui::SetCursorScreenPos(a_p);
            if (typing) Theme::GaugeInputPushAlign(a_id, a_w);   // outside the chrome push
            Theme::PushChromeStyle(a_grab);
            if (!typing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
            // ★★Without this the step arrows are DEAD. ImGui hands hover to
            // the item submitted FIRST, and this widget covers the whole track,
            // so a click on an arrow reached the slider instead — and two quick
            // clicks put it into text entry, which is what "the cursor blinks
            // and the slider stops responding" was.
            ImGui::SetNextItemAllowOverlap();
            ImGui::SetNextItemWidth(a_w);
            const bool ch = a_widget();
            if (!typing) ImGui::PopStyleColor();
            Theme::PopChromeStyle(a_grab);
            if (typing) {
                Theme::GaugeInputPopAlign();
            } else {
                Theme::GaugeValue(dl, a_p, a_w, h, a_fmt,
                                  static_cast<float>(a_shown), true);
            }
            return ch;
        }

        // ★★Right-click a track to put THAT ONE field back to its default —
        // the same gesture the settings sliders answer to. It restores exactly
        // the number already printed at the end of the row, so there is nothing
        // new to learn, and "Reset to default" stays the button that resets
        // everything at once.
        // Call it immediately after the DragFloat, while IsItemHovered still
        // refers to it.
        // ★★Right-click a track to put THAT ONE field back to where this
        // editing session STARTED — not to the category default, which is what
        // the "Reset to default" button is for. Those are different questions:
        // "undo what I just did to this row" and "forget this item's tuning
        // entirely", and the row note below names whichever one applies.
        bool ResetOnRightClick(float& a_val, float a_base)
        {
            if (!ImGui::IsItemHovered()) return false;
            // the row prints the value itself; the hint only names the gesture
            UIRoot::NoteHoverHint(Lang::T(Lang::Str::HintRowRevert));
            if (!ImGui::IsMouseClicked(ImGuiMouseButton_Right)) return false;
            a_val = a_base;
            return true;
        }

        // the "(was 90°)" / "(unchanged)" note that closes every editable row
        void RowNote(bool a_changed, const char* a_fmt, float a_base)
        {
            if (a_changed) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), a_fmt, Lang::T(Lang::Str::EditWas), a_base);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Chrome(0.85f)),
                    "%s", buf);
            } else {
                ImGui::TextDisabled("(%s)", Lang::T(Lang::Str::EditUnchanged));
            }
        }

        // Where the sprite sits inside its footprint, in CELLS. Same row
        // grammar as AngleSlider; the range is -1..1 because a nudge bigger
        // than one cell means the footprint itself is wrong.
        bool OffsetSlider(const char* a_label, float& a_val, float a_defVal)
        {
            const float S = Theme::Scale();
            RowLabel(a_label);
            ImGui::SameLine(kLabelW * S);
            const std::string id = std::string("##") + a_label;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float  h = ImGui::GetFrameHeight();
            const bool typing = Theme::GaugeEditing(id.c_str());
            const float tw = TrackW(S);
            bool changed = GaugeRow(at, tw,
                (a_val + 1.0f) * 0.5f, false, id.c_str(), "%.2f", a_val,
                [&] {
                    return ImGui::DragFloat(id.c_str(), &a_val, 0.005f,
                        -1.0f, 1.0f, "%.2f");
                });
            // ★Reset FIRST: it reads IsItemHovered, which means the last item
            // submitted -- and the arrows are items. Stepping before it would
            // aim the right-click at whichever arrow was drawn last, so a
            // right-click on the track would quietly stop resetting.
            changed |= ResetOnRightClick(a_val, a_defVal);
            if (!typing) {
                changed |= Theme::GaugeStep(at, tw, h, id.c_str(),
                                            a_val, 0.01f, -1.0f, 1.0f);
            }
            ImGui::SameLine();
            if (typing) ImGui::TextDisabled("%s", Theme::GaugeInputHint());
            else RowNote(std::fabs(a_val - a_defVal) > 0.005f, "(%s %.2f)", a_defVal);
            return changed;
        }

        // drag row against the session baseline (H6 ⑤). DragFloat gives
        // pixel-proportional fine control; Ctrl+click types an exact value.
        bool AngleSlider(const char* a_label, float& a_val, float a_defVal)
        {
            const float S = Theme::Scale();
            RowLabel(a_label);
            ImGui::SameLine(kLabelW * S);
            const std::string id = std::string("##") + a_label;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float  h = ImGui::GetFrameHeight();
            const bool typing = Theme::GaugeEditing(id.c_str());
            const float tw = TrackW(S);
            bool changed = GaugeRow(at, tw,
                (a_val + 180.0f) / 360.0f, false, id.c_str(), "%.1f\xC2\xB0", a_val,
                [&] {
                    return ImGui::DragFloat(id.c_str(), &a_val, 0.5f,
                        -180.0f, 180.0f, "%.1f\xC2\xB0");
                });
            changed |= ResetOnRightClick(a_val, a_defVal);   // before the arrows
            if (!typing) {
                changed |= Theme::GaugeStep(at, tw, h, id.c_str(),
                                            a_val, 1.0f, -180.0f, 180.0f);
            }
            ImGui::SameLine();
            if (typing) ImGui::TextDisabled("%s", Theme::GaugeInputHint());
            else RowNote(std::fabs(a_val - a_defVal) > 0.5f, "(%s %.0f\xC2\xB0)", a_defVal);
            return changed;
        }
    }

    void SetHooks(Hooks a_hooks)
    {
        g_hooks = std::move(a_hooks);
    }

    bool IsEditMode() { return g_editMode; }

    void ToggleEditMode()
    {
        g_editMode = !g_editMode;
        if (!g_editMode) {
            RevertSession();   // leaving EDIT without saving discards
            g_sel = nullptr;
            g_selKey.clear();
            IconCache::GetSingleton()->SetPin(nullptr);
        }
    }

    void Select(RE::TESBoundObject* a_obj, const std::string& a_key)
    {
        // ★Switching items DISCARDS unsaved work on the previous one — it used
        // to silently save it instead, which is the behaviour that made the
        // whole thing feel like it had no commit point.
        RevertSession();
        g_sel = a_obj;
        g_selKey = a_key;
        if (g_hooks.getEffective) g_cur = g_hooks.getEffective(a_obj);
        g_base = g_cur;
        g_stackApplied = g_cur.stack;   // ★see the Stack row
        g_baseOverride = g_hooks.hasOverride ? g_hooks.hasOverride(a_obj) : false;
        g_dirty = false;
        DefToPainter();
        // keep the selection's model loaded: rotation edits re-capture fast
        IconCache::GetSingleton()->SetPin(a_obj);
    }

    bool IsSelected(const std::string& a_key)
    {
        return g_editMode && !g_selKey.empty() && g_selKey == a_key;
    }

    bool HasUnsavedEdits() { return g_dirty && g_sel != nullptr; }
    bool DiscardNoteActive() { return ImGui::GetTime() < g_revertNote; }

    void OnMenuClosed()
    {
        RevertSession();   // ...and so does closing the whole menu
        g_editMode = false;
        g_sel = nullptr;
        g_selKey.clear();
        IconCache::GetSingleton()->SetPin(nullptr);
    }

    void DrawPanel()
    {
        if (!g_editMode) return;
        // (no debounce any more — nothing writes until SAVE)

        auto* wm = WinManager::GetSingleton();
        // ★Ask Theme directly. This used to read the cell size back out, on
        // the assumption that CellPx == 48 x Scale() — true until the board
        // got a scale of its own, after which the editor panel would have
        // shrunk along with the grid instead of with the text.
        const float s = Theme::Scale();
        // tall enough that the body never needs a scrollbar (v9.2 feedback;
        // v10.6 merged the painter+bag row, so the panel shrank).
        // +2x frame inset for tornFrame skins (breathing room)
        // ★+ the title's top pad, paid for in the height as every window that
        // takes it must (Theme::TitleTopPad).
        const float topPad = Theme::TitleTopPad();
        // ★★★THE HEIGHT COMES FROM THE CONTENT NOW, not from a number that
        // hoped to be big enough. 666 was picked as "tall enough that the body
        // never needs a scrollbar", and that held right up until something made
        // the content taller than the guess -- a low resolution, a longer
        // translation, and (next) a font-size setting. When it stopped holding
        // there was no recovery: the window kept its guessed height, the child
        // scrolled inside it, and the scrollbar ate the width the rows were
        // already using. Reported as "the edit window is unusable once a
        // scrollbar appears".
        //
        // Measured from the previous frame and clamped to the screen, which is
        // exactly what the settings window has always done -- and the reason
        // THAT window never had this bug. Past the clamp the child scrolls, and
        // the window widens by the scrollbar so the rows keep their room.
        // The first frame has nothing to measure, so it falls back to the old
        // fixed height and snaps one frame later.
        static float s_wantH = 0.0f;
        const float maxH = ImGui::GetIO().DisplaySize.y - 80.0f * s;
        const bool clamped = s_wantH > 0.0f && s_wantH > maxH;
        const float winH = s_wantH > 0.0f
                             ? (std::min)(s_wantH, maxH)
                             : 666.0f * s + 2.0f * Theme::FrameInsetY();
        const ImVec2 size(342.0f * s + 2.0f * Theme::FrameInsetX() +
                              (clamped ? ImGui::GetStyle().ScrollbarSize : 0.0f),
                          winH);   // +Stack row (G3)
        ImVec2 defPos(60.0f, 120.0f);
        if (auto* mw = wm->Find("main")) {
            defPos = ImVec2(mw->pos.x - size.x - 8.0f, mw->pos.y);
        }
        wm->ApplyNext("editor", defPos, size, WinManager::Anchor::kTopLeft, topPad);

        // Bake the torn-frame inset into the window padding: every line and
        // the body child then respect the ragged edges on BOTH sides (only
        // the first line honoured the TitleBar origin before — the right
        // margin vanished under the tear on skins 3/4).
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(Theme::PadX() + Theme::FrameInsetX(),
                   Theme::PadY() + Theme::FrameInsetY()));
        ImGui::Begin("##fablerim_editor", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        wm->TitleBar("editor", Lang::SentenceCase(Lang::T(Lang::Str::Edit)).c_str());

        // scrollable body child — the titlebar pins the content start to a
        // SCREEN position each frame, so window-level scrolling moves the
        // scrollbar but never the content; the child scrolls internally
        // ★where the body begins, so the measurement below can say how tall the
        // WINDOW wants to be rather than just the body
        const float childTop = ImGui::GetCursorPosY();
        ImGui::BeginChild("##editor_body", ImVec2(0.0f, 0.0f));

        if (!g_sel) {
            ImGui::TextDisabled("%s", Lang::T(Lang::Str::SelectHint));
            // ★s_wantH is deliberately NOT updated here. With nothing selected
            // the body is one line, and measuring that would shrink the window
            // to a sliver and snap it back the moment an item is clicked. The
            // last real measurement is the honest size for an empty editor.
            ImGui::EndChild();
            ImGui::End();
            ImGui::PopStyleVar();   // WindowPadding (torn-frame inset)
            return;
        }

        // (the category default is no longer read here — every row now compares
        //  against the session baseline; "Reset to default" fetches it itself)

        // ---- ① selection info (no thumbnail — the grid tile IS the preview) ----
        ImGui::BeginGroup();
        // ★The unsaved marker rides the NAME, not a corner of the panel: the
        // name is what the player is looking at while editing, and losing work
        // by closing the window is the failure this whole model introduces.
        if (g_dirty) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Val()),
                "%s  \xE2\x97\x8F %s", g_sel->GetName(), Lang::T(Lang::Str::EditUnsaved));
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Val()),
                "%s", g_sel->GetName());
        }
        ImGui::TextDisabled("%s", g_selKey.c_str());
        if (g_hooks.categoryName) {
            ImGui::TextDisabled("category: %s%s", g_hooks.categoryName(g_sel).c_str(),
                g_hooks.hasOverride && g_hooks.hasOverride(g_sel) ? "  [override]" : "");
        }
        ImGui::EndGroup();
        ImGui::Separator();

        // ---- ② rotation / ③ scale (applied live every frame) ----
        bool chOrient = false;   // rotation/scale: recapture only, no reflow
        bool chLayout = false;   // footprint/bag: grid placement changes
        // GI52: which pair is being edited follows the ICON STYLE. Three
        // angles orient a 3D model and mean nothing to a flat drawing; the
        // drawn icons get one angle and their own zoom, stored in their own
        // fields so switching styles back never finds the other's numbers.
        const bool drawnStyle = IconCache::GetSingleton()->FlatStyle();
        if (drawnStyle) {
            // ★What to name a custom drawing. The whole customisation story is
            // "drop a PNG in the folder", which only works if the name is
            // knowable — and it is not derivable by looking at the item (the
            // key comes from classification rules, and the per-item name is a
            // FormID spelling). So the editor just says it, for both levels,
            // and copies it. Without this row the feature needs a tool again.
            {
                const float S2 = Theme::Scale();
                // ★These two do NOT share the label column the sliders use. A
                // plugin name rides in the per-item one ("Grid Inventory.esp_
                // 0x000824.png"), so the value is routinely three times the
                // width of a "1.00" — putting it at the slider column ran it
                // straight through the label and off the panel. Own line,
                // indented, wrapped: length stops mattering.
                //
                // click-to-copy on a plain text line: hover/click resolve off
                // the last item's rect, so each line handles its own. (A
                // BeginGroup wrapper would put the tooltip on the group, whose
                // hover semantics are the one thing here worth not relying on.)
                // ★shown and COPIED are not the same string. The line has to
                // say "item\" or nobody knows which folder it goes in, but the
                // clipboard feeds a Save As box, where a stray folder prefix
                // makes the save land somewhere else or fail outright.
                auto copyLine = [&](const ImVec4& a_col, const std::string& a_shown,
                                    const std::string& a_copy) {
                    if (a_shown.empty()) return;
                    ImGui::PushStyleColor(ImGuiCol_Text, a_col);
                    ImGui::PushTextWrapPos(0.0f);   // wrap at the panel edge
                    ImGui::TextUnformatted(a_shown.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", Lang::T(Lang::Str::IconKeyHint));
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            ImGui::SetClipboardText(a_copy.c_str());
                        }
                    }
                };
                const std::string itemFile = Fallback::ItemFileName(g_sel);
                const std::string keyFile = Fallback::KeyFileName(g_sel);
                ImGui::TextColored(Theme::S().inkDim, "%s", Lang::T(Lang::Str::IconKeyLabel));
                ImGui::Indent(10.0f * S2);
                copyLine(ImGui::ColorConvertU32ToFloat4(Theme::Val()),
                    itemFile.empty() ? std::string{} : "item\\" + itemFile, itemFile);
                copyLine(Theme::S().inkDim, keyFile, keyFile);
                ImGui::Unindent(10.0f * S2);
                ImGui::Spacing();
            }
        }
        // ---- what the slider block below shows -----------------------------
        //
        // ★★A TAB, not more rows. The panel already carried six sliders and
        // adding two would have made eight — which is why the first attempt put
        // the offset on a drag over the footprint board instead. That was the
        // wrong trade: a drag cannot land on 0.30, and the sliders it was meant
        // to spare stayed on screen anyway. Swapping the block keeps the panel
        // the same height AND keeps every value typeable.
        {
            const float ST = Theme::Scale();
            const Lang::Str kT[2] = { Lang::Str::FootRotate, Lang::Str::FootMove };
            for (int i = 0; i < 2; ++i) {
                if (i) ImGui::SameLine(0.0f, 4.0f * ST);
                const bool on = g_footTab == i;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::BtnOn(1.15f));
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
                }
                if (Sfx::Button(Lang::T(kT[i]))) g_footTab = i;
                if (on) ImGui::PopStyleColor(3);
            }
        }
        if (g_footTab == 0) {
            if (drawnStyle) {
                chOrient |= AngleSlider("ROT", g_cur.frot, g_base.frot);
            } else {
                chOrient |= AngleSlider("RX", g_cur.rx, g_base.rx);
                chOrient |= AngleSlider("RY", g_cur.ry, g_base.ry);
                chOrient |= AngleSlider("RZ", g_cur.rz, g_base.rz);
            }
        } else {
            // ★Cells, both axes, both styles (ItemDef.h). Free-form footprints
            // are what made these necessary: an L wants its haft down the left
            // column and no automatic centre can reach that.
            chOrient |= OffsetSlider("X", g_cur.fx, g_base.fx);
            chOrient |= OffsetSlider("Y", g_cur.fy, g_base.fy);
        }
        {
            const float S0 = Theme::Scale();
            float&      zoom = drawnStyle ? g_cur.fscale : g_cur.scale;
            const float zdef = drawnStyle ? g_base.fscale : g_base.scale;
            const float zmin = drawnStyle ? 0.2f : 0.05f;
            const float zmax = drawnStyle ? 4.0f : 20.0f;
            RowLabel("Scale");
            ImGui::SameLine(kLabelW * S0);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float  h = ImGui::GetFrameHeight();
            const bool zTyping = Theme::GaugeEditing("##Scale");
            const float tw = TrackW(S0);
            if (GaugeRow(at, tw,
                    (zoom - zmin) / (drawnStyle ? 3.8f : 1.95f), false,
                    "##Scale", "%.2f", zoom,
                    [&] {
                        return ImGui::DragFloat("##Scale", &zoom, 0.01f,
                            zmin, zmax, "%.2f");
                    })) {
                chOrient = true;
            }
            if (ResetOnRightClick(zoom, zdef)) chOrient = true;   // before arrows
            if (!zTyping && Theme::GaugeStep(at, tw, h, "##Scale",
                                             zoom, 0.01f, zmin, zmax)) {
                chOrient = true;
            }
            ImGui::SameLine();
            if (zTyping) ImGui::TextDisabled("%s", Theme::GaugeInputHint());
            else RowNote(std::fabs(zoom - zdef) > 0.005f, "(%s %.2f)", zdef);
        }
        // ---- capture light -------------------------------------------------
        // ★The scene has ONE lamp, so an item's brightness is decided by which
        // face it turns toward it. Rather than hunt for a rotation that is both
        // recognisable AND lit, move the lamp for this item. Only the 3D styles
        // use it — a drawn icon is flat art.
        // ★★These are offsets from wherever SETTINGS > ICONS > CAPTURE LIGHT
        // has aimed the rig, not from the shipped angle, so 0/0 means "whatever
        // the global says" and stays true after the global moves. Tune the
        // global first for the whole set; come here only for the items that
        // still read badly under it.
        if (!drawnStyle) {
            const float S3 = Theme::Scale();
            auto lightRow = [&](const char* a_label, float& a_val, float a_base,
                                float a_lo, float a_hi) {
                RowLabel(a_label);
                ImGui::SameLine(kLabelW * S3);
                const std::string id = std::string("##") + a_label;
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float  hh = ImGui::GetFrameHeight();
                const bool typing = Theme::GaugeEditing(id.c_str());
                const float tw = TrackW(S3);
                bool ch = GaugeRow(at, tw,
                    (a_val - a_lo) / (a_hi - a_lo), false, id.c_str(),
                    "%.0f\xC2\xB0", a_val,
                    [&] {
                        return ImGui::DragFloat(id.c_str(), &a_val, 0.5f,
                            a_lo, a_hi, "%.0f\xC2\xB0");
                    });
                ch |= ResetOnRightClick(a_val, a_base);   // before the arrows
                if (!typing) {
                    ch |= Theme::GaugeStep(at, tw, hh, id.c_str(),
                                           a_val, 1.0f, a_lo, a_hi);
                }
                ImGui::SameLine();
                if (typing) ImGui::TextDisabled("%s", Theme::GaugeInputHint());
                else RowNote(std::fabs(a_val - a_base) > 0.5f, "(%s %.0f\xC2\xB0)", a_base);
                return ch;
            };
            if (lightRow("Lgt X", g_cur.lightAz, g_base.lightAz, -180.0f, 180.0f)) {
                chOrient = true;
            }
            if (lightRow("Lgt Y", g_cur.lightEl, g_base.lightEl, -80.0f, 80.0f)) {
                chOrient = true;
            }
        }
        ImGui::Separator();

        // ---- ④+⑤ footprint painter (left) + bag column (right): ONE row,
        // caption under the painter — mockup edrow2 layout (v10.6) ----
        const float S = Theme::Scale();
        {
            // ★The painter grew from 6x6 to 8x8 but its BLOCK did not: 180px
            // was tuned against the bag column beside it, so the cell shrinks
            // to keep the same total. 22.5 * 8 == 30 * 6.
            constexpr float kPaintBlock = 180.0f;
            const float cell = kPaintBlock / static_cast<float>(kPaintN);
            auto* dl = ImGui::GetWindowDrawList();
            const float availW = ImGui::GetContentRegionAvail().x;
            const ImVec2 base = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##painter", ImVec2(kPaintBlock, kPaintBlock));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int hc = static_cast<int>((mouse.x - base.x) / cell);
            const int hr = static_cast<int>((mouse.y - base.y) / cell);
            const bool inCell = hovered && hc >= 0 && hc < kPaintN &&
                                hr >= 0 && hr < kPaintN;

            // ★Drag-paint, as it always was. An earlier pass put sprite
            // aiming on this board as a drag and demoted painting to a click;
            // that was the wrong trade (a drag cannot land on 0.30, and the
            // sliders it was meant to spare stayed anyway). Aiming is a tabbed
            // slider block now, and the board is only a board again.
            if (inCell && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_painting = true;
                g_paintValue = !g_paint[hr][hc];   // first cell decides paint vs erase
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_painting = false;
            if (g_painting && inCell && g_paint[hr][hc] != g_paintValue) {
                g_paint[hr][hc] = g_paintValue;
                PainterToDef();
                chLayout = true;
            }

            // mockup painter: ivory border + skin-specific fill (OATHVEIN
            // paints with the sel colour, the rest with the accent)
            const auto& skp = Theme::S();
            // ★★It used to borrow bagOpen, on the reasoning that a painted
            // footprint and an open bag both mean "this is the one". That held
            // while bagOpen was a tint OF the theme. It stopped holding when
            // the derived skins made bagOpen the theme's COMPLEMENT — the open
            // bag is one tile among many and has to shout, but the painter is
            // a whole block of them, and a block of the opposite hue turned
            // the only cyan thing on a copper window into what reads as a bug.
            // BtnOn is what this actually is: a cell toggled on, in the
            // theme's own ON colour, which every other toggle in the UI uses.
            const ImU32 onCol = skp.lightPanel    ? Theme::BtnOn(0.85f)
                              : skp.diamondLabels ? Theme::Col(skp.sel, 0.60f)
                                                  : Theme::Acc(0.55f);
            for (int r = 0; r < kPaintN; ++r) {
                for (int c = 0; c < kPaintN; ++c) {
                    const ImVec2 p0(base.x + c * cell, base.y + r * cell);
                    const ImVec2 p1(p0.x + cell - 2, p0.y + cell - 2);
                    // ★FrameRounding(), not the raw field — see TrackChrome.
                    const float pr = Theme::FrameRounding();
                    dl->AddRectFilled(p0, p1,
                        g_paint[r][c] ? onCol : IM_COL32(0, 0, 0, 90), pr);
                    dl->AddRect(p0, p1, Theme::Acc(0.30f), pr);
                }
            }

            // ---- the sprite, ON the board ----------------------------------
            //
            // ★★This is the whole point of the change. The footprint and the
            // icon are two things that must line up, and until now they were
            // never on screen together: the board showed the shape, the icon
            // lived out on the grid, and the offset was a number in between.
            // Aiming it meant editing a value, closing the panel, looking, and
            // coming back. Now the thing being aimed is under the cursor that
            // aims it.
            //
            // Laid out by the same rules the game uses (Grid's FitOf): centred
            // on the occupied box, sized by the shape's reach per axis. If the
            // two ever disagree, this preview is worthless -- so it is written
            // from the same three numbers rather than from a second guess.
            {
                int minC = kPaintN, maxC = -1, minR = kPaintN, maxR = -1;
                int maxRow = 0;
                int colN[kPaintN] = {};
                for (int r = 0; r < kPaintN; ++r) {
                    int rowN = 0;
                    for (int c = 0; c < kPaintN; ++c) {
                        if (!g_paint[r][c]) continue;
                        ++rowN; ++colN[c];
                        minC = (std::min)(minC, c); maxC = (std::max)(maxC, c);
                        minR = (std::min)(minR, r); maxR = (std::max)(maxR, r);
                    }
                    maxRow = (std::max)(maxRow, rowN);
                }
                if (maxC >= 0) {
                    int maxCol = 0;
                    for (const int v : colN) maxCol = (std::max)(maxCol, v);
                    const float bw = static_cast<float>(maxC - minC + 1);
                    const float bh = static_cast<float>(maxR - minR + 1);
                    const float fill = (std::min)(maxRow / bw, maxCol / bh);
                    if (const auto* ic = IconCache::GetSingleton()->Get(g_sel)) {
                        const float target = (std::max)(bw, bh) * cell * 0.95f *
                                             g_cur.scale * fill;
                        const float ms =
                            static_cast<float>((std::max)(ic->w, ic->h));
                        const float dw = ic->w / ms * target;
                        const float dh = ic->h / ms * target;
                        const ImVec2 ctr(
                            base.x + (minC + maxC + 1) * 0.5f * cell + g_cur.fx * cell,
                            base.y + (minR + maxR + 1) * 0.5f * cell + g_cur.fy * cell);
                        UIRoot::DrawItemIconRot(dl, ic->srv, ctr, ImVec2(dw, dh),
                            drawnStyle ? g_cur.frot : 0.0f);
                    }
                }
            }

            // caption BELOW the painter (mockup .pnote)
            ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + kPaintBlock + 6.0f));
            if (std::fabs(g_cur.fx) > 0.005f || std::fabs(g_cur.fy) > 0.005f) {
                ImGui::TextDisabled("%dx%d%s · %+.2f, %+.2f",
                    g_cur.w, g_cur.h, g_cur.shape.empty() ? "" : " (shape)",
                    g_cur.fx, g_cur.fy);
            } else {
                ImGui::TextDisabled("%s · %dx%d%s", Lang::T(Lang::Str::FootprintHint),
                    g_cur.w, g_cur.h, g_cur.shape.empty() ? "" : " (shape)");
            }
            const float leftBottom = ImGui::GetCursorScreenPos().y;

            // ---- right column: Bag / W / H ----
            // stretches to the avail edge — the window padding now carries
            // the torn inset, so this stops at the proper right margin
            const float colX = base.x + kPaintBlock + 16.0f;
            const float colW = (std::max)(90.0f, base.x + availW - colX);
            const float lblW = 22.0f * S;
            const float trackW = colW - lblW;
            const float rowH = ImGui::GetFrameHeight() + 9.0f * S;

            ImGui::SetCursorScreenPos(ImVec2(colX, base.y));
            // items whose right-click/consume/system flows would fight the
            // bag window toggle can never BE bags: weapons, consumables,
            // spell tomes, ammo, keys, lockpicks, jewelry, gold coins/pouch
            bool bagAllowed = !(g_sel->Is(RE::FormType::Weapon) ||
                g_sel->Is(RE::FormType::AlchemyItem) ||
                g_sel->Is(RE::FormType::Ingredient) ||
                g_sel->Is(RE::FormType::Scroll) ||
                g_sel->Is(RE::FormType::Ammo) ||
                g_sel->Is(RE::FormType::KeyMaster) ||
                g_sel->GetFormID() == 0x0000000A ||                // lockpick
                GoldCoins::IsCoinForm(g_sel->GetFormID()));        // coins + pouch
            if (bagAllowed) {
                if (const auto* book = g_sel->As<RE::TESObjectBOOK>();
                    book && book->TeachesSpell()) {
                    bagAllowed = false;   // spell tome: right-click = learn
                }
                if (const auto* armo = g_sel->As<RE::TESObjectARMO>()) {
                    using SB = RE::BGSBipedObjectForm::BipedObjectSlot;
                    if (Grid::IsRing(armo) || armo->HasPartOf(SB::kAmulet)) {
                        bagAllowed = false;   // jewelry
                    }
                }
            }
            if (!bagAllowed && g_cur.bag != 0) {   // sanitize stale overrides
                g_cur.bag = 0;
                chLayout = true;
            }
            bool isBag = g_cur.bag != 0;
            // themed checkbox: dark ground + accent hover (kills the stock blue)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Acc(0.08f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::Acc(0.14f));
            ImGui::PushStyleColor(ImGuiCol_CheckMark, Theme::ValVec());
            ImGui::BeginDisabled(!bagAllowed);
            if (ImGui::Checkbox(Lang::T(Lang::Str::Bag), &isBag)) {
                g_cur.bag = isBag ? 1 : 0;
                chLayout = true;
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(4);

            auto rightTrack = [&](const char* lbl, int& v, float y) {
                ImGui::SetCursorScreenPos(ImVec2(colX, y + 3.0f * S));
                RowLabel(lbl);
                // ★1..16, matching the def clamp (ui/ItemDef.h). At 1..10 this
                // slider was a QUIET editor: opening EDIT on a 10x14 bag pulled
                // it down to 10 and saved that, so simply looking at the bag
                // shrank it.
                constexpr int kBagMax = 16;
                const ImVec2 tp(colX + lblW, y);
                const std::string id = std::string("##bag") + lbl;
                const bool typing = Theme::GaugeEditing(id.c_str());
                bool ch3 = GaugeRowI(tp, trackW,
                    static_cast<float>(v - 1) / static_cast<float>(kBagMax - 1),
                    true, id.c_str(), "%d", v,
                    [&] { return ImGui::SliderInt(id.c_str(), &v, 1, kBagMax); });
                if (!typing) {
                    ch3 |= Theme::GaugeStepInt(tp, trackW, ImGui::GetFrameHeight(),
                                               id.c_str(), v, 1, 1, kBagMax);
                }
                return ch3;
            };
            chLayout |= rightTrack("W", g_cur.bw, base.y + rowH);
            chLayout |= rightTrack("H", g_cur.bh, base.y + rowH * 2.0f);

            // continue below whichever column is taller
            const float rightBottom = base.y + rowH * 2.0f + ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(
                ImVec2(base.x, (std::max)(leftBottom, rightBottom) + 4.0f));
        }

        // ---- G3: per-item stack cap (0 = category default; gear is always 1) ----
        {
            const bool stackable = !(g_sel->Is(RE::FormType::Weapon) ||
                                     g_sel->Is(RE::FormType::Armor));
            if (!stackable && g_cur.stack != 0) {   // sanitize stale overrides
                g_cur.stack = 0;
                chLayout = true;
            }
            ImGui::BeginDisabled(!stackable);
            const float S0 = Theme::Scale();
            RowLabel("Stack");
            ImGui::SameLine(kLabelW * S0);
            // ★The only row whose FORMAT depends on the value ("auto" at 0,
            // "%d" above it). Arguments are evaluated before the widget runs,
            // so the frame that crosses 0 prints the old wording once — and
            // since "auto" takes no argument, printing it with a number is
            // harmless. One frame mid-drag is not visible; a wrong number
            // would have been.
            const char* stackFmt = g_cur.stack > 0 ? "%d" : "auto";
            const ImVec2 stAt = ImGui::GetCursorScreenPos();
            const float  stH = ImGui::GetFrameHeight();
            const bool sTyping = Theme::GaugeEditing("##StackCap");
            const float stW = TrackW(S0);
            if (GaugeRowI(stAt, stW,
                    g_cur.stack > 0 ? g_cur.stack / 100.0f : 0.0f, false,
                    "##StackCap", stackFmt, g_cur.stack,
                    [&] {
                        return ImGui::DragInt("##StackCap", &g_cur.stack, 0.25f,
                            0, 999, stackFmt);
                    })) {
                // deliberately no chLayout -- see g_stackApplied
            }
            if (!sTyping) {
                // the steppers move the number; the apply is below
                (void)Theme::GaugeStepInt(stAt, stW, stH,
                                          "##StackCap", g_cur.stack, 1, 0, 999);
            }
            // ★...and it is NOT applied here. It lands at Save (SaveSession).
            if (g_cur.stack != g_stackApplied) MarkDirty();
            ImGui::SameLine();
            if (g_cur.stack > 0) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Chrome(0.85f)), "(override)");
            } else {
                ImGui::TextDisabled("(default)");
            }
            ImGui::EndDisabled();
        }
        ImGui::Separator();

        if ((chOrient || chLayout) && g_hooks.setOverride) {
            g_hooks.setOverride(g_sel, LiveDef(), false);   // live apply, zero IO
            if (chLayout) Grid::RequestRebuild();       // footprint: reflow
            else          Grid::RefreshDefs();          // orientation: recapture only
            MarkDirty();                                // ini write, debounced
        }
        ImGui::Separator();

        // ---- ⑥ actions ----
        // ★SAVE leads, and it is the only control that writes. Lit while there
        // is something to write so the unsaved state is visible from the one
        // place that resolves it.
        // ★★Latch the state BEFORE drawing. The button's own action clears
        // g_dirty, so testing it again after the call took the other branch and
        // left two PushStyleColor unpopped for that frame — ImGui repairs the
        // stack at end-of-frame, which is why it showed up as chrome flashing
        // once per click instead of as a hard failure.
        // Any push/pop pair that straddles a widget has to read a LATCHED copy,
        // never the live state the widget can change.
        const bool wasDirty = g_dirty;
        if (wasDirty) {
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
        }
        ImGui::BeginDisabled(!wasDirty);
        if (Sfx::Button(Lang::T(Lang::Str::EditSave))) SaveSession();
        ImGui::EndDisabled();
        if (wasDirty) ImGui::PopStyleColor(2);
        ImGui::SameLine();

        // ★Reset asks a different question from the right-click revert: it
        // throws away this item's tuning ENTIRELY and goes back to whatever
        // the category says. It stays a session edit — nothing is written
        // until Save, so it can itself be undone by leaving without saving.
        if (Sfx::Button(Lang::T(Lang::Str::ResetDefault))) {
            const FullDef d = g_hooks.getDefault ? g_hooks.getDefault(g_sel) : FullDef{};
            g_cur = d;
            g_stackApplied = g_cur.stack;
            if (g_hooks.setOverride) g_hooks.setOverride(g_sel, g_cur, false);
            // (Reset is not an edit in progress -- it IS the value)
            DefToPainter();
            Grid::RequestRebuild();
            MarkDirty();
        }
        ImGui::SameLine();
        // ★Only a SAVED item may set the category default. Pushing a value
        // that is not yet committed out to every item in its category is the
        // one action here that reaches past the item in front of you.
        // same latching rule — this one does not currently change g_dirty, but
        // pairing a Begin/End against live state is the shape of the bug above
        ImGui::BeginDisabled(wasDirty);
        if (Sfx::Button(Lang::T(Lang::Str::SaveCategory))) {
            if (g_hooks.saveAsCategory) g_hooks.saveAsCategory(g_sel, g_cur);
            Grid::RequestRebuild();
        }
        ImGui::EndDisabled();
        if (wasDirty && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            UIRoot::NoteHoverHint(Lang::T(Lang::Str::EditUnsaved));
        }

        // property clipboard — same semantics as the offline tool: rotation,
        // scale and tiles travel; bag/stack stay per-item
        static bool    s_clipSet = false;
        static FullDef s_clip;
        if (Sfx::Button(Lang::T(Lang::Str::CopyProps))) {
            s_clip = g_cur;
            s_clipSet = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!s_clipSet);
        if (Sfx::Button(Lang::T(Lang::Str::PasteProps))) {
            g_cur.w = s_clip.w;
            g_cur.h = s_clip.h;
            g_cur.shape = s_clip.shape;
            g_cur.rx = s_clip.rx;
            g_cur.ry = s_clip.ry;
            g_cur.rz = s_clip.rz;
            g_cur.scale = s_clip.scale;
            g_cur.fscale = s_clip.fscale;   // GI52: the drawn pair travels too
            g_cur.frot = s_clip.frot;
            g_cur.fx = s_clip.fx;
            // the lamp travels with the orientation it was tuned against —
            // pasting a rotation without its light would hand the copy a pose
            // that was only legible under the angle left behind
            g_cur.lightAz = s_clip.lightAz;
            g_cur.lightEl = s_clip.lightEl;
            DefToPainter();
            if (g_hooks.setOverride) g_hooks.setOverride(g_sel, LiveDef(), false);
            Grid::RequestRebuild();
            MarkDirty();
        }
        ImGui::EndDisabled();
        // ★The height the window will be asked for NEXT frame. Taken before
        // EndChild, where the cursor still sits at the bottom of the content.
        const float bodyH = ImGui::GetCursorPosY() + 4.0f * s;   // bottom margin
        ImGui::EndChild();
        s_wantH = childTop + bodyH + 8.0f + Theme::FrameInsetY();
        ImGui::End();
        ImGui::PopStyleVar();   // WindowPadding (torn-frame inset)
    }
}
