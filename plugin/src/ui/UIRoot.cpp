#include "ui/UIRoot.h"
#include "ui/Editor.h"
#include "ui/Equip.h"
#include "game/Costume.h"
#include "game/DeltaWatch.h"
#include "game/DualRing.h"
#include "game/GoldCoins.h"
#include "ui/Loadout.h"
#include "ui/GridMenu.h"
#include "ui/Fallback.h"
#include "ui/Grid.h"
#include "ui/LootBarter.h"
#include "ui/IconCache.h"
#include "ui/ItemPreview.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/Wheeler.h"
#include "ui/WinManager.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>   // ClearActiveID (drop text-field focus on close)

#include <bit>            // countr_zero (pad button bookkeeping)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <filesystem>

// imgui_impl_win32.h leaves this for the app to declare (per its docs)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ImGui bootstrap/render-loop structure ported from ModExplorerMenu (Modex)
// by patchulidev — UIManager.cpp (GPL-3.0 with Modding Exception).
// Visuals implement the v9 mockup (design contract).

namespace FUI::UIRoot
{
    namespace
    {
        std::atomic<bool>     g_initialized = false;
        std::function<void()> g_onShow;
        std::function<void()> g_onHide;

        ImVec2          g_scrollEnergy = ImVec2(0.0f, 0.0f);
        constexpr float kScrollMultiplier = 1.5f;
        constexpr float kScrollSmoothing  = 10.0f;

        ImFont* g_fontMain = nullptr;
        ImFont* g_fontBold = nullptr;   // latin + hangul only, see BoldFont()

        // icon-brightness UP pass. GI57: the >1 gain used to be ADDITIVE
        // (dst + t*src) — bright pixels received the most, dark ones almost
        // nothing, which read as "glow" rather than "brighter" (user report).
        // What "brighter" means here is FILL LIGHT: lift the dim, keep the
        // highlights. Screen-style blend does exactly that with no shader:
        //   out = dst + t*src*(1 - dst)
        // (src carries t in the vertex COLOR; captures are effectively
        // premultiplied — transparent texels are black — so the pass is
        // alpha-gated for free and pure black albedo stays black.)
        ID3D11BlendState* g_fillBlend = nullptr;

        void FillLightBlendCB(const ImDrawList*, const ImDrawCmd*)
        {
            auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
            auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
            if (ctx && g_fillBlend) {
                const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                ctx->OMSetBlendState(g_fillBlend, bf, 0xFFFFFFFF);
            }
        }

        // ★Mip-enabled sampler for the WHOLE ImGui pass. The DX11 backend's
        // own sampler is created with MaxLOD = 0, which locks every draw to
        // mip level 0 — the icon textures now carry a full CPU-built chain
        // (IconCache) precisely so a ~250px capture shown in a ~40px cell is
        // sampled from the right level instead of aliasing. Bound once at the
        // head of the background draw list each frame; textures without mips
        // (fonts, frames) sample identically under it, so one sampler serves
        // everything.
        ID3D11SamplerState* g_mipSampler = nullptr;

        void MipSamplerCB(const ImDrawList*, const ImDrawCmd*)
        {
            auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
            auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
            if (ctx && g_mipSampler) {
                ctx->PSSetSamplers(0, 1, &g_mipSampler);
            }
        }

        void CreateMipSampler(ID3D11Device* a_device)
        {
            if (g_mipSampler) return;
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            // ★★CLAMP, not WRAP. Icons are drawn at uv 0..1, and a linear tap
            // at uv 0 wants texel -0.5 — under WRAP that is the texel at the
            // OPPOSITE edge, so the sprite's top row renders half-mixed with
            // its bottom row. Realistic sprites are alpha-trimmed, so their
            // edge rows are nearly transparent and it never showed; the PIXEL
            // style ends every sprite in an opaque 1-dot outline, and any
            // icon whose silhouette reaches the texture edge grew a straight
            // line across the empty cells above it (reported on Exploration
            // Pack, whose stacked books fill the bottom row solid).
            // Nothing here tiles — every AddImage/AddImageQuad in this plugin
            // passes uv inside 0..1 — so CLAMP is pixel-identical everywhere
            // else, including the font atlas.
            sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
            sd.MinLOD         = 0.0f;
            sd.MaxLOD         = D3D11_FLOAT32_MAX;   // the one field that differs
            a_device->CreateSamplerState(&sd, &g_mipSampler);
        }

        // ★★SILHOUETTE PASS. The item shadow re-stamps the sprite with a tint,
        // and a tint MULTIPLIES: black collapses the RGB and leaves the alpha
        // to draw the shape, which is why the shadow has always been black.
        // White multiplies to the sprite itself, so "a white shadow" came out
        // as eight offset copies of the item — a smear, not a halo (shipped
        // once, reported once). The alpha has to be read WITHOUT the colour,
        // and no blend state can do that. One tiny pixel shader can.
        //
        // Signature matches the ImGui DX11 backend's vertex output exactly, and
        // the return is NON-premultiplied because that is what the backend's
        // blend state (SRC_ALPHA / INV_SRC_ALPHA) expects.
        ID3D11PixelShader* g_silPS = nullptr;
        ID3D11PixelShader* g_prevPS = nullptr;   // the backend's, held over the pass

        void CreateSilhouettePS(ID3D11Device* a_device)
        {
            if (g_silPS) return;
            static const char kSrc[] =
                "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR0;"
                "               float2 uv : TEXCOORD0; };\n"
                "sampler sampler0;\n"
                "Texture2D texture0;\n"
                "float4 main(PS_IN i) : SV_Target\n"
                "{\n"
                "    float a = texture0.Sample(sampler0, i.uv).a * i.col.a;\n"
                "    return float4(i.col.rgb, a);\n"
                "}\n";
            ID3DBlob* blob = nullptr;
            ID3DBlob* err = nullptr;
            if (SUCCEEDED(D3DCompile(kSrc, sizeof(kSrc) - 1, nullptr, nullptr,
                                     nullptr, "main", "ps_4_0", 0, 0, &blob, &err))
                && blob) {
                a_device->CreatePixelShader(blob->GetBufferPointer(),
                                            blob->GetBufferSize(), nullptr, &g_silPS);
            } else {
                logger::warn("[UI] silhouette PS failed to compile: {}",
                    err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            }
            if (blob) blob->Release();
            if (err) err->Release();
        }

        void SilhouetteOnCB(const ImDrawList*, const ImDrawCmd*)
        {
            auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
            auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
            if (!ctx || !g_silPS) return;
            // ★Hand the backend's own shader back afterwards rather than
            // assuming what it was: ImGui may rebuild its device objects at any
            // point, and a cached pointer from startup would outlive them.
            ctx->PSGetShader(&g_prevPS, nullptr, nullptr);
            ctx->PSSetShader(g_silPS, nullptr, 0);
        }

        void SilhouetteOffCB(const ImDrawList*, const ImDrawCmd*)
        {
            auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
            auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
            if (ctx) ctx->PSSetShader(g_prevPS, nullptr, 0);
            if (g_prevPS) {   // PSGetShader AddRef'd it
                g_prevPS->Release();
                g_prevPS = nullptr;
            }
        }

        void CreateFillLightBlend(ID3D11Device* a_device)
        {
            if (g_fillBlend) return;
            D3D11_BLEND_DESC bd = {};
            bd.RenderTarget[0].BlendEnable           = TRUE;
            bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_INV_DEST_COLOR;
            bd.RenderTarget[0].DestBlend             = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            a_device->CreateBlendState(&bd, &g_fillBlend);
        }
        // B11: written from the render path, read from the message-queue path
        // — both are the main thread today, but atomics cost nothing and the
        // assumption is now explicit
        std::atomic<bool> g_showSettings = false;
        std::atomic<bool> g_textInputOn = false;   // ImGui WantTextInput mirror (no engine calls)

        // ---- INSPECT overlay (C key) ----
        // The rotation is euler, exactly like a def, so the whole capture path
        // (key hash, pin recycling, "rotation applied" gate) works unchanged:
        // horizontal drag -> RZ (screen-vertical axis), vertical -> RX,
        // Shift+drag -> RY. Gimbal-ish at extreme angles, but predictable and
        // it maps 1:1 onto what the EDIT sliders write.
        RE::TESBoundObject* g_inspObj = nullptr;
        std::string         g_inspKey;
        float g_inspRx = 0.0f, g_inspRy = 0.0f, g_inspRz = 0.0f;
        float g_inspRx0 = 0.0f, g_inspRy0 = 0.0f, g_inspRz0 = 0.0f;   // R resets here
        // ★GI70: the view OPENS fully zoomed out and the wheel only ever adds.
        // Opening at the middle of the range meant the first thing anyone did
        // was scroll back to see the whole object -- and on a greatsword or a
        // staff the ends were already off screen before they touched anything.
        // Zoom is for looking closer at a detail; the default should be the
        // shot that shows what the item IS.
        constexpr float kInspZoomMin = 0.5f;
        constexpr float kInspZoomMax = 3.5f;
        float           g_inspZoom   = kInspZoomMin;
        // Grid's C press and the overlay's C toggle run in the SAME ImGui frame
        // (grid draws first) — without this the view would open and shut at once
        int   g_inspOpenFrame = -1;
        bool  g_inspDrag = false;

        // The movie-less menu never receives GFxCharEvents (same reason the
        // raw I-key needed InputSink), so keyboard/char input is taken
        // straight off the game window — chained WndProc into the ImGui
        // Win32 backend (Modex-style). Mouse stays on the Scaleform relay.
        WNDPROC g_origWndProc = nullptr;
        HWND    g_gameWnd = nullptr;   // for ScreenToClient in MouseHandler
        // ★★Keystrokes reach ImGui through ONE road -- this chained window
        // proc -- and a report exists of that road being dead: no typing at all
        // in the search box or the preset rename, English, digits and Hangul
        // alike. The same player could not rotate with A/D either, which was
        // "fixed" only because that path grew a GetAsyncKeyState fallback that
        // bypasses ImGui entirely. Two symptoms, one cause: WM_CHAR / WM_KEYDOWN
        // never arriving.
        //
        // It cannot be reproduced here, so the build has to answer it by itself:
        // count what arrives, and say so when a text field has been waiting with
        // nothing coming.
        std::atomic<unsigned> g_wmCharSeen{ 0 };
        std::atomic<unsigned> g_wmKeySeen{ 0 };
        // Every message that reaches the thunk, of any kind. This is how you ask
        // "am I still in the chain" -- by whether you are being called, not by
        // comparing a pointer the OS may refuse to hand back (see below).
        std::atomic<unsigned> g_thunkMsgs{ 0 };
        // Key messages AT THE WINDOW, counted before our own guard: "did a key
        // message arrive at all" and "did we pass it on" are different
        // questions, and the report line below wants the first.
        std::atomic<unsigned> g_wmKeyRaw{ 0 };
        bool g_kbFallback = false;   // latched: synthesise characters from polling

        LRESULT CALLBACK WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            g_thunkMsgs.fetch_add(1, std::memory_order_relaxed);
            switch (m) {
            case WM_CHAR:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                // ★Counted BEFORE the guard: "did a key message reach the window"
                // and "did we let it through" are different questions, and the
                // fallback latch below needs the first one.
                g_wmKeyRaw.fetch_add(1, std::memory_order_relaxed);
                // ★★KEYS REACH US BY TWO ROADS, and only one of them was
                // watched. GridMenu::ProcessScaleformEvent drops key events
                // while the console is up — but this thunk is a SEPARATE,
                // earlier road straight off the game window, and it kept
                // handing every keystroke to ImGui. So typing into the console
                // still ran our hotkeys: R over a tile dropped the item, F
                // starred it. Blocking one road and calling it done is the
                // whole bug; the guard has to sit on every road.
                // (The mouse is deliberately NOT blocked — the console does not
                // use it, and freezing a window the player can still see is
                // worse than letting them point at it.)
                if (auto* ui = RE::UI::GetSingleton();
                    ui && ui->IsMenuOpen("GridInventoryMenu"sv) &&
                    !ui->IsMenuOpen(RE::Console::MENU_NAME) &&
                    ImGui::GetCurrentContext()) {
                    if (m == WM_CHAR) {
                        g_wmCharSeen.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_wmKeySeen.fetch_add(1, std::memory_order_relaxed);
                    }
                    ImGui_ImplWin32_WndProcHandler(h, m, w, l);
                }
                break;
            default:
                break;
            }
            return CallWindowProcA(g_origWndProc, h, m, w, l);
        }

        // ---- THE MISSING ROAD IS CHARACTERS, NOT KEYS ------------------------
        //
        // ★★★Keys and characters do NOT travel together here:
        //   keys  -> Scaleform GFx events   -> GridInventoryMenu::OnKeyEvent
        //   chars -> the chained window proc -> ImGui_ImplWin32_WndProcHandler
        //
        // OnCharEvent is deliberately empty ("chars come from the WndProc now"),
        // so keys have TWO roads and characters have exactly ONE. On a setup
        // where the window never receives key messages -- measured on a
        // reporter's machine as `chars 0 keys 0 msgs 2067`, our thunk called two
        // thousand times with not one keystroke among them -- every hotkey still
        // works and ONLY typing dies. Which is exactly what they reported:
        // "every key in the prompt bar works; the search box and preset rename
        // take nothing."
        //
        // (The same split explains why A/D rotation needed its own
        // GetAsyncKeyState fallback: in menu mode WASD arrives PRE-TRANSLATED
        // into arrow GFx codes, so the Scaleform road never carries an 'A' at
        // all -- see the arrow block in GridMenu::OnKeyEvent.)
        //
        // ★So this synthesises CHARACTERS ONLY. Emitting key events as well
        // would double every keystroke on that machine, because the Scaleform
        // road is alive and already delivering them.
        //
        // GetAsyncKeyState asks the OS for the physical key, with no window and
        // no focus in the question -- the one road that cannot be taken away.
        // The keys a name or a search term can be made of. Nothing else is
        // swept: this road exists for TEXT, and every other key already has one.
        bool TypedVk(int a_vk)
        {
            if (a_vk >= 'A' && a_vk <= 'Z') return true;
            if (a_vk >= '0' && a_vk <= '9') return true;
            if (a_vk >= VK_NUMPAD0 && a_vk <= VK_NUMPAD9) return true;
            switch (a_vk) {
            case VK_SPACE: case VK_OEM_7: case VK_OEM_COMMA: case VK_OEM_MINUS:
            case VK_OEM_PERIOD: case VK_OEM_2: case VK_OEM_1: case VK_OEM_PLUS:
            case VK_OEM_4: case VK_OEM_5: case VK_OEM_6: case VK_OEM_3:
            case VK_MULTIPLY: case VK_ADD: case VK_SUBTRACT: case VK_DECIMAL:
            case VK_DIVIDE:
                return true;
            default:
                return false;
            }
        }

        void PollTypedCharacters(ImGuiIO& a_io)
        {
            static bool     s_down[256] = {};
            static unsigned s_lastChars = 0;
            static int      s_contradictions = 0;

            // Only ever while a text field is waiting. Outside one there is
            // nothing to type into, and ToUnicodeEx is stateful (dead keys) --
            // calling it for every key at all times would leave half-composed
            // accents lying around for the next field that opens.
            if (!a_io.WantTextInput) {
                for (auto& d : s_down) d = false;
                return;
            }

            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

            const unsigned chars = g_wmCharSeen.load(std::memory_order_relaxed);
            const bool     wmAlive = chars != s_lastChars;
            s_lastChars = chars;

            BYTE ks[256] = {};
            if (g_kbFallback) {
                // ToUnicodeEx reads the WHOLE table, so the modifiers have to be
                // in it or every letter comes out lower case.
                if (shift) { ks[VK_SHIFT] = 0x80; ks[VK_LSHIFT] = 0x80; }
                if (ctrl)  { ks[VK_CONTROL] = 0x80; }
                if (alt)   { ks[VK_MENU] = 0x80; }
                if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 0x01;
            }

            for (int vk = 0; vk < 256; ++vk) {
                if (!TypedVk(vk)) continue;
                const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (now == s_down[vk]) continue;
                s_down[vk] = now;
                if (!now) continue;

                if (!g_kbFallback) {
                    // ★THE LATCH, and it is a CONTRADICTION rather than a
                    // timeout: a printable key went down while a text field was
                    // focused, and no WM_CHAR arrived for it. One such frame
                    // could be a race; three cannot. On a healthy setup this can
                    // never fire, so the road stays inert -- one sweep a frame
                    // while typing, and no events at all.
                    if (!wmAlive && ++s_contradictions >= 3) {
                        g_kbFallback = true;
                        SKSE::log::warn(
                            "[UI] input: printable keys are being pressed and no "
                            "WM_CHAR is arriving (chars {} rawKeys {} msgs {}). "
                            "Typing falls back to polled characters. NOTE: an IME "
                            "language cannot be composed this way -- IME needs the "
                            "window messages that are missing.",
                            chars, g_wmKeyRaw.load(std::memory_order_relaxed),
                            g_thunkMsgs.load(std::memory_order_relaxed));
                    }
                    continue;
                }

                if (ctrl || alt) continue;   // a shortcut, not a character
                const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
                WCHAR     buf[8] = {};
                const int n = ToUnicodeEx(static_cast<UINT>(vk), sc, ks, buf,
                                          static_cast<int>(std::size(buf)), 0,
                                          GetKeyboardLayout(0));
                for (int i = 0; i < n && i < static_cast<int>(std::size(buf)); ++i) {
                    if (buf[i] >= 0x20) a_io.AddInputCharacterUTF16(buf[i]);
                }
            }
        }
        // ★★★YOU CANNOT ASK A WINDOW WHETHER ITS PROC IS YOURS.
        //
        // A "self-heal" lived here for one build and crashed the game on the
        // first inventory open. It compared GetWindowLongPtrA against our thunk
        // -- and Skyrim's window is UNICODE, so when anything sets the proc with
        // the W entry point, the A getter stops returning an address and returns
        // an OPAQUE HANDLE instead (measured: 0xffff0923). That never equals our
        // pointer, so the check read "not ours" every single time, re-installed,
        // and stored the handle it got back as the next link in the chain.
        //
        // The handle it got back was OUR OWN THUNK. CallWindowProcA then called
        // us from inside us: infinite recursion, stack overflow, CTD.
        //
        // The measurement was still worth having -- it proved another mod hooks
        // after us with the W variant on a real setup, and that it CHAINS
        // correctly, because typing works there. So there was nothing to heal.
        // What remains below is observation only: counts of what actually
        // arrives. Liveness is a thing you measure, never a pointer you compare.

        // The atlas is built for the DISPLAY scale and for the glyph ranges
        // the active language pack wants -- those two, and nothing else, are
        // what this flag asks to be rebuilt. The player's text size is not in
        // here: it rides on style.FontScaleMain and needs no rebuild at all
        // (see BuildFonts).
        std::atomic<bool> g_fontsDirty = false;
        float             g_bakedScale = 1.0f;

        // ICON CACHE reset request (settings, two-click armed). Consumed in
        // Tick — SRVs must never be released inside the ImGui frame.
        std::atomic<bool> g_iconsReset = false;
        // GI47: a preset icon bundle waits to be merged (frame-outside).
        // The pak path rides in g_presetMergePak (same-thread handoff).
        std::atomic<bool> g_iconsMergePreset = false;
        std::string       g_presetMergePak;
        // drawn-icon folder re-read (settings). Same frame-outside rule: the
        // drawings it frees are in this frame's draw list until Render ends.
        std::atomic<bool> g_flatReload = false;
        // ...and how long its acknowledgement owns the prompt bar. The bar is
        // where this UI already answers "what just happened / what can I do",
        // so the confirmation belongs there rather than as a second label
        // growing out of the button and shoving the row's layout around.
        double g_flatReloadNote = 0.0;

