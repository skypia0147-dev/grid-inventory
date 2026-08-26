#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace FUI
{
    // Phase 2 D2: the ONE item definition. Formerly four drifting copies —
    // main.cpp ItemDef / Grid::GridDef / Editor::FullDef / IconDef — plus
    // field-by-field converters at every module boundary; a new def field
    // meant touching seven places. Every module now shares THIS struct (the
    // old names live on below as aliases), and the ini serialization walks
    // the field metatable, so a new numeric field = one member + one row.
    struct ItemDef
    {
        int         w = 1;
        int         h = 1;
        std::string shape;   // polyomino mask, rows of 1/0 joined by '|'
                             // ("11|10|10" = an L). Empty = plain w x h
                             // rectangle. Overrides w/h.
        float rx = -90.0f;   // model orientation in degrees (default: stand weapons up)
        float ry = 0.0f;
        float rz = 0.0f;
        float scale = 1.0f;  // multiplier on the auto fit-to-tile scale
        // GI52: the DRAWN (category) icon is 2D art on its own axis. It cannot
        // share rx/ry/rz — those orient a 3D model — and it cannot share
        // `scale`, which frames a model against a tile. Editing one style must
        // not disturb the other, so it gets its own pair.
        float fscale = 1.0f;   // drawn-icon zoom
        float frot = 0.0f;     // drawn-icon rotation, degrees
        // Where the sprite sits inside its footprint, in CELL WIDTHS, so one
        // value reads the same on a 1x1 and a 2x4.
        //
        // ★★Both axes, and both icon styles. It began as a horizontal-only
        // nudge for DRAWN icons — rotating a drawing moves its visual centre of
        // mass, and tiles are laid out in rows, so sideways was where that
        // showed. Free-form footprints made the other half necessary: an
        // L-shaped print wants its haft down the left column and a T wants the
        // head across the top, and no automatic centre can reach either. The
        // auto rule (occupied-box middle) is right for every symmetric shape;
        // this exists for the ones that are not.
        float fx = 0.0f;       // icon offset X, in cells
        float fy = 0.0f;       // icon offset Y, in cells
        // ★★1.0.5: the capture scene has exactly ONE light (measured: a warm
        // point lamp up and to the screen-left of the item), so how bright an
        // item reads depends on which face it happens to turn toward it — the
        // same book is legible at one rotation and a dark slab at another.
        // These nudge the lamp for THIS item, as an OFFSET from the global
        // default rather than an absolute angle: 0 means "wherever the default
        // is", so retuning the default later moves every item that never
        // needed a special case, and 0/0 costs nothing in the ini.
        float lightAz = 0.0f;  // azimuth offset, degrees (+ = toward screen right)
        float lightEl = 0.0f;  // elevation offset, degrees (+ = higher)
        int   bag = 0;       // 1 = bag item: right-click opens its own grid window
        int   bw = 4;        // bag grid size (columns x rows)
        int   bh = 4;
        // ★Typed bags (PLAN_TYPED_BAGS.md): the BagFilter id this bag accepts —
        // "ore", "alchemy", ... EMPTY = the general-purpose bag this mod has
        // always shipped, which takes anything that overflows. Empty must stay
        // the default: every bag already in a player's save has no accept token
        // and has to keep behaving exactly as before.
        std::string accept;
        int   stack = 0;     // G3: per-item stack cap (0 = category default)
        // ★Multi-pouch: this item is a GOLD POUCH holding up to N (0 = not a
        // pouch). The shipped pouch (0x804) is builtin at 10,000; a future
        // pouch form only needs an ESP record and a "pouchcap:N" here.
        int   pouchCap = 0;
    };

    // the old per-module names — every one is THIS struct now. Kept so call
    // sites read naturally (icon code says IconDef, grid code says GridDef).
    using IconDef = ItemDef;
    namespace Grid { using GridDef = ItemDef; }
    namespace Editor { using FullDef = ItemDef; }

    // bounding box from a "11|10|10" mask (w/h tokens ignored when shape set)
    inline void DeriveShapeBounds(ItemDef& a_def)
    {
        if (a_def.shape.empty()) return;
        int w = 0, h = 1, run = 0;
        for (char ch : a_def.shape) {
            if (ch == '|') { ++h; run = 0; }
            else { w = (std::max)(w, ++run); }
        }
        a_def.w = (std::max)(1, w);
        a_def.h = h;
    }

    namespace detail
    {
        // ---- ini serialization metatable ("k:v, k:v" lines) ----
        // ONE row per numeric token — ParseItemDef and FormatItemDef both
        // walk this list. `shape` (string) is handled explicitly by both.
        enum class DefRule
        {
            kCoreDims,   // w/h: parsed always, WRITTEN only without a shape
            kCore,       // always parsed and written
            kBagBlock,   // bag/bw/bh: written only when bag is set
            kIfPositive, // stack: written only when > 0
            kIfNotOne,   // GI52 fscale: 1.0 means "untouched" -> omit
            kIfNonZero,  // GI52 frot:   0   means "untouched" -> omit
        };

        struct DefField
        {
            const char*      key;
            float ItemDef::* fmem;      // exactly one of fmem/imem is set
            int   ItemDef::* imem;
            float            minv;
            float            maxv;
            int              decimals;  // printf precision (float fields)
            DefRule          rule;
        };

        inline constexpr float kNoClamp = 1.0e9f;
        inline constexpr DefField kDefFields[] = {
            // ★★A CEILING, THE SAME ONE THE BAG BLOCK USES. These two were the
            // only fields in the table left open-ended, and h is the one that
            // reaches memory unchecked: MaskOf trims w to kCols and then does
            // rows.assign(h, ...). A preset with `h:500000` -- and sharing
            // presets is a shipped feature, so the file need not be ours --
            // costs tens of megabytes per item, on every rebuild. The tallest
            // real item is 4 cells.
            { "w",     nullptr,         &ItemDef::w,     1.0f,     16.0f,    0, DefRule::kCoreDims },
            { "h",     nullptr,         &ItemDef::h,     1.0f,     16.0f,    0, DefRule::kCoreDims },
            { "rx",    &ItemDef::rx,    nullptr,        -kNoClamp, kNoClamp, 0, DefRule::kCore },
            { "ry",    &ItemDef::ry,    nullptr,        -kNoClamp, kNoClamp, 0, DefRule::kCore },
            { "rz",    &ItemDef::rz,    nullptr,        -kNoClamp, kNoClamp, 0, DefRule::kCore },
            { "scale", &ItemDef::scale, nullptr,        -kNoClamp, kNoClamp, 2, DefRule::kCore },
            { "bag",   nullptr,         &ItemDef::bag,   0.0f,     1.0f,     0, DefRule::kBagBlock },
            // ★16, not 10: the Mysterious Bag is 10x14 and a 10-row ceiling
            // silently cropped it to 10x10 — a def that parses, saves and looks
            // fine while quietly being a different item than the one authored.
            { "bw",    nullptr,         &ItemDef::bw,    1.0f,     16.0f,    0, DefRule::kBagBlock },
            { "bh",    nullptr,         &ItemDef::bh,    1.0f,     16.0f,    0, DefRule::kBagBlock },
            { "stack", nullptr,         &ItemDef::stack, 0.0f,     999.0f,   0, DefRule::kIfPositive },
            { "pouchcap", nullptr,      &ItemDef::pouchCap, 0.0f,  1000000.0f, 0, DefRule::kIfPositive },
            { "fscale", &ItemDef::fscale, nullptr,        0.2f,     4.0f,     2, DefRule::kIfNotOne },
            { "frot",   &ItemDef::frot,   nullptr,       -180.0f,   180.0f,   0, DefRule::kIfNonZero },
            { "fx",     &ItemDef::fx,     nullptr,        -1.0f,    1.0f,     2, DefRule::kIfNonZero },
            { "fy",     &ItemDef::fy,     nullptr,        -1.0f,    1.0f,     2, DefRule::kIfNonZero },
            // capture light, as an offset from the default rig. kIfNonZero is
            // exactly right here: 0/0 IS "use the default", so untouched items
            // add nothing to their line.
            { "laz",    &ItemDef::lightAz, nullptr,      -180.0f,  180.0f,   0, DefRule::kIfNonZero },
            { "lel",    &ItemDef::lightEl, nullptr,       -80.0f,   80.0f,   0, DefRule::kIfNonZero },
        };

        // token position ANCHORED at a field boundary (start / after ',' or
        // whitespace) — a plain find() matched "w:" inside "bw:" and "h:"
        // inside "bh:" on hand-edited lines that order the bag block first
        inline size_t FieldPos(const std::string& a_s, const std::string& a_pat)
        {
            size_t p = a_s.find(a_pat);
            while (p != std::string::npos) {
                if (p == 0 || a_s[p - 1] == ',' || a_s[p - 1] == ' ' || a_s[p - 1] == '\t') {
                    return p;
                }
                p = a_s.find(a_pat, p + 1);
            }
            return std::string::npos;
        }

        inline float FieldNum(const std::string& a_s, const char* a_k, float a_def)
        {
            const std::string pat = std::string(a_k) + ":";
            const auto p = FieldPos(a_s, pat);
            if (p == std::string::npos) return a_def;
            try { return std::stof(a_s.substr(p + pat.size())); } catch (...) { return a_def; }
        }

        inline std::string FieldStr(const std::string& a_s, const char* a_k)
        {
            const std::string pat = std::string(a_k) + ":";
            auto p = FieldPos(a_s, pat);
            if (p == std::string::npos) return "";
            p += pat.size();
            const auto e = a_s.find(',', p);
            std::string v = a_s.substr(p, e == std::string::npos ? std::string::npos : e - p);
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of(" \t\r") + 1);
            return v;
        }
    }

    // Parse a "k:v, k:v" value string over a_base (absent tokens keep the
    // base's value). Replaces the THREE hand-copied parsers that used to
    // live in main.cpp (item ini loader / ParseDefValues / preset loader).
    [[nodiscard]] inline ItemDef ParseItemDef(const std::string& a_val, const ItemDef& a_base)
    {
        ItemDef d;
        d.shape = detail::FieldStr(a_val, "shape");
        if (d.shape.empty()) d.shape = a_base.shape;
        d.accept = detail::FieldStr(a_val, "accept");
        if (d.accept.empty()) d.accept = a_base.accept;
        for (const auto& f : detail::kDefFields) {
            const float base = f.fmem ? a_base.*(f.fmem)
                                      : static_cast<float>(a_base.*(f.imem));
            float v = detail::FieldNum(a_val, f.key, base);
            v = (std::max)(f.minv, (std::min)(f.maxv, v));
            if (f.fmem) d.*(f.fmem) = v;
            else        d.*(f.imem) = static_cast<int>(v);
        }
        DeriveShapeBounds(d);   // shape (if any) owns w/h
        return d;
    }

    // One ini line: "key = shape:S|w:N, h:N, rx:.., ry:.., rz:.., scale:..
    // [, bag:1, bw:N, bh:N][, stack:N]" — same output the hand-written
    // formatter produced.
    [[nodiscard]] inline std::string FormatItemDef(const std::string& a_key, const ItemDef& a_def)
    {
        std::string line = a_key + " =";
        bool first = true;
        auto emit = [&](const std::string& a_frag) {
            line += first ? " " : ", ";
            line += a_frag;
            first = false;
        };
        if (!a_def.shape.empty()) emit("shape:" + a_def.shape);
        char buf[64];
        for (const auto& f : detail::kDefFields) {
            if (f.rule == detail::DefRule::kCoreDims && !a_def.shape.empty()) continue;
            if (f.rule == detail::DefRule::kBagBlock && !a_def.bag) continue;
            if (f.rule == detail::DefRule::kIfPositive && a_def.*(f.imem) <= 0) continue;
            // omit the drawn-icon pair at its defaults: these ride on EVERY
            // item line, and 9600 of "fscale:1.00, frot:0" is pure noise
            if (f.rule == detail::DefRule::kIfNotOne &&
                std::fabs(a_def.*(f.fmem) - 1.0f) < 0.005f) continue;
            if (f.rule == detail::DefRule::kIfNonZero &&
                std::fabs(a_def.*(f.fmem)) < 0.005f) continue;
            if (f.fmem) {
                std::snprintf(buf, sizeof(buf), "%s:%.*f", f.key, f.decimals, a_def.*(f.fmem));
            } else {
                std::snprintf(buf, sizeof(buf), "%s:%d", f.key, a_def.*(f.imem));
            }
            emit(buf);
        }
        // trails the bag block: only a bag can accept anything, and an empty
        // accept is the general-purpose bag — writing "accept:" for those would
        // churn every existing line in the shipped ini for no information
        if (a_def.bag && !a_def.accept.empty()) emit("accept:" + a_def.accept);
        return line;
    }
}
