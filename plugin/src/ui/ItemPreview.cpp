#include "ui/ItemPreview.h"
#include "game/Inv3D.h"

#include <RE/B/BSLight.h>
#include <RE/N/NiLight.h>
#include <RE/U/UI3DSceneManager.h>

#include <d3d11_1.h>

#include <algorithm>
#include <array>

// Ported from ModExplorerMenu (Modex) by patchulidev — Item3DPreview.cpp.
// https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
// Changes vs upstream: CommonLibSSE-NG (main) accessors, REL wrappers for
// Begin3D/End3D/Load/Unload (FUI::Inv3D), theme lookups replaced by constants.

namespace FUI
{
    namespace
    {
        // End3D walks loadedModels and derefs each entry's spModel — an entry
        // whose async load has not landed yet (null spModel) or a still-queued
        // load task makes that a null deref inside the engine (observed CTD
        // during icon-editing capture churn). The scene must never be torn
        // down while this returns true.
        bool LoadInFlight(RE::Inventory3DManager* a_mgr)
        {
            auto& rt = a_mgr->GetRuntimeData();
            if (rt.loadTask) return true;
            for (auto& lm : rt.loadedModels) {
                if (!lm.spModel) return true;
            }
            return false;
        }

        // ★★A NON-NULL spModel IS NOT A FINISHED ONE, and LoadInFlight cannot
        // tell the difference. It covers the hole it can see: an entry whose
        // pointer has not arrived. A crash on 2026-09-03 (SE 1.5.97, Community
        // Shaders + PGPatcher, a freshly generated PBR weapon) came through
        // that guard INTACT -- every spModel was non-null, so the teardown was
        // allowed, and End3D still read [rcx] with rcx = 0 one level deeper,
        // inside the model. The engine hands out the node before every pass is
        // attached to it and nothing in the entry says so.
        //
        // worldBound.radius is the one readiness signal already trusted
        // elsewhere: the capture gates refuse a model until it is positive
        // (see IconCache::CheckPendingGates). A zero radius here means the
        // geometry has not landed, so the entry is still being built and End3D
        // must not walk it yet.
        //
        // ★This is a GUARD, NOT A DIAGNOSIS. The crash log names no field, the
        // reporter's runtime is not reproducible here, and the shipped exe is
        // DRM-wrapped so the engine side cannot be read. It catches the
        // half-built case IF the null is geometry-related; it proves nothing
        // about the crash it was written for.
        //
        // ★Asked ONLY by the teardown, which is bounded: 300 reposts and then
        // it skips End3D entirely and lets the next open/close pair it. Asking
        // the same question in ResetScene would be unbounded -- a model that
        // never gains a radius would stall the capture queue for the whole
        // session with nothing to break the tie.
        bool SceneModelIncomplete(RE::Inventory3DManager* a_mgr)
        {
            for (auto& lm : a_mgr->GetRuntimeData().loadedModels) {
                if (lm.spModel && lm.spModel->worldBound.radius <= 0.0f) {
                    return true;
                }
            }
            return false;
        }

        // ★★★GI96: THE ENTRY CARRIES FOUR POINTERS AND THE GUARDS CHECKED TWO.
        //
        // LoadInFlight covers spModel, SceneModelIncomplete covers the geometry
        // hanging off it. Neither looks at the two FORM pointers in the same
        // entry, and End3D walks those: the engine loop that crashed advances
        // by 0x20, which is exactly sizeof(LoadedInventoryModel), and the
        // per-entry helper it calls read [rcx] with rcx = 0.
        //
        //   ItemPreview.cpp:705 TeardownWhenIdle
        //     -> 51756 End3D +0x77  (add rbx, 0x20 — the per-entry loop)
        //       -> 51774 +0xA8      (mov rax, [rcx]   rcx = 0)
        //   (crash 2026-09-13, 1.6.3, own machine, 628-tile first open)
        //
        // BOTH existing guards passed that teardown intact, so the null was
        // neither spModel nor its radius. itemBase and modelObj are what is
        // left in the entry, and neither is ever legitimately null for
        // anything WE load: Inv3D::Load is handed a TESBoundObject and the
        // engine fills both from it, which is also why FindCurrentModel is
        // allowed to compare against either one.
        //
        // ★Still a GUARD, NOT A DIAGNOSIS -- the same honesty the note above
        // keeps. If this crash is reported again with these checks in place,
        // the null is somewhere this side cannot see, and the answer then is to
        // stop calling End3D at all rather than to invent a fifth test.
        const char* SceneEntryHalfBuilt(RE::Inventory3DManager* a_mgr)
        {
            for (auto& lm : a_mgr->GetRuntimeData().loadedModels) {
                if (!lm.itemBase) return "an entry has no itemBase";
                if (!lm.modelObj) return "an entry has no modelObj";
            }
            return nullptr;
        }

        // ★The one question every End3D road has to ask, in one place so the
        // roads cannot drift apart. nullptr = safe to tear the scene down;
        // anything else is the reason it is not, ready to be logged.
        const char* SceneUnsafeToTearDown(RE::Inventory3DManager* a_mgr)
        {
            if (LoadInFlight(a_mgr))         return "a load is still in flight";
            if (SceneModelIncomplete(a_mgr)) return "an entry has no geometry yet";
            return SceneEntryHalfBuilt(a_mgr);
        }

        // ★★1.0.5 — the capture rig, measured from the shipped scene:
        //   item (-12.4,-500,-26.25)   lamp (100,-350,100)   |d| = 226
        // Read as spherical about the item, with the camera at the origin
        // looking down -Y, that lamp sits 37 degrees to the screen LEFT and 34
        // degrees up. (Screen left is +X here: mirroring X in the experiment
        // moved the highlight to the right.) Angles are the interface because
        // they are what a person can reason about; the distance stays fixed
        // because moving the lamp closer only changes brightness, which the
        // ICON LIGHT slider already owns.
        //
        // ★★These two are the SHIPPED rig — the origin the offsets are measured
        // from, not the angle in use. The caller hands in ONE offset that
        // already sums the global setting and the item def's own (see
        // IconCache::CaptureLightFor), so that a changed GLOBAL reaches
        // SetLightOffset as a changed value and un-settles the park exactly the
        // way a changed item does. Reading the global here instead would leave
        // the rig moving while ItemPreview still believed nothing had changed.
        constexpr float kBaseAzDeg = -37.0f;   // - = screen left
        constexpr float kBaseElDeg =  34.0f;
        constexpr float kLightDist = 226.0f;
        constexpr float kBaseRadius = 400.0f;  // the shipped lamp's reach
        constexpr float kDeg2Rad   = 3.14159265f / 180.0f;

        struct LightProbe
        {
            RE::NiPoint3 menuPos{};
            RE::NiPoint3 bsPos{};
            RE::NiPoint3 niWorld{};
            RE::NiPoint3 niLocal{};
            float        menuRadius = 0.0f;
            RE::NiPoint3 niRadius{};
            bool         held = false;
        };
        LightProbe g_probe;

        // ★Diagnostic state for the capture lamp, beside the probe so the
        // restore path clears both together -- a deliberate put-back must not
        // read as the engine taking the lamp off us.
        RE::NiPoint3 g_lampWrote{};
        bool         g_lampHasWrote = false;

        // the one MenuLight that is actually attached to the scene (bsLight
        // non-null) — the survey showed exactly one, on the inventory scheme.
        // ★The slot INDEX used to be recorded here, in case the engine ever
        // attached a different MenuLight on a scheme rebuild. It never did:
        // the 1.0.5 capture-light investigation ran the comparison to its end
        // and the answer held every time, so the index and the two loggers
        // that read it are gone.
        [[nodiscard]] RE::MenuLight* AttachedMenuLight()
        {
            auto* sm = RE::UI3DSceneManager::GetSingleton();
            if (!sm) return nullptr;
            for (RE::MenuLight* L : sm->menuLights) {
                if (L && L->light) return L;
            }
            return nullptr;
        }

