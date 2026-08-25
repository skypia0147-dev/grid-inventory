#include "ui/Theme.h"

#include <SKSE/SKSE.h>

#include "ui/IconCache.h"   // IconStyleSlot(): which style's values are live
#include "ui/Lang.h"        // GaugeInputHint(): the typing note is translated

#include <d3d11.h>   // ReloadInkArt releases the sheet's SRV/texture

#include <imgui_internal.h>   // ImTextCharFromUtf8 (TextInkCentered)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace FUI::Theme
{
    namespace
    {
        constexpr ImVec4 Rgba(int r, int g, int b, float a = 1.0f)
        {
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        // ── light-panel chrome fallbacks ───────────────────────────────────
        // What a light-panel skin gets when it names none of the lp* tokens.
        // These are SIMPLE's own values, which is why every theme derived from
        // it must name its own — silence here means "inherit blue".
        //
        // ★A `kDarkInk` used to sit alongside these, for labels and body text.
        // It is gone: Chrome() and ValVec() read the SKIN's ink now, so a
        // second dark ink would be a copy of a decision each skin already
        // made. On SIMPLE that ink is white with a black outline (21:1 over
        // any panel); on parchment it is the brown the body already uses.
        constexpr ImVec4 kBtnOnFace = Rgba(143, 211, 222); // active button face
        constexpr ImVec4 kBtnOnInk  = Rgba(14, 48, 56);    // ink ON that face
        constexpr ImVec4 kRuleInk   = Rgba(13, 32, 46);    // rules, drawn at .70

        // "this skin names it, else use the built-in" — the light-panel
        // palette defaults to SIMPLE's colours (alpha 0 = not named)
        [[nodiscard]] const ImVec4& LP(const ImVec4& a_skin, const ImVec4& a_def)
        {
            return a_skin.w > 0.0f ? a_skin : a_def;
        }

        // v9 mockup skin table (+ 5/6: light CREAM torn rebakes, dark ink)
        // ★No size on purpose: the SIMPLE family gained five colour variants in
        // 1.0.5 and a literal here is one more place to forget. SkinCount()
        // reads std::size of this table, and everything else reads SkinCount().
        const Skin kSkins[] = {
            // ★★ORDER IS PRESENTATION ONLY. Nothing outside this array may
            // identify a skin by its position: a saved setting names it
            // (WinManager writes !skin3/!disp[name]/!shad[name]), and
            // LightItemShadow matches on the name for the same reason. Two
            // reorders have already happened -- "Fable Amber" removed, then
            // the ink pair moved to the front and the Glass pair dropped --
            // and each one silently handed every skin's saved icon gain and
            // shadow to its neighbour until the file stopped counting.
            // ★No number in the comments either. They used to carry one and it
            // was already wrong: "Simple" sat at 14 wearing a "// 6".
            // ★Named by CHROME FAMILY first, colour second -- Sumi / Fable /
            // Parchment / Simple. The frame is what the eye picks out; the
            // accent colour is the sub-choice inside it.
            // ★Removed at the author's request, and the reason each is gone:
            //   Fable Amber   -- identical colours to Parchment Amber, only the
            //                    frame differed, so the look still exists.
            //   Glass Dark    -- the translucent pair. The `translucent` token
            //   Glass Clear      and its machinery STAY: they cost nothing
            //                    unused and a future glass skin needs no new
            //                    code. Saved settings migrate to Simple
            //                    Charcoal, the nearest dark board.
            {   // Sumi Parchment — 먹 · 낡은 양피지
            // ★The SAME skin with a darker sheet. Only two values move: the
            // paper and the shade that has to keep its distance from it. Every
            // sprite is white+alpha and takes its colour from the tokens, so
            // the two share one art set entirely.
                "Sumi Parchment",
                Rgba(35, 29, 21, 0.19f),
                Rgba(83, 49, 42),
                Rgba(35, 29, 21),
                Rgba(35, 29, 21, 0.58f),
                // sheet measures 225.6 / 208.5 / 173.4 — this asks for barely
                // under it, both to leave the tint headroom and because this
                // one was never the problem.
                Rgba(222, 205, 171),                    // winBg — aged sheet
                Rgba(35, 29, 21, 0.30f),
                Rgba(35, 29, 21, 0.34f),                // shade — +0.04, see above
                Rgba(173, 127, 119),
                Rgba(173, 127, 119),
                4.0f, 3.0f,
                false, false, false, false,
                false,
                false, false,
                true,
                false,
                Rgba(35, 29, 21, 0.06f),
                {}, {},
                Rgba(173, 127, 119, 0.55f),
                Rgba(83, 49, 42),
                24.0f,
                Rgba(203, 186, 152),
                Rgba(214, 199, 168),
                Rgba(186, 168, 132),
                Rgba(35, 29, 21),
                Rgba(173, 127, 119),
                Rgba(247, 242, 232),
                Rgba(35, 29, 21),
                "paper_parchment.png",
            },
            {   // Sumi — 먹 · 종이 위의 붓
            // ★★Every value here was MEASURED off the reference design, not
            // picked. The three colours are all there are -- paper, ink, clay
            // -- and everything else is one of them at an alpha.
            // ★shade: the wash behind a worn item measured #ADA9A1, and
            // reversing that through paper and ink gave 0.346 / 0.349 / 0.352
            // per channel. It is not a grey someone liked; it is ink at a
            // third. 0.37 rather than 0.35 because this paper sits 16 luma
            // below the reference sheet and the mark has to keep its distance.
                "Sumi",
                Rgba(35, 29, 21, 0.19f),                // acc — rules, hairlines
                // ★★hi is NOT sel. The clay accent measures 2.8:1 on this
                // paper, which is a SURFACE colour -- a tab fill, a button, a
                // title stroke. Put it on a badge NUMBER and the number cannot
                // be read. The reference used a far deeper brick for anything
                // that had to be legible, and so does this.
                Rgba(83, 49, 42),                       // hi
                Rgba(35, 29, 21),                       // ink
                Rgba(35, 29, 21, 0.58f),                // inkDim
                // ★winBg is the paper ITSELF now — the colour the sheet is
                // tinted to read as (Theme::PaperTint). paper_sumi.png measures
                // 237.4 / 227.2 / 206.8, luma 227.9, and beside the parchment
                // sheet that was 8.7% brighter with 40% less colour in it: two
                // axes moving the same way, which is why it read as far more
                // than 8.7%. Pulled to luma 215.6 while keeping R-B at 31, so
                // the pair still says white paper / aged paper without the
                // white one going incandescent on a bright exterior.
                Rgba(225, 215, 194),                    // winBg — paper
                Rgba(35, 29, 21, 0.30f),                // glyph
                Rgba(35, 29, 21, 0.30f),                // shade
                Rgba(173, 127, 119),                    // sel — clay
                Rgba(173, 127, 119),                    // filled
                4.0f, 3.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // ★NOT tornFrame: the
                                                        // edge is a brush mark,
                                                        // not a tear
                false, false,                           // not translucent, no bevel
                true,                                   // lightPanel
                false,                                  // engravedCells
                Rgba(35, 29, 21, 0.06f),                // cellBg — a seat
                {}, {},                                 // groove / btnFace
                Rgba(173, 127, 119, 0.55f),             // bagOpen — clay, = sel
                Rgba(83, 49, 42),                       // goldNum — deep, = hi
                24.0f,                                  // titleSize
                Rgba(214, 200, 172),                    // lpBtn
                Rgba(224, 212, 188),                    // lpBtnHov
                Rgba(198, 182, 150),                    // lpBtnAct
                Rgba(35, 29, 21),                       // lpBorder
                Rgba(173, 127, 119),                    // lpBtnOnFace — clay stamp
                Rgba(247, 242, 232),                    // lpBtnOnInk
                Rgba(35, 29, 21),                       // lpRule
                "paper_sumi.png",                       // paper
            },
            {   // Fable Crimson (v10.4) — 순흑 워밍 패널 + 코너-페이드
                // 테두리 + 상단 크림슨 스트립 + 글로우 타이틀 + ◇ 크림슨 라벨
                "Fable Crimson",
                Rgba(230, 226, 216), Rgba(245, 242, 234), Rgba(220, 216, 206),
                Rgba(220, 216, 206, 0.45f), Rgba(32, 32, 32),   // 목업 #202020
                Rgba(236, 232, 222), Rgba(0, 0, 0, 0.5f),
                Rgba(168, 64, 47), Rgba(230, 226, 216),
                0.0f, 5.0f,
                true, true, true, true,   // cornerFade / topStrip / titleGlow / diamondLabels
            },
            {   // Parchment Amber — REAL parchment: a pale sheet in a torn
                // frame, dark ink on it.
                // ★★The frame was always parchment; the panel behind it was
                // near-black, which read as a parchment mount with black paper
                // in it. Warming the panel helped but never made it PAPER —
                // paper is light, and everything written on light paper is
                // dark. So this skin flips: lightPanel and dark ink.
                // ★Nothing here is new machinery. SIMPLE already carries the
                // whole light-panel grammar; it simply had its blues compiled
                // in. Those are skin tokens now (lp*), and this is the second
                // skin to use them.
                "Parchment Amber",
                Rgba(110, 85, 53),                      // acc    lines, hairline grid
                Rgba(122, 90, 18),                      // hi     (unused on a light panel)
                Rgba(58, 46, 30),                       // ink    the writing — dark
                Rgba(58, 46, 30, 0.70f),                // inkDim
                // ★winBg finally reaches the screen. It never did on a torn
                // skin — the texture was the window and this value was dead.
                // ★★Every earlier value here was tuned against a ReShade-
                // tinted screen. With the preset off the sheet measured
                // #998D76 — the specified colour exactly, and 15% saturation,
                // which is grey card, not paper. The post-process had been
                // adding the warmth AND +23% brightness, so each "too bright"
                // correction darkened a colour that was never the problem.
                // This is the material colour: same hue, 29% saturation, and
                // light enough that ink on it reads as ink (6.1:1).
                // ★-6% off the first cream on sight. Everything below that is
                // derived from the sheet moves WITH it (see bagOpen and the
                // button family) so the relationships stay where they were
                // chosen; only the whole thing sits a step lower.
                Rgba(189, 174, 147),                    // winBg — cream parchment
                Rgba(120, 95, 60),                      // glyph  slot silhouettes
                Rgba(120, 95, 60, 0.30f),               // shade  occupied cell
                Rgba(150, 110, 40),                     // sel
                Rgba(122, 90, 18),                      // filled
                4.0f, 3.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                true,                                   // tornFrame
                false, false,                           // not translucent, no bevelChrome
                true,                                   // ★lightPanel
                false,                                  // hairline grid, not carved
                Rgba(120, 95, 60, 0.13f),               // cellBg — a seat for the item
                {}, {},                                 // groove/btnFace
                // ★bagOpen has to be OUT of family to work — it is the one
                // mark that says "this tile is the bag you have open", and a
                // warm brown on warm paper says nothing (it also collides
                // with `shade`, which already means "occupied").
                // ★★Out of family is not licence to be loud. This is painted
                // OVER `shade`, not over the sheet, and ink blue landed the
                // tile 42 luma below its neighbours while ALSO inverting the
                // hue — the tile read as a hole punched in the board.
                // Brightness is what shouts; hue is what distinguishes. Sage
                // sits at the luma of an ordinary occupied tile (+9) and
                // turns only the hue, which keeps the answer and drops the
                // shouting.
                Rgba(133, 154, 118, 0.85f),             // bagOpen — sage, iso-luminant
                Rgba(92, 64, 8),                        // goldNum — darker: the
                                                        // sheet is pale, so money
                                                        // reads by being DEEP
                24.0f,                                  // titleSize
                // light-panel palette: darker than the sheet, same family
                // ★These used to be LIGHTER than the sheet — the line above
                // described an intention, not the values — which only passed
                // unnoticed while the "sheet" was a dark card. On real paper a
                // control has to read as something pressed INTO it, so the
                // whole family moves below the sheet and the comment is now
                // true. Keep them in this order: btn < hov, act < btn.
                Rgba(162, 141, 102),                    // lpBtn
                Rgba(171, 152, 117),                    // lpBtnHov
                Rgba(145, 124,  84),                    // lpBtnAct
                Rgba(110, 85, 53),                      // lpBorder
                Rgba(140, 110, 60),                     // lpBtnOnFace — an inked stamp
                Rgba(245, 235, 216),                    // lpBtnOnInk  — paper on ink
                Rgba(90, 70, 45),                       // lpRule
            },
            {   // Parchment Crimson — 크림슨 + 찢긴 프레임 + 글로우 타이틀
                // + ◇ 크림슨 라벨. (텍스처 시절의 소프트 글로우 변형은 사라짐 —
                // 찢김은 이제 그려지고 변형이 하나뿐이다.)
                // ★winBg: this used to be the SAME value as Parchment Amber,
                // which is why the crimson strip sank into it. Sending the
                // amber skin warm and this one cool separates them and buys
                // the accent something to sit against — red reads reddest
                // next to its opposite. #171A1C is warmth −5: still black to
                // the eye, but the strip and the ◇ labels now carry the only
                // colour in the window.
                "Parchment Crimson",
                Rgba(230, 226, 216), Rgba(245, 242, 234), Rgba(220, 216, 206),
                Rgba(220, 216, 206, 0.45f), Rgba(23, 26, 28),
                Rgba(236, 232, 222), Rgba(0, 0, 0, 0.5f),
                Rgba(168, 64, 47), Rgba(230, 226, 216),
                0.0f, 5.0f,
                true, false, true, true,   // cornerFade + titleGlow + diamondLabels
                true,                      // tornFrame
            },
            {   // Simple Charcoal — 숯 · 진짜 중성 차콜 (신규)
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Charcoal",
                Rgba(17, 17, 20),                      // borders/ink
                Rgba(63, 65, 66),                      // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(202, 203, 204, 0.8f),             // inkDim
                // ★1.0.5 (6-4): the panel came DOWN, from #4A4C4E. Charcoal's
                // panel used to sit ABOVE its cells, which fenced the occupied
                // colour in from both sides — below it lay the empty cell,
                // above it the panel, and every candidate in between landed on
                // one of them. Nothing worked until the fence moved.
                Rgba(74, 76, 78, 0.68f),               // winBg (translucent board)
                Rgba(44, 46, 47),                      // glyph
                Rgba(13, 15, 15, 0.8f),                // shade (occupied cell)
                Rgba(81, 83, 85),                      // sel
                Rgba(81, 83, 85),                      // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(27, 29, 29, 0.75f),               // cellBg
                Rgba(20, 21, 22, 0.85f),               // cellGroove
                Rgba(30, 32, 33),                      // btnFace
                Rgba(161, 123, 99, 0.5f),              // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(41, 43, 44),                      // lpBtn
                Rgba(58, 60, 61),                      // lpBtnHov
                Rgba(29, 30, 31),                      // lpBtnAct
                Rgba(24, 25, 27),                      // lpBorder
                Rgba(150, 153, 154),                   // lpBtnOnFace — the ON state
                Rgba(8, 10, 10),                       // lpBtnOnInk
                Rgba(3, 4, 5),                         // lpRule
            },
            {   // Simple Graphite — 먹빛 · 청회색
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Graphite",
                Rgba(33, 30, 47),                      // borders/ink
                Rgba(83, 96, 107),                     // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(213, 218, 222, 0.8f),             // inkDim
                Rgba(95, 107, 121, 0.68f),             // winBg (translucent board)
                Rgba(61, 74, 81),                      // glyph
                Rgba(23, 30, 33),                      // shade (occupied cell)
                Rgba(100, 116, 127),                   // sel
                Rgba(100, 116, 127),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(41, 51, 57, 0.75f),               // cellBg
                Rgba(31, 39, 44, 0.85f),               // cellGroove
                Rgba(46, 54, 65),                      // btnFace
                Rgba(177, 143, 114, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(60, 69, 79),                      // lpBtn
                Rgba(79, 90, 100),                     // lpBtnHov
                Rgba(45, 52, 61),                      // lpBtnAct
                Rgba(38, 45, 54),                      // lpBorder
                Rgba(161, 180, 187),                   // lpBtnOnFace — the ON state
                Rgba(16, 24, 27),                      // lpBtnOnInk
                Rgba(10, 14, 20),                      // lpRule
            },
            {   // Simple Silver — 은 · 거의 무채색
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Silver",
                Rgba(70, 70, 77),                      // borders/ink
                Rgba(137, 143, 147),                   // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(231, 233, 234, 0.8f),             // inkDim
                Rgba(148, 153, 158, 0.68f),            // winBg (translucent board)
                Rgba(116, 121, 124),                   // glyph
                Rgba(62, 65, 66),                      // shade (occupied cell)
                Rgba(153, 160, 164),                   // sel
                Rgba(153, 160, 164),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(91, 95, 97, 0.75f),               // cellBg
                Rgba(76, 79, 82, 0.85f),               // cellGroove
                Rgba(96, 100, 104),                    // btnFace
                Rgba(203, 163, 137, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(113, 117, 121),                   // lpBtn
                Rgba(133, 138, 142),                   // lpBtnHov
                Rgba(94, 98, 101),                     // lpBtnAct
                Rgba(85, 88, 92),                      // lpBorder
                Rgba(200, 208, 210),                   // lpBtnOnFace — the ON state
                Rgba(51, 54, 55),                      // lpBtnOnInk
                Rgba(37, 38, 40),                      // lpRule
            },
            {   // Simple Pine — 소나무 · 짙은 침엽 (신규)
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Pine",
                Rgba(1, 41, 41),                       // borders/ink
                Rgba(70, 108, 83),                     // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(209, 223, 213, 0.8f),             // inkDim
                Rgba(71, 122, 97, 0.68f),              // winBg (translucent board)
                Rgba(64, 83, 57),                      // glyph
                Rgba(24, 35, 22),                      // shade (occupied cell)
                Rgba(93, 128, 97),                     // sel
                Rgba(93, 128, 97),                     // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(40, 58, 40, 0.75f),               // cellBg
                Rgba(27, 46, 31, 0.85f),               // cellGroove
                Rgba(20, 66, 50),                      // btnFace
                Rgba(174, 142, 175, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(48, 81, 62),                      // lpBtn
                Rgba(67, 102, 80),                     // lpBtnHov
                Rgba(32, 62, 47),                      // lpBtnAct
                Rgba(16, 55, 42),                      // lpBorder
                Rgba(170, 186, 151),                   // lpBtnOnFace — the ON state
                Rgba(17, 28, 12),                      // lpBtnOnInk
                Rgba(0, 19, 14),                       // lpRule
            },
            {   // Simple Forest — 상록
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Forest",
                Rgba(0, 67, 62),                       // borders/ink
                Rgba(104, 139, 104),                   // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(220, 233, 220, 0.8f),             // inkDim
                Rgba(105, 152, 117, 0.68f),            // winBg (translucent board)
                Rgba(99, 113, 81),                     // glyph
                Rgba(48, 57, 40),                      // shade (occupied cell)
                Rgba(126, 157, 116),                   // sel
                Rgba(126, 157, 116),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(71, 87, 62, 0.75f),               // cellBg
                Rgba(55, 72, 51, 0.85f),               // cellGroove
                Rgba(53, 96, 72),                      // btnFace
                Rgba(182, 158, 194, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(81, 112, 85),                     // lpBtn
                Rgba(101, 133, 103),                   // lpBtnHov
                Rgba(63, 92, 69),                      // lpBtnAct
                Rgba(47, 83, 63),                      // lpBorder
                Rgba(196, 204, 162),                   // lpBtnOnFace — the ON state
                Rgba(38, 46, 29),                      // lpBtnOnInk
                Rgba(13, 34, 25),                      // lpRule
            },
            {   // Simple Petrol — 페트롤 · 짙은 청록 (신규)
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Petrol",
                Rgba(0, 38, 50),                       // borders/ink
                Rgba(35, 107, 105),                    // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(201, 223, 221, 0.8f),             // inkDim
                Rgba(29, 121, 123, 0.68f),             // winBg (translucent board)
                Rgba(36, 83, 72),                      // glyph
                Rgba(9, 34, 29),                       // shade (occupied cell)
                Rgba(57, 128, 121),                    // sel
                Rgba(57, 128, 121),                    // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(16, 58, 52, 0.75f),               // cellBg
                Rgba(5, 45, 42, 0.85f),                // cellGroove
                Rgba(0, 63, 67),                       // btnFace
                Rgba(190, 137, 148, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(18, 79, 79),                      // lpBtn
                Rgba(38, 101, 100),                    // lpBtnHov
                Rgba(1, 61, 62),                       // lpBtnAct
                Rgba(1, 53, 57),                       // lpBorder
                Rgba(140, 190, 169),                   // lpBtnOnFace — the ON state
                Rgba(0, 28, 23),                       // lpBtnOnInk
                Rgba(0, 18, 19),                       // lpRule
            },
            {   // Simple Steel — 강철 · 짙은 청회 (신규)
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Steel",
                Rgba(19, 36, 73),                      // borders/ink
                Rgba(66, 107, 129),                    // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(209, 222, 230, 0.8f),             // inkDim
                Rgba(76, 119, 146, 0.68f),             // winBg (translucent board)
                Rgba(46, 85, 96),                      // glyph
                Rgba(16, 36, 42),                      // shade (occupied cell)
                Rgba(80, 128, 149),                    // sel
                Rgba(80, 128, 149),                    // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(30, 60, 71, 0.75f),               // cellBg
                Rgba(21, 47, 57, 0.85f),               // cellGroove
                Rgba(32, 63, 85),                      // btnFace
                Rgba(186, 145, 122, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(49, 80, 98),                      // lpBtn
                Rgba(66, 102, 121),                    // lpBtnHov
                Rgba(34, 62, 78),                      // lpBtnAct
                Rgba(27, 53, 72),                      // lpBorder
                Rgba(143, 191, 200),                   // lpBtnOnFace — the ON state
                Rgba(6, 29, 35),                       // lpBtnOnInk
                Rgba(4, 18, 29),                       // lpRule
            },
            {   // Simple Sky — 하늘 · 시안 도는 파랑 (신규)
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Sky",
                Rgba(7, 62, 98),                       // borders/ink
                Rgba(64, 140, 157),                    // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(211, 233, 239, 0.8f),             // inkDim
                Rgba(66, 152, 176, 0.68f),             // winBg (translucent board)
                Rgba(57, 117, 121),                    // glyph
                Rgba(27, 59, 61),                      // shade (occupied cell)
                Rgba(77, 160, 173),                    // sel
                Rgba(77, 160, 173),                    // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(40, 89, 95, 0.75f),               // cellBg
                Rgba(30, 73, 81, 0.85f),               // cellGroove
                Rgba(22, 95, 116),                     // btnFace
                Rgba(206, 154, 145, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(53, 112, 127),                    // lpBtn
                Rgba(69, 134, 150),                    // lpBtnHov
                Rgba(39, 92, 106),                     // lpBtnAct
                Rgba(23, 82, 101),                     // lpBorder
                Rgba(145, 212, 210),                   // lpBtnOnFace — the ON state
                Rgba(13, 49, 51),                      // lpBtnOnInk
                Rgba(5, 33, 44),                       // lpRule
            },
            {   // Simple — a plain blue windowed panel. Two things carry it
                // and neither is a colour: the border is TWO lines (dark outer
                // + bright inner, via bevelChrome), and the cell grid is CARVED
                // rather than drawn (engravedCells). Buttons are recessed for
                // the same reason — face darker than the chrome, same two-line
                // edge.
                // ★Every blue here is a step below the measured reference. The
                // reference colours live inside a 24px window frame on a 2003
                // client; ours cover half a modern screen at 1440p, and the
                // same value spread over that area is simply a brighter object.
                // Matching the swatch is not the goal — matching how it reads is.
                "Simple",
                Rgba(24, 58, 112),                      // acc    borders/ink
                Rgba(74, 138, 166),                     // hi — the inner rim sits just ABOVE the panel, not a bright ring
                Rgba(255, 255, 255),                    // ink
                Rgba(214, 232, 242, 0.80f),             // inkDim
                Rgba(82, 149, 185, 0.68f),              // winBg (-15%: see-through board)
                Rgba(58, 116, 130),                     // glyph
                Rgba(28, 58, 66, 1.0f),                 // shade (occupied cell)
                Rgba(84, 158, 184),                     // sel
                Rgba(84, 158, 184),                     // filled
                0.0f, 0.0f,                             // rounding / titleSpacing (plain titles)
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // no torn frame
                true, true,                             // translucent + bevelChrome
                true,                                   // lightPanel
                true,                                   // engravedCells
                // ★Pulled DOWN from the measured #468896. The reference cell
                // sits under a 24px-wide window frame; ours is the same colour
                // under a much larger panel, and at that scale the two read as
                // one flat field. The cell has to be a clear step below the
                // chrome or the board stops looking like a board.
                // ★These carry their OWN alpha now (Grid reads it instead of
                // forcing 1.0). Grooves and faces never overlap, so what is
                // written here is what lands on screen.
                // ★The COLOUR is what the old two-layer stack composited to
                // (groove .85 under face .85); the groove is no longer drawn --
                // the gap IS the panel.
                // ★The ALPHA no longer follows that arithmetic. Carrying it to
                // .9775 made the cells a near-solid sheet, which is a
                // translucent skin that is not translucent anywhere the player
                // actually looks -- the board is nearly all cells. .75 is
                // chosen, not derived: the cell keeps its own colour and the
                // room still shows through it.
                Rgba(43, 88, 102, 0.75f),               // cellBg
                Rgba(34, 72, 86, 0.85f),                // cellGroove (inner shadow only)
                Rgba(42, 92, 122),                      // btnFace
                Rgba(190, 158, 166, 0.50f),             // bagOpen: pale, half strength
                Rgba(238, 206, 118),                    // goldNum — money, not "a value"
                24.0f,                                  // titleSize (34px bar leaves 5px above and below)
            },
            {   // Simple Plum — 자두 · 어두운 보라 (신규)
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Plum",
                Rgba(62, 2, 31),                       // borders/ink
                Rgba(99, 81, 112),                     // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(219, 212, 224, 0.8f),             // inkDim
                Rgba(117, 90, 124, 0.68f),             // winBg (translucent board)
                Rgba(65, 63, 89),                      // glyph
                Rgba(25, 24, 37),                      // shade (occupied cell)
                Rgba(114, 101, 137),                   // sel
                Rgba(114, 101, 137),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(46, 41, 61, 0.75f),               // cellBg
                Rgba(38, 30, 48, 0.85f),               // cellGroove
                Rgba(64, 40, 64),                      // btnFace
                Rgba(135, 151, 113, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(73, 56, 81),                      // lpBtn
                Rgba(94, 76, 104),                     // lpBtnHov
                Rgba(57, 41, 62),                      // lpBtnAct
                Rgba(53, 32, 53),                      // lpBorder
                Rgba(165, 170, 205),                   // lpBtnOnFace — the ON state
                Rgba(20, 18, 32),                      // lpBtnOnInk
                Rgba(23, 4, 20),                       // lpRule
            },
            {   // Simple Violet — 제비꽃 · 보라
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Violet",
                Rgba(102, 33, 72),                     // borders/ink
                Rgba(136, 124, 166),                   // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(232, 227, 242, 0.8f),             // inkDim
                Rgba(155, 132, 178, 0.68f),            // winBg (translucent board)
                Rgba(100, 107, 140),                   // glyph
                Rgba(50, 53, 71),                      // shade (occupied cell)
                Rgba(148, 144, 189),                   // sel
                Rgba(148, 144, 189),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(79, 80, 107, 0.75f),              // cellBg
                Rgba(68, 65, 88, 0.85f),               // cellGroove
                Rgba(102, 79, 113),                    // btnFace
                Rgba(163, 170, 130, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(112, 99, 131),                    // lpBtn
                Rgba(132, 119, 156),                   // lpBtnHov
                Rgba(93, 80, 108),                     // lpBtnAct
                Rgba(89, 69, 98),                      // lpBorder
                Rgba(182, 200, 241),                   // lpBtnOnFace — the ON state
                Rgba(39, 43, 61),                      // lpBtnOnInk
                Rgba(38, 26, 40),                      // lpRule
            },
            {   // Simple Indigo — 남색 · 짙은 청보라
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Indigo",
                Rgba(85, 42, 93),                      // borders/ink
                Rgba(111, 130, 172),                   // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(224, 229, 244, 0.8f),             // inkDim
                Rgba(128, 140, 188, 0.68f),            // winBg (translucent board)
                Rgba(80, 111, 141),                    // glyph
                Rgba(40, 55, 71),                      // shade (occupied cell)
                Rgba(120, 150, 194),                   // sel
                Rgba(120, 150, 194),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(63, 84, 109, 0.75f),              // cellBg
                Rgba(54, 68, 91, 0.85f),               // cellGroove
                Rgba(81, 84, 121),                     // btnFace
                Rgba(181, 165, 127, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(92, 104, 137),                    // lpBtn
                Rgba(110, 125, 162),                   // lpBtnHov
                Rgba(76, 84, 114),                     // lpBtnAct
                Rgba(72, 73, 105),                     // lpBorder
                Rgba(160, 206, 239),                   // lpBtnOnFace — the ON state
                Rgba(28, 45, 62),                      // lpBtnOnInk
                Rgba(30, 28, 45),                      // lpRule
            },
            {   // Simple Wine — 적포도주
            // ★Derived from Simple in CIELAB: the hue is rotated while L* and C*
            // are HELD, so every skin in this family carries the same perceptual
            // weight and only the colour differs. Rotating in HSL does not — the
            // same saturation number is quiet in brown and loud in magenta — which
            // is how Wine once ended up competing with the item icons.
                "Simple Wine",
                Rgba(103, 38, 33),                     // borders/ink
                Rgba(165, 116, 139),                   // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(242, 225, 232, 0.8f),             // inkDim
                Rgba(183, 125, 146, 0.68f),            // winBg (translucent board)
                Rgba(129, 99, 124),                    // glyph
                Rgba(65, 49, 62),                      // shade (occupied cell)
                Rgba(183, 134, 163),                   // sel
                Rgba(183, 134, 163),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(101, 74, 92, 0.75f),              // cellBg
                Rgba(85, 60, 74, 0.85f),               // cellGroove
                Rgba(120, 74, 87),                     // btnFace
                Rgba(130, 176, 152, 0.5f),             // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                   // goldNum — money is money on every theme
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(133, 93, 109),                    // lpBtn
                Rgba(157, 113, 132),                   // lpBtnHov
                Rgba(111, 75, 89),                     // lpBtnAct
                Rgba(105, 64, 76),                     // lpBorder
                Rgba(220, 190, 225),                   // lpBtnOnFace — the ON state
                Rgba(55, 39, 52),                      // lpBtnOnInk
                Rgba(45, 24, 29),                      // lpRule
            },
            {   // Simple Strawberry Milk — 딸기우유 · 연분홍 + 크림
            // ★★A CONCEPT skin, not a hue rotation. The fruit names three colours
            // in fixed roles, so the rotation builds only the body and the rest are
            // placed by hand: the cap on every toggled-ON control (and so on the
            // EDIT painter — green is the theme here, not the mistake it would be
            // on Copper), the leaf on the open-bag tile, the seeds on the money.
            // ★Louder than the wheel skins on purpose. At their lightness the sRGB
            // gamut gives red barely a third of this chroma, and what came out was
            // a dusty rose indistinguishable from Ruby — the very thing a concept
            // skin exists to avoid.
                "Simple Strawberry Milk",
                Rgba(124, 62, 41),                     // borders/ink
                Rgba(192, 135, 150),                   // inner rim
                Rgba(255, 255, 255),                   // ink
                Rgba(250, 230, 235, 0.8f),             // inkDim
                Rgba(209, 143, 155, 0.68f),            // winBg (translucent board)
                Rgba(158, 118, 140),                   // glyph
                Rgba(89, 67, 78),                      // shade (occupied cell)
                Rgba(209, 150, 172),                   // sel
                Rgba(209, 150, 172),                   // filled
                0.0f, 0.0f,                            // rounding / titleSpacing
                false, false, false, false,            // no cornerFade/strip/glow/◇
                false,                                 // no torn frame
                true, true,                            // translucent + bevelChrome
                true,                                  // lightPanel
                true,                                  // engravedCells
                Rgba(129, 94, 109, 0.75f),             // cellBg
                Rgba(111, 80, 91, 0.85f),              // cellGroove
                Rgba(147, 95, 101),                    // btnFace
                Rgba(240, 223, 168, 0.5f),             // bagOpen (the leaf)
                Rgba(255, 233, 176),                   // goldNum (the seeds)
                24.0f,                                 // titleSize
                // light-panel palette. Naming every one is not optional: a
                // skin that stays silent inherits the built-in fallbacks,
                // which are SIMPLE's blues — a blue preset "+" and a blue
                // toggled-ON chip on an otherwise unrelated window.
                Rgba(160, 113, 124),                   // lpBtn
                Rgba(184, 132, 145),                   // lpBtnHov
                Rgba(138, 96, 104),                    // lpBtnAct
                Rgba(130, 86, 90),                     // lpBorder
                Rgba(245, 194, 206),                   // lpBtnOnFace — the ON state
                Rgba(92, 30, 44),                      // lpBtnOnInk
                Rgba(62, 39, 40),                      // lpRule
            },
        };

        // ★Initialised FROM the exported defaults, never from a second copy of
        // the number — the reset gesture reads the same constants.
        // ★No longer const: ResolveScale writes it once, at D3D init, from
        // the backbuffer height. It stays kDefScale until then, which is what
        // anything running before a swapchain exists will read.
        float g_scale = kDefScale;
        // ★The SETTING, not the board multiplier — 1.00 by default, and
        // kScaleBase (0.90) is what that 1.00 draws at. See Theme.h.
        float g_cellScale = kDefCellScale;
        // ★The player's text-size multiplier, on top of the automatic Scale().
        // 1.00 is exactly what shipped. See Theme.h for why it is separate.
        float g_fontScale = 1.0f;
        // ★★The shipped default, NAMED. It was `= 3`, with a comment saying
        // "Parchment Crimson" -- and by then 3 was Fable Crimson. A number
        // cannot state which skin it means, so it stops being true the first
        // time the table moves and nothing anywhere says so.
        // 0 means "not resolved yet"; EnsureSkin turns the name into an index
        // the first time anything asks, because kSkins is not comparable at
        // static-init time.
        constexpr const char* kDefaultSkin = "Sumi Parchment";
        int   g_skin = 0;   // 1-based once resolved
        // ★Capture-lamp offset shared by every icon. Deliberately NOT in the
        // per-skin block below — see Theme.h.
        float g_capLightAz = kDefCapLightAz;
        float g_capLightEl = kDefCapLightEl;

        // GI59: glow and icon light are kept PER ICON STYLE — [0] realistic
        // (3D captures), [1] drawn (flat art), [2] pixel. A photographed model
        // and a flat drawing do not take the same fill light.
        // ★★1.0.5: every DISPLAY setting is per SKIN as well.
        constexpr int kSkinSlots = static_cast<int>(std::size(kSkins));

        int   g_glowStyle[kSkinSlots][3] = {};      // legacy, ini format only
        int   g_iconStyle[kSkinSlots] = {};         // 0 realistic, ditto
        float g_glowGain[kSkinSlots][3][2] = {};    // legacy, ini format only
        float g_iconGain[kSkinSlots][3] = {};
        // ★★1.0.5 item shadow: [skin][icon style][0 dist / 1 blur / 2 opacity].
        float g_shadow[kSkinSlots][3][3] = {};
        // ★Seeded from the SAME constants the reset gesture reads, so a fresh
        // skin and a right-clicked slider can never disagree.
        const bool g_dispSeeded = [] {
            for (int s = 0; s < kSkinSlots; ++s) {
                for (int t = 0; t < 3; ++t) {
                    g_iconGain[s][t]    = DefIconGain(t);
                    g_glowGain[s][t][0] = DefGlowGain(0);
                    g_glowGain[s][t][1] = DefGlowGain(1);
                    for (int a = 0; a < 3; ++a) g_shadow[s][t][a] = DefShadow(a);
                }
            }
            return true;
        }();

        [[nodiscard]] constexpr int C01(int a_v)
        {
            return a_v < 0 ? 0 : (a_v > 2 ? 2 : a_v);
        }
        [[nodiscard]] constexpr int CAxis(int a_v)
        {
            return a_v < 0 ? 0 : (a_v > 2 ? 2 : a_v);
        }
        [[nodiscard]] constexpr float ClampShadow(int a_axis, float a_v)
        {
            const float hi = a_axis == 2 ? 1.0f : 8.0f;   // opacity is a fraction
            // ★NEGATED, so NaN lands on the floor instead of sailing through.
            // strtof accepts "nan", both comparisons in `v < 0 ? .. : v > hi`
            // are false for it, and the value was written straight back out to
            // the ini -- one hand-edited line and the shadow was permanently
            // NaN, which is not a shadow, it is no shadow at all.
            return !(a_v > 0.0f) ? 0.0f : (a_v > hi ? hi : a_v);
        }

        // skin index is 1-BASED everywhere it is spoken about (ini, chips,
        // SkinIndex); the arrays are 0-based. One converter, clamped.
        [[nodiscard]] constexpr int CSkin(int a_skin1)
        {
            const int i = a_skin1 - 1;
            return i < 0 ? 0 : (i >= kSkinSlots ? kSkinSlots - 1 : i);
        }
        [[nodiscard]] int SkinSlot() { return CSkin(g_skin); }

        // ★The icon style is the SKIN's, so Theme holds the number and the
        // cache is told about it. Raw values are IconCache::Style's own
        // (0 realistic / 2 drawn / 3 pixel — 1 is retired).
        void ApplyIconStyle()
        {
            auto* ic = IconCache::GetSingleton();
            if (!ic) return;
            switch (g_iconStyle[SkinSlot()]) {
            case 2:  ic->SetStyle(IconCache::Style::kFlat);  break;
            case 3:  ic->SetStyle(IconCache::Style::kPixel); break;
            default: ic->SetStyle(IconCache::Style::kRealistic); break;
            }
        }
    }

    int IconStyleSlot()
    {
        // The singleton can be absent while settings load at startup; realistic
        // is the right assumption then, and the loader never relies on this.
        const auto* ic = IconCache::GetSingleton();
        if (!ic) return 0;
        switch (ic->GetStyle()) {
        case IconCache::Style::kFlat:  return 1;
        case IconCache::Style::kPixel: return 2;
        default:                       return 0;
        }
    }

    // ---- DISPLAY settings: [skin][icon style][glow style] --------------------
    // The no-argument forms act on whatever is live (this skin, this icon
    // style) — every UI call site uses those. The *Of / *At forms name every
    // axis outright and are what persistence uses.

    float IconGain() { return g_iconGain[SkinSlot()][IconStyleSlot()]; }
    void  SetIconGain(float a_gain) { SetIconGainOf(g_skin, IconStyleSlot(), a_gain); }
    float IconGainOf(int a_skin, int a_slot)
    {
        return g_iconGain[CSkin(a_skin)][C01(a_slot)];
    }

    void SetIconGainOf(int a_skin, int a_slot, float a_gain)
    {
        g_iconGain[CSkin(a_skin)][C01(a_slot)] =
            (std::max)(0.4f, (std::min)(1.6f, a_gain));
    }

    // ---- item shadow ---------------------------------------------------------
    float ShadowAxis(int a_axis)
    {
        return g_shadow[SkinSlot()][IconStyleSlot()][CAxis(a_axis)];
    }
    float ShadowDist()    { return ShadowAxis(0); }
    float ShadowBlur()    { return ShadowAxis(1); }
    float ShadowOpacity() { return ShadowAxis(2); }

    void SetShadowAxis(int a_axis, float a_v)
    {
        SetShadowAt(g_skin, IconStyleSlot(), a_axis, a_v);
    }

    float ShadowAt(int a_skin, int a_slot, int a_axis)
    {
        return g_shadow[CSkin(a_skin)][C01(a_slot)][CAxis(a_axis)];
    }

    void SetShadowAt(int a_skin, int a_slot, int a_axis, float a_v)
    {
        const int ax = CAxis(a_axis);
        g_shadow[CSkin(a_skin)][C01(a_slot)][ax] = ClampShadow(ax, a_v);
    }

    int  GlowStyleOf(int a_skin, int a_slot)
    {
        return g_glowStyle[CSkin(a_skin)][C01(a_slot)];
    }

    void SetGlowStyleOf(int a_skin, int a_slot, int a_style)
    {
        g_glowStyle[CSkin(a_skin)][C01(a_slot)] = C01(a_style);
    }

    float GlowGain()
    {
        const int sk = SkinSlot(), slot = IconStyleSlot();
        return g_glowGain[sk][slot][g_glowStyle[sk][slot]];
    }

    float DefaultIconGain() { return DefIconGain(IconStyleSlot()); }

    void SetGlowGain(float a_gain)
    {
        const int sk = SkinSlot(), slot = IconStyleSlot();
        SetGlowGainAt(g_skin, slot, g_glowStyle[sk][slot], a_gain);
    }

    float GlowGainAt(int a_skin, int a_slot, int a_style)
    {
        return g_glowGain[CSkin(a_skin)][C01(a_slot)][C01(a_style)];
    }

    void SetGlowGainAt(int a_skin, int a_slot, int a_style, float a_gain)
    {
        g_glowGain[CSkin(a_skin)][C01(a_slot)][C01(a_style)] =
            (std::max)(0.2f, (std::min)(2.5f, a_gain));
    }

    int  IconStyleOf(int a_skin) { return g_iconStyle[CSkin(a_skin)]; }

    void SetIconStyleOf(int a_skin, int a_style)
    {
        g_iconStyle[CSkin(a_skin)] = a_style;
        if (CSkin(a_skin) == SkinSlot()) ApplyIconStyle();
    }

    void SetIconStyle(int a_style) { SetIconStyleOf(g_skin, a_style); }

    // ---- capture light -------------------------------------------------------
    float CaptureLightAz() { return g_capLightAz; }
    float CaptureLightEl() { return g_capLightEl; }

    void SetCaptureLight(float a_azDeg, float a_elDeg)
    {
        g_capLightAz = (std::max)(-180.0f, (std::min)(180.0f, a_azDeg));
        g_capLightEl = (std::max)(-80.0f, (std::min)(80.0f, a_elDeg));
    }

    // ---- scale ---------------------------------------------------------------
    float Scale() { return g_scale; }

    void ResolveScale(float a_displayH)
    {
        if (a_displayH < 240.0f) return;   // nonsense: keep the default
        const float s = a_displayH / kScaleBaseH;
        g_scale = (std::max)(kScaleMin, (std::min)(kScaleMax, s));
        SKSE::log::info("[UI] display height {:.0f} -> UI scale {:.3f}",
                        a_displayH, g_scale);
    }

    float ScaleSetting() { return g_cellScale; }

    // the board's actual multiplier — the setting against what 1.00 means
    float CellScale() { return g_cellScale * kScaleBase; }

    void SetScaleSetting(float a_scale)
    {
        // ★Clamped to the SAME numbers the slider draws (Theme.h). They used
        // to differ -- slider 0.6~1.2, clamp 0.5~1.4 -- which meant a value
        // typed into the box, or arriving from an older ini, could sit outside
        // what the control could ever show or take back.
        g_cellScale = (std::max)(kMinCellScale, (std::min)(kMaxCellScale, a_scale));
    }

    float FontScale() { return g_fontScale; }

    bool SetFontScale(float a_scale)
    {
        const float v = (std::max)(kMinFontScale, (std::min)(kMaxFontScale, a_scale));
        // ★The MOVED answer is the point: the atlas has to be re-baked when
        // this changes and must NOT be re-baked when it does not. A slider
        // being dragged sends a value every frame, and rebuilding the font
        // atlas every frame would stall the render thread for as long as the
        // player held the mouse down.
        if (v == g_fontScale) return false;
        g_fontScale = v;
        return true;
    }

    // ---- skin ----------------------------------------------------------------
    int SkinCount() { return static_cast<int>(std::size(kSkins)); }

    namespace
    {
        // resolves kDefaultSkin once. Safe to call from anything that reads
        // g_skin; takes an index, so it cannot recurse through SkinAt.
        void EnsureSkin()
        {
            if (g_skin != 0) return;
            g_skin = 1;
            for (int i = 1; i <= SkinCount(); ++i) {
                const char* n = kSkins[i - 1].name;
                if (n && std::strcmp(n, kDefaultSkin) == 0) { g_skin = i; break; }
            }
        }
    }

    int SkinIndex() { EnsureSkin(); return g_skin; }

    const Skin& SkinAt(int a_index)
    {
        // ★clamp against the TABLE, not a literal: an out-of-range index would
        // otherwise read past the array, and a saved number from a build with
        // more skins is exactly that.
        const int i = (std::max)(1, (std::min)(SkinCount(), a_index));
        return kSkins[i - 1];
    }

    const Skin& S() { EnsureSkin(); return SkinAt(g_skin); }

    void SetSkin(int a_index)
    {
        g_skin = (std::max)(1, (std::min)(SkinCount(), a_index));
        if (ImGui::GetCurrentContext()) Apply();
        ApplyIconStyle();
    }

    int SkinIndexByName(const char* a_name)
    {
        if (!a_name || !*a_name) return 0;
        for (int i = 1; i <= SkinCount(); ++i) {
            const char* n = SkinAt(i).name;
            if (n && std::strcmp(n, a_name) == 0) return i;
        }
        return 0;
    }

    const char* SkinNameAt(int a_index)
    {
        if (a_index < 1 || a_index > SkinCount()) return "";
        const char* n = SkinAt(a_index).name;
        return n ? n : "";
    }

    bool SetSkinByName(const char* a_name)
    {
        const int i = SkinIndexByName(a_name);
        if (i <= 0) {
            SKSE::log::warn("[THEME] saved skin \"{}\" no longer exists — keeping {}",
                            a_name ? a_name : "", SkinNameAt(g_skin));
            return false;
        }
        SetSkin(i);
        return true;
    }

    // ★★The one table that maps a "!skin2"-era POSITION onto a name. Written
    // out in full rather than as arithmetic on the index: two removals and a
    // move do not compose into an offset, and the next edit to kSkins must not
    // be able to quietly change what an old file means. A removed skin names
    // the skin its settings should land on instead.
    namespace
    {
        constexpr const char* kSkin2Order[] = {
            "Fable Crimson",                    //  1
            "Parchment Amber",                  //  2
            "Parchment Crimson",                //  3
            "Simple Charcoal",                  //  4  was Glass Dark  — nearest dark board
            "Simple Charcoal",                  //  5  was Glass Clear —   "
            "Simple Charcoal",                  //  6
            "Simple Graphite",                  //  7
            "Simple Silver",                    //  8
            "Simple Pine",                      //  9
            "Simple Forest",                    // 10
            "Simple Petrol",                    // 11
            "Simple Steel",                     // 12
            "Simple Sky",                       // 13
            "Simple",                           // 14
            "Simple Plum",                      // 15
            "Simple Violet",                    // 16
            "Simple Indigo",                    // 17
            "Simple Wine",                      // 18
            "Simple Strawberry Milk",           // 19
            "Sumi",                             // 20
            "Sumi Parchment",                   // 21
        };
    }

    int MigrateSkin2Index(int a_oldIndex)
    {
        constexpr int n = static_cast<int>(std::size(kSkin2Order));
        if (a_oldIndex < 1 || a_oldIndex > n) return 0;
        // ★Glass 4 and 5 both migrate onto Simple Charcoal, so two old blocks
        // can land on one skin. Last one wins, which is the only answer that
        // does not need a merge rule -- and either is better than a block
        // silently landing on whoever now sits at 4.
        return SkinIndexByName(kSkin2Order[a_oldIndex - 1]);
    }

    void SetSkinLegacy2(int a_oldIndex)
    {
        const int i = MigrateSkin2Index(a_oldIndex);
        if (i > 0) SetSkin(i);
    }

    void SetSkinLegacy(int a_oldIndex)
    {
        // ★"Fable Amber" sat at index 2 and was removed; everything above it
        // shifted down by one. A file that says "!skin" was written in that
        // numbering, so 1 stays 1, 2 (the removed one) becomes Parchment
        // Amber's 2, and everything past it drops — landing in the "!skin2"
        // numbering, which then needs the second conversion too.
        const int i = a_oldIndex <= 2 ? (std::max)(1, a_oldIndex)
                                      : a_oldIndex - 1;
        SetSkinLegacy2(i);
    }

    // ---- colour helpers ------------------------------------------------------
    ImU32 Col(const ImVec4& a_c, float a_alpha)
    {
        ImVec4 c = a_c;
        if (a_alpha >= 0.0f) c.w = a_alpha;
        return ImGui::GetColorU32(c);
    }

    ImU32 Acc(float a_alpha)
    {
        // translucent skins: hairlines at paper-era alphas vanish against the
        // world showing through — boost every acc line/grid across the board
        if (S().translucent && a_alpha >= 0.0f) {
            a_alpha = (std::min)(1.0f, a_alpha * 1.9f);
        }
        return Col(S().acc, a_alpha);
    }

    const ImVec4& ValVec()
    {
        const Skin& sk = S();
        // ★★A light panel has no headroom above it — `hi` is tuned to glow on a
        // dark ground and simply fades into a bright one. The answer is the
        // skin's own INK: on SIMPLE that is white (and every number carries a
        // black outline, so it reads at 21:1 whatever the panel is doing), on
        // parchment it is the same brown the body text uses.
        // ★NOT acc. Acc is the darkest token in a light skin — it is the frame
        // colour — so numbers painted in it read as engraved into the panel
        // rather than as the figures the eye is hunting for.
        return sk.lightPanel ? sk.ink : sk.hi;
    }

    ImU32 Val(float a_alpha) { return Col(ValVec(), a_alpha); }

    ImU32 GoldCol()
    {
        const Skin& sk = S();
        return sk.goldNum.w > 0.0f ? Col(sk.goldNum, 1.0f) : Val();
    }

    ImU32 Chrome(float a_alpha)
    {
        const Skin& sk = S();
        // ★Over a LIGHT panel acc is the darkest thing on screen, and headings
        // painted in it stop reading as headings — they read as borders. The
        // answer is the skin's own INK, not a darker colour still: on SIMPLE
        // that is white (and TextOutlined gives it a black edge, 21:1), on
        // parchment it is the brown the body text already uses. Each skin
        // already decided what "written on this panel" looks like.
        return sk.lightPanel ? Col(sk.ink, a_alpha) : Col(sk.acc, a_alpha);
    }

    // ---- ink skins: the paper sheet -----------------------------------------
    namespace
    {
        constexpr const char* kInkDir = "Data/SKSE/Plugins/GridInventory_ink/";
        // ★Keyed by the FILE NAME, not by a slot or an index. Two skins can
        // name the same sheet, and a skin inserted above them must not hand
        // one skin's art to another -- the same reason LightItemShadow()
        // matches on name.
        // ★A sheet carries its own average colour. It is the divisor in
        // PaperTint(): a skin states the colour its paper should READ as, and
        // reaching that by multiplication is only possible against what the
        // file already is. Kept in the same entry as the texture so the two
        // cannot be dropped or reloaded apart.
        struct InkSheet
        {
            IconCache::Icon ic{};
            float           mean[3] = { 255.0f, 255.0f, 255.0f };
        };
        std::unordered_map<std::string, InkSheet> g_paper;
        // bumped by ReloadInkArt — anything caching a value DERIVED from a
        // sheet (PaperTint) has to notice the sheet was swapped under it
        int g_inkGen = 0;

        // The marks. One set serves every ink skin: they are white + alpha and
        // take their colour from the caller.
        constexpr const char* kStroke = "stroke_0.png";   // every brush line
        constexpr const char* kRule   = "rule_2.png";     // the fine ruled line
        constexpr const char* kCorner = "corner_0.png";   // the frame's corner
        // ★The WHEEL's title banner, reused. The quick wheel already had a
        // painted plate behind its group name; making a second one for the
        // inventory would be two marks that mean the same thing and look
        // almost alike, which is how a UI stops reading as one hand.
        constexpr const char* kTitle  = "title.png";
        // ★A stamped seal. Its RED is its identity -- a 낙관 is vermilion the
        // way ink is black -- so it is drawn in its own colour rather than the
        // skin's clay accent, which is a surface tone and would read as a
        // faded print. Still stored white+alpha: what is baked in is the
        // CRUMBLE of the pad, not the pigment.
        constexpr const char* kSeal   = "seal.png";
        constexpr ImU32       kSealRed = IM_COL32(0xAA, 0x32, 0x19, 255);
        constexpr int         kWashes = 12;               // wash_0 .. wash_11

        // corner_0's arm geometry, measured off the sprite and expressed as
        // fractions of its own box so it survives being drawn at any size.
        // See Theme.h InkFrame for why a constant cannot replace these.
        constexpr float kArmY  = 0.053f;   // horizontal arm centreline / height
        constexpr float kArmTH = 0.089f;   // ...and its thickness
        constexpr float kArmX  = 0.062f;   // vertical arm centreline / width
        constexpr float kArmTV = 0.099f;
        // ★★★0.30 -- MORE overlap, not less, because the sides now fade into it.
        // The corner's arms thin from ~0.55 of the sprite and are gone by ~0.8;
        // a side that starts inside that and fades UP as the corner fades DOWN
        // crosses over without either of them showing an edge. Starting later
        // (or butting exactly, which a re-baked corner allowed) only trades the
        // doubled band for an exposed end -- both were rendered and both are
        // worse. See kInkArmFade and Theme::MarkTailed.
        constexpr float kArmRun = 0.30f;
        // How much of a frame side is taper, as a fraction of its length. Sized
        // from the CORNER, not from the side: it has to cover the crossover, and
        // that is ~0.85 cs long however wide the window is. Clamped so a long
        // main-window edge does not become a ghost and a stubby slot edge does
        // not become all taper.
        constexpr float kInkArmFade = 0.85f;

        [[nodiscard]] const InkSheet* InkSheetFor(const std::string& a_name)
        {
            auto it = g_paper.find(a_name);
            if (it == g_paper.end()) {
                // ★A FAILED load is cached too, as an empty Icon. Without that
                // a missing file costs a probe every frame -- and the wheel
                // already learned the other half of this, that a cached
                // failure must be droppable (see ReloadInkArt).
                InkSheet sh{};
                IconCache::LoadPngTexture(kInkDir + a_name, sh.ic, false, sh.mean);
                if (!sh.ic.srv) {
                    SKSE::log::warn("[THEME] ink art missing: {}{}", kInkDir, a_name);
                } else if (a_name.rfind("paper", 0) == 0) {
                    // ★Only the SHEETS. Their average is PaperTint's divisor, so
                    // it is worth having when a new one is photographed; the
                    // brush sprites are white by construction and printing 255
                    // 255 255 seventeen times a load says nothing.
                    SKSE::log::info("[THEME] {} — mean rgb {:.1f} {:.1f} {:.1f}",
                                    a_name, sh.mean[0], sh.mean[1], sh.mean[2]);
                }
                it = g_paper.emplace(a_name, sh).first;
            }
            return it->second.ic.srv ? &it->second : nullptr;
        }

        [[nodiscard]] const IconCache::Icon* InkArt(const std::string& a_name)
        {
            const auto* sh = InkSheetFor(a_name);
            return sh ? &sh->ic : nullptr;
        }

        // ★★THE THREE THE DRAW LOOPS ASK FOR BY THE HUNDRED.
        //
        // InkArt takes a std::string, so calling it with one of the kStroke /
        // kRule / kCorner literals minted a temporary and hashed it -- once per
        // grid line, per lattice, per frame. The answer only changes when the
        // sheets are reloaded, and there is already a counter for that.
        // Caller supplies the two statics so each site keeps its own slot.
        [[nodiscard]] const IconCache::Icon* InkArtPinned(
            const char* a_name, const IconCache::Icon*& a_slot, int& a_gen)
        {
            if (a_gen != g_inkGen) {
                a_gen  = g_inkGen;
                a_slot = InkArt(a_name);   // a cached miss is a null, and stays one
            }
            return a_slot;
        }

        // One sprite, stretched into a rect. Vertical marks reuse the same
        // horizontal sprite by swapping the uv corners -- a 90 degree turn
        // costs nothing and keeps one file doing both jobs.
        // One sprite into a rect. a_u0/a_u1 select how much of its LENGTH is
        // used; vertical marks reuse the same horizontal sprite by turning the
        // uv corners, which costs nothing and keeps one file doing both jobs.
        void Mark(ImDrawList* a_dl, const IconCache::Icon* a_ic,
                  ImVec2 a_min, ImVec2 a_max, ImU32 a_col, bool a_vert,
                  float a_u0 = 0.0f, float a_u1 = 1.0f)
        {
            if (!a_dl || !a_ic) return;
            // ★★★SNAP TO A CONSTANT WIDTH, not to the nearest pixel on each
            // side. Rounding the two edges independently makes the WIDTH
            // depend on where the line happened to fall: a 2.4px rule at x=100
            // rounds to 2px and at x=100.5 to 3px. Cells sit at fractional
            // multiples of the cell size, so each line drew at its own phase
            // and the grid came out with a few lines visibly heavier than the
            // rest -- one whole line at a time, which is what gave it away.
            const float tw = a_max.x - a_min.x, tht = a_max.y - a_min.y;
            if (tw < 6.0f || tht < 6.0f) {
                if (tw <= tht) {                       // a vertical mark
                    const float w = (std::max)(1.0f, std::round(tw));
                    a_min.x = std::round(a_min.x + (tw - w) * 0.5f);
                    a_max.x = a_min.x + w;
                } else {                               // ...a horizontal one
                    const float h = (std::max)(1.0f, std::round(tht));
                    a_min.y = std::round(a_min.y + (tht - h) * 0.5f);
                    a_max.y = a_min.y + h;
                }
            }
            const auto tex = reinterpret_cast<ImTextureID>(a_ic->srv);
            if (!a_vert) {
                a_dl->AddImage(tex, a_min, a_max,
                               ImVec2(a_u0, 0.0f), ImVec2(a_u1, 1.0f), a_col);
                return;
            }
            a_dl->AddImageQuad(tex,
                ImVec2(a_min.x, a_max.y), ImVec2(a_min.x, a_min.y),
                ImVec2(a_max.x, a_min.y), ImVec2(a_max.x, a_max.y),
                ImVec2(a_u0, 0.0f), ImVec2(a_u1, 0.0f),
                ImVec2(a_u1, 1.0f), ImVec2(a_u0, 1.0f), a_col);
        }

        // ★★★THE SAME MARK, BUT ITS ENDS FADE OUT. A slice of a brush is cut
        // square at full strength, so wherever a side meets the corner one of
        // them steps: overlap and the two inks composite into a band darker and
        // wider than the line (0.67 over 0.67 = 0.89); butt them and the cut is
        // simply exposed. Both were measured and both look wrong.
        // A brush on paper does neither -- it thins away, and two marks that
        // both thin can lie on top of one another and read as one stroke. The
        // corner sprite was painted that way from the start; only the sides
        // were not, which is why re-baking the CORNER made it worse (two steps
        // instead of one) while this is the half that was missing.
        // ★Per-VERTEX alpha, not three quads at three alphas: a stepped ramp
        // just moves the steps inward. Four vertex columns -- 0, fade, 1-fade,
        // 1 -- with alpha 0/1/1/0 give the interpolator a true linear ramp for
        // the cost of two extra quads.
        void MarkTailed(ImDrawList* a_dl, const IconCache::Icon* a_ic,
                        ImVec2 a_min, ImVec2 a_max, ImU32 a_col, bool a_vert,
                        float a_u0, float a_u1, float a_fade)
        {
            if (!a_dl || !a_ic) return;
            const float f = std::clamp(a_fade, 0.01f, 0.49f);
            // ★One fact, one place. The knot count decides the array sizes, both
            // loop bounds AND the two numbers PrimReserve is handed -- and those
            // last two are the ones that break SILENTLY if they disagree with
            // what is actually written (a mis-sized reserve corrupts the draw
            // list rather than failing). Derived, so they cannot.
            constexpr int kKnots = 4;                 // 0, fade, 1-fade, 1
            constexpr int kQuads = kKnots - 1;
            const float knot[kKnots] = { 0.0f, f, 1.0f - f, 1.0f };
            const float mul[kKnots]  = { 0.0f, 1.0f, 1.0f, 0.0f };
            ImVec2 pos[kKnots][2], uvs[kKnots][2];
            for (int i = 0; i < kKnots; ++i) {
                const float t = knot[i];
                const float u = a_u0 + (a_u1 - a_u0) * t;
                if (!a_vert) {
                    const float x = a_min.x + (a_max.x - a_min.x) * t;
                    pos[i][0] = ImVec2(x, a_min.y); uvs[i][0] = ImVec2(u, 0.0f);
                    pos[i][1] = ImVec2(x, a_max.y); uvs[i][1] = ImVec2(u, 1.0f);
                } else {
                    // ★The vertical case turns the uvs exactly as Mark does --
                    // one sprite serves both directions, and t runs from the
                    // BOTTOM up, which is the order Mark's quad established.
                    const float y = a_max.y + (a_min.y - a_max.y) * t;
                    pos[i][0] = ImVec2(a_min.x, y); uvs[i][0] = ImVec2(u, 0.0f);
                    pos[i][1] = ImVec2(a_max.x, y); uvs[i][1] = ImVec2(u, 1.0f);
                }
            }
            const auto  tex = reinterpret_cast<ImTextureID>(a_ic->srv);
            const ImU32 rgb = a_col & ~IM_COL32_A_MASK;
            const auto  al  = static_cast<float>((a_col >> IM_COL32_A_SHIFT) & 0xFF);
            a_dl->PushTexture(tex);
            a_dl->PrimReserve(kQuads * 6, kKnots * 2);
            const unsigned int base = a_dl->_VtxCurrentIdx;
            for (int i = 0; i < kKnots; ++i) {
                const ImU32 c = rgb | (static_cast<ImU32>(al * mul[i]) << IM_COL32_A_SHIFT);
                a_dl->PrimWriteVtx(pos[i][0], uvs[i][0], c);
                a_dl->PrimWriteVtx(pos[i][1], uvs[i][1], c);
            }
            for (unsigned int s = 0; s < kQuads; ++s) {
                const auto a = static_cast<ImDrawIdx>(base + s * 2);
                a_dl->PrimWriteIdx(a);
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 2));
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3));
                a_dl->PrimWriteIdx(a);
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3));
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1));
            }
            a_dl->PopTexture();
        }

        // ★★How much of the sprite a mark of this size should USE. Stretching
        // the whole 25:1 stroke into an 82px slot edge compresses it 16 times,
        // and every dry gap and swell in it averages out -- what lands is a
        // flat bar, which is exactly the "too straight, too angular" the doll
        // came back with. Taking a SECTION instead keeps the brush at its own
        // proportion; only lines longer than the sprite can carry get stretched.
        // a_key slides which section, so neighbouring marks are not clones.
        void MarkSpan(const IconCache::Icon* a_ic, float a_len, float a_th,
                      unsigned int a_key, float& a_u0, float& a_u1)
        {
            a_u0 = 0.0f;
            a_u1 = 1.0f;
            if (!a_ic || a_ic->h <= 0 || a_th <= 0.01f) return;
            const float natural = static_cast<float>(a_ic->w) / a_ic->h * a_th;
            if (natural <= a_len * 1.02f) return;      // must stretch: use it all
            const float span = (std::max)(0.12f, a_len / natural);
            const float slide = static_cast<float>((a_key * 2654435761u >> 8) & 0xFFFF)
                              / 65535.0f;
            a_u0 = (1.0f - span) * slide;
            a_u1 = a_u0 + span;
        }
    }

    bool InkChrome()
    {
        const char* p = S().paper;
        return p && *p;
    }

    // ★★NOT a fraction of GetWindowWidth(). Measured in game: the doll sits in
    // a 335px child while the board sits in another, so one formula written as
    // "0.61% of the window" handed each panel a different answer -- the doll's
    // border came out 2px where the spec meant 6.7, and nothing on screen said
    // which width had been asked. The weights are a property of the UI's SCALE,
    // which is the same number everywhere, not of whichever child happens to be
    // current.
    float InkRulePx()  { return (std::max)(1.0f, std::round(2.0f * Scale())); }
    float InkEdgePx()  { return (std::max)(1.0f, std::round(3.0f * Scale())); }
    // ★2.5, down from 4. And it moves TWO things at once, which is why 4 read
    // as so heavy: the doll's corner is sized from this (InkCornerFor), so at 4
    // the corner mark came out 45px on an 82px slot -- more than half the cell
    // was corner. Thinning the line shrinks the flourish with it.
    // ★NOT rounded, unlike the two above. This one feeds a corner SIZE as well
    // as a line, and rounding it to whole pixels quantises the flourish for no
    // gain -- the line it produces is snapped later, where snapping belongs.
    float InkHeavyPx() { return (std::max)(1.0f, 2.5f * Scale()); }

    namespace
    {
        // ★★★WHERE A STROKE SITS IN ITS WINDOW, not where the window sits on the
        // screen. The key below chooses which slice of the brush sprite a mark
        // uses, and it was hashed from SCREEN coordinates -- so dragging a window
        // re-dealt every rule, every lattice line and every frame stroke, a new
        // slice per frame, and the ink visibly crawled inside its own lines
        // (user report: Sumi, main window drag).
        // A window already solved exactly this for its paper sheet and its torn
        // silhouette by seeding from the window KEY -- "a window keeps ONE piece
        // of paper for the whole session, and moving it must not deal a new one"
        // (WinManager::TitleBar). The strokes never got the same treatment.
        // ★CONTENT-local, not merely window-local: the scroll is added back, or
        // the same crawl returns the moment a partner's list is scrolled rather
        // than dragged.
        // ★★★...AND WHICH WINDOW IT IS. Local coordinates alone are not enough:
        // every window's frame starts at the same offset inside itself, so a
        // purely local key handed EVERY window the identical slice -- and with
        // it the identical cut end, in the identical place, on every bag on
        // screen at once (user report). Screen coordinates used to scatter that
        // by accident, which is the only reason the cuts were not visible
        // before; the scattering is what has to be kept, minus the crawl.
        // The window ID is stable for the life of the window, so the slice is
        // dealt once and never again.
        unsigned int InkSpanKey(ImVec2 a_at)
        {
            const auto* w = ImGui::GetCurrentWindowRead();
            const ImVec2 o = w ? ImVec2(w->Pos.x - w->Scroll.x, w->Pos.y - w->Scroll.y)
                               : ImVec2(0.0f, 0.0f);
            const auto local = static_cast<unsigned int>(
                std::lround((a_at.x - o.x) * 7.0f + (a_at.y - o.y) * 13.0f));
            return local ^ (w ? static_cast<unsigned int>(w->ID) * 2246822519u : 0u);
        }
    }

    void InkStroke(ImDrawList* a_dl, ImVec2 a_from, float a_len, float a_th,
                   ImU32 a_col, bool a_vert, bool a_whole, float a_fade)
    {
        if (a_len <= 0.5f || a_th <= 0.05f) return;
        static const IconCache::Icon* s_stroke = nullptr;
        static int s_strokeGen = -1;
        const auto* ic = InkArtPinned(kStroke, s_stroke, s_strokeGen);
        // ★Overshoot half a thickness at each end. Four marks butted at a
        // corner leave a notch; the reference has them CROSS, one running a
        // little past the other, and half a width is the least that closes it.
        // ★★NOT when the whole sprite is used. The overshoot exists to close a
        // join between two CUT ends; a whole mark already ends in its own
        // taper, so the extra only pushed it outward -- which is how the title
        // stroke kept escaping the window however carefully its start was
        // clamped. The clamp was right; something downstream was undoing it.
        const float over = a_whole ? 0.0f : a_th * 0.5f;
        const float len = a_len + over * 2.0f;
        const ImVec2 from(a_vert ? a_from.x : a_from.x - over,
                          a_vert ? a_from.y - over : a_from.y);
        const unsigned int key = InkSpanKey(from);
        float u0 = 0.0f, u1 = 1.0f;
        if (!a_whole) MarkSpan(ic, len, a_th, key, u0, u1);
        const float h = a_th * 0.5f;
        const ImVec2 mn = a_vert ? ImVec2(from.x - h, from.y) : ImVec2(from.x, from.y - h);
        const ImVec2 mx = a_vert ? ImVec2(from.x + h, from.y + len)
                                 : ImVec2(from.x + len, from.y + h);
        if (a_fade > 0.0f) MarkTailed(a_dl, ic, mn, mx, a_col, a_vert, u0, u1, a_fade);
        else               Mark(a_dl, ic, mn, mx, a_col, a_vert, u0, u1);
    }

    void RuleLine(ImDrawList* a_dl, ImVec2 a_from, ImVec2 a_to)
    {
        if (!a_dl) return;
        if (!InkChrome()) {
            a_dl->AddLine(a_from, a_to, Rule());
            return;
        }
        const bool vert = std::fabs(a_to.y - a_from.y) > std::fabs(a_to.x - a_from.x);
        const float len = vert ? (a_to.y - a_from.y) : (a_to.x - a_from.x);
        InkStroke(a_dl, a_from, std::fabs(len), InkRulePx(),
                  Col(S().ink, 0.55f), vert, /*whole=*/true);
    }

    void InkRule(ImDrawList* a_dl, ImVec2 a_from, float a_len, float a_th,
                 ImU32 a_col, bool a_vert)
    {
        if (a_len <= 0.5f || a_th <= 0.05f) return;
        static const IconCache::Icon* s_rule = nullptr;
        static int s_ruleGen = -1;
        const auto* ic = InkArtPinned(kRule, s_rule, s_ruleGen);
        const unsigned int key = InkSpanKey(a_from);
        float u0 = 0.0f, u1 = 1.0f;
        MarkSpan(ic, a_len, a_th, key, u0, u1);
        const float h = a_th * 0.5f;
        Mark(a_dl, ic,
             a_vert ? ImVec2(a_from.x - h, a_from.y) : ImVec2(a_from.x, a_from.y - h),
             a_vert ? ImVec2(a_from.x + h, a_from.y + a_len)
                    : ImVec2(a_from.x + a_len, a_from.y + h),
             a_col, a_vert, u0, u1);
    }


    float InkCornerFor(float a_th) { return a_th / kArmTH; }

    void InkFrame(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col,
                  float a_corner)
    {
        static const IconCache::Icon* s_corner = nullptr;
        static int s_cornerGen = -1;
        const auto* cn = InkArtPinned(kCorner, s_corner, s_cornerGen);
        if (!a_dl || !cn) return;
        const float w = a_max.x - a_min.x, h = a_max.y - a_min.y;
        if (w < 6.0f || h < 6.0f) return;
        // ★★SHRINK THE CORNER, never drop it. The first cut fell back to four
        // plain strokes when the box was small, and that is where the shape
        // went wrong: a section of a brush has a CUT at both ends, so four of
        // them meet at four square notches -- which is the "too angular" the
        // doll came back with twice. The corner sprite exists precisely to be
        // the thing that is not square there.
        const float cs = (std::min)(a_corner, (std::min)(w, h) * 0.58f);
        const float hy = kArmY * cs, hth = kArmTH * cs;   // top/bottom runs
        const float vx = kArmX * cs, vth = kArmTV * cs;   // left/right runs
        const float run = cs * kArmRun;
        // ★★★TRIED AND REVERTED: whole=true on these four arms, to stop them
        // ending in a cut. It does not, and it costs. Measured offline against
        // the real sprites: stroke_0.png is 1308x51 and its alpha goes 0 -> 245
        // in ONE column, so the "taper" a whole mark ends in is a single pixel
        // -- a whole mark has hard ends too. What it does change is the middle:
        // squeezing a 25:1 brush into a short arm flattens every dry gap in it
        // (the note on MarkSpan says the same), and it put a dark blob halfway
        // down every bag's side. The step at the corner did not move, because
        // the step is not a cut -- see below.
        const float lenH = w - run * 2.0f, lenV = h - run * 2.0f;
        const auto fadeOf = [&](float a_len) {
            return a_len > 1.0f ? std::clamp(kInkArmFade * cs / a_len, 0.05f, 0.45f) : 0.0f;
        };
        const float fH = fadeOf(lenH), fV = fadeOf(lenV);
        InkStroke(a_dl, ImVec2(a_min.x + run, a_min.y + hy), lenH, hth, a_col, false, false, fH);
        InkStroke(a_dl, ImVec2(a_min.x + run, a_max.y - hy), lenH, hth, a_col, false, false, fH);
        InkStroke(a_dl, ImVec2(a_min.x + vx, a_min.y + run), lenV, vth, a_col, true, false, fV);
        InkStroke(a_dl, ImVec2(a_max.x - vx, a_min.y + run), lenV, vth, a_col, true, false, fV);
        // ★★★THE SIDE IS THE HALF THAT WAS WRONG, NOT THE CORNER.
        // Measured from corner_0.png (273x348): its arms hold full thickness to
        // ~0.55 of the sprite and have faded to nothing by ~0.8 --
        //     x .55W th .086   x .70W th .063   x .85W th .006
        // That taper is CORRECT; it is how a brush leaves the paper. The sides
        // were the ones ending square, at full strength, so the crossover could
        // only ever double (0.67 over 0.67 = 0.89, a band darker and wider than
        // the line, ending in a step) or gap.
        // Everything geometric was rendered offline against the real sprites,
        // and every one of them failed:
        //   run .62 / .72 / .80  the band shrinks, then the side's blunt start
        //                        is exposed instead, which is worse
        //   corner cropped to    butts two full ends, but stroke_0 is DARKEST
        //     .55 and run .55    in its first column -- a dark blob, not a step
        //   arms drawn last      identical; the corner's arm is the wider one
        //   whole=true strokes   stroke_0 goes 0 -> 245 in ONE column, so a
        //                        whole mark has hard ends too. It only
        //                        flattened the brush on short sides.
        //   re-baked corner with its arms run out at full thickness: made it
        //                        WORSE -- the pasted band's own start became a
        //                        second edge, so there were two steps.
        // The sides fade instead, and overlap the corner while doing it.
        // ...corners last, over the joins and over the cuts.
        const auto tex = reinterpret_cast<ImTextureID>(cn->srv);
        const ImVec2 uv[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        struct Q { ImVec2 o; int a, b, c, d; };   // uv order = flip
        const Q corners[4] = {
            { ImVec2(a_min.x, a_min.y),           0, 1, 2, 3 },   // as drawn
            { ImVec2(a_max.x - cs, a_min.y),      1, 0, 3, 2 },   // mirrored X
            { ImVec2(a_min.x, a_max.y - cs),      3, 2, 1, 0 },   // mirrored Y
            { ImVec2(a_max.x - cs, a_max.y - cs), 2, 3, 0, 1 },   // both
        };
        for (const auto& q : corners) {
            a_dl->AddImageQuad(tex, q.o, ImVec2(q.o.x + cs, q.o.y),
                ImVec2(q.o.x + cs, q.o.y + cs), ImVec2(q.o.x, q.o.y + cs),
                uv[q.a], uv[q.b], uv[q.c], uv[q.d], a_col);
        }
    }

    void InkTitleMark(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col)
    {
        const auto* ic = InkArt(kTitle);
        if (!a_dl || !ic) return;
        a_dl->AddImage(reinterpret_cast<ImTextureID>(ic->srv), a_min, a_max,
                       ImVec2(0, 0), ImVec2(1, 1), a_col);
    }

    void InkSeal(ImDrawList* a_dl, ImVec2 a_centre, float a_size, float a_alpha)
    {
        const auto* ic = InkArt(kSeal);
        if (!a_dl || !ic || a_size < 4.0f) return;
        const int al = static_cast<int>(255.0f * std::clamp(a_alpha, 0.0f, 1.0f));
        if (al <= 2) return;
        const float h = a_size * 0.5f;
        a_dl->AddImage(reinterpret_cast<ImTextureID>(ic->srv),
            ImVec2(a_centre.x - h, a_centre.y - h),
            ImVec2(a_centre.x + h, a_centre.y + h),
            ImVec2(0, 0), ImVec2(1, 1),
            (kSealRed & 0x00FFFFFF) | (static_cast<ImU32>(al) << IM_COL32_A_SHIFT));
    }

    void InkWash(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col,
                 unsigned int a_key)
    {
        if (!a_dl) return;
        const float w = a_max.x - a_min.x, h = a_max.y - a_min.y;
        if (w < 4.0f || h < 4.0f) return;
        // ★A small bleed, not a generous one. The marks already end in a soft
        // halo; a wide overspill on top of that put ink in the GAP between
        // slots, which reads as a leak rather than as a wash crossing its cell.
        const float bx = w * 0.02f, by = h * 0.02f;

        // ★★ONE MARK PER SQUARE, TWO FOR A LONG CELL. These were painted as
        // square patches, so stretching one down a tall slot pulls its
        // horizontal strokes into streaks and the ink stops looking laid. Two
        // marks butted together keep the brushwork at its own proportion, and
        // the overlap where they meet is a second bleed for free.
        const float ar = w / h;
        const int n = (ar > 1.45f || ar < 0.69f) ? 2 : 1;
        const bool stackY = (ar < 1.0f);

        const auto one = [&](ImVec2 p0, ImVec2 p1, unsigned int k) {
            const unsigned int m = k * 2654435761u;
            const auto* ic = InkArt("wash_" + std::to_string((m >> 8) % 12) + ".png");
            if (!ic) return;
            const ImVec2 uv[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            // ★A HALF TURN, never a quarter. These marks are laid in horizontal
            // strokes; turning one on its side makes it obviously the same
            // picture rotated, where flipping only changes which end is heavy.
            const int f = static_cast<int>((m >> 20) & 3);
            const int o[4][4] = { {0,1,2,3}, {1,0,3,2}, {3,2,1,0}, {2,3,0,1} };
            a_dl->AddImageQuad(reinterpret_cast<ImTextureID>(ic->srv),
                p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y),
                uv[o[f][0]], uv[o[f][1]], uv[o[f][2]], uv[o[f][3]], a_col);
        };

        const ImVec2 q0(a_min.x - bx, a_min.y - by), q1(a_max.x + bx, a_max.y + by);
        if (n == 1) {
            one(q0, q1, a_key);
            return;
        }
        // ★The two halves OVERLAP by a tenth. Butted exactly they leave a seam
        // the eye finds immediately -- one straight edge in a shape that has no
        // other straight edge anywhere.
        if (stackY) {
            const float mid = (q0.y + q1.y) * 0.5f, ov = (q1.y - q0.y) * 0.05f;
            one(q0, ImVec2(q1.x, mid + ov), a_key);
            one(ImVec2(q0.x, mid - ov), q1, a_key * 7919u + 13u);
        } else {
            const float mid = (q0.x + q1.x) * 0.5f, ov = (q1.x - q0.x) * 0.05f;
            one(q0, ImVec2(mid + ov, q1.y), a_key);
            one(ImVec2(mid - ov, q0.y), q1, a_key * 7919u + 13u);
        }
    }

    // ★★winBg IS the paper. Not a colour behind it -- the sheet is opaque and
    // Apply() hands ImGui a transparent WindowBg on any paper skin, so a value
    // that merely sat behind would be dead. It states what the sheet must READ
    // as on screen, and the tint that gets there is winBg / the file's own
    // average. Two things follow, and both are the point:
    //   · the SETTINGS chip and the window now come from one number, so they
    //     cannot drift apart the way a chip painted from a token and a window
    //     painted from a file always could;
    //   · swapping the sheet for a lighter or darker photograph leaves the skin
    //     unchanged -- the divisor moves with the file.
    // ★A tint only multiplies: asking for a paper BRIGHTER than the file is
    // unanswerable and clamps. Said out loud once per sheet rather than
    // silently, because the symptom is a token you can edit with no effect.
    ImU32 PaperTint()
    {
        // ★Asked once per window per frame, and the answer only moves when the
        // skin changes or the art is reloaded — so it is resolved on those two
        // events, not on the map's string hash every time. Same shape as
        // LightItemShadow's cache, plus the reload generation, because a sheet
        // swapped under a live skin changes the divisor.
        static int   s_skin = -1;
        static int   s_gen  = -1;
        static ImU32 s_tint = IM_COL32_WHITE;
        // SkinIndex(), not g_skin — the default may not be resolved yet, and
        // caching against 0 would key this on a skin that never renders.
        const int skin = SkinIndex();
        if (s_skin == skin && s_gen == g_inkGen) return s_tint;
        s_skin = skin;
        s_gen  = g_inkGen;
        s_tint = IM_COL32_WHITE;

        const Skin& sk = S();
        if (!sk.paper || !*sk.paper) return s_tint;
        const auto* sh = InkSheetFor(sk.paper);
        if (!sh) return s_tint;

        const float want[3] = { sk.winBg.x * 255.0f, sk.winBg.y * 255.0f,
                                sk.winBg.z * 255.0f };
        int   out[3]{};
        bool  clamped = false;
        for (int i = 0; i < 3; ++i) {
            const float m = (sh->mean[i] > 1.0f) ? sh->mean[i] : 1.0f;
            float t = want[i] / m;
            if (t > 1.0f) { t = 1.0f; clamped = true; }
            out[i] = static_cast<int>(t * 255.0f + 0.5f);
        }
        if (clamped) {
            SKSE::log::warn("[THEME] {} asks for paper brighter than {} "
                            "(want {:.0f} {:.0f} {:.0f}, sheet {:.1f} {:.1f} {:.1f}) "
                            "— a tint cannot add light, so winBg is capped",
                            sk.name, sk.paper, want[0], want[1], want[2],
                            sh->mean[0], sh->mean[1], sh->mean[2]);
        }
        s_tint = IM_COL32(out[0], out[1], out[2], 255);
        return s_tint;
    }

    bool PaperTexture(ImTextureID& a_out, ImVec2 a_size, unsigned int a_key,
                      ImVec2& a_uv0, ImVec2& a_uv1)
    {
        const char* nm = S().paper;
        if (!nm || !*nm) return false;
        const auto* ic = InkArt(nm);
        if (!ic) return false;
        a_out = reinterpret_cast<ImTextureID>(ic->srv);

        // The largest piece of the sheet that has the window's aspect. Working
        // in uv space means the texture's pixel size never enters the result,
        // so a sheet swapped for a bigger one needs no numbers changed here.
        const float tw = static_cast<float>((std::max)(ic->w, 1));
        const float th = static_cast<float>((std::max)(ic->h, 1));
        const float want = (a_size.y > 0.5f) ? (a_size.x / a_size.y) : (tw / th);
        float u = 1.0f, v = 1.0f;
        if (tw / th > want) {
            u = (th * want) / tw;      // sheet is wider than the window: trim u
        } else {
            v = (tw / want) / th;      // ...taller: trim v
        }
        // ★Slide the cut, do not centre it. Centring gives every window the
        // same square inch, and the bags open in a row -- five identical
        // sheets side by side is the one thing a paper texture must not be.
        // Two cheap mixes off the key: enough to decorrelate, and stable, so a
        // window keeps its own piece while it is open and after it moves.
        const float fx = static_cast<float>((a_key * 2654435761u) >> 8 & 0xFFFF) / 65535.0f;
        const float fy = static_cast<float>((a_key * 40503u + 12345u) >> 8 & 0xFFFF) / 65535.0f;
        a_uv0 = ImVec2((1.0f - u) * fx, (1.0f - v) * fy);
        a_uv1 = ImVec2(a_uv0.x + u, a_uv0.y + v);
        return true;
    }

    bool PaperPanel(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max,
                    unsigned int a_key, float a_fade)
    {
        ImTextureID tex{};
        ImVec2 uv0, uv1;
        const ImVec2 size(a_max.x - a_min.x, a_max.y - a_min.y);
        if (!a_dl || size.x < 4.0f || size.y < 4.0f) return false;
        if (!PaperTexture(tex, size, a_key, uv0, uv1)) return false;

        // A 3x3 of quads: the middle is the sheet, the ring around it is the
        // same sheet fading to nothing. Written as a vertex grid rather than
        // nine AddImage calls because the alpha has to vary ACROSS a quad, and
        // AddImage carries one colour for the whole thing.
        const float f = (std::min)({ a_fade, size.x * 0.45f, size.y * 0.45f });
        const float xs[4] = { a_min.x, a_min.x + f, a_max.x - f, a_max.x };
        const float ys[4] = { a_min.y, a_min.y + f, a_max.y - f, a_max.y };
        // uv follows the same stops, so the fade band shows the sheet's own
        // pixels rather than a stretched copy of the edge row.
        const float du = (uv1.x - uv0.x), dv = (uv1.y - uv0.y);
        const float us[4] = { uv0.x, uv0.x + du * (f / size.x),
                              uv1.x - du * (f / size.x), uv1.x };
        const float vs[4] = { uv0.y, uv0.y + dv * (f / size.y),
                              uv1.y - dv * (f / size.y), uv1.y };
        const int   a[4]  = { 0, 255, 255, 0 };   // transparent at the outside
        // the skin's own paper colour, reached by multiplying the sheet
        const ImU32 tint  = PaperTint() & 0x00FFFFFFu;

        a_dl->PushTexture(tex);
        a_dl->PrimReserve(9 * 6, 16);
        const unsigned int base = a_dl->_VtxCurrentIdx;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                const unsigned int i = base + static_cast<unsigned int>(r * 4 + c);
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(i));
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1));
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 5));
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(i));
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 5));
                a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 4));
            }
        }
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                // a corner of the ring is transparent on BOTH axes, so take
                // the weaker of the two -- otherwise the corners stay opaque
                // and the sheet keeps its square.
                const int al = (std::min)(a[c], a[r]);
                a_dl->PrimWriteVtx(ImVec2(xs[c], ys[r]), ImVec2(us[c], vs[r]),
                                   tint | (static_cast<ImU32>(al) << IM_COL32_A_SHIFT));
            }
        }
        a_dl->PopTexture();
        return true;
    }

    void ReloadInkArt()
    {
        for (auto& [k, sh] : g_paper) {
            if (sh.ic.srv) sh.ic.srv->Release();
            if (sh.ic.tex) sh.ic.tex->Release();
        }
        const size_t n = g_paper.size();
        g_paper.clear();
        ++g_inkGen;
        SKSE::log::info("[THEME] ink art dropped ({} sheet(s))", n);
    }

    ImU32 OccupiedGround()
    {
        // ★ONE answer for the board, the doll and the partner window. They used
        // to each reach for sk.shade with their own alpha, so the same colour
        // said "occupied" loudly on one half of the window and almost nothing
        // on the other.
        //
        // ★★DERIVING THIS WAS TRIED AND REVERTED — twice, and the second try
        // was measured. Do not reach for it again without reading this.
        //
        // The complaint that started it is real: on the darkest skins the
        // occupied cell lands near #101010 and a black-edged iron sprite has
        // nothing to sit against. And the numbers looked damning — sixteen of
        // the eighteen dark skins make the occupied cell DARKER than the empty
        // one, and the value nobody chose (an absolute rgba(0,0,0,0.5) wash
        // over whatever happens to be underneath).
        //
        // Two derivations were built anyway:
        //   1. empty cell + N% toward the ink.  On SIMPLE the panel is a light
        //      frame with cells carved into it, so one step up from the cell
        //      landed ON the panel: the grid dissolved into a flat sheet.
        //   2. panel ± N%, direction chosen by relative luminance, both numbers
        //      swept for the best worst-case contrast (1.37-1.93:1 against the
        //      panel AND the empty cell on all nineteen). It measured better
        //      than the originals on every axis and still looked WORSE.
        //
        // ★★Why: these skins are not flat colour, they are a MATERIAL. Cells
        // are recessed wells (SIMPLE even has groove machinery for it), and an
        // occupied cell is a deeper well — the ground under a thing. Both
        // derivations turned it into a raised, lighter plate on most skins,
        // which is a different material language, and the contrast numbers
        // cannot see that. The hand-tuned values are carrying something the
        // one-number rule does not encode.
        //
        // ★So the ground is the wrong lever. Legibility of a dark sprite on a
        // dark ground belongs to the SPRITE: 1.0.5 already ships a shadow
        // (distance/blur/opacity) for the mirror case, a pale item on a pale
        // panel. The same machinery with a light backing is the untried
        // candidate, and it leaves every skin's material alone.
        // (An earlier mock "rejected" that idea, but it drew a crude circular
        // glow rather than the real shadow renderer, so it rejected a straw man.)
        return Col(S().shade);
    }

    bool LightItemShadow()
    {
        // ★★Named, not computed, and not indexed either.
        // Not COMPUTED because the ground's luminance gets it wrong twice: the
        // GLASS pair shows the game world through the cell, so what the halo
        // has to read against is not a colour this table knows; and Silver's
        // board is pale enough that a white halo fogs it rather than lifting
        // the sprite. Those are looked-at judgements, not measurements.
        // Not INDEXED because a skin inserted or reordered would silently hand
        // the light halo to its neighbour. A name survives both.
        static const char* const kLight[] = {
            "Fable Crimson", "Parchment Crimson",
            "Simple Charcoal", "Simple Graphite", "Simple Pine",
            "Simple Forest", "Simple Petrol", "Simple Steel", "Simple Sky",
            "Simple", "Simple Plum", "Simple Violet", "Simple Indigo",
            "Simple Wine", "Simple Strawberry Milk",
        };
        // asked per sprite, so answer it once per skin change
        static int  s_for = -1;
        static bool s_val = false;
        if (s_for != SkinIndex()) {   // resolves the default first — see EnsureSkin
            s_for = SkinIndex();
            s_val = false;
            const char* nm = S().name;
            for (const char* n : kLight) {
                if (nm && std::strcmp(n, nm) == 0) { s_val = true; break; }
            }
        }
        return s_val;
    }

    // ---- metrics -------------------------------------------------------------
    float PadX() { return 12.0f * g_scale; }
    float PadY() { return 8.0f * g_scale; }

    // ★★Skin::rounding == 0 means "this skin does not name one", NOT "square".
    // Windows and frames want different amounts — 6px on a 700px window is a
    // soft corner, the same 6px on a 17px button is a lozenge — so the default
    // is a PAIR, and a skin that sets `rounding` overrides both with its one
    // value (that is the "single knob" the header speaks of).
    float WinRounding()
    {
        const float r = S().rounding;
        return r > 0.0f ? r : 6.0f;
    }

    float FrameRounding()
    {
        const float r = S().rounding;
        return r > 0.0f ? (std::min)(r, 4.0f) : 3.0f;
    }

    // larger for the torn frame: the panel is inset from the window edge by
    // the ragged margin, so content must clear it. translucent skins get a
    // small margin too — with zero inset every separator ran edge-to-edge.
    float FrameInsetX()
    {
        const Skin& sk = S();
        return sk.tornFrame ? 22.0f * g_scale : sk.translucent ? 10.0f * g_scale : 0.0f;
    }

    float FrameInsetY()
    {
        const Skin& sk = S();
        return sk.tornFrame ? 24.0f * g_scale : sk.translucent ? 6.0f * g_scale : 0.0f;
    }

    float TitleTopPad()
    {
        // ★★EVERY SKIN, deliberately. This began as clearance for the two
        // treatments whose top chrome reaches into the bar -- Sumi's brush frame
        // and Fable's crimson strip -- and was then made the rule everywhere at
        // the author's request, so a title sits the same way on all of them.
        // ★What it does, stated plainly, because it is NOT a centring tweak:
        // the band grows by this and the title moves down by all of it, so the
        // gap ABOVE the title exceeds the gap below it by exactly this number.
        // Without it the title is dead centre in its band (the half-inset rule
        // in WinManager::TitleBar is exact centring); with it, every skin is
        // offset the same way instead.
        return 8.0f * g_scale;
    }

    float TopControlRightPad()
    {
        return FrameInsetX() + PadX() * g_scale + 2.0f * g_scale;
    }

    float BorderPx() { return (std::max)(1.0f, std::round(g_scale)); }

    ImU32 WinBorder()
    {
        // ★OPAQUE on purpose: two translucent frames stacked on one pixel row
        // blend to a darker line, which is the doubled seam the snap overlap
        // exists to remove.
        const Skin& sk = S();
        return Col(LP(sk.lpBorder, Rgba(13, 32, 46)), 1.0f);
    }

    float BorderOverlap()
    {
        // ★A window's right frame sits at right-0.5 and its neighbour's left
        // frame at left+0.5. They coincide when left = right-1 — whatever the
        // stroke width. Overlapping by the full stroke put the two lines a
        // pixel apart and the join went back to two.
        return S().lightPanel ? 1.0f : 0.0f;
    }

    ImU32  BtnOnInk()    { const Skin& sk = S();
                           return sk.lightPanel ? Col(LP(sk.lpBtnOnInk, kBtnOnInk)) : Val(); }
    ImVec4 BtnOnInkVec() { const Skin& sk = S();
                           return sk.lightPanel ? LP(sk.lpBtnOnInk, kBtnOnInk) : ValVec(); }
    ImU32  TitleInk()    { const Skin& sk = S(); return Col(sk.ink, 1.0f); }

    ImU32 BtnOn(float a_alpha)
    {
        const Skin& sk = S();
        return sk.lightPanel ? Col(LP(sk.lpBtnOnFace, kBtnOnFace), a_alpha)
                             : Acc(0.28f * a_alpha);
    }

    // tooltip palette — read off the reference tooltip, one colour per KIND
    // of fact rather than one per emphasis level
    namespace
    {
        // ★★★ONE COLOUR PER KIND OF FACT — and it took a measurement to notice
        // that the palette had stopped meaning that. Six tokens existed, and of
        // the 40 coloured lines in a tooltip, 24 were the SAME grey: the
        // armour rating, the price, and "right-click to open" all read alike,
        // so nothing in the card had a shape the eye could aim at. TipHead,
        // TipBad and TipBody were down to one use each.
        //
        // The kinds, and what each is for:
        //   Body   the item's own name, and running text (descriptions,
        //          effects) -- the brightest thing, because it is the subject
        //   Head   CLASSIFICATION and MONEY: what kind of thing this is
        //          (Body / Heavy Armour) and what it is worth. Gold reads as
        //          coin on a board whose coins are already gold
        //   Val    NUMBERS you compare -- damage, armour, charge, cells
        //   Good   what the item does FOR you: temper, enchantment
        //   State  a fact about this copy that is neither number nor rarity:
        //          read, exhibited, the soul inside a gem
        //   Bad    what blocks you: a merchant's refusal, a full bag
        //   Sub    the quietest line there is -- hints and placeholders,
        //          dimmed further so it stops competing with the facts
        const ImVec4 kTipHead  = Rgba(232, 200, 110);   // classification, money
        const ImVec4 kTipVal   = Rgba(111, 200, 240);   // numbers
        const ImVec4 kTipGood  = Rgba(127, 201, 138);   // temper, enchantment
        const ImVec4 kTipBad   = Rgba(232, 106, 106);   // restriction, overload
        // ★★The SAME purple the museum wedge uses (Grid.cpp kRelicOwe). Two
        // places on one screen say "the museum still wants this", and they now
        // say it in one colour -- the mark on the tile and the line on the card
        // are the same fact, so a player learns the colour once.
        const ImVec4 kTipState = Rgba(169, 123, 232);   // read / exhibited / soul
        // ★Dimmed from 151,163,172. It used to carry two thirds of the card, so
        // it had to be readable as a primary; now it carries hints only, and a
        // hint that competes with a stat is a hint drawn too bright.
        const ImVec4 kTipSub   = Rgba(120, 132, 142);   // hints, placeholders
        const ImVec4 kTipBody  = Rgba(230, 237, 242);   // name, running text

        // ★The tooltip's CHROME, named once. PushTipStyle pushes these and
        // TipBg/TipBorder/... hand the same values to whoever has to paint the
        // panel by hand (Theme.h). While they were literals inside PushTipStyle
        // the compare card could not reach them, so it invented its own.
        const ImVec4    kTipGround = { 0.055f, 0.063f, 0.075f, 0.96f };
        const ImVec4    kTipEdge   = { 0.62f, 0.66f, 0.70f, 0.45f };
        constexpr float kTipRound  = 3.0f;
        const ImVec2    kTipPad    = { 10.0f, 8.0f };
    }

    const ImVec4& TipHead() { return kTipHead; }
    const ImVec4& TipVal()  { return kTipVal; }
    const ImVec4& TipGood() { return kTipGood; }
    const ImVec4& TipBad()  { return kTipBad; }
    const ImVec4& TipState(){ return kTipState; }
    const ImVec4& TipSub()  { return kTipSub; }
    const ImVec4& TipBody() { return kTipBody; }

    ImU32  TipBg()       { return ImGui::GetColorU32(kTipGround); }
    ImU32  TipBorder()   { return ImGui::GetColorU32(kTipEdge); }
    float  TipRounding() { return kTipRound; }
    ImVec2 TipPadding()  { return kTipPad; }

    void PushTipStyle()
    {
        // ★The tooltip goes DARK on every skin. Over a light panel a tooltip
        // painted in the same blue lands on the window it describes and
        // dissolves — it needs an edge of its own.
        ImGui::PushStyleColor(ImGuiCol_PopupBg, kTipGround);
        ImGui::PushStyleColor(ImGuiCol_Border, kTipEdge);
        ImGui::PushStyleColor(ImGuiCol_Text, kTipBody);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kTipRound);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kTipPad);
    }

    void PopTipStyle()
    {
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
    }

    // ---- one lighting model --------------------------------------------------
    ImU32 BevelLit(bool a_window)
    {
        // light from the TOP-LEFT everywhere. a_window drops to about two
        // thirds: the same alpha reads far stronger along a long edge.
        return IM_COL32(255, 255, 255, a_window ? 34 : 50);
    }

    ImU32 BevelShd(bool a_window)
    {
        return IM_COL32(0, 0, 0, a_window ? 52 : 78);
    }

    bool InkNeedsOutline()
    {
        // ★Asked of the INK, not of a flag: the outline exists to lift LIGHT
        // ink off a mid-tone panel. Dark ink on a pale panel is already the
        // strongest thing there, and an edge in the same value range just
        // fattens every stroke until the word reads as a blob.
        const ImVec4& c = S().ink;
        const float lum = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
        return lum > 0.55f;
    }

    // ---- type scale ----------------------------------------------------------
    float SnapPx(float a_size)
    {
        // ★Whole pixels. The atlas is baked at integer sizes, and asking for
        // 17.6 gets a scaled blit of the 18 — which is exactly the smearing
        // the bake was meant to avoid.
        // ★★TWO scales, and they are different things. g_scale sizes the
        // PANEL from the display; g_fontScale sizes what is written on it,
        // and is the player's. It has to land HERE, not only on the baked
        // atlas: every string that asks for an explicit size (values,
        // captions, the stat column) goes straight to the draw list, where
        // ImGui's own font scale never reaches it. Without this term the text
        // -size setting moved the running text and left the numbers alone —
        // and while a drag was in flight the two families sat at different
        // sizes on top of each other, which is what a doubled, ghosted string
        // actually was.
        return (std::max)(1.0f, std::round(a_size * g_scale * g_fontScale));
    }

    float SnapAbs(float a_px)
    {
        // ★★The same rounding WITHOUT the scale, and the reason it exists:
        // SnapPx multiplies by g_scale, so handing it a number that is
        // already a pixel count SQUARES the scale. That was invisible for
        // as long as the scale was a constant 0.90 — everything simply came
        // out 10% small and was tuned around, once, by eye. It stopped
        // being invisible the moment 1.2.1 derived the scale from the
        // screen: at 4K the scale is 1.5 and 1.5² is 2.25, which is a title
        // bar whose name no longer fits it and three controls drawn half as
        // wide again as the boxes they were measured into.
        return (std::max)(1.0f, std::round(a_px));
    }

    float FontValue()   { return SnapPx(20.0f); }
    float FontBody()    { return SnapPx(17.0f); }
    float FontCaption() { return SnapPx(12.0f); }

    // ★★The title follows the text-size setting like everything else, but it
    // is the one string with a CEILING, and the ceiling is the bar it lives
    // in: WinManager::TitleBarH is 34 design units of LAYOUT, and every
    // window's height is measured against it. A 24px name grown to 38 in a
    // bar that stayed 34 does not read as larger text, it reads as a name
    // falling out of its bar.
    // ★30 rather than 34: TitleBar centres the GLYPH BOX in the band, so this
    // leaves 2px of band above and below. Titles are drawn uppercase, so the
    // ink inside that box is cap-height and clears it with room to spare.
    float FontTitle()
    {
        return (std::min)(SnapPx(S().titleSize), SnapAbs(30.0f * g_scale));
    }

    ImU32 Rule()
    {
        const Skin& sk = S();
        // ★A rule on a LIGHT panel has to be dark. White at .42 measures
        // 1.74:1 — it is not a divider, it is a smudge.
        return sk.lightPanel ? Col(LP(sk.lpRule, kRuleInk), 0.70f) : Acc(0.25f);
    }

    // ★★A gauge on a LIGHT panel fills UPWARD in brightness. Everywhere else
    // the track is a dark well with an accent fill, but on a light panel the
    // accent IS the darkest token — a track drawn that way reads as a groove
    // cut into the panel, and the filled part is indistinguishable from the
    // empty part. The ON-button face is already the skin's "brightest, means
    // active" colour, so the fill borrows it and the whole control reads at a
    // glance.
    ImU32 GaugeTrack()
    {
        const Skin& sk = S();
        return sk.lightPanel ? IM_COL32(255, 255, 255, 38) : IM_COL32(0, 0, 0, 51);
    }

    ImU32 GaugeFill()
    {
        const Skin& sk = S();
        // ★WHITE on a light panel, not the skin's brightest token. A gauge is
        // read at a glance, and the only thing that survives being that small
        // against a mid-tone panel is the value the panel cannot reach — a
        // tinted fill has to compete with the panel's own hue.
        //
        // ★★HALF strength, because the VALUE sits on top of it. At full white
        // a filled slider swallowed its own number, and worst of all at half
        // fill: the digits were sliced down the middle, half of them legible.
        // Something read halfway is more distracting than something not read.
        // The capacity bar keeps full white (UIRoot) — nothing is written over
        // that one, so it has no reason to give any brightness back.
        if (sk.lightPanel) return IM_COL32(255, 255, 255, 107);
        return sk.translucent ? Col(sk.sel, 0.55f) : Acc(0.20f);
    }

    ImU32 GaugeBorder()
    {
        const Skin& sk = S();
        if (sk.lightPanel) return Col(LP(sk.lpBorder, Rgba(39, 80, 106)), 0.75f);
        return sk.translucent ? Col(sk.sel, 0.60f) : Acc(0.25f);
    }

    // ---- text ----------------------------------------------------------------
    ImVec2 TrackedSize(const char* a_text, float a_size, float a_spacing)
    {
        if (!a_text || !*a_text) return ImVec2(0.0f, 0.0f);
        ImFont* f = ImGui::GetFont();
        const float sz = a_size > 0.0f ? a_size : ImGui::GetFontSize();
        if (a_spacing <= 0.0f) return f->CalcTextSizeA(sz, FLT_MAX, 0.0f, a_text);
        float w = 0.0f;
        int   n = 0;
        for (const char* p = a_text; *p; ) {
            unsigned int cp = 0;
            const int adv = ImTextCharFromUtf8(&cp, p, nullptr);
            p += adv > 0 ? adv : 1;
            char buf[8] = {};
            std::memcpy(buf, p - (adv > 0 ? adv : 1), static_cast<size_t>(adv > 0 ? adv : 1));
            w += f->CalcTextSizeA(sz, FLT_MAX, 0.0f, buf).x;
            ++n;
        }
        if (n > 1) w += a_spacing * static_cast<float>(n - 1);
        return ImVec2(w, sz);
    }

    void TextOutlined(ImDrawList* a_dl, ImVec2 a_pos, ImU32 a_col, const char* a_text,
                      float a_size, float a_spacing)
    {
        if (!a_dl || !a_text || !*a_text) return;
        ImFont*     f  = ImGui::GetFont();
        const float sz = a_size > 0.0f ? a_size : ImGui::GetFontSize();
        const ImU32 blk = IM_COL32(0, 0, 0, 190);
        auto put = [&](ImVec2 p, ImU32 c) {
            if (a_spacing <= 0.0f) {
                a_dl->AddText(f, sz, p, c, a_text);
                return;
            }
            // tracking needs the glyphs drawn one at a time
            float x = p.x;
            for (const char* q = a_text; *q; ) {
                unsigned int cp = 0;
                const int adv = ImTextCharFromUtf8(&cp, q, nullptr);
                const int n = adv > 0 ? adv : 1;
                char buf[8] = {};
                std::memcpy(buf, q, static_cast<size_t>(n));
                a_dl->AddText(f, sz, ImVec2(x, p.y), c, buf);
                x += f->CalcTextSizeA(sz, FLT_MAX, 0.0f, buf).x + a_spacing;
                q += n;
            }
        };
        if (InkNeedsOutline()) {
            put(ImVec2(a_pos.x - 1.0f, a_pos.y), blk);
            put(ImVec2(a_pos.x + 1.0f, a_pos.y), blk);
            put(ImVec2(a_pos.x, a_pos.y - 1.0f), blk);
            put(ImVec2(a_pos.x, a_pos.y + 1.0f), blk);
        }
        put(a_pos, a_col);
    }

    float GaugeStepW() { return 16.0f * g_scale; }

    bool GaugeEditing(const char* a_id)
    {
        // ★GetID resolves against the CURRENT id stack, which is the same stack
        // the widget will be submitted on -- so this is the widget's own id,
        // asked one line early.
        return a_id && ImGui::TempInputIsActive(ImGui::GetID(a_id));
    }

    void GaugeInputFrame(ImDrawList* a_dl, const ImVec2& a_p, float a_w, float a_h)
    {
        if (!a_dl) return;
        const Skin& sk = S();
        const ImVec2 p1(a_p.x + a_w, a_p.y + a_h);
        const float  r = FrameRounding();
        // ★Darker than the well, on either kind of panel. A text field is a
        // hole you put something into; the gauge's own well is a track with a
        // level in it, and at a glance the two must not swap meanings.
        a_dl->AddRectFilled(a_p, p1,
            sk.lightPanel ? IM_COL32(0, 0, 0, 92) : IM_COL32(0, 0, 0, 130), r);
        // accent border, doubled -- this is the one row on the panel that is
        // currently listening to the keyboard, and it should look like it
        a_dl->AddRect(a_p, p1, BtnOn(1.0f), r, 0, 2.0f * g_scale);
    }

    void GaugeInputPushAlign(const char* a_id, float a_w)
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        float tw = 0.0f;
        // ★The text being typed lives in ImGui's own edit buffer, not in the
        // variable behind the slider -- the variable only catches up when the
        // entry is accepted. Measure what is actually on screen.
        if (a_id) {
            if (auto* s = ImGui::GetInputTextState(ImGui::GetID(a_id))) {
                const char* t = s->GetText();
                tw = ImGui::CalcTextSize(t, t + s->TextLen).x;
            }
        }
        // ★Never narrower than the normal padding: once the text is long enough
        // to fill the box this has to degrade back into an ordinary left-aligned
        // field, or ImGui's scroll-follow would fight the centring for the caret.
        const float pad = (std::max)(st.FramePadding.x, (a_w - tw) * 0.5f);
        // y untouched -- the row measured its height with the old padding, and
        // GetFrameHeight() is built from FramePadding.y.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad, st.FramePadding.y));
    }

    void GaugeInputPopAlign() { ImGui::PopStyleVar(); }

    const char* GaugeInputHint() { return Lang::T(Lang::Str::GaugeTyping); }

    void GaugeBar(ImDrawList* a_dl, const ImVec2& a_p, float a_w, float a_h,
                  float a_frac)
    {
        if (!a_dl) return;
        const float r = FrameRounding();
        const ImVec2 p1(a_p.x + a_w, a_p.y + a_h);
        // ★★INK: a laid stroke, not a filled well. Three marks -- the track,
        // the fill over it, and a thin red rule beneath -- which is what the
        // reference has and what the rest of this skin is made of. A rounded
        // rectangle in the middle of a painted window is the one shape that
        // says "widget" out loud.
        if (InkChrome()) {
            const float f = (std::max)(0.0f, (std::min)(1.0f, a_frac));
            const float th = (std::max)(2.0f, a_h * 0.70f);
            const float cy = a_p.y + a_h * 0.50f;
            // ★★THE ARROWS LIVE INSIDE THIS RECT. The rounded well this
            // replaces spanned the whole width and let its FILL start a step
            // in; the well was a background, so covering the arrow zone cost
            // nothing. A brush mark is not a background -- laid across the same
            // span it paints over the left arrow and buries it. Both the track
            // and the fill get the inner run, and the two steps stay bare.
            const float sw = GaugeStepW();
            const float ix = a_p.x + sw;
            const float iw = (std::max)(0.0f, a_w - sw * 2.0f);
            if (iw <= 1.0f) return;
            // ★★WHOLE, which is also what keeps it inside its own box. A
            // sectioned mark overshoots half a thickness at each end to close a
            // corner join -- on a settings slider that overhang reached back
            // over the row's left arrow and hid it. A whole mark ends in its
            // own taper and needs no overshoot.
            InkStroke(a_dl, ImVec2(ix, cy), iw, th, Col(S().ink, 0.26f),
                      false, /*whole=*/true);
            if (f > 0.0f) {
                InkStroke(a_dl, ImVec2(ix, cy), iw * f, th, Col(S().ink, 0.92f),
                          false, /*whole=*/true);
            }
            // ★No red rule under it. That mark belongs to the SPACE bar, where
            // the reference puts it; repeating it under every slider turned a
            // one-off accent into the loudest thing on the settings page.
            return;
        }
        a_dl->AddRectFilled(a_p, p1, GaugeTrack(), r);
        const float sw = GaugeStepW();
        const float inner = (std::max)(0.0f, a_w - sw * 2.0f);
        const float f = (std::max)(0.0f, (std::min)(1.0f, a_frac));
        if (f > 0.0f && inner > 0.0f) {
            // ★Square, and inset on BOTH sides: the fill no longer touches the
            // frame, so rounding it would leave a sliver of well showing along
            // a corner it never reaches.
            a_dl->AddRectFilled(ImVec2(a_p.x + sw, a_p.y),
                                ImVec2(a_p.x + sw + inner * f, p1.y),
                                GaugeFill(), 0.0f);
        }
        a_dl->AddRect(a_p, p1, GaugeBorder(), r);
    }

    namespace
    {
        // One arrow. Draws itself and reports whether it fired this frame.
        // ★The glyph is DRAWN, not typed: a "<" from the atlas is a text
        // glyph whose weight and baseline follow the body font, and next to a
        // 10px number it read as punctuation rather than a control. Two
        // triangles are the same at every scale and every skin.
        bool StepArrow(ImDrawList* a_dl, const ImVec2& a_p, float a_h,
                       const char* a_id, bool a_right, bool a_atEnd)
        {
            const float w = GaugeStepW();
            ImGui::SetCursorScreenPos(a_p);
            ImGui::PushID(a_id);
            const bool hit = ImGui::InvisibleButton(a_right ? "##r" : "##l",
                                                    ImVec2(w, a_h));
            const bool hov = ImGui::IsItemHovered();
            const bool act = ImGui::IsItemActive();
            ImGui::PopID();

            const ImVec2 p1(a_p.x + w, a_p.y + a_h);
            // A face only under the cursor: an always-on plate would read as
            // two more chrome elements per row, and there are six rows.
            if (hov || act) {
                a_dl->AddRectFilled(a_p, p1,
                    act ? BtnOn(1.0f) : Col(S().lightPanel ? S().ink : S().hi, 0.22f),
                    FrameRounding());
            }
            // ★Greyed at the end of travel rather than hidden -- a control that
            // vanishes at the limit makes the row twitch as the value crosses
            // it, and the player is usually holding the button when it happens.
            const float a = a_atEnd ? 0.28f : (hov || act ? 1.0f : 0.62f);
            const ImU32 ink = act && !a_atEnd ? BtnOnInk() : Col(S().ink, a);
            const float cx = a_p.x + w * 0.5f, cy = a_p.y + a_h * 0.5f;
            const float s = 3.2f * g_scale;
            if (a_right) {
                a_dl->AddTriangleFilled(ImVec2(cx - s * 0.6f, cy - s),
                                        ImVec2(cx - s * 0.6f, cy + s),
                                        ImVec2(cx + s * 0.8f, cy), ink);
            } else {
                a_dl->AddTriangleFilled(ImVec2(cx + s * 0.6f, cy - s),
                                        ImVec2(cx + s * 0.6f, cy + s),
                                        ImVec2(cx - s * 0.8f, cy), ink);
            }
            return hit;
        }
    }

    bool GaugeStep(const ImVec2& a_p, float a_w, float a_h, const char* a_id,
                   float& a_val, float a_step, float a_lo, float a_hi)
    {
        auto* dl = ImGui::GetWindowDrawList();
        bool changed = false;
        // ★Repeat is ImGui's own (KeyRepeatDelay / KeyRepeatRate), so holding
        // an arrow behaves like holding a key anywhere else in the game.
        ImGui::PushButtonRepeat(true);
        if (StepArrow(dl, a_p, a_h, a_id, false, a_val <= a_lo + 1e-4f)) {
            a_val = (std::max)(a_lo, a_val - a_step);
            changed = true;
        }
        if (StepArrow(dl, ImVec2(a_p.x + a_w - GaugeStepW(), a_p.y), a_h, a_id,
                      true, a_val >= a_hi - 1e-4f)) {
            a_val = (std::min)(a_hi, a_val + a_step);
            changed = true;
        }
        ImGui::PopButtonRepeat();
        return changed;
    }

    bool GaugeStepInt(const ImVec2& a_p, float a_w, float a_h, const char* a_id,
                      int& a_val, int a_step, int a_lo, int a_hi)
    {
        float v = static_cast<float>(a_val);
        const bool ch = GaugeStep(a_p, a_w, a_h, a_id, v,
                                  static_cast<float>(a_step),
                                  static_cast<float>(a_lo),
                                  static_cast<float>(a_hi));
        if (ch) a_val = static_cast<int>(std::lround(v));
        return ch;
    }

    void TextOutlinedFlow(ImU32 a_col, const char* a_text, float a_size,
                          float a_spacing)
    {
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const ImVec2 ts = TrackedSize(a_text, a_size, a_spacing);
        TextOutlined(ImGui::GetWindowDrawList(), p, a_col, a_text,
                     a_size, a_spacing);
        ImGui::Dummy(ts);
    }

    void TextInkCentered(ImDrawList* a_dl, const ImVec2& a_p0, const ImVec2& a_p1,
                         ImU32 a_col, const char* a_text, float a_size)
    {
        if (!a_dl || !a_text || !*a_text) return;
        ImFont*     f  = ImGui::GetFont();
        const float sz = a_size > 0.0f ? a_size : ImGui::GetFontSize();
        const ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0.0f, a_text);
        // ★★Centre by the INK, not by the line box. ImGui reserves descender
        // room whether or not the string has one, so all-caps labels ride high
        // and x-height ones sit low — by a different amount each, which is why
        // no single nudge straightens them.
        // ★ImGui 1.92 moved glyph metrics behind ImFontBaked: a glyph's box is
        // only meaningful at a SIZE, so the font hands out a baked set per size
        // instead of one set that callers scaled themselves.
        // ★FindGlyph, not FindGlyphNoFallback: a string whose glyphs are all
        // missing would otherwise fall through to the line box and centre the
        // ImGui way — the exact behaviour this function exists to replace.
        ImFontBaked* baked = f->GetFontBaked(sz);
        float top = FLT_MAX, bot = -FLT_MAX;
        for (const char* p = a_text; *p; ) {
            unsigned int cp = 0;
            const int adv = ImTextCharFromUtf8(&cp, p, nullptr);
            p += adv > 0 ? adv : 1;
            const ImFontGlyph* g =
                baked ? baked->FindGlyph(static_cast<ImWchar>(cp)) : nullptr;
            // a space has no ink and would drag `top` to its own empty box
            if (g && g->Y1 > g->Y0) {
                top = (std::min)(top, g->Y0);
                bot = (std::max)(bot, g->Y1);
            }
        }
        if (top > bot) { top = 0.0f; bot = ts.y; }
        // ★NOT rounded. The caller's rect is already at whatever subpixel
        // position the layout put it, and snapping only the text moves the
        // label off the centre of the box it belongs to — by up to half a
        // pixel, in whichever direction, per label.
        const float cx = a_p0.x + ((a_p1.x - a_p0.x) - ts.x) * 0.5f;
        const float cy = a_p0.y + ((a_p1.y - a_p0.y) - (bot - top)) * 0.5f - top;
        a_dl->AddText(f, sz, ImVec2(cx, cy), a_col, a_text);
    }

    // ---- chrome widgets ------------------------------------------------------
    // ★★The slider's number, drawn by US so it can carry the black edge
    // ImGui cannot give it. On a light panel the fill is white and the ink
    // is white: at full fill the value vanished, and at HALF fill it was
    // sliced down the middle with one half legible — worse than gone,
    // because a half-read string keeps pulling the eye back.
    // Centred on the whole track, which is where ImGui put it.
    // ★No longer file-local: the EDIT panel draws its own tracks and needs
    // this half on its own. See the note in Theme.h.
    void GaugeValue(ImDrawList* a_dl, const ImVec2& a_p, float a_w, float a_h,
                    const char* a_fmt, float a_v, bool a_isInt)
    {
        if (!a_dl || !a_fmt) return;
        char buf[64];
        if (a_isInt) {
            std::snprintf(buf, sizeof(buf), a_fmt, static_cast<int>(a_v));
        } else {
            std::snprintf(buf, sizeof(buf), a_fmt, a_v);
        }
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        TextOutlined(a_dl,
            ImVec2(a_p.x + (a_w - ts.x) * 0.5f, a_p.y + (a_h - ts.y) * 0.5f),
            Val(), buf);
    }

    bool ChromeSliderInt(const char* a_id, int* a_v, int a_min, int a_max,
                         float a_w, const char* a_fmt)
    {
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const float frac = a_max > a_min
            ? static_cast<float>(*a_v - a_min) / static_cast<float>(a_max - a_min)
            : 0.0f;
        // ★Asked BEFORE the widget: the answer decides whether the text is
        // pushed transparent, and that cannot be undone after submission.
        const bool typing = GaugeEditing(a_id);
        if (typing) GaugeInputFrame(dl, p, a_w, h);
        else        GaugeBar(dl, p, a_w, h, frac);
        if (typing) GaugeInputPushAlign(a_id, a_w);   // keep the figure centred
        PushChromeStyle(true);
        // ★ImGui draws the value itself and cannot outline it, so it draws
        // NOTHING (transparent ink) and we put the number back below. See the
        // float version for why the outline matters.
        // ★★...but NOT while typing, or the characters go invisible too.
        if (!typing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        // ★★Without this the arrows are DEAD. ImGui gives hover to the item
        // submitted FIRST, and the slider covers the whole track — so a click
        // on an arrow reached the slider instead, and two quick clicks put it
        // into text-entry mode, which is what "the cursor started blinking and
        // the slider stopped working" was.
        ImGui::SetNextItemAllowOverlap();
        ImGui::SetNextItemWidth(a_w);
        const bool ch = ImGui::SliderInt(a_id, a_v, a_min, a_max, a_fmt,
            ImGuiSliderFlags_AlwaysClamp);
        if (!typing) ImGui::PopStyleColor();
        PopChromeStyle(true);
        if (typing) {
            GaugeInputPopAlign();
            return ch;           // no painted value, no arrows over a text field
        }
        GaugeValue(dl, p, a_w, h, a_fmt, static_cast<float>(*a_v), true);
        // arrows last, so they sit above the widget that owns the whole track
        return GaugeStepInt(p, a_w, h, a_id, *a_v, 1, a_min, a_max) || ch;
    }

    bool ChromeSliderFloat(const char* a_id, float* a_v, float a_min, float a_max,
                           float a_w, const char* a_fmt, float a_resetTo, float a_step)
    {
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const float frac = a_max > a_min
            ? (std::max)(0.0f, (std::min)(1.0f, (*a_v - a_min) / (a_max - a_min)))
            : 0.0f;
        const bool typing = GaugeEditing(a_id);   // before the widget — see the int version
        if (typing) GaugeInputFrame(dl, p, a_w, h);
        else        GaugeBar(dl, p, a_w, h, frac);
        if (typing) GaugeInputPushAlign(a_id, a_w);   // keep the figure centred
        PushChromeStyle(true);
        if (!typing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::SetNextItemAllowOverlap();   // or the arrows are dead — see above
        ImGui::SetNextItemWidth(a_w);
        bool ch = ImGui::SliderFloat(a_id, a_v, a_min, a_max, a_fmt,
            ImGuiSliderFlags_AlwaysClamp);
        if (!typing) ImGui::PopStyleColor();
        PopChromeStyle(true);
        if (typing) {
            GaugeInputPopAlign();
            return ch;
        }
        GaugeValue(dl, p, a_w, h, a_fmt, *a_v, false);
        // ★Right-click restores the default. One gesture, defined here so every
        // settings slider has it without each row remembering to add it.
        // ★★BEFORE the arrows: IsItemHovered reads the LAST item, and the
        // arrows are items too. Adding them above this line would have pointed
        // the reset at whichever arrow was drawn last, so right-clicking a
        // track silently stopped working at both ends of it.
        if (a_resetTo > -1.0e8f && ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            *a_v = a_resetTo;
            ch = true;
        }
        // One step is a hundredth of a fine range, a tenth of a medium one, and
        // a whole unit once the range is wide enough that hundredths would take
        // all day (angles run -180..180).
        // ★A caller may name its own, for a range where the derived step is
        // too fine to do anything: the text size spans 0.75, so it took the
        // hundredth — and a hundredth of it does not move the rendered size by
        // a whole pixel, so most presses of the arrow visibly did nothing.
        const float span = a_max - a_min;
        const float step = a_step > 0.0f ? a_step
                         : span >= 100.0f ? 1.0f
                         : span >= 10.0f  ? 0.1f
                                          : 0.01f;
        if (GaugeStep(p, a_w, h, a_id, *a_v, step, a_min, a_max)) ch = true;
        return ch;
    }

    void PushChromeStyle(bool a_sliderGrab)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);   // chrome owns the border
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.04f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        if (a_sliderGrab) {
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ValVec());
    }

    void PopChromeStyle(bool a_sliderGrab)
    {
        ImGui::PopStyleColor(a_sliderGrab ? 6 : 4);
        ImGui::PopStyleVar();
    }

    void CornerFadeEdges(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max,
                         ImU32 a_top, ImU32 a_left, ImU32 a_right, ImU32 a_bottom,
                         float a_frac)
    {
        const auto t = [](ImU32 a_c) { return a_c & 0x00FFFFFFu; };   // alpha 0
        const float fw = (a_max.x - a_min.x) * a_frac;
        const float fh = (a_max.y - a_min.y) * a_frac;
        // top
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_min.y),
            ImVec2(a_min.x + fw, a_min.y + 1.0f), a_top, t(a_top), t(a_top), a_top);
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - fw, a_min.y),
            ImVec2(a_max.x, a_min.y + 1.0f), t(a_top), a_top, a_top, t(a_top));
        // bottom
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_max.y - 1.0f),
            ImVec2(a_min.x + fw, a_max.y), a_bottom, t(a_bottom), t(a_bottom), a_bottom);
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - fw, a_max.y - 1.0f),
            ImVec2(a_max.x, a_max.y), t(a_bottom), a_bottom, a_bottom, t(a_bottom));
        // verticals inset 1px so the corner pixel never double-blends (v10.3)
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_min.y + 1.0f),
            ImVec2(a_min.x + 1.0f, a_min.y + 1.0f + fh), a_left, a_left, t(a_left), t(a_left));
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_max.y - 1.0f - fh),
            ImVec2(a_min.x + 1.0f, a_max.y - 1.0f), t(a_left), t(a_left), a_left, a_left);
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - 1.0f, a_min.y + 1.0f),
            ImVec2(a_max.x, a_min.y + 1.0f + fh), a_right, a_right, t(a_right), t(a_right));
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - 1.0f, a_max.y - 1.0f - fh),
            ImVec2(a_max.x, a_max.y - 1.0f), t(a_right), t(a_right), a_right, a_right);
    }

    void CornerFade(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col, float a_frac)
    {
        CornerFadeEdges(a_dl, a_min, a_max, a_col, a_col, a_col, a_col, a_frac);
    }

    namespace
    {
        // value noise on an INTEGER lattice — the shape depends on how far
        // along the edge we are, not on where the window sits, so dragging a
        // window does not make its paper crawl
        [[nodiscard]] float Hash01(int a_i, unsigned int a_seed)
        {
            unsigned int x = static_cast<unsigned int>(a_i) * 374761393u + a_seed * 668265263u;
            x = (x ^ (x >> 13)) * 1274126177u;
            return static_cast<float>((x ^ (x >> 16)) & 0xFFFFu) / 65535.0f;
        }
        [[nodiscard]] float VNoise(float a_x, unsigned int a_seed)
        {
            const float fl = std::floor(a_x);
            const int   i  = static_cast<int>(fl);
            float f = a_x - fl;
            const float a = Hash01(i, a_seed), b = Hash01(i + 1, a_seed);
            f = f * f * (3.0f - 2.0f * f);
            return a + (b - a) * f;
        }
        // how deep the paper is bitten at distance t along an edge, in px
        // ★Periods and depth both scale with the UI, so the tearing keeps its
        // proportions — but NOT with the window, which is the whole point.
        [[nodiscard]] float TornDepth(float a_t, unsigned int a_seed, float a_s)
        {
            const float n = 0.50f * VNoise(a_t / (2.750f * a_s), a_seed)
                          + 0.32f * VNoise(a_t / (1.075f * a_s), a_seed + 7u)
                          + 0.18f * VNoise(a_t / (0.425f * a_s), a_seed + 19u);
            // a deeper bite now and then; without it the edge reads as fur
            const float bite =
                (std::max)(0.0f, VNoise(a_t / (10.25f * a_s), a_seed + 53u) - 0.86f) * 2.2f;
            return (n + bite) * 7.0f * a_s;
        }
    }

    void TornPanel(ImDrawList* a_dl, const ImVec2& a_min, const ImVec2& a_max,
                   ImU32 a_col, unsigned int a_seed)
    {
        if (!a_dl) return;
        const float S = Scale();
        a_dl->AddRectFilled(a_min, a_max, a_col);   // the flat sheet
        const float step = 3.0f * S;
        auto run = [&](ImVec2 a_org, ImVec2 a_dir, ImVec2 a_nrm, float a_len,
                       unsigned int a_s) {
            for (float t = 0.0f; t < a_len; ) {
                const float t2 = (std::min)(t + step, a_len);
                const float d1 = TornDepth(t, a_s, S);
                const float d2 = TornDepth(t2, a_s, S);
                const ImVec2 q[4] = {
                    { a_org.x + a_dir.x * t,  a_org.y + a_dir.y * t },
                    { a_org.x + a_dir.x * t2, a_org.y + a_dir.y * t2 },
                    { a_org.x + a_dir.x * t2 + a_nrm.x * d2,
                      a_org.y + a_dir.y * t2 + a_nrm.y * d2 },
                    { a_org.x + a_dir.x * t  + a_nrm.x * d1,
                      a_org.y + a_dir.y * t  + a_nrm.y * d1 },
                };
                a_dl->AddConvexPolyFilled(q, 4, a_col);
                t = t2;
            }
        };

        const float w = a_max.x - a_min.x, h = a_max.y - a_min.y;
        run(ImVec2(a_min.x, a_min.y), ImVec2(1, 0), ImVec2(0, -1), w, a_seed);
        run(ImVec2(a_min.x, a_max.y), ImVec2(1, 0), ImVec2(0,  1), w, a_seed + 101u);
        run(ImVec2(a_min.x, a_min.y), ImVec2(0, 1), ImVec2(-1, 0), h, a_seed + 211u);
        run(ImVec2(a_max.x, a_min.y), ImVec2(0, 1), ImVec2( 1, 0), h, a_seed + 307u);
    }

    void Apply()
    {
        const Skin& sk = S();
        auto& style = ImGui::GetStyle();
        style.WindowRounding    = WinRounding();
        style.ChildRounding     = WinRounding();
        style.PopupRounding     = WinRounding();
        style.FrameRounding     = FrameRounding();
        style.GrabRounding      = FrameRounding();
        style.TabRounding       = FrameRounding();
        // ★The scrollbar was the one metric left on ImGui's defaults: a 9px
        // pill 14px wide, while every other number here is scaled and every
        // other corner follows the skin. At 2x that is a hairline beside a 96px
        // cell. Both come from the same knobs as the rest of the chrome now.
        // ★The partner window sizes its gutter from ScrollbarSize (LootBarter
        // `sbW`), so its width follows this on its own -- no second place to
        // keep in step.
        style.ScrollbarSize     = 14.0f * g_scale;
        style.ScrollbarRounding = FrameRounding();
        // cornerFade / tornFrame replace the full window border — kill the
        // geometry, not just the colour (decisive regardless of style state)
        style.WindowBorderSize  = (sk.cornerFade || sk.tornFrame || sk.bevelChrome) ? 0.0f : 1.0f;
        // mockup: every field/button/checkbox carries a visible border —
        // without it they read as invisible black boxes (v10.5 feedback)
        // ★Stays 1px. Widening it to hide a fill that bleeds past a rounded
        // corner was the wrong fix twice over: 2px made every small control
        // heavy, and the 1.5px in between was WORSE than 1 — baked-texture AA
        // only accepts integer widths, so a fractional stroke drops to the
        // geometric path where the solid core is (width - 1). The bleed is
        // fixed at the source instead: Sfx::Button insets the fill.
        style.FrameBorderSize   = 1.0f;
        // ★KEEP baked-texture AA lines ON. Turning it off to help the outline
        // pass (a baked line's uvs differ, so the pass read borders as glyphs)
        // cost every 1px border its definition: the geometric path spreads one
        // line across the centre vertex plus a transparent vertex 1px out on
        // each side, so on a half-pixel boundary the ink splits between two
        // pixels and neither reaches the colour asked for. Borders went pale
        // everywhere. The outline pass now recognises baked lines by their
        // constant v instead — see BuildTextOutline.
        style.AntiAliasedLinesUseTex = true;
        // ★-4px of side padding on a light panel. The frame is drawn ON the
        // window edge here rather than inset, so the content already reads as
        // held in; 12px on top of that pushed every column away from its own
        // border. Vertical stays — the title bar sets that rhythm.
        style.WindowPadding     = ImVec2(PadX(), PadY());
        style.ItemSpacing       = ImVec2(8.0f, 6.0f);

        auto mix = [&](float a) { ImVec4 c = sk.acc; c.w = a; return c; };
        auto* c = style.Colors;
        ImVec4 win = sk.winBg;
        // opaque: the parked capture model hides behind windows. translucent
        // skins opt out — their park point is covered by the caching card.
        if (!sk.translucent) win.w = 1.0f;
        // tornFrame: Theme::TornPanel paints the (opaque) fill instead, so the
        // ImGui bg rect must be transparent or it squares off the tears
        // ★An ink skin paints a SHEET for the same reason a torn one paints a
        // silhouette: ImGui's own rect would sit on top of it. Same branch,
        // same reason -- keep them together so neither is fixed alone.
        c[ImGuiCol_WindowBg]         = (sk.tornFrame || sk.paper) ? ImVec4(0, 0, 0, 0) : win;
        // transparent: children must not paint over the window's frame chrome
        c[ImGuiCol_ChildBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        // ★★A popup is a READING surface. `win` carries the skin's own alpha,
        // so on Glass (0.58 / 0.38) the tooltip inherited it and the text had
        // the whole world showing through it. The WINDOW may be glass; the
        // card you read on top of it may not.
        c[ImGuiCol_PopupBg]          = ImVec4(win.x, win.y, win.z, 0.94f);
        // Border colours FRAMES (fields/buttons/checkbox — mockup .field/.abtn);
        // the skin-2 WINDOW border is killed via WindowBorderSize = 0 instead
        c[ImGuiCol_Border]           = mix(0.40f);
        // ★Everything stays WHITE. Contrast comes from the black outline
        // (21:1 whatever the panel does), not from flipping the ink dark —
        // dark labels next to white button text read as two different UIs.
        // Widget text carries the no-outline tag (Theme::Plain): ImGui draws
        // it and cannot outline it, so anything that needs the outline is
        // drawn by us through Theme::TextOutlined instead.
        c[ImGuiCol_Text]             = sk.ink;
        c[ImGuiCol_TextDisabled]     = sk.inkDim;
        // ★A black wash on a PALE panel comes out as dirty grey — sliders and
        // text fields read as smudges rather than as sunken tracks. A light
        // skin sinks its fields with a much lighter touch.
        c[ImGuiCol_FrameBg]          = sk.lightPanel ? ImVec4(0, 0, 0, 0.10f)
                                                     : ImVec4(0, 0, 0, 0.25f);
        c[ImGuiCol_FrameBgHovered]   = mix(0.12f);
        c[ImGuiCol_FrameBgActive]    = mix(0.18f);
        // ★A recessed button is a FACE colour, not an accent wash: the
        // reference paints it DARKER than the chrome it sits on, which an
        // alpha of the border colour can only approximate by accident.
        // btnFace alpha 0 = the skin does not use it, keep the wash.
        if (sk.btnFace.w > 0.0f) {
            auto lift = [&](float a) {
                return ImVec4(sk.btnFace.x + (sk.hi.x - sk.btnFace.x) * a,
                              sk.btnFace.y + (sk.hi.y - sk.btnFace.y) * a,
                              sk.btnFace.z + (sk.hi.z - sk.btnFace.z) * a,
                              sk.btnFace.w);
            };
            c[ImGuiCol_Button]        = sk.btnFace;
            c[ImGuiCol_ButtonHovered] = lift(0.28f);
            c[ImGuiCol_ButtonActive]  = lift(0.48f);
        } else {
            c[ImGuiCol_Button]        = mix(0.10f);
            c[ImGuiCol_ButtonHovered] = mix(0.22f);
            c[ImGuiCol_ButtonActive]  = mix(0.30f);
        }
        c[ImGuiCol_Header]           = mix(0.16f);
        c[ImGuiCol_HeaderHovered]    = mix(0.22f);
        c[ImGuiCol_HeaderActive]     = mix(0.28f);
        c[ImGuiCol_SliderGrab]       = mix(0.45f);
        c[ImGuiCol_SliderGrabActive] = ValVec();
        c[ImGuiCol_CheckMark]        = ValVec();
        c[ImGuiCol_Separator]        = sk.lightPanel ? ImVec4(sk.ink.x, sk.ink.y, sk.ink.z, 0.42f)
                                                     : mix(0.25f);
        c[ImGuiCol_Tab]              = mix(0.06f);
        c[ImGuiCol_TabHovered]       = mix(0.20f);
        c[ImGuiCol_TabSelected]      = mix(0.16f);
        c[ImGuiCol_TitleBg]          = win;
        c[ImGuiCol_TitleBgActive]    = win;
        // ★★★THE TRACK PAINTS NOTHING. `win` here was an opaque rect stamped
        // over whatever the skin had already drawn -- and ImGui rounds it with
        // the CHILD rounding, so it arrives with corners of its own. On the
        // skins that paint their own sheet (paper / tornFrame / ink) WindowBg is
        // deliberately transparent for exactly this reason, but the scrollbar
        // ignored that and laid a flat cream slab down the gutter; where the
        // sheet is shaded, its rounded BOTTOM corner read as a stray border
        // under the bar (user report, merchant window). On a translucent skin it
        // double-coated the glass instead, darkening that one column.
        // Only the MERCHANT ever showed it, because only the merchant scrolls.
        // Transparent is the one value that is right on every skin: where the
        // window is opaque `win` the track was that same colour anyway, so
        // nothing there changes.
        c[ImGuiCol_ScrollbarBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_ScrollbarGrab]    = mix(0.25f);
        // ★These two were never set, so the grab flashed ImGui's DEFAULT opaque
        // grey the moment the cursor touched it -- the one widget on screen
        // wearing no skin at all. Same accent, same ladder as every other
        // hover/active pair above.
        c[ImGuiCol_ScrollbarGrabHovered] = mix(0.38f);
        c[ImGuiCol_ScrollbarGrabActive]  = mix(0.50f);

        // bevelChrome (kept for future skins): dark translucent beveled
        // buttons/fields so white ink reads — acc-tinted fills read flat on
        // a translucent grey ground
        if (sk.bevelChrome) {
            // ★An OFF button shows the panel through it — no face of its own,
            // just the frame. Painting winBg here would not match the panel, it
            // would DOUBLE it: the button sits on the panel, so a second coat
            // of the same translucent colour lands at 0.90 instead of 0.68.
            // Transparent is the only fill that actually equals the ground.
            // ★A mint face, not grey. A grey control on an all-blue panel was
            // the one object on screen with no hue in common with anything
            // else, and it read as borrowed from another program.
            c[ImGuiCol_Button]         = sk.lightPanel
                ? LP(sk.lpBtn, Rgba(62, 110, 134)) : ImVec4(0.32f, 0.32f, 0.34f, 0.85f);
            c[ImGuiCol_ButtonHovered]  = sk.lightPanel
                ? LP(sk.lpBtnHov, Rgba(78, 132, 158)) : ImVec4(0.42f, 0.42f, 0.44f, 0.90f);
            c[ImGuiCol_ButtonActive]   = sk.lightPanel
                ? LP(sk.lpBtnAct, Rgba(48, 90, 112)) : ImVec4(0.26f, 0.26f, 0.28f, 0.92f);
            c[ImGuiCol_FrameBg]        = ImVec4(0.16f, 0.16f, 0.18f, 0.45f);
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.24f, 0.55f);
            c[ImGuiCol_FrameBgActive]  = ImVec4(0.28f, 0.28f, 0.30f, 0.65f);
            c[ImGuiCol_Border]         = sk.lightPanel
                ? LP(sk.lpBorder, Rgba(39, 80, 106)) : ImVec4(0.24f, 0.24f, 0.26f, 0.85f);
            c[ImGuiCol_PopupBg]        = ImVec4(win.x, win.y, win.z, 0.92f);
        }
    }
}
