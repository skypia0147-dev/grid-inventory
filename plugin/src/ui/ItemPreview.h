#pragma once

#include "ui/ItemDef.h"

#include <imgui.h>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11Device;
struct D3D11_TEXTURE2D_DESC;

namespace FUI
{
    // IconDef (capture orientation: rx/ry/rz/scale) is an alias of the ONE
    // shared FUI::ItemDef (Phase 2 D2) — see ui/ItemDef.h.

    // Ported from ModExplorerMenu (Modex) by patchulidev — Item3DPreview.
    // https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
    //
    // Captures the engine's own Inventory3DManager render (the vanilla
    // inventory preview pipeline) off the backbuffer during the UI render
    // pass, and exposes it to ImGui as a shader resource view. This is the
    // only path that renders skinned meshes (bows, spell tome pages).
    class ItemPreview
    {
    public:
        static constexpr unsigned int kTexSize = 2048;

        // Margin applied to prevent clipping capture bounds
        static constexpr float kSafetyMargin = 1.5f;

        // Fallback model scale when Request() is passed a negative scale
        static constexpr float kDefaultModelScale = 0.85f;

        // Background painted behind the model before capture, so tall/wide
        // icons can overflow their footprint without covering neighbouring
        // cells.
        //
        // ★★★THE ALPHA IS THE POINT; the magenta is only a fallback tint.
        //
        // This used to be alpha 1.0, and IconCache found transparency by
        // looking for magenta. That works until an item has an alpha texture:
        // a half-transparent pixel comes back blended with the backdrop, so it
        // is neither the model's colour nor pure magenta, and it was kept as
        // an opaque purple sheet (reported -- a bridal veil).
        //
        // Clearing alpha to 0 instead let us ask whether the engine writes
        // real alpha. It does:
        //
        //   after-clear   alpha 0=100.0%  mid=0.0%  255=0.0%
        //   after-model   alpha 0=94.1%   mid=5.3%  255=0.5%
        //
        // So IconCache reads transparency rather than guessing at it. The RGB
        // stays magenta because a blended pixel still carries the backdrop's
        // cast, and min(R,B)-G is exactly how much of it to remove -- being
        // symmetric in R/B, that arithmetic never has to care whether the
        // surface is BGRA or RGBA.
        // ★★★GI77: TWO BACKDROPS, AND ALPHA IS NEVER READ AGAIN.
        //
        // One backdrop cannot tell a half-transparent pixel from an opaque
        // pixel of the backdrop's colour, and every heuristic that followed --
        // the magenta spill, the hides rule, the colour-key fallback, the
        // one-pixel reach -- was an attempt to guess which it was. Two
        // backdrops make it arithmetic. The same model is drawn once over
        // black and once over white; where the two agree the pixel is opaque,
        // where they differ by the whole backdrop it is transparent, and the
        // difference in between IS the transparency. The colour comes straight
        // out of the black pass. No surface alpha is consulted, so a 10-bit
        // surface with two alpha bits and a surface that hands back none at
        // all get exactly the same result as a perfect one.
        //
        // A SPELL takes the black pass only: an additive glow over white
        // saturates instead of blending, so its alpha is its brightness (see
        // IconCache's sprite pass), which the black pass alone provides.
        static constexpr float kMatteBlack[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr float kMatteWhite[4] = { 1.0f, 1.0f, 1.0f, 0.0f };

        static ItemPreview* GetSingleton();

        void Begin();
        void End();
        void Render();

        // ★GI74b: a_spell is TOLD, not derived. The object handed in is the
        // spell's DISPLAY model (CaptureSourceOf), which is a plain static and
        // will never answer As<SpellItem>() -- so deriving it here said "not a
        // spell", kept the magenta backdrop, and the brightness-alpha pass on
        // the other side then made that magenta solid: a ring of pink squares
        // (zhenguoce, second screenshot). The caller knows; it says so.
        void Request(RE::TESBoundObject* a_item, ImVec2 a_screenPos, ImVec2 a_screenSize,
                     float a_modelScale = -1.0f, float a_offsetX = 0.0f, float a_offsetY = 0.0f,
                     const IconDef* a_def = nullptr, bool a_spell = false);

        // Called from the game-update hook (BEFORE the frame renders): applies
        // the def orientation and parks the model as soon as it lands, so the
        // engine's own on-screen draw never shows it at the default position.
        void Tick();

        ImVec2 GetCapturedSize() const { return m_lastCapturedSize; }

        // Full safety-margin region around the model centre (kSafetyMargin x
        // the inner box). IconCache stores THIS so content that outgrows the
        // inner box (rotation diagonals) never bakes in clipped; the tile
        // draw compensates by kSafetyMargin.
        void GetMarginUV(ImVec2& a_uv0, ImVec2& a_uv1) const;

        bool IsRunning() const { return m_running; }

        // IconCache support: raw capture texture and a monotonically
        // increasing stamp (bumped on every completed capture).
        ID3D11Texture2D*    GetTexture() const { return m_dstTex; }    // black pass
        ID3D11Texture2D*    GetTextureB() const { return m_dstTexB; }  // white pass (GI77)
        std::uint32_t       GetCaptureStamp() const { return m_captureStamp; }

        // The loadedModels entry matching m_current (async loads land late, so
        // back() can be a stale previous item). Null until render-ready.
        RE::NiAVObject* FindCurrentModel() const;

        // True when the model node's rotation matches the requested def.
        // On the landing frame the engine stomps the node with its own
        // default pose AFTER our UpdateParking ran — a capture accepted that
        // frame bakes the wrong orientation into the cache permanently.
        bool RotationApplied() const;

        // GI68: is the engine still working on a model load? Separates "slow"
        // from "never took" without guessing at the player's disk speed --
        // a load that IS in flight deserves another pass later; one that never
        // took (no task, no entry) will not improve with more time.
        [[nodiscard]] bool LoadPending() const;

        void UnloadCurrent();  // unload AFTER the model landed (actually removes)
        // End3D+Begin3D recovery when loadedModels fills up (7 cap). Returns
        // false when DEFERRED: End3D derefs every entry's spModel, so a
        // not-yet-landed entry (null spModel, async load in flight) is a
        // guaranteed engine CTD — callers must retry on a later frame.
        bool ResetScene();

        // The engine ALSO draws loadedModels on screen every frame (that draw
        // IS the vanilla right-pane preview). Park the model so it projects
        // at this screen point — put it under an opaque UI window to hide it.
        void SetParkPos(ImVec2 a_screen) { m_parkPos = a_screen; m_hasPark = true; }

        // Caching-card support (translucent skins can't hide the parked model
        // behind a window): how big the capture region is, and whether ANY
        // engine model is currently on screen.
        ImVec2 CaptureCover() const { return m_captureSize; }
        int    SceneModelCount() const;

        // Clipped-capture retry: some records render far larger than the
        // standard box (engine-side per-item scale invisible on the node).
        // The capture consumer detects content touching the box edge and
        // requests a bigger box for the next attempt. Resets on item change.
        void BoostCapture(float a_px) { m_captureBoost = (std::max)(m_captureBoost, a_px); }

        // INSPECT mode (C key): the sprite is drawn several times larger than a
        // tile, so the engine must render the model bigger or the capture has
        // too few pixels to read fine detail (dragon-claw glyphs). Multiplier
        // on the engine's own item scale, re-applied every frame because the
        // engine recomputes it; 0 restores the saved value.
        void SetInspectScale(float a_mul) { m_inspectScale = a_mul; }

        // ★★1.0.5: where the capture lamp sits for the item being captured,
        // as an OFFSET in degrees from the default rig. The scene has one
        // light, so an item's brightness is decided by which face it turns
        // toward it; this lets a def move the lamp instead of the item.
        // Re-applied every frame while parked (the engine owns the scene and
        // may reassert the scheme), and restored in End().
        // ★The caller passes the TOTAL offset (global setting + item def) —
        // see IconCache::CaptureLightFor. A changed global therefore arrives
        // here as a changed value and re-arms the park gate, which is what
        // makes moving the global slider re-photograph rather than re-use.
        void SetLightOffset(float a_azDeg, float a_elDeg);

        // ★★Has THIS request's model been parked at least one full frame?
        // RotationApplied() was doing this job by proxy and got it wrong: it
        // asks "does the node carry the requested rotation", which a DIFFERENT
        // item's leftover model answers YES to whenever the two share a
        // rotation. Category defaults make that common — armor_head and
        // armor_body_heavy are both rx:90 — so a body could be accepted while
        // the helmet's model was still on screen, one frame before its own
        // orientation and light were applied. Weapons and bags have distinct
        // angles, which is the only reason they never showed it.
        [[nodiscard]] bool ParkSettled() const;
        // ★The raw tick count behind ParkSettled, for the timeout diagnostic.
        // A capture that times out with a loaded, rotated model has only two
        // suspects left -- this and the capture stamp -- and the log printed
        // NEITHER, which is why a user's "nothing ever caches" report could not
        // be told apart from a slow disk.
        [[nodiscard]] int ParkTicks() const;

    private:
        ItemPreview() = default;

        bool Initialize();
        void Shutdown();
        // ★The ONE place a model is handed to the engine — both call sites
        // (fresh request, self-heal reload) go through it so the loader choice
        // stays in a single spot.
        void LoadForCapture(RE::Inventory3DManager* a_mgr, RE::TESBoundObject* a_item);

        void UpdateParking();   // apply def + move model under the park point
        // Unload + End3D, once, if the scene is safe to take apart. Returns
        // nullptr on success, else the reason it was left standing.
        const char* AttemptTeardown(RE::Inventory3DManager* a_mgr);
        // ★GI97: the engine scene teardown, retried ONE TRY PER FRAME from
        // Tick() until the scene settles or the frame budget runs out. It used
        // to retry by reposting an SKSE task, which drains in the same pass and
        // so waited for nothing at all -- see the note on the definition.
        // Abandoned when the session no longer matches (the menu reopened; the
        // new session pairs its own End3D).
        void TeardownTick();
        // ★Put the capture zoom back on the node before letting go of it.
        // Every path that unloads MUST call this first — see m_scaledNode.
        void RestoreNodeScale();

        bool                m_initialized = false;
        bool                m_running     = false;
        bool                m_requested   = false;
        RE::TESBoundObject* m_current     = nullptr;
        // ★GI74: decided ONCE, where m_current is assigned and the object is
        // known live -- never re-derived from the pointer at Render time. The
        // engine's list can hold a dead form (see FindCurrentModel's note), and
        // the backdrop choice must not be the thing that dereferences it.
        bool                m_currentIsSpell = false;
        std::uint32_t       m_session     = 0;   // bumped by Begin(); guards deferred teardown
        // ★★GI73: is a Begin3D OUTSTANDING? A different question from m_running,
        // and the distance between the two is the bug: m_running is our own
        // preview state and is cleared the instant the menu hides, while the
        // engine scene lives until a teardown actually runs -- and a teardown
        // can be deferred or refused outright. Begin() reads THIS, so a scene
        // still standing is adopted instead of having a second one stacked on
        // it. See Begin() for the report that found it.
        bool                m_scene3D     = false;
        // ★GI97: the deferred teardown, waiting on real frames. m_teardownFrames
        // counts frames waited, not tasks reposted, which is the whole point.
        bool                m_teardownPending = false;
        std::uint32_t       m_teardownSession = 0;
        int                 m_teardownFrames  = 0;
        // ★★GI98: the scene still standing was GIVEN UP ON, not merely waited
        // for. A deferred teardown leaves a whole scene the next open may adopt;
        // a refused one leaves the very scene we could not take apart, and
        // adopting THAT is how the 2026-09-13 CTD was reached. Begin() tells the
        // two apart on this flag: a refused scene is retried once and, failing
        // that, left alone with no 3D for the open rather than built upon.
        bool                m_sceneRefused    = false;

        ImVec2 m_capturePos       = ImVec2(0.0f, 0.0f);
        ImVec2 m_captureSize      = ImVec2(0.0f, 0.0f);  // full rect including safety margin
        ImVec2 m_innerSize        = ImVec2(0.0f, 0.0f);  // requested box (no margin) — capture size derives from it
        ImVec2 m_lastCapturedSize = ImVec2(0.0f, 0.0f);  // actual captured pixels after backbuffer clamp
        ImVec2 m_modelInTexture   = ImVec2(0.0f, 0.0f);  // model's centre position in captured-texture pixels

        // Per-request offset overrides applied at Render
        float m_overrideOffsetX   = 0.0f;
        float m_overrideOffsetY   = 0.0f;
        bool  m_hasOverrideOffset = false;

        ImVec2  m_parkPos = ImVec2(0.0f, 0.0f);
        bool    m_hasPark = false;
        // GI68: consecutive self-heal repairs, for log throttling only.
        int     m_healRun = 0;
        float   m_inspectScale  = 0.0f;   // 0 = off
        float   m_savedItemScale = 0.0f;  // engine value to restore
        bool    m_hasSavedScale = false;
        float   m_savedNodeScale = 1.0f;  // model node scale to restore
        // ★★The node we scaled, by IDENTITY — not a bool. A bool says "some
        // node is scaled", which is the wrong question after an unload: the
        // engine DEDUPES loads of the same nif and can hand the SAME node
        // back, still carrying our 2.5x. Clearing a flag then re-saved that
        // 2.5x as the base and multiplied again — 1068 -> 2670 -> 6675 ->
        // 16689 -> 41723 radius in one EDIT rotation drag (measured). The
        // pointer answers "did I already scale THIS node", which no unload
        // timing can get wrong.
        RE::NiAVObject* m_scaledNode = nullptr;
        float   m_captureBoost = 0.0f;   // min capture-box side requested by BoostCapture
        float   m_lightAz = 0.0f;        // total lamp offset in effect (global + def)
        float   m_lightEl = 0.0f;
        // ★How many times UpdateParking has applied THIS request's orientation
        // and light. 0 = never, 1 = applied but the frame it applied to has not
        // been rendered yet, 2+ = settled.
        // ★NOT a frame number: ImGui's counter advances inside Render, while
        // parking happens in the game-update Tick, so "current frame > parked
        // frame" could compare two readings taken from the same count and never
        // become true — which stalled every capture until it timed out.
        // Counting our own applications has no such ambiguity.
        int     m_parkTicks = 0;
        IconDef m_def;   // rotation only — scale is a crop-region zoom (Request param)

        ID3D11Texture2D*          m_dstTex     = nullptr;
        ID3D11ShaderResourceView* m_dstSRV     = nullptr;
        ID3D11Texture2D*          m_scratchTex = nullptr;
        ID3D11Texture2D*          m_dstTexB    = nullptr;   // GI77: the white pass
        // ★★What the capture textures were built for. renderWindows[0] is NOT
        // reliably the surface the engine draws into: with a D3D12 swap chain
        // (CS Upscaling) it is a different resource in a different format —
        // measured 0x..2060 fmt=24 (R10G10B10A2) against a bound 0x..12a0
        // fmt=28 (R8G8B8A8). A copy between those two formats fails outright,
        // so the textures have to be rebuilt for whatever is actually bound.
        // stored as raw ints so this header needs no d3d11/dxgi include
        std::uint32_t             m_texFormat  = 0;   // DXGI_FORMAT_UNKNOWN
        std::uint32_t             m_texW       = 0;
        std::uint32_t             m_texH       = 0;
        // Build/rebuild m_dstTex + m_dstSRV + m_scratchTex for this surface.
        // Cheap no-op when the description already matches.
        bool EnsureCaptureTextures(ID3D11Device* a_device,
                                   const D3D11_TEXTURE2D_DESC& a_src);
        std::uint32_t             m_captureStamp = 0;
    };
}