        // ★Three places hold this position and we do not know which one the
        // renderer consumes: MenuLight (the engine's settings copy), BSLight
        // (the scene registration) and NiLight (the node itself). The mirror
        // experiment wrote all three and the capture followed, so all three
        // keep being written — narrowing it down would buy nothing and risks
        // finding the wrong one on a different runtime.
        void WriteLightWorld(const RE::NiPoint3& a_pos, float a_radius)
        {
            RE::MenuLight* L = AttachedMenuLight();
            if (!L) {
                // ★★THIS USED TO RETURN IN SILENCE, and that is how a whole
                // precache came out at half brightness with nothing in the log
                // to say why: with no lamp to move, every item is captured
                // under the default scheme -- and m_parkTicks counts up all
                // the same, so the capture is accepted exactly as if the rig
                // were in place. A failure that leaves no trace is one nobody
                // can report.
                static bool s_warned = false;
                if (!s_warned) {
                    s_warned = true;
                    SKSE::log::warn("[LIGHT] no attached MenuLight -- captures run "
                                    "under the default scheme (icons come out dark)");
                }
                return;
            }
            // ★Did the last write survive the frame? The engine re-asserts the
            // menu light scheme on its own schedule, and a lamp put back between
            // our write and the render is indistinguishable from one that was
            // never moved -- except right here, before we overwrite it again.
            if (g_lampHasWrote) {
                const RE::NiPoint3& cur = L->translate;
                if (std::fabs(cur.x - g_lampWrote.x) > 0.5f ||
                    std::fabs(cur.y - g_lampWrote.y) > 0.5f ||
                    std::fabs(cur.z - g_lampWrote.z) > 0.5f) {
                    static bool s_revert = false;
                    if (!s_revert) {
                        s_revert = true;
                        SKSE::log::warn("[LIGHT] lamp put back by the engine: found "
                            "{:.0f},{:.0f},{:.0f} where we left {:.0f},{:.0f},{:.0f}",
                            cur.x, cur.y, cur.z,
                            g_lampWrote.x, g_lampWrote.y, g_lampWrote.z);
                    }
                }
            }
            if (!g_probe.held) {   // first write of this session: remember it all
                g_probe.menuPos = L->translate;
                g_probe.menuRadius = L->radius;
                if (RE::BSLight* b = L->light.get()) {
                    g_probe.bsPos = b->worldTranslate;
                    if (RE::NiLight* n = b->light.get()) {
                        g_probe.niWorld = n->world.translate;
                        g_probe.niLocal = n->local.translate;
                        g_probe.niRadius = n->GetLightRuntimeData().radius;
                    }
                }
                g_probe.held = true;
                SKSE::log::info("[LIGHT] lamp {:.0f},{:.0f},{:.0f} r={:.0f}"
                    " -> {:.0f},{:.0f},{:.0f} r={:.0f}",
                    g_probe.menuPos.x, g_probe.menuPos.y, g_probe.menuPos.z,
                    g_probe.menuRadius, a_pos.x, a_pos.y, a_pos.z, a_radius);
                // ★★WHAT WE DO NOT WRITE. Position and radius are ours; the
                // COLOUR and the FADE belong to whatever scheme the engine had
                // loaded, and a lamp at half fade is half the light with every
                // other thing about the capture unchanged -- which is the exact
                // shape of the icons-came-out-dark report (uniform across R/G/B,
                // same silhouette, same framing).
                // ★The lamp COUNT too: only the first attached one is moved, so
                // a scene lit by two and a scene lit by one differ by a stop.
                int lamps = 0;
                if (auto* sm = RE::UI3DSceneManager::GetSingleton()) {
                    for (RE::MenuLight* M : sm->menuLights) {
                        if (M && M->light) ++lamps;
                    }
                }
                if (RE::BSLight* b0 = L->light.get()) {
                    if (RE::NiLight* n0 = b0->light.get()) {
                        const auto& d = n0->GetLightRuntimeData();
                        SKSE::log::info("[LIGHT] diffuse {:.2f},{:.2f},{:.2f}"
                            "  ambient {:.2f},{:.2f},{:.2f}  fade {:.2f}"
                            "  niradius {:.0f}  attached lamps {}",
                            d.diffuse.red, d.diffuse.green, d.diffuse.blue,
                            d.ambient.red, d.ambient.green, d.ambient.blue,
                            d.fade, d.radius.x, lamps);
                    }
                }
            }
            L->translate = a_pos;
            L->radius = a_radius;
            if (RE::BSLight* bl = L->light.get()) {
                bl->worldTranslate = a_pos;
                if (RE::NiLight* nl = bl->light.get()) {
                    nl->world.translate = a_pos;
                    nl->local.translate = a_pos;
                    // NiLight keeps radius as a vector; the shipped value only
                    // ever used .x, so scale the whole thing off that
                    nl->GetLightRuntimeData().radius =
                        RE::NiPoint3{ a_radius, a_radius, a_radius };
                }
            }
            g_lampWrote = a_pos;
            g_lampHasWrote = true;
        }

        // item-relative spherical -> world, using the engine's own item origin
        // so the rig follows if the parked position ever moves.
        // ★A model-size-relative rig was tried and REVERTED: the guess was that
        // big items outgrow the lamp's 400 reach, but measured bound radii are
        // 4~11 (armour 9, axe 11, satchel 4) against that 400. The model is
        // tiny compared to the sphere, not larger than it, so the scaling
        // clamped to 1.00 on every single item and changed nothing. Whatever
        // makes armour ignore its angle, it is not the light's reach.
        // ★★★THE CAPTURE INHERITS THE WORLD'S SKY, AND IT CANNOT BE STOPPED
        // FROM HERE. Skyrim lights a menu item with the menu lamp AND the
        // scene's directional ambient. That ambient is the sky where the
        // PLAYER stands: measured in game, its level term reads 0.85 outdoors
        // at noon against 0.074 in a dark interior, and icons captured under
        // the two differ by 1.8x. Precache in a cave and the whole set comes
        // out dark; the shipped pak was made outdoors, so an icon captured
        // indoors later does not match the ones beside it.
        //
        // Overriding BSShaderManager::State::directionalAmbientTransform DOES
        // fix the icons -- measured, 1.00x against the shipped pak from a dark
        // interior. It was removed anyway, because three things are all true
        // at once and together they leave no way to do it invisibly:
        //   1. the ambient is ONE GLOBAL. ShadowSceneNode -- the per-scene
        //      object -- carries lights, fog and a portal graph, but no
        //      ambient, so a separate scene cannot have a separate sky.
        //   2. the WORLD RENDERS BEFORE THE CAPTURE (Tick -> world -> 
        //      PostDisplay), so anything written early enough for the capture
        //      lights the world the player is looking at.
        //   3. writing it LATE, on the line before inv->Render(), reaches
        //      nothing: the frame's shader constants are already uploaded.
        //      Tried and measured -- the icons stayed dark.
        // Gating the override on "only while the icon queue is busy" was
        // tried too; with a cold cache the queue is busy for as long as the
        // inventory is open, so the world simply brightened on open.
        //
        // WHAT TO DO INSTEAD: precache OUTDOORS IN DAYLIGHT. That is what the
        // shipped pak was made under, and it is why the shipped icons are
        // consistent. The one lever that is ours alone is the menu LAMP
        // (per-scene, no world effect) -- driving it hard enough to swamp the
        // ambient would even out the level, but it is a directional light
        // standing in for fill, so the shading hardens and it needs tuning
        // rounds in game. Left undone deliberately.
        void PlaceLight(float a_azOff, float a_elOff)
        {
            const float az = (kBaseAzDeg + a_azOff) * kDeg2Rad;
            const float el = (kBaseElDeg + a_elOff) * kDeg2Rad;
            const float rh = kLightDist * std::cos(el);
            RE::NiPoint3 base{};
            if (auto* mgr = RE::Inventory3DManager::GetSingleton()) base = mgr->itemPos;
            WriteLightWorld(RE::NiPoint3{
                base.x - std::sin(az) * rh,
                base.y + std::cos(az) * rh,
                base.z + std::sin(el) * kLightDist },
                kBaseRadius);
        }