        // (re)build the font atlas at 17px * UI scale. Call OUTSIDE the
        // NewFrame/Render pair only.
        // ★GI71: the glyph range the active language pack wants, or null when it
        // needs nothing beyond the built-in atlas. Presets cover what Windows
        // ships a face for; the explicit "0x0400-0x052F" form is the escape
        // hatch for anything else, because guessing a script's block boundaries
        // is exactly the sort of thing a translator can look up and we cannot.
        const ImWchar* PackRanges()
        {
            const char* r = Lang::FontRange();
            if (!r || !r[0]) return nullptr;
            auto& io = ImGui::GetIO();
            std::string s(r);
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (s == "cyrillic")   return io.Fonts->GetGlyphRangesCyrillic();
            if (s == "greek")      return io.Fonts->GetGlyphRangesGreek();
            if (s == "thai")       return io.Fonts->GetGlyphRangesThai();
            if (s == "vietnamese") return io.Fonts->GetGlyphRangesVietnamese();
            if (s == "latin-ext") {
                static const ImWchar kLatinExt[] = { 0x0020, 0x024F, 0 };
                return kLatinExt;
            }
            // explicit "0xAAAA-0xBBBB" (repeatable, comma separated)
            static ImVector<ImWchar> s_custom;
            s_custom.clear();
            size_t pos = 0;
            while (pos < s.size()) {
                const auto comma = s.find(',', pos);
                const auto part  = s.substr(pos, comma == std::string::npos
                                                     ? std::string::npos : comma - pos);
                const auto dash  = part.find('-');
                if (dash != std::string::npos) {
                    const auto lo = std::strtoul(part.substr(0, dash).c_str(), nullptr, 0);
                    const auto hi = std::strtoul(part.substr(dash + 1).c_str(), nullptr, 0);
                    if (lo > 0 && hi >= lo && hi <= 0xFFFF) {
                        s_custom.push_back(static_cast<ImWchar>(lo));
                        s_custom.push_back(static_cast<ImWchar>(hi));
                    }
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (s_custom.empty()) {
                SKSE::log::warn("[LANG] unrecognised #range '{}' - no extra glyphs baked", r);
                return nullptr;
            }
            s_custom.push_back(0);
            return s_custom.Data;
        }

        // ★Body text size. Measured against the 24px window title: at 17 the
        // title's caps came out 11px and a label's 7px, and a 1px outline that
        // is a thin edge on 11px eats the whole gap between strokes on 7px —
        // the label reads as a dark clot rather than an outlined word. 20
        // narrows that ratio and is the size the panel wanted anyway.
        constexpr float kBodyFont = 17.0f;

        // ★★THE TEXT-SIZE SETTING IS NOT IN HERE, and that is measured, not
        // preference. ImGui 1.92 sizes text as
        //     style.FontSizeBase * style.FontScaleMain
        // and FontSizeBase is seeded ONCE, from the first font's LegacySize:
        //
        //     if (g.Style.FontSizeBase <= 0.0f)                 // imgui.cpp
        //         g.Style.FontSizeBase = font->LegacySize;
        //
        // -- so rebuilding the atlas at a bigger size does NOT resize a single
        // string. It never did. Baking the player's setting in here would also
        // mean that on the one startup where the ini loaded BEFORE the first
        // bake, the seed carried the setting AND FontScaleMain multiplied it
        // again. The bake carries the DISPLAY scale, which is what seeds a
        // base; the player's multiplier rides on FontScaleMain, live.
        void BuildFonts()
        {
            auto& io = ImGui::GetIO();
            const float k = Theme::Scale();

            static ImVector<ImWchar> mainRanges;
            if (mainRanges.empty()) {
                ImFontGlyphRangesBuilder b;
                b.AddRanges(io.Fonts->GetGlyphRangesKorean());
                // gear, pencil, degree, em-dash, middot, ▾, ×, ∞ (F3 merchant gold)
                b.AddText("\xE2\x9A\x99\xE2\x9C\x8E\xC2\xB0\xE2\x80\x94\xC2\xB7\xE2\x96\xBE\xC3\x97\xE2\x88\x9E");
                b.BuildRanges(&mainRanges);
            }

            auto exists = [](const char* p) { return std::filesystem::exists(p); };
            const char* kMalgun = "C:\\Windows\\Fonts\\malgun.ttf";
            const char* kMalgunBd = "C:\\Windows\\Fonts\\malgunbd.ttf";
            const char* kYaHei  = "C:\\Windows\\Fonts\\msyh.ttc";
            const char* kMeiryo = "C:\\Windows\\Fonts\\meiryo.ttc";
            const char* kYuGoth = "C:\\Windows\\Fonts\\YuGothM.ttc";

            io.Fonts->Clear();
            g_fontMain = nullptr;
            g_fontBold = nullptr;

            if (exists(kMalgun)) {
                ImFontConfig base;
                base.OversampleH = 2;
                base.OversampleV = 2;   // default V=1 leaves dense hangul strokes rough
                g_fontMain = io.Fonts->AddFontFromFileTTF(kMalgun, kBodyFont * k, &base, mainRanges.Data);
                ImFontConfig mc;
                mc.MergeMode = true;
                mc.OversampleH = 2;
                mc.OversampleV = 2;
                if (exists(kYaHei)) {
                    io.Fonts->AddFontFromFileTTF(kYaHei, kBodyFont * k, &mc,
                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                }
                const char* jp = exists(kMeiryo) ? kMeiryo : (exists(kYuGoth) ? kYuGoth : nullptr);
                if (jp) {
                    io.Fonts->AddFontFromFileTTF(jp, kBodyFont * k, &mc, io.Fonts->GetGlyphRangesJapanese());
                }
                // malgun has no U+2699 (gear) / U+270E (pencil) — Segoe UI Symbol
                // supplies them (merge skips codepoints malgun already covers)
                const char* kSegSym = "C:\\Windows\\Fonts\\seguisym.ttf";
                static const ImWchar symRanges[] = { 0x2010, 0x2BFF, 0 };
                if (exists(kSegSym)) {
                    io.Fonts->AddFontFromFileTTF(kSegSym, kBodyFont * k, &mc, symRanges);
                }
                // ★GI71: whatever the active language pack asks for, merged LAST
                // so it only fills gaps. #range is the part that matters: the
                // atlas above bakes Latin + Hangul + CJK and nothing else, so a
                // Cyrillic pack draws tofu until it names the range — even when
                // the face already on disk has the glyphs. #font is optional;
                // omitted, the extra range is baked from the main face.
                if (const ImWchar* extra = PackRanges()) {
                    const char* pf = Lang::FontPath();
                    const char* face = (pf && pf[0] && exists(pf)) ? pf : kMalgun;
                    io.Fonts->AddFontFromFileTTF(face, kBodyFont * k, &mc, extra);
                }
                // ★A second, BOLD face -- the only honest way to get weight.
                // Redrawing a glyph at small offsets thickens it, but each copy
                // lands on its own subpixel phase, so the counters inside a
                // letter fill with half-tone and the word reads smeared rather
                // than heavy. Malgun ships a bold cut; use it.
                // ★No glyph range here, unlike the body face. This atlas bakes
                // on demand, so an unrestricted face costs only the characters
                // actually drawn -- and it keeps IsGlyphInFont() honest: with a
                // range set, the face can own a glyph the atlas is not allowed
                // to bake, and BoldFont() would hand back a font that draws
                // nothing.
                if (exists(kMalgunBd)) {
                    ImFontConfig bc;
                    bc.OversampleH = 2;
                    bc.OversampleV = 2;
                    g_fontBold = io.Fonts->AddFontFromFileTTF(kMalgunBd, kBodyFont * k, &bc);
                }
            } else {
                g_fontMain = io.Fonts->AddFontDefault();
            }

            io.Fonts->Build();
            ImGui_ImplDX11_InvalidateDeviceObjects();   // font texture recreates on NewFrame
            g_bakedScale = k;
        }

        // ---- gamepad (user report: "no pointer at all on a controller") ----
        //
        // The mod never owned a pointer: it borrowed the game's Cursor Menu and
        // read MenuCursor::cursorPos*. Both are mouse-only — in gamepad mode
        // the engine suppresses that cursor and never advances its position, so
        // the UI kept running with nothing to aim and no way to click.
        //
        // So on a controller we draw and drive our own: ImGui's software cursor
        // at a position WE integrate from the left stick, with the pad's face
        // buttons translated into the mouse/key events every existing call site
        // already understands. Nothing downstream needed to learn about pads.
        // ★What a pad button MEANS is asked of the engine, not hardcoded.
        //  ControlMap knows what this player bound each button to in OUR input
        //  context, so a remap (or a non-Xbox pad the engine maps differently)
        //  is followed for free. We only decide what each ACTION does to the
        //  grid — which is the part no engine could know, since vanilla's pad
        //  inventory is a list with no pointer at all.
        enum PadAction : std::uint32_t
        {
            kActPrimary   = 1u << 0,   // pick up / place      (left click)
            kActSecondary = 1u << 1,   // equip / read / bag   (right click)
            kActDrop      = 1u << 2,   // drop one / take all  (R)
            kActFavorite  = 1u << 3,   // toggle star          (F)
            kActInspect   = 1u << 4,   // 3D view              (C)
            kActSplit     = 1u << 5,   // stack split / compare (Shift)
            kActNudgeL    = 1u << 6,
            kActNudgeR    = 1u << 7,
            kActNudgeU    = 1u << 8,
            kActNudgeD    = 1u << 9,
            // ★②: rotation, the one thing on this board a pad could not do.
            // Everything else here already answers to a VANILLA binding -- the
            // engine names the button and the player's own remap decides it --
            // but turning an item is ours alone, so there is no game action to
            // inherit and these two are pinned to physical buttons.
            kActRotL      = 1u << 10,
            kActRotR      = 1u << 11,
            // ★LT's empty-cursor meaning. Both triggers used to carry the
            // split/compare modifier; RT keeps it (one modifier is enough),
            // and LT becomes the recharge key the board and the doll already
            // listen for as T -- the one hover verb a pad had no way to say.
            kActRecharge  = 1u << 12,
        };

        std::atomic<std::uint32_t> g_padRaw{ 0 };       // physical buttons held
        std::uint32_t              g_btnAction[32]{};   // action of each held bit
        std::atomic<std::uint32_t> g_padHeld{ 0 };      // OR of held actions
        std::uint32_t              g_padPrev = 0;       // actions consumed last frame
        std::atomic<float>         g_padMoveX{ 0.0f };   // left stick, deadzoned
        std::atomic<float>         g_padMoveY{ 0.0f };
        std::atomic<float>         g_padScrollY{ 0.0f };  // right stick
        std::atomic<bool>          g_padActive{ false };  // a pad drives the UI
        ImVec2                     g_padCursor{ 0.0f, 0.0f };
        // ★d-pad nudges accumulate here and are applied AFTER the frame's
        // position source has spoken -- in ENGINE mode the engine's read
        // overwrote g_padCursor every frame, so a nudge written directly
        // into it never survived to a pos event (the d-pad "stopped
        // working" for exactly the players whose engine drives the cursor).
        float                      g_padNudgeX = 0.0f;
        float                      g_padNudgeY = 0.0f;

        constexpr float kPadCursorSpeed = 1400.0f;   // px/s at full deflection
        constexpr float kPadScrollRate  = 26.0f;

        std::atomic<bool> g_padSeed{ false };   // park the cursor mid-screen once
        std::atomic<bool> g_padSuppressed{ false };   // the mouse took over

        // Who owns the pointer on a pad — decided ONCE by observation, never
        // per frame (a per-frame verdict is what made it blink).
        enum class PadCursorMode : std::uint8_t
        {
            kProbing = 0,   // offering the stick to the engine, watching
            kEngine,        // the game's own arrow follows: use it, draw nothing
            kOwn            // it does not: integrate and draw our own
        };
        PadCursorMode g_padCursorMode = PadCursorMode::kProbing;
        float         g_engineLastX = 0.0f;
        float         g_engineLastY = 0.0f;
        int           g_engineStillFrames = 0;
        bool              g_bookWasOpen = false;   // Book Menu edge (see Render)

        // Called from the input sink (game thread, but not the render pass) —
        // only flags are touched here; the cursor is seeded during the frame.
        // a_fromDevice: an actual pad event, which is the ONLY thing allowed to
        // lift a mouse takeover. The engine's mode flag must not, or the two
        // would keep handing the pointer back and forth.
        void MarkPadActive(bool a_fromDevice)
        {
            if (a_fromDevice) g_padSuppressed.store(false);
            if (!g_padActive.exchange(true)) g_padSeed.store(true);
        }

        // actions -> the input this UI is already built on
        void TranslatePadButtons()
        {
            auto& io = ImGui::GetIO();
            const std::uint32_t now = g_padHeld.load();
            const std::uint32_t changed = now ^ g_padPrev;
            if (changed == 0) return;

            const auto edge = [&](std::uint32_t a_bit) { return (changed & a_bit) != 0; };
            const auto down = [&](std::uint32_t a_bit) { return (now & a_bit) != 0; };

            // ★★★A POPUP SPEAKS KEYBOARD, SO THE PAD SPEAKS KEYBOARD TO IT.
            //
            // The quantity slider and the confirm dialogs listen for Enter /
            // Space / Escape and the arrow keys -- none of which any pad
            // button translated to. Every button below was a mouse button or
            // a board shortcut, so "take 40 gold" on a controller meant
            // steering the cursor onto each little button and clicking it
            // (user report). While one of those windows is up, the face
            // buttons become what vanilla's dialogs taught: A confirms, Y is
            // Max, the d-pad walks the count. B already cancels through the
            // user-event channel (ue->cancel closes the top window), so it
            // needs nothing here.
            //
            // ★Resolved at PRESS and remembered per button -- the same rule
            // the triggers follow. A popup that closes while A is held must
            // release the Enter it pressed, not a mouse button it never did.
            const bool modal = LootBarter::SliderActive() ||
                               LootBarter::ConfirmActive() ||
                               Grid::IsTrashConfirmOpen() ||
                               Equip::IsPopupOpen();
            static bool s_priAsEnter = false;
            static bool s_dropAsMax  = false;
            static bool s_nudgeLKey  = false;
            static bool s_nudgeRKey  = false;

            if (edge(kActPrimary)) {
                if (down(kActPrimary)) s_priAsEnter = modal;
                if (s_priAsEnter) io.AddKeyEvent(ImGuiKey_Enter, down(kActPrimary));
                else              io.AddMouseButtonEvent(0, down(kActPrimary));
            }
            if (edge(kActSecondary)) io.AddMouseButtonEvent(1, down(kActSecondary));
            if (edge(kActDrop)) {
                if (down(kActDrop)) s_dropAsMax = modal;
                // M is the slider's Max key (see DrawSlider); R stays the
                // board's drop/take-all everywhere else.
                io.AddKeyEvent(s_dropAsMax ? ImGuiKey_M : ImGuiKey_R, down(kActDrop));
            }
            if (edge(kActFavorite))  io.AddKeyEvent(ImGuiKey_F, down(kActFavorite));
            if (edge(kActInspect))   io.AddKeyEvent(ImGuiKey_C, down(kActInspect));
            if (edge(kActRotL))      io.AddKeyEvent(ImGuiKey_A, down(kActRotL));
            if (edge(kActRotR))      io.AddKeyEvent(ImGuiKey_D, down(kActRotR));
            // LT, empty cursor: the same T the recharge hover handlers read
            if (edge(kActRecharge))  io.AddKeyEvent(ImGuiKey_T, down(kActRecharge));
            if (edge(kActSplit)) {
                io.AddKeyEvent(ImGuiMod_Shift, down(kActSplit));
                io.AddKeyEvent(ImGuiKey_LeftShift, down(kActSplit));
            }
            // d-pad nudges exactly one cell — the only way to hit a specific
            // tile reliably without a mouse. In a popup the count is what
            // needs walking, not the cursor: left/right become the arrow keys
            // the slider already listens for (held, so key-repeat runs).
            const float step = Grid::CellPx();
            if (edge(kActNudgeL)) {
                if (down(kActNudgeL)) s_nudgeLKey = modal;
                if (s_nudgeLKey) io.AddKeyEvent(ImGuiKey_LeftArrow, down(kActNudgeL));
                else if (down(kActNudgeL)) g_padNudgeX -= step;
            }
            if (edge(kActNudgeR)) {
                if (down(kActNudgeR)) s_nudgeRKey = modal;
                if (s_nudgeRKey) io.AddKeyEvent(ImGuiKey_RightArrow, down(kActNudgeR));
                else if (down(kActNudgeR)) g_padNudgeX += step;
            }
            if (edge(kActNudgeU) && down(kActNudgeU)) g_padNudgeY -= step;
            if (edge(kActNudgeD) && down(kActNudgeD)) g_padNudgeY += step;

            g_padPrev = now;
        }

        // Ask the engine first; fall back to the physical layout for anything
        // the Inventory context leaves unbound (the triggers, typically).
        std::uint32_t ActionForButton(std::uint32_t a_idCode)
        {
            using K = RE::BSWin32GamepadDevice::Keys;
            auto* cm = RE::ControlMap::GetSingleton();
            auto* ue = RE::UserEvents::GetSingleton();
            if (cm && ue) {
                // ★Ask the contexts that actually CARRY gamepad bindings, in
                //  order. Our menu declares kInventory (so the Inventory key
                //  toggles it), but measured in game that context binds almost
                //  nothing for a pad — every button came back "(unbound)" and
                //  the whole mapping was silently running on the fallback
                //  below. Vanilla's own item screens live in kItemMenu /
                //  kMenuMode, which is where the real bindings are.
                using Ctx = RE::UserEvents::INPUT_CONTEXT_ID;
                std::string_view name{};
                for (const auto ctx : { Ctx::kItemMenu, Ctx::kMenuMode, Ctx::kInventory }) {
                    name = cm->GetUserEventName(a_idCode, RE::INPUT_DEVICE::kGamepad, ctx);
                    if (!name.empty()) break;
                }
                const auto is = [&](const RE::BSFixedString& a_ev) {
                    const char* s = a_ev.c_str();
                    return s && !name.empty() && name == s;
                };
                // Vanilla already has a name for most of what our grid does —
                // use ITS name, so the player's own binding decides the button.
                // Measured: vanilla's item screens hand the face buttons over
                // as GENERIC names (XButton / YButton) and let the menu decide
                // what they mean — exactly what we are doing. Only Accept /
                // Cancel and the equip actions carry a meaning of their own.
                if (is(ue->accept))         return kActPrimary;
                if (is(ue->equip) || is(ue->xButton)) return kActSecondary;
                if (is(ue->dropItem) || is(ue->takeAll) || is(ue->yButton)) return kActDrop;
                if (is(ue->toggleFavorite)) return kActFavorite;
                if (is(ue->itemZoom))       return kActInspect;
                if (is(ue->left))  return kActNudgeL;
                if (is(ue->right)) return kActNudgeR;
                if (is(ue->up))    return kActNudgeU;
                if (is(ue->down))  return kActNudgeD;
            }
            // Physical layout for the rest. The triggers deliberately stay
            // here: the item menu calls them LeftEquip / RightEquip, and
            // pinning our split modifier to THAT name would make it wander if
            // the player rebinds hand-equip. A trigger is a trigger.
            switch (a_idCode) {
            case K::kA:             return kActPrimary;
            case K::kX:             return kActSecondary;
            case K::kY:             return kActDrop;
            case K::kLeftShoulder:  return kActFavorite;
            case K::kRightShoulder: return kActInspect;
            // ★★THE TRIGGERS MEAN TWO THINGS, AND THE CURSOR SAYS WHICH.
            //
            // Holding an item, they ROTATE it. Otherwise they are the split /
            // compare modifier they have always been. The two can never want
            // the trigger at the same moment: splitting is something you do to
            // a tile you are picking UP, and comparing is something you do to a
            // tile you are hovering -- both are gestures of an empty cursor.
            // With something on the cursor, that meaning has nothing to act on
            // and the trigger is free.
            //
            // Resolved at PRESS and remembered (see NotePadButton), so a
            // trigger held across a pickup still releases the action it took,
            // rather than leaving Shift stuck down.
            //
            // ★Rotation is deliberately not looked up in ControlMap above:
            // there is no game action called "turn the thing you are holding",
            // so there would be nothing to ask for.
            // ★★LT's empty-cursor half is RECHARGE now (user ask), not a
            // second split modifier. RT alone carries split/compare -- the
            // two never disagreed anyway, so nothing is lost -- and the
            // labels below follow: kActSplit resolves to RT, recharge to LT.
            case K::kLeftTrigger:
                return Grid::IsHolding() ? kActRotL : kActRecharge;
            case K::kRightTrigger:
                return Grid::IsHolding() ? kActRotR : kActSplit;
            case K::kLeft:          return kActNudgeL;
            case K::kRight:         return kActNudgeR;
            case K::kUp:            return kActNudgeU;
            case K::kDown:          return kActNudgeD;
            default:                return 0;
            }
        }

        // Hint labels: which pad button ends up carrying each action. Answering
        // runs the binding lookup above (ControlMap + string compares) once per
        // button, so it is resolved on menu open and cached — KeyLabel() is
        // called every frame a tooltip is up.
        const char* g_padLabel[9]{};   // indexed by Act
        bool        g_padLabelReady = false;

        void ResolvePadLabels()
        {
            using K = RE::BSWin32GamepadDevice::Keys;
            // Xbox names, which is what vanilla prints too. A pad with another
            // layout still lands on the right BUTTON — the engine normalises
            // every controller onto these codes.
            static constexpr struct { std::uint32_t id; const char* name; } kBtn[] = {
                { K::kA, "A" }, { K::kB, "B" }, { K::kX, "X" }, { K::kY, "Y" },
                { K::kLeftShoulder, "LB" }, { K::kRightShoulder, "RB" },
                { K::kLeftTrigger, "LT" }, { K::kRightTrigger, "RT" },
                { K::kLeftThumb, "LS" }, { K::kRightThumb, "RS" },
                { K::kUp, "D-Up" }, { K::kDown, "D-Down" },
                { K::kLeft, "D-Left" }, { K::kRight, "D-Right" },
            };
            static constexpr std::uint32_t kWanted[] = {
                kActPrimary, kActSecondary, kActDrop,
                kActFavorite, kActInspect, kActSplit,
                // ★②: rotate has buttons now -- the triggers, while something
                // is on the cursor -- so the prompts can name them.
                kActRotL, kActRotR,
                // LT's empty-cursor half; the loop resolves it naturally
                // (ResolvePadLabels runs with an empty cursor).
                kActRecharge,
            };
            static_assert(std::size(kWanted) == std::size(g_padLabel));

            for (auto& s : g_padLabel) s = nullptr;
            for (const auto& b : kBtn) {
                const std::uint32_t act = ActionForButton(b.id);
                if (act == 0) continue;
                for (std::size_t i = 0; i < std::size(kWanted); ++i) {
                    // first button wins — both triggers carry kActSplit
                    if ((act & kWanted[i]) != 0 && !g_padLabel[i]) g_padLabel[i] = b.name;
                }
            }
            // ★The triggers answer differently depending on whether something is
            // on the cursor, and this runs once, at menu open, with an empty one
            // -- so the loop above can only ever have seen them as the split
            // modifier. Name their other meaning outright rather than resolving
            // a state-dependent binding at a moment whose state is known to be
            // the wrong one.
            for (std::size_t i = 0; i < std::size(kWanted); ++i) {
                if (kWanted[i] == kActRotL) g_padLabel[i] = "LT";
                if (kWanted[i] == kActRotR) g_padLabel[i] = "RT";
            }
            g_padLabelReady = true;
        }

        // ★★★THE POINTER IS OURS WHILE OUR WINDOW IS UP.
        //
        // The game's arrow is drawn by CursorMenu, and its depthPriority is 13
        // against our 11 (measured, [UI] CursorMenu depthPriority=13) -- so it
        // lands on top of everything we draw, tooltips included, and the first
        // line of a tooltip is the item's NAME.
        //
        // ImGui cannot place a tooltip clear of a cursor whose size it does not
        // know, and every cursor replacer ships a different one; 1.4.1 tried a
        // fixed clearance and it was either too small on one machine or absurd
        // on another. So the cursor becomes ours instead: the engine's movie is
        // hidden and ImGui draws its own, which is exactly the size ImGui's
        // tooltip placement was designed around.
        //
        // ★Draw ORDER is untouched -- no depthPriority is changed here, so the
        // console and message boxes keep their standing above us.
        //
        // ★Called every frame on purpose: CursorMenu can open AFTER we do (our
        // kUsesCursor flag is what opens it), so hiding once at OnShow misses.
        // No string is built here -- MENU_NAME is already a BSFixedString
        // (원칙 3).
        void SetGameCursorVisible(bool a_visible)
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            if (const auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME);
                menu && menu->uiMovie) {
                menu->uiMovie->SetVisible(a_visible);
            }
        }

        // ★★★THE POINTER ITSELF -- "Bone", picked from the mockups.
        //
        // Four corners and no tail: tip, heel, notch, shoulder. The body is
        // split along the tip-to-notch edge so the left face sits a shade
        // darker than the right, which is the whole of its relief.
        //
        // ★The keyline is deliberately heavy. A white pointer on parchment and
        // a white pointer on snow are both invisible without it -- Skyrim is
        // half snowfield, and that is the case this weight is paying for.
        //
        // Drawn on the FOREGROUND list, so it sits over every window we own
        // without touching any depthPriority.
        constexpr ImU32 kPtrBody   = IM_COL32(255, 255, 255, 255);
        constexpr ImU32 kPtrShade  = IM_COL32(222, 216, 201, 255);
        constexpr ImU32 kPtrEdge   = IM_COL32( 26,  23,  20, 255);
        // ★Height in px at scale 1.0, and everything else is derived from it --
        // the keyline thickens with the body rather than staying hairline on a
        // grown arrow. 22 was the first build and read too small in game; this
        // is that × 1.5, still well under the engine cursor it replaced.
        constexpr float kPtrHeight = 33.0f;

        void DrawPointer()
        {
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2   m  = io.MousePos;
            // ImGui parks an unknown position far off screen; do not draw there
            if (m.x < -1000.0f || m.y < -1000.0f) return;

            const float s = (kPtrHeight / 20.0f) * Theme::Scale();
            const ImVec2 tip  (m.x,                m.y);
            const ImVec2 heel (m.x,                m.y + 20.0f * s);
            const ImVec2 notch(m.x +  5.9f * s,    m.y + 14.0f * s);
            const ImVec2 shld (m.x + 13.3f * s,    m.y + 13.6f * s);

            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddTriangleFilled(tip, heel, notch, kPtrShade);
            dl->AddTriangleFilled(tip, notch, shld, kPtrBody);
            const ImVec2 poly[4] = { tip, heel, notch, shld };
            dl->AddPolyline(poly, 4, kPtrEdge, ImDrawFlags_Closed, 2.1f * s);
        }

        void MouseHandler()
        {
            auto& io = ImGui::GetIO();
            SetGameCursorVisible(false);   // ★ours for as long as we are up

            // ★Who owns the pointer is decided by the ENGINE's input mode and by
            //  real device events — NEVER by watching the OS cursor position.
            //  In gamepad mode the game parks that cursor at screen centre and
            //  keeps warping it back, so reading its movement as "the user
            //  grabbed the mouse" kicked us out of pad mode at random; the next
            //  stick event pulled us back in, and the pointer alternated
            //  between the stick position and dead centre every few frames.
            //  That is the tooltip/carried-item flicker at screen centre.
            if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
                const bool padMode = idm->IsGamepadEnabled();
                if (padMode && !g_padSuppressed.load()) {
                    MarkPadActive(false);
                } else if (!padMode) {
                    g_padActive.store(false);
                }
            }

            // Handing back to the mouse with a pad button still held would
            // leave ImGui with a button that is down forever — its release is
            // only translated while pad mode is on. Flush it here.
            if (!g_padActive.load() && g_padPrev != 0) {
                g_padRaw.store(0);
                g_padHeld.store(0);
                TranslatePadButtons();   // emits the releases
            }

            if (g_padActive.load()) {
                auto* mc = RE::MenuCursor::GetSingleton();
                auto* uiS = RE::UI::GetSingleton();
                const bool cursorUp =
                    mc && uiS && uiS->IsMenuOpen(RE::CursorMenu::MENU_NAME);

                const bool justSeeded = g_padSeed.exchange(false);
                if (justSeeded) {
                    // Take over from wherever the pointer already is, so the
                    // handover from mouse to stick has no jump.
                    g_padCursor = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
                    if (mc && mc->cursorPosX >= 0.0f && mc->cursorPosX < io.DisplaySize.x &&
                        mc->cursorPosY >= 0.0f && mc->cursorPosY < io.DisplaySize.y) {
                        g_padCursor = ImVec2(mc->cursorPosX, mc->cursorPosY);
                    }
                }

                const float mx = g_padMoveX.load();
                const float my = g_padMoveY.load();
                const bool  wanted = (mx != 0.0f || my != 0.0f);

                // ★Did the ENGINE advance its own cursor? That is the whole
                //  question, and it is cheaper to observe than to predict:
                //  FeedEngineCursor hands CursorMenu the real stick event, and
                //  if its own handler acts on it, cursorPos* moves by itself.
                //  Latch the answer once — deciding per frame is what made the
                //  pointer blink, since the verdict flipped with the Cursor
                //  Menu's own open/close.
                bool engineMoved = false;
                if (cursorUp && !justSeeded) {
                    engineMoved = (mc->cursorPosX != g_engineLastX ||
                                   mc->cursorPosY != g_engineLastY);
                }
                if (cursorUp) {
                    g_engineLastX = mc->cursorPosX;
                    g_engineLastY = mc->cursorPosY;
                }

                if (g_padCursorMode == PadCursorMode::kProbing && wanted) {
                    if (engineMoved) {
                        g_padCursorMode = PadCursorMode::kEngine;
                        SKSE::log::info("[PAD] the engine drives its own cursor — using it");
                    } else if (++g_engineStillFrames > 20) {
                        g_padCursorMode = PadCursorMode::kOwn;
                        SKSE::log::info("[PAD] engine cursor did not follow — drawing our own");
                    }
                }

                if (g_padCursorMode == PadCursorMode::kEngine) {
                    // ★★★THE ENGINE DRIVES THE POSITION, NOT THE DRAWING.
                    //
                    // This branch used to set MouseDrawCursor = false and let
                    // the game's arrow BE the pointer. When the engine moved
                    // that cursor but never drew it, the position was right and
                    // the screen was empty -- which is what pad players meant
                    // by "there is no cursor". Now every branch draws.
                    g_padCursor = ImVec2(mc->cursorPosX, mc->cursorPosY);
                } else {
                    const float dt = std::clamp(io.DeltaTime, 1.0f / 240.0f, 1.0f / 20.0f);
                    g_padCursor.x += mx * kPadCursorSpeed * dt;
                    g_padCursor.y += my * kPadCursorSpeed * dt;
                    g_padCursor.x = std::clamp(g_padCursor.x, 0.0f, io.DisplaySize.x - 1.0f);
                    g_padCursor.y = std::clamp(g_padCursor.y, 0.0f, io.DisplaySize.y - 1.0f);
                    // ★DrawPointer draws it; ImGui's own arrow stays off.
                    // Keep the game's frozen arrow parked on top of ours, so the
                    // sync it does perform (on a button press) lands HERE rather
                    // than leaving a second arrow stranded mid-screen. Not done
                    // while probing: it would forge the very signal we measure.
                    if (g_padCursorMode == PadCursorMode::kOwn && mc) {
                        mc->cursorPosX = g_padCursor.x;
                        mc->cursorPosY = g_padCursor.y;
                    }
                }
                // ★the pending d-pad step lands on whatever drove the
                // position this frame, and is pushed back into the engine's
                // cursor so its next read keeps the step instead of undoing
                // it. Our write must not read back as "the engine moved".
                if (g_padNudgeX != 0.0f || g_padNudgeY != 0.0f) {
                    g_padCursor.x = std::clamp(g_padCursor.x + g_padNudgeX,
                                               0.0f, io.DisplaySize.x - 1.0f);
                    g_padCursor.y = std::clamp(g_padCursor.y + g_padNudgeY,
                                               0.0f, io.DisplaySize.y - 1.0f);
                    g_padNudgeX = 0.0f;
                    g_padNudgeY = 0.0f;
                    if (mc) {
                        mc->cursorPosX = g_padCursor.x;
                        mc->cursorPosY = g_padCursor.y;
                        g_engineLastX = mc->cursorPosX;
                        g_engineLastY = mc->cursorPosY;
                    }
                }
                io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                io.AddMousePosEvent(g_padCursor.x, g_padCursor.y);
                TranslatePadButtons();
                if (const float sy = g_padScrollY.load(); sy != 0.0f) {
                    AddScrollEvent(0.0f, sy * kPadScrollRate);
                }
                return;
            }

            // ★NOT ImGui's own cursor -- DrawPointer draws ours at the end of
            // the frame. This flag would put ImGui's arrow underneath it.
            io.MouseDrawCursor = false;
            if (auto* ui = RE::UI::GetSingleton()) {
                if (ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
                    const auto* menuCursor = RE::MenuCursor::GetSingleton();
                    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                    io.AddMousePosEvent(menuCursor->cursorPosX, menuCursor->cursorPosY);
                } else if (POINT client{}; GetCursorPos(&client) != FALSE) {
                    // ★CLIENT coordinates. GetCursorPos is desktop-absolute, so
                    // windowed/offset setups fed a position shifted by the whole
                    // window origin — and since this branch alternates with the
                    // Cursor Menu one above whenever that menu is flickering,
                    // the carried item icon jumped between the two every frame.
                    if (g_gameWnd) ScreenToClient(g_gameWnd, &client);
                    io.AddMousePosEvent(static_cast<float>(client.x),
                                        static_cast<float>(client.y));
                }
            }
        }

