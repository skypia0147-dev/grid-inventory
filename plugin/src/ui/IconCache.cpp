#include "ui/IconCache.h"
#include "ui/ItemPreview.h"
#include "ui/Theme.h"

#include <imgui.h>   // GetFrameCount: "was this drawn recently?"

#include <algorithm>   // sort (eviction order)
#include <vector>

#include <d3d11.h>
// ★PNG decoding uses WIC — part of the Windows SDK, already on every machine
// that runs the game. A vendored decoder (stb_image et al) would be a new
// third-party source file in a mod that people install from a download page;
// this is the same trade the D3D11 dependency already makes.
#include <wincodec.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace FUI
{
    static constexpr const char* kIconDir = "Data/SKSE/Plugins/GridInventory_icons";   // legacy (absorbed)
    static constexpr const char* kPakPath = "Data/SKSE/Plugins/GridInventory_icons.pak";
    // capture keys that permanently failed (e.g. mod items whose preview
    // model never renders) — persisted so a relog does NOT retry them
    static constexpr const char* kFailPath = "Data/SKSE/Plugins/GridInventory_iconfail.txt";
    // GI68: ran out of window while still LOADING - retried on request,
    // unlike the fail list which is never retried
    static constexpr const char* kSlowPath = "Data/SKSE/Plugins/GridInventory_iconslow.txt";
    static constexpr const char* kPakTmp  = "Data/SKSE/Plugins/GridInventory_icons.pak.tmp";
    // ★★★v8/v9 ARE WRITTEN; v5/v6/v7 ARE STILL READ.
    //
    // Everything before v8 was chroma-keyed, and those pixels are wrong
    // wherever an item had an alpha texture: the magenta backdrop is blended
    // INTO them and was then declared opaque. A veil stored as a solid purple
    // sheet cannot be repaired by reading it differently.
    //
    // But refusing them outright would throw away every icon pak a player has
    // built or been given, to fix items most paks do not contain. So the old
    // magics stay readable and only new captures are written as v8/v9. A
    // player who wants the fix applied to icons they already have deletes
    // GridInventory_icons.pak and lets them re-capture.
    //
    // Is there room to capture at all? See the note at the call site for why
    // this is asked BEFORE arming a queue entry rather than after failing one.
    //
    // ★A FRACTION, FENCED AT BOTH ENDS -- all three numbers earn their place.
    //
    // The FRACTION is the honest measure of "this machine is running out":
    // the reported crash sat at 1.13 of 23.91 GB, which is 4.7%.
    //
    // The FLOOR exists because a fraction alone is meaningless on a small
    // machine -- 10% of 8GB is 800MB, and one that size lives near there
    // normally. Below 512MB nothing should be captured on any box.
    //
    // The CEILING exists because a capture's cost does NOT scale with RAM. It
    // is two 2048² targets and a mesh, the same on every machine, so demanding
    // 6.4GB free on a 64GB box would stall captures on the machine least
    // likely to be in trouble. Past 2GB of headroom the question is settled.
    //
    // ★10%, not the 5% first written: 5% would have cleared the reported crash
    // by 0.3 of a percentage point, and a margin that thin is not a margin.
    [[nodiscard]] static bool MemoryHeadroom()
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        // ★Cannot ask -> proceed. A capture that might fail beats an icon
        // system that silently never runs because one API said no.
        if (!::GlobalMemoryStatusEx(&ms)) return true;
        constexpr ULONGLONG kFloor   =  512ull * 1024 * 1024;
        constexpr ULONGLONG kCeiling = 2048ull * 1024 * 1024;
        const ULONGLONG tenth = ms.ullTotalPhys / 10;
        const ULONGLONG need =
            (std::min)(kCeiling, (std::max)(kFloor, tenth));
        return ms.ullAvailPhys >= need;
    }

    // 'FIC8': no rotation field.
    static constexpr std::uint32_t kIconMagic = 0x38434946;
    // 'FIC9': records the FIRST GEOMETRY's world.rotate at capture (9 f32
    // row-major) — the transform the renderer actually draws with, wherever
    // the engine hid its extra rotation. The offline tool combines it with
    // the capture def + its own nif chain for an EXACT preview orientation.
    static constexpr std::uint32_t kIconMagicRot = 0x39434946;

    // ---- read-only: the chroma-keyed generations -------------------------
    // 'FIC5' no rotation · 'FIC6' meaningless rotation (the root's, measured
    // identity for every item) · 'FIC7' the geometry's rotation, same field
    // layout as FIC9.
    static constexpr std::uint32_t kIconMagicV5 = 0x35434946;
    static constexpr std::uint32_t kIconMagicV6 = 0x36434946;
    static constexpr std::uint32_t kIconMagicV7 = 0x37434946;

    [[nodiscard]] inline bool IsKnownIconMagic(std::uint32_t a_m)
    {
        return a_m == kIconMagic || a_m == kIconMagicRot ||
               a_m == kIconMagicV5 || a_m == kIconMagicV6 || a_m == kIconMagicV7;
    }
    // Which records carry the 9-float rotation block, whatever it means.
    [[nodiscard]] inline bool IconMagicHasRotField(std::uint32_t a_m)
    {
        return a_m == kIconMagicRot || a_m == kIconMagicV6 || a_m == kIconMagicV7;
    }
    // ...and which of those rotations is the one worth using.
    [[nodiscard]] inline bool IconMagicRotUsable(std::uint32_t a_m)
    {
        return a_m == kIconMagicRot || a_m == kIconMagicV7;
    }

    // ---- single-pak disk cache ----
    // One append-only file instead of thousands of per-item .fic files:
    //   v5: [magic u32 | key u64 | w u32 | h u32 | fmt u32 | len u32 | pixels]
    //   v6: [magic u32 | key u64 | w u32 | h u32 | fmt u32 | rot 9*f32 |
    //        len u32 | pixels]
    // Re-captures append a NEW record (last one wins at scan); superseded
    // bytes are compacted at scan time once they exceed 30%. A truncated
    // tail (crash mid-append) just ends the scan — icons are re-derivable
    // cache data, the worst case is a re-render.
    static constexpr std::uint64_t kPakHdrSize    = 4 + 8 + 4 * 4;        // 28
    static constexpr std::uint64_t kPakHdrSizeRot = kPakHdrSize + 36;     // 64

    namespace
    {
        struct PakEntry
        {
            std::uint64_t off = 0;   // record start (header)
            std::uint32_t w = 0, h = 0, fmt = 0, len = 0;
            bool          rotOnDisk = false;   // header carries the 36B field
            bool          hasRot = false;      // ...and it is v7-valid data
            float         rot[9] = {};

            [[nodiscard]] std::uint64_t hdrSize() const
            {
                return rotOnDisk ? kPakHdrSizeRot : kPakHdrSize;
            }
        };
        std::unordered_map<std::uint64_t, PakEntry> g_pakIndex;
        bool g_pakScanned = false;

        // persistent append handle: opening/closing the pak PER ICON was a
        // per-capture disk hitch during mass precache. flush() per append
        // keeps the same durability the old close() gave (stdio -> OS write;
        // neither ever fsync'd). MUST be closed before rename/remove.
        std::ofstream g_pakOut;

        void ClosePakHandle()
        {
            if (g_pakOut.is_open()) g_pakOut.close();
        }

        // NEW entries are always plain v5 — the v6/v7 rot record is write-dead
        // (the offline tool's orientation bugs were fixed at the source);
        // reading old v6/v7 entries stays supported in ScanPak/CompactPak.
        bool AppendToPakRaw(std::uint64_t a_key, std::uint32_t a_w, std::uint32_t a_h,
                            std::uint32_t a_fmt, const std::uint8_t* a_px, std::uint32_t a_len)
        {
            if (!g_pakOut.is_open()) {
                g_pakOut.clear();
                g_pakOut.open(kPakPath, std::ios::binary | std::ios::app);
                if (!g_pakOut) return false;
            }
            g_pakOut.seekp(0, std::ios::end);
            const std::uint64_t off = static_cast<std::uint64_t>(g_pakOut.tellp());
            g_pakOut.write(reinterpret_cast<const char*>(&kIconMagic), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_key), 8);
            g_pakOut.write(reinterpret_cast<const char*>(&a_w), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_h), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_fmt), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_len), 4);
            g_pakOut.write(reinterpret_cast<const char*>(a_px), a_len);
            g_pakOut.flush();
            if (!g_pakOut) {
                ClosePakHandle();
                return false;
            }
            g_pakIndex[a_key] = PakEntry{ off, a_w, a_h, a_fmt, a_len };
            return true;
        }

        void CompactPak()
        {
            ClosePakHandle();   // the rename below fails on an open handle
            std::ofstream out(kPakTmp, std::ios::binary | std::ios::trunc);
            std::ifstream in(kPakPath, std::ios::binary);
            if (!out || !in) return;
            std::unordered_map<std::uint64_t, PakEntry> fresh;
            std::vector<std::uint8_t> px;
            for (const auto& [key, en] : g_pakIndex) {
                px.resize(en.len);
                in.seekg(static_cast<std::streamoff>(en.off + en.hdrSize()));
                if (!in.read(reinterpret_cast<char*>(px.data()), en.len)) return;
                const std::uint64_t off = static_cast<std::uint64_t>(out.tellp());
                // v6 entries (meaningless root-rotation experiment) compact
                // back to v5 — the junk 36B field is dropped
                const std::uint32_t magic = en.hasRot ? kIconMagicRot : kIconMagic;
                out.write(reinterpret_cast<const char*>(&magic), 4);
                out.write(reinterpret_cast<const char*>(&key), 8);
                out.write(reinterpret_cast<const char*>(&en.w), 4);
                out.write(reinterpret_cast<const char*>(&en.h), 4);
                out.write(reinterpret_cast<const char*>(&en.fmt), 4);
                if (en.hasRot) {
                    out.write(reinterpret_cast<const char*>(en.rot), 36);
                }
                out.write(reinterpret_cast<const char*>(&en.len), 4);
                out.write(reinterpret_cast<const char*>(px.data()), en.len);
                if (!out) return;
                PakEntry ne = en;
                ne.off = off;
                ne.rotOnDisk = en.hasRot;
                fresh[key] = ne;
            }
            in.close();
            out.close();
            // old pak is only removed AFTER the tmp completed: a crash here
            // leaves either a full old pak or a full tmp + old pak (tmp wins
            // nothing — it's simply rewritten next time)
            std::error_code ec;
            std::filesystem::remove(kPakPath, ec);
            std::filesystem::rename(kPakTmp, kPakPath, ec);
            if (!ec) {
                g_pakIndex = std::move(fresh);
                SKSE::log::info("[ICONS] pak compacted ({} icons)", g_pakIndex.size());
            } else {
                // B1: the old pak is already GONE here — keeping the old
                // index would point future appends/reads at wrong offsets in
                // a brand-new file (silently reading other items' pixels).
                // Drop the index and force a rescan of whatever survives.
                g_pakIndex.clear();
                g_pakScanned = false;
                SKSE::log::error("[ICONS] pak compaction rename failed ({}) — "
                                 "index dropped, rescanning", ec.message());
            }
        }

        void ScanPak()
        {
            if (g_pakScanned) return;
            g_pakScanned = true;
            g_pakIndex.clear();

            std::uint64_t fileSize = 0;
            {
                std::ifstream in(kPakPath, std::ios::binary);
                if (in) {
                    in.seekg(0, std::ios::end);
                    fileSize = static_cast<std::uint64_t>(in.tellg());
                    in.seekg(0);
                    std::uint64_t goodEnd = 0;   // end of the last valid record
                    while (true) {
                        const std::uint64_t off = static_cast<std::uint64_t>(in.tellg());
                        std::uint32_t magic = 0, w = 0, h = 0, fmt = 0, len = 0;
                        std::uint64_t key = 0;
                        float rot[9] = {};
                        if (!in.read(reinterpret_cast<char*>(&magic), 4) ||
                            !IsKnownIconMagic(magic) ||
                            !in.read(reinterpret_cast<char*>(&key), 8) ||
                            !in.read(reinterpret_cast<char*>(&w), 4) ||
                            !in.read(reinterpret_cast<char*>(&h), 4) ||
                            !in.read(reinterpret_cast<char*>(&fmt), 4)) {
                            break;   // clean EOF or truncated tail — stop
                        }
                        const bool rotField = IconMagicHasRotField(magic);
                        const bool hasRot = IconMagicRotUsable(magic);
                        if (rotField &&
                            !in.read(reinterpret_cast<char*>(rot), 36)) {
                            break;
                        }
                        if (!in.read(reinterpret_cast<char*>(&len), 4)) {
                            break;
                        }
                        const std::uint64_t hdr = rotField ? kPakHdrSizeRot : kPakHdrSize;
                        if (w == 0 || h == 0 || w > 2048 || h > 2048 ||
                            len != w * h * 4 || off + hdr + len > fileSize) {
                            break;   // corrupt record: drop it and the tail
                        }
                        PakEntry en{ off, w, h, fmt, len };
                        en.rotOnDisk = rotField;
                        if (hasRot) {
                            en.hasRot = true;
                            std::memcpy(en.rot, rot, 36);
                        }
                        g_pakIndex[key] = en;
                        goodEnd = off + hdr + len;
                        in.seekg(static_cast<std::streamoff>(goodEnd));
                    }
                    in.close();
                    // GI53 (review): a torn tail must be TRUNCATED, not just
                    // ignored. Appends land at the file END -- behind a torn
                    // tail they are unreachable to every future scan, so each
                    // session re-captured and re-appended the same icons until
                    // the dead-bytes threshold finally forced a compaction.
                    if (goodEnd < fileSize) {
                        ClosePakHandle();
                        std::error_code tec;
                        std::filesystem::resize_file(kPakPath, goodEnd, tec);
                        if (!tec) {
                            SKSE::log::info("[ICONS] pak tail truncated ({} -> {} bytes)",
                                fileSize, goodEnd);
                            fileSize = goodEnd;
                        } else {
                            SKSE::log::error("[ICONS] pak tail truncation failed: {}",
                                tec.message());
                        }
                    }
                }
            }

            // one-time migration: absorb the legacy per-item .fic directory
            // (stale-version files are deleted without absorbing — re-render)
            std::error_code ec;
            if (std::filesystem::exists(kIconDir, ec)) {
                std::vector<std::filesystem::path> files;
                for (const auto& e : std::filesystem::directory_iterator(kIconDir, ec)) {
                    if (e.is_regular_file() && e.path().extension() == ".fic") {
                        files.push_back(e.path());
                    }
                }
                int absorbed = 0;
                for (const auto& f : files) {
                    std::uint64_t key = 0;
                    try {
                        key = std::stoull(f.stem().string(), nullptr, 16);
                    } catch (...) {
                        continue;
                    }
                    {
                        std::ifstream in(f, std::ios::binary);
                        std::uint32_t magic = 0, w = 0, h = 0, fmt = 0;
                        if (in.read(reinterpret_cast<char*>(&magic), 4) &&
                            magic == kIconMagic &&
                            in.read(reinterpret_cast<char*>(&w), 4) &&
                            in.read(reinterpret_cast<char*>(&h), 4) &&
                            in.read(reinterpret_cast<char*>(&fmt), 4) &&
                            w > 0 && h > 0 && w <= 2048 && h <= 2048) {
                            std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
                            if (in.read(reinterpret_cast<char*>(px.data()),
                                    static_cast<std::streamsize>(px.size()))) {
                                if (AppendToPakRaw(key, w, h, fmt, px.data(),
                                        static_cast<std::uint32_t>(px.size()))) {
                                    ++absorbed;
                                }
                            }
                        }
                    }
                    std::filesystem::remove(f, ec);
                }
                std::filesystem::remove(kIconDir, ec);   // only succeeds when empty
                if (absorbed) {
                    SKSE::log::info("[ICONS] migrated {} legacy .fic files into the pak", absorbed);
                }
                std::ifstream in(kPakPath, std::ios::binary | std::ios::ate);
                if (in) fileSize = static_cast<std::uint64_t>(in.tellg());
            }

            // compact when superseded records exceed 30% (and 8+ MB)
            std::uint64_t live = 0;
            for (const auto& [key, en] : g_pakIndex) live += en.hdrSize() + en.len;
            if (fileSize > live) {
                const std::uint64_t dead = fileSize - live;
                if (dead * 10 >= fileSize * 3 && dead > (8ull << 20)) {
                    SKSE::log::info("[ICONS] pak compaction: {} MB dead of {} MB",
                        dead >> 20, fileSize >> 20);
                    CompactPak();
                }
            }

            SKSE::log::info("[ICONS] pak scanned: {} icons", g_pakIndex.size());
        }

    }

    // ★★★1.0.5 — THE SILHOUETTE SPRITE IS GONE.
    //
    // It downscaled each icon's alpha into a small padded canvas, blurred it,
    // and uploaded a white texture the tile draw tinted: first as the rarity
    // halo, then, once rarity became a corner wedge, as the item's drop shadow.
    //
    // The canvas is why it had to go. A 300px capture was box-averaged down to
    // a 96px core, blurred there, then stretched back up to a ~40px tile, and
    // that round trip put a FLOOR under the softness — the shape was destroyed
    // by the downsample, not by the blur, so shrinking the radius stopped
    // helping well before the shadow got crisp. Four tunings all landed in the
    // same place for that reason.
    //
    // Grid::DrawItemShadow now stamps the icon's OWN sprite in black on a ring
    // instead. Full capture resolution, the blur radius means the pixels it
    // says, and this path costs nothing at all: no CPU blur per capture, no
    // 43KB of VRAM per on-screen icon, one less upload on every cache fill.
    //
    // Icon::glowSrv / glowTex / gw / gh / gpad survive as always-null fields —
    // every reader already checked them for null, and the release paths are
    // harmless on a null. See IconCache.h.

    // ★Icon textures carry a FULL CPU-built mip chain. The grid draws a
    // ~250px capture into a ~40px cell; a mipless texture makes the GPU pick
    // colours from one several-times-too-fine level, and that aliasing read
    // as "the icon is lower quality than the model it was captured from"
    // (user report). Built on the CPU on purpose: GenerateMips needs the
    // immediate CONTEXT, which is not thread-safe, while device->Create* is —
    // this path must stay callable from wherever uploads happen today.
    // Alpha-WEIGHTED 2x2 box per level — averaging straight RGBA would pull
    // edge colours toward the transparent black margin and ring every sprite
    // with a dark fringe. Per-channel, so RGBA and BGRA both work.
    static ID3D11Texture2D* CreateMippedTexture(ID3D11Device* a_device,
        const std::uint8_t* a_px, int a_w, int a_h, DXGI_FORMAT a_fmt,
        ID3D11ShaderResourceView** a_srv)
    {
        int levels = 1;
        for (int m = (std::max)(a_w, a_h); m > 1; m >>= 1) ++levels;

        std::vector<std::vector<std::uint8_t>> mips;
        mips.reserve(static_cast<size_t>(levels));
        mips.emplace_back(a_px, a_px + static_cast<size_t>(a_w) * a_h * 4);
        int pw = a_w, ph = a_h;
        for (int l = 1; l < levels; ++l) {
            const int w = (std::max)(1, pw >> 1), h = (std::max)(1, ph >> 1);
            std::vector<std::uint8_t> dst(static_cast<size_t>(w) * h * 4);
            const auto& src = mips.back();
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int sx0 = (std::min)(x * 2, pw - 1), sx1 = (std::min)(x * 2 + 1, pw - 1);
                    const int sy0 = (std::min)(y * 2, ph - 1), sy1 = (std::min)(y * 2 + 1, ph - 1);
                    const std::uint8_t* s[4] = {
                        &src[(static_cast<size_t>(sy0) * pw + sx0) * 4],
                        &src[(static_cast<size_t>(sy0) * pw + sx1) * 4],
                        &src[(static_cast<size_t>(sy1) * pw + sx0) * 4],
                        &src[(static_cast<size_t>(sy1) * pw + sx1) * 4],
                    };
                    int asum = 0, csum[3] = { 0, 0, 0 };
                    for (const auto* p : s) {
                        const int a = p[3];
                        asum += a;
                        csum[0] += p[0] * a;
                        csum[1] += p[1] * a;
                        csum[2] += p[2] * a;
                    }
                    std::uint8_t* d = &dst[(static_cast<size_t>(y) * w + x) * 4];
                    if (asum > 0) {
                        d[0] = static_cast<std::uint8_t>(csum[0] / asum);
                        d[1] = static_cast<std::uint8_t>(csum[1] / asum);
                        d[2] = static_cast<std::uint8_t>(csum[2] / asum);
                    } else {
                        d[0] = d[1] = d[2] = 0;
                    }
                    d[3] = static_cast<std::uint8_t>((asum + 2) / 4);
                }
            }
            mips.push_back(std::move(dst));
            pw = w;
            ph = h;
        }

        std::vector<D3D11_SUBRESOURCE_DATA> init(static_cast<size_t>(levels));
        int w = a_w, h = a_h;
        for (int l = 0; l < levels; ++l) {
            init[static_cast<size_t>(l)].pSysMem     = mips[static_cast<size_t>(l)].data();
            init[static_cast<size_t>(l)].SysMemPitch = static_cast<UINT>(w * 4);
            w = (std::max)(1, w >> 1);
            h = (std::max)(1, h >> 1);
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = static_cast<UINT>(a_w);
        td.Height           = static_cast<UINT>(a_h);
        td.MipLevels        = static_cast<UINT>(levels);
        td.ArraySize        = 1;
        td.Format           = a_fmt;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D* tex = nullptr;
        if (FAILED(a_device->CreateTexture2D(&td, init.data(), &tex))) return nullptr;
        if (FAILED(a_device->CreateShaderResourceView(tex, nullptr, a_srv))) {
            tex->Release();
            return nullptr;
        }
        return tex;
    }

    // pixels -> GPU texture (+ optional silhouette glow) — shared by the pak
    // loader and the loose-file loader (slot silhouettes, torn frames)
    static bool CreateIconTexture(const std::uint8_t* a_px, int a_w, int a_h,
                                  std::uint32_t a_fmt, bool a_makeGlow,
                                  IconCache::Icon& a_out)
    {
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data) return false;
        auto* device = reinterpret_cast<ID3D11Device*>(data->forwarder);
        if (!device) return false;

        // ★★Reject 10-bit entries HERE, where every loader meets. Such a file was
        // written by a build that copied a R10G10B10A2 backbuffer byte-for-byte
        // without unpacking it, so its pixels are channel-shredded -- the
        // speckled icons reported under Frame Generation. Refusing it is a cache
        // miss, and a miss recaptures cleanly; there is nothing to salvage.
        if (a_fmt == static_cast<std::uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM) ||
            a_fmt == static_cast<std::uint32_t>(DXGI_FORMAT_R10G10B10A2_UINT)) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                SKSE::log::warn("[ICONS] cached icons from an older build are 10-bit "
                                "(shredded) -- recapturing them");
            }
            return false;
        }

        IconCache::Icon icon;
        icon.w = a_w;
        icon.h = a_h;
        // ★Every entry is priced HERE, where the texture is actually made, so
        // no caller can add one to the cache without a size on it. The 4/3 is
        // the mip tail: 1 + 1/4 + 1/16 + ... converges there.
        icon.bytes = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(a_w) * a_h * 4ull * 4ull / 3ull);
        icon.tex = CreateMippedTexture(device, a_px, a_w, a_h,
            static_cast<DXGI_FORMAT>(a_fmt), &icon.srv);
        if (!icon.tex) return false;
        // a_makeGlow is inert since 1.0.5 — the silhouette sprite it asked for
        // is gone (see the note above). Kept in the signature because it is a
        // public loader flag with call sites outside this file, and dropping it
        // would churn them for nothing.
        (void)a_makeGlow;
        a_out = icon;
        return true;
    }

    IconCache* IconCache::GetSingleton()
    {
        static IconCache singleton;
        return std::addressof(singleton);
    }

    IconDef IconCache::ResolveDef(RE::TESBoundObject* a_obj) const
    {
        // deliberately UNTOUCHED by inspect: the drag rotation is injected at
        // the Request site only, so tile keys / Get() / the pak stay identical
        // whether or not something is being inspected
        return m_resolver ? m_resolver(a_obj) : IconDef{};
    }

    void IconCache::ReleaseIcon(Icon& a_icon)
    {
        if (a_icon.srv) a_icon.srv->Release();
        if (a_icon.tex) a_icon.tex->Release();
        if (a_icon.glowSrv) a_icon.glowSrv->Release();
        if (a_icon.glowTex) a_icon.glowTex->Release();
        a_icon = Icon{};
    }

    void IconCache::SetInspect(RE::TESBoundObject* a_obj, float a_rx, float a_ry, float a_rz)
    {
        if (!a_obj) {
            ClearInspect();
            return;
        }
        m_inspect = a_obj;
        SetInspectRot(a_rx, a_ry, a_rz);
        m_inspectValid = false;   // nothing captured yet ("caching" for a frame)
        // no pin, no cache key: PreRender simply gives the preview to this item
        // while the overlay is open, and the result lands in m_inspectIcon
        ItemPreview::GetSingleton()->SetInspectScale(kInspectModelScale);
    }

    void IconCache::SetInspectRot(float a_rx, float a_ry, float a_rz)
    {
        m_inspectDef.rx = a_rx;
        m_inspectDef.ry = a_ry;
        m_inspectDef.rz = a_rz;
    }

    void IconCache::ClearInspect()
    {
        if (!m_inspect) return;
        m_inspect = nullptr;
        ItemPreview::GetSingleton()->SetInspectScale(0.0f);
        m_inspectValid = false;   // stop drawing it immediately
        m_inspectRetire = true;   // ...free it next Tick (see ProcessDeferredRelease)
    }

    void IconCache::ProcessDeferredRelease()
    {
        if (!m_inspectRetire) return;
        m_inspectRetire = false;
        ReleaseIcon(m_inspectIcon);
    }

    // ★★1.0.5: the lamp angle a capture is actually taken at — the GLOBAL
    // setting plus this item's own offset, both measured from the shipped rig.
    // ONE function so the cache key and the capture request can never be
    // computed from different sums; two copies of this addition would show up
    // as icons that are cached under one light and photographed under another.
    static void CaptureLightFor(const IconDef& a_def, float& a_az, float& a_el)
    {
        a_az = Theme::CaptureLightAz() + a_def.lightAz;
        a_el = Theme::CaptureLightEl() + a_def.lightEl;
    }

    // Rotation only (whole-degree quantised): scale is a DRAW-time zoom
    // and must not force a re-capture or split cache keys.
    static std::uint32_t RotHash(const IconDef& a_def)
    {
        // ★★The LIGHT belongs in this hash for exactly the reason the rotation
        // does: it changes the pixels. Leave it out and moving an item's lamp
        // produces the same cache key, so the old sprite is served straight
        // back and the setting looks like it does nothing — the failure would
        // read as "light offsets are broken" rather than "the cache answered".
        // ★And it is the TOTAL angle, not the def's own: the global setting
        // re-lights every item including the thousands that carry 0/0, so a
        // hash over the def alone would leave all of them answering from the
        // pak under the old light. Changing the global therefore misses every
        // key at once — which is correct, because every pixel really did
        // change — and changing it BACK hits again, so comparing two global
        // angles costs one re-capture each way rather than two.
        float laz = 0.0f, lel = 0.0f;
        CaptureLightFor(a_def, laz, lel);
        return static_cast<std::uint32_t>(static_cast<int>(a_def.rx)) * 73856093u ^
               static_cast<std::uint32_t>(static_cast<int>(a_def.ry)) * 19349663u ^
               static_cast<std::uint32_t>(static_cast<int>(a_def.rz)) * 83492791u ^
               static_cast<std::uint32_t>(static_cast<int>(laz)) * 2654435761u ^
               static_cast<std::uint32_t>(static_cast<int>(lel)) * 40503u;
    }

    // Model-shared slot: FNV-1a of the normalised world-model path. Items
    // sharing one nif render identically, so they share one capture
    // (measured: 10,711 records over 2,143 unique models = 80% duplicates).
    // Items with ALTERNATE TEXTURES (same nif, different pixels) and items
    // without a model path keep the per-FormID slot. A stale hit would need a
    // full 64-bit collision incl. the rotation hash — effectively impossible.
    // ★★★A SPELL HAS NO MODEL, AND THE ENGINE ALREADY KNOWS WHAT TO SHOW.
    //
    // MDOB (BGSMenuDisplayObject) is the bound object the vanilla magic menu
    // stands in its own 3D scene for a spell: a flame for Flames, a ward for a
    // ward. SpellItem carries one and so does every magic EFFECT, which is
    // where the game actually keeps most of them.
    //
    // Measured on a real load order before any of this was built: of 940
    // pickable spells, 706 have their own MDOB with a model and 102 inherit
    // one from their first effect. The 132 with none are script and creature
    // spells -- "Bleeding Damage", "Werewolf Feed Victim" -- that never reach
    // a wheel, so the real coverage of what a player can pick is effectively
    // whole.
    //
    // ★★So the capture pipeline needs no new kind of asset. It needs to be
    // told, in ONE place, that the thing to PHOTOGRAPH is not always the thing
    // being asked about. Every site that reaches for a model asks this first:
    // the key, the renderability probes, and the capture request itself. They
    // have to agree -- a key taken from the spell and a capture taken from the
    // flame would file the picture under a name nothing ever looks up.
    //
    // ★Spells sharing a display object share an icon, and that is correct
    // rather than merely cheap: two spells the engine draws identically are
    // two spells that look identical, and the wheel would be lying to show
    // them apart. It also means Flames and its stronger cousins may come up
    // the same picture, which is the honest cost of using the game's own art.
    //
    // Returns the object itself when there is nothing better -- so an item is
    // untouched, and a spell with no display object falls through to the drawn
    // category icon exactly as it does today.
    static RE::TESBoundObject* CaptureSourceOf(RE::TESBoundObject* a_obj)
    {
        auto* sp = a_obj ? a_obj->As<RE::SpellItem>() : nullptr;
        if (!sp) return a_obj;
        const auto renderable = [](RE::TESBoundObject* a_o) -> RE::TESBoundObject* {
            if (!a_o) return nullptr;
            const auto* m = a_o->As<RE::TESModel>();
            return (m && m->GetModel() && m->GetModel()[0]) ? a_o : nullptr;
        };
        if (auto* own = renderable(sp->GetMenuDisplayObject())) return own;
        // ★FIRST effect only, which is what the magic menu itself shows -- and
        // a spell's identity in a list is its primary effect. Walking the rest
        // would hand a fire spell the picture of the fear it also carries.
        for (const auto* e : sp->effects) {
            if (!e || !e->baseEffect) break;
            if (auto* fx = renderable(e->baseEffect->GetMenuDisplayObject())) return fx;
            break;
        }
        return a_obj;
    }

    static std::uint32_t ModelSlot32(RE::TESBoundObject* a_in)
    {
        // Key the PICTURE, not the asker -- see CaptureSourceOf.
        RE::TESBoundObject* a_obj = CaptureSourceOf(a_in);
        const char* p = nullptr;
        const char* p2 = nullptr;   // armour: the OTHER sex's ground model
        std::uint32_t altCount = 0;
        if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            // armor is NOT a TESModel (skyrim_cast returns null — that made
            // every ARMO fall back to per-FormID keys): its GND model lives
            // on TESBipedModelForm::worldModels
            const auto& wm = armo->worldModels[RE::TESBipedModelForm::Sexes::kMale];
            const auto& wf = armo->worldModels[RE::TESBipedModelForm::Sexes::kFemale];
            // ★★★BOTH SEXES, because an armour record has TWO ground models and
            // the male one alone is only half its identity.
            //
            // Reported against a female-only armour pack: an item's icon comes
            // up as ANOTHER PIECE OF THE SAME SET, and dragging a rotation
            // slider in EDIT walks through the set instead of turning the item.
            // That is what one shared bucket looks like from the outside -- the
            // key said these records are the same picture, so the first capture
            // answered for all of them, and rotation was the only axis left to
            // tell them apart. Setting a unique angle "fixed" it by buying the
            // item a bucket of its own.
            //
            // A pack that dresses one sex routinely leaves the other's ground
            // model as one shared placeholder across a whole set, so matching on
            // it alone declares a dozen different garments identical. Two
            // records that agree on BOTH paths really do render the same
            // picture -- that is the whole of what the engine draws here -- so
            // this is the identity the key was reaching for.
            // ★It costs almost nothing. Measured over Skyrim.esm: 2522 eligible
            // armours, 326 buckets by the male path and 329 by the pair. Three
            // more captures, and 99.9% of the saving kept.
            p  = wm.GetModel();
            p2 = wf.GetModel();
            // ★And the female half gets the same alternate-texture veto as the
            // male: same nif, different pixels, so no sharing either way.
            altCount = wm.numAlternateTextures + wf.numAlternateTextures;
        } else if (const auto* mdl = skyrim_cast<RE::TESModel*>(a_obj)) {
            p = mdl->GetModel();
            if (const auto* swap = skyrim_cast<RE::TESModelTextureSwap*>(a_obj)) {
                altCount = swap->numAlternateTextures;
            }
        }
        if (!p || !*p) return a_obj->GetFormID();
        if (altCount > 0) return a_obj->GetFormID();   // same nif, other pixels
        std::uint32_t h = 2166136261u;
        const auto fold = [&h](const char* a_path) {
            const char* s = a_path;
            if (_strnicmp(s, "meshes", 6) == 0 && (s[6] == '\\' || s[6] == '/')) {
                s += 7;
            }
            for (; *s; ++s) {
                char c = *s;
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c == '/') c = '\\';
                h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
            }
        };
        fold(p);
        // ★The second path is folded behind a SEPARATOR, and only when there is
        // one -- so every key that has ever existed keeps its value. A weapon, a
        // potion, a book and an armour with no second ground model all hash
        // exactly as they did, and the shipped sprite pak still answers for
        // them. Only the records this fix is about are re-keyed.
        // ★The separator is not decoration: without it ("a", "bc") and
        // ("ab", "c") fold to the same number.
        if (p2 && *p2) {
            h = (h ^ 0x1Fu) * 16777619u;
            fold(p2);
            // ★★★AND WHEN THE TWO PATHS DIFFER, WHICH BODY IS WEARING IT.
            //
            // The line above says this key names the picture. For a record
            // whose two ground models are different nifs, the sex is PART of
            // which picture it is: we do not choose the model, the engine does,
            // and it draws the one belonging to the character standing there.
            // One slot for both therefore held whichever sex photographed it
            // first -- fine while a character keeps the body they started with,
            // wrong the moment showracemenu says otherwise, and wrong for the
            // items ini's own new |F / |M lines whenever the two angles happen
            // to agree (equal rotations hash equal, so the tuning alone could
            // not tell the two pictures apart).
            //
            // ★Only when they DIFFER. Thousands of records name the same nif
            // twice or fill one side, and the engine shows that one model to
            // everybody -- splitting those would double their captures to
            // store the same pixels under two names.
            if (_stricmp(p, p2) != 0) {
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* base = pc->GetActorBase()) {
                        h = (h ^ (base->GetSex() == RE::SEX::kFemale ? 0xF1u : 0x4Du))
                          * 16777619u;
                    }
                }
            }
        }
        // ★★1.0.5 — the base-form ENCHANTMENT deliberately does NOT join this
        // hash, though it looks like it should: Iron Sword and Iron Sword of
        // Burning share a nif, and an enchant glow would make them different
        // pictures. It was tried and reverted, because the glow never reaches
        // a cache capture in the first place (see Inv3D::Load) — so splitting
        // the key would multiply the icon set for pixel-identical results.
        // Enchanted items are told apart by the rarity halo, which is drawn at
        // tile time and costs no capture at all.
        return h;
    }

    // Display-only forms (Hearthfire build-menu previews under \Interface\,
    // furniture markers): named MISC forms the player can never obtain —
    // precaching them wastes ~314 captures of house walls/roofs.
    static bool IsDisplayOnlyModel(RE::TESBoundObject* a_obj)
    {
        const char* p = nullptr;
        if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            p = armo->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
        } else if (const auto* mdl = skyrim_cast<RE::TESModel*>(a_obj)) {
            p = mdl->GetModel();
        }
        if (!p || !*p) return false;
        std::string s(p);
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c == '/') c = '\\';
        }
        return s.find("\\interface\\") != std::string::npos ||
               s.find("marker") != std::string::npos;
    }

    // ...and the same category, for forms the path heuristic above cannot see.
    // These reach the inventory in NO circumstance, so an icon for them can
    // never be displayed and rendering one is pure waste.
    //
    // Coinpurse: a world object. Activating it plays the pickup and raises the
    // player's gold — the MISC form itself is never added to any container, so
    // no inventory ever shows it. That is also why Bethesda never noticed its
    // model points at 'Clutter\CoinBagLarge.nif' while the mesh actually ships
    // as 'meshes\plants\coinbaglarge.nif'; USSEP leaves it alone for the same
    // reason. Correcting the path would only buy an icon nothing can draw.
    static bool IsUnobtainable(RE::TESBoundObject* a_obj)
    {
        struct Entry
        {
            const char*  plugin;
            RE::FormID   local;
        };
        static constexpr Entry kEntries[] = {
            { "Skyrim.esm", 0x09DA9C },   // Coinpurse (large)
            { "Skyrim.esm", 0x09DA9D },   // Coinpurse (medium)
            { "Skyrim.esm", 0x09DA9E },   // Coinpurse (small)
        };
        // Resolved once and then reused -- the load order cannot change while
        // the game runs, and this is consulted for every item ever queued.
        // NOT a `static const` initialised in place: that latches whatever the
        // first call sees, so one call before the data handler exists would
        // cache an empty table for the rest of the session.
        static std::unordered_set<RE::FormID> s_ids;
        static bool                           s_built = false;
        if (!s_built) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return false;   // too early -- try again next call
            for (const auto& e : kEntries) {
                if (const auto id = dh->LookupFormID(e.local, e.plugin)) {
                    s_ids.insert(id);
                }
            }
            s_built = true;
        }
        return s_ids.contains(a_obj->GetFormID());
    }

    std::uint64_t IconCache::KeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const
    {
        return (static_cast<std::uint64_t>(ModelSlot32(a_obj)) << 32) | RotHash(a_def);
    }

    std::uint64_t IconCache::LegacyKeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const
    {
        return (static_cast<std::uint64_t>(a_obj->GetFormID()) << 32) | RotHash(a_def);
    }

    const IconCache::Icon* IconCache::Get(RE::TESBoundObject* a_obj) const
    {
        if (!a_obj) return nullptr;
        // ★GI52 flat style: there is no sprite to find — every caller already
        // falls through to Fallback::Get on a miss, so answering "nothing"
        // here is the whole switch. Paired with Capturable() refusing to queue,
        // this style renders the entire inventory with zero engine work.
        if (m_style == Style::kFlat) return nullptr;
        const IconDef def = ResolveDef(a_obj);
        const std::uint64_t key = KeyFor(a_obj, def);
        // ★PIXEL: a derived sprite keyed like the realistic one. A miss is not
        // a failure — it queues the derivation and falls through to the
        // realistic sprite for now, so the grid never blanks while a style
        // switch sweeps through.
        if (m_style == Style::kPixel) {
            // ★The dot RESOLUTION comes from the FOOTPRINT, which is not part
            // of the key (that carries the model and the rotation only). So an
            // editor footprint change — 1x1 to 2x4 — leaves a matching key
            // whose sprite has a quarter of the dots it now needs, blown up
            // over four cells and four times chunkier than its neighbours.
            // Compare the size the sprite was DERIVED at, not just the key.
            const std::pair<int, int> cells{ (std::max)(1, def.w), (std::max)(1, def.h) };
            const auto cit = m_pixelCells.find(key);
            const bool sized = cit != m_pixelCells.end() && cit->second == cells;
            const auto pit = m_pixelIcons.find(key);
            if (pit != m_pixelIcons.end() && sized) return &pit->second;
            m_pixelCells[key] = cells;
            if (m_pixelQueued.insert(key).second) m_pixelQueue.push_back(key);
            // a wrong-sized sprite still beats snapping back to the realistic
            // one for the frame the re-derive takes
            if (pit != m_pixelIcons.end()) return &pit->second;
        }
        auto it = m_icons.find(key);
        if (it == m_icons.end()) {
            it = m_icons.find(LegacyKeyFor(a_obj, def));   // pre-migration pak
        }
        // ★★Fill the miss HERE, from the pak, in this frame. The queue path
        // below would also load it -- synchronously, even -- but only AFTER
        // this call has already answered "nothing", so the tile spends one
        // frame on its category drawing and then swaps. One frame is enough to
        // read as a flicker, and with a budget in play that flicker would fire
        // every time a scrolled-away icon came back. Costs a file read and an
        // upload; both are already happening on this thread.
        // ★Rate-limited: a screenful of misses at once is a hitch, so the rest
        // fall through to the old behaviour for a frame and arrive next.
        if (it == m_icons.end() && m_refillLeft > 0 && !m_failed.contains(key)) {
            if (LoadFromDisk(key)) {
                --m_refillLeft;
                it = m_icons.find(key);
            }
        }
        if (it != m_icons.end()) {
            // ★What "recently used" MEANS. Nothing else marks an icon as being
            // on screen -- the draw just reads the SRV -- so if this line is
            // not here the budget cannot tell the visible grid from a corpse
            // looted an hour ago, and it will happily drop what the player is
            // looking at.
            it->second.lastUsed = m_tick.load(std::memory_order_relaxed);
            return &it->second;
        }

        // live edit: while the current key's capture is in flight, keep
        // showing the pinned item's latest completed capture (no flicker)
        if (a_obj == m_pin && m_pinLastKey) {
            const auto it2 = m_icons.find(m_pinLastKey);
            if (it2 != m_icons.end()) return &it2->second;
        }
        // ★★...and the same for every other item, which is what a board-wide
        // re-key needs. Drawing the previous angle for the frames the new one
        // takes is strictly better than drawing nothing: the icon is still the
        // right item, still the right shape, and only the lighting lags.
        if (const auto lg = m_lastGood.find(a_obj); lg != m_lastGood.end()) {
            const auto it3 = m_icons.find(lg->second);
            if (it3 != m_icons.end()) return &it3->second;
        }
        return nullptr;
    }

    void IconCache::SetPin(RE::TESBoundObject* a_obj)
    {
        if (m_pin && m_pin != a_obj && m_pinLastKey && !m_pinSprite.empty()) {
            // deferred disk write: only the FINAL edited sprite hits disk
            // (a drag would otherwise write one file per degree)
            SaveToDisk(m_pinLastKey, m_pinW, m_pinH, m_pinFmt, m_pinSprite);
        }
        if (m_pin != a_obj) {
            m_pinLastKey = 0;
            m_pinSprite.clear();
        }
        m_pin = a_obj;
    }

    bool IconCache::LoadFicTexture(const std::string& a_path, Icon& a_out,
                                   std::uint32_t a_exactMagic, bool a_makeGlow)
    {
        std::ifstream in(a_path, std::ios::binary);
        if (!in) return false;

        std::uint32_t magic = 0, w = 0, h = 0, fmt = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        in.read(reinterpret_cast<char*>(&w), 4);
        in.read(reinterpret_cast<char*>(&h), 4);
        in.read(reinterpret_cast<char*>(&fmt), 4);
        const bool magicOk = a_exactMagic ? magic == a_exactMagic
                                          : (magic & 0x00FFFFFFu) == 0x00434946u;   // "FIC?"
        if (!in || !magicOk || w == 0 || h == 0 || w > 2048 || h > 2048) return false;

        std::vector<std::uint8_t> pixels(static_cast<size_t>(w) * h * 4);
        in.read(reinterpret_cast<char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
        if (!in) return false;

        return CreateIconTexture(pixels.data(), static_cast<int>(w),
            static_cast<int>(h), fmt, a_makeGlow, a_out);
    }

    namespace
    {
        // One factory for the process. WIC needs COM, and an SKSE plugin cannot
        // assume the thread it runs on was initialised — the game does call
        // CoInitializeEx, but on ITS schedule, and the first icon can be asked
        // for before that. So: try, and if COM says it is not up yet, bring it
        // up on this thread and try once more. Apartment threading matches what
        // the game's UI thread uses; RPC_E_CHANGED_MODE just means somebody
        // already chose, which is fine — WIC works either way.
        IWICImagingFactory* WicFactory()
        {
            static IWICImagingFactory* s_factory = nullptr;
            static bool s_tried = false;
            if (s_tried) return s_factory;
            s_tried = true;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_factory));
            if (hr == CO_E_NOTINITIALIZED) {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_factory));
            }
            if (FAILED(hr)) {
                SKSE::log::error("[ICONS] WIC unavailable (0x{:08X}) — PNG icons off", static_cast<std::uint32_t>(hr));
                s_factory = nullptr;
            }
            return s_factory;
        }
    }

    bool IconCache::LoadPngTexture(const std::string& a_path, Icon& a_out, bool a_makeGlow,
                                   float* a_meanRgb)
    {
        auto* factory = WicFactory();
        if (!factory) return false;

        // widen: WIC takes UTF-16 only, and a load order can put anything in a
        // file name. MultiByteToWideChar with CP_UTF8 keeps non-ASCII names
        // working instead of mangling them through the ANSI code page.
        const int need = MultiByteToWideChar(CP_UTF8, 0, a_path.c_str(), -1, nullptr, 0);
        if (need <= 0) return false;
        std::wstring wide(static_cast<size_t>(need), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, a_path.c_str(), -1, wide.data(), need);

        IWICBitmapDecoder* decoder = nullptr;
        if (FAILED(factory->CreateDecoderFromFilename(wide.c_str(), nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
            return false;   // missing file is the common case — not an error
        }

        bool ok = false;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(frame->GetSize(&w, &h)) && w && h && w <= 2048 && h <= 2048 &&
                SUCCEEDED(factory->CreateFormatConverter(&conv))) {
                // ★Convert to STRAIGHT RGBA, matching what .fic files hold and
                // what CreateIconTexture expects. Asking WIC for a premultiplied
                // format instead would darken every semi-transparent edge once
                // the draw path multiplies again.
                if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
                    if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4,
                            static_cast<UINT>(px.size()), px.data()))) {
                        // ★Measured HERE, off the straight-RGBA buffer, before
                        // CreateIconTexture may premultiply or mip it. Reading
                        // it back off the GPU later would be answering the same
                        // question from a copy that has been through more.
                        if (a_meanRgb) {
                            double sr = 0.0, sg = 0.0, sb = 0.0, sa = 0.0;
                            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                                const double al = px[i + 3] / 255.0;
                                sr += px[i] * al;
                                sg += px[i + 1] * al;
                                sb += px[i + 2] * al;
                                sa += al;
                            }
                            const double n = (sa > 1.0) ? sa : 1.0;
                            a_meanRgb[0] = static_cast<float>(sr / n);
                            a_meanRgb[1] = static_cast<float>(sg / n);
                            a_meanRgb[2] = static_cast<float>(sb / n);
                        }
                        ok = CreateIconTexture(px.data(), static_cast<int>(w),
                            static_cast<int>(h), 28 /*R8G8B8A8_UNORM*/, a_makeGlow, a_out);
                    }
                }
            }
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        decoder->Release();
        return ok;
    }

    namespace
    {
        // ---- pixel style: fixed palette + dot grid ------------------------
        // ★A muted Skyrim-leaning ramp. The palette is the whole reason a
        // DERIVED style still reads as one art set: every sprite, vanilla or
        // modded, is forced through these 36 colours, so nothing can clash.
        // ★NO fixed palette. Every colour complaint this feature has had
        // came from one — rust speckles on plain leather, greens resolving
        // to grey, greys picking up a tint — because a shared ramp always
        // has a nearest entry, and "nearest" across 44 fixed colours is
        // often the wrong hue. The palette is now derived PER ITEM from its
        // own dots (see BuildAdaptivePalette), so every entry is by
        // construction a colour that item actually contains.
        constexpr int kPixelColours = 12;   // entries per item

        // k-means over the item's dot colours. Seeded DETERMINISTICALLY by
        // walking the luma-sorted dots at even intervals: a random seed
        // would give the same item a different palette on every re-derive,
        // and the icon would flicker as the style is toggled.
        void BuildAdaptivePalette(const std::vector<std::array<double, 3>>& a_dots,
                                  std::vector<std::array<double, 3>>& a_out)
        {
            a_out.clear();
            if (a_dots.empty()) return;
            const int k = (std::min)(kPixelColours, static_cast<int>(a_dots.size()));
            std::vector<int> order(a_dots.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
            std::sort(order.begin(), order.end(), [&](int x, int y) {
                const auto lum = [&](int i) {
                    return 0.2126 * a_dots[i][0] + 0.7152 * a_dots[i][1] + 0.0722 * a_dots[i][2];
                };
                return lum(x) < lum(y);
            });
            for (int i = 0; i < k; ++i) {
                a_out.push_back(a_dots[order[(i * 2 + 1) * order.size() / (2 * k)]]);
            }
            std::vector<int> owner(a_dots.size(), 0);
            for (int iter = 0; iter < 12; ++iter) {
                bool moved = false;
                for (std::size_t d = 0; d < a_dots.size(); ++d) {
                    int best = 0; double bd = 1e30;
                    for (int c = 0; c < k; ++c) {
                        const double dr = a_dots[d][0] - a_out[c][0];
                        const double dg = a_dots[d][1] - a_out[c][1];
                        const double db = a_dots[d][2] - a_out[c][2];
                        const double dd = dr * dr + dg * dg + db * db;
                        if (dd < bd) { bd = dd; best = c; }
                    }
                    if (owner[d] != best) { owner[d] = best; moved = true; }
                }
                std::vector<double> sr(k, 0.0), sg(k, 0.0), sb(k, 0.0);
                std::vector<int> n(k, 0);
                for (std::size_t d = 0; d < a_dots.size(); ++d) {
                    const int c = owner[d];
                    sr[c] += a_dots[d][0]; sg[c] += a_dots[d][1]; sb[c] += a_dots[d][2];
                    ++n[c];
                }
                for (int c = 0; c < k; ++c) {
                    if (n[c] == 0) continue;
                    a_out[c] = { sr[c] / n[c], sg[c] / n[c], sb[c] / n[c] };
                }
                if (!moved) break;   // converged
            }
        }

        // ★NO per-channel contrast expansion here — it was tried and reverted.
        // Stretching R, G and B independently about a pivot also stretches the
        // DISTANCE BETWEEN them, i.e. saturation: a leather brown (R>G>B) came
        // out redder, crossed into the palette's rust entries, and speckled
        // the sprite with red dots that exist nowhere in the source. Any tone
        // correction here has to leave chroma alone.

        // src RGBA (w x h) -> dot grid (dw x dh), palette-locked, hard alpha,
        // 1-dot outline. Area-weighted and alpha-weighted for the same reason
        // the mip chain is: averaging straight RGBA drags edge colour toward
        // the transparent margin and rings the sprite.
        std::vector<std::uint8_t> Pixelize(const std::uint8_t* a_src, int a_w, int a_h,
                                           bool a_bgra, int& a_dw, int& a_dh)
        {
            const int ri = a_bgra ? 2 : 0, bi = a_bgra ? 0 : 2;
            // content bounds first: the capture is trimmed, but a derived
            // sprite must fill its dot box or small items lose half their dots
            int x0 = a_w, y0 = a_h, x1 = -1, y1 = -1;
            for (int y = 0; y < a_h; ++y) {
                for (int x = 0; x < a_w; ++x) {
                    if (a_src[(static_cast<size_t>(y) * a_w + x) * 4 + 3] <= 40) continue;
                    x0 = (std::min)(x0, x); x1 = (std::max)(x1, x);
                    y0 = (std::min)(y0, y); y1 = (std::max)(y1, y);
                }
            }
            if (x1 < x0 || y1 < y0) return {};
            const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
            // fit inside the box keeping aspect, one dot of breathing room
            const double s = (std::min)(static_cast<double>(a_dw - 2) / cw,
                                        static_cast<double>(a_dh - 2) / ch);
            const int nw = (std::max)(1, static_cast<int>(cw * s));
            const int nh = (std::max)(1, static_cast<int>(ch * s));

            std::vector<std::uint8_t> out(static_cast<size_t>(a_dw) * a_dh * 4, 0);
            std::vector<std::array<double, 3>> dots;   // pass 1: dot colours
            std::vector<int> slot;                     // ...and where each goes
            const double fx = static_cast<double>(cw) / nw;
            const double fy = static_cast<double>(ch) / nh;
            const int offX = (a_dw - nw) / 2, offY = (a_dh - nh) / 2;

            for (int y = 0; y < nh; ++y) {
                const double sy0 = y0 + y * fy, sy1 = y0 + (y + 1) * fy;
                for (int x = 0; x < nw; ++x) {
                    const double sx0 = x0 + x * fx, sx1 = x0 + (x + 1) * fx;
                    double aw = 0.0, wsum = 0.0, c[3] = { 0, 0, 0 };
                    for (int sy = static_cast<int>(sy0);
                         sy <= (std::min)(a_h - 1, static_cast<int>(std::ceil(sy1)) - 1); ++sy) {
                        const double wy = (std::min)(sy1, sy + 1.0) - (std::max)(sy0, 1.0 * sy);
                        if (wy <= 0.0) continue;
                        for (int sx = static_cast<int>(sx0);
                             sx <= (std::min)(a_w - 1, static_cast<int>(std::ceil(sx1)) - 1); ++sx) {
                            const double wx = (std::min)(sx1, sx + 1.0) - (std::max)(sx0, 1.0 * sx);
                            if (wx <= 0.0) continue;
                            const auto* p = &a_src[(static_cast<size_t>(sy) * a_w + sx) * 4];
                            const double w = wx * wy, av = p[3] * w;
                            wsum += w; aw += av;
                            c[0] += p[ri] * av; c[1] += p[1] * av; c[2] += p[bi] * av;
                        }
                    }
                    if (wsum <= 0.0) continue;
                    const double cov = aw / wsum;
                    if (cov <= 100.0) continue;               // hard alpha: no partial dots
                    double r = c[0] / aw, g = c[1] / aw, b = c[2] / aw;
                    // ★Modest chroma lift around LUMA — not a per-channel
                    // contrast stretch (that one manufactured red dots). The
                    // engine's menu lighting washes colour out, so a green
                    // sack arrives barely green and resolves to grey; lifting
                    // saturation while holding brightness recovers the hue
                    // (green 46%->47%, teal 42%->50%) and, because neutrals
                    // have no chroma to lift, leaves grey exactly where it is.
                    {
                        constexpr double kSat = 1.15;
                        const double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                        r = std::clamp(l + (r - l) * kSat, 0.0, 255.0);
                        g = std::clamp(l + (g - l) * kSat, 0.0, 255.0);
                        b = std::clamp(l + (b - l) * kSat, 0.0, 255.0);
                    }
                    // pass 1 records the colour; the palette does not exist
                    // yet (it is built FROM these dots, just below)
                    dots.push_back({ r, g, b });
                    slot.push_back(static_cast<int>((offY + y)) * a_dw + (offX + x));
                }
            }

            // ---- pass 2: this item's own palette, then quantise to it ----
            std::vector<std::array<double, 3>> pal;
            BuildAdaptivePalette(dots, pal);
            for (std::size_t d = 0; d < dots.size(); ++d) {
                int best = 0; double bd = 1e30;
                for (std::size_t c = 0; c < pal.size(); ++c) {
                    const double dr = dots[d][0] - pal[c][0];
                    const double dg = dots[d][1] - pal[c][1];
                    const double db = dots[d][2] - pal[c][2];
                    const double dd = dr * dr + dg * dg + db * db;
                    if (dd < bd) { bd = dd; best = static_cast<int>(c); }
                }
                auto* dp = &out[static_cast<size_t>(slot[d]) * 4];
                dp[ri] = static_cast<std::uint8_t>(std::lround(std::clamp(pal[best][0], 0.0, 255.0)));
                dp[1]  = static_cast<std::uint8_t>(std::lround(std::clamp(pal[best][1], 0.0, 255.0)));
                dp[bi] = static_cast<std::uint8_t>(std::lround(std::clamp(pal[best][2], 0.0, 255.0)));
                dp[3]  = 255;
            }
            // one-dot outline so the silhouette reads against the cell
            std::vector<std::uint8_t> ol = out;
            for (int y = 0; y < a_dh; ++y) {
                for (int x = 0; x < a_dw; ++x) {
                    auto* dp = &ol[(static_cast<size_t>(y) * a_dw + x) * 4];
                    if (dp[3] != 0) continue;
                    bool nb = false;
                    if (x > 0)          nb |= out[(static_cast<size_t>(y) * a_dw + x - 1) * 4 + 3] != 0;
                    if (!nb && x < a_dw-1) nb |= out[(static_cast<size_t>(y) * a_dw + x + 1) * 4 + 3] != 0;
                    if (!nb && y > 0)   nb |= out[(static_cast<size_t>(y-1) * a_dw + x) * 4 + 3] != 0;
                    if (!nb && y < a_dh-1) nb |= out[(static_cast<size_t>(y+1) * a_dw + x) * 4 + 3] != 0;
                    if (!nb) continue;
                    dp[ri] = 0x0D; dp[1] = 0x0B; dp[bi] = 0x0A; dp[3] = 255;
                }
            }
            // ★Trim to the dots that carry content — AFTER the outline, so
            // the border survives the cut. The realistic sprite is alpha-
            // trimmed and the tile scales whatever it draws by the sprite's
            // LONG SIDE, so returning the full dot box (content fitted
            // inside it with a margin) drew every pixel icon several
            // percent smaller than its realistic twin.
            int tx0 = a_dw, ty0 = a_dh, tx1 = -1, ty1 = -1;
            for (int y = 0; y < a_dh; ++y) {
                for (int x = 0; x < a_dw; ++x) {
                    if (ol[(static_cast<size_t>(y) * a_dw + x) * 4 + 3] == 0) continue;
                    tx0 = (std::min)(tx0, x); tx1 = (std::max)(tx1, x);
                    ty0 = (std::min)(ty0, y); ty1 = (std::max)(ty1, y);
                }
            }
            if (tx1 < tx0 || ty1 < ty0) return ol;
            const int tw = tx1 - tx0 + 1, th = ty1 - ty0 + 1;
            if (tw == a_dw && th == a_dh) return ol;
            std::vector<std::uint8_t> cut(static_cast<size_t>(tw) * th * 4);
            for (int y = 0; y < th; ++y) {
                std::memcpy(&cut[(static_cast<size_t>(y) * tw) * 4],
                            &ol[(static_cast<size_t>(ty0 + y) * a_dw + tx0) * 4],
                            static_cast<size_t>(tw) * 4);
            }
            a_dw = tw;
            a_dh = th;
            return cut;
        }

        // pak pixels for one key, without building a texture
        bool ReadPakPixels(std::uint64_t a_key, std::vector<std::uint8_t>& a_px,
                           int& a_w, int& a_h, std::uint32_t& a_fmt)
        {
            ScanPak();
            const auto it = g_pakIndex.find(a_key);
            if (it == g_pakIndex.end()) return false;
            std::ifstream in(kPakPath, std::ios::binary);
            if (!in) return false;
            in.seekg(static_cast<std::streamoff>(it->second.off + it->second.hdrSize()));
            a_px.resize(it->second.len);
            if (!in.read(reinterpret_cast<char*>(a_px.data()),
                    static_cast<std::streamsize>(a_px.size()))) {
                return false;
            }
            a_w = static_cast<int>(it->second.w);
            a_h = static_cast<int>(it->second.h);
            a_fmt = it->second.fmt;
            // ★Same rejection as CreateIconTexture, for the pixel-style derive
            // path that reads the pak directly instead of going through it.
            if (a_fmt == static_cast<std::uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM) ||
                a_fmt == static_cast<std::uint32_t>(DXGI_FORMAT_R10G10B10A2_UINT)) {
                return false;
            }
            return true;
        }
    }

    bool IconCache::DerivePixelIcon(std::uint64_t a_key, int a_cw, int a_ch)
    {
        std::vector<std::uint8_t> px;
        int w = 0, h = 0;
        std::uint32_t fmt = 0;
        if (!ReadPakPixels(a_key, px, w, h, fmt)) {
            // ★LIVE EDIT: a pinned item's captures are deliberately kept OFF
            // disk (a drag would write one file per degree), so the pak lookup
            // misses on exactly the key the editor is working on. Falling back
            // to the in-memory sprite is what makes a rotation edit visible in
            // THIS style — without it the tile quietly reverted to the
            // realistic sprite for the whole edit, and the dot version could
            // only be judged after leaving the item.
            if (a_key != m_pinLastKey || m_pinSprite.empty()) return false;
            px = m_pinSprite;
            w = m_pinW;
            h = m_pinH;
            fmt = m_pinFmt;
        }
        const bool bgra = (fmt == 87 || fmt == 88);
        int dw = (std::max)(1, a_cw) * kPixelDots;
        int dh = (std::max)(1, a_ch) * kPixelDots;
        auto dots = Pixelize(px.data(), w, h, bgra, dw, dh);
        if (dots.empty()) return false;

        // ★Blow the dots up 4x with NEAREST before handing them over. The UI
        // samples linearly with a mip chain, and a 24-dot sprite stretched
        // across a 46px cell that way comes out soft — the one thing pixel
        // art cannot be. Enlarging first means the chain's own levels land
        // ON the dot grid (96 -> 48 -> 24), so whichever level the GPU picks
        // for the cell is a clean multiple and the edges stay hard. Cheaper
        // than a second sampler: that would need a callback pair per icon.
        constexpr int kZoom = 4;
        const int zw = dw * kZoom, zh = dh * kZoom;
        std::vector<std::uint8_t> big(static_cast<size_t>(zw) * zh * 4);
        for (int y = 0; y < zh; ++y) {
            const auto* srow = &dots[(static_cast<size_t>(y / kZoom) * dw) * 4];
            auto* drow = &big[(static_cast<size_t>(y) * zw) * 4];
            for (int x = 0; x < zw; ++x) {
                std::memcpy(&drow[x * 4], &srow[(x / kZoom) * 4], 4);
            }
        }

        Icon icon;
        // no glow sprite: a dot silhouette with a halo reads as a smudge
        if (!CreateIconTexture(big.data(), zw, zh, fmt, false, icon)) return false;
        // a re-derive (footprint change / live edit) REPLACES the entry — free
        // the previous texture or every editor drag leaks one
        if (auto old = m_pixelIcons.find(a_key); old != m_pixelIcons.end()) {
            ReleaseIcon(old->second);
        }
        m_pixelIcons[a_key] = icon;
        return true;
    }

    void IconCache::TickPixelDerive()
    {
        // deferred from SetStyle — safe here, outside any draw list
        if (m_pixelDropPending) {
            m_pixelDropPending = false;
            ReleasePixelIcons();
            m_pixelCells.clear();
        }
        if (m_style != Style::kPixel) return;
        int budget = kPixelPerFrame;
        while (budget > 0 && !m_pixelQueue.empty()) {
            const auto key = m_pixelQueue.front();
            m_pixelQueue.pop_front();
            m_pixelQueued.erase(key);
            // NO "already derived -> skip" here: Get() queues a key it already
            // has a sprite for when the footprint changed under it, and the
            // editor re-queues the pinned key on every capture. Both need the
            // derivation to actually RUN and replace the entry.
            const auto cit = m_pixelCells.find(key);
            const int cw = cit == m_pixelCells.end() ? 1 : cit->second.first;
            const int ch = cit == m_pixelCells.end() ? 1 : cit->second.second;
            if (!DerivePixelIcon(key, cw, ch)) {
                // nothing in the pak yet (capture still pending) — the grid
                // keeps showing the realistic sprite and asks again later
                continue;
            }
            --budget;
        }
    }

    void IconCache::ReleasePixelIcons()
    {
        for (auto& [k, ic] : m_pixelIcons) ReleaseIcon(ic);
        m_pixelIcons.clear();
        m_pixelQueue.clear();
        m_pixelQueued.clear();
    }

    bool IconCache::LoadFromDisk(std::uint64_t a_key) const
    {
        ScanPak();
        const auto it = g_pakIndex.find(a_key);
        if (it == g_pakIndex.end()) return false;

        std::ifstream in(kPakPath, std::ios::binary);
        if (!in) return false;
        in.seekg(static_cast<std::streamoff>(it->second.off + it->second.hdrSize()));
        std::vector<std::uint8_t> px(it->second.len);
        if (!in.read(reinterpret_cast<char*>(px.data()),
                static_cast<std::streamsize>(px.size()))) {
            return false;
        }

        Icon icon;
        if (!CreateIconTexture(px.data(), static_cast<int>(it->second.w),
                static_cast<int>(it->second.h), it->second.fmt, true, icon)) {
            return false;
        }
        m_icons[a_key] = icon;
        return true;
    }

    void IconCache::SetStyle(Style a_style)
    {
        // ★Leaving pixel style frees its derived textures: they are cheap to
        // rebuild (one pak read + a downscale) and there is no reason to hold
        // a second copy of every icon in VRAM for a style nobody is looking
        // at. Re-entering re-derives on the frame budget.
        // ★★But NOT here. This runs from the settings button, i.e. from
        // inside the ImGui frame, and the frame's draw list is already
        // holding the very SRVs this would release — the grid drew its dot
        // sprites moments earlier. Releasing them mid-frame hands the
        // renderer a dead view and the crash lands inside the D3D driver
        // with no frame of ours on the stack. Hand it to Tick instead, the
        // same way the cache reset and the drawn-icon reload already do.
        if (m_style == Style::kPixel && a_style != Style::kPixel) {
            m_pixelDropPending = true;
        }
        m_style = a_style;
    }

    void IconCache::SaveToDisk(std::uint64_t a_key, int a_w, int a_h, std::uint32_t a_fmt,
                               const std::vector<std::uint8_t>& a_pixels)
    {
        ScanPak();   // index/migration must exist before the first append
        AppendToPakRaw(a_key, static_cast<std::uint32_t>(a_w),
            static_cast<std::uint32_t>(a_h), a_fmt, a_pixels.data(),
            static_cast<std::uint32_t>(a_pixels.size()));
    }

    // THREADING NOTE (B11): g_pakIndex/g_pakOut are unsynchronized. This is
    // safe because every access path runs on the MAIN thread — Tick (player
    // Update hook / AdvanceMovie) and PostDisplay (menu render) never overlap.
    // If a worker thread ever touches the pak, add a mutex here first.
    void IconCache::ResetDiskCache()
    {
        // NEVER call from inside the ImGui frame: Clear() releases SRVs the
        // current draw list may still reference (consumed from UIRoot::Tick)
        Clear();
        g_pakIndex.clear();
        g_pakScanned = true;   // stays empty until new captures append
        ClosePakHandle();      // the remove below fails on an open handle
        std::error_code ec;
        std::filesystem::remove(kPakPath, ec);
        std::filesystem::remove(kPakTmp, ec);
        std::filesystem::remove(kFailPath, ec);      // failed keys get a fresh chance
        std::filesystem::remove(kSlowPath, ec);      // GI68: and the deferred ones
        m_deferred.clear();
        m_deferredObj.clear();
        m_slowLoaded = false;
        // GI60: the retired "stylized" derivative. The style is gone, so the
        // file is dead weight in the player's folder — sweep it here.
        std::filesystem::remove("Data/SKSE/Plugins/GridInventory_icons_styl.pak", ec);
        std::filesystem::remove_all(kIconDir, ec);   // legacy leftovers too
        SKSE::log::info("[ICONS] disk cache reset (retexture refresh)");
    }

    namespace
    {
        // ★The SAME normalisation the key folds with, or every answer here
        // would disagree with the thing it is answering about: a record whose
        // two paths differ only by "meshes\" or a slash is ONE picture to the
        // key, and must not be treated as two.
        [[nodiscard]] std::string NormModelPath(const char* a_p)
        {
            std::string s(a_p ? a_p : "");
            if (_strnicmp(s.c_str(), "meshes", 6) == 0 &&
                (s.size() > 6 && (s[6] == '\\' || s[6] == '/'))) {
                s.erase(0, 7);
            }
            for (auto& c : s) {
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c == '/') c = '\\';
            }
            return s;
        }

        // Two ground models that are BOTH present and DIFFERENT: the picture
        // depends on who is wearing it, and one cached icon cannot serve both.
        // One side empty is not two pictures -- the engine falls back to the
        // model that exists and both sexes see it.
        [[nodiscard]] bool IsSexSpecific(RE::TESObjectARMO* a_armo)
        {
            if (!a_armo) return false;
            const std::string m = NormModelPath(
                a_armo->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel());
            const std::string f = NormModelPath(
                a_armo->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel());
            return !m.empty() && !f.empty() && m != f;
        }

        // Can this record's icon ever be shown? NOT Capturable(), which is the
        // capture QUEUE's gate and answers false for the entire flat style --
        // the question here is what a PAK would ship, not what one player's
        // style draws.
        [[nodiscard]] bool ShippableItem(RE::TESBoundObject* a_obj)
        {
            if (!a_obj || !a_obj->GetPlayable() || IsUnobtainable(a_obj)) return false;
            const char* nm = a_obj->GetName();
            return nm && *nm;
        }

        // ★★★WHOSE ITEM IS THIS, and it is asked because a shipped pak is
        // built on ONE machine's load order.
        //
        // A full precache photographs everything the author happens to have
        // installed, so the bundle that goes out carries icons for armour packs
        // and gear mods most downloaders do not own. They pay for those in
        // megabytes and can never see them. Measured on this build: 4649 icons
        // against the 2364 of the version before, and nearly all of the
        // difference was one machine's private list.
        //
        // So the shipping pak keeps only what everybody has: the base game, its
        // official add-ons, Creation Club content (official, and free with the
        // Anniversary edition), and our own plugin. Anyone running something
        // else captures it themselves on first sight, which is what already
        // happens today for anything the author did not own either.
        [[nodiscard]] bool ShippableSource(RE::TESForm* a_form)
        {
            const auto* file = a_form ? a_form->GetFile(0) : nullptr;
            if (!file) return false;   // dynamic / runtime form: nobody else has it
            const std::string_view name = file->GetFilename();
            static constexpr std::string_view kBase[] = {
                "Skyrim.esm", "Update.esm", "Dawnguard.esm",
                "HearthFires.esm", "Dragonborn.esm",
            };
            for (const auto& b : kBase) {
                if (name.size() == b.size() &&
                    _strnicmp(name.data(), b.data(), b.size()) == 0) {
                    return true;
                }
            }
            // ★Creation Club ships as cc<code>-<name>.esl/.esm. Official, and
            // the free ones arrive with every Anniversary install.
            if (name.size() > 2 && _strnicmp(name.data(), "cc", 2) == 0) return true;
            return name.starts_with("Grid Inventory.");
        }

        // Model slots reachable from a form ANY downloader could have. A slot
        // used by even one such form stays -- it is the same picture whoever
        // asks for it.
        [[nodiscard]] std::unordered_set<std::uint32_t> ShippableSlots(int* a_forms)
        {
            std::unordered_set<std::uint32_t> keep;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return keep;
            int n = 0;
            const auto sweep = [&](const auto& a_arr) {
                for (auto* form : a_arr) {
                    auto* obj = form ? form->template As<RE::TESBoundObject>() : nullptr;
                    if (!obj || !ShippableSource(form)) continue;
                    ++n;
                    keep.insert(ModelSlot32(obj));
                }
            };
            sweep(dh->GetFormArray<RE::TESObjectWEAP>());
            sweep(dh->GetFormArray<RE::TESObjectARMO>());
            sweep(dh->GetFormArray<RE::TESAmmo>());
            sweep(dh->GetFormArray<RE::AlchemyItem>());
            sweep(dh->GetFormArray<RE::IngredientItem>());
            sweep(dh->GetFormArray<RE::TESObjectBOOK>());
            sweep(dh->GetFormArray<RE::TESObjectMISC>());
            sweep(dh->GetFormArray<RE::TESSoulGem>());
            sweep(dh->GetFormArray<RE::TESKey>());
            sweep(dh->GetFormArray<RE::ScrollItem>());
            sweep(dh->GetFormArray<RE::SpellItem>());
            if (a_forms) *a_forms = n;
            return keep;
        }

        // The upper half of every icon key (see KeyFor) -- so one entry here
        // retires a record's icon in EVERY rotation it was ever captured at.
        [[nodiscard]] std::unordered_set<std::uint32_t> SexSpecificSlots(int* a_count)
        {
            std::unordered_set<std::uint32_t> out;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return out;
            int n = 0;
            for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
                auto* obj = armo ? armo->As<RE::TESBoundObject>() : nullptr;
                if (!ShippableItem(obj) || !IsSexSpecific(armo)) continue;
                ++n;
                out.insert(ModelSlot32(obj));
            }
            if (a_count) *a_count = n;
            return out;
        }
    }

    void IconCache::ReportSexSpecificArmour()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;

        int total = 0, differ = 0, oneSided = 0;
        std::string sample;
        int shown = 0;
        for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
            auto* obj = armo ? armo->As<RE::TESBoundObject>() : nullptr;
            if (!ShippableItem(obj)) continue;
            ++total;
            const std::string m = NormModelPath(
                armo->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel());
            const std::string f = NormModelPath(
                armo->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel());
            if (m == f) continue;
            if (m.empty() || f.empty()) { ++oneSided; continue; }
            ++differ;
            if (shown < 12) {
                ++shown;
                sample += "\n           ";
                sample += armo->GetName();
                sample += " (";
                sample += std::to_string(armo->GetFormID());
                sample += ")";
            }
        }
        SKSE::log::info(
            "[ICONS] sex-specific armour: {} of {} capturable ARMO have two "
            "DIFFERENT ground models ({} more have only one side, which is "
            "fine -- both sexes see it).{}{}",
            differ, total, oneSided,
            differ ? "  First few:" : "", sample);
    }

    void IconCache::ReportSpellDisplayObjects()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;

        // A model path is what a capture needs; an MDOB pointing at something
        // with no nif is coverage on paper only.
        const auto hasModel = [](RE::TESBoundObject* a_o) {
            if (!a_o) return false;
            const auto* m = a_o->As<RE::TESModel>();
            return m && m->GetModel() && m->GetModel()[0];
        };

        int shown = 0, own = 0, viaEffect = 0, none = 0;
        std::string sample;
        int listed = 0;
        for (auto* sp : dh->GetFormArray<RE::SpellItem>()) {
            if (!sp) continue;
            // Only what a player can actually pick in the wheel. Abilities,
            // diseases and enchantments are carried by other things and never
            // appear as a choice, so counting them would flatter the answer.
            const auto t = sp->GetSpellType();
            if (t != RE::MagicSystem::SpellType::kSpell &&
                t != RE::MagicSystem::SpellType::kPower &&
                t != RE::MagicSystem::SpellType::kLesserPower) {
                continue;
            }
            const char* nm = sp->GetName();
            if (!nm || !*nm) continue;
            ++shown;

            if (hasModel(sp->GetMenuDisplayObject())) { ++own; continue; }
            // ★The spell's own MDOB is usually EMPTY in vanilla -- the picture
            // lives on the magic EFFECT, which is what the magic menu falls
            // back to. First effect: that is the one the menu shows, and a
            // spell's identity in a list is its primary effect anyway.
            bool viaFx = false;
            for (const auto* e : sp->effects) {
                if (!e || !e->baseEffect) continue;
                if (hasModel(e->baseEffect->GetMenuDisplayObject())) { viaFx = true; }
                break;   // FIRST effect only, like the menu
            }
            if (viaFx) { ++viaEffect; continue; }
            ++none;
            if (listed < 12) {
                ++listed;
                sample += "\n           ";
                sample += nm;
            }
        }
        SKSE::log::info(
            "[ICONS] spell display objects: {} pickable spells -- {} have their "
            "own MDOB with a model, {} inherit one from their first effect, {} "
            "have none.{}{}",
            shown, own, viaEffect, none,
            none ? "  Without:" : "", sample);
    }

    void IconCache::QueueFavouriteSpells()
    {
        auto* fav = RE::MagicFavorites::GetSingleton();
        if (!fav) return;
        int asked = 0;
        for (auto* form : fav->spells) {
            // ★Shouts live in this list too and are NOT bound objects (TESShout
            // is a TESForm), so they cannot be photographed by this path at
            // all. They keep their drawn icon; asking would be a null deref.
            auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!obj || !obj->As<RE::SpellItem>()) continue;
            // Capturable() already refuses one with no display object, so the
            // 132 script spells cost nothing but this loop.
            QueueCapture(obj);
            ++asked;
        }
        if (asked > 0) {
            SKSE::log::info("[ICONS] {} favourite spell(s) offered to the "
                            "capture queue", asked);
        }
    }

    bool IconCache::ExportShippingPak(const char* a_path)
    {
        // ★★★THE PAK THAT GOES TO OTHER PEOPLE, minus the icons that are only
        // right for the character who captured them.
        //
        // Measured on a real load order: 268 of 4386 shippable armours have two
        // different ground models. Those 268 cannot be shipped -- one cached
        // icon per record, rendered as whoever pressed the button -- so they
        // are left out and each install captures them on its OWN character,
        // where the engine picks the right one for free. The cost is one
        // capture per record the player actually sees, appended to their own
        // pak, so it does not repeat.
        //
        // The other 4118 are unaffected: a shared ground model looks the same
        // on everybody, and dropping those would be pure waste.
        ClosePakHandle();
        std::error_code ec;
        if (!std::filesystem::exists(kPakPath, ec)) {
            SKSE::log::error("[ICONS] shipping export: no capture pak here");
            return false;
        }
        if (!g_pakScanned) ScanPak();

        int  records = 0, shipForms = 0;
        const auto drop = SexSpecificSlots(&records);
        const auto keep = ShippableSlots(&shipForms);

        std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
        std::ifstream in(kPakPath, std::ios::binary);
        if (!out || !in) {
            SKSE::log::error("[ICONS] shipping export: cannot open files");
            return false;
        }
        std::vector<std::uint8_t> px;
        std::size_t kept = 0, dropped = 0, foreign = 0;
        // Same record layout CompactPak writes -- this IS that loop with one
        // condition added, and the two must not drift apart.
        for (const auto& [key, en] : g_pakIndex) {
            const auto slot = static_cast<std::uint32_t>(key >> 32);
            if (drop.contains(slot)) {
                ++dropped;
                continue;
            }
            // ★...and anything only THIS machine's load order can reach.
            if (!keep.contains(slot)) {
                ++foreign;
                continue;
            }
            px.resize(en.len);
            in.seekg(static_cast<std::streamoff>(en.off + en.hdrSize()));
            if (!in.read(reinterpret_cast<char*>(px.data()), en.len)) {
                SKSE::log::error("[ICONS] shipping export: short read at key {:016X}", key);
                return false;
            }
            const std::uint32_t magic = en.hasRot ? kIconMagicRot : kIconMagic;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&key), 8);
            out.write(reinterpret_cast<const char*>(&en.w), 4);
            out.write(reinterpret_cast<const char*>(&en.h), 4);
            out.write(reinterpret_cast<const char*>(&en.fmt), 4);
            if (en.hasRot) out.write(reinterpret_cast<const char*>(en.rot), 36);
            out.write(reinterpret_cast<const char*>(&en.len), 4);
            out.write(reinterpret_cast<const char*>(px.data()), en.len);
            if (!out) {
                SKSE::log::error("[ICONS] shipping export: write failed");
                return false;
            }
            ++kept;
        }
        in.close();
        out.close();
        SKSE::log::info("[ICONS] shipping pak written: {} kept, {} dropped "
                        "({} sex-specific armour records), {} left out as "
                        "third-party ({} shippable forms seen) -> {}",
                        kept, dropped, records, foreign, shipForms, a_path);
        return true;
    }

    bool IconCache::ExportPakTo(const char* a_path)
    {
        ClosePakHandle();   // flush pending appends before the copy
        std::error_code ec;
        if (!std::filesystem::exists(kPakPath, ec)) {
            // no captures yet: make sure no STALE bundle rides along either
            std::filesystem::remove(a_path, ec);
            SKSE::log::info("[ICONS] preset export: no capture pak to bundle");
            return false;
        }
        std::filesystem::copy_file(kPakPath, a_path,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            SKSE::log::error("[ICONS] preset pak copy failed: {}", ec.message());
            return false;
        }
        SKSE::log::info("[ICONS] preset pak bundled ({} keys)", g_pakIndex.size());
        return true;
    }

    bool IconCache::MergePak(const char* a_path)
    {
        std::ifstream in(a_path, std::ios::binary);
        if (!in) return false;
        // sanity: the stream must OPEN with a known record magic, or we would
        // append garbage the scanner then trips over
        std::uint32_t magic = 0;
        if (!in.read(reinterpret_cast<char*>(&magic), 4) ||
            !IsKnownIconMagic(magic)) {
            SKSE::log::error("[ICONS] preset pak rejected (bad magic)");
            return false;
        }
        in.seekg(0);
        ClosePakHandle();
        {
            std::ofstream out(kPakPath, std::ios::binary | std::ios::app);
            if (!out) return false;
            out << in.rdbuf();
        }
        // frame-outside only: SRVs drop here, tiles reload lazily from the
        // merged pak (same visual as a cache reset, minus the re-captures)
        Clear();
        g_pakScanned = false;
        ScanPak();
        SKSE::log::info("[ICONS] preset pak merged -> {} keys indexed",
            g_pakIndex.size());
        return true;
    }

    // ★GI68b: a list written before the deferred split is not trustworthy —
    // back then ANY straggler was recorded as a permanent failure, including
    // items that were merely still loading. Those keys are skipped forever and
    // the user has no way to know which ones deserve another look, so a list
    // without the marker is discarded wholesale. Costs one re-capture attempt
    // per key; recovers every icon the old logic wrote off by mistake.
    // Bumped whenever the rules that PUT a key here change, because entries
    // written under the old rules are no longer evidence of anything. v3: the
    // empty-world-model and missing-mesh cases are now caught before a capture
    // is ever armed, so anything the old gate recorded deserves a clean look.
    static constexpr const char* kFailVer = "; ver 3";

    void IconCache::EnsureFailLoaded()
    {
        if (m_failLoaded) return;
        m_failLoaded = true;
        std::ifstream in(kFailPath);
        if (!in) return;
        std::string line;
        int  n = 0;
        bool versioned = false;
        while (std::getline(in, line)) {
            if (line.starts_with(kFailVer)) {
                versioned = true;
                continue;
            }
            if (line.empty() || line[0] == ';') continue;
            const auto key = std::strtoull(line.c_str(), nullptr, 16);
            if (key) {
                m_failed.insert(key);
                ++n;
            }
        }
        in.close();
        if (!versioned) {
            std::error_code ec;
            std::filesystem::remove(kFailPath, ec);
            SKSE::log::info(
                "[ICONS] fail list discarded ({} pre-GI68 keys) - they get another chance", n);
            m_failed.clear();
            return;
        }
        if (n) {
            SKSE::log::info("[ICONS] {} permanently-failed capture keys loaded", n);
        }
    }

    void IconCache::PersistFail(std::uint64_t a_key)
    {
        // ios::ate, not bare ios::app: MSVC's append stream only seeks to the
        // end AT WRITE TIME, so tellp() on a freshly opened app-mode file
        // reports 0 no matter how much is already in it. Without ate, the
        // "file is empty -> write the header" test was true on every single
        // append and a 16-key list came out 48 lines long.
        std::ofstream out(kFailPath, std::ios::app | std::ios::ate);
        if (!out) return;
        if (out.tellp() == 0) {
            out << kFailVer << "\n";
            out << "; Capture keys that permanently failed - never retried across sessions\n";
            out << "; 캡처가 계속 실패한 키 목록 - 재접속 후에도 재시도하지 않습니다 (캐시 초기화 시 함께 삭제)\n";
        }
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%016llX\n",
            static_cast<unsigned long long>(a_key));
        out << buf;
    }

    // ---- GI68: the deferred list ------------------------------------------
    // Same file format as the fail list, different meaning: these WILL be
    // retried, just not during a pass that other items are waiting on.

    void IconCache::EnsureSlowLoaded()
    {
        if (m_slowLoaded) return;
        m_slowLoaded = true;
        std::ifstream in(kSlowPath);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';') continue;
            if (const auto key = std::strtoull(line.c_str(), nullptr, 16); key) {
                m_deferred.insert(key);
            }
        }
        if (!m_deferred.empty()) {
            SKSE::log::info("[ICONS] {} deferred (slow) capture keys loaded",
                m_deferred.size());
        }
    }

    void IconCache::PersistSlow(std::uint64_t a_key)
    {
        std::ofstream out(kSlowPath, std::ios::app | std::ios::ate);   // see PersistFail
        if (!out) return;
        if (out.tellp() == 0) {
            out << "; Captures that ran out of time while still loading - retried on request\n";
            out << "; 로딩 중에 시간이 부족했던 아이콘 - 설정에서 '다시 시도'를 누르면 재처리합니다\n";
        }
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%016llX\n",
            static_cast<unsigned long long>(a_key));
        out << buf;
    }

    void IconCache::RewriteSlow()
    {
        std::error_code ec;
        if (m_deferred.empty()) {
            std::filesystem::remove(kSlowPath, ec);
            return;
        }
        std::ofstream out(kSlowPath, std::ios::trunc);
        if (!out) return;
        out << "; Captures that ran out of time while still loading - retried on request\n";
        out << "; 로딩 중에 시간이 부족했던 아이콘 - 설정에서 '다시 시도'를 누르면 재처리합니다\n";
        char buf[24];
        for (const auto k : m_deferred) {
            std::snprintf(buf, sizeof(buf), "%016llX\n",
                static_cast<unsigned long long>(k));
            out << buf;
        }
    }

    size_t IconCache::DeferredCount()
    {
        EnsureSlowLoaded();
        // entries whose icon has since been captured another way are not owed a
        // retry -- count only what is still missing
        size_t n = 0;
        for (const auto k : m_deferred) {
            if (!m_icons.contains(k) && !g_pakIndex.contains(k)) ++n;
        }
        return n;
    }

    size_t IconCache::RetryDeferred()
    {
        EnsureSlowLoaded();
        if (m_deferred.empty()) return 0;
        // ★The form pointers are only known for keys deferred THIS session; a
        // list read from disk carries keys but no objects, so there is nothing
        // to re-queue for those. They are DROPPED rather than left on the list:
        // the button reports how many items it still owes a retry, and a key it
        // can never act on would keep that number up forever and do nothing
        // when pressed. Dropped is also the right outcome -- with the key gone
        // the item takes a fresh full window the next time it is queued
        // normally, which is the retry it was owed.
        const size_t before = m_deferred.size();
        size_t       n      = 0;
        m_retryPass = true;
        for (auto it = m_deferred.begin(); it != m_deferred.end();) {
            const auto k   = *it;
            const auto obj = m_deferredObj.find(k);
            if (obj == m_deferredObj.end() || !obj->second) {
                it = m_deferred.erase(it);
                continue;
            }
            ++it;
            if (m_icons.contains(k) || m_queued.contains(k)) continue;
            m_queue.push_back(Pending{ obj->second, obj->second->GetFormID(), k, false, 0.0f });
            m_queued.insert(k);
            ++n;
        }
        SKSE::log::info("[ICONS] retry pass: {} of {} deferred re-queued ({} dropped, no form)",
            n, before, before - m_deferred.size());
        return n;
    }

    void IconCache::ClearDeferred()
    {
        m_deferred.clear();
        m_deferredObj.clear();
        m_slowLoaded = true;
        RewriteSlow();
    }

    namespace
    {
        // Leveled-item stubs (unresolved LVLI entries inside merchant chests /
        // containers) have no model; feeding one to Inv3D::Load makes the
        // engine's NewInventoryMenuItemLoadTask deref a null model and CTD.
        // The grid never draws them (name/playable filtered), but Prefetch
        // walks raw GetInventory() output — gate every queue entry here.
        bool Capturable(RE::TESBoundObject* a_in)
        {
            // ★Judge the thing that will actually be rendered. A spell asked
            // about itself answers "no model" and is refused for ever; asked
            // about its display object it answers with a flame.
            RE::TESBoundObject* a_obj = CaptureSourceOf(a_in);
            if (!a_obj || a_obj->Is(RE::FormType::LeveledItem)) return false;
            // GI52 flat style: nothing is ever drawn from a capture, so don't
            // spend a single engine render on one. This is what makes the
            // style's "no first scan" claim literally true.
            if (IconCache::GetSingleton()->FlatStyle()) return false;
            // ★GI51: an empty world model can NEVER render, so queueing it only
            // buys 20-45 frames of waiting x up to 4 retries before the gates
            // give up -- each. On a heavily modded load order that dead weight
            // is a large part of what makes a first scan feel endless. This
            // used to be checked in PrecacheAll only, which is the one path
            // that already knew better; the two paths that actually run during
            // play did not.
            if (auto* mdl = a_obj->As<RE::TESModel>();
                mdl && (!mdl->GetModel() || !mdl->GetModel()[0])) {
                return false;
            }
            // ★GI68c: ...and the line above is BLIND TO ARMOR. ARMO does not
            // inherit TESModel at all (see TESObjectARMO's base list: it takes
            // TESBipedModelForm instead), so As<TESModel> returns null for
            // every single piece of armor and the guard silently never fires.
            // Body/skin slots -- nude bodies, skeletons, "hide underwear"
            // placeholders -- carry no ground model, so there is nothing to
            // render and no amount of waiting produces one. Two measured full
            // precache runs ended with the SAME 16 timeouts in the SAME order,
            // and 12 of them were exactly this: 45 frames each, 9 seconds of a
            // 71-second scan spent proving a blank string is still blank.
            if (auto* bip = a_obj->As<RE::TESBipedModelForm>()) {
                const char* m = bip->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
                const char* f = bip->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel();
                if ((!m || !m[0]) && (!f || !f[0])) return false;
            }
            if (IsUnobtainable(a_obj)) return false;
            return true;
        }

        // EVERY nif the engine could draw this object from -- an object is only
        // unrenderable when all of them are gone.
        //
        // ★A weapon has TWO sources: MODL, and the 1st-person STAT in WNAM
        // that the loader falls back to. Measured: a Dark Souls weapon whose
        // MODL pointed into an uninstalled mesh pack ('Xerperious\DS3\...')
        // rendered perfectly from its WNAM every run -- until a first draft of
        // this probe judged it on MODL alone and threw the working icon away.
        // ARMO likewise keeps its paths on TESBipedModelForm, one per sex.
        void ModelPathsOf(RE::TESBoundObject* a_in, std::vector<const char*>& a_out)
        {
            // Same substitution the key makes -- a spell's nif is its display
            // object's (CaptureSourceOf).
            RE::TESBoundObject* a_obj = CaptureSourceOf(a_in);
            if (!a_obj) return;
            const auto add = [&a_out](const char* p) {
                if (p && p[0]) a_out.push_back(p);
            };
            if (auto* bip = a_obj->As<RE::TESBipedModelForm>()) {
                add(bip->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel());
                add(bip->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel());
            }
            if (auto* mdl = a_obj->As<RE::TESModel>()) {
                add(mdl->GetModel());
            }
            if (auto* weap = a_obj->As<RE::TESObjectWEAP>();
                weap && weap->firstPersonModelObject) {
                if (auto* fp = weap->firstPersonModelObject->As<RE::TESModel>()) {
                    add(fp->GetModel());
                }
            }
        }

        // First named path, for diagnostics only.
        const char* ModelPathOf(RE::TESBoundObject* a_obj)
        {
            std::vector<const char*> paths;
            ModelPathsOf(a_obj, paths);
            return paths.empty() ? "<none>" : paths.front();
        }

        // ★GI68c: does the nif this record names actually EXIST? A record can
        // carry a perfectly well-formed path to a file that was never shipped,
        // and the engine's loader then returns without creating anything --
        // indistinguishable, from the outside, from a load that is merely slow.
        // The only way to tell them apart is to ask the archive directly.
        //
        // All four survivors of the armor fix turned out to be this, and three
        // of them are Bethesda's own: the vanilla Coinpurse records point at
        // 'Clutter\CoinBagLarge.nif' while the mesh actually ships as
        // 'meshes\plants\coinbaglarge.nif'. Cut content, wrong folder, never
        // fixed. Worth catching in general, not for these four: a load order
        // missing a mod's meshes has HUNDREDS of them, and each one used to
        // cost 45 frames of waiting to learn nothing.
        //
        // Memoised per PATH, not per item — a load order full of retextures
        // shares the same handful of nifs — so the archive is hit once each.
        bool PathMissing(const char* a_rel)
        {
            std::string path = a_rel;
            for (auto& c : path) {
                if (c == '/') c = '\\';
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            static std::unordered_map<std::string, bool> s_missing;
            if (const auto it = s_missing.find(path); it != s_missing.end()) {
                return it->second;
            }
            // records store the path relative to meshes\, but not always
            const std::string full =
                path.starts_with("meshes\\") ? path : ("meshes\\" + path);
            const bool missing = !RE::BSResourceNiBinaryStream(full.c_str()).good();
            s_missing.emplace(path, missing);
            return missing;
        }

        bool MeshMissing(RE::TESBoundObject* a_obj)
        {
            std::vector<const char*> paths;
            ModelPathsOf(a_obj, paths);
            if (paths.empty()) return true;   // nothing named at all
            // ONE survivor is enough -- the engine only needs one nif to draw.
            for (const char* p : paths) {
                if (!PathMissing(p)) return false;
            }
            return true;
        }
    }

    void IconCache::QueueCapture(RE::TESBoundObject* a_obj)
    {
        if (!Capturable(a_obj)) return;
        const auto key = KeyFor(a_obj, ResolveDef(a_obj));

        // live edit: a drag produces a new key every frame — drop the stale
        // backlog so only the LATEST value gets captured (no lag-behind)
        if (a_obj == m_pin) {
            for (auto it = m_queue.begin(); it != m_queue.end();) {
                if (it->obj == a_obj && it->key != key) {
                    m_queued.erase(it->key);
                    it = m_queue.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (m_icons.contains(key) || m_queued.contains(key)) return;
        EnsureFailLoaded();
        if (m_failed.contains(key)) return;   // gave up on this one — stay out
        if (m_pendingBusy && m_pending.key == key) return;

        // Session-persistent icons: load from disk before spending any
        // engine renders (the slow path runs once per item, ever).
        if (LoadFromDisk(key)) return;
        // pre-migration pak entries (FormID keys) satisfy the item too
        if (const auto legacy = LegacyKeyFor(a_obj, ResolveDef(a_obj));
            legacy != key && (m_icons.contains(legacy) || LoadFromDisk(legacy))) {
            return;
        }

        // ★GI51: FRONT of the queue. QueueCapture means "something on screen
        // right now has no sprite" — Prefetch means "we may need this later".
        // Draining them in insertion order filled the cache with items the
        // player could not see while the visible grid stayed blank.
        m_queue.push_front({ a_obj, a_obj->GetFormID(), key });
        m_queued.insert(key);
    }

    void IconCache::Prefetch(RE::TESBoundObject* a_obj, bool a_evictAfter)
    {
        if (!Capturable(a_obj)) return;
        // B5: the on-disk checks below need the pak index — without this,
        // a precache that runs before the first LoadFromDisk saw an empty
        // index and re-queued every icon already on disk
        ScanPak();
        const IconDef def = ResolveDef(a_obj);
        const auto key = KeyFor(a_obj, def);
        if (m_icons.contains(key) || m_queued.contains(key)) return;
        EnsureFailLoaded();
        if (m_failed.contains(key)) return;
        if (m_pendingBusy && m_pending.key == key) return;
        // already on disk: NO texture upload here — a grid that actually
        // draws the item loads it lazily via the normal QueueCapture path
        // (legacy FormID-keyed entries from the pre-model-key pak count too)
        if (g_pakIndex.contains(key)) return;
        if (const auto legacy = LegacyKeyFor(a_obj, def);
            legacy != key && g_pakIndex.contains(legacy)) {
            return;
        }
        m_queue.push_back({ a_obj, a_obj->GetFormID(), key, a_evictAfter });
        m_queued.insert(key);
    }

    size_t IconCache::PrecacheAll()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return 0;
        const size_t before = m_queue.size();
        auto sweep = [&](const auto& a_arr) {
            for (auto* form : a_arr) {
                auto* obj = form ? form->template As<RE::TESBoundObject>() : nullptr;
                if (!obj) continue;
                const char* nm = obj->GetName();
                if (!nm || !nm[0]) continue;   // nameless = dummy/system form
                // (the empty-world-model guard that used to be duplicated here
                // now lives in Capturable, which every queueing path shares)
                if (IsDisplayOnlyModel(obj)) continue;
                if (!obj->GetPlayable()) continue;   // hidden scripting forms
                Prefetch(obj, true);   // pak-only: VRAM stays flat
            }
        };
        sweep(dh->GetFormArray<RE::TESObjectWEAP>());
        sweep(dh->GetFormArray<RE::TESObjectARMO>());
        sweep(dh->GetFormArray<RE::TESAmmo>());
        sweep(dh->GetFormArray<RE::AlchemyItem>());
        sweep(dh->GetFormArray<RE::IngredientItem>());
        sweep(dh->GetFormArray<RE::TESObjectBOOK>());
        sweep(dh->GetFormArray<RE::TESObjectMISC>());
        sweep(dh->GetFormArray<RE::TESSoulGem>());
        sweep(dh->GetFormArray<RE::TESKey>());
        sweep(dh->GetFormArray<RE::ScrollItem>());
        // ★★SPELLS TOO, and by their own rules rather than the sweep's.
        //
        // The generic filter asks GetPlayable() and looks for a world model,
        // neither of which means anything for a spell: what gets photographed
        // is its MDOB (CaptureSourceOf), and what makes it worth photographing
        // is that a player can pick it. Abilities, diseases and enchantments
        // are carried BY things rather than chosen, so they never reach a
        // wheel and would be pure capture cost.
        //
        // ★Every spell, not the ones this character happens to know. The pak
        // is built once and shipped to people whose spell lists we cannot see,
        // and an icon missing from it is a capture THEY wait for. Capturable()
        // refuses the ~130 with no display object, so the ones without cost
        // nothing but this loop.
        for (auto* sp : dh->GetFormArray<RE::SpellItem>()) {
            if (!sp) continue;
            const auto t = sp->GetSpellType();
            if (t != RE::MagicSystem::SpellType::kSpell &&
                t != RE::MagicSystem::SpellType::kPower &&
                t != RE::MagicSystem::SpellType::kLesserPower) {
                continue;
            }
            const char* nm = sp->GetName();
            if (!nm || !nm[0]) continue;
            if (auto* obj = sp->As<RE::TESBoundObject>()) Prefetch(obj, true);
        }
        const size_t queued = m_queue.size() - before;
        SKSE::log::info("[ICONS] precache: {} queued ({} already on disk)",
            queued, g_pakIndex.size());
        return queued;
    }

    void IconCache::CancelPrecache()
    {
        // visible items re-queue themselves next frame via the grid draw
        m_queue.clear();
        m_queued.clear();
    }

    bool IconCache::InspectShotStale() const
    {
        if (!m_inspectValid) return true;   // nothing on screen yet
        return m_inspectShotRx != m_inspectDef.rx ||
               m_inspectShotRy != m_inspectDef.ry ||
               m_inspectShotRz != m_inspectDef.rz ||
               m_inspectShotH != ImGui::GetIO().DisplaySize.y;
    }

    void IconCache::PreRender()
    {
        auto* pv = ItemPreview::GetSingleton();
        if (!pv->IsRunning()) return;

        // INSPECT owns the preview while the overlay is open: its result goes
        // to m_inspectIcon, so the tile queue is simply paused — no keys, no
        // disk, nothing of the icon cache is touched.
        //
        // ★It re-arms only when the rotation has actually moved. Holding still
        // now costs one early return per frame; it used to cost a full engine
        // re-render of a 900px model plus a mipped-texture upload, every frame,
        // to redraw a picture that had not changed.
        // ★★★THE GATE HAS TO BE ASKED FIRST. The long note below explains why
        // it exists; this is why it sat in the wrong place. It tested
        // !m_pendingBusy -- and arming an inspect is exactly what SETS that
        // flag, a few lines above. So on a machine at the wall the tile queue
        // stopped, correctly, while C walked straight past: the most expensive
        // request this plugin makes, a 900px model at 3x scale, going into the
        // same engine NIF loader the reported crash died inside.
        const bool memOk = MemoryHeadroom();

        if (m_inspect && !m_pendingBusy) {
            if (!memOk) {
                if (!m_memPaused) {
                    m_memPaused = true;
                    SKSE::log::warn("[ICONS] paused: system memory is low -- the inspect "
                                    "capture waits until it frees up");
                }
                return;   // nothing dropped; the next frame simply asks again
            }
            if (InspectShotStale()) {
                m_pending = Pending{ m_inspect, m_inspect ? m_inspect->GetFormID() : 0u, 0 };
                m_pendingInspect = true;
                m_pendingBusy = true;
                m_frames = 0;
            } else {
                // ★Nothing to re-shoot: leave with the preview untouched. This
                // MUST return rather than fall through -- everything below
                // belongs to the tile queue and the idle cleanup, and both
                // borrow the very scene the open inspect is standing on
                // (ResetScene would drop its model outright).
                return;
            }
        }
        // GI68: the retry pass owns the generous window only while ITS entries
        // are in flight. Once the queue drains, ordinary captures must go back
        // to the short window or one straggler would hold up a whole menu.
        if (m_retryPass && m_queue.empty() && !m_pendingBusy) {
            m_retryPass = false;
            RewriteSlow();   // whatever survived stays on the list
            SKSE::log::info("[ICONS] retry pass done: {} still deferred",
                m_deferred.size());
        }
        // ★★★DO NOT BE THE LAST DROP.
        //
        // A capture is the most expensive moment this plugin has: two 2048²
        // render targets, and behind them the engine's NIF loader allocating
        // a mesh. On a machine already at the wall, ours is the request that
        // fails -- and it fails INSIDE the engine, in a loader we do not call
        // directly and cannot guard. A reported crash landed exactly there:
        // NiBinaryStream reading note03.nif with physical memory at 22.78 of
        // 23.91 GB and 6.6M page faults. Nothing downstream could have caught
        // it; the only place we can act is before asking.
        //
        // ★The QUEUE IS UNTOUCHED. Nothing is dropped and no timeout runs --
        // an entry that never armed cannot age out -- so when memory frees up
        // the next frame simply carries on. This costs the player a wait, not
        // an icon.
        if (!memOk && !m_memPaused && !m_queue.empty()) {
            m_memPaused = true;
            SKSE::log::warn("[ICONS] paused: system memory is low -- {} icon(s) "
                            "stay queued and resume when it frees up",
                m_queue.size());
        } else if (memOk && m_memPaused) {
            m_memPaused = false;
            SKSE::log::info("[ICONS] resumed: memory recovered");
        }

        if (!m_pendingBusy && memOk) {
            // A skipped entry does NOT end the frame's work -- the loop keeps
            // going until it arms something -- so a run of skips is paid all at
            // once. First-time archive probes are the expensive kind, and a
            // load order missing a whole mesh pack can have hundreds in a row,
            // which would land as one long hitch. Bound it: what does not get
            // decided this frame gets decided on the next.
            int budget = 64;
            while (!m_queue.empty() && budget-- > 0) {
                const Pending p = m_queue.front();
                m_queue.pop_front();
                m_queued.erase(p.key);
                // The form may have been destroyed while this entry waited (a
                // brewed potion whose save was reloaded). Re-resolve by id and
                // drop the entry rather than touching freed memory.
                if (p.id != 0 &&
                    RE::TESForm::LookupByID<RE::TESBoundObject>(p.id) != p.obj) {
                    continue;
                }
                // ★GI68c: probe the archive BEFORE spending the slot. A nif
                // that is not there cannot load, so arming this entry would
                // buy a guaranteed 45 frames of nothing. NOT persisted to the
                // fail list on purpose: the probe is instant, so re-deciding
                // every session costs nothing and the item heals itself the
                // moment the missing mesh is installed.
                if (MeshMissing(p.obj)) {
                    SKSE::log::warn("[ICONS] '{}' skipped: mesh not found ('{}')",
                        p.obj->GetName(), ModelPathOf(p.obj));
                    continue;
                }
                if (!m_icons.contains(p.key)) {
                    m_pending = p;
                    m_pendingInspect = false;
                    m_pendingBusy = true;
                    m_frames = 0;
                    m_captureShrink = 1.0f;   // fresh ladder per item
                    break;
                }
            }
        }
        if (!m_pendingBusy) {
            // capture idle: hand the engine its own item scale back. Normal
            // captures push kIconCaptureScale (see the Request site below) and
            // UpdateParking's restore path only runs once the scale is zeroed
            // — leaving it set would hand a 2.5x model to whatever uses
            // Inventory3DManager next. The open inspect keeps its own scale.
            if (!m_inspect) {
                pv->SetInspectScale(0.0f);
            }
            // translucent skins: lingering models sit at the park point in
            // PLAIN SIGHT (no opaque window covers them). Once the queue
            // drains, purge stragglers that resisted Unload (pre-landing
            // Unload is a no-op). Throttled: ResetScene is End3D+Begin3D.
            if (Theme::S().translucent && m_queue.empty()) {
                static int s_idleFrames = 0;
                if (pv->SceneModelCount() > 0) {
                    // a deferred reset (load in flight) keeps the counter hot
                    // so it retries every frame until it lands
                    if (++s_idleFrames > 30 && pv->ResetScene()) {
                        s_idleFrames = 0;
                    }
                } else {
                    s_idleFrames = 0;
                }
            }
            return;
        }

        m_stampBefore = pv->GetCaptureStamp();
        IconDef def = ResolveDef(m_pending.obj);
        // the drag rotation is injected HERE and nowhere else — ResolveDef
        // itself stays clean, so tile keys never move during an inspect
        if (m_pendingInspect) {
            def.rx = m_inspectDef.rx;
            def.ry = m_inspectDef.ry;
            def.rz = m_inspectDef.rz;
            // record what THIS request is taken at -- the gate compares the
            // landed shot against the live rotation, so a drag that moves while
            // the capture is in flight still re-arms afterwards
            m_inspectReqRx = def.rx;
            m_inspectReqRy = def.ry;
            m_inspectReqRz = def.rz;
            m_inspectReqH = ImGui::GetIO().DisplaySize.y;
        }
        // Always capture at the STANDARD crop — def.scale is applied when the
        // tile draws (linear, instant, no capture-boundary nonlinearities).
        //
        // ★Resolution comes from the MODEL SCALE (see kIconCaptureScale): the
        // engine renders the item at its own ~275px regardless of the box, so
        // every capture now pushes the inspect-zoom scale lever and the box
        // just grows to cover the enlarged model. The box is still clamped to
        // what the screen can physically render (margin included) — pixels
        // the backbuffer cannot hold do not exist to capture.
        pv->SetInspectScale(m_pendingInspect ? kInspectModelScale
                                             : kIconCaptureScale * m_captureShrink);
        // ★★The capture lamp belongs to the ITEM, and it has to be set from the
        // SAME def this request carries — set it anywhere else and a slow
        // precache would light one item with the previous item's angle. Zero
        // offsets are the default rig, so items that never needed tuning cost
        // nothing here.
        float laz = 0.0f, lel = 0.0f;
        CaptureLightFor(def, laz, lel);   // same sum RotHash keyed on
        pv->SetLightOffset(laz, lel);
        float boxPx = m_pendingInspect ? kInspectRequestSize
                                       : kIconRequestSize * kIconCaptureScale;
        const float screenCap =
            ImGui::GetIO().DisplaySize.y / ItemPreview::kSafetyMargin - 8.0f;
        if (screenCap > 64.0f) boxPx = (std::min)(boxPx, screenCap);
        // ★The LAST of the four sites that must agree (see CaptureSourceOf):
        // the key, the two renderability probes, and the render itself. A key
        // taken from the spell with a picture taken from the flame would file
        // the capture under a name nothing ever looks up, and the icon would
        // be re-photographed every single time it was asked for.
        pv->Request(CaptureSourceOf(m_pending.obj), ImVec2(0.0f, 0.0f),
            ImVec2(boxPx, boxPx), -1.0f, 0.0f, 0.0f, &def);
        if (m_pending.boost > 0.0f) {
            pv->BoostCapture(m_pending.boost);   // B4: resume the clip-boost ladder
        }
    }

    void IconCache::GiveUpPending(const char* a_why)
    {
        SKSE::log::warn("[ICONS] '{}' skipped ({})", m_pending.obj->GetName(), a_why);

        // ★★SAY IT ONCE WHEN THE PATTERN IS REAL. A capture borrows the render
        // surface: paint a rect, have the engine draw the model into it, read
        // those pixels back. If the surface being read is not the one being
        // drawn into, every icon times out and NOTHING anywhere says why —
        // which is exactly how a swap-chain mismatch cost an entire
        // investigation (see the bound-target note in ItemPreview::Render;
        // that specific case is fixed).
        //
        // A long run of timeouts is close enough to proof of a systemic fault:
        // a genuinely missing mesh never gets a slot, and a slow item recovers
        // within a few. So leave a marker for whatever the NEXT such fault
        // turns out to be, without naming a cause we no longer believe.
        if (!m_pendingInspect && std::strstr(a_why, "timeout") != nullptr) {
            if (++m_timeoutStreak == 8 && !m_emptyCaptureWarned) {
                m_emptyCaptureWarned = true;
                SKSE::log::warn(
                    "[ICONS] ================================================");
                SKSE::log::warn(
                    "[ICONS] 8 icons in a row captured an EMPTY frame.");
                SKSE::log::warn(
                    "[ICONS] The model loads and is drawn, but the pixels read");
                SKSE::log::warn(
                    "[ICONS] back are blank -- so the surface being read is");
                SKSE::log::warn(
                    "[ICONS] probably not the one being drawn into.");
                SKSE::log::warn(
                    "[ICONS] This is a rendering-pipeline conflict, not a mod");
                SKSE::log::warn(
                    "[ICONS] list problem. Please report it WITH THIS LOG and");
                SKSE::log::warn(
                    "[ICONS] your post-process setup (ENB / Community Shaders,");
                SKSE::log::warn(
                    "[ICONS] upscaling, frame generation).");
                SKSE::log::warn(
                    "[ICONS] ================================================");
            }
        }
        ItemPreview::GetSingleton()->UnloadCurrent();
        m_pendingBusy = false;
        // an inspect frame carries no cache key (0): its failures must never
        // reach the PERSISTED fail list
        if (m_pendingInspect) return;
        // GI68: one verdict, no attempt counting. Reaching here means either the
        // engine never even started a load (more time cannot help) or the retry
        // pass already gave it ten seconds. Either way it is done.
        if (m_failed.insert(m_pending.key).second) {
            PersistFail(m_pending.key);
        }
    }

    // Phase 3: PostRender stage 1 — every wait/abandon decision before the
    // pixel pipeline runs (body moved verbatim from the old front half).
    IconCache::GateResult IconCache::CheckPendingGates()
    {
        // How long one capture may hold the slot before it is judged. Every
        // capture is one per frame on a single engine scene, so a window is
        // paid by EVERY item still in the queue, not just this one.
        //
        // ★★These were 20 / 45, picked as "3x the normal latency" — and that
        // multiplier was measured on ONE machine. The latency is not a property
        // of the item or of the load order; it is how long the engine's async
        // loader takes to come back, and that varies per setup by more than 3x.
        // Measured across three:
        //     93 plugins    2-3 frames   (78 icons in 3s, no timeouts)
        //   3826 plugins    2-4 frames   (1342 icons, 1 timeout in 1343)
        //      a user's PC   22 frames   (every single item, 100% failure)
        // The last one is not a slow disk — RTX 5070 / i5-14600K, and the frame
        // count is identical for a ring and a full cuirass, so nothing about
        // the file explains it. It is a fixed round-trip that happens to sit
        // just past 20.
        // ★So the window is a SAFETY NET, not a schedule. A fast setup never
        // reaches it (that is what 1342-of-1343 shows), which means widening it
        // costs those setups exactly nothing; it only buys the ones sitting on
        // the wrong side of the line. Sixty frames is ~1s at 60fps and roughly
        // 3x the worst latency actually observed rather than 3x the best.
        // ★★60/90 was tried and REVERTED. The theory was that the loader simply
        // needed longer, and the numbers refuted it outright: the wait tracked
        // the deadline instead of the load.
        //     deadline 20  ->  21 frames used
        //     deadline 20  ->  22 frames (one grace frame added)
        //     deadline 60  ->  62 frames
        // A file read cannot know what the deadline is. Whatever blocks the
        // gates blocks them until the deadline arrives, so widening it buys
        // nothing and makes every genuine failure three times as slow.
        constexpr int kSoftFrames     = 20;    // ~0.33s
        constexpr int kPrecacheFrames = 45;    // ~0.75s: a mass pass, still cheap
        constexpr int kRetryFrames    = 600;   // ~10s, but only in the retry pass
        auto* pv = ItemPreview::GetSingleton();

        // ★★An enchant GLOW never survives a cache capture: the first shot of a
        // freshly loaded model renders without the effect pass, and a second one
        // only reaches ~11% of the tint a long-lived model shows (measured by
        // channel share; EDIT looked right only because a rotation drag re-shoots
        // the same model for seconds). Buying it would cost seconds PER ITEM, so
        // it is not bought — the rarity halo already marks enchanted items and
        // costs no capture. See Inv3D::Load for the loader side of the same call.

        // INSPECT: a live view, not a cache fill — no requeue, no attempt
        // counting, and never the PERSISTED fail list (its "key" is 0, which
        // is not a cache key at all). A stalled frame just leaves the previous
        // capture on screen; releasing the slot lets PreRender re-arm.
        if (m_pendingInspect) {
            if (m_frames > kTimeoutFrames) {
                m_pendingBusy = false;
                return GateResult::kAbandoned;
            }
            if (pv->GetCaptureStamp() == m_stampBefore) return GateResult::kNotReady;
            auto* mdl = pv->FindCurrentModel();
            if (!mdl || mdl->worldBound.radius <= 0.0f) return GateResult::kNotReady;
            if (!pv->RotationApplied()) return GateResult::kNotReady;
            return GateResult::kReady;
        }

        // ★★A capture that lands on the very frame the window closes has done
        // everything asked of it, and the deadline used to be judged without
        // ever looking — the work was thrown away and the verdict PERSISTED.
        // So the expiry branches below take ONE last look before giving up
        // (see kDeadlineGrace).
        //
        // ★★★It has to be ONE. Putting this check ABOVE the expiry branches
        // instead — "ready beats expired" — hung the whole queue: a pending
        // whose gates read open but whose harvest does not complete answers
        // kReady every frame, never reaches a verdict, and the cache stops
        // dead on that item. A user's log showed 75 seconds on a single
        // nameless item with no 'cached' and no 'skipped' line. The deadline
        // is the one thing that must ALWAYS be reachable; a grace frame is a
        // last chance, not a way around it.
        const auto expired = [this](int a_limit) {
            return m_frames > a_limit;
        };
        // exactly one frame of grace, so the sequence is bounded no matter
        // what the gates say
        const auto graceFrame = [this](int a_limit) {
            return m_frames == a_limit + 1 && CaptureAccepted();
        };

        // Precache entries WAIT IN PLACE instead of requeueing: a requeue is
        // Unload+reLoad, which throws away the in-flight async model load and
        // restarts it — the main "stall cluster" during a mass precache. One
        // generous window, then a verdict.
        //
        // ★GI68b: the verdict is the SAME two-way split the normal path uses.
        // This branch used to send every straggler straight to the permanent
        // fail list, so a full re-cache — the one operation that touches every
        // item and is therefore most likely to meet a slow one — could never
        // produce a deferred entry. A measured run blacklisted 'Coinpurse'
        // while the log line one millisecond earlier read
        //     [PREVIEW] state: models=3 cur='Coinpurse'
        // i.e. the scene HAD the entry, its spModel was simply still null.
        // FindCurrentModel() returning null does not mean "no model exists";
        // only LoadPending() can tell a slow load from an absent one.
        if (m_pending.evict &&
            expired(m_retryPass ? kRetryFrames : kPrecacheFrames)) {
            if (graceFrame(m_retryPass ? kRetryFrames : kPrecacheFrames)) {
                return GateResult::kReady;
            }
            const bool loading = pv->LoadPending();
            // DIAGNOSTIC: which gate starved? (stamp = captures ran at all,
            // model/rot = scene state, content probe logs separately below)
            auto* dmdl = pv->FindCurrentModel();
            SKSE::log::warn(
                "[ICONS] precache gates '{}': {} (model={} radius={:.1f} rot={} "
                "park={} stamp={}->{} mesh='{}')",
                m_pending.obj->GetName(), loading ? "deferred" : "no model",
                dmdl != nullptr, dmdl ? dmdl->worldBound.radius : -1.0f,
                pv->RotationApplied(), pv->ParkTicks(),
                m_stampBefore, pv->GetCaptureStamp(), ModelPathOf(m_pending.obj));
            if (loading && !m_retryPass) {
                if (m_deferred.insert(m_pending.key).second) {
                    m_deferredObj[m_pending.key] = m_pending.obj;
                    PersistSlow(m_pending.key);
                }
                GiveUpPending("precache deferred");
                return GateResult::kAbandoned;
            }
            m_deferred.erase(m_pending.key);
            m_deferredObj.erase(m_pending.key);
            if (m_failed.insert(m_pending.key).second) {   // no second chance,
                PersistFail(m_pending.key);                // relogs included
            }
            GiveUpPending("precache timeout");
            return GateResult::kAbandoned;
        }

        // Soft skip: a straggler must NOT stall the whole queue (3 stalled
        // items once accounted for 6s of a 6.5s first render). Requeue it at
        // the back quickly and keep moving; it retries after the scene state
        // has moved on.
        //
        // ★GI68: the LAST attempt waits in place instead. A requeue is
        // Unload + reLoad, which throws away the in-flight async model load and
        // starts over -- the precache path already knew this (see the comment
        // above) but the normal path did not, so an item that simply loads
        // slower than 0.33s could never finish: every attempt restarted it and
        // the fourth one gave up for good. Measured on a user's log, 65 items
        // burned 20s each and every one of them landed in the PERSISTED fail
        // list, which is why "mod icons never cache" was the report.
        //
        // Giving the final attempt a full second costs nothing in the common
        // case (nothing reaches attempt 4 unless it is genuinely slow) and it
        // breaks the loop below as well: fewer requeues means fewer ResetScene
        // calls, and a reset is what makes the NEXT load land empty.
        if (!m_pending.evict &&
            expired(m_retryPass ? kRetryFrames : kSoftFrames)) {
            if (graceFrame(m_retryPass ? kRetryFrames : kSoftFrames)) {
                return GateResult::kReady;
            }
            // ★GI68: NO requeue. It used to try four times, and each try began
            // with Unload+reLoad -- throwing away the in-flight async load and
            // starting over. An item that simply loads slower than 0.33s could
            // therefore never finish, no matter how many attempts it got: a
            // user's log showed 65 items spending 20s each and every one of
            // them landing in the PERSISTED fail list.
            //
            // One window, then a verdict, and the verdict comes from asking the
            // ENGINE rather than counting frames:
            //   loading  -> DEFERRED. It works, it is just slower than this pass
            //               can afford. Waiting here would stall every other
            //               item, so it gets its time in the retry pass.
            //   not even -> FAILED. No task, no entry: more time changes
            //               nothing (HDT/physics meshes, phantom weapons).
            const bool loading = pv->LoadPending();
            auto* dmdl = pv->FindCurrentModel();
            // ★park/stamp are the two gates that used to be invisible here.
            // stamp A->B equal means NO capture ran at all (the pixel path
            // returned early); park < 2 means the rig had not settled. Without
            // them a timeout line said only "not ready" and every cause looked
            // identical.
            SKSE::log::info(
                "[ICONS] '{}' {} (model={} radius={:.1f} rot={} park={} "
                "stamp={}->{} loading={})",
                m_pending.obj->GetName(), loading ? "deferred" : "no model",
                dmdl != nullptr, dmdl ? dmdl->worldBound.radius : -1.0f,
                pv->RotationApplied(), pv->ParkTicks(),
                m_stampBefore, pv->GetCaptureStamp(), loading);

            if (loading && !m_retryPass) {
                if (m_deferred.insert(m_pending.key).second) {
                    m_deferredObj[m_pending.key] = m_pending.obj;
                    PersistSlow(m_pending.key);
                }
                pv->UnloadCurrent();
                m_pendingBusy = false;
                return GateResult::kAbandoned;
            }
            // retry pass ran out too, or the load never took at all
            m_deferred.erase(m_pending.key);
            m_deferredObj.erase(m_pending.key);
            GiveUpPending("timeout");
            return GateResult::kAbandoned;
        }

        return CaptureAccepted() ? GateResult::kReady : GateResult::kNotReady;
    }

    // Are all four acceptance gates open RIGHT NOW?
    //
    // ★★Split out of CheckPendingGates so it can be asked BEFORE the timeout
    // verdicts rather than after them. It used to sit at the bottom, and that
    // ordering threw away work that was finished: on the frame the window
    // expires the code judged first and never looked, so an item whose capture
    // landed on frame 20 of 20 went to the fail list with its pixels ready.
    // A user's log showed exactly that -- every line read
    //     model=true radius=12.8 rot=true park=2 stamp=314->315
    // i.e. all four gates open, "skipped (timeout)" on the next line. On a fast
    // machine the capture lands around frame 2 and nothing ever notices; the
    // slower the disk, the closer it creeps to the deadline, which is why this
    // reproduced for one user and never here.
    bool IconCache::CaptureAccepted() const
    {
        return CaptureRejectReason() == nullptr;
    }

    // ★★DIAGNOSTIC SPLIT: which gate said no, as a word.
    //
    // Three fixes were aimed at this by inference and all three missed, because
    // the only log we had was written AFTER the verdict and showed every gate
    // open. The deciding measurement is the one frame BEFORE — and the two
    // differ. Returning the reason (instead of a bool) lets the per-frame trace
    // print exactly which line rejected, on every frame, so the frame the state
    // flips is visible rather than guessed.
    const char* IconCache::CaptureRejectReason() const
    {
        auto* pv = ItemPreview::GetSingleton();

        // A capture must have completed since this item was armed.
        if (pv->GetCaptureStamp() == m_stampBefore) return "stamp";

        // Accept only when OUR item's model is render-ready (the recentre in
        // ItemPreview::Render also keys off the matching entry, so the crop is
        // centred correctly even while stale async loads are still landing).
        auto* pvModel = pv->FindCurrentModel();
        if (!pvModel) return "model:null";
        if (pvModel->worldBound.radius <= 0.0f) return "model:radius";

        // Landing-frame race: the engine stomps the node with its default
        // pose after our rotation ran, and that first capture would bake the
        // wrong orientation into the cache PERMANENTLY (square diagonal-sword
        // icons). Accept only once the node carries the requested rotation —
        // UpdateParking re-applies it next frame.
        if (!pv->RotationApplied()) return "rot";

        // ★★RotationApplied is not enough on its own. It asks whether the node
        // carries the requested rotation, and a leftover model from a DIFFERENT
        // item answers yes whenever the two share one — which category defaults
        // make routine (armor_head and armor_body_heavy are both rx:90). A body
        // was being accepted a frame before its own park ran, so the pixels
        // came from the previous item's model under the previous item's light.
        // Measured: ACCEPT on f8977, that item's park on f8978.
        // Weapons and bags never showed it only because their angles differ.
        if (!pv->ParkSettled()) return "park";

        return nullptr;   // accepted
    }

    void IconCache::PostRender()
    {
        if (!m_pendingBusy) return;
        ++m_frames;

        auto* pv = ItemPreview::GetSingleton();

        // stage 1: timeouts / soft-skip / capture readiness gates
        if (CheckPendingGates() != GateResult::kReady) return;

        // stage 2+: pixel pipeline (readback -> trim/sprite -> persist). Kept
        // in one body: its locals (crop rect, mapped rows, trim bounds) flow
        // straight through — splitting them would only add plumbing structs.
        auto giveUp = [&](const char* a_why) { GiveUpPending(a_why); };

        // Pixel rect of the FULL margin region (kSafetyMargin x inner box):
        // rotation diagonals that outgrow the inner box stay uncut; tiles
        // draw the icon kSafetyMargin larger to compensate.
        ImVec2 uv0, uv1;
        pv->GetMarginUV(uv0, uv1);
        const float kTex = static_cast<float>(ItemPreview::kTexSize);
        int x0 = static_cast<int>(uv0.x * kTex);
        int y0 = static_cast<int>(uv0.y * kTex);
        int x1 = static_cast<int>(uv1.x * kTex);
        int y1 = static_cast<int>(uv1.y * kTex);

        const auto cap = pv->GetCapturedSize();
        x0 = (std::max)(0, x0);
        y0 = (std::max)(0, y0);
        x1 = (std::min)(x1, static_cast<int>(cap.x));
        y1 = (std::min)(y1, static_cast<int>(cap.y));
        const int w = x1 - x0;
        const int h = y1 - y0;
        if (w <= 0 || h <= 0) { giveUp("empty crop"); return; }

        auto* srcTex = pv->GetTexture();
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!srcTex || !data) { giveUp("no texture"); return; }
        auto* device  = reinterpret_cast<ID3D11Device*>(data->forwarder);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
        if (!device || !context) { giveUp("no device"); return; }

        D3D11_TEXTURE2D_DESC srcDesc = {};
        srcTex->GetDesc(&srcDesc);

        // ★★★FOUR BYTES A PIXEL IS AN ASSUMPTION, SO STATE IT.
        //
        // The capture texture is created to match whatever surface was bound,
        // and the whole pipeline below -- the readback memcpy of w*4 per row,
        // the trim, the chroma key, CreateMippedTexture's SysMemPitch = w*4 --
        // then treats it as 8-bit RGBA. The 10-bit case is handled (unpacked
        // just below). Nothing handled the rest, and HDR swap chains are real:
        // R16G16B16A16_FLOAT is EIGHT bytes a pixel, so D3D reads w*8 per row
        // out of a buffer holding w*h*4 and walks off the end of it, while
        // R11G11B10_FLOAT is the right size and the wrong meaning -- float bits
        // stored as colour. Both then got written into the pack under that
        // format tag, so removing the HDR mod afterwards changed nothing.
        //
        // Refuse instead. The item falls back to its glyph, and the log names
        // the format so an unknown one can be added deliberately rather than
        // guessed at.
        switch (srcDesc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            break;
        default: {
            static bool s_saidFmt = false;
            if (!s_saidFmt) {
                s_saidFmt = true;
                SKSE::log::error("[ICONS] capture surface format {} is not 8-bit RGBA -- "
                                 "icons stay on their fallback glyphs. Report this number.",
                    static_cast<int>(srcDesc.Format));
            }
            giveUp("unsupported surface format");
            return;
        }
        }

        // Content gate + alpha-trim: read the whole margin region back, find
        // the model's true pixel bounds, and store ONLY that rect. A sprite
        // built this way can never bake in clipping, no matter how far
        // rotation/scale pushed the projection. (Empty read = the engine has
        // not drawn yet — retry next frame; timeout still bounds us.)
        std::vector<std::uint8_t> pixels;
        int trimX = 0, trimY = 0, trimW = 0, trimH = 0;

        // ★★★The backbuffer is not always 8 bits per channel. Community Shaders'
        // Frame Generation installs a D3D11-to-D3D12 proxy and the swap chain
        // comes back as R10G10B10A2 -- measured side by side with the same save:
        //
        //   FG off: fmt=28 (R8G8B8A8)     magenta reads FF00FF   pure=100%
        //   FG on : fmt=24 (R10G10B10A2)  magenta reads FF03F0   pure=0%
        //
        // FF03F0 is not corruption; it is 0xFFF003FF (R=1023,G=0,B=1023,A=3)
        // read a byte at a time. Everything downstream assumes 8-bit RGBA, so
        // the pixels are unpacked once here and the rest of the pipeline -- trim,
        // chroma key, defringe, mips, disk cache -- keeps its one assumption.
        const bool is10Bit = srcDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                             srcDesc.Format == DXGI_FORMAT_R10G10B10A2_UINT;
        // What `pixels` actually holds afterwards. ★Not srcDesc.Format: storing
        // unpacked bytes under the 10-bit tag would bake the same defect into the
        // texture and into the on-disk cache.
        const DXGI_FORMAT pixFmt = is10Bit ? DXGI_FORMAT_R8G8B8A8_UNORM : srcDesc.Format;
        if (is10Bit) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                SKSE::log::info("[ICONS] backbuffer is 10-bit ({}) -- unpacking captures "
                                "to 8-bit RGBA (upscaler/frame-generation proxy)",
                    static_cast<int>(srcDesc.Format));
            }
        }
        {
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width            = static_cast<UINT>(w);
            sd.Height           = static_cast<UINT>(h);
            sd.MipLevels        = 1;
            sd.ArraySize        = 1;
            sd.Format           = srcDesc.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage            = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

            ID3D11Texture2D* staging = nullptr;
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) { giveUp("stage fail"); return; }

            D3D11_BOX cbox = {};
            cbox.left   = static_cast<UINT>(x0);
            cbox.top    = static_cast<UINT>(y0);
            cbox.front  = 0;
            cbox.right  = static_cast<UINT>(x1);
            cbox.bottom = static_cast<UINT>(y1);
            cbox.back   = 1;
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, srcTex, 0, &cbox);

            int nonBg = 0;
            int minX = w, minY = h, maxX = -1, maxY = -1;
            D3D11_MAPPED_SUBRESOURCE map = {};
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                pixels.resize(static_cast<size_t>(w) * h * 4);
                for (int y = 0; y < h; ++y) {
                    const auto* row = static_cast<const std::uint8_t*>(map.pData) +
                                      static_cast<size_t>(y) * map.RowPitch;
                    auto* dst = pixels.data() + static_cast<size_t>(y) * w * 4;
                    if (is10Bit) {
                        // ★★★Unpack, do not memcpy. A 10-bit surface is still 4
                        // bytes per pixel, so a straight copy "works" and every
                        // channel lands across a byte boundary: what comes out is
                        // neighbouring channels welded together, which is exactly
                        // the speckled icons reported under Frame Generation.
                        const auto* src32 = reinterpret_cast<const std::uint32_t*>(row);
                        for (int x = 0; x < w; ++x) {
                            const std::uint32_t v = src32[x];
                            dst[x * 4 + 0] = static_cast<std::uint8_t>(((v >>  0) & 0x3FF) >> 2);
                            dst[x * 4 + 1] = static_cast<std::uint8_t>(((v >> 10) & 0x3FF) >> 2);
                            dst[x * 4 + 2] = static_cast<std::uint8_t>(((v >> 20) & 0x3FF) >> 2);
                            // ★★★THE TWO ALPHA BITS ARE THE WHOLE PICTURE HERE.
                            // This wrote 255 unconditionally, with "2-bit alpha
                            // is not ours to use" beside it. Harmless while the
                            // backdrop was found by COLOUR — and the reported
                            // pink background the moment the key became alpha:
                            // no pixel reads as background, so the magenta sheet
                            // is kept as opaque model and baked into the icon.
                            // (1.4.0 was immune for exactly that reason, which is
                            // why downgrading fixed it for the reporter.)
                            // Two bits is coarse — 0/85/170/255 — but the two
                            // ends are the two that carry the meaning: 0 is the
                            // backdrop we cleared, 3 is solid geometry.
                            //
                            // ★KNOWN AND ACCEPTED: a genuinely translucent
                            // pixel whose real alpha is 192..255 rounds to 255
                            // here, and the sprite pass leaves a==255 alone by
                            // design (that rule is what keeps a purple potion
                            // purple). Such a pixel still carries up to a
                            // quarter of the magenta backdrop, so a lace veil
                            // shows a faint violet cast on this surface and
                            // none on an 8-bit one — measured both ways.
                            // Not fixable from here: two bits cannot tell 200
                            // from 255, and treating 255 as suspect would bleach
                            // every actually-purple object on this hardware.
                            // Visible only while rotating a veil in EDIT.
                            dst[x * 4 + 3] =
                                static_cast<std::uint8_t>(((v >> 30) & 0x3) * 85);
                        }
                    } else {
                        std::memcpy(dst, row, static_cast<size_t>(w) * 4);
                    }
                }
                context->Unmap(staging, 0);

                // ★★★DOES THIS SURFACE GIVE US ALPHA AT ALL?
                //
                // Everything downstream reads transparency out of the alpha
                // channel, which is only sound while the channel survives the
                // trip — and it does not always. A 10-bit backbuffer carries
                // two bits; an upscaler proxy can hand back a surface with
                // none. The failure is SILENT: every pixel reads opaque, the
                // trim keeps the whole rect, and the magenta backdrop bakes
                // into the icon as a pink sheet.
                //
                // We cleared this rect to alpha 0 OURSELVES and the model never
                // fills it (kSafetyMargin is 1.5x the box, and anything that
                // does reach the edge is sent back for a bigger box below). So
                // a capture with ZERO transparent pixels is not a full frame —
                // it is a channel that never arrived. Say so, and fall back to
                // what 1.4.0 did: key the magenta by colour and write the alpha
                // ourselves, after which nothing downstream need know.
                //
                // ★The gate is not "is this 10-bit" on purpose. We know of one
                // surface that eats alpha; the reporter's may be another. The
                // question that generalises is whether the channel came back,
                // and the answer costs nothing to ask — a healthy capture's
                // first pixel IS backdrop, so this breaks on iteration one.
                bool alphaOk = false;
                for (size_t i = 3; i < pixels.size(); i += 4) {
                    if (pixels[i] == 0) { alphaOk = true; break; }
                }
                if (!alphaOk && !pixels.empty()) {
                    for (size_t i = 0; i < pixels.size(); i += 4) {
                        // Symmetric in R/B, so BGRA vs RGBA never matters.
                        const bool key = pixels[i] > 200 && pixels[i + 2] > 200 &&
                                         pixels[i + 1] < 60;
                        pixels[i + 3] = key ? 0 : 255;
                    }
                    static bool s_saidKey = false;
                    if (!s_saidKey) {
                        s_saidKey = true;
                        SKSE::log::info("[ICONS] capture surface returned no alpha (fmt={}) "
                                        "-- keying the backdrop by colour instead",
                            static_cast<int>(srcDesc.Format));
                    }
                }

                for (int y = 0; y < h; ++y) {
                    const auto* dst = pixels.data() + static_cast<size_t>(y) * w * 4;
                    for (int x = 0; x < w; ++x) {
                        // ★★★CONTENT IS ALPHA, NOT COLOUR. The background is
                        // cleared to alpha 0 and the engine writes real alpha
                        // where it draws (measured: after-clear 0=100%, and
                        // after-model 0=94.1% mid=5.3% on a lace veil). A
                        // colour test could not see a half-transparent pixel
                        // at all -- it reads as magenta because it IS mostly
                        // magenta -- so a veil was trimmed away as background.
                        // ★Read from dst, not the raw row: on a 10-bit surface
                        // those are different numbers, and alpha only means
                        // this after the unpack.
                        const bool bg = dst[x * 4 + 3] == 0;
                        if (!bg) {
                            ++nonBg;
                            minX = (std::min)(minX, x);
                            maxX = (std::max)(maxX, x);
                            minY = (std::min)(minY, y);
                            maxY = (std::max)(maxY, y);
                        }
                    }
                }
            }
            staging->Release();