        void RestoreCaptureLight()
        {
            if (!g_probe.held) return;
            g_probe.held = false;
            g_lampHasWrote = false;   // our own put-back, not the engine's
            // ★Re-find rather than keep a stored pointer: the scene may have
            // been rebuilt between move and restore, and writing through a
            // stale MenuLight* would be a write into freed memory.
            RE::MenuLight* L = AttachedMenuLight();
            if (!L) {
                SKSE::log::warn("[LIGHT]no attached light at restore — "
                                "the engine re-applies the scheme on next open");
                g_probe = {};
                return;
            }
            L->translate = g_probe.menuPos;
            L->radius = g_probe.menuRadius;
            if (RE::BSLight* bl = L->light.get()) {
                bl->worldTranslate = g_probe.bsPos;
                if (RE::NiLight* nl = bl->light.get()) {
                    nl->world.translate = g_probe.niWorld;
                    nl->local.translate = g_probe.niLocal;
                    nl->GetLightRuntimeData().radius = g_probe.niRadius;
                }
            }
            g_probe = {};
            SKSE::log::info("[LIGHT]light restored");
        }
    }


#ifdef GI_CAPTURE_DIAG
    // ★DIAGNOSTIC BUILD ONLY. Reads the capture rect back to the CPU, which
    // stalls the render thread -- never ship this enabled.
    //
    // Reports what the reported "speckled icon" needs to be told apart:
    //   pure%   how much of the rect is EXACTLY the magenta we painted
    //   keyed%  how much would pass the chroma key (R>200 B>200 G<60)
    // Called once before the model is drawn and once after. If pure% is already
    // below 100 in "after-clear", the rect was polluted before the model existed
    // and the alpha pipeline never had a chance -- that is the upscaler.
    static void ProbeRect(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src,
                          const D3D11_BOX& a_box, int a_w, int a_h, const char* a_when)
    {
        static int s_probes = 0;
        if (s_probes >= 8) return;   // a handful of icons, then silence
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        auto* dev = data ? reinterpret_cast<ID3D11Device*>(data->forwarder) : nullptr;
        if (!dev) return;

        D3D11_TEXTURE2D_DESC sd = {};
        a_src->GetDesc(&sd);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(a_w);
        td.Height = static_cast<UINT>(a_h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = sd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return;
        a_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, a_src, 0, &a_box);

        D3D11_MAPPED_SUBRESOURCE map = {};
        if (SUCCEEDED(a_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            // ★a0/a255/aMid answer the one question that decides the fix for
            // the purple-alpha report: did the engine leave us usable alpha?
            //   aMid > 0            -> real alpha survives; key it instead
            //   a0 high, aMid 0     -> alpha is binary; still better than chroma
            //   a255 = 100%         -> the engine forces opaque; needs 2 passes
            int total = 0, pure = 0, keyed = 0;
            int a0 = 0, a255 = 0, aMid = 0;
            auto at = [&](int x, int y) {
                const auto* p = static_cast<const std::uint8_t*>(map.pData) +
                                static_cast<size_t>(y) * map.RowPitch + static_cast<size_t>(x) * 4;
                return std::array<int, 4>{ p[0], p[1], p[2], p[3] };
            };
            for (int y = 0; y < a_h; y += 2) {
                for (int x = 0; x < a_w; x += 2) {
                    const auto c = at(x, y);
                    ++total;
                    // The painted colour is (255,0,255) in whichever channel
                    // order the surface uses -- both ends are 255, middle is 0.
                    if (c[0] == 255 && c[1] == 0 && c[2] == 255) ++pure;
                    if (c[0] > 200 && c[2] > 200 && c[1] < 60) ++keyed;
                    if (c[3] == 0)        ++a0;
                    else if (c[3] == 255) ++a255;
                    else                  ++aMid;
                }
            }
            const auto tl = at(1, 1);
            const auto br = at((std::max)(0, a_w - 2), (std::max)(0, a_h - 2));
            const auto mid = at(a_w / 2, a_h / 2);
            // ★★SAMPLE THE BLEND ITSELF. RGB alone cannot say how a
            // half-transparent pixel got its colour: un-blending the magenta
            // needs the alpha that produced it, and the first attempt at the
            // arithmetic came out negative -- which means an assumption is
            // wrong, not that it is impossible. These are the first few pixels
            // whose alpha is neither 0 nor 255, printed whole.
            {
                std::string s;
                int shown = 0;
                for (int y = 0; y < a_h && shown < 6; y += 7) {
                    for (int x = 0; x < a_w && shown < 6; x += 7) {
                        const auto c = at(x, y);
                        if (c[3] == 0 || c[3] == 255) continue;
                        s += std::format(" {:02X}{:02X}{:02X}/a{:02X}",
                                         c[0], c[1], c[2], c[3]);
                        ++shown;
                    }
                }
                if (shown) SKSE::log::info("[ICONDIAG]   blended:{}", s);
            }
            a_ctx->Unmap(staging, 0);
            ++s_probes;
            SKSE::log::info("[ICONDIAG] {} rect=({},{}) {}x{} pure={:.1f}% keyed={:.1f}% "
                            "| alpha 0={:.1f}% mid={:.1f}% 255={:.1f}% "
                            "| corner {:02X}{:02X}{:02X}/{:02X}{:02X}{:02X} "
                            "mid {:02X}{:02X}{:02X}",
                a_when, a_box.left, a_box.top, a_w, a_h,
                total ? 100.0 * pure / total : 0.0,
                total ? 100.0 * keyed / total : 0.0,
                total ? 100.0 * a0 / total : 0.0,
                total ? 100.0 * aMid / total : 0.0,
                total ? 100.0 * a255 / total : 0.0,
                tl[0], tl[1], tl[2], br[0], br[1], br[2], mid[0], mid[1], mid[2]);
        }
        staging->Release();
    }
#endif

    // ★1.0.5 — every load goes through here so the fresh-request path and the
    // self-heal reload cannot drift apart; they used to be two separate calls,
    // which is exactly how one gets changed and the other does not.
    // (An item-loader variant that attaches enchant effect passes was measured
    // and rejected — see Inv3D::Load for why.)
    void ItemPreview::LoadForCapture(RE::Inventory3DManager* a_mgr,
                                     RE::TESBoundObject* a_item)
    {
        Inv3D::Load(a_mgr, a_item, nullptr);
    }

    void ItemPreview::SetLightOffset(float a_azDeg, float a_elDeg)
    {
        // ★A changed lamp un-settles the rig even when the item is the same —
        // that is what makes an EDIT slider drag re-capture under the NEW angle
        // instead of accepting a frame still lit by the old one.
        if (a_azDeg != m_lightAz || a_elDeg != m_lightEl) m_parkTicks = 0;
        m_lightAz = a_azDeg;
        m_lightEl = a_elDeg;
    }

    bool ItemPreview::ParkSettled() const
    {
        return m_parkTicks >= 2;
    }

    int ItemPreview::ParkTicks() const
    {
        return m_parkTicks;
    }

    ItemPreview* ItemPreview::GetSingleton()
    {
        static ItemPreview singleton;
        return std::addressof(singleton);
    }

    void ItemPreview::GetMarginUV(ImVec2& a_uv0, ImVec2& a_uv1) const
    {
        const float kTex = static_cast<float>(kTexSize);
        const float effW = (std::min)(m_captureSize.x, m_lastCapturedSize.x);
        const float effH = (std::min)(m_captureSize.y, m_lastCapturedSize.y);
        const float startX = m_modelInTexture.x - effW * 0.5f;
        const float startY = m_modelInTexture.y - effH * 0.5f;
        a_uv0 = ImVec2(startX / kTex, startY / kTex);
        a_uv1 = ImVec2((startX + effW) / kTex, (startY + effH) / kTex);
    }

    bool ItemPreview::Initialize()
    {
        if (m_initialized) return true;

        // ★the member accessor is versioned now; the singleton is the one
        // that still answers statically
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data) return false;
        auto* device = reinterpret_cast<ID3D11Device*>(data->forwarder);
        if (!device) return false;

        // Source is the on-screen swap chain. Get the underlying texture.
        auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderWindows[0].renderView);
        if (!rtv) return false;

        ID3D11Resource* srcRes = nullptr;
        rtv->GetResource(&srcRes);
        if (!srcRes) return false;

        ID3D11Texture2D* srcTex = nullptr;
        srcRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
        srcRes->Release();
        if (!srcTex) return false;

        D3D11_TEXTURE2D_DESC srcDesc = {};
        srcTex->GetDesc(&srcDesc);
        srcTex->Release();

        if (!EnsureCaptureTextures(device, srcDesc)) return false;