        void ScrollHandler()
        {
            auto& io = ImGui::GetIO();
            ImVec2 now = ImVec2(0.0f, 0.0f);

            if (std::abs(g_scrollEnergy.x) > 0.01f) {
                now.x = g_scrollEnergy.x * io.DeltaTime * kScrollSmoothing;
                g_scrollEnergy.x -= now.x;
            } else {
                g_scrollEnergy.x = 0.0f;
            }
            if (std::abs(g_scrollEnergy.y) > 0.01f) {
                now.y = g_scrollEnergy.y * io.DeltaTime * kScrollSmoothing;
                g_scrollEnergy.y -= now.y;
            } else {
                g_scrollEnergy.y = 0.0f;
            }

            io.MouseWheel  = now.y;
            io.MouseWheelH = -now.x;
        }

        // ★The radial-falloff texture that used to live here is GONE (1.0.5).
        // It fed the rarity halo, and rarity has been a corner wedge for
        // several versions — so every start-up was building a 128x128 RGBA
        // gradient on the CPU, uploading it, and handing the view to nobody.
        // Its accessor (UIRoot::GlowTexture) went with it.

        // ---- SETTINGS window (⚙): scale + skin swatches + language ----
        // ---- Phase 3: settings rows as a table ----
        // One row = a function drawing "dim label at the shared column +
        // control"; DrawSettingsWindow just walks kSettingsRows with the
        // standard gap. Adding an option (F3/F4 merchant toggles etc.) =
        // one function + one table entry; F5's sectioning builds on this.
        struct SettingsCtx
        {
            float padLabelW;   // label column width (child-local since F5)
            float trackW;      // slider track width
            float S;           // UI scale
        };

        // ★ImGui cannot outline the text it draws, so anything that needs the
        // outline is drawn by US at the cursor and then given the same space
        // back with a Dummy. Layout is unchanged — SameLine and wrapping still
        // see an item of exactly the text's size.
        // ★Moved to Theme::TextOutlinedFlow so the EDIT panel can use the same
        // label. Kept as a name-local alias — this file calls it many times.
        void OutlinedText(ImU32 a_col, const char* a_txt,
                          float a_size = 0.0f, float a_spacing = 0.0f)
        {
            Theme::TextOutlinedFlow(a_col, a_txt, a_size, a_spacing);
        }