#ifdef GI_CAPTURE_DIAG
            // DIAGNOSTIC: is the capture rect empty magenta (engine drew
            // nothing) or does it hold content that some later gate rejects?
            if (m_pending.evict && m_frames == 30) {
                SKSE::log::info("[ICONS] content probe '{}': nonBg={} rect={}x{}",
                    m_pending.obj->GetName(), nonBg, w, h);
            }
#endif
            // ★★A SILENT retry, and it stays silent on purpose: the common case
            // is simply "the engine has not finished drawing", which resolves
            // on the next frame and would otherwise spam a line per frame per
            // icon. What it must never do again is hide a REAL fault — a run
            // of these ends in "timeout", and GiveUpPending names the likely
            // cause once a run is long enough to mean something.
            //
            // (This line is where "no icon ever caches" lived for a whole
            // investigation. The capture was landing on a surface nobody
            // displays — see the bound-target note in ItemPreview::Render.)
            if (nonBg < 40) return;   // engine hasn't drawn yet — retry next frame

            // Clipped capture: content touches the margin edge, i.e. the
            // engine rendered the model larger than the box (per-record
            // oversize like Moth Priest Robes — invisible on worldBound).
            // Grow the box and recapture instead of baking a cropped sprite.
            if (minX <= 0 || minY <= 0 || maxX >= w - 1 || maxY >= h - 1) {
                const ImVec2 cover = pv->CaptureCover();
                const float cur = (std::max)(cover.x, cover.y);
                // ★The ladder's ceiling is the BACKBUFFER, not kTexSize. The
                // capture copies screen pixels; a box larger than the screen
                // reads out of bounds, D3D drops the copy, and the bake then
                // harvests the PREVIOUS capture's stale pixels — the
                // "rotating in EDIT suddenly zooms/crops the icon" report
                // (2.5x models reach 2048 in two boost steps now). Past the
                // screen no bigger box exists: bake the slightly clipped
                // sprite, which is at least made of REAL pixels.
                const auto scr = RE::BSGraphics::Renderer::GetScreenSize();
                float cap = static_cast<float>(ItemPreview::kTexSize);
                if (scr.width > 0 && scr.height > 0) {
                    cap = (std::min)({ cap, static_cast<float>(scr.width),
                                       static_cast<float>(scr.height) });
                }
                if (cur < cap - 1.0f) {
                    const float grow = (std::min)(cap, cur * 1.6f);
                    pv->BoostCapture(grow);
                    SKSE::log::info("[ICONS] '{}' clipped at {}x{} — retry with {:.0f}px box",
                        m_pending.obj->GetName(), w, h, grow);
                    return;
                }
                // Box is at the screen ceiling and the model still overflows.
                // Rung two: render it smaller. A sprite at 70% of the pixels
                // is a small loss; a sprite with its silhouette sliced off is
                // wrong forever, and the pixel style outlines that cut into a
                // rectangle around the icon.
                if (!m_pendingInspect && m_captureShrink > kMinCaptureShrink) {
                    m_captureShrink = (std::max)(kMinCaptureShrink, m_captureShrink * 0.7f);
                    SKSE::log::info("[ICONS] '{}' still clipped at box ceiling {:.0f}px — "
                        "retry at {:.0f}% model scale",
                        m_pending.obj->GetName(), cap, m_captureShrink * 100.0f);
                    return;
                }
            }

            trimX = (std::max)(0, minX - 2);
            trimY = (std::max)(0, minY - 2);
            trimW = (std::min)(w - 1, maxX + 2) - trimX + 1;
            trimH = (std::min)(h - 1, maxY + 2) - trimY + 1;
        }

        // ★★★THE ALPHA THE ENGINE GAVE US, NOT A COLOUR GUESS.
        //
        // This used to be a chroma key: paint magenta behind the model, then
        // call every pure-magenta pixel transparent. It cannot work on an item
        // with an alpha texture. A half-transparent pixel comes back as model
        // BLENDED with magenta -- it is neither the model's colour nor pure
        // magenta -- so the key kept it, marked it opaque, and a bridal veil
        // arrived as a solid purple sheet (reported, [COCO] Deliciously Bride).
        // The old defringe could not save it either: that only touched pixels
        // ADJACENT to transparency, deliberately, so interior purples like
        // potions and enchant glows would survive. A veil is interior.
        //
        // Measured instead of argued. Clearing the background to alpha 0 and
        // reading the rect back:
        //
        //   after-clear   alpha 0=100.0%  mid=0.0%  255=0.0%
        //   after-model   alpha 0=94.1%   mid=5.3%  255=0.5%
        //
        // The engine writes REAL alpha. So transparency is simply read, and
        // the three cases are exact rather than inferred:
        //
        //   a == 0     background          -> clear (RGB zeroed so bilinear
        //                                     sampling cannot bleed magenta)
        //   a == 255   opaque model pixel  -> ITS OWN COLOUR, untouched. This
        //                                     is what keeps a purple potion
        //                                     purple.
        //   otherwise  blended with the background. Sampled pairs:
        //                 940B92/a38   8D0F8B/a40   FF65F5/a28
        //              every one is R≈B≫G, which is the magenta cast, and
        //              min(R,B)-G is exactly how much of it there is.
        //              940B92 -> spill 135 -> (13,11,11): grey lace, correct.
        //
        // Defringe is gone with the key. Its whole job was guessing at edges
        // what alpha now states outright, and running both would darken and
        // fade the same pixel twice.
        std::vector<std::uint8_t> sprite(static_cast<size_t>(trimW) * trimH * 4);
        for (int y = 0; y < trimH; ++y) {
            const auto* src = pixels.data() + (static_cast<size_t>(trimY + y) * w + trimX) * 4;
            auto* dst = sprite.data() + static_cast<size_t>(y) * trimW * 4;
            std::memcpy(dst, src, static_cast<size_t>(trimW) * 4);
            for (int x = 0; x < trimW; ++x) {
                auto* px = dst + x * 4;
                const int a = px[3];
                if (a == 0) {
                    px[0] = px[1] = px[2] = 0;
                } else if (a < 255) {
                    const int spill =
                        (std::min)(static_cast<int>(px[0]), static_cast<int>(px[2])) - px[1];
                    if (spill > 0) {
                        px[0] = static_cast<std::uint8_t>((std::max)(0, px[0] - spill));
                        px[2] = static_cast<std::uint8_t>((std::max)(0, px[2] - spill));
                    } else {
                        // ★★★LOW ALPHA WITH NO BACKDROP IN IT IS NOT
                        // TRANSPARENCY. Hides and pelts came out see-through
                        // (reported: goat and elk). Measured, they carry NO
                        // opaque pixel at all — 'Goat Hide' 255=0, every pixel
                        // between 64 and 191 — and yet the engine draws them
                        // solid, because they are alpha-TESTED: the shader
                        // writes the material's alpha and the test decides
                        // visibility, so the number in the buffer describes
                        // the material, not what you can see through.
                        //
                        // The two cases separate by COLOUR, not by alpha. A
                        // pixel that really was blended has the magenta
                        // backdrop mixed into it and shows up as spill (a lace
                        // veil measured 940B92 -> 135). One that covered the
                        // backdrop outright has none, and on natural colours
                        // min(R,B)-G lands at or below zero. So: backdrop in
                        // the pixel means it is genuinely see-through, and no
                        // backdrop means the low alpha is bookkeeping.
                        //
                        // ★This keeps the veil intact — that was the whole
                        // point of reading alpha — while giving the hides back
                        // the solidity the engine gives them.
                        px[3] = 255;
                    }
                }
            }
        }

        // ★★★CENTRE WHAT THE EYE SEES, for spells only.
        //
        // Everything that draws a sprite centres its BOUNDING BOX, which is
        // the right answer for a solid object: the box IS the object, and a
        // bottle should sit in its cell the way it sits on a table. It stops
        // being the right answer when the content is a LIGHT. A flame with a
        // faint plume up one side has a box that reaches the plume, and the
        // part anyone actually looks at then sits off to the other side of the
        // box's middle. Measured on the ring: 'Flames' put its luminous weight
        // at 0.60 of its own height where every other spell measured 0.50, and
        // it was the one icon that looked wrong (reported).
        //
        // ★Not the centroid, though the centroid is what found this. A mean
        // gets dragged by a wide dim halo, and that halo is exactly the part
        // nobody sees. What is centred here is the BOX OF THE VISIBLE CORE:
        // pixels carrying at least a quarter of the brightest one. That is a
        // "where does the shape look like it is" answer rather than a "where
        // is its mass" one, and for a two-part model -- a dim flame above a
        // bright orb -- it lands between them, which is where an eye puts it.
        //
        // ★Done by PADDING rather than by moving the crop: the crop is bounded
        // by the capture, and a core near an edge would need pixels that were
        // never rendered. Transparent rows cost nothing and the draw centres
        // the result for free -- no draw-side change, no per-form ini, and the
        // correction rides in the shipped pak.
        //
        // ★ITEMS ARE LEFT ALONE, deliberately. A wine bottle measures 0.48 /
        // 0.64 because it is heavy at the base, and "correcting" that would
        // float it in its cell. Its box is honest; a spell's is not.
        if (m_pending.obj->As<RE::SpellItem>() && trimW > 0 && trimH > 0) {
            const auto vOf = [](const std::uint8_t* a_px) {
                const double lum = (a_px[0] * 0.299 + a_px[1] * 0.587 +
                                    a_px[2] * 0.114) / 255.0;
                return (a_px[3] / 255.0) * lum;
            };
            double vmax = 0.0;
            for (int y = 0; y < trimH; ++y) {
                const auto* row = sprite.data() + static_cast<size_t>(y) * trimW * 4;
                for (int x = 0; x < trimW; ++x) vmax = (std::max)(vmax, vOf(row + x * 4));
            }
            if (vmax > 0.0) {
                const double thr = vmax * 0.25;
                int x0 = trimW, y0 = trimH, x1 = -1, y1 = -1;
                for (int y = 0; y < trimH; ++y) {
                    const auto* row = sprite.data() + static_cast<size_t>(y) * trimW * 4;
                    for (int x = 0; x < trimW; ++x) {
                        if (vOf(row + x * 4) < thr) continue;
                        x0 = (std::min)(x0, x); x1 = (std::max)(x1, x);
                        y0 = (std::min)(y0, y); y1 = (std::max)(y1, y);
                    }
                }
                if (x1 >= x0 && y1 >= y0) {
                    const int cx = (x0 + x1 + 1) / 2;
                    const int cy = (y0 + y1 + 1) / 2;
                    // Pad the side the content is NEAREST, so its core lands on
                    // the new middle. (Derivation: with padL added on the left,
                    // the core sits at cx+padL and the middle at (trimW+padL)/2;
                    // equal when padL = trimW - 2*cx.)
                    int padL = (std::max)(0, trimW - 2 * cx);
                    int padR = (std::max)(0, 2 * cx - trimW);
                    int padT = (std::max)(0, trimH - 2 * cy);
                    int padB = (std::max)(0, 2 * cy - trimH);
                    // ★A core hard against one edge would otherwise ask to
                    // double the sprite, spending memory and resolution on
                    // emptiness. Past this the picture is simply lopsided and
                    // half-correcting it is better than paying for the rest.
                    const int capW = trimW * 3 / 5, capH = trimH * 3 / 5;
                    padL = (std::min)(padL, capW); padR = (std::min)(padR, capW);
                    padT = (std::min)(padT, capH); padB = (std::min)(padB, capH);
                    if (padL || padR || padT || padB) {
                        const int nw = trimW + padL + padR;
                        const int nh = trimH + padT + padB;
                        std::vector<std::uint8_t> padded(
                            static_cast<size_t>(nw) * nh * 4, 0);
                        for (int y = 0; y < trimH; ++y) {
                            std::memcpy(
                                padded.data() +
                                    (static_cast<size_t>(y + padT) * nw + padL) * 4,
                                sprite.data() + static_cast<size_t>(y) * trimW * 4,
                                static_cast<size_t>(trimW) * 4);
                        }
                        SKSE::log::info(
                            "[ICONS] '{}' centred: core box middle was {} / {} "
                            "of {}x{} -- padded L{} R{} T{} B{}",
                            m_pending.obj->GetName(), cx, cy, trimW, trimH,
                            padL, padR, padT, padB);
                        sprite = std::move(padded);
                        trimW = nw;
                        trimH = nh;
                    }
                }
            }
        }

        // ★Store at the size the TILE can actually show, not the size we
        // captured at. Rendering the model large is what buys the detail
        // (see kIconCaptureScale) — keeping every one of those pixels
        // afterwards does not: a 1x1 potion occupies ~46 screen px and its
        // sprite was ~500, so the pak grew to 1.2GB of pixels no one ever
        // sees. Downscaling from the big capture is SUPERSAMPLING, so the
        // stored icon is sharper than one captured small would have been.
        // The budget follows the footprint (a 2x4 greatbow really is four
        // times a ring on screen), and the alpha-weighted box filter is the
        // same one the mip chain uses — a plain average pulls edge colour
        // toward the transparent margin and rings the sprite.
        // Inspect is exempt: it draws at 62% of screen height, not in a cell.
        if (!m_pendingInspect) {
            const IconDef sd = ResolveDef(m_pending.obj);
            const int cells = std::clamp((std::max)(sd.w, sd.h), 1, 4);
            // ★160 per cell, not 128+96n. The first cut was measured against
            // a 1x1 and left the 3-cell bags (Canvas / Buckled / Adventure)
            // visibly softer than the capture — those carry buckle and strap
            // detail across three cells, and 320px could not hold it.
            const int limit = 160 * cells;   // 1x1 160 .. 2x4 640
            const int longSide = (std::max)(trimW, trimH);
            if (longSide > limit) {
                const int nw = (std::max)(1, trimW * limit / longSide);
                const int nh = (std::max)(1, trimH * limit / longSide);
                // ★TRUE area resampling: every source pixel contributes in
                // proportion to how much of it the destination pixel covers.
                // The first version walked integer pixel ranges, so at a
                // non-power-of-two ratio (600 -> 320) one output pixel took
                // two source pixels and its neighbour took one — uneven
                // sampling, which is exactly the shimmer that read as "lower
                // quality" on the busiest icons. Weights are in 1/256ths of a
                // pixel; alpha still weights the colour so the transparent
                // margin cannot darken the edges.
                std::vector<std::uint8_t> shrunk(static_cast<size_t>(nw) * nh * 4);
                const double fx = static_cast<double>(trimW) / nw;
                const double fy = static_cast<double>(trimH) / nh;
                for (int y = 0; y < nh; ++y) {
                    const double y0 = y * fy, y1 = (y + 1) * fy;
                    const int iy0 = static_cast<int>(y0);
                    const int iy1 = (std::min)(trimH - 1, static_cast<int>(std::ceil(y1)) - 1);
                    for (int x = 0; x < nw; ++x) {
                        const double x0 = x * fx, x1 = (x + 1) * fx;
                        const int ix0 = static_cast<int>(x0);
                        const int ix1 = (std::min)(trimW - 1, static_cast<int>(std::ceil(x1)) - 1);
                        double aw = 0.0, wsum = 0.0, c[3] = { 0.0, 0.0, 0.0 };
                        for (int sy = iy0; sy <= iy1; ++sy) {
                            const double wy = (std::min)(y1, sy + 1.0) - (std::max)(y0, 1.0 * sy);
                            if (wy <= 0.0) continue;
                            for (int sx = ix0; sx <= ix1; ++sx) {
                                const double wx = (std::min)(x1, sx + 1.0) - (std::max)(x0, 1.0 * sx);
                                if (wx <= 0.0) continue;
                                const auto* s = &sprite[(static_cast<size_t>(sy) * trimW + sx) * 4];
                                const double w = wx * wy;
                                const double a = s[3] * w;
                                wsum += w;
                                aw += a;
                                c[0] += s[0] * a; c[1] += s[1] * a; c[2] += s[2] * a;
                            }
                        }
                        auto* dp = &shrunk[(static_cast<size_t>(y) * nw + x) * 4];
                        if (aw > 0.0) {
                            dp[0] = static_cast<std::uint8_t>(std::lround(c[0] / aw));
                            dp[1] = static_cast<std::uint8_t>(std::lround(c[1] / aw));
                            dp[2] = static_cast<std::uint8_t>(std::lround(c[2] / aw));
                        } else {
                            dp[0] = dp[1] = dp[2] = 0;
                        }
                        dp[3] = wsum > 0.0
                            ? static_cast<std::uint8_t>(std::clamp<long>(
                                  std::lround(aw / wsum), 0, 255))
                            : 0;
                    }
                }
                sprite.swap(shrunk);
                trimW = nw;
                trimH = nh;
            }
        }

        Icon icon;
        icon.w = trimW;
        icon.h = trimH;
        icon.tex = CreateMippedTexture(device, sprite.data(), trimW, trimH,
            pixFmt, &icon.srv);
        if (!icon.tex) { giveUp("tex fail"); return; }
        // ★1.0.5: the glow sprite that used to be built here is gone, and with
        // it a CPU downscale+blur on EVERY capture. A precache already skipped
        // it (evicting captures released it unused); now nothing pays for it.

        // (v7's capture-time geometry rotation record was removed: the offline
        // tool no longer needs it — its projection/root-transform bugs were
        // fixed at the source. Reading old v6/v7 pak entries stays supported;
        // all NEW writes are plain v5.)

        // INSPECT: its own slot, keyed by nothing, never persisted. Replacing
        // the previous texture here is as safe as the pin recycle below —
        // PostRender runs BEFORE UIRoot::Render builds this frame's draw list.
        if (m_pendingInspect) {
            if (m_inspect) {
                ReleaseIcon(m_inspectIcon);
                m_inspectIcon = icon;
                m_inspectValid = true;
                m_inspectShotRx = m_inspectReqRx;
                m_inspectShotRy = m_inspectReqRy;
                m_inspectShotRz = m_inspectReqRz;
                m_inspectShotH = m_inspectReqH;
            } else {
                // closed mid-capture: this texture was never drawn — drop it
                ReleaseIcon(icon);
            }
            m_pendingBusy = false;
            return;
        }

        m_icons[m_pending.key] = icon;
        if (m_pending.obj == m_pin) {
            // live edit: keep ONE slot — drop the previous intermediate
            // texture and defer the disk write until the pin moves/clears
            if (m_pinLastKey && m_pinLastKey != m_pending.key) {
                if (auto old = m_icons.find(m_pinLastKey); old != m_icons.end()) {
                    if (old->second.srv) old->second.srv->Release();
                    if (old->second.tex) old->second.tex->Release();
                    if (old->second.glowSrv) old->second.glowSrv->Release();
                    if (old->second.glowTex) old->second.glowTex->Release();
                    m_icons.erase(old);
                }
                // the derived dot version of that intermediate goes with it
                if (auto pold = m_pixelIcons.find(m_pinLastKey); pold != m_pixelIcons.end()) {
                    ReleaseIcon(pold->second);
                    m_pixelIcons.erase(pold);
                }
            }
            m_pinLastKey = m_pending.key;
            m_pinSprite  = std::move(sprite);
            m_pinW = trimW;
            m_pinH = trimH;
            m_pinFmt = static_cast<std::uint32_t>(pixFmt);
            // ★Queue the dot version HERE, not from the draw. TickPixelDerive
            // runs between this harvest and the draw, so a key queued by the
            // draw is only reached on the NEXT frame — by which time the next
            // capture has already overwritten the in-memory sprite it needed,
            // and a rotation drag would never resolve to dots at all.
            if (m_style == Style::kPixel) {
                const IconDef pd = ResolveDef(m_pending.obj);
                m_pixelCells[m_pending.key] = { (std::max)(1, pd.w), (std::max)(1, pd.h) };
                if (m_pixelQueued.insert(m_pending.key).second) {
                    m_pixelQueue.push_front(m_pending.key);
                }
            }
        } else {
            SaveToDisk(m_pending.key, trimW, trimH,
                static_cast<std::uint32_t>(pixFmt), sprite);
            SKSE::log::info("[ICONS] cached '{}' {}x{} ({} total, {} queued)",
                m_pending.obj->GetName(), trimW, trimH, m_icons.size(), m_queue.size());
            m_timeoutStreak = 0;   // a real capture landed: the run is broken
            // GI68: it made it -- off the deferred list. This also covers the
            // case where a later, quieter frame captures it without any retry
            // pass at all, which is the common outcome.
            if (!m_deferred.empty() && m_deferred.erase(m_pending.key)) {
                m_deferredObj.erase(m_pending.key);
                if (!m_retryPass) RewriteSlow();   // the pass rewrites once at its end
            }
            // mass precache: keep only the pak copy — the icon was created
            // this call and never drawn, so releasing it here is safe. A grid
            // that later shows the item reloads it from disk on demand.
            if (m_pending.evict) {
                if (auto ev = m_icons.find(m_pending.key); ev != m_icons.end()) {
                    if (ev->second.srv) ev->second.srv->Release();
                    if (ev->second.tex) ev->second.tex->Release();
                    if (ev->second.glowSrv) ev->second.glowSrv->Release();
                    if (ev->second.glowTex) ev->second.glowTex->Release();
                    m_icons.erase(ev);
                }
            } else {
                // ★★This item now has a newer picture, so the one the board has
                // been falling back to is dead weight — release it here rather
                // than let a light drag stack one sprite per angle in VRAM.
                // Order matters: the new key is already in m_icons, so there is
                // never a frame with nothing to draw.
                if (const auto lg = m_lastGood.find(m_pending.obj);
                    lg != m_lastGood.end() && lg->second != m_pending.key) {
                    if (auto old = m_icons.find(lg->second); old != m_icons.end()) {
                        ReleaseIcon(old->second);
                        m_icons.erase(old);
                    }
                    if (auto pold = m_pixelIcons.find(lg->second); pold != m_pixelIcons.end()) {
                        ReleaseIcon(pold->second);
                        m_pixelIcons.erase(pold);
                        m_pixelCells.erase(lg->second);
                    }
                }
                m_lastGood[m_pending.obj] = m_pending.key;
            }
        }
        m_pendingBusy = false;

        // Unload NOW, after the model landed — unloading before it lands is a
        // no-op and lets the 7-slot array fill up (the timeout root cause).
        // Exception: the editor's pinned item stays loaded so rotation edits
        // re-capture within a few frames (no reload round-trip).
        // translucent skins also unload the pinned item: keeping it loaded
        // parks a visible model at screen centre for the whole EDIT session.
        // Cost: rotation edits pay a reload round-trip (~a few frames).
        //
        // Precache (evict) keeps the model loaded instead: the same-nif fast
        // path in ItemPreview::Request then chains enchanted variants without
        // any reload (the idle purge clears the tail once the queue drains).
        //
        // ★★MEASURED, do not "fix" this again. The scene-reset count looks like
        // this exception's fault — 357 resets over a 1823-item precache, one
        // every five captures — so unloading on the precache path too was
        // tried, on the theory that a returned slot would keep the engine's
        // 7-entry array below the reset threshold. The A/B says otherwise:
        //
        //            resets   loads   same-nif reuse   elapsed
        //   keep      357     1786    36               58s
        //   unload    363     1822     0               58s
        //
        // Unloading every item left the reset cadence at EXACTLY five captures
        // (359 of 363 gaps) — the same number, mechanically, as when nothing
        // was unloaded at all. Inv3D::Unload is Inventory3DManager::Clear3D
        // (RELOCATION_ID 50886/51759, see the NG header), and Clear3D does NOT
        // give the slot back: loadedModels only empties at End3D. So the reset
        // is structural — nothing this call site does can change it — and the
        // exception's 36 saved loads are pure profit rather than a trade.
        if (!m_pending.evict && (m_pending.obj != m_pin || Theme::S().translucent)) {
            pv->UnloadCurrent();
        }
    }

    void IconCache::OnRevert()
    {
        // Never touch m_pending.obj here -- if it went stale that dereference is
        // the very crash this exists to prevent. Drop the request by flag only.
        if (m_pendingBusy) ItemPreview::GetSingleton()->UnloadCurrent();
        m_pendingBusy = false;
        m_pendingInspect = false;
        m_pending = Pending{};
        m_queue.clear();
        m_queued.clear();
        // the warm queue names the save being left; kPostLoadGame refills it
        m_warmQueue.clear();
        m_warmDelay = 0;
        // ★★★AND EVERY OTHER PLACE A FORM POINTER SLEEPS. The note above got
        // m_pending right and stopped there. A load destroys and remints every
        // dynamic form (0xFF...): a potion the player brewed, a weapon they
        // enchanted. Two maps were still holding those pointers afterwards.
        //
        // m_deferredObj is the dangerous one -- RetryDeferred (the settings
        // window's "try again") walks it and calls GetFormID() on what it
        // finds, BEFORE the queue's LookupByID revalidation can save it. The
        // `!obj->second` null test never caught this; a destroyed form is not
        // a null pointer. Nothing is lost by dropping the list: an icon still
        // missing gets re-queued the next time its tile asks for one.
        m_deferred.clear();
        m_deferredObj.clear();
        // m_lastGood is keyed BY the pointer rather than holding one, so it
        // cannot crash -- it can lie. Let a new form land on a freed address
        // and this lookup hands back somebody else's sprite, and because the
        // tile path only queues a capture when the icon comes back null, that
        // wrong picture never asks to be replaced.
        m_lastGood.clear();
    }

    std::uint64_t IconCache::VramBytes() const
    {
        std::uint64_t n = 0;
        for (const auto& [key, ic] : m_icons) n += ic.bytes;
        return n;
    }

    void IconCache::QueueWarm(std::vector<RE::FormID> a_forms)
    {
        if (!m_warmEnabled || a_forms.empty()) return;
        m_warmQueue.assign(a_forms.begin(), a_forms.end());
        m_warmDelay = kWarmDelayTicks;
        SKSE::log::info("[ICONS] warm-up queued: {} form(s), starting in ~{}s",
            m_warmQueue.size(), kWarmDelayTicks / 60);
    }

    void IconCache::TrimToBudget()
    {
        // The refill allowance is per FRAME, and this is the once-a-frame
        // place that runs outside the draw. Reset it here so the two can
        // never drift apart. Same for the clock the ages are measured on.
        // ★A menu-open BURST widens it for a few ticks: the open transition
        // is already a covered moment, so a screenful of pak loads there is
        // invisible, while the same loads trickled at 8/frame read as
        // pop-in (user report: first open "느리다").
        if (m_burstFrames > 0) {
            --m_burstFrames;
            m_refillLeft = kBurstRefill;
        } else {
            m_refillLeft = kRefillPerFrame;
        }
        const int now = m_tick.fetch_add(1, std::memory_order_relaxed) + 1;

        // ---- post-load warm-up: make the pak sprites the player is carrying
        // resident BEFORE the first open asks for them. Gentle by design for
        // slow machines: waits out the load spike, then at most kWarmPerTick
        // pak restores per tick (two small reads + uploads), pak-only -- a
        // form with no pak entry is simply dropped; the capture pipeline
        // remains the menu's business. Runs here because this is the one
        // per-tick spot that already owns m_icons outside the draw.
        if (!m_warmQueue.empty() && m_warmEnabled && m_style != Style::kFlat) {
            if (m_warmDelay > 0) {
                --m_warmDelay;
            } else {
                int loaded = 0, seen = 0;
                while (!m_warmQueue.empty() && loaded < kWarmPerTick &&
                       seen < kWarmPerTick * 4) {
                    ++seen;
                    const RE::FormID id = m_warmQueue.front();
                    m_warmQueue.pop_front();
                    auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
                    if (!obj) continue;
                    const std::uint64_t key = KeyFor(obj, ResolveDef(obj));
                    if (m_icons.contains(key) || m_failed.contains(key)) continue;
                    if (LoadFromDisk(key)) ++loaded;
                }
                if (m_warmQueue.empty()) {
                    SKSE::log::info("[ICONS] warm-up done ({} resident)",
                        m_icons.size());
                }
            }
        }

        std::uint64_t total = VramBytes();
        if (total <= kVramBudget) return;

        // ★★Only what the pak can give back. A pinned item's captures are kept
        // OFF disk on purpose (a rotation drag would write one file per
        // degree), so dropping one would not be an eviction -- it would be a
        // deletion, and the edit in progress would lose its subject.
        // ★And nothing drawn in the last couple of seconds. The grid does not
        // tell us what is on screen; the only evidence is that Get() was
        // called, so a window of ticks is what stands in for "visible". Too
        // tight and a scroll evicts the row it is scrolling toward.
        constexpr int kKeepTicks = 120;   // ~2s at 60fps
        ScanPak();

        std::vector<std::pair<int, std::uint64_t>> victims;   // (lastUsed, key)
        victims.reserve(m_icons.size());
        for (const auto& [key, ic] : m_icons) {
            if (key == m_pinLastKey) continue;
            if (now - ic.lastUsed < kKeepTicks) continue;
            if (!g_pakIndex.contains(key)) continue;   // nowhere to load it back from
            victims.emplace_back(ic.lastUsed, key);
        }
        std::sort(victims.begin(), victims.end());   // oldest first

        std::uint64_t freed = 0;
        std::size_t   n = 0;
        for (const auto& [used, key] : victims) {
            if (total - freed <= kVramBudget) break;
            const auto it = m_icons.find(key);
            if (it == m_icons.end()) continue;
            freed += it->second.bytes;
            ++n;
            ReleaseIcon(it->second);
            m_icons.erase(it);
        }
        if (n) {
            SKSE::log::info("[ICONS] trimmed {} icons, {} MB -> {} MB (budget {} MB)",
                n, total >> 20, (total - freed) >> 20, kVramBudget >> 20);
        }
    }

    void IconCache::Clear()
    {
        for (auto& [key, icon] : m_icons) ReleaseIcon(icon);
        // ★The derived dot sprites go too. Their key is the model + rotation,
        // which a RETEXTURE does not change — so an icon-cache reset (the one
        // thing that exists for retextures) would re-capture every realistic
        // sprite and still serve dots quantised from the OLD pixels. Same for
        // a preset import, which replaces the pak under identical keys.
        ReleasePixelIcons();
        m_pixelCells.clear();
        ReleaseIcon(m_inspectIcon);
        m_inspectValid = false;
        m_inspectRetire = false;
        m_pendingInspect = false;
        m_icons.clear();
        m_queue.clear();
        m_queued.clear();
        m_failed.clear();
        m_failLoaded = false;   // persisted fail keys reload on next access
        m_pendingBusy = false;
        m_pinLastKey = 0;
        m_pinSprite.clear();
        // ★Every key it names was just released — leaving them would have Get()
        // hand out a freed SRV, which is a crash, not a stale picture.
        m_lastGood.clear();
    }

    // ★★Drop everything that has not started yet. The global capture light
    // re-keys the whole board on every frame of a drag, and a queue keyed by
    // VALUE cannot dedupe those: three seconds of dragging leaves thousands of
    // entries, each asking for an angle the player has already scrolled past,
    // and the cache spends minutes afterwards photographing history into the
    // pak. QueueCapture already does this for the pinned item (one item, one
    // key); this is the same idea when the change is board-wide.
    // The in-flight capture is left alone — it is a single frame's work and
    // cancelling it mid-engine-load is what the deferred-teardown path exists
    // to avoid.
    void IconCache::PurgeQueue()
    {
        for (const auto& e : m_queue) m_queued.erase(e.key);
        m_queue.clear();
    }
}