        m_initialized = true;
        return true;
    }

    bool ItemPreview::EnsureCaptureTextures(ID3D11Device* a_device,
                                            const D3D11_TEXTURE2D_DESC& a_src)
    {
        if (!a_device) return false;
        // Already built for this surface.
        if (m_dstTex && m_dstSRV && m_scratchTex && m_dstTexB &&
            m_texFormat == static_cast<std::uint32_t>(a_src.Format) &&
            m_texW == a_src.Width && m_texH == a_src.Height) {
            return true;
        }
        // ★Rebuilt, not patched: the scratch has to match the surface EXACTLY
        // (CopyResource is a whole-resource copy) and the destination has to
        // share its format family (CopySubresourceRegion). A 10-bit surface
        // and an 8-bit one satisfy neither, and D3D reports it by silently
        // doing nothing — which is exactly how this failure looked from the
        // outside: every step "succeeded" and the pixels never arrived.
        if (m_dstSRV)     { m_dstSRV->Release();     m_dstSRV = nullptr; }
        if (m_dstTex)     { m_dstTex->Release();     m_dstTex = nullptr; }
        if (m_dstTexB)    { m_dstTexB->Release();    m_dstTexB = nullptr; }
        if (m_scratchTex) { m_scratchTex->Release(); m_scratchTex = nullptr; }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = kTexSize;
        desc.Height           = kTexSize;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = a_src.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &m_dstTex))) return false;
        if (FAILED(a_device->CreateShaderResourceView(m_dstTex, nullptr, &m_dstSRV))) {
            m_dstTex->Release();
            m_dstTex = nullptr;
            return false;
        }

        // Scratch holds the FULL backbuffer: the capture draw paints the
        // model to the live frame without clipping to the capture rect, so a
        // rect-only restore leaves any overspill visible (oversized items
        // peeking past the caching card for the capture frame). Full
        // save/restore reverts every pixel we touched.
        // GI77: the white pass lands here. Same shape as the black pass; it is
        // only ever read back on the CPU, never sampled, so no SRV.
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &m_dstTexB))) {
            m_dstSRV->Release(); m_dstSRV = nullptr;
            m_dstTex->Release(); m_dstTex = nullptr;
            return false;
        }
        D3D11_TEXTURE2D_DESC sdesc = a_src;
        sdesc.MipLevels      = 1;
        sdesc.ArraySize      = 1;
        sdesc.Usage          = D3D11_USAGE_DEFAULT;
        sdesc.BindFlags      = 0;
        sdesc.CPUAccessFlags = 0;
        sdesc.MiscFlags      = 0;
        if (FAILED(a_device->CreateTexture2D(&sdesc, nullptr, &m_scratchTex))) {
            m_dstSRV->Release(); m_dstSRV = nullptr;
            m_dstTex->Release(); m_dstTex = nullptr;
            m_dstTexB->Release(); m_dstTexB = nullptr;
            return false;
        }

        m_texFormat = static_cast<std::uint32_t>(a_src.Format);
        m_texW      = a_src.Width;
        m_texH      = a_src.Height;
        SKSE::log::info("[PREVIEW] capture textures built for {}x{} fmt={}",
            a_src.Width, a_src.Height, static_cast<int>(a_src.Format));
        return true;
    }

    void ItemPreview::Shutdown()
    {
        auto release = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };

        release(m_dstSRV);
        release(m_dstTex);
        release(m_dstTexB);
        release(m_scratchTex);
        m_initialized = false;
    }

    void ItemPreview::Begin()
    {
        if (m_running) return;

        // Default park point BEFORE anything loads: screen centre (the UI
        // window spawns there). Off-screen is impossible — the capture reads
        // the on-screen backbuffer, so the model must be on screen, hidden
        // behind the opaque window. Once the window draws, SetParkPos tracks
        // its actual centre every frame (and persists across opens).
        if (!m_hasPark) {
            const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
            if (sz.width > 0 && sz.height > 0) {
                m_parkPos = ImVec2(static_cast<float>(sz.width) * 0.5f,
                                   static_cast<float>(sz.height) * 0.5f);
                m_hasPark = true;
            }
        }

        if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
            // ★★★GI100: A SCENE LEFT STANDING IS NOT STILL OURS.
            //
            // GI73 adopted it, on the premise that our Begin3D was outstanding
            // and the scene still up was therefore the scene this session
            // wanted. The premise is false, and two independent measurements
            // say so.
            //
            // Between our close and our next open the VANILLA InventoryMenu
            // opens and closes -- it is on the road to every one of our opens,
            // and the intercept logs it every time:
            //
            //   [INV] intercepted InventoryMenu -> deferring GridInventoryMenu open
            //   [INV] InventoryMenu closed -> GridInventoryMenu opening
            //
            // That menu drives this very same Inventory3DManager. Whatever an
            // unpaired Begin3D of ours left behind does not survive it.
            //
            // ★MEASURED ACROSS THE BOUNDARY, one menu close apart:
            //
            //   before (our own scene)  every capture lands, ~30ms each
            //   after  (adopted scene)  ZERO land. Every item reads "no model"
            //                           and goes onto the PERMANENT fail list,
            //                           so the adoption does not merely fail to
            //                           work -- it condemns good items.
            //
            // ★And the close that follows is the CTD: three for three, always
            // the same instruction inside End3D (51774+0xA8), always on an
            // adopted scene. GI96's extra pointer tests passed it intact and
            // GI99's removal of the Unload in front of it did not stop it,
            // because the entry End3D walks belongs to a scene that is gone.
            //
            // So the old scene is neither adopted nor torn down -- both were
            // measured as crashes. The claim is dropped and a fresh scene is
            // opened.
            //
            // ★★The honest cost: IF our old Begin3D really were still
            // outstanding, this counts two against one and leaves a scene
            // standing at the end -- the rendering fault GI73 was written for
            // ("everything other than the player stops rendering", LO13OL75,
            // 1.6.0). Against a CTD plus a poisoned icon cache, that trade is
            // not close. The warning below is how a log will say it happened.
            if (m_scene3D) {
                // ★Name what the teardown was still waiting on. Dropping the
                // claim is safe but it is not free -- it spends a Begin3D that
                // no End3D of ours will ever pair -- so the next question is
                // why the close could not finish, and only the blocking reason
                // can answer it. Measured on the first build that had this
                // rule: thirteen drops in ninety seconds, every successful
                // teardown landing on frame ZERO. That is not a slow load; it
                // is a condition that never clears, and this line says which.
                const char* blocked = SceneUnsafeToTearDown(mgr);
                SKSE::log::warn("[PREVIEW] the previous scene was still claimed ({}, "
                                "blocked on: {}) -- dropping the claim and opening a "
                                "fresh one, because the vanilla InventoryMenu has been "
                                "through this manager since (GI100)",
                                m_sceneRefused ? "teardown refused" : "teardown never ran",
                                blocked ? blocked : "nothing right now");
            }
            m_scene3D      = false;
            m_sceneRefused = false;
            Inv3D::Begin3D(mgr, RE::INTERFACE_LIGHT_SCHEME::kInventory);
            m_scene3D = true;
            SKSE::log::info("[PREVIEW] Begin3D");
            m_running = true;
            ++m_session;   // cancels any teardown still deferred from the last close
            m_teardownPending = false;
        } else {
            SKSE::log::warn("[PREVIEW] Begin: Inventory3DManager null");
        }
    }

    void ItemPreview::End()
    {
        const bool wasRunning = m_running;
        // ★Put the light back BEFORE anything else in the teardown — the scene
        // is still whole here, whereas TeardownTick may run frames later
        // (or be skipped entirely on a stuck load).
        RestoreCaptureLight();
        // still running, node still reachable: take our zoom off it before
        // the scene is handed back (a 2.5x left behind is the next system's
        // problem, and it would be invisible until something else loads it)
        RestoreNodeScale();
        m_running   = false;
        m_requested = false;
        m_current   = nullptr;
        Shutdown();
        // Engine teardown may have to wait for an in-flight model load (menu
        // closed mid-capture) — End3D right now would be the null-spModel CTD.
        if (wasRunning) {
            m_teardownPending = true;
            m_teardownSession = m_session;
            m_teardownFrames  = 0;
            TeardownTick();   // the ordinary case lands on this very frame
        }
    }

    // The teardown itself, in one place because three roads reach it now: the
    // close, the per-frame wait behind it, and GI98's retry at the next open.
    // nullptr when the scene is down; otherwise the reason it is not.
    const char* ItemPreview::AttemptTeardown(RE::Inventory3DManager* a_mgr)
    {
        if (const char* why = SceneUnsafeToTearDown(a_mgr)) return why;
        // ★★★GI99: NOTHING TOUCHES THE ARRAY BEFORE End3D WALKS IT.
        //
        // An Inv3D::Unload stood here, and EVERY crash of this shape has that
        // call immediately in front of End3D:
        //
        //   2026-08-21  ResetScene       -> Unload -> End3D -> rcx = 0
        //   2026-09-13  TeardownWhenIdle -> Unload -> End3D -> rcx = 0  (1.6.3)
        //   2026-09-13  AttemptTeardown  -> Unload -> End3D -> rcx = 0  (GI96-98)
        //
        // all three at the identical instruction, 51774+0xA8, reached from
        // 51756 = End3D's per-entry loop. The one teardown in those same logs
        // that did NOT crash is the one where the scene was empty and the
        // Unload therefore did nothing.
        //
        // ★And the call was never buying anything. This project's own A/B
        // measured it and wrote it down in IconCache::FinishPending: "Inv3D::
        // Unload is Inventory3DManager::Clear3D ... and Clear3D does NOT give
        // the slot back: loadedModels only empties at End3D." So it clears the
        // model OUT OF an entry that then STAYS in the array -- and End3D
        // walks that entry. End3D empties the array by itself, which is the
        // whole reason ResetScene is built out of End3D + Begin3D.
        //
        // ★This is also why the 2026-08-21 answer -- ask the guard AGAIN after
        // the Unload -- could not work, and why the crash came back twice with
        // that re-ask in place. The guard reads pointers, and an entry the
        // engine has cleared does not have to answer null.
        //
        // ★GI96's two extra pointer tests stay. They cost nothing and they are
        // how we will know, next time, that the entry was whole going in.
        {
            // One line per entry, once per menu close, at most seven: if this
            // ever crashes again the log names the array End3D was about to
            // walk, which the crash dump cannot.
            for (auto& lm : a_mgr->GetRuntimeData().loadedModels) {
                SKSE::log::info("[PREVIEW]   End3D entry {:08X} '{}' model={} r={:.1f}",
                    lm.itemBase ? lm.itemBase->GetFormID() : 0,
                    lm.itemBase && lm.itemBase->GetName() ? lm.itemBase->GetName() : "-",
                    lm.spModel ? 1 : 0,
                    lm.spModel ? lm.spModel->worldBound.radius : -1.0f);
            }
        }
        Inv3D::End3D(a_mgr);
        m_scene3D      = false;   // GI73: the pair is closed; the next open opens a scene
        m_sceneRefused = false;
        return nullptr;
    }

    // ★★★GI97: ONE TRY PER FRAME, AND A FRAME HAS TO BE A FRAME.
    //
    // This waited by reposting itself through SKSE's task interface and
    // counting the reposts, on the reading that one repost was one frame of
    // patience. It is not: a task queued while the task queue is DRAINING is
    // drained in that same pass, so the loop spun inside a single frame and
    // spent the whole budget before the engine had one opportunity to finish
    // the load it was waiting for. Measured, in the log this was found in:
    //
    //   00:38:42.158  [UI] menu hidden
    //   00:38:42.165  [PREVIEW] End: model never finished (load stuck in
    //                 flight), teardown skipped
    //
    // Three hundred "frames" of patience in SEVEN MILLISECONDS. The scene was
    // left standing, the next open adopted it (GI98), and that session's close
    // was the CTD. The budget was never the problem; the wait was.
    //
    // Tick() runs from the PlayerCharacter update hook on every unpaused
    // frame, which is exactly the window this waits in: the menu is shut, so
    // the AdvanceMovie road is gone and the game is running again. So 300 is
    // now the five seconds it always read as.
    void ItemPreview::TeardownTick()
    {
        constexpr int kTeardownFrames = 300;   // ~5s at 60fps
        if (!m_teardownPending) return;
        // the menu reopened: the NEW session owns the scene now and its own
        // End() pairs the teardown — this stale one must not fire
        if (m_teardownSession != m_session || m_running) {
            m_teardownPending = false;
            return;
        }
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        if (!mgr) {
            m_teardownPending = false;
            return;
        }
        if (const char* why = AttemptTeardown(mgr)) {
            if (++m_teardownFrames < kTeardownFrames) return;   // wait a real frame
            // A load that never lands: leave the scene untouched rather than
            // risk the CTD -- and REMEMBER that we did, so the next open does
            // not quietly build on it (GI98).
            m_teardownPending = false;
            m_sceneRefused    = true;
            SKSE::log::warn("[PREVIEW] End: the scene never settled ({}) in {} "
                            "frames -- teardown skipped, and the next open will "
                            "not build on it (GI98)", why, kTeardownFrames);
            return;
        }
        m_teardownPending = false;
        // ★GI73: ALWAYS, not only when it was deferred. A successful teardown
        // used to log nothing at all, so a log could show Begin3D twice and give
        // no way to tell whether an End3D had run between them -- which is why
        // the unbalanced pair could not be seen in any of the three reporter
        // logs that went past it. One line per menu close buys the whole
        // question back.
        SKSE::log::info("[PREVIEW] End3D (waited {} frame(s))", m_teardownFrames);
    }

    RE::NiAVObject* ItemPreview::FindCurrentModel() const
    {
        if (!m_current) return nullptr;
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        if (!mgr) return nullptr;
        for (auto& lm : mgr->GetRuntimeData().loadedModels) {
            if ((lm.itemBase == m_current || lm.modelObj == m_current) && lm.spModel) {
                return lm.spModel.get();
            }
        }

        // Same-nif fallback: the engine DEDUPES loads that share a model file
        // (enchanted weapon variants etc.) — no new entry is created and the
        // existing one keeps the first requester as itemBase, so the exact
        // match above never fires (this stalled 3 items for 2s each). Same
        // nif = identical visual, so capturing that entry is exact.
        const auto* mdl = skyrim_cast<RE::TESModel*>(m_current);
        const char* path = mdl ? mdl->GetModel() : nullptr;
        if (path && *path) {
            for (auto& lm : mgr->GetRuntimeData().loadedModels) {
                if (!lm.spModel) continue;
                // BUGFIX (precache "model=false" cascade): the dedup entry
                // keeps the FIRST requester in itemBase — the old fallback
                // compared modelObj only, which is often null, so same-nif
                // variants never matched and every one timed out. Check both.
                for (RE::TESForm* src : { lm.itemBase,
                         static_cast<RE::TESForm*>(lm.modelObj) }) {
                    // ★★THE ENGINE'S LIST CAN HOLD A DEAD POINTER, AND A NULL
                    // CHECK CANNOT SEE IT.
                    //
                    // These two are raw form pointers inside the engine's
                    // loadedModels, and during a model swap an entry can keep
                    // its spModel while the form beside it has already gone.
                    // skyrim_cast then walks a vtable that is not there any
                    // more and the RTTI machinery THROWS (__non_rtti_object) --
                    // which reaches the render thread as an unhandled exception
                    // and takes the game down.
                    //
                    // Measured: drinking two potions straight out of a container
                    // in quick succession churns the board, the typed bag's
                    // preview reloads, and the crash log named our own bag MISC
                    // as the object whose RTTI could not be read. The list
                    // belongs to the engine and there is nothing here to lock,
                    // so the cast is guarded and a bad entry is skipped.
                    // GetModel() is inside the guard for the same reason: it
                    // dereferences the very object we could not verify.
                    if (!src) continue;
                    const char* p2 = nullptr;
                    try {
                        if (const auto* mdl2 = skyrim_cast<RE::TESModel*>(src)) {
                            p2 = mdl2->GetModel();
                        }
                    } catch (...) {
                        continue;   // recycled entry -- it is not our model
                    }
                    if (p2 && _stricmp(p2, path) == 0) {
                        return lm.spModel.get();
                    }
                }
            }
        }
        return nullptr;
    }

    bool ItemPreview::LoadPending() const
    {
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        return mgr && LoadInFlight(mgr);
    }

    bool ItemPreview::RotationApplied() const
    {
        auto* model = FindCurrentModel();
        if (!model) return false;
        constexpr float kDeg = 0.017453292f;
        RE::NiMatrix3 want;
        want.SetEulerAnglesXYZ(m_def.rx * kDeg, m_def.ry * kDeg, m_def.rz * kDeg);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (std::fabs(want.entry[r][c] - model->local.rotate.entry[r][c]) > 0.02f) {
                    return false;
                }
            }
        }
        return true;
    }

    // ★Symmetry: the capture zoom is OUR write on an engine-owned node, so it
    // comes off before we let the node go. Skipping this is not "the node is
    // about to die anyway" — the engine dedupes same-nif loads and can hand
    // the very same node back, and it comes back wearing our 2.5x.
    void ItemPreview::RestoreNodeScale()
    {
        if (!m_scaledNode) return;
        // The node may already be freed — the pointer alone is not proof it
        // exists. Only write through it if the scene still holds it.
        if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
            for (auto& lm : mgr->GetRuntimeData().loadedModels) {
                if (lm.spModel && lm.spModel.get() == m_scaledNode) {
                    m_scaledNode->local.scale = m_savedNodeScale;
                    break;
                }
            }
        }
        m_scaledNode = nullptr;
    }

    void ItemPreview::UnloadCurrent()
    {
        RestoreNodeScale();
        if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
            Inv3D::Unload(mgr);
        }
        m_current = nullptr;
    }

    bool ItemPreview::ResetScene()
    {
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        if (!mgr || !m_running) return false;
        if (LoadInFlight(mgr)) {
            return false;   // deferred — caller retries once the load lands
        }
        // ★GI96: this road calls the same End3D, so it asks the same
        // null-pointer question. NOT the radius one: that is a "not yet" and
        // this path is unbounded (see SceneModelIncomplete's note), whereas an
        // entry with no form pointer is a defect no amount of waiting mends.
        if (const char* half = SceneEntryHalfBuilt(mgr)) {
            SKSE::log::warn("[PREVIEW] scene reset refused: {}", half);
            return false;
        }
        RestoreNodeScale();
        // ★★★GI99: AND NO Inv3D::Unload HERE EITHER -- see AttemptTeardown.
        //
        // This is the call site of the 2026-08-21 crash, and it had the same
        // Unload-then-End3D pair. The answer taken then was to ask the guard a
        // SECOND time, after the Unload, on the reading that the first answer
        // had gone stale. It could not work: a guard reads pointers, and an
        // entry the engine has cleared does not have to answer null. The crash
        // returned twice.
        //
        // Clear3D does not free the slot anyway (measured -- IconCache::
        // FinishPending has the A/B), so the call was buying nothing and
        // handing End3D an entry it had already been through. End3D on its own
        // is what empties the array, which is the whole point of this function.
        //
        // ★RestoreNodeScale above stays and stays FIRST: that is OUR write on
        // an engine node, and it has to come off while the node is still ours
        // to reach.
        // ★GI73: balanced IN PLACE -- one out, one straight back in -- so the
        // outstanding-Begin3D count is unchanged and m_scene3D stays true. It is
        // written rather than left implied because the two calls have to move
        // together: dropping the Begin3D here without clearing the flag would
        // leave Begin() adopting a scene that is no longer there.
        Inv3D::End3D(mgr);
        Inv3D::Begin3D(mgr, RE::INTERFACE_LIGHT_SCHEME::kInventory);
        m_scene3D = true;
        m_current = nullptr;
        SKSE::log::info("[PREVIEW] scene reset (loadedModels was full)");
        return true;
    }

    void ItemPreview::Tick()
    {
        // ★GI97: BEFORE the running guard -- this is the job that belongs to a
        // CLOSED menu. Tick() arrives here from the PlayerCharacter update hook
        // on unpaused frames, which is the only per-frame heartbeat left once
        // our own AdvanceMovie has stopped driving one.
        TeardownTick();
        if (!m_running) return;
        UpdateParking();
    }

    int ItemPreview::SceneModelCount() const
    {
        if (!m_running) return 0;
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        return mgr ? static_cast<int>(mgr->GetRuntimeData().loadedModels.size()) : 0;
    }

    void ItemPreview::UpdateParking()
    {
        // Capture zoom: ★the NODE scale is the ONE lever, deliberately.
        // The engine's mgr->itemScale used to be pushed as well ("whichever
        // the engine honours wins") — but right after game launch the engine
        // honours BOTH for the first captures, and 2.5 x 2.5 is the "some
        // icons capture huge until I reset once more" report. The node is
        // re-applied by us every frame, so it alone is deterministic.
        if (m_hasSavedScale) {   // heal anything an earlier code path left behind
            if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
                mgr->itemScale = m_savedItemScale;
            }
            m_hasSavedScale = false;
        }

        auto* model = FindCurrentModel();
        if (!model || model->worldBound.radius <= 0.0f) return;
        // Node scale is SAVED and RESTORED, never left behind: the engine keeps
        // one node per nif and the same-nif fast path retargets it without a
        // reload, so a stray 2x would leak into later captures of that model
        // (enchant variants) — those aren't pinned, so the oversized sprite
        // would have been written to the pak permanently.
        // ★Re-applied EVERY frame, like the node scale and for the same reason:
        // the engine owns this scene and re-asserts the light scheme on its own
        // schedule. Setting it once at request time would work until it didn't,
        // and the failure would be one item captured under its neighbour's lamp.
        const bool newNode = (m_scaledNode != model);
        PlaceLight(m_lightAz, m_lightEl);
        // ★★ONLY WHILE A CAPTURE IS IN FLIGHT. This is scene-global state, so
        // held for the whole menu it lights the WORLD as well -- invisible
        // behind an opaque skin, plainly wrong behind a translucent one.
        // ★Asked of the icon QUEUE, not of m_requested: that flag is raised
        // and consumed inside PostDisplay, long after this runs, so it reads
        // false here every time. IsBusy() is true from the frame the queue
        // takes work, which is at least one frame before m_parkTicks lets any
        // pixels be accepted -- the sky is always in place before the shot.
        // ★This request's rig is now on the scene. The gate waits for a SECOND
        // application, so the pixels it accepts were rendered after this one —
        // not with whatever the previous item left behind.
        if (m_parkTicks < 2) ++m_parkTicks;

        if (m_inspectScale > 0.0f) {
            if (m_scaledNode != model) {
                // a DIFFERENT node: hand the old one its base back (it may
                // still be in the scene) before reading this one's base
                RestoreNodeScale();
                m_savedNodeScale = model->local.scale;
                // Backstop: an item node's own scale is ~1. Anything wildly
                // past that is not a base, it is a zoom that escaped some
                // path we do not know about — and multiplying it again is
                // how the runaway starts. Refuse it and say so.
                if (!(m_savedNodeScale > 0.001f && m_savedNodeScale < 100.0f)) {
                    SKSE::log::warn("[PREVIEW] implausible node scale {:.3f} on '{}' — using 1.0",
                        m_savedNodeScale, m_current ? m_current->GetName() : "-");
                    m_savedNodeScale = 1.0f;
                }
                m_scaledNode = model;
                // ★Verification aid: log ONLY the items whose def actually
                // moves the lamp. During a full precache that is a handful of
                // lines among thousands, and it answers the one question that
                // matters — did each item get ITS angle, or did a neighbour's
                // setting bleed across because the write landed a frame late.
                // (the per-frame park log above covers this; nothing extra here)
            }
            // recomputed from the base every frame, never from the current
            // value — re-entry can never compound
            model->local.scale = m_savedNodeScale * m_inspectScale;
        } else {
            RestoreNodeScale();
        }

        // Rotation lives on the node (the engine leaves it alone). Scale is
        // NOT applied to the engine at all — Modex's own approach: the def
        // scale shrinks/grows the CROP REGION (Request's modelScale param),
        // and the fixed-size tile stretch does the zoom. Pure 2D, no engine
        // state to fight (node/itemScale writes kept getting overwritten).
        constexpr float kDeg = 0.017453292f;
        model->local.rotate.SetEulerAnglesXYZ(m_def.rx * kDeg, m_def.ry * kDeg, m_def.rz * kDeg);
        bool dirty = true;

        if (m_hasPark) {
            auto* scn = RE::UI3DSceneManager::GetSingleton();
            const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
            if (scn && sz.width > 0 && sz.height > 0) {
                const auto& vf = scn->viewFrustum;
                const auto& t  = model->local.translate;
                const float world_minx   = -vf.fLeft * t.y;
                const float world_minz   = -vf.fBottom * t.y;
                const float world_width  = -vf.fRight * t.y - world_minx;
                const float world_height = -vf.fTop * t.y - world_minz;
                const float ratio_x = world_width / static_cast<float>(sz.width);
                const float ratio_y = world_height / static_cast<float>(sz.height);
                if (ratio_x != 0.0f && ratio_y != 0.0f) {
                    if (dirty) {   // rotation moved the bound centre: refresh first
                        RE::NiUpdateData ud;
                        model->Update(ud);
                        dirty = false;
                    }
                    const auto& c = model->worldBound.center;
                    const float model_sx = -(c.x + world_minx) / ratio_x;
                    const float model_sy = -(c.z + world_minz) / ratio_y;
                    const float dsx = m_parkPos.x - model_sx;
                    const float dsy = m_parkPos.y - model_sy;
                    if (std::fabs(dsx) > 0.5f || std::fabs(dsy) > 0.5f) {
                        // inverse of the projection: dworld = -dscreen * ratio
                        model->local.translate.x += -dsx * ratio_x;
                        model->local.translate.z += -dsy * ratio_y;
                        dirty = true;
                    }
                }
            }
        }

        if (dirty) {
            RE::NiUpdateData ud;
            model->Update(ud);
        }
    }

    void ItemPreview::Request(RE::TESBoundObject* a_item, ImVec2 a_screenPos, ImVec2 a_screenSize,
                              float a_modelScale, float a_offsetX, float a_offsetY,
                              const IconDef* a_def, bool a_spell)
    {
        if (!m_running || a_item == nullptr) return;
        // model-less leveled-item stubs CTD inside the engine's load task —
        // last-ditch guard behind IconCache's queue-side filter
        if (a_item->Is(RE::FormType::LeveledItem)) return;

        // ★★Reset the settle counter ONLY when the target actually changes.
        // Request() is re-issued every frame while a capture is pending, so
        // clearing it unconditionally held the count at 0 forever and every
        // capture timed out with "no model" despite the model being present
        // and correctly rotated. What the gate needs to know is "has THIS
        // item's rig been applied", and that only becomes false again when the
        // item — or its light, see SetLightOffset — changes.
        if (a_item != m_current) {
            m_parkTicks = 0;
            m_captureBoost = 0.0f;
        }

        // Same-nif fast path: enchanted variants share one model file, and the
        // engine DEDUPES such loads anyway — worse, a dedup onto an entry a
        // previous Unload detached renders EMPTY and the capture times out
        // (the precache "2/sec, all skipped" cascade). If the incoming item
        // uses the SAME nif as the currently loaded one, keep the model and
        // just retarget: capture accepts within a frame or two.
        if (a_item != m_current && m_current) {
            const auto* mdlNew = skyrim_cast<RE::TESModel*>(a_item);
            const auto* mdlCur = skyrim_cast<RE::TESModel*>(m_current);
            if (mdlNew && mdlCur && mdlNew->GetModel() && mdlCur->GetModel() &&
                mdlNew->GetModel()[0] != '\0' &&
                _stricmp(mdlNew->GetModel(), mdlCur->GetModel()) == 0 &&
                FindCurrentModel() != nullptr) {
                m_current = a_item;
                m_currentIsSpell = a_spell;   // GI74b: told by the caller, never derived here
                m_def = a_def ? *a_def : IconDef{};
            }
        }

        if (a_item != m_current) {
            if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
                // The 7-slot loadedModels array fills up with late-landing
                // async loads (Unload before landing is a no-op); when near
                // capacity a fresh Load silently fails — reset the scene first.
                if (mgr->GetRuntimeData().loadedModels.size() >= 5) {
                    ResetScene();
                }

                // Birth position: point the manager's itemPos at the park
                // point BEFORE loading, so the model never spends a single
                // frame at the engine's default on-screen spot.
                if (m_hasPark) {
                    if (auto* scn = RE::UI3DSceneManager::GetSingleton()) {
                        const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
                        float ty = mgr->itemPos.y;
                        if (ty > -1.0f) ty = -500.0f;   // uninitialised → vanilla-ish depth
                        const auto& vf = scn->viewFrustum;
                        const float world_minx = -vf.fLeft * ty;
                        const float world_minz = -vf.fBottom * ty;
                        const float ww = -vf.fRight * ty - world_minx;
                        const float wh = -vf.fTop * ty - world_minz;
                        if (sz.width > 0 && sz.height > 0 && ww != 0.0f && wh != 0.0f) {
                            const float rw = ww / static_cast<float>(sz.width);
                            const float rh = wh / static_cast<float>(sz.height);
                            RE::NiPoint3 p;
                            p.x = -m_parkPos.x * rw - world_minx;
                            p.z = -m_parkPos.y * rh - world_minz;
                            p.y = ty;
                            mgr->itemPos = p;
                            mgr->itemPosCopy = p;
                        }
                    }
                }

                // BEFORE the unload, while the node is still reachable: give
                // it its base scale back. Dropping a flag here instead (what
                // this did) was the bug — a dedup can return this same node,
                // and its base would then be read back with our zoom already
                // baked in.
                RestoreNodeScale();
                Inv3D::Unload(mgr);
                LoadForCapture(mgr, a_item);
                SKSE::log::info("[PREVIEW] load '{}'", a_item->GetName());
            }
            m_current = a_item;
            m_currentIsSpell = a_spell;   // GI74b: told by the caller, never derived here
            m_def = a_def ? *a_def : IconDef{};
        } else if (a_def && (a_def->rx != m_def.rx || a_def->ry != m_def.ry ||
                             a_def->rz != m_def.rz || a_def->scale != m_def.scale)) {
            // SAME item, new def (live editing): re-apply without reloading —
            // rotation is absolute and scale is base-anchored, so this is safe
            m_def = *a_def;
            // ★fresh ladder for a fresh orientation: a boost escalated by one
            // rotation (its diagonal touched the box) is meaningless for the
            // next, and carrying it forward is how one bad angle mid-drag
            // walked the box to its ceiling and stuck there
            m_captureBoost = 0.0f;
        }

        const float modelScale = (a_modelScale >= 0.0f) ? a_modelScale : kDefaultModelScale;
        const float expand     = (modelScale > 0.0f) ? (1.0f / modelScale) : 1.0f;

        m_capturePos  = a_screenPos;
        m_innerSize   = ImVec2((std::max)(4.0f, a_screenSize.x * expand),
                               (std::max)(4.0f, a_screenSize.y * expand));
        m_captureSize = ImVec2(m_innerSize.x * kSafetyMargin, m_innerSize.y * kSafetyMargin);
        if (m_captureBoost > (std::max)(m_captureSize.x, m_captureSize.y)) {
            // clamp to the SCREEN as well as the copy texture: the capture
            // reads backbuffer pixels, and a copy box larger than the screen
            // is rejected wholesale by D3D — leaving stale pixels behind
            float lim = static_cast<float>(kTexSize);
            const auto scr = RE::BSGraphics::Renderer::GetScreenSize();
            if (scr.width > 0 && scr.height > 0) {
                lim = (std::min)({ lim, static_cast<float>(scr.width),
                                   static_cast<float>(scr.height) });
            }
            const float grown = (std::min)(m_captureBoost, lim);
            m_captureSize = ImVec2(grown, grown);
            m_innerSize   = ImVec2(grown / kSafetyMargin, grown / kSafetyMargin);
        }
        m_requested   = true;

        m_hasOverrideOffset = true;
        m_overrideOffsetX   = a_offsetX;
        m_overrideOffsetY   = a_offsetY;
    }

    void ItemPreview::Render()
    {
        static int s_frame = 0;
        ++s_frame;

        const bool req = m_requested;
        m_requested = false;
        if (!m_running || !req) return;
        if (!m_initialized && !Initialize()) {
            if (s_frame % 120 == 0) SKSE::log::warn("[PREVIEW] Initialize failing");
            return;
        }

        auto* inv = RE::Inventory3DManager::GetSingleton();
        if (!inv) return;

        // Self-heal: a Load that never landed. Two causes -- the vanilla menu's
        // deferred teardown, and a Load issued on the frame ResetScene rebuilt
        // the scene (the request hits a scene still standing up and is dropped;
        // observed 49 times in one user session, always right after a reset).
        //
        // ★GI68: the test is "the scene is empty AND the engine is not loading",
        // not a frame countdown. loadTask being set means the request DID take
        // and the engine is working on it -- on a hard disk or with 4K textures
        // that can take many frames, and re-issuing then would only throw the
        // work away. Asking the engine what it is doing needs no constant and
        // no guess about how fast the player's disk is.
        //
        // A repair sets loadTask, so this fires at most once per frame and stops
        // by itself the moment the model lands.
        {
            auto& rt = inv->GetRuntimeData();
            if (m_current && rt.loadedModels.empty() && !rt.loadTask) {
                LoadForCapture(inv, m_current);
                if (++m_healRun <= 3) {   // log the first few; the rest is noise
                    SKSE::log::info("[PREVIEW] self-heal reload '{}'", m_current->GetName());
                }
            } else {
                m_healRun = 0;
            }
            if (s_frame % 120 == 0) {
                const float r = (!rt.loadedModels.empty() && rt.loadedModels.back().spModel)
                    ? rt.loadedModels.back().spModel->worldBound.radius : -1.0f;
                SKSE::log::info("[PREVIEW] state: models={} backRadius={:.1f} cur='{}'",
                    rt.loadedModels.size(), r, m_current ? m_current->GetName() : "-");
            }
        }

        // Capture backbuffer in place without translation (upstream issue #48):
        // recentre the capture rect on the model's projected screen position.
        // Parking normally ran already in Tick() (game-update hook, before the
        // frame rendered); run it again here as a safety net for the first
        // frame after landing, then recentre on the (parked) model.
        UpdateParking();
        {
            auto* scn0 = RE::UI3DSceneManager::GetSingleton();
            auto& runtime0 = inv->GetRuntimeData();
            if (scn0 && !runtime0.loadedModels.empty()) {
                // Prefer the entry matching the requested item — back() can be
                // a stale previous model while async loads are still landing.
                auto* spModel0 = FindCurrentModel();
                if (!spModel0) spModel0 = runtime0.loadedModels.back().spModel.get();
                if (spModel0 && spModel0->worldBound.radius > 0.0f) {
                    const auto& vf = scn0->viewFrustum;
                    const auto& t  = spModel0->local.translate;
                    const float world_minx   = -vf.fLeft * t.y;
                    const float world_minz   = -vf.fBottom * t.y;
                    const float world_width  = -vf.fRight * t.y - world_minx;
                    const float world_height = -vf.fTop * t.y - world_minz;

                    const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
                    if (sz.width > 0 && sz.height > 0) {
                        const float ratio_x = world_width / static_cast<float>(sz.width);
                        const float ratio_y = world_height / static_cast<float>(sz.height);
                        if (ratio_x != 0.0f && ratio_y != 0.0f) {
                            // Some records make the engine render far larger
                            // than the standard box (e.g. Moth Priest Robes) —
                            // grow the capture rect to the projected bound so
                            // the alpha-trim never bakes in clipping. The trim
                            // stores only real pixels, so a larger box costs
                            // nothing for normal-sized items.
                            const float r = spModel0->worldBound.radius;
                            const float projW = 2.0f * r / std::fabs(ratio_x);
                            const float projH = 2.0f * r / std::fabs(ratio_y);
                            const float need  = (std::max)(projW, projH) * 1.05f;
                            if (need > m_captureSize.x || need > m_captureSize.y) {
                                const float grown = (std::min)(
                                    (std::max)({ need, m_captureSize.x, m_captureSize.y }),
                                    static_cast<float>(kTexSize));
                                m_captureSize = ImVec2(grown, grown);
                                m_innerSize   = ImVec2(grown / kSafetyMargin,
                                                       grown / kSafetyMargin);
                            }
                            const auto& c = spModel0->worldBound.center;
                            const float model_sx = -(c.x + world_minx) / ratio_x;
                            const float model_sy = -(c.z + world_minz) / ratio_y;
                            m_capturePos = ImVec2(
                                model_sx - m_captureSize.x * 0.5f,
                                model_sy - m_captureSize.y * 0.5f);
                        }
                    }
                }
            }
        }

        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data) return;
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
        if (!context) return;

        // ★★★ASK THE PIPELINE, DO NOT ASSUME. renderWindows[0] used to be
        // taken as "the screen", and with a D3D12 swap chain (CS Upscaling,
        // and therefore anyone on DLSS/FSR or frame generation) it is not: the
        // engine draws into a different resource in a different format. Every
        // step then landed on a surface nobody displays — the clear, the read
        // AND the restore — so the capture came back empty AND the parked
        // model stayed visible behind the window. Measured on that setup:
        //     renderWindows[0] = 0x..2060 fmt=24 (R10G10B10A2)
        //     actually bound   = 0x..12a0 fmt=28 (R8G8B8A8)
        // Whatever is bound right now IS the surface being drawn; take it, and
        // fall back to renderWindows[0] only when nothing is bound at all.
        struct RtvHold {
            ID3D11RenderTargetView* p = nullptr;
            bool owned = false;
            ~RtvHold() { if (owned && p) p->Release(); }
        } hold;
        context->OMGetRenderTargets(1, &hold.p, nullptr);   // AddRef on success
        hold.owned = (hold.p != nullptr);
        if (!hold.p) {
            hold.p = reinterpret_cast<ID3D11RenderTargetView*>(
                data->renderWindows[0].renderView);
        }
        auto* rtv = hold.p;
        if (!rtv) return;

        ID3D11Resource* srcRes = nullptr;
        rtv->GetResource(&srcRes);
        if (!srcRes) return;
        ID3D11Texture2D* srcTex = nullptr;
        srcRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
        srcRes->Release();
        if (!srcTex) return;

        // The bound surface decides the capture textures, not the other way
        // round. Free when it changes (swap-chain switch mid-session).
        {
            D3D11_TEXTURE2D_DESC bd{};
            srcTex->GetDesc(&bd);
            auto* dev = reinterpret_cast<ID3D11Device*>(data->forwarder);
            if (!EnsureCaptureTextures(dev, bd)) { srcTex->Release(); return; }
        }
        if (!m_dstTex || !m_dstTexB || !m_scratchTex) { srcTex->Release(); return; }

        // Compute the clamped backbuffer rect once. Save/clear/capture/restore
        // all operate on this single box.
        const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
        int left   = static_cast<int>(m_capturePos.x);
        int top    = static_cast<int>(m_capturePos.y);
        int width  = static_cast<int>(m_captureSize.x);
        int height = static_cast<int>(m_captureSize.y);
        if (left < 0) { width += left; left = 0; }
        if (top < 0)  { height += top; top = 0; }

        // ★★These two exits used to be SILENT, and they are the only way a
        // capture can fail with a model that is loaded, rotated and settled:
        // the stamp never advances, every gate keeps answering "not ready",
        // and 45 frames later the item lands in the PERSISTED fail list. A
        // user reporting "nothing ever caches" produced logs indistinguishable
        // from a slow disk because of it. The rect is clamped against the
        // BACKBUFFER, so a park point near a screen edge — the main window's
        // centre on any opaque skin — is enough to starve it.
        auto rectStarved = [&](const char* a_where) {
            static int s_seen = 0;
            if ((s_seen++ % 120) != 0) return;   // one line per 120 misses
            SKSE::log::warn(
                "[PREVIEW] capture rect starved ({}): rect=({},{}) {}x{} "
                "screen={}x{} park=({:.0f},{:.0f}) box={:.0f}x{:.0f} cur='{}'",
                a_where, left, top, width, height,
                screenSize.width, screenSize.height, m_parkPos.x, m_parkPos.y,
                m_captureSize.x, m_captureSize.y,
                m_current ? m_current->GetName() : "-");
        };
        if (width <= 0 || height <= 0) {
            rectStarved("negative after top-left clamp");
            srcTex->Release();
            return;
        }

        const int maxW = static_cast<int>(screenSize.width) - left;
        const int maxH = static_cast<int>(screenSize.height) - top;
        if (width > maxW)  width = maxW;
        if (height > maxH) height = maxH;
        if (width > static_cast<int>(kTexSize))  width = static_cast<int>(kTexSize);
        if (height > static_cast<int>(kTexSize)) height = static_cast<int>(kTexSize);
        if (width <= 0 || height <= 0) {
            rectStarved("off the right/bottom edge");
            srcTex->Release();
            return;
        }

        D3D11_BOX box = {};
        box.left   = static_cast<UINT>(left);
        box.top    = static_cast<UINT>(top);
        box.front  = 0;
        box.right  = static_cast<UINT>(left + width);
        box.bottom = static_cast<UINT>(top + height);
        box.back   = 1;

        // ★★★ALWAYS ON, ONCE PER SESSION. This used to be behind
        // GI_CAPTURE_DIAG, which meant the one fact that explains most capture
        // reports was missing from every log a player ever sent. Two
        // investigations were spent getting it back by other means: a pink
        // backdrop that turned out to be a 10-bit surface, and a crash whose
        // log showed frame generation only because the DLL happened to be in
        // the module list. Both would have been one line here.
        //
        // Costs one QueryInterface and one log line for the whole session.
        //
        // Surface facts, once. An upscaler changes exactly these: the format
        // (HDR/typeless), the size (render resolution below output), and the
        // sample count. Reported by NAME so the report needs no lookup table.
        {
            static bool s_told = false;
            if (!s_told) {
                s_told = true;
                D3D11_TEXTURE2D_DESC sd = {};
                srcTex->GetDesc(&sd);
                const char* fmt = "other";
                switch (sd.Format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM:      fmt = "R8G8B8A8_UNORM"; break;
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: fmt = "R8G8B8A8_UNORM_SRGB"; break;
                case DXGI_FORMAT_B8G8R8A8_UNORM:      fmt = "B8G8R8A8_UNORM"; break;
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: fmt = "B8G8R8A8_UNORM_SRGB"; break;
                case DXGI_FORMAT_R10G10B10A2_UNORM:   fmt = "R10G10B10A2_UNORM"; break;
                case DXGI_FORMAT_R16G16B16A16_FLOAT:  fmt = "R16G16B16A16_FLOAT"; break;
                case DXGI_FORMAT_R11G11B10_FLOAT:     fmt = "R11G11B10_FLOAT"; break;
                default: break;
                }
                SKSE::log::info("[ICONDIAG] backbuffer {}x{} fmt={} ({}) samples={} "
                                "bindFlags=0x{:X} misc=0x{:X}",
                    sd.Width, sd.Height, static_cast<int>(sd.Format), fmt,
                    sd.SampleDesc.Count, sd.BindFlags, sd.MiscFlags);

                // ★★★Is D3D11 still the real device, or a proxy over D3D12?
                // Community Shaders' Frame Generation installs a D3D11-to-D3D12
                // proxy and says so in its own UI ("can create compatibility
                // issues"); its status line then reads "D3D12 Swap Chain:
                // Active". Under that proxy the D3D11 backbuffer this capture
                // reads is not the surface being presented, which fits the
                // report exactly: shape correct (we drew it), colour garbage
                // (we read a buffer nobody filled).
                //
                // Asked of the SWAP CHAIN rather than inferred from anything
                // else -- one QueryInterface settles it.
                if (auto* win = data->renderWindows;
                    win && win[0].swapChain) {
                    auto* sc = reinterpret_cast<IUnknown*>(win[0].swapChain);
                    IUnknown* dev12 = nullptr;
                    IDXGISwapChain* sc1 = nullptr;
                    bool is12 = false;
                    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain),
                                                     reinterpret_cast<void**>(&sc1))) && sc1) {
                        // GUID of ID3D12Device, spelled out so this file needs
                        // no d3d12.h just to ask the question.
                        const GUID kID3D12Device = { 0x189819f1, 0x1db6, 0x4b57,
                            { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
                        is12 = SUCCEEDED(sc1->GetDevice(kID3D12Device,
                                                        reinterpret_cast<void**>(&dev12))) && dev12;
                        if (dev12) dev12->Release();
                        sc1->Release();
                    }
                    SKSE::log::info("[ICONDIAG] swapchain device is {} -- {}",
                        is12 ? "D3D12" : "D3D11",
                        is12 ? "PROXY ACTIVE: backbuffer capture is not reliable here"
                             : "native, capture path is valid");
                }
            }
        }

        // Step 1 (save): stash the WHOLE backbuffer for restoration — the
        // model draw is not confined to the capture rect.
        context->CopyResource(m_scratchTex, srcTex);

        // Step 2 (clear): paint a solid background into that rect so the
        // capture catches model + background.
        {
            ID3D11DeviceContext1* ctx1 = nullptr;
            if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                    reinterpret_cast<void**>(&ctx1))) && ctx1) {
                D3D11_RECT rect = { left, top, left + width, top + height };
                // GI77: pass A is always black -- for an item it is one half of
                // the matte, for a spell it is the whole capture.
                ctx1->ClearView(rtv, kMatteBlack, &rect, 1);
                ctx1->Release();
            }
        }

        // ★DIAG: read the rect back BEFORE the model is drawn. The whole alpha
        // pipeline assumes this rect is now PURE magenta -- if an upscaler (or
        // anything else hooked into the frame) has already touched it, every
        // later stage is working on a lie and the chroma key produces the
        // reported speckle. Measuring after the model can never tell the two
        // apart: model pixels and polluted pixels both read as "not background".