        // ★Values are RIGHT-aligned in the settings panel, the way the stats
        // panel already reads. Left-starting values left a 114px ragged edge
        // — and the ragged shape changed with every translation, because a
        // Korean button is wider than its English label.
        void RightAlign(float a_w)
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > a_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - a_w);
        }
        float BtnW(const char* a_label)
        {
            // ★Hide anything past "##" -- it is an id, not a word, and Sfx
            // measures its own label the same way.
            return ImGui::CalcTextSize(a_label, nullptr, true).x +
                   ImGui::GetStyle().FramePadding.x * 2.0f;
        }
        float BtnRowW(std::initializer_list<const char*> a_labels)
        {
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            float w = 0.0f; bool first = true;
            for (const char* l : a_labels) { if (!first) w += gap; w += BtnW(l); first = false; }
            return w;
        }

        // ★★ONE path for every settings slider, so the right-click gesture and
        // the line that explains it cannot drift apart — the failure mode of
        // "add the reset, forget the hint on one row" is silent, because a
        // gesture nobody is told about looks exactly like no gesture.
        // The EDIT panel answers this question differently, with a "(def 90°)"
        // column beside each field. These rows have no width to spare, so the
        // help goes to the bottom bar, which already carries hover help.
        bool SettingSlider(const char* a_id, float* a_v, float a_lo, float a_hi,
                           float a_w, float a_def, const char* a_fmt = "%.2f",
                           float a_snap = 0.0f)
        {
            const bool ch =
                Theme::ChromeSliderFloat(a_id, a_v, a_lo, a_hi, a_w, a_fmt, a_def, a_snap);
            if (ImGui::IsItemHovered()) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s  %.2f",
                    Lang::T(Lang::Str::HintSliderReset), a_def);
                NoteHoverHint(buf);
            }
            return ch;
        }

        void SettingLabel(const SettingsCtx& a_c, Lang::Str a_label)
        {
            // ★All-caps applied HERE, not in the strings: a built-in that
            // already reads "PRECACHE ALL" is replaced by en.ini's "Precache
            // All" (rule 91), which is why that one row came out sentence case
            // while its neighbours did not. See Lang::UpperCase.
            OutlinedText(Theme::Chrome(1.0f), Lang::UpperCase(Lang::T(a_label)).c_str());
            ImGui::SameLine(a_c.padLabelW);
        }

        // SCALE — mockup track: black .2 bg, acc .20 fill, centred value.
        // Pending-apply: while dragging only this local value moves — live
        // per-frame resizing of every managed window read as a ghosted /
        // doubled image (user-reported), and the font rebake was deferred to
        // release anyway. Scale, save and rebake all land on release.
        // SCALE — the board's scale, and the only one left. ★It used to be the
        // second of two rows: a SCALE that moved type/buttons/spacing and a
        // CELL that moved the board. The grid and the doll are 97% of the
        // window's width, so CELL was what "make it smaller" actually meant,
        // and the other one mostly set the chrome against itself. The UI-scale
        // row and its !uiscale key are gone; this took over the name.
        void RowCellScale(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::ScaleLabel);
            RightAlign(a_c.trackW);
            float cs = Theme::ScaleSetting();   // the shown value, 1.00 default
            if (SettingSlider("##cellscale", &cs,
                              Theme::kMinCellScale, Theme::kMaxCellScale, a_c.trackW,
                              Theme::kDefCellScale)) {
                Theme::SetScaleSetting(cs);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WinManager::GetSingleton()->Save();
                Grid::RequestRebuild();   // cell size changes what fits per row
            }
        }

        // TEXT SIZE — the automatic display scale is not a setting, and until
        // now nothing was: a 4K player who found 17px too small had nowhere to
        // go. This multiplies that automatic value, so 1.00 is exactly what
        // shipped and the panel keeps sizing itself by resolution underneath.
        //
        // ★★A CONTROL MUST NOT MOVE WHILE A HAND IS ON IT. That is the whole
        // reason this row defers, and it took three wrong theories to see it.
        //
        // This panel is made of the very text this row sizes. Applying the
        // value live re-laid the panel out underneath itself: the caption and
        // the SCALE row above grew taller, the window grew with them, and the
        // slider slid DOWN out from under the cursor -- which is still holding
        // the grab where the mouse is. Grab and track were then in two places,
        // moving against each other, every frame. That is what looked like a
        // doubled, ghosted image.
        //
        // ★The cell-size slider never did this and the difference is not the
        // slider, it is what each one resizes: cells live on the BOARD window,
        // so the panel holding that slider stays still. This one resizes the
        // panel it is in.
        // ★It also explains why bigger steps were WORSE rather than better:
        // fewer reflows, but each one moved the control further.
        //
        // So the number follows the hand and the size follows the release.
        // The arrows and the right-click default apply at once -- they are one
        // change with nothing held down, so nothing slides anywhere.
        void RowFontScale(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::FontScaleLabel);
            RightAlign(a_c.trackW);

            // ★s_held is set ONLY by this widget and cleared the moment the
            // value lands, so a preset load — which writes the same setting
            // from elsewhere — is never overwritten by a stale pending number.
            static float s_want = 1.0f;
            static bool  s_held = false;

            float fs = s_held ? s_want : Theme::FontScale();
            // ★NO "and the value moved" test here, and that is the fix for one:
            // SettingSlider is already true only when something moved it, and
            // adding `fs != s_want` compared the new value against a static
            // that starts at 1.0 rather than at the setting. So the FIRST move
            // to exactly 1.00 in a session was thrown away -- which is the
            // right-click default, the one gesture most likely to land there.
            if (SettingSlider("##fontscale", &fs,
                              Theme::kMinFontScale, Theme::kMaxFontScale, a_c.trackW,
                              1.0f, "%.2f", Theme::kFontScaleStep)) {
                s_want = fs;
                s_held = true;
            }
            // ★"Nothing is held" is asked of ImGui rather than of the slider:
            // IsItemDeactivatedAfterEdit only answers for the drag, while the
            // right-click default never activates the slider at all and the
            // step arrows are their own items. One condition, all four ways.
            if (s_held && !ImGui::IsAnyItemActive()) {
                s_held = false;
                // ★The write rides along: true means the value really moved,
                // so this cannot put the ini through a save per frame.
                if (Theme::SetFontScale(s_want)) {
                    WinManager::GetSingleton()->Save();
                }
            }
        }

        // SKIN — ★GI73: grouped by CHROME FAMILY, two colour variants each.
        //
        // Six flat colour chips could not say what any of them were: 3/4 are
        // 1/2 with a torn frame and 5/6 differ ONLY in panel transparency, so
        // a 1px accent ring was carrying the entire distinction and losing.
        // Now the family caption carries the frame, a base+wedge split carries
        // the accent, and the two Glass chips are drawn at their REAL alpha
        // over a hatch — transparency is not a colour and cannot be shown as
        // one, so it is shown as itself.
        void RowSkin(const SettingsCtx& a_c)
        {
            const auto& sk = Theme::S();
            auto* dl = ImGui::GetWindowDrawList();

            // ★Desaturated against the raw accent values. A 24px chip of solid
            // #D8B878 is a far brighter object than the same hue spread thin
            // over a window border, and six of them in a row glared. Pulled
            // ~40% toward their own luminance and darkened a touch; the hue is
            // unchanged, so they still read as gold / crimson / bone.
            constexpr ImU32 kGold  = IM_COL32(0xC0, 0xAE, 0x89, 255);
            constexpr ImU32 kBrown = IM_COL32(0x6B, 0x4A, 0x2A, 255);
            constexpr ImU32 kBlack = IM_COL32(0x14, 0x14, 0x14, 255);
            constexpr ImU32 kRed   = IM_COL32(0x99, 0x46, 0x38, 255);
            constexpr ImU32 kWhite = IM_COL32(0xCE, 0xCA, 0xC2, 255);

            struct Sw
            {
                ImU32 base;    // 0 = translucent: draw the hatch and use `alpha`
                ImU32 wedge;
                int   alpha;   // panel alpha, 0-255, only when base == 0
            };
            // ★Only these need listing. Their chip is deliberately NOT their
            // panel — the parchment ones invert — so no rule derives it.
            // ★★Keyed by NAME. This table was positional and held the two Glass
            // entries at 4 and 5; when the ink pair moved to the front, every
            // row pointed at the wrong skin and Fable Crimson would have been
            // painted with the parchment gold. A chip is the only place a
            // player sees a skin before clicking it, so a wrong one here is a
            // wrong choice made on its behalf.
            struct NamedSw { const char* skin; Sw sw; };
            static const NamedSw kSw[] = {
                { "Fable Crimson",     { kBlack, kRed,   0 } },
                { "Parchment Amber",   { kGold,  kBrown, 0 } },
                { "Parchment Crimson", { kBlack, kWhite, 0 } },
            };
            // ★★From SIMPLE on, the chip IS the panel, so it is READ from the
            // skin rather than copied here. The old table proved the point by
            // holding #848B91 / #43474F for Silver — winBg and acc, transcribed
            // by hand. That is survivable for five entries and not for twenty:
            // a table nobody remembers to extend paints a new skin with the
            // previous one's colours, and the chip is the only place a player
            // sees the difference before clicking.
            const auto swatchOf = [](int a_idx) -> Sw {
                const Theme::Skin& sk2 = Theme::SkinAt(a_idx);
                for (const auto& n : kSw) {
                    if (sk2.name && std::strcmp(n.skin, sk2.name) == 0) return n.sw;
                }
                // ★The ink skins land here and that is CORRECT, not a gap:
                // winBg is literally their paper now (Theme::PaperTint), so a
                // derived chip and the window it opens are the same value.
                return { Theme::Col(sk2.winBg, 1.0f),
                         Theme::Col(sk2.acc, 1.0f), 0 };
            };
            // ★★A family is now a RANGE, not a pair. It was {a, b} with 0
            // meaning "no second member", which capped every family at two and
            // needed a skip in both the draw loop and the hit test — the kind
            // of duplication that put the two out of step once already (see the
            // hit-test note below). SIMPLE has six members now; first/count
            // says so once and both loops read it.
            struct Fam { const char* name; int first, count; };
            // ★Fable lost its amber half (it was Parchment Amber's twin in
            // everything but the frame), so that family is down to one chip;
            // GLASS lost both of its.
            // ★★Each family ASKS WHERE ITS FIRST MEMBER IS rather than stating
            // a number. The literals here were 1 / 2 / 4 / 6 and every one of
            // them was wrong the moment the ink pair moved to the front — a
            // caption over somebody else's chips, and no way for the build to
            // notice. The families are still contiguous ranges, which is a
            // property of the kSkins order, so only the START has to be found.
            const auto at = [](const char* n) { return Theme::SkinIndexByName(n); };
            const int  simpleFirst = at("Simple Charcoal");
            const Fam  kFam[4] = {
                // ★"INK WASH" — 수묵화, the English name of the technique, not
                // the Japanese word the skins are named for. The caption says
                // what the family IS to someone who has never heard "sumi";
                // the skins keep their own names.
                { "INK WASH",  at("Sumi Parchment"),   2 },
                { "FABLE",     at("Fable Crimson"),    1 },
                { "PARCHMENT", at("Parchment Amber"),  2 },
                // ★The count comes from the TABLE. SIMPLE absorbs every skin
                // added after it — a literal here is one more place to forget,
                // with the same failure as the swatch table above: chips that
                // stop at the old count simply never show the new skins.
                { "SIMPLE",    simpleFirst, Theme::SkinCount() - simpleFirst + 1 }
            };
            // ★Families wrap: a row of every chip would push the settings
            // window far wider than any other control needs.
            const float kRowMax = 232.0f * a_c.S;

            const float side  = 24.0f * a_c.S;
            const float capH  = 15.0f * a_c.S;
            const float capPx = Theme::FontCaption();
            // ★The family gap is set by the CAPTION, not by the chips: at 12px
            // "PARCHMENT" is wider than the two 24px chips it labels, so the
            // gap has to absorb the overhang or it runs into GLASS. Keep this
            // in step with kFamGap in the swatchW budget above.
            const float famGap = 16.0f * a_c.S;
            ImFont*     font   = ImGui::GetFont();

            SettingLabel(a_c, Lang::Str::SkinLabel);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            // ★The gap BETWEEN chips, side to side. Hoisted because the line
            // spacing below is the same number — see wrapH.
            const float chipGap = 8.0f * a_c.S;
            // A line that carries a family CAPTION: caption, chips, breathing room.
            const float rowH = capH + side + 9.0f * a_c.S;
            // ★★A line that does NOT. A family wrapping inside itself puts its
            // caption on the first line only, so the lines after it were being
            // spaced as though they had one — chips 24px apart vertically and
            // 8px apart horizontally, which read as two loose rows rather than
            // one block. `y` is the caption baseline and chips hang at y+capH,
            // so leaving capH out of the step is exactly what closes it.
            const float wrapH = side + chipGap;
            float        x = origin.x;
            float        y = origin.y;
            // ...and if a family ever wrapped and another followed it, the next
            // caption would land on top of those chips. SIMPLE absorbs every
            // skin so it is structurally last and this cannot happen today;
            // it is one line to make that not matter.
            bool innerWrapped = false;
            float        rowW = 0.0f;   // widest line, for the hit area
            // ★★The hit test reads the rects the DRAW produced instead of
            // recomputing the layout. The two used to be separate walks with a
            // comment ordering them to stay in step, and they did not: when
            // Fable became a one-member family every chip after it was
            // hit-tested a slot to the left of where it was painted. Wrapping
            // would have been a second chance to make the same mistake.
            struct Chip { ImVec2 p0; int idx; };
            // ★Sized well past the table rather than at it: this array is
            // written to inside the draw loop, and the one thing it must never
            // do is run short of the skins the loop is walking.
            Chip chips[64] = {};
            int  nChips = 0;
            for (const auto& f : kFam) {
                if (innerWrapped) {   // give this family's caption its line back
                    y += capH;
                    innerWrapped = false;
                }
                const float famW = f.count * (side + chipGap) + famGap;
                if (x > origin.x && (x - origin.x) + famW > kRowMax) {
                    rowW = (std::max)(rowW, x - origin.x);
                    x = origin.x;
                    y += rowH;
                }
                dl->AddText(font, capPx, ImVec2(x, y),
                    Theme::Col(sk.inkDim, 0.85f), f.name);
                for (int i = 0; i < f.count; ++i) {
                    // ★★A family wraps INTERNALLY now. The old rule — never
                    // split a family across lines, because the caption belongs
                    // to its chips — quietly assumed every family fits on one
                    // line. SIMPLE at twenty members wants ~640px against a
                    // 232px limit, so holding the rule would have drawn most of
                    // this family off the edge of the window. The caption stays
                    // on the family's FIRST line: it names the whole run, and a
                    // run that wraps is still one run.
                    if (x > origin.x && (x - origin.x) + side > kRowMax) {
                        rowW = (std::max)(rowW, x - origin.x);
                        x = origin.x;
                        y += wrapH;          // no caption on this line
                        innerWrapped = true;
                    }
                    const int idx = f.first + i;
                    const ImVec2 p0(x, y + capH);
                    const ImVec2 p1(p0.x + side, p0.y + side);
                    const Sw     s = swatchOf(idx);
                    if (s.base) {
                        dl->AddRectFilled(p0, p1, s.base, 4.0f);
                    } else {
                        // Hatch, then the panel at its true alpha on top: the
                        // stripes that survive ARE the transparency. Neutral
                        // greys on purpose — a coloured hatch (the first cut
                        // used a grass green) stops reading as "see-through"
                        // and starts reading as "the green skin".
                        dl->AddRectFilled(p0, p1, IM_COL32(0x6E, 0x6E, 0x74, 255), 4.0f);
                        // ★PushClipRect is a RECTANGLE — it ignores the corner
                        // rounding, so stripes clipped to p0..p1 filled the
                        // rounded corners and these two chips came out square
                        // next to four round ones. Inset the stripe band
                        // instead: the rounded base itself covers the edge.
                        const float in = 3.0f * a_c.S;
                        dl->PushClipRect(ImVec2(p0.x + in, p0.y + in),
                                         ImVec2(p1.x - in, p1.y - in), true);
                        for (float o = -side; o < side * 2.0f; o += 6.0f * a_c.S) {
                            dl->AddLine(ImVec2(p0.x + o, p1.y), ImVec2(p0.x + o + side, p0.y),
                                IM_COL32(0x45, 0x45, 0x4B, 255), 3.0f * a_c.S);
                        }
                        dl->PopClipRect();
                        dl->AddRectFilled(p0, p1, IM_COL32(0x0C, 0x0C, 0x0E, s.alpha), 4.0f);
                    }
                    // ★The accent wedge has to round its OUTER corner the same
                    // 4px the base does. A square corner at p1 pokes out past
                    // the rounded base on every chip — a hard little point at
                    // the bottom right that reads as a rendering fault, and no
                    // clip can fix it (PushClipRect is a rectangle and ignores
                    // corner rounding). Draw the corner as an arc instead.
                    constexpr float kChipR = 4.0f;   // == the base's rounding
                    const float     wg     = side * 0.57f;
                    dl->PathLineTo(ImVec2(p1.x, p1.y - wg));
                    // 0 -> PI/2 runs (p1.x, p1.y-r) round to (p1.x-r, p1.y):
                    // down the right edge, round the corner, onto the bottom
                    dl->PathArcTo(ImVec2(p1.x - kChipR, p1.y - kChipR), kChipR,
                                  0.0f, IM_PI * 0.5f);
                    dl->PathLineTo(ImVec2(p1.x - wg, p1.y));
                    dl->PathFillConvex(s.wedge);
                    dl->AddRect(p0, p1, Theme::Acc(0.4f), 4.0f);
                    if (Theme::SkinIndex() == idx) {
                        dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2),
                            Theme::Val(), 5.0f, 0, 2.0f);
                    }
                    if (nChips < static_cast<int>(std::size(chips))) {
                        chips[nChips++] = { p0, idx };
                    }
                    x += side + chipGap;
                }
                x += famGap;
            }
            rowW = (std::max)(rowW, x - origin.x);
            // ONE hit area for the whole block, then hit-test the recorded
            // chips: captions and gaps make per-chip InvisibleButtons fight the
            // cursor layout, and the block is fixed-size anyway.
            const float blockH = (y - origin.y) + capH + side;
            ImGui::SetCursorScreenPos(origin);
            ImGui::InvisibleButton("##skinrow", ImVec2(rowW, blockH));
            if (ImGui::IsItemClicked()) {
                const ImVec2 m = ImGui::GetIO().MousePos;
                for (int i = 0; i < nChips; ++i) {
                    const ImVec2& q = chips[i].p0;
                    if (m.x >= q.x && m.x < q.x + side &&
                        m.y >= q.y && m.y < q.y + side) {
                        Theme::SetSkin(chips[i].idx);
                        WinManager::GetSingleton()->Save();
                        break;
                    }
                }
            }
        }

        // language chip labels — shared by the width budget and the row
        // LANGUAGE — padded chips (image-3 spacing). ★GI71: the list is however
        // many languages exist, not four: user packs append to it. Chips wrap
        // instead of running off the panel once packs are installed.
        void RowLanguage(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::LanguageLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            {   // right-align the whole strip when it fits on one line
                float total = 0.0f;
                for (int i = 0; i < Lang::Count(); ++i) {
                    if (i) total += 6.0f * a_c.S;
                    total += ImGui::CalcTextSize(Lang::DisplayName(i)).x + 16.0f * a_c.S;
                }
                if (total <= ImGui::GetContentRegionAvail().x) RightAlign(total);
            }
            const float avail = ImGui::GetContentRegionAvail().x;
            const float gap   = 6.0f * a_c.S;
            float       used  = 0.0f;
            const int   n     = Lang::Count();
            for (int i = 0; i < n; ++i) {
                const char* label = Lang::DisplayName(i);
                const float w = ImGui::CalcTextSize(label).x + 16.0f * a_c.S;
                if (i > 0) {
                    if (used + gap + w <= avail) {
                        ImGui::SameLine(0.0f, gap);
                        used += gap;
                    } else {
                        // Wrapped. Without this the chip falls back to the
                        // window margin and sits under the LANGUAGE label
                        // instead of under the row above it -- SettingLabel
                        // parks every control at padLabelW, so a second line
                        // has to be put there by hand.
                        ImGui::SetCursorPosX(a_c.padLabelW);
                        used = 0.0f;
                    }
                }
                used += w;
                const bool on = Lang::Get() == i;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
                }
                ImGui::PushID(i);
                if (Sfx::Button(label)) {
                    Lang::SetLang(i);
                    // a pack can ask for glyphs the current atlas never baked
                    g_fontsDirty.store(true);
                    WinManager::GetSingleton()->Save();
                }
                ImGui::PopID();
                if (on) ImGui::PopStyleColor(2);
            }
            ImGui::PopStyleVar();
        }

        // PRESET (GI46-48): the dropdown lists every GridInventory_<name>.ini
        // beside the plugin (Default, P1, P2, ...); Import applies the picked
        // one on the spot. The next row exports under a chosen name.
        void RowPreset(const SettingsCtx& a_c)
        {
            static std::vector<std::string> s_list;
            static int  s_sel = -1;
            static bool s_wasOpen = false;

            SettingLabel(a_c, Lang::Str::PresetLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            ImGui::SetNextItemWidth(140.0f * a_c.S);
            const bool picked = s_sel >= 0 && s_sel < static_cast<int>(s_list.size());
            const bool open = ImGui::BeginCombo("##presetpick",
                picked ? s_list[s_sel].c_str() : "...");
            if (open) {
                if (!s_wasOpen) {   // rescan the folder as the combo drops down
                    const std::string keep = picked ? s_list[s_sel] : "";
                    s_list = WinManager::GetSingleton()->ListPresets();
                    s_sel = -1;
                    for (int i = 0; i < static_cast<int>(s_list.size()); ++i) {
                        if (s_list[i] == keep) { s_sel = i; break; }
                    }
                }
                for (int i = 0; i < static_cast<int>(s_list.size()); ++i) {
                    if (ImGui::Selectable(s_list[i].c_str(), s_sel == i)) s_sel = i;
                }
                ImGui::EndCombo();
            }
            s_wasOpen = open;
            ImGui::SameLine(0.0f, 6.0f * a_c.S);
            if (Sfx::Button(Lang::T(Lang::Str::PresetImport))) {
                if (s_sel >= 0 && s_sel < static_cast<int>(s_list.size()) &&
                    WinManager::GetSingleton()->ImportPreset(s_list[s_sel])) {
                    WinManager::GetSingleton()->Save();   // persist into the ui ini
                    g_fontsDirty.store(true);             // scale may have changed
                    g_presetMergePak = WinManager::PresetPakPath(s_list[s_sel]);
                    g_iconsMergePreset.store(true);       // GI47: icons on the Tick
                } else {
                    Sfx::FailNote(Lang::T(Lang::Str::PresetMissing));
                }
            }
            ImGui::PopStyleVar();
        }

        // EXPORT row: preset name ("Default" when left blank) + save button
        void RowPresetExport(const SettingsCtx& a_c)
        {
            static char s_name[64] = {};
            SettingLabel(a_c, Lang::Str::PresetExport);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            ImGui::SetNextItemWidth(140.0f * a_c.S);
            ImGui::InputTextWithHint("##presetname", "Default", s_name, sizeof(s_name));
            ImGui::SameLine(0.0f, 6.0f * a_c.S);
            if (Sfx::Button(Lang::T(Lang::Str::Save))) {
                WinManager::GetSingleton()->ExportPreset(s_name[0] ? s_name : "Default");
            }
            ImGui::PopStyleVar();
        }

        // GLOW — rarity glow style chips (silhouette=1 / radial=0)
        // ICON STYLE — realistic auto-captures vs the GI59 stylized filter
        // (derived from the captures on demand; covers every item)
        void RowIconStyle(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::IconStyleLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            RightAlign(BtnRowW({ Lang::T(Lang::Str::StyleRealistic),
                                 Lang::T(Lang::Str::StyleFlat),
                                 Lang::T(Lang::Str::StylePixel) }));
            auto* icons = IconCache::GetSingleton();
            // Realistic captures the game's models; Drawn (GI52) uses the
            // authored category set and captures nothing. The derived
            // "stylized" filter that used to sit between them was retired in
            // GI60 — the drawn set answers the same want without a scan.
            static constexpr struct { IconCache::Style s; Lang::Str label; } kStyles[] = {
                { IconCache::Style::kRealistic, Lang::Str::StyleRealistic },
                { IconCache::Style::kFlat,      Lang::Str::StyleFlat      },
                // Pixel is DERIVED from the realistic capture, so it needs no
                // art of its own and covers mod items exactly as well.
                { IconCache::Style::kPixel,     Lang::Str::StylePixel     },
            };
            for (int i = 0; i < static_cast<int>(std::size(kStyles)); ++i) {
                const bool on = icons->GetStyle() == kStyles[i].s;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
                }
                ImGui::PushID(i);
                if (Sfx::Button(Lang::T(kStyles[i].label))) {
                    // ★Through Theme, not IconCache: the style belongs to the
                    // SKIN now, and Theme is what hands it to the cache and
                    // what Save writes out. Poking the cache directly would
                    // change the sprites and lose the choice on the next open.
                    Theme::SetIconStyle(static_cast<int>(kStyles[i].s));
                    WinManager::GetSingleton()->Save();
                    Grid::Rebuild();   // tiles re-resolve which sprite they draw
                }
                ImGui::PopID();
                if (on) ImGui::PopStyleColor(2);
                if (i + 1 < static_cast<int>(std::size(kStyles))) {
                    ImGui::SameLine(0.0f, 6.0f * a_c.S);
                }
            }
            ImGui::PopStyleVar();
        }

        // ★★★1.0.5 ITEM SHADOW — three rows, and they are the three controls a
        // drop shadow has in every tool that has ever had one: DISTANCE, BLUR,
        // OPACITY. What stood here before was a strength slider plus a
        // Soft/Sharp pair, and that pair was an implementation detail dressed
        // as a choice — "Soft" sampled a silhouette baked onto a 96px canvas,
        // "Sharp" stamped the sprite, and the user was being asked to pick
        // between two code paths because neither could be tuned into the other.
        // The draw does its own blur at sprite resolution now, so the shape
        // follows the numbers and the numbers are the ones people expect.
        //
        // ★One helper for all three: they differ only in axis, range and
        // format, and three near-identical copies is exactly how a right-click
        // reset ends up on two rows out of three.
        // a_mul lets a row SHOW different units than it stores: opacity lives as
        // 0..1 and reads as 0..100%, and doing that here keeps the conversion
        // off the draw path, where a stray /100 would be a silent bug.
        void ShadowRow(const SettingsCtx& a_c, Lang::Str a_label, const char* a_id,
                       int a_axis, float a_hi, const char* a_fmt, float a_mul = 1.0f)
        {
            SettingLabel(a_c, a_label);
            RightAlign(a_c.trackW);
            float v = Theme::ShadowAxis(a_axis) * a_mul;
            if (SettingSlider(a_id, &v, 0.0f, a_hi * a_mul, a_c.trackW,
                              Theme::DefShadow(a_axis) * a_mul, a_fmt)) {
                Theme::SetShadowAxis(a_axis, v / a_mul);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WinManager::GetSingleton()->Save();
            }
        }

        // px the shadow falls toward the lower right. 0 = ambient, spread even
        // on every side — which is also the only setting that stays put under
        // the 90-degree tile rotations.
        void RowShadowDist(const SettingsCtx& a_c)
        {
            ShadowRow(a_c, Lang::Str::ShadowDistLabel, "##shaddist", 0, 8.0f, "%.1f");
        }

        // px of spread. 0 is the sprite's exact outline in black — no longer a
        // separate "Sharp" mode, just the bottom of this slider.
        void RowShadowBlur(const SettingsCtx& a_c)
        {
            ShadowRow(a_c, Lang::Str::ShadowBlurLabel, "##shadblur", 1, 8.0f, "%.1f");
        }

        // how dark, as a fraction. Shown as a percentage because that is how
        // the mockups that settled it were labelled.
        void RowShadowOpac(const SettingsCtx& a_c)
        {
            ShadowRow(a_c, Lang::Str::ShadowOpacLabel, "##shadopac", 2, 1.0f, "%.0f%%", 100.0f);
        }

        // ICON LIGHT — item icon brightness, LIVE: <=1 darkens via tint,
        // >1 brightens via the fill-light pass (see DrawItemIcon)
        void RowIconGain(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::IconBrightLabel);
            RightAlign(a_c.trackW);
            float ig = Theme::IconGain();
            if (SettingSlider("##icongain", &ig, 0.4f, 1.6f, a_c.trackW,
                              Theme::DefaultIconGain())) {
                Theme::SetIconGain(ig);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WinManager::GetSingleton()->Save();
            }
        }

        // ★★CAPTURE LIGHT — where the menu scene's single lamp stands while an
        // icon is photographed, for EVERY item at once. Two reasons it lives
        // here and not in DISPLAY: DISPLAY is stored per skin, and a per-skin
        // lamp would throw away the whole icon cache every time the player
        // tried another skin; and this is a property of the photograph, which
        // the ICONS section already owns (cache, precache, reload).
        //
        // ★★LIVE, exactly like the EDIT panel's own light rows — a lamp you
        // cannot see move is a lamp you cannot aim. Two things have to be true
        // for that to be affordable, and both are provided rather than assumed:
        //   1. PurgeQueue() — every frame of the drag is a new key for EVERY
        //      item, and the queue dedupes by key, so without it a three-second
        //      drag leaves thousands of captures queued at angles already
        //      scrolled past. Purging keeps the backlog at "what is on screen".
        //   2. IconCache's per-item last-good fallback — a missing key draws
        //      the previous angle instead of nothing, so the board never blanks
        //      mid-drag. (Get(): m_lastGood.)
        // Only the ini WRITE waits for release; the pixels do not.
        void RowCaptureLight(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::CaptureLightLabel);
            // ★The warning goes on the LABEL: both sliders already spend their
            // own hover on the right-click hint, and this row is the one whose
            // cost ("every icon re-photographs") is not obvious from its name.
            if (ImGui::IsItemHovered()) NoteHoverHint(Lang::T(Lang::Str::CaptureLightHint));
            RightAlign(a_c.trackW);

            float az = Theme::CaptureLightAz();
            float el = Theme::CaptureLightEl();

            bool changed = false, released = false;
            const float half = (a_c.trackW - 6.0f * a_c.S) * 0.5f;
            // ★Both axes report into the SAME pair of flags. Testing
            // IsItemDeactivatedAfterEdit after the row instead would only ever
            // see the LAST slider, so releasing X would never save.
            const auto axis = [&](const char* a_id, float* a_v, float a_lo, float a_hi,
                                  float a_def, const char* a_fmt) {
                if (SettingSlider(a_id, a_v, a_lo, a_hi, half, a_def, a_fmt)) changed = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) released = true;
            };
            // ★Axis named INSIDE the value ("X 12°"), not in a second label
            // column: two rows of one slider would push ICONS past the panel
            // height for a pair of numbers that are always read together.
            axis("##caplightx", &az, -180.0f, 180.0f,
                 Theme::kDefCapLightAz, "X %.0f\xC2\xB0");
            ImGui::SameLine(0.0f, 6.0f * a_c.S);
            axis("##caplighty", &el, -80.0f, 80.0f,
                 Theme::kDefCapLightEl, "Y %.0f\xC2\xB0");

            if (changed) {
                Theme::SetCaptureLight(az, el);
                IconCache::GetSingleton()->PurgeQueue();   // old angle's backlog is worthless
                Grid::RefreshDefs();                       // re-queue the board at the new one
            }
            // ★The ini write is the ONE thing that still waits: it is disk IO
            // and the intermediate angles are not worth persisting. The
            // right-click reset never goes through an active state, so it is
            // caught by the second clause instead of the release.
            if (released || (changed && !ImGui::IsAnyItemActive())) {
                WinManager::GetSingleton()->Save();
            }
        }

        // ICON CACHE — manual reset for retexture installs: every icon
        // re-renders from the CURRENT meshes/textures. Two-click armed
        // (3s window) so a stray click can't wipe the cache.
        // ★It re-reads the DRAWN icons too. Those had their own button, and it
        // was one button too many: both answer the same question — "I changed
        // what an icon is made of, show me" — and which one to press depended
        // on knowing whether that icon happens to be a 3D capture or a PNG,
        // which is exactly the thing the player should not have to know.
        void RowCacheReset(const SettingsCtx& a_c)
        {
            const auto& sk = Theme::S();
            SettingLabel(a_c, Lang::Str::CacheLabel);
            static double s_armedUntil = 0.0;
            const bool armed = ImGui::GetTime() < s_armedUntil;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            if (armed) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col(sk.sel, 0.55f));
            RightAlign(BtnW(Lang::T(armed ? Lang::Str::Confirm : Lang::Str::CacheReset)));
            if (Sfx::Button(Lang::T(armed ? Lang::Str::Confirm : Lang::Str::CacheReset))) {
                if (armed) {
                    g_iconsReset.store(true);
                    g_flatReload.store(true);   // Tick: SRVs must not drop mid-frame
                    // ★The captures visibly re-render, so this note is not
                    // there to prove the button worked -- it is there for the
                    // one thing a reload cannot do. A PNG added to the mod
                    // folder after launch is not in the virtual Data folder
                    // the plugin sees, and no amount of re-reading will find
                    // it.
                    g_flatReloadNote = ImGui::GetTime() + 3.5;
                    s_armedUntil = 0.0;
                } else {
                    s_armedUntil = ImGui::GetTime() + 3.0;
                }
            }
            if (armed) ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // PRECACHE (C): one-shot batch capture of every inventory form in
        // the load order. Runs through the normal queue while the menu stays
        // open; already-on-disk items are skipped for free, captures land in
        // the pak only (VRAM stays flat). Click again to cancel; visible
        // items re-queue themselves as usual.
        // GI53 — ICON MAP: write every item's drawn-icon assignment to json so
        // IconStudio can show what the rules actually produce on THIS load
        // order. Classification only, no captures, so it returns at once.
        // The tool deliberately does not re-implement the rules: a second copy
        // in another language would answer confidently and wrongly the moment
        // Fallback.cpp changes.
        void RowPrecache(const SettingsCtx& a_c)
        {
            const auto& sk = Theme::S();
            SettingLabel(a_c, Lang::Str::PrecacheLabel);
            static bool s_precacheOn = false;
            static size_t s_precacheMax = 0;   // queue length at the start
            auto* cache = IconCache::GetSingleton();
            const size_t q = cache->QueuedCount();
            if (s_precacheOn && q == 0) s_precacheOn = false;   // drained

            // ★Right-align BOTH states. The alignment used to sit inside the
            // idle branch only, so the moment the run started the button
            // jumped to the left margin — every other row in the panel keeps
            // its control on the right edge.
            // ★And size it to the WIDEST label it will ever show: the count
            // shrinks as the queue drains (1400 -> 999 -> 99), and a button
            // that narrows under right alignment crawls rightwards for the
            // whole run.
            char lbl[64], wide[64];
            if (s_precacheOn) {
                // ★The count is DISPLAY; the id after "##" is what the click
                // needs to stay still (Sfx::Button).
                std::snprintf(lbl, sizeof(lbl), "%s (%zu)##precachecancel",
                    Lang::T(Lang::Str::Cancel), q);
                std::snprintf(wide, sizeof(wide), "%s (%zu)",
                    Lang::T(Lang::Str::Cancel), s_precacheMax);
            } else {
                std::snprintf(lbl, sizeof(lbl), "%s", Lang::T(Lang::Str::PrecacheStart));
                std::snprintf(wide, sizeof(wide), "%s", lbl);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            const float bw = BtnW(wide);
            RightAlign(bw);
            if (s_precacheOn) {
                ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col(sk.sel, 0.55f));
                if (Sfx::Button(lbl, ImVec2(bw, 0.0f), true)) {   // cancel
                    cache->CancelPrecache();
                    s_precacheOn = false;
                }
                ImGui::PopStyleColor();
            } else {
                if (Sfx::Button(lbl, ImVec2(bw, 0.0f))) {
                    const size_t n = cache->PrecacheAll();
                    s_precacheOn = n > 0;
                    s_precacheMax = n;
                }
            }
            ImGui::PopStyleVar();
        }

        // GI68 — DEFERRED ICONS: only appears when something is actually owed a
        // retry. A row that is always there would need explaining; a row that
        // shows up with a number on it explains itself.
        void RowDeferred(const SettingsCtx& a_c)
        {
            auto* cache = IconCache::GetSingleton();
            const size_t n = cache->DeferredCount();
            if (n == 0) return;
            SettingLabel(a_c, Lang::Str::DeferredLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            char lbl[64];
            std::snprintf(lbl, sizeof(lbl), "%s (%zu)##deferredretry",
                Lang::T(Lang::Str::DeferredRetry), n);
            RightAlign(BtnW(lbl) + 6.0f * a_c.S +
                       BtnW(Lang::T(Lang::Str::DeferredForget)));
            if (Sfx::Button(lbl)) cache->RetryDeferred();
            ImGui::SameLine(0.0f, 6.0f * a_c.S);
            if (Sfx::Button(Lang::T(Lang::Str::DeferredForget), ImVec2(0, 0), true)) {
                cache->ClearDeferred();
            }
            ImGui::PopStyleVar();
        }

        // F3 — MERCHANT GOLD: Default / Unlimited chips (GlowStyle grammar).
        // PushID: both trade rows share the "Default" chip label.
        // ★★QUICK WHEEL — on, or the game's own favourites menu. The wheel
        // REPLACES that menu rather than sitting beside it: it answers the same
        // key and suppresses the screen that key used to open. So the switch is
        // not "show a feature", it is "which of these two exists" -- and off
        // gives the vanilla screen back whole, hotkey binding included.
        void RowWheelEnable(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::WheelEnableLabel);
            ImGui::PushID("wheelon");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            RightAlign(BtnRowW({ Lang::T(Lang::Str::ToggleOn),
                                 Lang::T(Lang::Str::ToggleOff) }));
            for (int want : { 1, 0 }) {
                const bool on = Wheeler::Enabled() == (want == 1);
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
                }
                if (Sfx::Button(Lang::T(want ? Lang::Str::ToggleOn : Lang::Str::ToggleOff))) {
                    Wheeler::SetEnabled(want == 1);
                    WinManager::GetSingleton()->Save();
                }
                if (on) ImGui::PopStyleColor(2);
                if (want == 1) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
            ImGui::PopID();
        }

        void RowMerchantGold(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::MerchGoldSetLabel);
            ImGui::PushID("merchgold");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            RightAlign(BtnRowW({ Lang::T(Lang::Str::ToggleDefault),
                                 Lang::T(Lang::Str::ToggleUnlimited) }));
            for (int inf : { 0, 1 }) {
                const bool on = LootBarter::MerchantGoldInfinite() == (inf == 1);
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
                }
                if (Sfx::Button(Lang::T(inf ? Lang::Str::ToggleUnlimited
                                            : Lang::Str::ToggleDefault))) {
                    LootBarter::SetMerchantGoldInfinite(inf == 1);
                    WinManager::GetSingleton()->Save();
                }
                if (on) ImGui::PopStyleColor(2);
                if (inf == 0) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
            ImGui::PopID();
        }

        // F4 — MERCHANT BUYS: Default / Anything chips. The stolen-goods rule
        // stays either way (fence-only), only the category list is lifted.
        void RowMerchantStock(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::MerchStockSetLabel);
            ImGui::PushID("merchstock");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            RightAlign(BtnRowW({ Lang::T(Lang::Str::ToggleDefault),
                                 Lang::T(Lang::Str::ToggleAnything) }));
            for (int all : { 0, 1 }) {
                const bool on = LootBarter::MerchantBuysAll() == (all == 1);
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
                }
                if (Sfx::Button(Lang::T(all ? Lang::Str::ToggleAnything
                                            : Lang::Str::ToggleDefault))) {
                    LootBarter::SetMerchantBuysAll(all == 1);
                    WinManager::GetSingleton()->Save();
                }
                if (on) ImGui::PopStyleColor(2);
                if (all == 0) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
            ImGui::PopID();
        }

        using SettingsRowFn = void (*)(const SettingsCtx&);

        // F5: rows grouped into titled sections (GENERAL / DISPLAY / TRADE /
        // ICONS). Adding an option = one row function + one entry here.
        struct SettingsSection
        {
            Lang::Str            title;
            const SettingsRowFn* rows;
            size_t               count;
        };
        // ★No WHEEL KEY row. The quick menu answers to the game's own
        // Favorites binding now, so the place to change it is the game's
        // controls -- a second, private binding for the same key would be a
        // setting that can disagree with the one the player already trusts.
        // ★TEXT SIZE sits right under SCALE: both answer "this is too small",
        // and putting them together is what lets a player try one and then the
        // other without hunting.
        constexpr SettingsRowFn kRowsGeneral[] = { RowCellScale, RowFontScale,
                                                   RowLanguage,
                                                   RowWheelEnable,
                                                   RowPreset, RowPresetExport };
        // ★SKIN leads DISPLAY rather than sitting in GENERAL: every row under
        // it is now stored PER SKIN, so the skin chip is the context those
        // rows are read in. Choosing a skin first and tuning below it is the
        // order the settings are actually used in.
        // ★1.0.5: the GLOW STYLE row (silhouette / radial) is gone with the
        // halo it selected between. What is left is the drop shadow, which
        // has one shape and only a strength.
        constexpr SettingsRowFn kRowsDisplay[] = { RowSkin, RowIconStyle, RowShadowDist,
                                                   RowShadowBlur, RowShadowOpac, RowIconGain };
        constexpr SettingsRowFn kRowsTrade[]   = { RowMerchantGold, RowMerchantStock };
        // ★CAPTURE LIGHT leads the section: it decides what the captures look
        // like, and the rows under it (reset, precache) are how you re-take
        // them. Reading top to bottom is the order the work is actually done.
        constexpr SettingsRowFn kRowsIcons[]   = { RowCaptureLight, RowCacheReset,
                                                   RowPrecache, RowDeferred };
        constexpr SettingsSection kSettingsSections[] = {
            { Lang::Str::SectionGeneral, kRowsGeneral, std::size(kRowsGeneral) },
            { Lang::Str::SectionDisplay, kRowsDisplay, std::size(kRowsDisplay) },
            { Lang::Str::SectionTrade,   kRowsTrade,   std::size(kRowsTrade) },
            { Lang::Str::SectionIcons,   kRowsIcons,   std::size(kRowsIcons) },
        };

        // section title + thin rule running to the right edge
        void SettingsSectionHeader(const SettingsCtx& a_c, Lang::Str a_title)
        {
            const char* txt = Lang::T(a_title);
            const float availW = ImGui::GetContentRegionAvail().x;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 ts = Theme::TrackedSize(txt, Theme::FontBody(), 2.6f * a_c.S);
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(p.x + ts.x + 10.0f * a_c.S, p.y + ts.y * 0.55f),
                ImVec2(p.x + availW, p.y + ts.y * 0.55f), Theme::Rule());
            // ★Full white at BODY size. At the caption step it was small and
            // held back at once, and two reductions on a 17px string leave it
            // legible only if you already know what it says. Tracking alone
            // carries "this is a heading" — it costs no contrast.
            OutlinedText(Theme::Chrome(1.0f), txt, Theme::FontBody(), 2.6f * a_c.S);
            ImGui::Dummy(ImVec2(0.0f, 6.0f * a_c.S));
        }

        void DrawSettingsWindow()
        {
            if (!g_showSettings) return;

            auto* wm = WinManager::GetSingleton();
            const float S = Theme::Scale();
            // label column sized to the WIDEST label (e.g. "LANGUAGE") so the
            // value column never overlaps it in any language; generous
            // label->control gap (32px) with a floor so short labels (KO
            // "크기") don't collapse the column
            // measured on the SAME string SettingLabel draws — all caps is
            // wider, so measuring the raw text would let a label run into its
            // control
            const auto lw = [](Lang::Str a_s) {
                return ImGui::CalcTextSize(Lang::UpperCase(Lang::T(a_s)).c_str()).x;
            };
            const float labelW = (std::max)(84.0f * S, 32.0f * S + (std::max)({
                lw(Lang::Str::ScaleLabel),
                lw(Lang::Str::FontScaleLabel),
                lw(Lang::Str::SkinLabel),
                lw(Lang::Str::LanguageLabel),
                lw(Lang::Str::PresetLabel),
                lw(Lang::Str::PresetExport),
                lw(Lang::Str::ShadowDistLabel),
                lw(Lang::Str::ShadowBlurLabel),
                lw(Lang::Str::ShadowOpacLabel),
                lw(Lang::Str::IconBrightLabel),
                lw(Lang::Str::CacheLabel),
                lw(Lang::Str::CaptureLightLabel),
                lw(Lang::Str::PrecacheLabel),
                lw(Lang::Str::MerchGoldSetLabel),
                lw(Lang::Str::WheelEnableLabel),
                lw(Lang::Str::MerchStockSetLabel) }));
            const float trackW = 176.0f * S;
            // language chips sized like image-3: real padding, so the window
            // width follows the actual row width (no dead right margin)
            // ★GI71: the BUILT-IN four only. Packs append to the row but must
            // not widen the window — summing them all would stretch the panel
            // off screen once someone installs a few; instead the row wraps
            // inside the width the four define.
            float langW = 0.0f;
            for (int i = 0; i < 4; ++i) {
                langW += ImGui::CalcTextSize(Lang::DisplayName(i)).x + 16.0f * S;
                if (i < 3) langW += 6.0f * S;
            }
            // ★GI73: three families of two — 2 chips + inner gap per family,
            // plus a family gap (kFamGap, must match RowSkin) or the row runs
            // past the panel edge.
            constexpr float kFamGap = 16.0f;
            // ★★The skin row WRAPS now (RowSkin kRowMax), so this is no longer
            // the sum of every chip — it is the width one line is allowed to
            // reach. Eleven chips on one line would have made the settings
            // window ~160px wider than any other control needs it, to show a
            // row nobody scans left-to-right anyway.
            // Keep kRowMax in RowSkin equal to this.
            const float swatchW = 232.0f * S;
            // glow style chips (silhouette / radial), same chip metrics as langs
            const float glowW =
                ImGui::CalcTextSize(Lang::T(Lang::Str::GlowSilhouette)).x + 16.0f * S +
                6.0f * S +
                ImGui::CalcTextSize(Lang::T(Lang::Str::GlowRadial)).x + 16.0f * S;
            // trade chips (F3/F4): widest of the two Default/<on> pairs
            const float defW = ImGui::CalcTextSize(Lang::T(Lang::Str::ToggleDefault)).x;
            const float tradeW = defW + 16.0f * S + 6.0f * S + 16.0f * S + (std::max)(
                ImGui::CalcTextSize(Lang::T(Lang::Str::ToggleUnlimited)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::ToggleAnything)).x);
            const float insX = Theme::FrameInsetX();
            const float insY = Theme::FrameInsetY();

            // F5: rows live in a scrollable child, so the label column is
            // CHILD-local (the window padding below already carries the
            // torn-frame inset — the old +insX label shift is baked in there).
            const float ctrlW = (std::max)({ trackW, langW, swatchW, glowW, tradeW });

            // height: measured content from the previous frame, clamped to the
            // screen — beyond the clamp the child scrolls (F5). First frame
            // falls back near the old fixed height; one frame later it snaps.
            static float s_wantH = 0.0f;   // desired full window height
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const float maxH = disp.y - 80.0f * S;
            const bool clamped = s_wantH > 0.0f && s_wantH > maxH;
            const float winH = s_wantH > 0.0f ? (std::min)(s_wantH, maxH)
                                              : 440.0f * S + 2.0f * insY;
            const ImVec2 size(
                12.0f + insX + labelW + ctrlW + 12.0f + insX +
                    (clamped ? ImGui::GetStyle().ScrollbarSize : 0.0f),
                winH);
            ImVec2 defPos(200.0f, 200.0f);
            if (auto* mw = wm->Find("main")) {
                defPos = ImVec2(mw->pos.x + mw->size.x - size.x, mw->pos.y + 40.0f * S);
            }
            wm->ApplyNext("settings", defPos, size, WinManager::Anchor::kTopLeft,
                          Theme::TitleTopPad());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                ImVec2(Theme::PadX() + insX, Theme::PadY() + insY));
            ImGui::Begin("##fablerim_settings", nullptr, kManagedWinFlags);
            NoteOverlayRect();
            // ★No height term needed here: s_wantH is measured from `childTop`,
            // which is the cursor TitleBar leaves behind, so the pad is already
            // inside it. Only the first frame's fallback height misses it, and
            // that snaps a frame later.
            wm->TitleBar("settings", Lang::SentenceCase(Lang::T(Lang::Str::Settings)).c_str());

            // EDIT-style lifetime (user request): stays open until the gear
            // toggle or ESC. The old click-outside-closes popup rule ALSO ate
            // titlebar grabs whenever another window overlapped (hover
            // resolved to the front window), so settings could never be
            // dragged freely.

            const SettingsCtx ctx{ labelW, trackW, S };
            const float childTop = ImGui::GetCursorPosY();
            ImGui::BeginChild("##settings_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                ImGuiWindowFlags_NoBackground);
            ImGui::Dummy(ImVec2(0.0f, 4.0f * S));
            for (size_t s = 0; s < std::size(kSettingsSections); ++s) {
                const auto& sec = kSettingsSections[s];
                if (s > 0) ImGui::Dummy(ImVec2(0.0f, 14.0f * S));
                SettingsSectionHeader(ctx, sec.title);
                for (size_t i = 0; i < sec.count; ++i) {
                    sec.rows[i](ctx);
                    if (i + 1 < sec.count) ImGui::Dummy(ImVec2(0.0f, 9.0f * S));
                }
            }
            const float bodyH = ImGui::GetCursorPosY() + 4.0f * S;   // bottom margin
            ImGui::EndChild();
            s_wantH = childTop + bodyH + 8.0f + insY;   // + bottom window padding

            ImGui::End();
            ImGui::PopStyleVar();   // WindowPadding (torn-frame inset)
        }

        // ---- S1: stats panel data ----
        // Displayed weapon damage via the engine routine the vanilla inventory
        // uses (skill/perk adjusted). Falls back to the unarmed AV bare-handed.
        float StatDamageValue()
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return 0.0f;
            auto* entry = p->GetEquippedEntryData(false);
            if (!entry) entry = p->GetEquippedEntryData(true);
            if (!entry) {
                return p->AsActorValueOwner()->GetActorValue(RE::ActorValue::kUnarmedDamage);
            }
            using func_t = float(RE::PlayerCharacter*, RE::InventoryEntryData*);
            // ★RE::Offset is gone from the NG line; this is the id it held
            static REL::Relocation<func_t> func{ RELOCATION_ID(39179, 40253) };
            return func(p, entry);
        }

        // ---- stats block metrics (S1) ------------------------------------
        // ★GI76: every gap around a divider is kStatGap. It used to be 4/7 and
        // 3/8 — the top halves came from rowH's built-in leading and the 3 was
        // not a decision at all but the remainder left over when the block's
        // hard-coded height did not match what the rows actually used. Derive
        // the height from the same numbers and that class of leftover cannot
        // come back.
        // ★These are DERIVED, not frozen. kStatText used to be a literal 17 —
        // "the font size" written down in a second place — so the day the body
        // font moved to 20 the glyphs grew past their own row boxes and every
        // divider gap below them went wrong. Ask the scale instead.
        // money reads in thousands; "6223" makes the eye count digits
        [[nodiscard]] inline std::string Grouped(int a_v)
        {
            char raw[32];
            std::snprintf(raw, sizeof(raw), "%d", a_v < 0 ? -a_v : a_v);
            std::string out;
            const int n = static_cast<int>(std::char_traits<char>::length(raw));
            for (int i = 0; i < n; ++i) {
                if (i && (n - i) % 3 == 0) out += ',';
                out += raw[i];
            }
            return (a_v < 0 ? std::string("-") : std::string()) + out;
        }

        [[nodiscard]] inline float StatValuePx() { return Theme::FontValue(); }
        [[nodiscard]] inline float StatLabelPx() { return Theme::FontCaption(); }
        [[nodiscard]] inline float StatRowH()    { return Theme::FontValue() + 3.0f * Theme::Scale(); }
        constexpr float kStatGap  = 6.0f;    // glyph edge <-> divider, both sides
        constexpr float kCapBarH   = 5.0f;   // capacity bar thickness
        // ★Same air above the bar as below it. It used to be 2px above and 9px
        // below, which reads as the bar being stuck to the SPACE row rather
        // than spaced under it — the mockup gives it equal margins.
        constexpr float kCapBarGap = 6.0f;
        // ★The bar's air BELOW is its own value. Everywhere else kStatGap is
        // the distance from a glyph edge to a rule, but the capacity bar is a
        // solid block, not type — it has no descender slack under it, so the
        // same 6px reads noticeably tighter than every other gap in the panel.
        constexpr float kCapBarBelow = 10.0f;
        // ★The bottom strip's CONTENTS (the GOLD row and the trash can) sit
        // lower than the divider gap alone would place them — user request,
        // for breathing room under the rule. The rule itself and everything
        // above it must NOT move, so this is applied at the two draw sites
        // and never folded into BottomStripY (which the stats block above
        // also reads). It fits inside the strip's own 38px.
        //
        // ★Skin-dependent: a window's bottom margin is `pad + FrameInsetY`,
        // and only skins 1-2 have no inset at all (torn 3-4 = 24px,
        // translucent 5-6 = 6px). The full drop reads right against that
        // extra margin but sits too low without it. ONE function so the
        // amount cannot differ between the gold row and the trash can.
        [[nodiscard]] float GoldStripDrop()
        {
            const float S = Theme::Scale();
            // ★Centred in the strip, not a fixed 5px. The old number was blind
            // to the type size, so the row sat low once the value font grew —
            // and it is the only thing in that strip, with nothing beside it to
            // line up against. kStatGap*S is already added before this lands,
            // so it comes back out here.
            const float centred = (38.0f * S - Theme::FontValue()) * 0.5f - kStatGap * S;
            // torn-frame skins carry a deeper margin below the strip; they
            // still want the extra step down that used to be hard-coded
            return centred + (Theme::FrameInsetY() > 0.0f ? 5.0f * S : 0.0f);
        }

        // total height of the stats block (bodyH reserves this under the doll):
        // gap, 4 rows, gap-to-divider, gap, the space glyph, gap to the GOLD rule
        [[nodiscard]] float StatsPanelH()
        {
            const float S = Theme::Scale();
            const float lead = StatRowH() - StatValuePx();
            // gap, 4 rows, gap-to-divider, gap, space row, capacity bar, gap.
            // The bar block is (gap above - the row's leading) + the bar, and
            // the trailing kStatGap is its air below: equal on both sides.
            return kStatGap * S + 4.0f * StatRowH() + (kStatGap * S - lead) +
                   kStatGap * S + StatRowH() +
                   ((kCapBarGap + kCapBarH) * S - lead) + kCapBarBelow * S;
        }

        // ★GI75: top of the bottom strip (GOLD bar on the left, trash on the
        // right), measured from the body child's top.
        //
        // bodyH is  itemsLabelH + grid + 30 (strip) + 8 (bottom margin), so the
        // grid's bottom border sits at bodyH - 38. The strip used to start at
        // bodyH - 30 — that put the whole margin ABOVE it and left the GOLD
        // divider hanging 8px below the line it is supposed to continue. The
        // margin belongs under the strip, not over it.
        //
        // Everything in that strip and everything stacked on top of it (the
        // stats block) reads its position from here, so the left column and the
        // right column cannot drift apart.
        [[nodiscard]] float BottomStripY(float a_bodyH)
        {
            return a_bodyH - 38.0f * Theme::Scale();
        }

        // S1/S2: [divider, armor/damage/speed/crit, divider, space] pinned
        // ABOVE the GOLD bar in the left column. Space turns crimson while the
        // grid is overloaded (W2) — no extra status line, per the final spec.
        void DrawStatsPanel(float a_leftW, float a_bodyH)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetWindowPos();
            const float rowH = StatRowH();
            const float lpx = StatLabelPx(), vpx = StatValuePx();
            const float trk = 1.6f * S;   // caption tracking, as in the settings headers
            float y = cp.y + BottomStripY(a_bodyH) - StatsPanelH();

            // ★Label and value share ONE style here — same size, same white,
            // same outline. The stats block is a two-column list read straight
            // across, so holding the label back made the left half look
            // disabled rather than subordinate. All caps carries the "this is
            // a name, not a number" difference on its own.
            (void)lpx; (void)trk;
            auto row = [&](const char* a_label, const char* a_val, ImU32 a_valCol) {
                const std::string lbl = Lang::UpperCase(a_label);
                Theme::TextOutlined(dl, ImVec2(cp.x + 2.0f, y), Theme::Chrome(1.0f),
                                    lbl.c_str(), vpx);
                const float w = Theme::TrackedSize(a_val, vpx, 0.0f).x;
                Theme::TextOutlined(dl, ImVec2(cp.x + a_leftW - w - 2.0f, y), a_valCol,
                                    a_val, vpx);
                y += rowH;
            };

            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* avo = p ? p->AsActorValueOwner() : nullptr;
            const ImU32 hi = Theme::Val();
            char buf[48];

            Theme::RuleLine(dl, ImVec2(cp.x, y), ImVec2(cp.x + a_leftW, y));
            y += kStatGap * S;

            std::snprintf(buf, sizeof(buf), "%.0f", StatDamageValue());
            row(Lang::T(Lang::Str::StatDamage), buf, hi);

            std::snprintf(buf, sizeof(buf), "%.0f",
                avo ? avo->GetActorValue(RE::ActorValue::kDamageResist) : 0.0f);
            row(Lang::T(Lang::Str::StatArmor), buf, hi);

            float spd = avo ? avo->GetActorValue(RE::ActorValue::kWeaponSpeedMult) : 0.0f;
            if (spd <= 0.01f) spd = 1.0f;   // engine treats 0 as unmodified
            std::snprintf(buf, sizeof(buf), "%.2fx", spd);
            row(Lang::T(Lang::Str::StatSpeed), buf, hi);

            unsigned crit = 0;
            if (p) {
                if (auto* right = p->GetEquippedObject(false)) {
                    if (auto* weap = right->As<RE::TESObjectWEAP>()) {
                        crit = weap->GetCritDamage();
                    }
                }
            }
            std::snprintf(buf, sizeof(buf), "%u", crit);
            row(Lang::T(Lang::Str::StatCrit), buf, hi);

            // row() already advanced past the last glyph by the row's leading
            y += kStatGap * S - (StatRowH() - StatValuePx());
            Theme::RuleLine(dl, ImVec2(cp.x, y), ImVec2(cp.x + a_leftW, y));
            y += kStatGap * S;

            const bool over = Grid::IsOverloaded();
            const int used = Grid::SpaceUsed(), total = (std::max)(1, Grid::SpaceTotal());
            // ★W3: carry-weight cells are part of the total -- say so, so a
            // potion or a perk visibly moves this figure
            if (const int cwb = Grid::CwBonusCells(); cwb > 0) {
                std::snprintf(buf, sizeof(buf), "%d / %d (+%d)", used, total, cwb);
            } else {
                std::snprintf(buf, sizeof(buf), "%d / %d", used, total);
            }
            const ImU32 spaceCol = over ? IM_COL32(204, 81, 72, 255) : hi;
            row(Lang::T(Lang::Str::StatSpace), buf, spaceCol);

            // ★A bar under the figure. "154 / 832" makes the reader do the
            // division; a bar answers "how full am I" without any reading, and
            // it is the one stat that has a natural maximum to draw against.
            {
                const float bh = kCapBarH * S;
                // measured from the SPACE glyph box's bottom edge, so the air
                // above the bar matches the kStatGap below it
                const float by = y - (StatRowH() - StatValuePx()) + kCapBarGap * S;
                const ImVec2 b0(cp.x + 2.0f, by);
                const ImVec2 b1(cp.x + a_leftW - 2.0f, by + bh);
                // ★The capacity bar is a gauge like any other — same well, so
                // it lightens on a light panel instead of cutting a dark slot
                // across the stats block.
                const float f = (std::min)(1.0f,
                    static_cast<float>(used) / static_cast<float>(total));
                // ★★INK: laid marks, not a rounded well. This bar never went
                // through Theme::Gauge, so teaching the gauge about the ink
                // skin left it behind -- the one rounded rectangle in a painted
                // window, which is precisely the shape that reads as a widget.
                if (Theme::InkChrome()) {
                    // ★★TWICE the reserved height, and no red rule under it.
                    // The band is 5px because a rounded well only needs to be
                    // legible; a laid stroke has to look like something a brush
                    // could have made, and at 3px it read as a hairline with a
                    // colour. The second mark that used to sit below it is gone
                    // and its room went into this one -- one heavy stroke says
                    // "how full" better than a thin one with an underline.
                    const float th = (std::max)(3.0f, bh * 2.0f);
                    const ImVec2 c0(b0.x, b0.y + bh * 0.80f);
                    Theme::InkStroke(dl, c0, b1.x - b0.x, th,
                                     Theme::Col(Theme::S().ink, 0.28f));
                    if (f > 0.0f) {
                        Theme::InkStroke(dl, c0, (b1.x - b0.x) * f, th,
                                         (spaceCol & ~IM_COL32_A_MASK)
                                             | (230u << IM_COL32_A_SHIFT));
                    }
                } else {
                    dl->AddRectFilled(b0, b1, Theme::GaugeTrack(), bh * 0.5f);
                    if (f > 0.0f) {
                        dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * f, b1.y),
                                          spaceCol, bh * 0.5f);
                    }
                }
            }
        }

        // Main inventory window (v9): [tabs + equip doll + GOLD] | [ITEMS + grid]
        // ---- Phase 3: DrawMainWindow helpers (bodies moved verbatim) ----

        // ★★A title-bar control's BOX is this much wider than its label on each
        // side, and that is why the number has a name. The layout below knows
        // only the LABEL's width, so aligning the label's right edge to a
        // margin leaves the drawn box hanging this far past it — measured, the
        // x sat 8px from the frame while FIND, whose widget frame IS its item
        // rect, sat at the 14 both were meant to share. Read by the button that
        // draws the box and by the code that places it.
        float TitleBtnBoxPad() { return 6.0f * Theme::Scale(); }

        // ★The ✕ is drawn at the TITLE's size while its neighbours are
        // body text, so its multiplier is the ratio between the two — and
        // that ratio is SCALE-FREE, because both sides already carry the
        // scale.
        // ★★One function, because the number is read TWICE: once to size the
        // glyph and once to reserve the strip the drag zone must not cover.
        // Those two had drifted into a hardcoded 1.55 and a live
        // recomputation of the same thing.
        // ★The ratio is no longer scale-free in BOTH terms, and that is the
        // point: the title stays put while the body grows with the text-size
        // setting, so the multiplier shrinks by exactly as much and the ✕
        // lands at the same absolute size inside its unchanged bar.
        [[nodiscard]] float TitleCloseMul()
        {
            return Theme::FontTitle() /
                   (std::max)(1.0f, ImGui::GetFontSize());
        }

        // titlebar text button (v10.6): dim tracked text, hover brightens,
        // active = hi + underline (fade on skin 2). Fires on RELEASE — a
        // press-time toggle opened the settings popup and the same click's
        // outside-close check shut it in the very same frame.
        // a_fontMul: draw the label above the shared baseline size (the ✕
        // close glyph was unreadably small at 1.0 — user feedback).
        // a_hitPad: grow the INVISIBLE hitbox around the glyph on every side
        // (the ✕ was readable but needed pixel-perfect aim — user feedback).
        bool TitleBarTextButton(float a_x, float a_ty, const char* a_lbl, float a_w, bool a_on,
                                float a_fontMul = 1.0f, float a_hitPad = 0.0f)
        {
            const auto& sk = Theme::S();
            const float lh = ImGui::GetTextLineHeight() * a_fontMul;
            ImGui::SetCursorScreenPos(ImVec2(a_x - a_hitPad, a_ty - 2.0f - a_hitPad));
            const bool pressed = ImGui::InvisibleButton(a_lbl,
                ImVec2(a_w + 2.0f * a_hitPad, lh + 7.0f + 2.0f * a_hitPad));
            const bool hov = ImGui::IsItemHovered();
            if (hov) Sfx::HoverNote(ImGui::GetItemID());
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec4 col = a_on   ? Theme::BtnOnInkVec()
                             : sk.lightPanel ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                             : (hov ? sk.ink : sk.inkDim);
            // scaled glyphs sit centred on the normal text line
            const float dy = (ImGui::GetTextLineHeight() - lh) * 0.5f;
            // ★A light panel gives these BOXES. Bare text on a dark panel reads
            // as a control because the letters are the only bright thing there;
            // on a light panel it just reads as a label, and the underline that
            // marks "on" is nearly invisible. The reference boxes its title-bar
            // controls for exactly this reason.
            // ★The box is sized from the NORMAL line, but a scaled glyph (the
            // close ×, at 1.55x) is taller than that — so centring the glyph on
            // the normal line dropped it below the box's middle. Give the box
            // the taller of the two and centre the glyph inside THE BOX, not
            // inside a line height the glyph does not use.
            // ★EVERY control in the bar is boxed, and the box is what makes it
            // read as a control — measured off the reference: the inactive one
            // is an OUTLINE ONLY (acc .55, no fill), the active one is the same
            // outline darkened (acc .85) over an acc .55 fill. Marking "on" with
            // a fill alone left the others as bare text, which is what "the EDIT
            // and X buttons have no border" was about.
            // ★The box is sized from the NORMAL line so every control in the bar
            // shares one height, and the glyph is centred in THAT — the close x
            // draws at 1.55x and would otherwise hang below its neighbours.
            const float boxH = ImGui::GetTextLineHeight() + 6.0f;
            const float boxTop = a_ty - 3.0f;
            // ★1.0.5: EVERY skin boxes these now, not just the light one. Bare
            // text read as a caption rather than as a control on the dark
            // skins too — and each of them already owns a square button frame
            // (ImGuiCol_Border + FrameRounding), so this is that same frame,
            // not a new idea. Only the BEVEL stays light-panel-only: it is
            // part of the recessed-button grammar SIMPLE is built on.
            const float pad = TitleBtnBoxPad();
            const ImVec2 b0(a_x - pad, boxTop);
            const ImVec2 b1(a_x + a_w + pad, boxTop + boxH);
            {
                // OFF shows the panel through it, exactly like every other
                // button; ON is the one bright face in the skin.
                // ★Inset by the stroke, same as Sfx::Button — a fill drawn on
                // the frame's own rect bleeds past a rounded corner.
                const float fr = Theme::FrameRounding();
                const ImVec2 f0(b0.x + 1.0f, b0.y + 1.0f);
                const ImVec2 f1(b1.x - 1.0f, b1.y - 1.0f);
                const float  fr2 = (fr > 1.0f) ? fr - 1.0f : 0.0f;
                if (a_on)      dl->AddRectFilled(f0, f1, Theme::BtnOn(), fr2);
                else if (hov)  dl->AddRectFilled(f0, f1, IM_COL32(255, 255, 255, 26), fr2);
                // ★The frame is the SAME one every other control wears
                // (ImGuiCol_Border), not an accent tint. Two things worked
                // against acc here: it is BLUE on a blue title bar, so it has
                // only lightness to separate it, and the bar is TRANSLUCENT —
                // over a dark scene the darkest token on the skin has nothing
                // left to stand against. The ordinary buttons never had this
                // problem because their frame is grey. The fill still says
                // which control is active.
                dl->AddRect(b0, b1, ImGui::GetColorU32(ImGuiCol_Border), fr, 0, 1.0f);
                // same bevel every other button wears (Sfx::Button) — the
                // recessed grammar belongs to the light panel alone
                if (sk.lightPanel) {
                const float bx0 = b0.x + 1.5f, bx1 = b1.x - 1.5f;
                const float by0 = b0.y + 1.5f, by1 = b1.y - 1.5f;
                dl->AddLine(ImVec2(bx0 + fr2, by0), ImVec2(bx1 - fr2, by0),
                            Theme::BevelLit());
                dl->AddLine(ImVec2(bx0, by0 + fr2), ImVec2(bx0, by1 - fr2),
                            Theme::BevelLit());
                dl->AddLine(ImVec2(bx0 + fr2, by1), ImVec2(bx1 - fr2, by1),
                            Theme::BevelShd());
                dl->AddLine(ImVec2(bx1, by0 + fr2), ImVec2(bx1, by1 - fr2),
                            Theme::BevelShd());
                }
            }
            // ★Ink-centred in the BOX, always — not only for the scaled ✕.
            // A line box reserves descender room; "EDIT" has no descender and
            // ✕ has neither, so each label sits at its own wrong offset and no
            // single nudge straightens them together (Theme::TextInkCentered).
            // ★SnapAbs, not SnapPx: GetFontSize() is ALREADY scaled pixels,
            // and SnapPx scales again. The label was drawn at S² while the box
            // around it had been measured at S — which is nothing at S=0.90 and
            // is three controls on top of each other at 4K.
            const float fsz = Theme::SnapAbs(ImGui::GetFontSize() * a_fontMul);
            Theme::TextInkCentered(dl, b0, b1, ImGui::GetColorU32(col), a_lbl, fsz);
            return pressed;
        }

        // right-aligned titlebar controls (F1): … EDIT SETTINGS ✕ — the close
        // button sits at the right edge and closes EVERYTHING at once (no
        // sub-window cascade), with the close sound played up front.
        void DrawTitleBarControls(const ImVec2& a_mainSize,
                                  const char* a_editLbl, const char* a_setLbl,
                                  float a_editW, float a_setW, float a_btnGap)
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            // ★On the TITLE's own line, from the one accessor that knows how
            // that line is built -- these controls drifted off it twice while
            // the formula was copied here by hand.
            const float ty = WinManager::TitleTextY("main", ImGui::GetTextLineHeight());
            const char* closeLbl = "\xC3\x97";   // × (U+00D7, already baked)
            // ★The x is capped at the TITLE size. The old 1.55 multiplier was
            // set against a 17px body and silently became 31px — bigger than
            // the title beside it — the moment the body font moved.
            // ★An ABSOLUTE size, not a ratio. A ratio recomputed from the
            // live font size drifts in the last decimal every frame, and ImGui
            // bakes a new glyph set for every distinct size it is handed.
            const float kCloseMul = TitleCloseMul();
            const float closeW = ImGui::CalcTextSize(closeLbl).x * kCloseMul;
            // ★The same right margin the FIND box below it keeps. ★★Minus the
            // BOX pad as well as the label width: these controls are drawn as
            // boxes and the margin the eye reads is to the box's edge, not to
            // the last letter. Subtracting only closeW aligned the GLYPH and
            // left the frame 6px proud -- an 8px margin under a 14px one,
            // which is the "barely moved" this went through once already.
            // COLLECT needs no such term: Sfx::Button's frame IS its item rect.
            const float xClose = wp.x + a_mainSize.x - Theme::TopControlRightPad() -
                                 closeW - TitleBtnBoxPad();
            const float xSet = xClose - a_btnGap - a_setW;
            const float xEdit = xSet - a_btnGap - a_editW;
            if (TitleBarTextButton(xEdit, ty, a_editLbl, a_editW, Editor::IsEditMode())) {
                Editor::ToggleEditMode();
                if (Editor::IsEditMode()) Sfx::SelectOn();
                else                      Sfx::SelectOff();
            }
            if (TitleBarTextButton(xSet, ty, a_setLbl, a_setW, g_showSettings)) {
                g_showSettings = !g_showSettings;
                if (g_showSettings) Sfx::SelectOn();
                else                Sfx::SelectOff();
            }
            // generous invisible hitbox: the glyph stays this size, the click
            // target doesn't (8px < half the 18px button gap — no overlap)
            const float closePad = 8.0f * Theme::Scale();
            if (TitleBarTextButton(xClose, ty, closeLbl, closeW, false, kCloseMul, closePad)) {
                Sfx::MenuClose();
                GridInventoryMenu::MarkCloseSfxPlayed();   // no OnHide double-play
                Close();
            }
        }

        // park the engine-drawn model: behind the opaque main window, or (for
        // translucent skins) at SCREEN CENTRE under the caching card
        // ★★KEEP THE CAPTURE BOX ON SCREEN. The model is parked behind the main
        // window and photographed out of the BACKBUFFER, so the box around it
        // has to be backbuffer that exists. Park is the window's centre, the
        // window can be dragged to an edge (only its titlebar is kept
        // reachable), and a box hanging off the right or bottom clamps to a
        // negative width -- at which point the pixel path returns without a
        // word and EVERY item times out. Translucent skins never showed it
        // because they park at screen centre regardless.
        // ★Clamp the PARK, not the rect: pulling the rect back instead would
        // photograph empty backbuffer beside the model.
        // ★Every SetParkPos goes through here. The open-time anchor bypassed
        // this rule while the per-frame one obeyed it, which would have left
        // exactly the first (and most expensive) capture pass unprotected.
        [[nodiscard]] ImVec2 ParkOnScreen(ImVec2 a_park)
        {
            const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
            if (sz.width <= 0 || sz.height <= 0) return a_park;
            const ImVec2 centre(static_cast<float>(sz.width) * 0.5f,
                                static_cast<float>(sz.height) * 0.5f);
            const ImVec2 box = ItemPreview::GetSingleton()->CaptureCover();
            if (box.x <= 0.0f || box.y <= 0.0f) return a_park;   // no shot sized yet
            const float hw = box.x * 0.5f, hh = box.y * 0.5f;
            // A box larger than the screen cannot be satisfied — centre it and
            // let the rect clamp take the largest slice available.
            a_park.x = (box.x >= static_cast<float>(sz.width))  ? centre.x
                     : (std::max)(hw, (std::min)(static_cast<float>(sz.width) - hw, a_park.x));
            a_park.y = (box.y >= static_cast<float>(sz.height)) ? centre.y
                     : (std::max)(hh, (std::min)(static_cast<float>(sz.height) - hh, a_park.y));
            return a_park;
        }

        void ParkPreviewModel(const ImVec2& a_mainSize)
        {
            auto* pv = ItemPreview::GetSingleton();
            if (Theme::S().translucent) {
                const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
                pv->SetParkPos(ImVec2(static_cast<float>(sz.width) * 0.5f,
                                      static_cast<float>(sz.height) * 0.5f));
                return;
            }
            const ImVec2 wp = ImGui::GetWindowPos();
            pv->SetParkPos(ParkOnScreen(
                ImVec2(wp.x + a_mainSize.x * 0.5f, wp.y + a_mainSize.y * 0.5f)));
        }

        // GOLD bar — pinned to the bottom of a column of width a_colW.
        // a_rightReserve (F2): non-zero when the trash-can button SHARES this
        // strip (compact mode, where the bar spans the grid column).
        void DrawGoldBar(float a_colW, float a_bodyH, float a_rightReserve = 0.0f)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetWindowPos();
            const float gy = cp.y + BottomStripY(a_bodyH);   // the rule: unmoved
            const float ty = gy + kStatGap * S + GoldStripDrop();
            Theme::RuleLine(dl, ImVec2(cp.x, gy), ImVec2(cp.x + a_colW, gy));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s", Grouped(Grid::GoldAmount()).c_str());

            char lbl[48];
            if (sk.diamondLabels) {   // v10.4: crimson "◇ GOLD"
                std::snprintf(lbl, sizeof(lbl), "\xE2\x97\x87 %s", Lang::T(Lang::Str::Gold));
            } else {
                std::snprintf(lbl, sizeof(lbl), "%s", Lang::T(Lang::Str::Gold));
            }
            // same single style the stats rows use
            const float vpx = StatValuePx();
            const ImVec2 ls = Theme::TrackedSize(lbl, vpx, 0.0f);
            Theme::TextOutlined(dl, ImVec2(cp.x + 2.0f, ty),
                sk.diamondLabels ? Theme::Col(sk.sel, 1.0f) : Theme::Chrome(1.0f),
                lbl, vpx);

            // ★The amount follows its LABEL when the trash can shares the strip.
            //  Right-aligning it there parked the number against the trash glyph,
            //  which reads as "delete 6223 gold" instead of "you have 6223"
            //  (user report). In the plain left column there is no trash button
            //  and the stats rows above are label-left / value-right, so the
            //  amount stays right-aligned to match them.
            if (a_rightReserve > 0.0f) {
                Theme::TextOutlined(dl, ImVec2(cp.x + 2.0f + ls.x + 10.0f * S, ty),
                    Theme::GoldCol(), buf, vpx);
            } else {
                const float amtW = Theme::TrackedSize(buf, vpx, 0.0f).x;
                Theme::TextOutlined(dl, ImVec2(cp.x + a_colW - amtW - 2.0f, ty),
                    Theme::GoldCol(), buf, vpx);
            }
        }

        // F2: trash-can button in the bottom-right strip of the grid column.
        // Vector glyph (lid + body + ribs) — no baked icon needed. Toggles
        // the 6x4 trash window.
        void DrawTrashButton(float a_colW, float a_bodyH)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetWindowPos();
            const float side = 18.0f * S;
            // top-aligned with the GOLD figure: in compact layout both sit in
            // this same strip, and a 2px offset between them showed
            // ★★The SEAL decides where the can sits, not the other way round.
            // The can was flush to the column's right edge, which is correct
            // for an 18px glyph and wrong the moment something twice its size
            // is stamped behind it -- the seal ran off the page. Both move
            // left together; the button is the thing you aim at, and it has to
            // stay on its own mark.
            // ★★The SEAL's right edge is placed, and the can follows it. a_colW
            // is the grid's exact width, so cp.x + a_colW IS the line the board
            // ends on -- putting the stamp there lines it up with the column of
            // cells above instead of with the 18px glyph in front of it.
            // ★Expressed as one equation rather than two offsets: the can used
            // to be positioned and the seal drawn around it, so every change to
            // the seal's size moved it off the grid again.
            constexpr float kSealMul  = 1.64f;   // seal size, x the can's size
            // ★NEGATIVE: just INSIDE the line, not over it. cp.x + a_colW is
            // also where this child's clip ends, so anything lapping past it is
            // not drawn over the border -- it is cut off. Same boundary the
            // doll's right column taught, met from the other side.
            constexpr float kSealBite = -0.08f;  // ...and how far it sits inside
            float px = cp.x + a_colW - side - 2.0f;
            if (Theme::InkChrome()) {
                const float half = side * kSealMul * 0.5f;
                px = cp.x + a_colW + side * kSealBite - half - side * 0.5f;
            }
            const ImVec2 p0(px,
                            cp.y + BottomStripY(a_bodyH) + kStatGap * S + GoldStripDrop());
            ImGui::SetCursorScreenPos(ImVec2(p0.x - 4.0f * S, p0.y - 4.0f * S));
            const bool pressed = ImGui::InvisibleButton("##gi_trashbtn",
                ImVec2(side + 8.0f * S, side + 8.0f * S));
            const bool hov = ImGui::IsItemHovered();
            if (hov) Sfx::HoverNote(ImGui::GetItemID());
            const bool on = Grid::IsTrashOpen();
            const ImU32 col = on    ? Theme::Col(sk.sel, 1.0f)
                              : hov ? ImGui::GetColorU32(sk.ink)
                                    : ImGui::GetColorU32(sk.inkDim);
            const float w = side, h = side;
            const float t = (std::max)(1.0f, 1.2f * S);
            // ★★A seal UNDER the can, on the ink skins. It is the only stamped
            // mark in the window and the only place the paper is signed, so it
            // goes where the page ends -- bottom right, under the one control
            // that lives there. Drawn first: the can is the button, the seal is
            // the paper it was pressed onto.
            if (Theme::InkChrome()) {
                Theme::InkSeal(dl, ImVec2(p0.x + w * 0.5f, p0.y + h * 0.5f),
                               side * kSealMul, on ? 0.60f : hov ? 0.48f : 0.34f);
            }
            // lid + handle
            dl->AddLine(ImVec2(p0.x + 0.10f * w, p0.y + 0.18f * h),
                        ImVec2(p0.x + 0.90f * w, p0.y + 0.18f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.35f * w, p0.y + 0.06f * h),
                        ImVec2(p0.x + 0.65f * w, p0.y + 0.06f * h), col, t);
            // tapered body
            dl->AddLine(ImVec2(p0.x + 0.18f * w, p0.y + 0.18f * h),
                        ImVec2(p0.x + 0.28f * w, p0.y + 0.95f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.82f * w, p0.y + 0.18f * h),
                        ImVec2(p0.x + 0.72f * w, p0.y + 0.95f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.28f * w, p0.y + 0.95f * h),
                        ImVec2(p0.x + 0.72f * w, p0.y + 0.95f * h), col, t);
            // ribs
            dl->AddLine(ImVec2(p0.x + 0.42f * w, p0.y + 0.32f * h),
                        ImVec2(p0.x + 0.44f * w, p0.y + 0.82f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.58f * w, p0.y + 0.32f * h),
                        ImVec2(p0.x + 0.56f * w, p0.y + 0.82f * h), col, t);
            if (pressed) Grid::ToggleTrash();
        }

        // ---- GI63: contextual prompt bar, bottom edge of the SCREEN ----------
        //
        // ★The division of labour with the tooltip is the whole design:
        //   tooltip  = what can I do with THIS ITEM   (needs a hover)
        //   this bar = what can I do RIGHT NOW        (needs no hover)
        // Several of these could never live in a tooltip at all -- orbiting the
        // 3D view, the wheel zoom, "closing the trash confirms the delete" --
        // because they are not attached to any item.
        //
        // Drawn on the FOREGROUND list against the screen, not a window, so it
        // costs the inventory no height and stays put while the cursor moves.
        // ★A hint for the bottom bar, set by whatever is hovered THIS frame.
        // The bag's COLLECT used to raise a floating tooltip right under the
        // cursor, which covered the very grid the player was aiming at — and
        // every other hover in this UI already answers in the bar.
        std::string g_hoverHint;
        int         g_hoverHintFrame = -1;

        // ★PromptBit moved to the header and the entry point is public now —
        // the quick menu draws THIS bar rather than a lookalike, and two
        // copies of the keycap chrome would have drifted apart within a
        // release. The body stays here; the public name forwards to it.
        void DrawPromptRowImpl(const std::vector<PromptBit>& a_bits, bool a_warn, float a_fade)
        {
            if (a_bits.empty() || a_fade <= 0.01f) return;
            const auto& io = ImGui::GetIO();
            auto* fg = ImGui::GetForegroundDrawList();
            // ★★PINNED, and pinned to VALUES rather than to a skin. This bar is
            // not part of any window — it floats on the WORLD at the bottom of
            // the screen, so what it has to stand against is the game, never
            // the panel. Following the active skin gave a pale skin pale
            // keycaps over a bright room, where they vanished.
            // ★★It used to read Theme::SkinAt(4), "Glass Dark", the one palette
            // in the table built to sit on the world. Then Glass was removed
            // and index 4 became Parchment Amber — a pale sheet — and the bar
            // silently went back to the exact failure the pinning existed to
            // prevent. Nothing warned: an index is always a valid skin. So the
            // three colours it actually used are written out here. Borrowing
            // from a table it does not belong to was the coupling.
            const struct { ImVec4 acc, sel, inkDim; } sk = {
                ImVec4(212 / 255.0f, 212 / 255.0f, 216 / 255.0f, 1.00f),   // silver hairline
                ImVec4(134 / 255.0f,  38 / 255.0f,  28 / 255.0f, 1.00f),   // rust, for a warning
                ImVec4(228 / 255.0f, 228 / 255.0f, 232 / 255.0f, 0.55f),   // the lettering
            };
            const float S = Theme::Scale();
            const float gap = 6.0f * S;      // between a key and its label
            const float wide = 13.0f * S;    // between groups
            const float capPad = 5.0f * S;
            const float capH = ImGui::GetTextLineHeight() + 3.0f * S;

            auto capW = [&](const std::string& k) {
                return (std::max)(capH, ImGui::CalcTextSize(k.c_str()).x + capPad * 2.0f);
            };

            // A bare key (no label of its own) is one half of a PAIR -- the A of
            // "A D rotate" -- so it hugs the next bit instead of taking a full
            // group gap, or the two read as separate prompts.
            auto lead = [&](std::size_t i) {
                if (!i) return 0.0f;
                if (a_bits[i].sep) return wide * 2.0f;
                return a_bits[i - 1].label.empty() ? gap * 0.7f : wide;
            };

            float total = 0.0f;
            for (std::size_t i = 0; i < a_bits.size(); ++i) {
                const auto& b = a_bits[i];
                total += lead(i);
                if (!b.key.empty()) {
                    total += capW(b.key);
                    if (!b.label.empty()) total += gap;
                }
                total += ImGui::CalcTextSize(b.label.c_str()).x;
            }

            const float y = io.DisplaySize.y - 44.0f * S;
            float x = (io.DisplaySize.x - total) * 0.5f;

            // vignette: the bar has to read over a snowfield or a dark crypt
            // alike, and a flat plate would box in a screen that has no other
            // chrome at its edge.
            // ★GI63: the vignette is not one linear ramp. The band the TEXT sits
            // in is held at full darkness so the row reads the same over a
            // snowfield and a crypt; only above it does the darkness let go, and
            // it lets go on a CURVE (alpha ~ t^1.7) so the top edge dissolves
            // instead of ending on a visible line. A straight ramp put the
            // halfway tone right behind the text and drew its own horizon.
            //
            // ImGui interpolates a rect linearly, so the curve is approximated
            // by stacked bands -- ten is past the point the seams are findable.
            // Reach: 200px of fade, but it is SPENT in the lower quarter -- see
            // the ramp below. The long tail is what makes the top edge
            // undetectable; the short saturation keeps the text band solid.
            const float vh = 200.0f * S;        // total reach
            const float aMax = 195.0f * a_fade;
            const float top = io.DisplaySize.y - vh;

            // ★ONE smoothstep, no separate solid rectangle. It starts at 4% and
            // is fully dark by 77.5% of the way down -- 45px off the bottom,
            // just above the text -- so the row's own band never varies.
            // Both ends of a smoothstep have zero slope, and that is what
            // removes the seams: a power curve arrives at the flat section
            // still climbing, and the eye reads that kink as an edge (it showed
            // as bands in the first build). Zero slope at the join leaves
            // nothing to see.
            // ★The constants are coupled to `vh`: (0.04 + 0.735) * 200 = 155px
            // down = 45px up from the bottom, which must clear the text's top
            // edge at 44px. Change the height and these move with it.
            auto ramp = [](float t) {
                const float u = (std::min)(1.0f, (std::max)(0.0f, (t - 0.04f) / 0.735f));
                return u * u * (3.0f - 2.0f * u);
            };
            // 200 slices over ~200px: one per pixel. Slice count costs nothing
            // in fill (they tile, they do not overlap) -- only vertices, and
            // that many quads is noise. Per-pixel means the ramp is as smooth as
            // the 8-bit alpha channel allows; there is no kink left to find.
            constexpr int kBands = 200;
            for (int i = 0; i < kBands; ++i) {
                const float t0 = static_cast<float>(i) / kBands;
                const float t1 = static_cast<float>(i + 1) / kBands;
                const auto c0 = IM_COL32(0, 0, 0, static_cast<int>(aMax * ramp(t0)));
                const auto c1 = IM_COL32(0, 0, 0, static_cast<int>(aMax * ramp(t1)));
                fg->AddRectFilledMultiColor(
                    ImVec2(0.0f, top + vh * t0),
                    ImVec2(io.DisplaySize.x, top + vh * t1), c0, c0, c1, c1);
            }

            // every colour rides the same fade, vignette included, so the whole
            // strip arrives and leaves as one object rather than text-then-plate
            auto fade = [&](ImVec4 c) { c.w *= a_fade; return ImGui::GetColorU32(c); };
            // ★★The keycap CHROME has to be pinned too. Only the two text
            // colours below took `sk`; the cap fill, its rim and the group
            // separator called Theme::Acc, which reads the ACTIVE skin — so
            // the bar was half pinned and the caps went on changing colour
            // underneath fixed lettering.
            // Glass Dark was translucent and not light-panelled, so Theme::Acc
            // gave it the x1.9 line boost; applying it in the same order (alpha
            // first, then boost, then clamp) keeps the bar pixel-identical to
            // what it always looked like, now that the skin itself is gone.
            auto acc = [&](float a) {
                ImVec4 c = sk.acc;
                c.w = (std::min)(1.0f, a * 1.9f);
                return ImGui::GetColorU32(c);
            };
            const ImU32 keyCol  = fade(a_warn ? sk.sel : sk.acc);
            const ImU32 textCol = fade(a_warn ? sk.sel : sk.inkDim);
            const ImU32 sepCol  = acc(0.22f * a_fade);

            for (std::size_t i = 0; i < a_bits.size(); ++i) {
                const auto& b = a_bits[i];
                if (b.sep && i) {
                    const float sx = x + wide;
                    fg->AddLine(ImVec2(sx, y + 2.0f * S),
                                ImVec2(sx, y + capH - 2.0f * S), sepCol, 1.0f);
                }
                x += lead(i);
                if (!b.key.empty()) {
                    const float w = capW(b.key);
                    // keycap: ghost fill + accent rim, so it reads as a KEY
                    // rather than as another word in the sentence
                    fg->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + capH),
                        acc((a_warn ? 0.10f : 0.09f) * a_fade), 3.0f * S);
                    fg->AddRect(ImVec2(x, y), ImVec2(x + w, y + capH),
                        a_warn ? fade(sk.sel) : acc(0.55f * a_fade), 3.0f * S);
                    const ImVec2 ts = ImGui::CalcTextSize(b.key.c_str());
                    fg->AddText(ImVec2(x + (w - ts.x) * 0.5f,
                                       y + (capH - ts.y) * 0.5f), keyCol, b.key.c_str());
                    x += w;
                    if (!b.label.empty()) x += gap;
                }
                const ImVec2 ls = ImGui::CalcTextSize(b.label.c_str());
                fg->AddText(ImVec2(x, y + (capH - ls.y) * 0.5f), textCol, b.label.c_str());
                x += ls.x;
            }
        }

        // Which state owns the bar this frame. Ordered by how modal each one is:
        // an open 3D view is on top of everything, an overload warning is the
        // background hum that anything else outranks.
        void DrawPromptBar()
        {
            using S = Lang::Str;
            auto T = [](S s) { return std::string(Lang::T(s)); };
            auto K = [](Act a) { return std::string(KeyLabel(a)); };
            std::vector<PromptBit> bits;
            bool warn = false;

            const auto mode = LootBarter::CurrentMode();

            // a hovered control's own words outrank the ambient hints
            if (g_hoverHintFrame == ImGui::GetFrameCount() && !g_hoverHint.empty()) {
                bits = { { "", g_hoverHint } };
            } else if (IsInspectOpen()) {
                bits = { { "", T(S::PromptDrag) }, { "", T(S::PromptOrbit) },
                         { "", T(S::PromptWheel), true }, { "", T(S::PromptZoom) },
                         { K(Act::kDrop), T(S::PromptReset), true },
                         { "ESC", T(S::PromptClose), true } };
            // an explicit click deserves an answer, so this outranks every
            // ambient state below — but not an open 3D view, which is modal
            // and cannot be reached from the settings window anyway
            } else if (ImGui::GetTime() < g_flatReloadNote) {
                bits = { { "", T(S::IconReloadDone) } };
            } else if (LootBarter::SliderActive() || Grid::IsPouchOpen() ||
                       LootBarter::ConfirmActive() || Grid::IsTrashConfirmOpen() ||
                       Equip::IsPopupOpen()) {
                // the pouch withdraw window answers the same keys as the
                // quantity slider -- it just is not a LootBarter one.
                // ★On a pad the row names the BUTTONS, now that the buttons
                // work here (TranslatePadButtons remaps them while a popup is
                // up): A confirms, Y is Max, the d-pad walks the count, and B
                // cancels through the same user-event channel as ESC. A row
                // that said "Enter" to a player holding a controller was a
                // hint about somebody else's hands.
                // ★The confirm dialogs (sell, trash, equip) join the branch:
                // they answer the same keys minus the counting pair.
                const bool pad = g_padActive.load();
                const bool counting = LootBarter::SliderActive() || Grid::IsPouchOpen();
                bits.clear();
                if (counting) {
                    if (pad) {
                        bits.push_back({ "D-Pad", T(S::PromptStep) });
                    } else {
                        bits.push_back({ "\xE2\x86\x90", "" });
                        bits.push_back({ "\xE2\x86\x92", T(S::PromptStep) });
                    }
                }
                bits.push_back({ pad ? KeyLabel(Act::kPrimary) : "Enter",
                                 T(S::Confirm), !bits.empty() });
                if (LootBarter::SliderActive()) {
                    bits.push_back({ pad ? KeyLabel(Act::kDrop) : "MAX",
                                     T(S::PromptMax), true });
                }
                bits.push_back({ pad ? "B" : "ESC", T(S::Cancel), true });
            } else if (Grid::IsHolding()) {
                bits = { { K(Act::kPrimary), T(S::PromptPlace) },
                         { K(Act::kSecondary), T(S::Cancel), true } };
                if (Grid::HeldCanRotate()) {
                    bits.insert(bits.begin(), { K(Act::kRotateCCW), "" });
                    bits.insert(bits.begin() + 1, { K(Act::kRotateCW), T(S::ActRotate) });
                    bits[2].sep = true;   // divider after the rotate group
                }
            // ★(1.5.0 audit) HOVER OUTRANKS the trash's standing row. The verb
            // resolver already answers "discard" over a board item and
            // "restore" over a parked one while the bin is open -- but this
            // branch sat ahead of the hover branch, so the bar kept promising
            // "restore" over an item the click would BIN. The warning row
            // stays for the idle bar.
            } else if (Grid::IsTrashOpen() && !Grid::HoveredPrompt().active) {
                warn = true;
                bits = { { "", T(S::WarnTrashClose) },
                         { K(Act::kSecondary), T(S::ActRestore), true } };
            } else if (Editor::IsEditMode()) {
                // ★★Hovering an item has to answer even here. EDIT's own hints
                // describe the 3D preview — what you do AFTER choosing
                // something — so sitting ahead of the hover branch left the one
                // action that STARTS the whole mode with no prompt at all.
                // Split inside this branch rather than reordering the chain:
                // the item prompts below assume the normal (carry) verbs, and
                // in EDIT a click selects instead of picking up.
                // ★An edit that is about to be lost outranks every hint here.
                // Discarding is silent by design (a confirm on every item
                // switch would make EDIT unusable), so the bar is where the
                // player is told it happened — and told beforehand that
                // something is still unsaved.
                if (Editor::DiscardNoteActive()) {
                    warn = true;
                    bits = { { "", T(S::EditDiscarded) } };
                } else if (Editor::HasUnsavedEdits()) {
                    warn = true;
                    bits = { { "", T(S::EditUnsaved) },
                             { "", T(S::EditSave), true } };
                } else if (const auto hp = Grid::HoveredPrompt(); hp.active) {
                    bits = { { K(Act::kPrimary), T(S::PromptSelect) } };
                } else {
                    bits = { { "", T(S::PromptDrag) }, { "", T(S::PromptOrbit) },
                             { "", T(S::PromptWheel), true }, { "", T(S::PromptZoom) } };
                }
            } else if (const auto hp = Grid::HoveredPrompt(); hp.active) {
                // ★Hovering an item takes the bar, and it carries EVERY key the
                // tooltip used to list. They appear only when they apply: the
                // right-click verb is whatever this item does (equip / use /
                // read / learn / sell / store / buy / steal / open bag /
                // restore / withdraw / unequip), Shift only on a stack, compare
                // only on gear.
                // Grouped, not evenly spaced: the two mouse buttons belong
                // together, the two Shift forms together, discard and star
                // together. A divider before every entry turns six prompts into
                // six competing ones -- three groups reads as a sentence.
                if (hp.canPick) {
                    bits.push_back({ K(Act::kPrimary), T(S::ActPickUp) });
                }
                if (hp.hasVerb) {
                    bits.push_back({ K(Act::kSecondary), T(hp.verb) });
                }
                // ★(1.5.0) shelf USE MODE: a container's book reads (a tome
                // learns) in place on Shift+right-click -- the one verb of
                // that board no click could discover on its own
                if (hp.canShelfUse) {
                    bits.push_back({ K(Act::kSplit) + "+" + K(Act::kSecondary),
                                     T(hp.useVerb) });
                }
                if (hp.canCompare) {
                    bits.push_back({ K(Act::kSplit), T(S::ActCompare), !bits.empty() });
                }
                if (hp.canSplit) {
                    bits.push_back({ K(Act::kSplit) + "+" + K(Act::kPrimary),
                                     T(S::PromptQty), !bits.empty() && !hp.canCompare });
                }
                if (hp.canDrop) {
                    bits.push_back({ K(Act::kDrop), T(S::ActDrop), !bits.empty() });
                }
                if (hp.canFav) {
                    bits.push_back({ K(Act::kFavorite), T(S::ActFavorite),
                                     !bits.empty() && !hp.canDrop });
                }
                // ★Grouped with inspect rather than with discard/star: those
                // two act on the item's place in the bag, this one and 3D look
                // at the item itself.
                if (hp.canRecharge) {
                    bits.push_back({ K(Act::kRecharge), T(S::ActRecharge),
                                     !bits.empty() });
                }
                bits.push_back({ K(Act::kInspect), T(S::PromptInspect),
                                 !bits.empty() && !hp.canRecharge });
            } else if (mode == LootBarter::Mode::kPickpocket) {
                warn = true;
                bits = { { "", T(S::WarnPickpocket) } };
            // ★No barter row on purpose. The tooltip already names the verb
            // (buy / sell, which side of the counter decides) AND offers Shift
            // to set a quantity -- and it only offers it on a stack, which a
            // standing bar cannot know. Repeating it here would be the exact
            // duplication this bar exists to avoid, so barter falls through to
            // the overload warning instead.
            } else if (LootBarter::IsLootMode(mode)) {
                bits = { { K(Act::kDrop), T(S::PromptTakeAll) } };
            } else if (Grid::IsOverloaded()) {
                warn = true;
                bits = { { "", T(S::WarnOverload) }, { "", T(S::WarnOverloadFix), true } };
            }

            // ★Fade, and hold the last row while it fades OUT. Without the
            // hold there is nothing left to draw the moment the state ends, so
            // the strip would vanish on the frame it was told to leave and the
            // fade would only ever be visible on the way in.
            static float s_fade = 0.0f;
            static std::vector<PromptBit> s_shown;
            static bool s_shownWarn = false;
            if (!bits.empty()) {
                s_shown = bits;
                s_shownWarn = warn;
            }
            const float dt = (std::min)(ImGui::GetIO().DeltaTime, 0.1f);
            const float target = bits.empty() ? 0.0f : 1.0f;
            s_fade += (target - s_fade) * (1.0f - std::exp(-dt * 14.0f));
            if (s_fade > 0.995f) s_fade = 1.0f;
            DrawPromptRow(s_shown, s_shownWarn, s_fade);
        }

        void DrawMainWindow()
        {
            const auto& io = ImGui::GetIO();
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            const float insX = Theme::FrameInsetX();   // tornFrame breathing room
            const float insY = Theme::FrameInsetY();
            auto* wm = WinManager::GetSingleton();

            // A안: loot/barter mode hides the EQUIP doll + stats — the player
            // window is just the item grid + GOLD bar, mirroring the partner
            // window. Plain inventory keeps the full left column.
            const bool  compact = LootBarter::CurrentMode() != LootBarter::Mode::kNormal;
            const float barH   = 34.0f * S;
            const float pad    = Theme::PadX() * S;
            // ★The BOTTOM margin is not the side margin. `pad` closes the
            // window under the gold bar as well as spacing the columns, so
            // narrowing the sides silently cropped the window's foot too.
            const float padB   = 12.0f * S;
            const float leftW  = compact ? 0.0f : Equip::PanelW();
            // exact grid width — the legacy +20 scrollbar slack made the
            // right margin visibly wider than the left (v10.7 feedback)
            const float gridW  = Grid::kCols * Grid::CellPx();
            // grid column height = ITEMS label row + the grid itself + the
            // 30px bottom strip (GOLD bar / trash button). The label row was
            // missing here, so the strip's baseline sat ON the last grid row
            // and the trash button overlapped the cells (user-reported).
            float itemsLabelH = ImGui::GetTextLineHeightWithSpacing() + 3.0f * S;
            // ★GI77: with the doll beside it, the grid starts on the SAME line
            // as the first equipment slot. The two columns are read as one
            // board, and a grid whose top edge floats above the doll's makes
            // the window look like two panels that happened to be stacked.
            // The offset is the tab strip's measured height from last frame —
            // 0 only before the first draw, where the label height stands in.
            if (!compact) {
                if (const float slotsTop = Equip::SlotsTopOffset(); slotsTop > 0.0f) {
                    itemsLabelH = slotsTop;
                }
            }
            const float gridBodyH = itemsLabelH + Grid::kMinRows * Grid::CellPx() + 30.0f * S;
            // left column must fit doll + stats panel + GOLD bar (S1); compact
            // reserves the GOLD-bar strip under the grid instead
            // ★The doll-to-stats gap is what's LEFT OVER, not a fixed 44.
            // BottomStripY is derived from bodyH, so whenever the grid column
            // is the taller of the two the stats panel's last rule lands on
            // exactly the grid's last row line — the layout was always built
            // to do that. A fixed gap made the left column win instead, by a
            // different amount at every cell size (the grid shrinks with the
            // cell, the stats panel doesn't — it is type). Letting the gap
            // absorb the difference keeps the two columns level.
            // ★★AND THE FLOOR HAS TO PAY FOR THE STRIP. BottomStripY reserves
            // 38*S under the body for the GOLD bar and the stats block is
            // pinned ABOVE that, while bodyH adds 8*S back at the end -- so the
            // air the player actually sees between doll and stats is
            // (dollGap - 30*S), not dollGap. A 12*S floor therefore started the
            // block 18*S INSIDE the doll the moment the grid column stopped
            // being the taller one, which is every cell scale below ~0.85: the
            // stat rows landed on the bottom row of equipment slots. Only
            // visible once the scale floor was lowered (1.2.1) -- the old
            // minimum of 0.85 sat right on the edge of it.
            constexpr float kDollStatsAir = 12.0f;   // what the eye should see
            const float dollGap = (std::max)((kDollStatsAir + 38.0f - 8.0f) * S,
                gridBodyH - Equip::PanelH() - StatsPanelH());
            const float bodyH  = (compact
                ? gridBodyH + 30.0f * S
                : (std::max)(Equip::PanelH() + dollGap + StatsPanelH(), gridBodyH)) + 8.0f * S;
            // ★PAID FOR HERE. TitleBar only spends the pad; the height has to
            // grow by the same number or the extra clearance at the top comes
            // straight out of the gold bar's margin at the foot.
            const float topPad = Theme::TitleTopPad();
            const ImVec2 mainSize(compact
                    ? pad + gridW + pad + 2.0f * insX
                    : pad + leftW + pad + 1.0f + pad + gridW + pad + 2.0f * insX,
                barH + bodyH + padB + 2.0f * insY);

            // ★Pin the RIGHT edge across the compact/normal size change. The
            //  item grid is the half that exists in both layouts and it sits
            //  against the right frame, so anchoring there keeps the grid --
            //  and anything the user docked beside it -- perfectly still while
            //  the equipment column folds away to the left, into the space the
            //  partner window occupies anyway.
            wm->ApplyNext("main",
                ImVec2((io.DisplaySize.x - mainSize.x) * 0.5f,
                       (io.DisplaySize.y - mainSize.y) * 0.5f),
                mainSize, WinManager::Anchor::kTopRight, topPad);

            if (!ImGui::Begin("##fablerim_main", nullptr, kManagedWinFlags)) {
                ImGui::End();
                return;
            }
            const char* editLbl = Lang::T(Lang::Str::Edit);
            const char* setLbl = Lang::T(Lang::Str::Settings);
            const float editW = ImGui::CalcTextSize(editLbl).x;
            const float setW = ImGui::CalcTextSize(setLbl).x;
            const float btnGap = 18.0f * S;
            const float closeW = ImGui::CalcTextSize("\xC3\x97").x * TitleCloseMul();
            // strip excludes the right-aligned control zone (EDIT + SETTINGS
            // + ✕) so the buttons below actually receive their clicks
            wm->TitleBar("main", Lang::T(Lang::Str::Inventory),
                pad + insX + editW + setW + closeW + 2.0f * btnGap + 14.0f * S);

            const ImVec2 bodyTop = ImGui::GetCursorScreenPos();

            DrawTitleBarControls(mainSize, editLbl, setLbl, editW, setW, btnGap);
            ParkPreviewModel(mainSize);

            // controls moved the cursor — body starts back under the titlebar
            ImGui::SetCursorScreenPos(bodyTop);

            // ---- left column: tabs + doll + GOLD bar (plain inventory only) ----
            if (!compact) {
                ImGui::BeginChild("fab_left", ImVec2(leftW, bodyH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                Equip::Draw();
                DrawStatsPanel(leftW, bodyH);   // S1/S2: combat + space, above GOLD
                DrawGoldBar(leftW, bodyH);
                ImGui::EndChild();

                // vertical divider
                auto* dl = ImGui::GetWindowDrawList();
                const float dx = bodyTop.x + leftW + pad;
                // ★This one asked for Acc(0.18), not Rule(), which is why the
                // first sweep over the dividers missed it: the search was for
                // the rule COLOUR, and a divider that names a different colour
                // is still a divider. Ask the skin instead.
                Theme::RuleLine(dl, ImVec2(dx, bodyTop.y),
                                ImVec2(dx, bodyTop.y + bodyH));
            }

            // ---- right column: ITEMS label + grid (+ GOLD bar when compact) ----
            const float rightX = compact ? bodyTop.x : bodyTop.x + leftW + pad + 1.0f + pad;
            ImGui::SetCursorScreenPos(ImVec2(rightX, bodyTop.y));
            ImGui::BeginChild("fab_right", ImVec2(gridW, bodyH), ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                SectionLabel(Lang::T(Lang::Str::Items));
                auto* cache = IconCache::GetSingleton();
                if (cache->IsBusy()) {
                    ImGui::SameLine();
                    ImGui::TextColored(sk.inkDim, "  %s %zu",
                        Lang::T(Lang::Str::Caching), cache->QueuedCount());
                }
                // ★Search rides the ITEMS header, right-aligned. It belongs to
                // the board underneath it — putting it in the settings window
                // would make "where is my healing potion" a two-window job.
                // Right-aligned so it never pushes the label around as the
                // caching counter comes and goes.
                {
                    // ★★Position from the child's RIGHT EDGE, not from what is
                    // left at the cursor. GetContentRegionAvail was being read
                    // BEFORE SameLine — at that moment the cursor sits on the
                    // next line at x=0, so it reported the FULL width, and
                    // adding it to the post-SameLine cursor pushed the box past
                    // the edge by the width of the "ITEMS" label. The overflow
                    // was larger than any margin could hide, which is why the
                    // right border stayed missing after the first fix.
                    // GetContentRegionMax is the edge itself and does not move
                    // with the cursor.
                    const float S = Theme::Scale();
                    const float edge = 2.0f * S;   // clear of the clip boundary
                    const float sw = 148.0f * S;
                    const float right = ImGui::GetContentRegionMax().x;
                    if (right > sw + edge + 8.0f) {
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::SetCursorPosX(right - sw - edge);
                        static char s_find[64] = "";
                        // ESC clears the model; the widget has to follow or the
                        // box would still show a term that no longer applies
                        if (!Grid::SearchActive() && s_find[0]) s_find[0] = '\0';
                        ImGui::SetNextItemWidth(sw);
                        if (ImGui::InputTextWithHint("##find",
                                Lang::T(Lang::Str::SearchHint), s_find, sizeof(s_find))) {
                            Grid::SetSearch(s_find);
                        }
                    }
                }
            }
            // ★GI77: drop to the equipment column's first slot line. The label
            // keeps its own place at the top; only the grid moves.
            ImGui::SetCursorPosY(itemsLabelH);
            Grid::Draw();
            if (compact) {
                // GOLD strip under the grid; amount clears the trash button
                DrawGoldBar(gridW, bodyH, 26.0f * S);
            }
            DrawTrashButton(gridW, bodyH);   // F2: bottom-right of the grid column
            ImGui::EndChild();

            // ★The accessory drawer hangs off this window's LEFT edge, which
            // is outside it -- a window clips to its own rectangle, so the
            // panel has to be its own. Drawn right after End() with the rect
            // this frame actually used, so it tracks a window drag exactly.
            const ImVec2 mainPos = ImGui::GetWindowPos();
            const ImVec2 mainDim = ImGui::GetWindowSize();

            ImGui::End();

            // ★No doll in compact mode, so no drawer hanging off it — the
            // panel would belong to a column that is not on screen. (The
            // button lives in the doll's own strip, so it goes with it.)
            if (!compact) {
                Equip::DrawDrawer(mainPos, mainDim, Equip::SlotsTopScreen());
            }
        }
    }


    namespace
    {
        // GI52: the drawn icons carry their own rotation, so every pass below
        // submits a QUAD rather than an axis-aligned rect. An unrotated call
        // passes the rect's four corners and is pixel-identical to before.
        void IconPass(ImDrawList* a_dl, ImTextureID a_tex, const ImVec2 a_p[4],
                      std::uint32_t a_col)
        {
            a_dl->AddImageQuad(a_tex, a_p[0], a_p[1], a_p[2], a_p[3],
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f),
                ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f), a_col);
        }
    }

    void DrawPromptRow(const std::vector<PromptBit>& a_bits, bool a_warn, float a_fade)
    {
        DrawPromptRowImpl(a_bits, a_warn, a_fade);
    }

    void SectionLabel(const char* a_text, const ImVec4* a_col)
    {
        if (!a_text) return;
        const auto& sk = Theme::S();
        if (sk.diamondLabels) {   // v10.4: "◇ LABEL" in crimson
            ImGui::TextColored(a_col ? *a_col : sk.sel, "\xE2\x97\x87 %s", a_text);
        } else {
            OutlinedText(a_col ? ImGui::GetColorU32(*a_col) : Theme::Chrome(1.0f), a_text);
        }
    }

    void NoteHoverHint(const char* a_text)
    {
        g_hoverHint = a_text ? a_text : "";
        g_hoverHintFrame = ImGui::GetFrameCount();
    }

    ImFont* BoldFont(const char* a_utf8)
    {
        ImFont* main = g_fontMain ? g_fontMain : ImGui::GetFont();
        if (!g_fontBold || !a_utf8) return main;
        // ★One codepoint it cannot spell sends the WHOLE line back to the main
        // face. Half a word in bold and half in tofu is worse than a word that
        // is merely not bold, and mixing faces mid-string is not something the
        // caller can see coming.
        for (const char* p = a_utf8; *p;) {
            unsigned int cp = 0;
            const int n = ImTextCharFromUtf8(&cp, p, nullptr);
            if (n <= 0) break;
            p += n;
            if (cp == ' ' || cp == '\t' || cp == '\n') continue;
            if (cp > IM_UNICODE_CODEPOINT_MAX) return main;
            if (!g_fontBold->IsGlyphInFont(static_cast<ImWchar>(cp))) return main;
        }
        return g_fontBold;
    }

    void DrawItemIconQuad(ImDrawList* a_dl, void* a_srv, const ImVec2 a_p[4])
    {
        const float g = Theme::IconGain();
        const auto  tex = reinterpret_cast<ImTextureID>(a_srv);
        // <=1: plain darkening tint. >1 (GI57): FILL-LIGHT top-up — a screen
        // blend adds t*icon*(1-dst), so dim midtones get lifted the most and
        // highlights cannot blow out. Live, no texture rebake.
        const auto c = static_cast<std::uint32_t>(
            255.0f * (std::min)(1.0f, g) + 0.5f);
        IconPass(a_dl, tex, a_p, IM_COL32(c, c, c, 255));
        if (g > 1.0f && g_fillBlend) {
            // slider top (1.6) maps to full lift strength
            const float tf = (std::min)(1.0f, (g - 1.0f) / 0.6f);
            const auto t = static_cast<std::uint32_t>(tf * 255.0f + 0.5f);
            a_dl->AddCallback(&FillLightBlendCB, nullptr);
            IconPass(a_dl, tex, a_p, IM_COL32(t, t, t, 255));
            // GI58: one screen pass tops out at +25% midtone, which left the
            // slider's upper range feeling dim (1.0 IS the raw capture — the
            // old additive default merely hid that by inflating highlights).
            // The upper half of the slider stacks a second lift; repeated
            // screen passes converge on a gamma-style curve and still cannot
            // blow out highlights. Same callback scope: consecutive quads
            // blend in submission order, so pass 2 sees pass 1's result.
            if (tf > 0.5f) {
                const auto t2 = static_cast<std::uint32_t>(
                    (tf - 0.5f) * 2.0f * 255.0f + 0.5f);
                IconPass(a_dl, tex, a_p, IM_COL32(t2, t2, t2, 255));
            }
            a_dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            // the reset just re-bound the backend's MaxLOD-0 sampler — every
            // icon drawn after this fill-light pass needs the mips back
            a_dl->AddCallback(&MipSamplerCB, nullptr);
        }
    }

    void DrawItemIcon(ImDrawList* a_dl, void* a_srv, const ImVec2& a_min, const ImVec2& a_max)
    {
        const ImVec2 p[4] = { a_min, ImVec2(a_max.x, a_min.y),
                              a_max, ImVec2(a_min.x, a_max.y) };
        DrawItemIconQuad(a_dl, a_srv, p);
    }

    void DrawItemIconRot(ImDrawList* a_dl, void* a_srv, const ImVec2& a_centre,
                         const ImVec2& a_size, float a_deg)
    {
        const float hx = a_size.x * 0.5f;
        const float hy = a_size.y * 0.5f;
        if (std::fabs(a_deg) < 0.01f) {   // the common case stays a plain rect
            DrawItemIcon(a_dl, a_srv, ImVec2(a_centre.x - hx, a_centre.y - hy),
                                      ImVec2(a_centre.x + hx, a_centre.y + hy));
            return;
        }
        const float r = a_deg * 3.14159265f / 180.0f;
        const float cs = std::cos(r);
        const float sn = std::sin(r);
        const ImVec2 o[4] = { { -hx, -hy }, { hx, -hy }, { hx, hy }, { -hx, hy } };
        ImVec2 p[4];
        for (int i = 0; i < 4; ++i) {
            p[i] = ImVec2(a_centre.x + o[i].x * cs - o[i].y * sn,
                          a_centre.y + o[i].x * sn + o[i].y * cs);
        }
        DrawItemIconQuad(a_dl, a_srv, p);
    }

    void RegisterMenu()
    {
        GridInventoryMenu::RegisterMenu();
    }

    bool TryInitD3D()
    {
        if (g_initialized.load()) return true;

        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data || !data->forwarder || !data->context) {
            SKSE::log::warn("[UI] TryInitD3D: renderer data unavailable");
            return false;
        }

        auto* swapChain = data->renderWindows[0].swapChain;
        if (!swapChain) {
            SKSE::log::warn("[UI] TryInitD3D: swap chain unavailable");
            return false;
        }

        REX::W32::DXGI_SWAP_CHAIN_DESC desc{};
        if (swapChain->GetDesc(&desc) < 0) {
            SKSE::log::error("[UI] TryInitD3D: GetDesc failed");
            return false;
        }

        // ★★BEFORE ImGui exists and long before BuildFonts: the atlas is baked
        // at Scale(), so resolving it after would give every glyph the default
        // size and only the layout the new one. The swapchain is the earliest
        // place the real height is known — io.DisplaySize is still zero here.
        Theme::ResolveScale(static_cast<float>(desc.bufferDesc.height));

        auto* device  = reinterpret_cast<ID3D11Device*>(data->forwarder);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
        auto  hwnd    = reinterpret_cast<HWND>(desc.outputWindow);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        if (!ImGui_ImplWin32_Init(hwnd)) {
            SKSE::log::error("[UI] ImGui_ImplWin32_Init failed");
            return false;
        }

        g_gameWnd = hwnd;
        // keyboard/char input: chained WndProc (see WndProcThunk above)
        if (!g_origWndProc) {
            g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcThunk)));
        }
        if (!ImGui_ImplDX11_Init(device, context)) {
            SKSE::log::error("[UI] ImGui_ImplDX11_Init failed");
            return false;
        }

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigNavMoveSetMousePos = false;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;
        // ★We push exactly ONE authoritative mouse position per frame, at the
        //  end of the queue. Trickling exists to spread a burst of real device
        //  events over several frames, and here it does the opposite of what we
        //  want: the Win32 backend also queues a position (the OS cursor, which
        //  the game parks at screen centre on a pad), and with trickling on,
        //  that stale one can win the frame whenever a button event splits the
        //  queue. Off = last write wins, which is ours.
        io.ConfigInputTrickleEventQueue = false;

        // fonts are baked at the saved UI scale (WinManager may not have
        // loaded yet here — OnShow re-requests a bake if the scale differs)
        BuildFonts();

        ImGui::StyleColorsDark();
        Theme::Apply();
        CreateFillLightBlend(device);
        CreateMipSampler(device);
        CreateSilhouettePS(device);

        // ★The four frame_torn_*.fic sheets that used to load here are gone:
        // the torn frame is drawn (Theme::TornPanel), so there is no texture to
        // pick between and no 9-slice to stretch.

        g_initialized.store(true);
        // ★The windows, side by side. If the swapchain's output window is not
        // the one the OS is sending input to (a proxy DXGI layer, a borderless
        // helper, a second render window), every keystroke goes somewhere else
        // and no amount of care inside the thunk can help. One line, and the
        // question is answered from a report instead of guessed at.
        SKSE::log::info("[UI] ImGui initialized (hwnd={:#x} fg={:#x} active={:#x} "
                        "chainedProc={})",
            reinterpret_cast<std::uintptr_t>(hwnd),
            reinterpret_cast<std::uintptr_t>(GetForegroundWindow()),
            reinterpret_cast<std::uintptr_t>(GetActiveWindow()),
            g_origWndProc != nullptr);
        return true;
    }

    void Open()
    {
        if (!TryInitD3D()) {
            SKSE::log::error("[UI] Open aborted: ImGui not initialized");
            return;
        }
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(GridInventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
    }

    void Close()
    {
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(GridInventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    void OpenInspect(RE::TESBoundObject* a_obj, const std::string& a_key)
    {
        if (!a_obj) return;
        g_inspObj = a_obj;
        g_inspKey = a_key;
        // start where the icon already looks, so the view is continuous with
        // the tile the player pressed C on
        const IconDef d = IconCache::GetSingleton()->ResolveDef(a_obj);
        g_inspRx = g_inspRx0 = d.rx;
        g_inspRy = g_inspRy0 = d.ry;
        g_inspRz = g_inspRz0 = d.rz;
        g_inspZoom = kInspZoomMin;
        g_inspDrag = false;
        g_inspOpenFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
        IconCache::GetSingleton()->SetInspect(a_obj, g_inspRx, g_inspRy, g_inspRz);
    }

    bool IsInspectOpen() { return g_inspObj != nullptr; }

    bool CloseInspect()
    {
        if (!g_inspObj) return false;
        g_inspObj = nullptr;
        g_inspKey.clear();
        g_inspDrag = false;
        IconCache::GetSingleton()->ClearInspect();
        Grid::RefreshDefs();   // tiles go back to their own def orientation
        return true;
    }

    namespace
    {
        // Modal 3D inspect: full-screen dim + the live engine capture drawn
        // large. Drawn LAST so ImGui hover/click blocking makes it modal, and
        // NoteOverlayRect keeps the grid's raw-mouse paths out.
        void DrawInspect()
        {
            if (!g_inspObj) return;
            auto* icons = IconCache::GetSingleton();
            auto& io = ImGui::GetIO();
            const float S = Theme::Scale();
            const auto& sk = Theme::S();

            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(6, 5, 4, 232));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            const bool open = ImGui::Begin("##fablerim_inspect", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoCollapse);
            if (open) {
                NoteOverlayRect();

                // Rotate by dragging — handled MANUALLY rather than with a
                // full-screen InvisibleButton, which would own ActiveId across
                // the whole screen. The overlay is deliberately WIDGET-FREE
                // (a button here fought that same ActiveId), so a plain
                // hover + mouse-down test is all it takes.
                if (!g_inspDrag && io.MouseClicked[0] && ImGui::IsWindowHovered()) {
                    g_inspDrag = true;
                }
                if (g_inspDrag && !io.MouseDown[0]) g_inspDrag = false;
                bool moved = false;
                if (g_inspDrag && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                    constexpr float kDegPerPx = 0.6f;
                    if (io.KeyShift) {
                        g_inspRy += io.MouseDelta.x * kDegPerPx;
                    } else {
                        g_inspRz += io.MouseDelta.x * kDegPerPx;
                        g_inspRx += io.MouseDelta.y * kDegPerPx;
                    }
                    auto wrap = [](float& v) {
                        while (v > 180.0f) v -= 360.0f;
                        while (v < -180.0f) v += 360.0f;
                    };
                    wrap(g_inspRx);
                    wrap(g_inspRy);
                    wrap(g_inspRz);
                    moved = true;
                }
                if (io.MouseWheel != 0.0f) {
                    g_inspZoom = std::clamp(g_inspZoom * (1.0f + io.MouseWheel * 0.12f),
                        kInspZoomMin, kInspZoomMax);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.WantTextInput) {
                    g_inspRx = g_inspRx0;
                    g_inspRy = g_inspRy0;
                    g_inspRz = g_inspRz0;
                    g_inspZoom = kInspZoomMin;   // R returns to the opening shot
                    moved = true;
                }
                // ★The capture is driven by IconCache::PreRender, and this is
                // the ONLY thing that makes it re-shoot. Anything that changes
                // what the model should look like has to set `moved`; zoom does
                // not, because it scales the existing sprite below.
                if (moved) icons->SetInspectRot(g_inspRx, g_inspRy, g_inspRz);

                // the sprite: native pixels, capped so it always fits
                const float boxH = io.DisplaySize.y * 0.62f * g_inspZoom;
                const float boxW = io.DisplaySize.x * 0.62f * g_inspZoom;
                if (const auto* ic = icons->InspectIcon(); ic && ic->srv && ic->w > 0) {
                    const float sc = (std::min)(boxW / static_cast<float>(ic->w),
                        boxH / static_cast<float>(ic->h));
                    const ImVec2 sz(static_cast<float>(ic->w) * sc,
                        static_cast<float>(ic->h) * sc);
                    const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
                    ImGui::GetWindowDrawList()->AddImage(
                        reinterpret_cast<ImTextureID>(ic->srv),
                        ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f),
                        ImVec2(c.x + sz.x * 0.5f, c.y + sz.y * 0.5f));
                } else {
                    const char* wait = Lang::T(Lang::Str::Caching);
                    const ImVec2 tw = ImGui::CalcTextSize(wait);
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2((io.DisplaySize.x - tw.x) * 0.5f,
                               (io.DisplaySize.y - tw.y) * 0.5f),
                        Theme::Col(sk.inkDim, 1.0f), wait);
                }

                // name (top) + control hint / adopt button (bottom)
                auto* dl = ImGui::GetWindowDrawList();
                if (const char* nm = g_inspObj->GetName(); nm && nm[0]) {
                    const ImVec2 nw = ImGui::CalcTextSize(nm);
                    dl->AddText(ImVec2((io.DisplaySize.x - nw.x) * 0.5f, 26.0f * S),
                        Theme::Val(), nm);
                }
                // GI63: the control hint used to be printed HERE, at the same
                // screen-bottom spot the prompt bar now owns -- the two drew on
                // top of each other. One statement of the controls, one place.

                // (no widgets here by design — see the drag comment above:
                // icon rotation is edited in the EDIT panel / IconStudio)
            }
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // C toggles it shut (ESC goes through the CloseTopWindow chain) —
            // never on the opening frame, where the grid's own C is still down
            if (g_inspObj && ImGui::GetFrameCount() != g_inspOpenFrame &&
                ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.WantTextInput) {
                CloseInspect();
            }
        }
    }

    namespace
    {
        // ★★I/ESC close the LAST thing opened, not a fixed priority list. The
        // chain this replaces read top-down — inspect, popups, trash, pouch,
        // settings, EDIT — so opening the SETTINGS and then turning EDIT on
        // closed the settings first, which is not what the player did last.
        //
        // The order is OBSERVED, not reported. Every layer already owns a bool
        // and asking all eight once a frame costs nothing, whereas threading a
        // push/pop through six modules' open paths would miss one sooner or
        // later — and that miss would be silent, showing up only as a window
        // closing out of turn.
        enum class Layer : std::uint8_t {
            kInspect, kTrashConfirm, kLootPopup, kEquipPopup,
            kTrash, kPouch, kRecharge, kSettings, kEdit, kSearch, kCount
        };

        [[nodiscard]] bool LayerOpen(Layer a_l)
        {
            switch (a_l) {
            case Layer::kInspect:      return g_inspObj != nullptr;
            case Layer::kTrashConfirm: return Grid::IsTrashConfirmOpen();
            case Layer::kLootPopup:    return LootBarter::IsPopupOpen();
            case Layer::kEquipPopup:   return Equip::IsPopupOpen();
            case Layer::kTrash:        return Grid::IsTrashOpen();
            case Layer::kPouch:        return Grid::IsPouchOpen();
            case Layer::kRecharge:     return Grid::IsRechargeOpen();
            case Layer::kSettings:     return g_showSettings.load();
            case Layer::kEdit:         return Editor::IsEditMode();
            case Layer::kSearch:       return Grid::SearchActive();
            default:                   return false;
            }
        }

        bool CloseLayer(Layer a_l)
        {
            switch (a_l) {
            case Layer::kInspect:      return CloseInspect();
            case Layer::kTrashConfirm: return Grid::CloseTrashConfirm();
            case Layer::kLootPopup:    return LootBarter::CloseTopPopup();
            case Layer::kEquipPopup:   return Equip::CloseTopPopup();
            case Layer::kTrash:        return Grid::CloseTrash();
            case Layer::kPouch:        return Grid::ClosePouch();
            case Layer::kRecharge:     return Grid::CloseRecharge();
            case Layer::kSettings:
                if (!g_showSettings.load()) return false;
                g_showSettings.store(false);
                return true;
            case Layer::kEdit:
                if (!Editor::IsEditMode()) return false;
                Editor::ToggleEditMode();   // same path as clicking EDIT off
                return true;
            case Layer::kSearch:       return Grid::ClearSearch();
            default: return false;
            }
        }

        std::vector<Layer> g_layerStack;   // oldest first, newest last
    }

    void TrackLayers()
    {
        // whatever was shut by its own button, confirm or X leaves the stack
        std::erase_if(g_layerStack, [](Layer l) { return !LayerOpen(l); });
        // ...and whatever is newly up joins the top. Two layers appearing in
        // the SAME frame is rare and their relative order is arbitrary anyway,
        // so enum order breaks that tie.
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Layer::kCount); ++i) {
            const auto l = static_cast<Layer>(i);
            if (!LayerOpen(l)) continue;
            if (std::find(g_layerStack.begin(), g_layerStack.end(), l) ==
                g_layerStack.end()) {
                g_layerStack.push_back(l);
            }
        }
    }

    void LogHudModes(const char* a_tag)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        const auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hud || !hud->uiMovie) return;
        RE::GFxValue modes;
        if (!hud->uiMovie->GetVariable(&modes,
                "_root.HUDMovieBaseInstance.HUDModes") ||
            !modes.IsArray()) {
            SKSE::log::info("[HUDMODE] {}: no HUDModes array", a_tag);
            return;
        }
        std::string s;
        const auto n = modes.GetArraySize();
        for (std::uint32_t i = 0; i < n; ++i) {
            RE::GFxValue v;
            if (modes.GetElement(i, &v) && v.IsString()) {
                s += v.GetString();
                if (i + 1 < n) s += ' ';
            }
        }
        SKSE::log::info("[HUDMODE] {}: [{}] ({})", a_tag, s, n);
    }

    bool CloseTopWindow()
    {
        // the key can arrive before the next frame observes a layer that a
        // click just opened, so look once more here
        TrackLayers();
        while (!g_layerStack.empty()) {
            const Layer l = g_layerStack.back();
            if (CloseLayer(l)) {
                // a module holding several popups of its own (loot slider under
                // a sale confirm) keeps its PLACE in line until the last one is
                // gone — popping it here would re-append it at the top instead
                if (!LayerOpen(l)) g_layerStack.pop_back();
                return true;
            }
            g_layerStack.pop_back();   // stale entry: try the one beneath
        }
        return false;
    }

    namespace
    {
        // menu-open whoosh: at kShow time the sound was swallowed (transition)
        // — play a few frames later once the menu is actually rendering
        int g_menuOpenSfx = 0;
    }

    void OnShow()
    {
        // PHASE 0 PROBE: the state the menu is opening onto. After a save/load
        // this is the reading that matters -- kPostLoadGame fires before the 3D
        // is back, so the report there cannot see what the body actually built.
        g_menuOpenSfx = 10;   // clear of the open transition (3 was too early)
        // ★Open-transition BURST: for the next few ticks the icon cache may
        // restore a screenful of pak sprites at once instead of 8 per frame.
        // The open is already a covered moment (menu fade), so the batch is
        // invisible where the trickle read as pop-in -- and a CONTAINER
        // session benefits most, since a chest's contents are exactly the
        // sprites the player is least likely to have resident.
        IconCache::GetSingleton()->Burst(4);
        // Re-ask the engine which button carries what: the player may have
        // rebound the controls since the last time the menu was up. Done HERE,
        // on the game thread, so the render thread only ever reads the result.
        ResolvePadLabels();
        // Park anchor = the SAVED main-window centre — set BEFORE the first
        // capture request so no frame is ever exposed.
        {
            const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
            const ImVec2 center(static_cast<float>(sz.width) * 0.5f,
                                static_cast<float>(sz.height) * 0.5f);
            auto* wm = WinManager::GetSingleton();
            wm->Load();
            ItemPreview::GetSingleton()->SetParkPos(
                Theme::S().translucent ? center
                                       : ParkOnScreen(wm->MainCenter(center)));

            // ini display scale arrived after the init-time bake -> rebake
            // once. NOT the text size: that one never goes through the atlas.
            if (std::fabs(Theme::Scale() - g_bakedScale) > 0.005f) {
                g_fontsDirty.store(true);
            }
        }

        // Callback FIRST: it hot-reloads the item defs which the grid and the
        // capture queue key off (building before the reload uses stale defs).
        if (g_onShow) g_onShow();

        // ★B4-1: conditional since the demolition began. A closed-menu count
        // delta raised the flag through its event; the census gate in
        // GridMenu::OnShow covers event-less value drifts (the grindstone).
        // A quiet open keeps the board untouched and every ladder idle.
        Grid::RebuildIfNeeded();

        // B: prefetch EVERYTHING the player carries the moment the menu
        // opens — one up-front caching burst instead of per-scroll/per-bag
        // trickle (bag contents live in the same inventory, so this covers
        // them too). Disk-cached items are skipped by pak index (no GPU).
        if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
            auto* cache = IconCache::GetSingleton();
            for (const auto& [obj, data] : pl->GetInventory()) {
                if (obj && data.first > 0) cache->Prefetch(obj);
            }
        }

        SKSE::log::info("[UI] menu shown ({} icons cached)",
            IconCache::GetSingleton()->CachedCount());
    }

    void OnClose()
    {
        CloseInspect();           // release the pinned inspect model + engine scale
        Editor::OnMenuClosed();   // flush pending edits, drop selection
        // F2: closing the whole menu confirms every parked deletion; flush
        // immediately (same context LootBarter::Reset moves items in)
        if (Grid::CloseTrash()) Grid::ProcessTrashDeletes();
        LootBarter::Reset();      // back to kNormal (loot/barter mode ends)
        Grid::ClearPendingEquips();   // no queued equip outlives the menu
        Equip::OnMenuClosed();        // GI53: nor does a loadout confirm popup
        if (Grid::IsHolding()) Grid::CancelHold();   // never close mid-carry --
                                      // a doll/carrier origin queues its re-wear
        // ★Ring session: EXECUTE the queue instead of abandoning it. "No
        // queued equip outlives the menu" was a claim the queue never
        // honoured -- a lift's unequip could sit here and fire on the NEXT
        // open. Running it now (the same context LootBarter::Reset moves
        // items in) also lands the cancel's re-wear before the menu dies.
        Equip::ProcessPending();
        Grid::NoteInventorySeen();    // GI65: closing the menu IS "I have seen it"
        // ★A search is about THIS visit. Carrying it over means the next open
        // shows a dimmed board for a term the player has long forgotten typing.
        Grid::ClearSearch();
        g_showSettings = false;
        g_textInputOn = false;
        if (ImGui::GetCurrentContext()) {
            ImGui::ClearActiveID();   // drop text-field focus: a stale ActiveId
                                      // keeps WantTextInput true past the close
        }
        WinManager::GetSingleton()->Save();   // window layout persists (F6)
        if (g_onHide) g_onHide();
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().ClearInputKeys();
            // ★★★AND THE MOUSE. ClearInputKeys skips the mouse buttons on
            // purpose -- ImGui keeps ClearInputMouse as a separate call, and
            // this plugin had never made it. Close the menu with the left
            // button DOWN (dragging a title bar and hitting I or Escape, which
            // is not a rare way to close a window) and the release goes to a
            // menu that is no longer relaying, so MouseDown[0] was still true
            // at the next open. IsMouseClicked is an EDGE, and an edge that
            // already happened never comes again: the first click after
            // reopening did nothing at all -- no pickup, no window drag, no
            // click-outside to dismiss. Press and release once and it healed,
            // which is why it never survived being tested.
            ImGui::GetIO().ClearInputMouse();
        }
        // ★The pad's own held state is ours, not ImGui's, and it has the same
        // problem: a button held across the close stays held in the mask.
        g_padRaw.store(0);
        g_padHeld.store(0);
        g_padPrev = 0;
        // ★★GIVE THE CURSOR BACK, unconditionally and last. A cursor left
        // hidden is a game the player cannot click, so this runs on every exit
        // from the menu -- not only the tidy ones.
        SetGameCursorVisible(true);
        SKSE::log::info("[UI] menu hidden");
    }

    // (caching card removed: the full-frame capture restore guarantees the
    // parked model never shows on screen, so translucent skins need no cover;
    // progress still shows via the main window's "Caching N" label)

    namespace
    {
        // overlay rects (x1,y1,x2,y2), double-buffered: the grid draws BEFORE
        // the overlays each frame, so it tests the PREVIOUS frame's rects
        std::vector<ImVec4> g_overlayNow;
        std::vector<ImVec4> g_overlayPrev;
    }

    void NoteOverlayRect()
    {
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        const float m = 14.0f * Theme::Scale();   // torn-frame chrome margin
        g_overlayNow.emplace_back(p.x - m, p.y - m, p.x + s.x + m, p.y + s.y + m);
    }

    bool CursorOwnsWindow(int a_extra)
    {
        // See the header for why both halves are here and why the flag is not
        // optional. Call from INSIDE the window whose claim is being tested.
        return ImGui::IsWindowHovered(static_cast<ImGuiHoveredFlags>(a_extra) |
                                      ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
               !MouseInOverlay();
    }

    bool MouseInOverlay()
    {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        for (const auto& r : g_overlayPrev) {
            if (mp.x >= r.x && mp.y >= r.y && mp.x <= r.z && mp.y <= r.w) {
                return true;
            }
        }
        return false;
    }

    const char* KeyLabel(Act a_act)
    {
        // Mouse/keyboard side is ours to choose (these are hardcoded above in
        // the ImGui translation), so it needs no lookup.
        static constexpr const char* kKeyboard[] = {
            "LMB", "RMB", "R", "F", "C", "Shift", "A", "D", "T",
        };
        const auto i = static_cast<std::size_t>(a_act);
        if (i >= std::size(kKeyboard)) return "";
        // (recharge used to stop at the keyboard here -- it rides LT now and
        // reads off the pad table like everything else)
        if (i >= std::size(g_padLabel)) return kKeyboard[i];
        // Read-only on the render thread: the table is filled on the game
        // thread in OnShow, so nothing here touches ControlMap. An unresolved
        // or unbound action still names the key, so a hint never goes blank.
        if (!g_padActive.load() || !g_padLabelReady) return kKeyboard[i];
        return g_padLabel[i] ? g_padLabel[i] : kKeyboard[i];
    }

    bool WantsGameCursor()
    {
        // Mouse mode always wants the vanilla arrow. On a pad we want it too —
        // unless we have concluded it will not follow and are drawing our own,
        // in which case asking for it just strands a second arrow on screen.
        return !g_padActive.load() || g_padCursorMode != PadCursorMode::kOwn;
    }

    void FeedEngineCursor(RE::ThumbstickEvent* a_event)
    {
        if (!a_event) return;
        if (g_padCursorMode == PadCursorMode::kOwn) return;   // settled: don't poke it
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) return;
        const auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME);
        if (!menu) return;
        auto* handler = static_cast<RE::CursorMenu*>(menu.get())->AsMenuEventHandler();
        if (!handler) return;

        // CanProcess is the engine's own gate. We log its first verdict — a
        // refusal is the single most useful thing to know if this path stays
        // dead — but still offer the event: we are deliberately driving a
        // cursor the engine has concluded nobody is driving.
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            SKSE::log::info("[PAD] CursorMenu handler reached (CanProcess={})",
                            handler->CanProcess(a_event) ? "yes" : "no");
        }
        handler->ProcessThumbstick(a_event);
    }

    void NotePadStick(bool a_right, float a_x, float a_y)
    {
        // Radial dead zone, then a squared response: small deflections stay
        // precise enough to land on one cell, full deflection still crosses
        // the board quickly.
        constexpr float kDead = 0.20f;
        const float mag = std::sqrt(a_x * a_x + a_y * a_y);
        float nx = 0.0f, ny = 0.0f;
        if (mag > kDead) {
            const float t = (mag - kDead) / (1.0f - kDead);
            const float s = t * t / mag;
            nx = a_x * s;
            ny = a_y * s;
        }
        if (a_right) {
            g_padScrollY.store(ny);
        } else {
            g_padMoveX.store(nx);
            g_padMoveY.store(-ny);   // stick +Y is up, screen +Y is down
        }
        if (nx != 0.0f || ny != 0.0f) MarkPadActive(true);
    }

    void NotePadButton(std::uint32_t a_idCode, bool a_pressed)
    {
        using K = RE::BSWin32GamepadDevice::Keys;
        // The triggers are analog axes with no bit in the XInput button mask —
        // give them one above every real bit so they ride the same bookkeeping.
        std::uint32_t bit = a_idCode;
        if (a_idCode == K::kLeftTrigger)  bit = 1u << 16;
        if (a_idCode == K::kRightTrigger) bit = 1u << 17;
        if (bit == 0 || (bit & (bit - 1)) != 0) return;   // single bit only

        const int idx = std::countr_zero(bit);
        if (a_pressed) {
            // Resolve at PRESS time and remember it, so the release clears the
            // same action even if the binding changed in between.
            g_btnAction[idx] = ActionForButton(a_idCode);
            g_padRaw.fetch_or(bit);
            // One line per button, first press only: what the engine called it
            // and what we made of it. Without this the binding is invisible.
            static std::uint32_t s_logged = 0;
            if ((s_logged & bit) == 0) {
                s_logged |= bit;
                auto* cm = RE::ControlMap::GetSingleton();
                using Ctx = RE::UserEvents::INPUT_CONTEXT_ID;
                std::string_view nm{};
                if (cm) {
                    for (const auto ctx : { Ctx::kItemMenu, Ctx::kMenuMode, Ctx::kInventory }) {
                        nm = cm->GetUserEventName(a_idCode, RE::INPUT_DEVICE::kGamepad, ctx);
                        if (!nm.empty()) break;
                    }
                }
                SKSE::log::info("[PAD] button {:#06x} -> user event '{}' -> action {:#x}",
                                a_idCode, nm.empty() ? "(unbound)" : std::string(nm),
                                g_btnAction[idx]);
            }
        } else {
            g_padRaw.fetch_and(~bit);
        }

        // Two buttons can carry the same action (both triggers = split), so the
        // held set is the OR over everything still down — never a single bit
        // cleared by whichever was released first.
        const std::uint32_t raw = g_padRaw.load();
        std::uint32_t actions = 0;
        for (std::uint32_t r = raw; r != 0; r &= (r - 1)) {
            actions |= g_btnAction[std::countr_zero(r)];
        }
        g_padHeld.store(actions);
        MarkPadActive(true);
    }

    void NoteMouseInput()
    {
        // A real mouse event — the only trustworthy "the user grabbed the
        // mouse" signal. Suppression holds until an actual pad event arrives.
        g_padSuppressed.store(true);
        g_padActive.store(false);
    }

    bool IsBookOpen()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->IsMenuOpen(RE::BookMenu::MENU_NAME);
    }

    bool BeginSilhouette(ImDrawList* a_dl)
    {
        if (!a_dl || !g_silPS) return false;
        a_dl->AddCallback(&SilhouetteOnCB, nullptr);
        return true;
    }

    void EndSilhouette(ImDrawList* a_dl)
    {
        if (!a_dl || !g_silPS) return;
        a_dl->AddCallback(&SilhouetteOffCB, nullptr);
    }

    void UseMipSampler(ImDrawList* a_dl)
    {
        if (a_dl) a_dl->AddCallback(&MipSamplerCB, nullptr);
    }

    void SyncDisplaySize()
    {
        // B11(P2): queried per frame — a once-cached size went stale after a
        // borderless/fullscreen switch and desynced from OnShow's park math.
        //
        // ★★★AND IT OVERRULES THE WIN32 BACKEND, which is the whole point.
        // ImGui_ImplWin32_NewFrame fills DisplaySize from the WINDOW's client
        // rect, and the window is not the picture: SSE Display Tweaks'
        // borderless upscale renders 1920x1080 into a 3840x2160 window, so the
        // backend's answer was twice the render target. Reported against the
        // quick wheel -- drawn at double size and pushed off the bottom right
        // -- because the wheel builds its OWN ImGui frame and was the one that
        // never overruled it. Hence a function: two frames, one answer.
        // ★Must run AFTER the backend's NewFrame and BEFORE ImGui::NewFrame.
        // Earlier and the backend overwrites it; later and the frame has
        // already been laid out against the wrong size.
        const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
        auto& io = ImGui::GetIO();
        io.DisplaySize.x = static_cast<float>(screenSize.width);
        io.DisplaySize.y = static_cast<float>(screenSize.height);
    }

    bool IsConsoleOpen()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->IsMenuOpen(RE::Console::MENU_NAME);
    }

    // ★Ships as 0, which is not a key. See UIRoot.h.
    // Written from the ini parse, read from the raw input thread.
    std::atomic<int> g_vanillaKey{ 0 };
    void SetVanillaKey(int a_scancode) { g_vanillaKey.store(a_scancode); }
    int  VanillaKey() { return g_vanillaKey.load(); }

    void Render()
    {
        if (!g_initialized.load()) return;
        // The book the player just right-clicked is a real Scaleform menu
        // UNDER our overlay. Skip the whole frame (not just the windows) so
        // nothing of ours is drawn and no ImGui state is touched meanwhile.
        if (IsBookOpen()) {
            if (!g_bookWasOpen) {
                g_bookWasOpen = true;
                // ★The click that opened the book never gets its release here
                // (our input relay stands down too), so queue one. Without it
                // ImGui resumes with the button still down and the first
                // frame back cancels a carry / fires a drop by itself.
                auto& io = ImGui::GetIO();
                io.AddMouseButtonEvent(0, false);
                io.AddMouseButtonEvent(1, false);
                // ★★★AND GIVE THE CURSOR BACK WHILE THE BOOK HAS THE SCREEN.
                // MouseHandler hides it every frame we run, and the only place
                // that ever showed it again was OnClose -- but this return
                // comes BEFORE MouseHandler, so the hide simply stood. Reading
                // a book or a note out of the grid meant reading it with no
                // mouse pointer, and the same hole belongs to any vanilla menu
                // that opens over us.
                SetGameCursorVisible(true);
            }
            // (1.5.x) the shelf page still answers E -- see the input sink --
            // but draws no chip of ours: a drawn prompt over the engine's
            // page read as foreign (user call), and the HUD channel is held
            // back while a menu is up. The gesture is the book's own, and
            // regulars know it from the world's pages.
            return;
        }
        if (g_bookWasOpen) {
            // ★back to us: MouseHandler takes the cursor again from here on
            g_bookWasOpen = false;
            // ★(1.5.x) the page just closed: if E flagged a shelf take while
            // it was up, this is where the transfer starts (render thread,
            // like every other request)
            LootBarter::ProcessShelfBookTake();
        }

        // ★★The console just came up. Keys stop reaching us from this frame on
        // (GridMenu::ProcessScaleformEvent), so anything HELD when it opened
        // would never see its release and would read as stuck. Dropping the
        // text focus too is what actually stops the typing: a focused ImGui
        // field keeps its caret and would resume mid-word when the console
        // closes, on a term the player was not editing.
        {
            static bool s_consoleWas = false;
            const bool consoleNow = IsConsoleOpen();
            // ★Both edges. Opening is the case above; CLOSING matters too now
            // that the thunk blocks key-downs outright — the release of
            // whatever was held as the console shut is the one event that DOES
            // get through, and ImGui would take it for a key it never saw
            // pressed.
            if (consoleNow != s_consoleWas) {
                ImGui::GetIO().ClearInputKeys();
                ImGui::ClearActiveID();
                g_textInputOn = false;
            }
            s_consoleWas = consoleNow;
        }

        // rebake outside the frame; the DX11 backend recreates the font
        // texture inside NewFrame after InvalidateDeviceObjects
        if (g_fontsDirty.exchange(false)) BuildFonts();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        auto& io = ImGui::GetIO();
        SyncDisplaySize();
        // ★★H′: THE text-size setting, applied to every string ImGui sizes.
        //
        // 1.92 rounds this product to whole pixels and then rasterises the
        // glyphs at that size on demand (ImFont::GetFontBaked), so the text is
        // crisp at any setting and a drag only ever asks for the dozen-odd
        // integer sizes the range contains. There is no atlas rebuild here and
        // no bitmap blit -- the old comment about scaled hangul smearing
        // belongs to the fixed-size atlas this predates.
        //
        // ★io.FontGlobalScale is the pre-1.92 spelling of this and is left at
        // 1: imgui asserts if both are set, and only one of them should ever
        // be the answer to "how big is the text".
        // ★★The OTHER half of the setting is Theme::SnapPx, which carries it
        // for the strings we size ourselves. Both are live, so they move
        // together -- when only one of them moved, the panel drew itself at
        // two sizes at once and the strings looked doubled.
        ImGui::GetStyle().FontScaleMain = Theme::FontScale();
        io.FontGlobalScale = 1.0f;
        // the atlas still carries the DISPLAY scale, so a display change (and
        // a language pack's glyph ranges) still asks for a rebuild
        if (std::fabs(Theme::Scale() - g_bakedScale) > 0.005f) {
            g_fontsDirty.store(true);
        }

        MouseHandler();
        ScrollHandler();

        // NOTE: ControlMap::AllowTextInput is deliberately NOT used. Chars
        // arrive via the chained WndProc (engine machinery not needed), and
        // hotkey suppression is handled by the user-event swallow + the
        // menu-open intercept. Touching the engine counter broke controls:
        // the engine pairs its OWN release on menu close, so our balanced
        // grant/release still went NET -1 per typing session (log-proven,
        // textEntryCount -1 -> -2) and movement/attack died.
        g_textInputOn = io.WantTextInput;

        // ★The polled CHARACTER road. Inert until it has proven that printable
        // keys are pressed and no WM_CHAR follows -- see PollTypedCharacters.
        PollTypedCharacters(io);

        // ---- input road health (OBSERVATION ONLY -- see the note above) ------
        // A text field focused for two seconds with ZERO characters delivered is
        // not a slow typist; it is a dead road, and this line is the only way
        // that fact reaches a report from a machine we cannot reproduce on.
        {
            static int      s_wantFrames = 0;
            static unsigned s_charsAtFocus = 0;
            static bool     s_said = false;
            if (io.WantTextInput) {
                if (s_wantFrames == 0) {
                    s_charsAtFocus = g_wmCharSeen.load(std::memory_order_relaxed);
                }
                ++s_wantFrames;
                if (s_wantFrames == 120 && !s_said &&
                    g_wmCharSeen.load(std::memory_order_relaxed) == s_charsAtFocus) {
                    s_said = true;
                    // ★`msgs` is the liveness signal: it counts EVERY message
                    // through our thunk, so 0 means the road itself is gone,
                    // while a rising number with no chars means the road is fine
                    // and the characters are being taken somewhere upstream.
                    // Two very different faults, one line apart.
                    SKSE::log::error(
                        "[UI] input: a text field has been focused for 120 frames "
                        "and NO WM_CHAR arrived (chars {} keys {} raw {} msgs {}, "
                        "hwnd {:#x} fg {:#x} polled {}).",
                        g_wmCharSeen.load(std::memory_order_relaxed),
                        g_wmKeySeen.load(std::memory_order_relaxed),
                        g_wmKeyRaw.load(std::memory_order_relaxed),
                        g_thunkMsgs.load(std::memory_order_relaxed),
                        reinterpret_cast<std::uintptr_t>(g_gameWnd),
                        reinterpret_cast<std::uintptr_t>(GetForegroundWindow()),
                        g_kbFallback);
                }
            } else {
                s_wantFrames = 0;
            }
        }

        if (g_menuOpenSfx > 0 && --g_menuOpenSfx == 0) {
            // UIInventoryOpen resolved but stayed inaudible — the Tab-menu
            // blade whoosh is the clearly audible vanilla menu sound
            Sfx::MenuOpen();
        }

        ImGui::NewFrame();
        // ★First command of the frame: swap the backend's MaxLOD-0 sampler
        // for the mip-enabled one (see CreateMipSampler). The background list
        // renders before every window, and the backend never re-binds its
        // sampler mid-frame except through ResetRenderState (handled at its
        // one call site) — so this single callback covers the whole UI.
        ImGui::GetBackgroundDrawList()->AddCallback(&MipSamplerCB, nullptr);
        g_overlayPrev.swap(g_overlayNow);
        g_overlayNow.clear();
        // hover-sound edge detection re-arms once nothing is hovered, so
        // leaving and re-entering the same widget ticks again
        if (!ImGui::IsAnyItemHovered()) Sfx::HoverReset();
        WinManager::GetSingleton()->SetDragLock(Grid::IsHolding());   // F1
        DrawMainWindow();
        Grid::DrawBagWindows();   // one managed window per open bag (E2/E5)
        LootBarter::DrawWindows();  // container/merchant partner window (loot/barter)
        DrawSettingsWindow();     // ⚙ popup (scale / skin / language)
        Equip::DrawLoadoutWindows();   // L2: loadout +buy / delete confirm (top level)
        Grid::DrawPouchWindow();       // G2: coin-pouch withdraw (top level)
        Grid::DrawRechargeWindow();    // (1.3.1) soul-gem recharge (top level)
        LootBarter::DrawShelfBag();    // (1.3.1) opened shelf-bag windows (top level)
        LootBarter::DrawShelfPouch();  // (1.3.2a) shelf-pouch withdraw (top level)
        Grid::DrawTrashConfirm();      // F2: favorite-intake confirm (top level)
        LootBarter::DrawSlider();      // loot/barter quantity slider (top level)
        LootBarter::DrawConfirm();     // favorite-sale confirm popup (top level)
        Editor::DrawPanel();      // B-6 EDIT panel (edit mode only)
        DrawInspect();            // C key: modal 3D inspect (drawn last = modal)
        Grid::FinishFrame();      // carry input + deferred rebuilds
        // GI63: LAST, so it sees this frame's hover -- the tooltips that record
        // it are drawn above. Being on the foreground list, drawing late costs
        // it no z-order: it still sits over every window.
        DrawPromptBar();
        WinManager::GetSingleton()->Update();   // drag / magnet / dock / clamp
        // ★AFTER every window drew, so a layer a click opened THIS frame is
        // already up when we look. Recording the order is the whole reason
        // I/ESC can close things in the order they were opened.
        TrackLayers();
        // ★★LAST THING DRAWN. The engine's own arrow is hidden while we are up
        // (SetGameCursorVisible), so this IS the pointer -- for mouse and pad
        // alike. Drawn after every window and after the prompt bar, on the
        // foreground list, so nothing can end up on top of it.
        DrawPointer();
        ImGui::Render();
        // ★★The draw-data outline pass is GONE, and it was the answer to a
        // week of "why do these two strings look different".
        //
        // Two outline systems were running at once. Chrome text draws its own
        // (Theme::TextOutlined). The pass then walked the finished frame and
        // outlined every glyph it recognised — including the black copies the
        // first system had just laid down, so a label ended up with an outline
        // ON its outline. Measured: our own coat is 0.50, a label came out at
        // 0.87, which is three coats.
        //
        // ★It did NOT touch the window title, and that is why the title looked
        // right while everything else looked heavy. The title draws through
        // AddText(font, SIZE, ...) — an explicit size lands on a different
        // baked font and a different draw command, which the pass's texture
        // filter then skipped. One string opted out by accident.
        //
        // Nothing needs the pass any more: every string that wants an edge
        // asks for one directly.
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    void Tick()
    {
        // ★B-3: the menu-context snapshot the DeltaWatch event handlers stamp
        // -- refreshed HERE because this tick runs on the main thread in both
        // worlds (the update hook unpaused, AdvanceMovie paused).
        DeltaWatch::RefreshMenuSnapshot();
        Grid::ProcessBookRead();   // raise the Book Menu OUTSIDE the render pass
        Grid::ProcessFavorites();  // GI32: favourites, same reason
        Grid::ProcessRecharge();   // (1.3.1) soul-gem recharge, same reason
        Equip::ProcessPending();   // equip/unequip OUTSIDE the render pass
        Loadout::ProcessPending();  // L1: deferred loadout tab switch
        // ★After the switch, not before: switching changes what is worn, and
        // the costume dresses whatever is worn. Coalesced -- a full set change
        // fires many equip events and DoReset3D rebuilds the whole actor.
        Costume::Tick();
        // Second ring: notices when the ring has left the inventory behind our
        // back (sold, dropped, taken by a script) and stands the carrier down.
        DualRing::Tick();
        LootBarter::ProcessTransfers();   // loot take/store OUTSIDE the render pass
        Grid::ProcessTrashDeletes();      // F2: confirmed deletions (engine RemoveItem)
        Grid::CapacityTick();       // W1+W2: weight bypass / space overload
        GoldCoins::Tick();          // G1: mirror the gold ledger into coins
        ItemPreview::GetSingleton()->Tick();

        // free the retired inspect texture (ClearInspect can run mid-frame,
        // where this frame's draw list still references it)
        IconCache::GetSingleton()->ProcessDeferredRelease();

        // ICON CACHE reset (settings): outside the ImGui frame — this frame's
        // draw list no longer references the SRVs being released. The next
        // draw re-queues every visible item for a fresh engine capture.
        if (g_iconsMergePreset.exchange(false)) {   // GI47: bundled icons
            IconCache::GetSingleton()->MergePak(g_presetMergePak.c_str());
        }
        if (g_iconsReset.exchange(false)) {
            IconCache::GetSingleton()->ResetDiskCache();
            Grid::RequestRebuild();
        }
        if (g_flatReload.exchange(false)) {
            // Drawn icons are looked up once per key and negative-cached, so a
            // file edited while the game runs is otherwise invisible until a
            // restart — which is the difference between "author a PNG" being a
            // loop and being a chore.
            Fallback::ReloadAssets();
            // ★The wheel keeps its OWN copies of the same drawings, so a reload
            // that only emptied the grid's cache left the two surfaces showing
            // different pictures of the same file.
            Wheeler::ReloadMedallions();
            // ★The ink skins' paper is drawn art too, and it caches its own
            // failures -- so a sheet dropped in after launch stays invisible
            // until this runs. Same reason the wheel's medallions are here.
            Theme::ReloadInkArt();
            Grid::RequestRebuild();
        }
        // ★★OUTSIDE the ImGui frame, which is the whole reason it lives here
        // and not in the draw: trimming releases SRVs, and a draw list still
        // holding one is the crash ResetDiskCache's comment warns about. Also
        // resets the per-frame refill allowance -- one place, so the two
        // cannot drift.
        IconCache::GetSingleton()->TrimToBudget();
    }

    bool IsTextInputActive()
    {
        return g_textInputOn;
    }

    void AddScrollEvent(float a_x, float a_y)
    {
        const float sx = a_x * kScrollMultiplier;
        const float sy = a_y * kScrollMultiplier;

        // Immediately stop if direction changes
        if (g_scrollEnergy.x * sx < 0.0f) g_scrollEnergy.x = 0.0f;
        if (g_scrollEnergy.y * sy < 0.0f) g_scrollEnergy.y = 0.0f;

        g_scrollEnergy.x += sx;
        g_scrollEnergy.y += sy;
    }

    void SetVisibilityCallbacks(std::function<void()> a_onShow, std::function<void()> a_onHide)
    {
        g_onShow = std::move(a_onShow);
        g_onHide = std::move(a_onHide);
    }
}
