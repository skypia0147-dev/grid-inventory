#pragma once

#include "ui/ItemDef.h"

#include <atomic>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace FUI
{
    // B-2: per-item icon cache (PLAN_B §2-G4). Each queued item is run once
    // through ItemPreview's backbuffer capture; the model-centred crop is
    // copied into an item-owned texture whose SRV lives for the process
    // lifetime, so re-opening the inventory shows icons instantly with no
    // further engine renders.
    class IconCache
    {
    public:
        struct Icon
        {
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11Texture2D*          tex = nullptr;
            int                       w = 0;
            int                       h = 0;
            // ★1.0.5: ALWAYS NULL / ZERO. These held a blurred silhouette of
            // the icon — first the rarity halo, then the drop shadow — baked
            // onto a 96px canvas. Nothing builds one any more: the shadow
            // stamps the icon's own sprite instead, at full resolution (see
            // Grid::DrawItemShadow). The fields stay because every reader was
            // already null-checking them and the release paths no-op on null,
            // so leaving them costs 5 words per icon and churns nothing.
            ID3D11ShaderResourceView* glowSrv = nullptr;
            ID3D11Texture2D*          glowTex = nullptr;
            int                       gw = 0;     // glow canvas incl. padding
            int                       gh = 0;
            int                       gpad = 0;   // margin baked on each side
            // ---- eviction bookkeeping (m_icons only; Fallback ignores these)
            int                       lastUsed = 0;   // cache tick Get() saw it
            std::uint32_t             bytes = 0;      // VRAM incl. the mip tail
        };

        static IconCache* GetSingleton();

        // Generic .fic (raw RGBA) loader — also used for slot silhouettes.
        // a_exactMagic 0 accepts any "FIC?" version (version-agnostic assets);
        // the icon cache passes its exact magic so stale captures re-render.
        // a_makeGlow additionally builds the silhouette halo sprite (item
        // icons only — slot silhouettes never glow).
        static bool LoadFicTexture(const std::string& a_path, Icon& a_out,
                                   std::uint32_t a_exactMagic = 0,
                                   bool a_makeGlow = false);

        // PNG loader (WIC) — the drawn icon set's format. Anything a player
        // drops into the fallback folder comes through here, so it must fail
        // QUIETLY on a missing or malformed file: the caller treats "no" as
        // "this key has no drawing", which is a normal answer.
        // ★a_meanRgb: optional out, THREE floats, 0..255, alpha-weighted. A
        // caller that has to reach a stated colour needs to know what the file
        // already is -- a tint multiplies, so "make this sheet average #E1D7C2"
        // is only answerable against the sheet's own average. Weighted by
        // alpha because a cut-out's transparent margin is not part of it.
        static bool LoadPngTexture(const std::string& a_path, Icon& a_out,
                                   bool a_makeGlow = false,
                                   float* a_meanRgb = nullptr);

        // main.cpp installs this: item -> capture orientation (category preset
        // + user ini override). The def is part of the cache key, so editing a
        // def naturally triggers a re-capture.
        using DefResolver = std::function<IconDef(RE::TESBoundObject*)>;
        void SetDefResolver(DefResolver a_resolver) { m_resolver = std::move(a_resolver); }
        [[nodiscard]] IconDef ResolveDef(RE::TESBoundObject* a_obj) const;

        // Enqueue an item for capture (no-op if cached or already queued).
        void QueueCapture(RE::TESBoundObject* a_obj);

        // ★Drop the not-yet-started backlog. For settings that re-key EVERY
        // item at once (the global capture light) — without it a slider drag
        // leaves thousands of queued captures at angles nobody asked for.
        void PurgeQueue();


        // Prefetch (B): queue a capture WITHOUT touching the GPU — items
        // already in the disk pak are skipped by index (their texture uploads
        // lazily when a grid actually draws them). a_evictAfter (mass
        // precache) keeps only the pak copy after capturing: VRAM stays flat.
        void Prefetch(RE::TESBoundObject* a_obj, bool a_evictAfter = false);

        // Precache (C): queue every nameable inventory form in the load
        // order (weapons/armor/potions/books/misc/...). Returns queued count.
        size_t PrecacheAll();
        void   CancelPrecache();   // drop the queue (current capture finishes)

        // ---- GI68: the DEFERRED list ---------------------------------------
        // An item whose model was still loading when its window closed is not a
        // failure -- it just needed more time than the first pass can give it.
        // ★The first pass must not wait for it: captures run one per frame on a
        // single engine scene, so every frame spent waiting is a frame no OTHER
        // item can use (three stragglers once ate 6s of a 6.5s first render).
        // So it is set aside instead, and time is granted later, in a pass where
        // holding the queue costs nobody anything.
        //
        // Persisted, or the same items would burn their window again every
        // session and land right back here.
        [[nodiscard]] size_t DeferredCount();
        size_t RetryDeferred();    // queue them with a generous window
        void   ClearDeferred();    // give up on them (they become plain misses)

        // Editor pin: keep this item's model loaded between captures so a
        // rotation edit re-captures every frame (live editing). While pinned,
        // captures replace a single in-memory slot and skip the disk write —
        // the FINAL sprite is flushed to disk when the pin moves/clears.
        void SetPin(RE::TESBoundObject* a_obj);

        // ---- VRAM budget -----------------------------------------------------
        // ★★A sprite in the pak costs nothing to get back -- one file read --
        // so holding every icon a session ever drew is a habit, not a need.
        // Without a ceiling the cache only grows: what is in VRAM at any moment
        // is "every item seen since launch", and that number has no end.
        // ★The budget does NOTHING in ordinary play. A full grid is ~77 MB and
        // a measured session sat at 49 MB; this is a roof, not a policy.
        // ★Only entries the pak can restore are ever dropped. A pinned item's
        // captures are deliberately kept off disk, so dropping one would lose
        // it -- see TrimToBudget.
        static constexpr std::uint64_t kVramBudget = 256ull << 20;
        // Uploads per frame while re-filling. Each one is a read plus a CPU
        // mip chain, and a screenful at once is a visible hitch.
        static constexpr int kRefillPerFrame = 8;
        void TrimToBudget();                  // call OUTSIDE the ImGui frame
        [[nodiscard]] std::uint64_t VramBytes() const;
        // ★Age is counted in TICKS, not ImGui frames. ImGui only runs a frame
        // while one of our menus is up, so its counter STOPS the moment the
        // inventory closes -- and then every icon looks freshly used forever
        // and nothing is ever trimmed, which is precisely when trimming is
        // wanted. This one is advanced from the game tick, so it keeps going
        // with nothing on screen.
        // ★atomic: written on the game thread, read from the render pass. A
        // torn read would only mis-age one icon by one tick, but the type
        // costs nothing and says which threads meet here.
        mutable std::atomic<int> m_tick{ 0 };

        // ---- post-load warm-up (first-open latency) ------------------------
        // Queue forms whose PAK sprite should be made resident before the
        // first menu open. Serviced from TrimToBudget -- the once-a-tick spot
        // that already owns m_icons outside the draw -- at a deliberately
        // gentle rate: a short grace after the load (the load spike is the
        // worst moment to add I/O on a slow machine), then at most
        // kWarmPerTick pak restores per tick. Pak reads ONLY, never a
        // capture: warm-up must be safe with no menu and no engine scene.
        // Forms are kept as FormIDs (원칙 2) and re-resolved at service time.
        void QueueWarm(std::vector<RE::FormID> a_forms);
        void SetWarmEnabled(bool a_on) { m_warmEnabled = a_on; }
        [[nodiscard]] bool WarmEnabled() const { return m_warmEnabled; }
        // Widen the refill allowance for the next a_frames ticks. Called at
        // menu open: a screenful of pak sprites lands during the open
        // transition (already a covered moment) instead of trickling in at
        // kRefillPerFrame and reading as pop-in.
        void Burst(int a_frames) { m_burstFrames = (std::max)(m_burstFrames, a_frames); }

        [[nodiscard]] bool        IsBusy() const { return m_pendingBusy || !m_queue.empty(); }
        [[nodiscard]] const Icon* Get(RE::TESBoundObject* a_obj) const;   // resolves def internally
        [[nodiscard]] size_t      CachedCount() const { return m_icons.size(); }
        [[nodiscard]] size_t      QueuedCount() const { return m_queue.size() + (m_pendingBusy ? 1 : 0); }

        // Per-frame hooks, called from GridInventoryMenu::PostDisplay:
        void PreRender();   // BEFORE ItemPreview::Render — request the pending item
        void PostRender();  // AFTER  ItemPreview::Render — harvest the capture

        void Clear();  // release all cached textures

        // Save-load boundary. Clear() is far too heavy here (it would drop every
        // texture and trigger a re-capture storm on each load); the ONLY thing a
        // revert invalidates is work still queued against forms the engine is
        // about to destroy and re-create. Cached textures are keyed by model
        // slot, so they stay valid across the load.
        void OnRevert();

        // ICON CACHE reset (settings): drop every texture AND the disk pak —
        // for users who install retexture mods after icons were captured.
        // Call OUTSIDE the ImGui frame only (UIRoot::Tick): the current draw
        // list may still reference the SRVs being released.
        // The retired stylized derivative is swept here too (GI60).
        void ResetDiskCache();

        // GI47: preset icon bundle. Export copies our capture pak next to the
        // preset ini; import APPENDS the bundle's records behind our own --
        // the scanner's last-one-wins rule then gives the preset every shared
        // key while reader-only keys (extra mods) survive untouched.
        // MergePak drops the live textures so tiles reload lazily from the
        // merged pak: NEVER call it inside the ImGui frame (UIRoot::Tick only).
        bool ExportPakTo(const char* a_path);
        bool MergePak(const char* a_path);

        // Icon style. REALISTIC = auto-captured from the game's own 3D models.
        // DRAWN (GI52) = the hand-drawn category set on its own: it captures
        // nothing at all, so a heavily modded load order is complete the moment
        // the menu opens instead of after a long scan. (The C-key 3D view still
        // works there: that path never goes through the icon queue.)
        //
        // Value 1 was a third style, "stylized" — a filter derived from the
        // captures. Retired in GI60; the drawn set covers the same want with
        // authored art and no scan. The NUMBER stays skipped so an old ini
        // reading 1 can be mapped to realistic rather than silently meaning
        // something else.
        enum class Style : std::uint8_t
        {
            kRealistic = 0,
            kFlat      = 2,
            // ★PIXEL is DERIVED from the realistic capture at runtime, not a
            // second art set: the sprite is quantised to a fixed palette on a
            // 24-dot-per-cell grid and given a one-dot outline. That is what
            // makes it cover mod items for free — anything the engine can
            // capture, this style can draw — and why it ships no assets.
            kPixel     = 3,
        };
        void SetStyle(Style a_style);
        [[nodiscard]] Style GetStyle() const { return m_style; }
        [[nodiscard]] bool  FlatStyle() const { return m_style == Style::kFlat; }

        // Pixel derivation runs on a frame budget (see kPixelPerFrame): until
        // an item's dot version exists the grid keeps drawing the realistic
        // one, so switching styles reads as a sweep rather than a blank grid.
        void TickPixelDerive();
        // ★Deferred texture drop. Leaving pixel style frees its derived
        // SRVs, and that MUST NOT happen inside the ImGui frame — the draw
        // list already holds those exact views. Same rule ResetDiskCache
        // spells out one screen down; the style switch just never got it.
        bool m_pixelDropPending = false;

        // INSPECT mode (C key = vanilla "Item Zoom"): one item is captured at
        // a mouse-driven rotation and drawn large, so the player can read model
        // detail the icon can't show (dragon-claw glyphs).
        //
        // ★The capture is taken when the ROTATION CHANGES, not every frame.
        // Re-arming unconditionally is what turned 120fps into 30 while the
        // model just sat there: a 900px model re-rendered by the engine and
        // re-uploaded as a mipped texture, at full framerate, showing a picture
        // identical to the one already on screen. Zoom does not re-capture
        // either -- it scales the sprite that is already there.
        //
        // FULLY SEPARATE from the icon cache: the capture lands in its own
        // texture slot, keyed by nothing, never persisted, and the item's
        // m_icons entry is never read, written or dropped. An inspect
        // therefore CANNOT change or lose the tile's icon — the earlier
        // rotation-key sharing is what caused exactly that class of bug.
        void SetInspect(RE::TESBoundObject* a_obj, float a_rx, float a_ry, float a_rz);
        void SetInspectRot(float a_rx, float a_ry, float a_rz);
        void ClearInspect();
        [[nodiscard]] const Icon* InspectIcon() const
        {
            return m_inspectValid ? &m_inspectIcon : nullptr;
        }
        // Frees the retired inspect texture. Releases an SRV -> call from
        // UIRoot::Tick (outside the ImGui frame) only: ClearInspect can run
        // mid-frame, when this frame's draw list still references it.
        void ProcessDeferredRelease();

    private:
        IconCache() = default;

        // Phase 3: PostRender's front half — timeouts / soft-skip / capture &
        // model / rotation gates. kReady = harvest the capture this frame.
        enum class GateResult : std::uint8_t
        {
            kNotReady,    // wait: try again next frame (pending stays busy)
            kAbandoned,   // gave up / requeued: pending released
            kReady,
        };
        GateResult CheckPendingGates();
        // ★The four acceptance gates on their own, so CheckPendingGates can ask
        // "is it ready" BEFORE "has it waited too long". Asking in the other
        // order discarded captures that completed on the deadline frame.
        [[nodiscard]] bool CaptureAccepted() const;
        // Which gate rejected, as a word; nullptr when all four are open.
        // Exists for the per-frame trace — see the note at the implementation.
        [[nodiscard]] const char* CaptureRejectReason() const;

        // giveUp MECHANISM (callers own the policy of when): warn, unload,
        // release the pending slot, and escalate repeat offenders to the
        // PERSISTED permanent-fail list.
        void GiveUpPending(const char* a_why);

        // ★The RESOLUTION lever is the model scale, not the box. The engine
        // renders the preview item at a size it chooses itself (~275px
        // regardless of the request box — measured; growing the box only
        // captured more empty margin, which the alpha-trim then removed, so
        // "bigger box" changed nothing). Rendering SMALL is also what made
        // items look glossy: at ~275px the GPU samples the low mips of the
        // diffuse AND the normal map, and averaged normals read as a smooth,
        // shiny surface. So captures push the same itemScale/node-scale pair
        // the inspect zoom already uses (save/restore machinery included) and
        // the box merely follows the enlarged model.
        static constexpr float kIconCaptureScale = 2.5f;
        static constexpr float kIconRequestSize  = 320.0f;   // box floor, x scale at request
        // inspect: same lever, larger — its overlay draws into ~62% of screen
        // height (890px at 1440p), so the model needs those pixels for real.
        // Smaller displays clamp the box at capture time (height / margin).
        static constexpr float kInspectModelScale  = 3.0f;
        static constexpr float kInspectRequestSize = 900.0f;

        static void ReleaseIcon(Icon& a_icon);   // free SRV/tex (+ glow)
        static constexpr int   kTimeoutFrames   = 180;

        // Cache key = MODEL HASH + quantised rotation hash: items sharing one
        // GND nif (enchant variants, keys, notes — 80% of all records) share
        // ONE capture. Items with alternate textures keep per-form keys.
        [[nodiscard]] std::uint64_t KeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const;
        // pre-model-key pak entries (FormID-based) stay readable via this
        [[nodiscard]] std::uint64_t LegacyKeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const;

        // Disk cache: an item is captured once EVER (per def); later sessions
        // load the pixels straight from disk — no engine renders at all.
        // const because Get() calls it: m_icons is mutable and nothing else
        // is touched, so filling a miss is not a change the caller can see.
        bool LoadFromDisk(std::uint64_t a_key) const;
        mutable int m_refillLeft = kRefillPerFrame;   // reset each TrimToBudget

        // warm-up state (see QueueWarm)
        std::deque<RE::FormID> m_warmQueue;
        int                    m_warmDelay = 0;
        int                    m_burstFrames = 0;
        bool                   m_warmEnabled = true;
        // ~3s at 60fps. TICKS, not seconds, on purpose: a slower machine
        // ticks slower, so the machines the grace protects wait LONGER in
        // real time (30fps = ~6s) while a fast one starts sooner -- the
        // adaptation comes free. Was 5s nominal; measured, the player can
        // reach the inventory in ~3.3s after a load, and the warm work is
        // two small reads a tick, so meeting them earlier costs nothing.
        static constexpr int kWarmDelayTicks = 180;
        static constexpr int kWarmPerTick    = 2;     // pak restores per tick
        static constexpr int kBurstRefill    = 128;   // open-transition allowance
        static void SaveToDisk(std::uint64_t a_key, int a_w, int a_h, std::uint32_t a_fmt,
                               const std::vector<std::uint8_t>& a_pixels);

        struct Pending
        {
            RE::TESBoundObject* obj = nullptr;
            // The queue outlives a menu close, and a RUNTIME-CREATED form (a
            // brewed potion, a player enchantment) is destroyed and re-made with
            // a new FormID when a save loads -- so `obj` can go stale while an
            // entry waits its turn. Keep the id to re-validate before any use.
            RE::FormID          id = 0;
            std::uint64_t       key = 0;
            bool                evict = false;   // precache: pak-only, free the GPU copy
            float               boost = 0.0f;    // B4: clip-boost carried across requeues
        };

        // ★Second rung of the anti-clipping ladder. The first grows the BOX,
        // and it has a hard ceiling: the capture reads backbuffer pixels, so
        // no box can exceed the screen. A model that still overflows there
        // used to be baked clipped — which the realistic sprite shows as a
        // sliced silhouette and the pixel style turns into a literal frame
        // (its 1-dot outline traces the straight cut). When the box cannot
        // grow, shrink the MODEL instead: fewer pixels, but real ones.
        float m_captureShrink = 1.0f;
        static constexpr float kMinCaptureShrink = 0.4f;   // 2.5x -> 1.0x floor

        // Pixel style: derived sprites, keyed exactly like m_icons. Memory
        // only — re-deriving costs a pak read plus a downscale, which is
        // cheaper than owning a second pak file and keeping it in sync.
        static constexpr int kPixelDots      = 24;   // dots per grid cell
        static constexpr int kPixelPerFrame  = 4;    // derivations per frame
        // mutable: Get() is const and only RECORDS a request here. The queue
        // is lazy-evaluation bookkeeping, not observable state — what the
        // caller sees is unchanged whether or not the derivation has run.
        std::unordered_map<std::uint64_t, Icon>       m_pixelIcons;
        mutable std::deque<std::uint64_t>             m_pixelQueue;
        mutable std::unordered_set<std::uint64_t>     m_pixelQueued;
        mutable std::unordered_map<std::uint64_t, std::pair<int, int>> m_pixelCells;
        bool DerivePixelIcon(std::uint64_t a_key, int a_cw, int a_ch);
        void ReleasePixelIcons();

        DefResolver                            m_resolver;
        // ★mutable, for the same reason the pixel bookkeeping above is: Get()
        // is const and now FILLS a miss from the pak on the spot rather than
        // reporting one. Same answer either way from the caller's side -- the
        // sprite was always going to arrive, this only decides whether it
        // arrives this frame or the next.
        mutable std::unordered_map<std::uint64_t, Icon> m_icons;
        Style                                   m_style = Style::kRealistic;
        std::deque<Pending>                    m_queue;
        std::unordered_set<std::uint64_t>      m_queued;    // membership for m_queue
        // permanently skipped this session: items whose capture keeps failing
        // (e.g. mod items with no inventory model) — without this the visible
        // grid re-queues them every frame and the caching spinner never ends
        std::unordered_set<std::uint64_t>      m_failed;
        bool                                   m_failLoaded = false;
        // GI68: ran out of window while the engine was STILL LOADING. Not a
        // failure -- a candidate for a later, unhurried pass. Kept apart from
        // m_failed so a retry can target exactly these and nothing else.
        std::unordered_set<std::uint64_t>      m_deferred;
        std::unordered_map<std::uint64_t, RE::TESBoundObject*> m_deferredObj;
        bool                                   m_slowLoaded = false;
        bool                                   m_retryPass = false;   // generous window

        void EnsureFailLoaded();               // lazy read of the persisted list
        void PersistFail(std::uint64_t a_key); // append one permanently-failed key
        void EnsureSlowLoaded();
        void PersistSlow(std::uint64_t a_key);
        void RewriteSlow();                    // after a retry resolves entries
        Pending                                m_pending;
        RE::TESBoundObject*                    m_pin = nullptr;   // editor selection
        // Re-capture gate. "req" is what the in-flight request was armed with,
        // "shot" is what the texture on screen was actually taken at -- they
        // differ while a capture is in flight, and comparing against the wrong
        // one would either re-shoot forever or miss the last drag frame.
        // Height is in there because the request size is clamped to the screen.
        float m_inspectShotRx = 0.0f, m_inspectShotRy = 0.0f, m_inspectShotRz = 0.0f;
        float m_inspectShotH = 0.0f;
        float m_inspectReqRx = 0.0f, m_inspectReqRy = 0.0f, m_inspectReqRz = 0.0f;
        float m_inspectReqH = 0.0f;
        [[nodiscard]] bool InspectShotStale() const;

        RE::TESBoundObject*                    m_inspect = nullptr;   // C-key overlay
        IconDef                                m_inspectDef{};        // live drag rotation
        Icon                                   m_inspectIcon{};       // its OWN texture
        bool                                   m_inspectValid = false;
        bool                                   m_inspectRetire = false;   // free on Tick
        bool                                   m_pendingInspect = false;  // in-flight kind
        bool                                   m_pendingBusy = false;
        // ★Held OFF because the machine is out of memory, not because anything
        // failed. Only here so the log says it once on the way in and once on
        // the way out instead of every frame.
        bool                                   m_memPaused = false;
        int                                    m_frames = 0;
        std::uint32_t                          m_stampBefore = 0;
        // ★A run of timeouts means the backbuffer read is coming back empty —
        // in practice, frame generation running over a menu. Counted so the
        // cause can be named ONCE in the log instead of leaving the user with
        // nothing but missing icons (see GiveUpPending).
        int                                    m_timeoutStreak = 0;
        bool                                   m_emptyCaptureWarned = false;

        // ★★1.0.5 — the same no-flicker guarantee the pinned item has, for
        // EVERY item: the newest key that item has a sprite for. A miss on the
        // current key falls back to this instead of drawing nothing.
        //
        // It exists because the GLOBAL capture light re-keys the whole board at
        // once. Without it, dragging that slider blanks every icon on screen
        // until each one is re-photographed one per frame — the grid empties
        // and refills, which reads as a crash rather than as a preview.
        // Written only for sprites that stay in memory; a precache eviction
        // has nothing to fall back to and must not claim it does.
        std::unordered_map<RE::TESBoundObject*, std::uint64_t> m_lastGood;

        // live-edit slot: latest completed capture of the pinned item (drawn
        // as a fallback while newer keys are still in flight -> no flicker)
        std::uint64_t             m_pinLastKey = 0;
        std::vector<std::uint8_t> m_pinSprite;   // pixels for the deferred disk write
        int                       m_pinW = 0;
        int                       m_pinH = 0;
        std::uint32_t             m_pinFmt = 0;
    };
}