#ifdef GI_CAPTURE_DIAG
        ProbeRect(context, srcTex, box, width, height, "after-clear ");
#endif

        // Step 3 (render): the engine paints the model into the bound surface.
        // Any overspill beyond the capture rect is erased by the full-frame
        // restore in Step 5, so nothing is ever visible on screen.
        // ★DO NOT SET THE SCENE AMBIENT HERE -- measured, it reaches nothing
        // this late in the frame. See the note above PlaceLight.
        inv->Render();

        // Step 4 (capture): copy backbuffer rect → top-left of our texture.
        context->CopySubresourceRegion(m_dstTex, 0, 0, 0, 0, srcTex, 0, &box);
        // ★★GI77: PASS B, the same model over WHITE, into the second texture.
        // The scene has not moved between the two draws -- same frame, same
        // rig, same rotation -- so the only thing that differs is what shows
        // through, and that difference is read back as the alpha. Skipped for
        // a spell (see kMatteWhite): an additive glow over white saturates.
        if (!m_currentIsSpell) {
            ID3D11DeviceContext1* ctx1 = nullptr;
            if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                    reinterpret_cast<void**>(&ctx1))) && ctx1) {
                D3D11_RECT rect = { left, top, left + width, top + height };
                ctx1->ClearView(rtv, kMatteWhite, &rect, 1);
                ctx1->Release();
            }
            inv->Render();
            context->CopySubresourceRegion(m_dstTexB, 0, 0, 0, 0, srcTex, 0, &box);
        }

        // Diagnostic probe (first few captures with a loaded model): read the
        // captured rect back and count pixels that differ from the painted
        // background — proves whether inv->Render() actually drew anything.
        // Compiled out by default: a GPU readback stall on the render thread.
#ifdef GI_CAPTURE_DIAG
        ProbeRect(context, srcTex, box, width, height, "after-model");
#endif

        // Step 5 (restore): write the whole saved frame back — erases the
        // clear rect AND every model pixel, including overspill beyond the
        // capture rect, so nothing is ever visible on screen.
        context->CopyResource(srcTex, m_scratchTex);

        srcTex->Release();

        m_lastCapturedSize = ImVec2(static_cast<float>(width), static_cast<float>(height));

        // Where the model's projected centre lands inside the texture
        const float model_cx = m_capturePos.x + m_captureSize.x * 0.5f;
        const float model_cy = m_capturePos.y + m_captureSize.y * 0.5f;
        m_modelInTexture = ImVec2(model_cx - static_cast<float>(left),
                                  model_cy - static_cast<float>(top));

        if (m_hasOverrideOffset) {
            m_modelInTexture.x -= m_overrideOffsetX;
            m_modelInTexture.y -= m_overrideOffsetY;
        }

        ++m_captureStamp;
    }
}
