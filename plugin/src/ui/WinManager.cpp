#include "ui/IconCache.h"
#include "ui/Grid.h"
#include "ui/Equip.h"
#include "ui/Lang.h"
#include "ui/LootBarter.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/Wheeler.h"
#include "ui/WinManager.h"
#include "game/Census.h"
#include "game/DeltaWatch.h"
#include "game/DualRing.h"
#include "game/Ledger.h"

#include <imgui_internal.h>   // ImTextCharFromUtf8 (the tracked-title walk)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace FUI
{
    namespace
    {
        // GI60: style 1 was "stylized", now retired. An ini written by an older
        // build must land on realistic rather than on whatever else holds 1.
        [[nodiscard]] IconCache::Style ParseIconStyle(const std::string& a_raw)
        {
            int v = 0;
            try { v = std::stoi(a_raw); } catch (...) { return IconCache::Style::kRealistic; }
            if (v == 2) return IconCache::Style::kFlat;
            if (v == 3) return IconCache::Style::kPixel;
            return IconCache::Style::kRealistic;
        }

        // ★1.0.5: DISPLAY settings became per-skin. A legacy key carried ONE
        // value for the whole UI, so loading one means writing it to every
        // skin — otherwise an old ini would leave five skins on defaults and
        // the look would change the moment the player switched skin.
        template <class F>
        void EachSkin(F&& a_fn)
        {
            for (int s = 1; s <= Theme::SkinCount(); ++s) a_fn(s);
        }

        // ★★"!disp3" or "!disp[Simple Pine]" -> the skin it means, 0 for none.
        // A BARE NUMBER is an old file speaking positions, so it goes through
        // Theme::MigrateSkin2Index; a bracketed NAME is current and is looked
        // up directly. Both live here rather than at the three call sites,
        // because the previous arrangement -- the same parse copied into Load
        // and ImportPreset -- is exactly how ImportPreset came to have no case
        // for !shad at all and dropped every shared preset's shadows.
        [[nodiscard]] int SkinFromKey(const std::string& a_key, size_t a_prefixLen)
        {
            const std::string tail = a_key.substr(a_prefixLen);
            if (tail.size() >= 2 && tail.front() == '[' && tail.back() == ']') {
                return Theme::SkinIndexByName(tail.substr(1, tail.size() - 2).c_str());
            }
            try { return Theme::MigrateSkin2Index(std::stoi(tail)); } catch (...) { return 0; }
        }

        // one skin's whole DISPLAY block, the way Load parses it back
        void WriteDispLine(std::ostream& a_out, int a_skin)
        {
            // ★Keyed by NAME. A position is only readable by the build that
            // wrote it -- see Theme.h (SkinIndexByName). Presets travel between
            // installs, so this one matters twice over.
            const std::string id = std::string("[") + Theme::SkinNameAt(a_skin) + "]";
            a_out << "!disp" << id << " = " << Theme::IconStyleOf(a_skin);
            for (int t = 0; t < 3; ++t) a_out << ", " << Theme::GlowStyleOf(a_skin, t);
            for (int t = 0; t < 3; ++t) {
                a_out << ", " << Theme::GlowGainAt(a_skin, t, 0)
                      << ", " << Theme::GlowGainAt(a_skin, t, 1);
            }
            for (int t = 0; t < 3; ++t) a_out << ", " << Theme::IconGainOf(a_skin, t);
            a_out << "\n";
            // ★★1.0.5 shadow gets its OWN line instead of three more fields on
            // !disp. Every earlier build parses !disp by field COUNT (>= 13),
            // so widening it would make an old file and a new one impossible to
            // tell apart while meaning different things — the worst kind of
            // format change, because it fails quietly. A key that simply is not
            // present is unambiguous: the defaults stand.
            //   !shadN = [dist, blur, opacity] x 3 icon styles
            a_out << "!shad" << id;
            for (int t = 0; t < 3; ++t) {
                a_out << (t == 0 ? " = " : ", ")
                      << Theme::ShadowAt(a_skin, t, 0) << ", "
                      << Theme::ShadowAt(a_skin, t, 1) << ", "
                      << Theme::ShadowAt(a_skin, t, 2);
            }
            a_out << "\n";
        }
    }

    static constexpr const char* kUiIniPath = "Data/SKSE/Plugins/GridInventory_ui.ini";
    // GI48: named presets live beside the plugin as GridInventory_<name>.ini
    static constexpr const char* kPresetPrefix = "Data/SKSE/Plugins/GridInventory_";

    // Windows-illegal filename characters flattened; blank falls back to
    // "Default" (the no-name export the settings row promises)
    static std::string SanitizePresetName(const std::string& a_name)
    {
        std::string out;
        for (char c : a_name) {
            const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' ||
                             c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
            out += bad ? '_' : c;
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
        return out.empty() ? "Default" : out;
    }

    // config files share the GridInventory_ prefix -- never list them as presets
    static bool ReservedPresetName(std::string a_name)
    {
        for (auto& c : a_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static constexpr const char* kReserved[] = {
            "ui", "layout", "items", "categories", "icons", "iconfail", "slots"
        };
        for (const char* r : kReserved) {
            if (a_name == r) return true;
        }
        return a_name.ends_with("_icons");   // icon bundles (defensive)
    }

    std::string WinManager::PresetIniPath(const std::string& a_name)
    {
        return kPresetPrefix + SanitizePresetName(a_name) + ".ini";
    }

    std::string WinManager::PresetPakPath(const std::string& a_name)
    {
        return kPresetPrefix + SanitizePresetName(a_name) + "_icons.pak";
    }

    std::vector<std::string> WinManager::ListPresets() const
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("Data/SKSE/Plugins", ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".ini") continue;
            const std::string stem = e.path().stem().string();
            constexpr std::string_view prefix = "GridInventory_";
            if (!stem.starts_with(prefix)) continue;
            std::string name = stem.substr(prefix.size());
            if (name.empty() || ReservedPresetName(name)) continue;
            out.push_back(std::move(name));
        }
        std::sort(out.begin(), out.end());
        const auto def = std::find(out.begin(), out.end(), "Default");
        if (def != out.end()) std::rotate(out.begin(), def, def + 1);
        return out;
    }

    WinManager* WinManager::GetSingleton()
    {
        static WinManager singleton;
        return std::addressof(singleton);
    }

    WinManager::Win& WinManager::Ensure(const std::string& a_key)
    {
        for (auto& w : m_wins) {
            if (w.key == a_key) return w;
        }
        m_wins.push_back({});
        m_wins.back().key = a_key;
        return m_wins.back();
    }

    WinManager::Win* WinManager::Find(const std::string& a_key)
    {
        for (auto& w : m_wins) {
            if (w.key == a_key) return &w;
        }
        return nullptr;
    }

    bool WinManager::IsOpen(const Win& a_win) const
    {
        // drawn within the last couple of ImGui frames = participates in
        // magnet/dock (closed bag windows must not attract or adopt)
        return a_win.lastSeen >= 0 && ImGui::GetFrameCount() - a_win.lastSeen <= 2;
    }

    ImVec2 WinManager::MainCenter(ImVec2 a_fallback)
    {
        if (auto* m = Find("main"); m && m->posKnown) {
            return ImVec2(m->pos.x + m->size.x * 0.5f, m->pos.y + m->size.y * 0.5f);
        }
        return a_fallback;
    }

    // ---- persistence (F6: replaces localStorage fabinv_winpos/winparent) ----
    // line format:  key = x,y,w,h[,parent:<key>]

    bool WinManager::ReadWheelEnabled(bool a_default)
    {
        std::ifstream in(kUiIniPath);
        if (!in) return a_default;
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            if (key != "!wheelon") continue;
            try {
                return std::stoi(line.substr(eq + 1)) != 0;
            } catch (...) {
                return a_default;
            }
        }
        return a_default;
    }

    void WinManager::Load()
    {
        m_loaded = true;
        std::ifstream in(kUiIniPath);
        if (!in) return;

        bool sawFlat = false;   // GI59: did this ini carry drawn-style values?
        bool sawDisp = false;   // 1.0.5: ...and per-skin DISPLAY blocks?
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '[') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) {
                const auto b = s.find_first_not_of(" \t\r");
                const auto e = s.find_last_not_of(" \t\r");
                return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
            };
            const std::string key = trim(line.substr(0, eq));
            if (key.empty()) continue;

            std::string rest = trim(line.substr(eq + 1));
            // ★★"!uiscale" is the SCALE THIS FILE'S POSITIONS WERE WRITTEN AT.
            // It went dead when the UI-scale slider was removed; it is read
            // again now as a stamp, not as a setting. A file without it came
            // from that era and its coordinates are 1.00 pixels, which is
            // exactly the default here -- so nothing has to be converted.
            // Still consumed before the window-layout parser at the foot of
            // this loop, which would otherwise file a phantom window.
            if (key == "!uiscale") {
                try {
                    const float v = std::stof(rest);
                    if (v > 0.2f && v < 8.0f) m_fileScale = v;
                } catch (...) {}
                continue;
            }
            // ★★"!cellscale" stored the BOARD MULTIPLIER; "!scale" stores the
            // SETTING, which is that divided by kScaleBase. Same board size,
            // different number for it — 0.90 became 1.00 — so an old file has
            // to be converted or every player's window would shrink by 10% on
            // the launch after updating. Key renamed for exactly that reason;
            // see !skin / !skin2 / !skin3 for the same mechanism.
            if (key == "!cellscale") {
                try {
                    Theme::SetScaleSetting(std::stof(rest) / Theme::kScaleBase);
                } catch (...) {}
                continue;
            }
            if (key == "!scale") {
                try { Theme::SetScaleSetting(std::stof(rest)); } catch (...) {}
                continue;
            }
            // ★The text-size multiplier, beside the display scale it
            // multiplies. Nothing is baked from it -- it is read every frame by
            // style.FontScaleMain and by Theme::SnapPx -- so arriving late
            // would cost nothing; it is here because this is where the other
            // half of the same number lives.
            if (key == "!fontscale") {
                try { Theme::SetFontScale(std::stof(rest)); } catch (...) {}
                continue;
            }
            // ★1.0.5: global capture-lamp offset, "az, el" in degrees. Loaded
            // BEFORE any icon is asked for, because it is part of every cache
            // key — reading it late would serve one frame of icons keyed on the
            // default rig and then re-photograph the lot.
            if (key == "!caplight") {
                float v[2] = { Theme::kDefCapLightAz, Theme::kDefCapLightEl };
                int n = 0;
                std::istringstream vs(rest);
                for (std::string tok; n < 2 && std::getline(vs, tok, ','); ++n) {
                    try { v[n] = std::stof(trim(tok)); } catch (...) {}
                }
                if (n == 2) Theme::SetCaptureLight(v[0], v[1]);
                continue;
            }
            // ★★THE KEY NAMES THE ERA, and that is the whole mechanism —
            // converting on every load would walk a player's skin down by one
            // each launch. Three eras now:
            //   !skin  a position, before "Fable Amber" was removed
            //   !skin2 a position, before the Glass pair went and Sumi moved up
            //   !skin3 a NAME, which no reorder can invalidate
            if (key == "!skin") {
                try { Theme::SetSkinLegacy(std::stoi(rest)); } catch (...) {}
                continue;
            }
            if (key == "!skin2") {
                try { Theme::SetSkinLegacy2(std::stoi(rest)); } catch (...) {}
                continue;
            }
            if (key == "!skin3") {
                Theme::SetSkinByName(rest.c_str());
                continue;
            }
            // Diagnostics switch, read from the same ini the skin and
            // language live in. OFF unless the file says so -- it is here for
            // reports that cannot be reproduced on the author's machine: the
            // [POOL] / [TAKE] lines name the pool, the signature and which
            // unit actually came off the board.
            // Test switch, not a setting: see Grid::SetSimDrift.
            if (key == "!simdrift") {
                Grid::SetSimDrift(rest == "1" || rest == "true");
                continue;
            }
            if (key == "!pooltrace") {
                Grid::SetPoolTrace(rest == "1" || rest == "true");
                continue;
            }
            // 1.4 / B0: engine-delta observation. Writes a lot and changes
            // nothing -- see DeltaWatch.h.
            if (key == "!delta") {
                DeltaWatch::SetEnabled(rest == "1" || rest == "true");
                continue;
            }
            // 1.4 / B1: kind-level audit -- ON BY DEFAULT since its promotion
            // (the take's assignment steers the rebuild's relabel pairing);
            // this line is the escape hatch ("!census = 0"). See Census.h.
            if (key == "!census") {
                Census::SetEnabled(rest == "1" || rest == "true");
                continue;
            }
            // Post-load icon warm-up (first-open latency). ON by default;
            // this is the escape hatch for machines where any background
            // I/O after a load is unwelcome ("!warmicons = 0").
            if (key == "!warmicons") {
                IconCache::GetSingleton()->SetWarmEnabled(
                    rest == "1" || rest == "true");
                continue;
            }
            // Carrier biped slot pin (editor 44..60) -- see DualRing.h. A
            // modlist fact, so it is the player's line to write.
            if (key == "!ring2slot") {
                try { DualRing::SetSlotOverride(std::stoi(rest)); } catch (...) {}
                continue;
            }
            // Scancode that hands a screen to the engine and back -- a
            // diagnostic, so it ships unassigned. 87 = 0x57 = F11. See
            // UIRoot::SetVanillaKey.
            if (key == "!vanillakey") {
                try { UIRoot::SetVanillaKey(std::stoi(rest)); } catch (...) {}
                continue;
            }
            // Accessory drawer, open or shut. A screen preference, so it
            // lives here rather than in the cosave.
            if (key == "!accdrawer") {
                Equip::SetDrawerOpen(rest == "1" || rest == "true");
                continue;
            }
            // Test switch, not a setting: see Grid::SetRebuildDrop.
            if (key == "!rbdrop") {
                Grid::SetRebuildDrop(rest.c_str());
                continue;
            }
            // 1.4 / B3-c: where do rebuilds come from.
            if (key == "!rbtrace") {
                Grid::SetRebuildTrace(rest == "1" || rest == "true");
                continue;
            }
            // ★W3: carry-weight bonus -> extra cells. Three values: CW per
            // cell (0 = off), baseline (0 = auto: the race's own base), max
            // bonus cells.
            if (key == "!cwcells") {
                int v[3] = { 10, 0, 50 };
                int n = 0;
                std::istringstream vs(rest);
                for (std::string tok; n < 3 && std::getline(vs, tok, ','); ++n) {
                    try { v[n] = std::stoi(tok); } catch (...) {}
                }
                Grid::SetCwCells(v[0], v[1], v[2]);
                continue;
            }
            // Request ledger -- ON BY DEFAULT since its promotion to permanent
            // wiring; this line is the escape hatch ("!ledger = 0"), kept for
            // bisecting reports. See Ledger.h.
            if (key == "!ledger") {
                Ledger::SetEnabled(rest == "1" || rest == "true");
                continue;
            }
            // Test switch, not a setting: see Ledger::SimRefuse.
            if (key == "!simrefuse") {
                try { Ledger::SetSimRefuse(std::stoi(rest)); } catch (...) {}
                continue;
            }
            if (key == "!lang") {
                // ★GI71: stored as an id ("en"/"ko"/"pl"), not an index. Files
                // join the list, so an index silently pointed at a different
                // language the moment one was installed or removed.
                if (const int byId = Lang::IndexOfId(rest.c_str()); byId >= 0) {
                    Lang::SetLang(byId);
                } else {
                    // ★GI74: a pre-1.0.4 save holds a bare 0..3. Those numbers
                    // are NOT list positions any more — only English is built
                    // in now and the rest sort by #order — so they have to be
                    // translated through the old fixed table, not fed to
                    // SetLang. Reading "1" as an index would land on whichever
                    // file happens to sort first.
                    static constexpr const char* kLegacy[4] = { "en", "ko", "zh", "ja" };
                    try {
                        const int n = std::stoi(rest);
                        if (n >= 0 && n < 4) {
                            Lang::SetLang((std::max)(0, Lang::IndexOfId(kLegacy[n])));
                        }
                    } catch (...) {}
                }
                continue;
            }
            // ★GI59: glow and icon light are per ICON STYLE, and every key here
            // NAMES ITS SLOT. Never the active-style setters: "!iconstyle" is
            // read further down this same loop, so the active style is still
            // the default while these lines are parsed and the values would
            // all land in slot 0.
            // ★★1.0.5: the whole DISPLAY block for ONE skin, on one line.
            // Spelling it out per axis would be 13 keys x 6 skins = 78 lines of
            // ini nobody could read. Order:
            //   iconStyle, glowStyle[3], glowGain[3][2], iconGain[3]
            if (key.rfind("!disp", 0) == 0) {
                const int skin = SkinFromKey(key, 5);
                if (skin <= 0) continue;   // a skin that no longer exists
                std::vector<float> v;
                std::istringstream vs(rest);
                for (std::string tok; std::getline(vs, tok, ','); ) {
                    try { v.push_back(std::stof(trim(tok))); } catch (...) { v.push_back(0.0f); }
                }
                if (v.size() >= 13) {
                    Theme::SetIconStyleOf(skin, static_cast<int>(v[0]));
                    for (int t = 0; t < 3; ++t) {
                        Theme::SetGlowStyleOf(skin, t, static_cast<int>(v[1 + t]));
                        Theme::SetGlowGainAt(skin, t, 0, v[4 + t * 2]);
                        Theme::SetGlowGainAt(skin, t, 1, v[5 + t * 2]);
                        Theme::SetIconGainOf(skin, t, v[10 + t]);
                    }
                    sawDisp = true;
                }
                continue;
            }
            // ★1.0.5 item shadow: [dist, blur, opacity] x 3 icon styles.
            // Absent in anything written before 1.0.5, and that is fine — the
            // seeded defaults are already in place when this runs.
            if (key.rfind("!shad", 0) == 0) {
                const int skin = SkinFromKey(key, 5);
                if (skin <= 0) continue;
                std::vector<float> v;
                std::istringstream vs(rest);
                for (std::string tok; std::getline(vs, tok, ','); ) {
                    try { v.push_back(std::stof(trim(tok))); } catch (...) { v.push_back(0.0f); }
                }
                if (v.size() >= 9) {
                    for (int t = 0; t < 3; ++t) {
                        for (int a = 0; a < 3; ++a) {
                            Theme::SetShadowAt(skin, t, a, v[t * 3 + a]);
                        }
                    }
                }
                continue;
            }
            // ---- legacy, pre-1.0.5: one set of values shared by every skin.
            // Applied to ALL skins so an existing setup opens looking exactly
            // as it did, and only drifts apart once a skin is tuned.
            if (key == "!glowstyle") {   // realistic: 1 silhouette / 0 radial
                try { EachSkin([&](int s) { Theme::SetGlowStyleOf(s, 0, std::stoi(rest)); }); } catch (...) {}
                continue;
            }
            if (key == "!fglowstyle") {  // drawn
                try { EachSkin([&](int s) { Theme::SetGlowStyleOf(s, 1, std::stoi(rest)); }); } catch (...) {}
                sawFlat = true;
                continue;
            }
            if (key == "!glowgain0" || key == "!glowgain1") {
                try { EachSkin([&](int s) { Theme::SetGlowGainAt(s, 0, key[9] - '0', std::stof(rest)); }); } catch (...) {}
                continue;
            }
            if (key == "!fglowgain0" || key == "!fglowgain1") {
                try { EachSkin([&](int s) { Theme::SetGlowGainAt(s, 1, key[10] - '0', std::stof(rest)); }); } catch (...) {}
                sawFlat = true;
                continue;
            }
            if (key == "!icongain") {    // item icon brightness, realistic
                try { EachSkin([&](int s) { Theme::SetIconGainOf(s, 0, std::stof(rest)); }); } catch (...) {}
                continue;
            }
            if (key == "!ficongain") {   // drawn
                try { EachSkin([&](int s) { Theme::SetIconGainOf(s, 1, std::stof(rest)); }); } catch (...) {}
                sawFlat = true;
                continue;
            }
            // pixel slot (2). No sawFlat here: that flag exists to migrate
            // pre-GI59 inis onto the drawn slot, and pixel postdates all of it.
            if (key == "!pglowstyle") {
                try { EachSkin([&](int s) { Theme::SetGlowStyleOf(s, 2, std::stoi(rest)); }); } catch (...) {}
                continue;
            }
            if (key == "!pglowgain0" || key == "!pglowgain1") {
                try { EachSkin([&](int s) { Theme::SetGlowGainAt(s, 2, key[10] - '0', std::stof(rest)); }); } catch (...) {}
                continue;
            }
            if (key == "!picongain") {
                try { EachSkin([&](int s) { Theme::SetIconGainOf(s, 2, std::stof(rest)); }); } catch (...) {}
                continue;
            }
            if (key == "!wheelon") {        // quick wheel on/off
                try { Wheeler::SetEnabled(std::stoi(rest) != 0); } catch (...) {}
                continue;
            }
            if (key == "!merchgoldinf") {   // F3: unlimited merchant gold
                try { LootBarter::SetMerchantGoldInfinite(std::stoi(rest) != 0); } catch (...) {}
                continue;
            }
            if (key == "!merchbuyall") {    // F4: vendor category lift
                try { LootBarter::SetMerchantBuysAll(std::stoi(rest) != 0); } catch (...) {}
                continue;
            }
            // ★★!wheelkb / !wheelpad are READ NO MORE, and existing lines are
            // skipped rather than obeyed. The quick menu answers to the game's
            // own Favorites binding now, and two sources for one key is a bug
            // waiting for its moment: this file is re-read every time the
            // inventory opens, so a stale line here quietly took the hotkey
            // back on the first visit -- the wheel worked until you opened your
            // bag, then stopped, with nothing in the log to connect the two.
            if (key == "!wheelkb" || key == "!wheelpad") continue;
            if (key == "!iconstyle") {      // 0 realistic / 2 drawn / 3 pixel (1 = retired)
                try {
                    const int v = static_cast<int>(ParseIconStyle(rest));
                    EachSkin([&](int s) { Theme::SetIconStyleOf(s, v); });
                } catch (...) {}
                continue;
            }
            if (key == "!glowgain") {    // legacy single-value key -> every slot
                try {
                    const float g = std::stof(rest);
                    EachSkin([&](int sk) {
                        for (int s = 0; s < 3; ++s)
                            for (int gs = 0; gs < 2; ++gs) Theme::SetGlowGainAt(sk, s, gs, g);
                    });
                } catch (...) {}
                continue;
            }
            std::string parent;
            if (const auto pp = rest.find("parent:"); pp != std::string::npos) {
                parent = trim(rest.substr(pp + 7));
                rest = rest.substr(0, pp);
            }
            float x = 0, y = 0, w = 0, h = 0;
            char c;
            std::istringstream ss(rest);
            if (!(ss >> x >> c >> y >> c >> w >> c >> h)) continue;

            auto& win = Ensure(key);
            win.pos = ImVec2(x, y);
            if (w > 0 && h > 0) win.size = ImVec2(w, h);
            win.parent = parent;
            win.posKnown = true;
        }
        // An ini written before GI59 has no "!f*" keys at all. Copy the
        // realistic values across so an existing setup opens looking EXACTLY
        // as it did; the two styles only drift apart once they are tuned.
        // ★Skipped when the file already carried per-skin blocks: those are
        // complete on their own, and copying slot 0 over slot 1 would undo a
        // drawn-icon setting the player had deliberately made different.
        if (!sawFlat && !sawDisp) {
            EachSkin([](int s) {
                Theme::SetGlowStyleOf(s, 1, Theme::GlowStyleOf(s, 0));
                Theme::SetGlowGainAt(s, 1, 0, Theme::GlowGainAt(s, 0, 0));
                Theme::SetGlowGainAt(s, 1, 1, Theme::GlowGainAt(s, 0, 1));
                Theme::SetIconGainOf(s, 1, Theme::IconGainOf(s, 0));
            });
        }
    }

    namespace
    {
        // GI47: def sections travel through these (registered by main.cpp)
        std::function<void(std::ostream&)>                            g_presetDefsWrite;
        std::function<void(int, const std::string&, const std::string&)> g_presetDefApply;
        std::function<void()>                                         g_presetDefsDone;
    }

    void WinManager::SetPresetDefsHooks(std::function<void(std::ostream&)> a_write,
                                        std::function<void(int, const std::string&,
                                                           const std::string&)> a_apply,
                                        std::function<void()> a_done)
    {
        g_presetDefsWrite = std::move(a_write);
        g_presetDefApply = std::move(a_apply);
        g_presetDefsDone = std::move(a_done);
    }

    void WinManager::ExportPreset(const std::string& a_name) const
    {
        const std::string iniPath = PresetIniPath(a_name);
        std::ofstream out(iniPath, std::ios::trunc);
        if (!out) {
            SKSE::log::error("[PRESET] cannot write {}", iniPath);
            return;
        }
        out << "; GridInventory preset -- ONE file: UI style + item grid definitions.\n";
        out << "; 프리셋 파일입니다 -- 이 파일 하나로 UI 스타일과 아이템 배치 정의까지 공유됩니다.\n";
        out << "; (창 위치와 언어는 담기지 않습니다 / window layout & language never travel)\n";
        out << "; 상인 옵션(무한 골드·전 품목 매입)은 함께 저장됩니다.\n";
        out << "; Merchant options (unlimited gold / buys anything) DO travel.\n";
        out << "!scale = " << Theme::ScaleSetting() << "\n";
        out << "!fontscale = " << Theme::FontScale() << "\n";
        out << "!skin3 = " << Theme::SkinNameAt(Theme::SkinIndex()) << "\n";
        // ★★The capture light MUST travel with a preset, and not because it is
        // part of the look: the preset ships the author's icon pak, and every
        // key in that pak was hashed with this angle. A reader whose global
        // differs would miss all of them and re-photograph the entire set — the
        // one cost exporting the pak exists to avoid.
        out << "!caplight = " << Theme::CaptureLightAz()
            << ", " << Theme::CaptureLightEl() << "\n";
        // ★A preset carries EVERY skin's display block, not just the active
        // one: the point of sharing a preset is that the recipient can switch
        // skins and still see what the author tuned.
        EachSkin([&](int s) { WriteDispLine(out, s); });
        // ★The merchant toggles travel too (author's call). They were held back
        // as "cheats" while window layout and language were, but those two are
        // properties of the READER's setup — a screen size and a language — and
        // these are a property of the SETUP BEING SHARED: a preset built around
        // "the merchant always has gold" plays differently without it. The file
        // says so in its header, and importing is always a deliberate act.
        out << "!wheelon = " << (Wheeler::Enabled() ? 1 : 0) << "\n";
        out << "!merchgoldinf = " << (LootBarter::MerchantGoldInfinite() ? 1 : 0) << "\n";
        out << "!merchbuyall = " << (LootBarter::MerchantBuysAll() ? 1 : 0) << "\n";
        // GI47: the item/category defs ride along -- one file, whole look
        if (g_presetDefsWrite) g_presetDefsWrite(out);
        out.close();
        // and the captured icons, so the reader never waits on re-captures
        IconCache::GetSingleton()->ExportPakTo(PresetPakPath(a_name).c_str());
        SKSE::log::info("[PRESET] preset '{}' exported (style + defs + icons)",
            SanitizePresetName(a_name));
    }

    bool WinManager::ImportPreset(const std::string& a_name)
    {
        std::ifstream in(PresetIniPath(a_name));
        if (!in) return false;
        // 0 = top (style keys), 1 = [categories], 2 = [items],
        // 3 = [glow] (old editor-preset files -- accepted for compatibility)
        int section = 0;
        bool sawFlat = false;   // GI59: does this preset carry drawn-style values?
        bool sawDisp = false;   // 1.0.5: ...and per-skin DISPLAY blocks?
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            // a section header carries no '=' -- plugin names may legally
            // START with '[' ("[ELLE] Sol.esp|0x... = w:2..."), so bracket
            // alone must not switch sections (real data was lost to that once)
            if (line[0] == '[' && line.find('=') == std::string::npos) {
                if (line.find("[categories]") != std::string::npos) section = 1;
                else if (line.find("[items]") != std::string::npos) section = 2;
                else if (line.find("[glow]") != std::string::npos) section = 3;
                else if (line.find("[flat]") != std::string::npos) section = 4;   // GI60
                else section = 0;
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string a_s) {
                const auto b = a_s.find_first_not_of(" \t\r");
                const auto e = a_s.find_last_not_of(" \t\r");
                return b == std::string::npos ? std::string{}
                                              : a_s.substr(b, e - b + 1);
            };
            const std::string key = trim(line.substr(0, eq));
            const std::string rest = trim(line.substr(eq + 1));
            if (key.empty()) continue;
            // STYLE keys. Language and window layout stay the READER's own even
            // if a hand-edited file tries to smuggle them — they describe the
            // reader's screen and the language they read. The merchant toggles
            // DO come across: they describe the setup being shared. See
            // ExportPreset.
            if (key[0] == '!') {
                try {
                    if (key == "!uiscale")        { /* dropped — see Load */ }
                    else if (key == "!cellscale") Theme::SetScaleSetting(   // old units
                                                      std::stof(rest) / Theme::kScaleBase);
                    else if (key == "!scale")     Theme::SetScaleSetting(std::stof(rest));
                    else if (key == "!fontscale") Theme::SetFontScale(std::stof(rest));
                    else if (key == "!skin")      Theme::SetSkinLegacy(std::stoi(rest));
                    else if (key == "!skin2")     Theme::SetSkinLegacy2(std::stoi(rest));
                    else if (key == "!skin3")     Theme::SetSkinByName(rest.c_str());
                    // ★Must be applied BEFORE the preset's pak is adopted —
                    // see ExportPreset: the pak's keys were hashed with it.
                    else if (key == "!caplight") {
                        float v[2] = { Theme::kDefCapLightAz, Theme::kDefCapLightEl };
                        int n = 0;
                        std::istringstream vs(rest);
                        for (std::string tok; n < 2 && std::getline(vs, tok, ','); ++n) {
                            try { v[n] = std::stof(trim(tok)); } catch (...) {}
                        }
                        if (n == 2) Theme::SetCaptureLight(v[0], v[1]);
                    }
                    // ★1.0.5 presets carry one block per skin
                    else if (key.rfind("!disp", 0) == 0) {
                        const int skin = SkinFromKey(key, 5);
                        if (skin <= 0) continue;
                        std::vector<float> v;
                        std::istringstream vs(rest);
                        for (std::string tok; std::getline(vs, tok, ','); ) {
                            try { v.push_back(std::stof(trim(tok))); } catch (...) { v.push_back(0.0f); }
                        }
                        if (v.size() >= 13) {
                            Theme::SetIconStyleOf(skin, static_cast<int>(v[0]));
                            for (int t = 0; t < 3; ++t) {
                                Theme::SetGlowStyleOf(skin, t, static_cast<int>(v[1 + t]));
                                Theme::SetGlowGainAt(skin, t, 0, v[4 + t * 2]);
                                Theme::SetGlowGainAt(skin, t, 1, v[5 + t * 2]);
                                Theme::SetIconGainOf(skin, t, v[10 + t]);
                            }
                            sawDisp = true;
                        }
                    }
                    // ★★1.0.5 shadow, and it MUST travel. ExportPreset has
                    // written !shadN since the feature landed (WriteDispLine
                    // emits it next to !dispN), but this reader had no case for
                    // it, so the key fell through the chain and was dropped: a
                    // shared preset arrived with the author's skin, icon gain
                    // and capture light, and the reader's own shadow. On the
                    // dark skins that is not a detail — the light silhouette is
                    // what keeps a dark item legible on a dark cell, and its
                    // distance/blur/opacity are exactly these three numbers.
                    // Same field order and same tolerance as Load.
                    else if (key.rfind("!shad", 0) == 0) {
                        const int skin = SkinFromKey(key, 5);
                        if (skin <= 0) continue;
                        std::vector<float> v;
                        std::istringstream vs(rest);
                        for (std::string tok; std::getline(vs, tok, ','); ) {
                            try { v.push_back(std::stof(trim(tok))); } catch (...) { v.push_back(0.0f); }
                        }
                        if (v.size() >= 9) {
                            for (int t = 0; t < 3; ++t) {
                                for (int a = 0; a < 3; ++a) {
                                    Theme::SetShadowAt(skin, t, a, v[t * 3 + a]);
                                }
                            }
                        }
                    }
                    // Merchant options: same keys and same tolerance as Load.
                    else if (key == "!wheelon") {
                        Wheeler::SetEnabled(std::stoi(rest) != 0);
                    }
                    else if (key == "!merchgoldinf") {
                        LootBarter::SetMerchantGoldInfinite(std::stoi(rest) != 0);
                    }
                    else if (key == "!merchbuyall") {
                        LootBarter::SetMerchantBuysAll(std::stoi(rest) != 0);
                    }
                    // ---- legacy: one value for the whole UI -> every skin
                    else if (key == "!glowstyle") EachSkin([&](int s) { Theme::SetGlowStyleOf(s, 0, std::stoi(rest)); });
                    else if (key == "!glowgain0") EachSkin([&](int s) { Theme::SetGlowGainAt(s, 0, 0, std::stof(rest)); });
                    else if (key == "!glowgain1") EachSkin([&](int s) { Theme::SetGlowGainAt(s, 0, 1, std::stof(rest)); });
                    else if (key == "!icongain")  EachSkin([&](int s) { Theme::SetIconGainOf(s, 0, std::stof(rest)); });
                    else if (key == "!fglowstyle") {
                        EachSkin([&](int s) { Theme::SetGlowStyleOf(s, 1, std::stoi(rest)); }); sawFlat = true;
                    } else if (key == "!fglowgain0") {
                        EachSkin([&](int s) { Theme::SetGlowGainAt(s, 1, 0, std::stof(rest)); }); sawFlat = true;
                    } else if (key == "!fglowgain1") {
                        EachSkin([&](int s) { Theme::SetGlowGainAt(s, 1, 1, std::stof(rest)); }); sawFlat = true;
                    } else if (key == "!ficongain") {
                        EachSkin([&](int s) { Theme::SetIconGainOf(s, 1, std::stof(rest)); }); sawFlat = true;
                    }
                    else if (key == "!iconstyle") {
                        const int v = static_cast<int>(ParseIconStyle(rest));
                        EachSkin([&](int s) { Theme::SetIconStyleOf(s, v); });
                    }
                } catch (...) {}
                continue;
            }
            if (section == 3) {   // old editor-preset glow grammar
                try {
                    if (key == "style")      EachSkin([&](int s) { Theme::SetGlowStyleOf(s, 0, std::stoi(rest)); });
                    else if (key == "gain0") EachSkin([&](int s) { Theme::SetGlowGainAt(s, 0, 0, std::stof(rest)); });
                    else if (key == "gain1") EachSkin([&](int s) { Theme::SetGlowGainAt(s, 0, 1, std::stof(rest)); });
                } catch (...) {}
                continue;
            }
            if ((section == 1 || section == 2 || section == 4) && g_presetDefApply) {
                g_presetDefApply(section, key, rest);   // merge: preset wins
            }
        }
        // A preset shared before GI59 carries only the realistic values; give
        // the drawn style the same ones so the preset looks as its author saw
        // it in either style, instead of half-applying.
        if (!sawFlat && !sawDisp) {
            EachSkin([](int s) {
                Theme::SetGlowStyleOf(s, 1, Theme::GlowStyleOf(s, 0));
                Theme::SetGlowGainAt(s, 1, 0, Theme::GlowGainAt(s, 0, 0));
                Theme::SetGlowGainAt(s, 1, 1, Theme::GlowGainAt(s, 0, 1));
                Theme::SetIconGainOf(s, 1, Theme::IconGainOf(s, 0));
            });
        }
        if (g_presetDefsDone) g_presetDefsDone();
        SKSE::log::info("[PRESET] preset '{}' imported (style + defs)",
            SanitizePresetName(a_name));
        return true;
    }

    void WinManager::Save() const
    {
        std::ofstream out(kUiIniPath, std::ios::trunc);
        // ★★A SILENT RETURN HERE LOSES EVERY SETTING THE PLAYER TOUCHED. It was
        // silent, and the failure looked exactly like a feature not working:
        // the quick wheel switch reported success in its own log, changed the
        // game immediately, and was gone on the next launch -- because nothing
        // between the button and the disk ever said the write had failed.
        // A write that can fail must say so.
        if (!out) {
            SKSE::log::error("[UI] SETTINGS NOT SAVED -- cannot open {} for writing. "
                             "Nothing changed in the settings panel will survive a restart.",
                             kUiIniPath);
            return;
        }
        out << "; GridInventory window layout (auto-generated)\n";
        out << "; key = x,y,w,h[,parent:<key>]\n";
        // ★★The scale THESE POSITIONS ARE IN. Sizes below are not
        // restored -- ApplyNext recomputes them every frame -- so the stamp is
        // for the coordinates alone: Load multiplies them by (current / this)
        // so a layout carried to another UI scale keeps its arrangement rather
        // than bunching up. Absent in a file from before 1.2.1, where it reads
        // as 1.00 -- exactly what those files were written at.
        // ★NOT in ExportPreset: a preset never carries window layout.
        out << "!uiscale = " << Theme::Scale() << "\n";
        out << "!scale = " << Theme::ScaleSetting() << "\n";
        out << "!fontscale = " << Theme::FontScale() << "\n";
        out << "!skin3 = " << Theme::SkinNameAt(Theme::SkinIndex()) << "\n";
        out << "!lang = " << Lang::Id(Lang::Get()) << "\n";
        // Diagnostic / test switches survive a restart once turned on --
        // a tester should not have to re-arm them every session. Written
        // only while ON, so an ordinary install never carries them.
        if (Grid::PoolTrace()) out << "!pooltrace = 1\n";
        if (Grid::SimDrift())  out << "!simdrift = 1\n";
        if (DeltaWatch::Enabled()) out << "!delta = 1\n";
        // ★Inverted since their promotions: ON is the default, so the line is
        // written only while OFF -- the escape hatch survives a restart, and
        // an ordinary install still carries no line.
        if (!Census::Enabled())    out << "!census = 0\n";
        if (!Ledger::Enabled())    out << "!ledger = 0\n";
        if (!IconCache::GetSingleton()->WarmEnabled()) out << "!warmicons = 0\n";
        if (DualRing::SlotOverride() >= 0) {
            out << "!ring2slot = " << DualRing::SlotOverride() << "\n";
        }
        if (UIRoot::VanillaKey() != 0) {
            out << "!vanillakey = " << UIRoot::VanillaKey() << "\n";
        }
        if (Equip::DrawerOpen())   out << "!accdrawer = 1\n";
        if (Grid::RebuildTrace())  out << "!rbtrace = 1\n";
        // Carry Weight bonus -> extra inventory cells
        // 소지 중량 보너스의 칸 환전
        out << "; !cwcells = CW per cell (0 = off), baseline (0 = auto: race base), max bonus cells\n";
        out << "; !cwcells = 칸당 CW (0 = 끔), 기준선 (0 = 자동: 종족 기본치), 보너스 칸 상한\n";
        out << "!cwcells = " << Grid::CwPerCell() << ", " << Grid::CwBase()
            << ", " << Grid::CwMaxCells() << "\n";
        out << "; !caplight = capture lamp offset in degrees (az, el)\n";
        out << "!caplight = " << Theme::CaptureLightAz()
            << ", " << Theme::CaptureLightEl() << "\n";
        // ★1.0.5: one DISPLAY block per skin. Slot-named, never the
        // active-style getters — see Theme.h (GI59).
        //   iconStyle, glowStyle[3], glowGain[3][2], iconGain[3]
        out << "; !disp[skin] = iconStyle, glowStyle x3, glowGain x6, iconGain x3\n";
        out << "; !shad[skin] = [shadow dist, blur, opacity] x3 icon styles\n";
        EachSkin([&](int s) { WriteDispLine(out, s); });
        out << "!wheelon = " << (Wheeler::Enabled() ? 1 : 0) << "\n";
        out << "!merchgoldinf = " << (LootBarter::MerchantGoldInfinite() ? 1 : 0) << "\n";
        out << "!merchbuyall = " << (LootBarter::MerchantBuysAll() ? 1 : 0) << "\n";
        // ★No wheel hotkey written any more -- see the reader above. It lives
        // in the game's own controls, and writing a second copy here is what
        // let an old value climb back in.
        for (const auto& w : m_wins) {
            if (!w.posKnown) continue;
            out << w.key << " = "
                << static_cast<int>(w.pos.x) << ',' << static_cast<int>(w.pos.y) << ','
                << static_cast<int>(w.size.x) << ',' << static_cast<int>(w.size.y);
            if (!w.parent.empty()) out << ",parent:" << w.parent;
            out << "\n";
        }
        // ★Checked AFTER the writes, not just at open. A stream can open and
        // still fail to commit -- a full disk, a virtualised path that accepts
        // the handle and drops the bytes -- and the caller has no other way to
        // find out.
        out.flush();
        if (!out) {
            SKSE::log::error("[UI] SETTINGS NOT SAVED -- {} opened but the write failed.",
                             kUiIniPath);
            return;
        }
        // ★★Say WHERE, not just "saved". The path is RELATIVE, so where it
        // lands depends on the working directory and on whatever virtual file
        // system the launcher put in front of it -- and a mod manager may
        // redirect the write somewhere neither the player nor this code can
        // guess. "It reported success and the file did not change" is not a
        // state anyone can debug without this line.
        // ★Logged once per session: this fires on every window move, and a
        // path that never changes does not need saying sixty times.
        static bool s_saidWhere = false;
        if (!s_saidWhere) {
            s_saidWhere = true;
            std::error_code ec;
            const auto abs = std::filesystem::absolute(kUiIniPath, ec);
            SKSE::log::info("[UI] settings saved -> {}",
                            ec ? kUiIniPath : abs.string());
        }
    }

    // ---- per-window draw helpers ----

    // ★A managed window's size is code-defined, but it is NOT constant: the
    //  main window drops its entire equipment column (~412px) whenever a
    //  partner window is up. Applying that new size while holding the stored
    //  top-left fixed moved the OTHER edge by the whole delta, which is what
    //  users saw as "the inventory jumped left when I opened a chest" and
    //  "the inventory swallowed my bag when I closed the chest".
    //
    //  Two things are needed to make a resize behave: the caller names the
    //  edge that must stay put, and anything DOCKED to this window keeps the
    //  edge it was docked to. Docking already survived a drag (StartDrag's
    //  follower list); this makes it survive a resize as well.
    // ★★POSITIONS ONLY. Sizes are code-defined and ApplyNext has already
    // recomputed them at the new scale, so touching them here would apply the
    // factor twice. Rounded because ImGui works in whole pixels and feeding
    // it fractions is what made a window crawl one frame at a time (GI72).
    void WinManager::RescalePositions(float a_ratio)
    {
        for (auto& w : m_wins) {
            if (!w.posKnown) continue;
            w.pos.x = std::round(w.pos.x * a_ratio);
            w.pos.y = std::round(w.pos.y * a_ratio);
        }
        SKSE::log::info("[WIN] layout rescaled x{:.3f} ({} windows)",
                        a_ratio, m_wins.size());
    }

    void WinManager::Reanchor(const std::string& a_key, ImVec2 a_newSize, Anchor a_anchor)
    {
        auto* w = Find(a_key);
        if (!w) return;
        // ★GI72: compare against the PREVIOUS REQUEST, not against what ImGui
        // handed back. ImGui floors a window to whole pixels, so asking for
        // 919.6 and reading 919.0 looked like a 0.6px layout change EVERY
        // frame -- and with kTopRight each of those "changes" moved the left
        // edge 0.6px further left, so the window crawled off to x=0 and stopped
        // only because the clamp below caught it. It reproduced on most UI
        // scales and not on 0.96 / 1.09 / 1.13: exactly the ones whose computed
        // width lands with a fractional part under the old 0.5 tolerance.
        //
        // The request is rounded in ApplyNext, so this is now an exact test
        // between two integers and the tolerance is only belt-and-braces.
        if (std::abs(w->reqSize.x - a_newSize.x) < 0.5f &&
            std::abs(w->reqSize.y - a_newSize.y) < 0.5f) {
            return;
        }

        const ImVec2 oldMin = w->pos;
        const ImVec2 oldMax(w->pos.x + w->size.x, w->pos.y + w->size.y);

        ImVec2 pos = oldMin;
        if (a_anchor == Anchor::kTopRight) pos.x = oldMax.x - a_newSize.x;
        // Keep it reachable: outside a drag nothing else clamps, and a window
        // that grows leftwards off-screen has no titlebar left to grab.
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (a_newSize.x < disp.x) pos.x = (std::max)(0.0f, (std::min)(disp.x - a_newSize.x, pos.x));
        if (a_newSize.y < disp.y) pos.y = (std::max)(0.0f, (std::min)(disp.y - a_newSize.y, pos.y));

        const ImVec2 newMin = pos;
        const ImVec2 newMax(pos.x + a_newSize.x, pos.y + a_newSize.y);
        const float  dL = newMin.x - oldMin.x, dR = newMax.x - oldMax.x;
        const float  dT = newMin.y - oldMin.y, dB = newMax.y - oldMax.y;
        w->pos = pos;
        if (dL == 0.0f && dR == 0.0f && dT == 0.0f && dB == 0.0f) return;

        // Docked children hold the edge they were flush against. Closed ones
        // are included or they would reopen detached.
        //
        // ★★Level by level, not flat over the subtree. A resize moves only THIS
        // window's edges, so only a DIRECT child can be tested against them; a
        // grandchild is flush against its own parent, which merely TRANSLATES,
        // so it has to inherit that parent's delta instead of being measured
        // against a window it never touched.
        // The old code walked the whole subtree but tested every level against
        // oldMin/oldMax, so the test could only ever match direct children:
        // a bag snapped to the main window followed when the partner panel
        // opened, and a bag snapped to THAT bag was left behind.
        constexpr float eps = 2.0f;
        std::unordered_map<std::string, ImVec2> delta;   // key -> how far it moved
        delta[a_key] = ImVec2(0.0f, 0.0f);               // root: already applied
        std::vector<std::string> order{ a_key };
        // breadth-first, so a parent's delta is settled before its children ask
        for (std::size_t i = 0; i < order.size(); ++i) {
            const std::string cur = order[i];   // by value: `order` grows below
            for (auto& c : m_wins) {
                if (c.parent != cur || c.key == "main") continue;
                if (delta.contains(c.key)) continue;   // cycle / diamond guard
                ImVec2 d(0.0f, 0.0f);
                if (cur == a_key) {
                    if (c.posKnown) {
                        const float cL = c.pos.x, cR = c.pos.x + c.size.x;
                        const float cT = c.pos.y, cB = c.pos.y + c.size.y;
                        if (std::abs(cR - oldMin.x) <= eps)      d.x = dL;
                        else if (std::abs(cL - oldMax.x) <= eps) d.x = dR;
                        if (std::abs(cB - oldMin.y) <= eps)      d.y = dT;
                        else if (std::abs(cT - oldMax.y) <= eps) d.y = dB;
                    }
                } else {
                    d = delta[cur];   // rigidly attached to a parent that moved
                }
                if (c.posKnown) { c.pos.x += d.x; c.pos.y += d.y; }
                delta[c.key] = d;
                order.push_back(c.key);
            }
        }
    }

    void WinManager::ApplyNext(const std::string& a_key, ImVec2 a_defaultPos,
                               ImVec2 a_defaultSize, Anchor a_anchor, float a_topPad)
    {
        if (!m_loaded) Load();
        // ★★DEFERRED FROM Load(). The scale is derived from the display and
        // the ini is read before there is one, so the conversion cannot happen
        // where the file is parsed. It happens once, on the first frame that
        // has a display -- which is this one, because nothing draws earlier.
        if (!m_scaleFixed) {
            const ImVec2 d0 = ImGui::GetIO().DisplaySize;
            if (d0.x > 64.0f && d0.y > 64.0f) {
                m_scaleFixed = true;
                const float from = m_fileScale > 0.0f ? m_fileScale : 1.0f;
                const float r = Theme::Scale() / from;
                if (std::abs(r - 1.0f) > 0.001f) RescalePositions(r);
            }
        }
        // ★The height pays for the title's clearance right here, at the one call
        // that owns the size and still runs before Begin. TitleBar reads the
        // same number back off the record, so "passed the pad but forgot the
        // height" is not a state this can be in.
        a_defaultSize.y += a_topPad;
        // ★GI72: ask for whole pixels. Every size here is derived from the UI
        // scale and lands on fractions at most scale values; ImGui floors them,
        // and any code comparing request to readback then sees a phantom
        // change forever. Rounding at this one choke point makes the two agree
        // and costs at most half a pixel of layout.
        const ImVec2 want(std::round(a_defaultSize.x), std::round(a_defaultSize.y));
        {
            auto& w = Ensure(a_key);
            if (!w.posKnown) {
                w.pos = a_defaultPos;
                w.posKnown = true;
                w.size = want;      // first sight: nothing to re-anchor
                w.reqSize = want;
            }
        }
        // No further insertions past this point — Reanchor/Find must not
        // invalidate the reference taken below.
        Reanchor(a_key, want, a_anchor);

        auto* w = Find(a_key);
        // Size is ALWAYS code-defined (windows aren't user-resizable); only
        // the position persists — otherwise a stale saved size wins forever.
        w->size = want;
        w->reqSize = want;
        w->topPad = a_topPad;
        // whole-pixel position too: ImGui rounds it anyway, and feeding its
        // rounded value back into the next frame's anchor maths is the same
        // trap one level down
        w->pos = ImVec2(std::round(w->pos.x), std::round(w->pos.y));
        // ★A window whose grab handle leaves the screen can never be brought
        // back (user report: with 18 bags the spawn cascade alone started a
        // window below the display, and the position then PERSISTED). Keep
        // the TITLEBAR reachable, not the whole window — parking a window
        // half off-screen stays legal, only the handle may not leave.
        {
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            if (disp.x > 64.0f && disp.y > 64.0f) {
                const float grab = 80.0f;
                w->pos.x = (std::max)(grab - w->size.x,
                                      (std::min)(disp.x - grab, w->pos.x));
                w->pos.y = (std::max)(0.0f, (std::min)(disp.y - 30.0f, w->pos.y));
            }
        }
        ImGui::SetNextWindowPos(w->pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(w->size, ImGuiCond_Always);
    }

    // uppercase + letter-tracked title text (skins set the tracking)
    // ★★★THE LEAD BYTE IS A CLAIM, NOT A GUARANTEE.
    //
    // Both of these walked UTF-8 by reading the lead byte's advertised length
    // and adding it, with nothing checking that the string was actually that
    // long. A name ending in 0xE9 claims three bytes, so `p += len` stepped
    // over the terminating NUL and `while (*p)` kept going through whatever
    // came next -- drawn as glyphs, and an access violation if the buffer sat
    // against a page boundary.
    //
    // Nothing about this needed a malformed file. Titles are game names
    // straight from obj->GetName(), and there is no code-page conversion
    // anywhere in this plugin: the CK writes ESP FULL names in Windows-1252,
    // so an accented mod item does it in English and a Russian or Polish build
    // does it on most container names in the game.
    //
    // ImTextCharFromUtf8 is what the rest of the project already uses (three
    // sites in Theme.cpp) -- given an end pointer it consumes one byte on a
    // broken sequence and never reads past it.
    static float TrackedTextWidth(ImFont* a_font, float a_size, const char* a_text, float a_spacing)
    {
        if (!a_text || !*a_text) return 0.0f;
        float width = 0.0f;
        const char* p = a_text;
        const char* const end = a_text + std::strlen(a_text);
        while (p < end) {
            unsigned int cp = 0;
            const int len = ImTextCharFromUtf8(&cp, p, end);
            if (len <= 0) break;
            width += a_font->CalcTextSizeA(a_size, FLT_MAX, 0.0f, p, p + len).x + a_spacing;
            p += len;
        }
        return width > 0.0f ? width - a_spacing : 0.0f;
    }

    static void DrawTrackedText(ImDrawList* a_dl, ImFont* a_font, float a_size, ImVec2 a_pos,
                                ImU32 a_col, const char* a_text, float a_spacing)
    {
        if (!a_text || !*a_text) return;   // see TrackedTextWidth
        const char* p = a_text;
        const char* const end = a_text + std::strlen(a_text);
        float x = a_pos.x;
        while (p < end) {
            unsigned int cp = 0;
            const int len = ImTextCharFromUtf8(&cp, p, end);
            if (len <= 0) break;
            a_dl->AddText(a_font, a_size, ImVec2(x, a_pos.y), a_col, p, p + len);
            x += a_font->CalcTextSizeA(a_size, FLT_MAX, 0.0f, p, p + len).x + a_spacing;
            p += len;
        }
    }

    float WinManager::TitleBarH() { return 34.0f * Theme::Scale(); }

    float WinManager::TitleTextY(const std::string& a_key, float a_glyphH)
    {
        auto* const w = GetSingleton()->Find(a_key);
        return (w ? w->pos.y : ImGui::GetWindowPos().y) + Theme::FrameInsetY() * 0.5f +
               (w ? w->topPad : 0.0f) + (TitleBarH() - a_glyphH) * 0.5f;
    }

    // ★★★A WINDOW'S GROUND, SEPARATED FROM ITS TITLE.
    //
    // Every skin that paints its own sheet -- torn paper, the ink
    // photograph, the flat panel -- used to do it INSIDE TitleBar, which
    // meant a panel without a title bar had no ground at all. The
    // accessory drawer is exactly that panel, and on the ink skin it came
    // out as floating cells over the world (reported).
    //
    // Split out rather than copied: two places painting the same sheet is
    // how skins drift apart one at a time.
    //
    // a_topPad/a_barH describe the title strip this ground sits under, for
    // the one gradient that needs to know where it ends. A panel with no
    // title passes 0 for both, and the gradient collapses to nothing.
    void WinManager::PaintGround(ImDrawList* dl, ImVec2 wp, ImVec2 we,
                                 const std::string& a_key, float a_topPad,
                                 float a_barH, ImVec2 a_clipMin, ImVec2 a_clipMax)
    {
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const float barH = a_barH;

        // Window chrome must reach the EDGE pixels, but the window drawlist
        // is clipped ~half-padding inside the window (edge-hugging strips and
        // corner-fade lines silently vanished). Visual-only clip override.
        // ★The drawn tearing sticks out past the rect, so the clip has to give
        // it room — otherwise the overhang is cut off square, which is the one
        // shape this whole treatment exists to avoid.
        const float bleed = sk.tornFrame  ? Theme::kTornOut * S
                          : Theme::InkChrome() ? (Theme::kInkBleed + Theme::kInkFade) * S
                                               : 0.0f;
        // ★The override is intentional (see the note above) — but a caller may
        // fence it. Intersecting rather than replacing keeps the bleed for
        // everyone who does not ask for one.
        ImVec2 clipLo(wp.x - bleed, wp.y - bleed);
        ImVec2 clipHi(we.x + bleed, we.y + bleed);
        if (a_clipMax.x > a_clipMin.x && a_clipMax.y > a_clipMin.y) {
            clipLo.x = (std::max)(clipLo.x, a_clipMin.x);
            clipLo.y = (std::max)(clipLo.y, a_clipMin.y);
            clipHi.x = (std::min)(clipHi.x, a_clipMax.x);
            clipHi.y = (std::min)(clipHi.y, a_clipMax.y);
        }
        dl->PushClipRect(clipLo, clipHi, false);

        // ★The window key, hashed. Both the torn silhouette and the ink
        // sheet's cut are derived from it, for the same reason: a window keeps
        // ONE piece of paper for the whole session, and moving or resizing it
        // must not deal a new one.
        unsigned int wseed = 2166136261u;
        for (const char c : a_key) {
            wseed = (wseed ^ static_cast<unsigned char>(c)) * 16777619u;
        }

        // OATHVEIN TORN: 9-slice torn-paper panel fills the window (opaque
        // centre hides the parked model; ragged edges show the world). Drawn
        // first so all chrome/content lands on top.
        if (sk.tornFrame) {
            // ★Drawn, not blitted. The nine-slice stretched its edge strips,
            // and those carry the torn silhouette — so the same paper came out
            // needle-fine on a small bag and coarse on the main window. This
            // walks the border at a fixed 3px and is blind to window size.
            // The seed is the window KEY, so every window keeps one shape for
            // the whole session and moving it changes nothing.
            const unsigned int seed = wseed;
            // ★The SHEET is the window rect; only the teeth go past it. Give
            // the sheet the bled rect instead and its teeth land exactly on
            // the clip boundary, which erases them.
            Theme::TornPanel(dl, wp, we, Theme::Col(sk.winBg, 1.0f), seed);
        }
        // ★★INK: a photographed sheet, stretched to the window. Drawn in the
        // same slot as the torn panel and for the same reason -- it is the
        // window's ground, so everything else has to land on top of it.
        //
        // ★STRETCHED, never tiled. Measured, the sheet's opposite edges differ
        // by 8.3/8.5 luma, so a tile would show its seam. Grain does not mind
        // being stretched, and the vignette arriving at a different ratio on
        // each window makes every window look like its own piece of paper.
        //
        // ★The skin's winBg still tints it. The sheet was baked around that
        // exact colour, so a white tint is a no-op today -- but it means a
        // future ink skin can reuse a sheet and shift it, instead of baking a
        // third 2 MB file to move one hue.
        // ★The sheet is drawn OVERSIZE, by the same amount the frame overhangs.
        // Its edge then lands under the brush instead of beside it -- the
        // window rect is where the CONTENT stops, not where the paper does.
        // ★And its edge dissolves. Even perfectly covered, a razor-cut sheet
        // shows through the frame's dry gaps and past its corners, which is
        // the one thing this whole treatment exists to avoid.
        Theme::PaperPanel(dl,
            ImVec2(wp.x - Theme::kInkBleed * S, wp.y - Theme::kInkBleed * S),
            ImVec2(we.x + Theme::kInkBleed * S, we.y + Theme::kInkBleed * S),
            wseed, Theme::kInkFade * S);
        // bevelChrome (kept for future skins): grey gradient titlebar + full
        // bevel border (dark
        // outer line, light inner line) — classic beveled chrome. The
        // gradient covers the whole inset title zone and ends exactly on the
        // generic (inset) title separator — no extra full-width line that
        // would poke into the border (v12.7).
        // ★The bevel border is TWO lines, and that is the whole grammar: a dark
        // outer edge with a bright line immediately inside it. Collapse it to
        // one and the window stops sitting on top of the world. Colours come
        // from the skin now (acc outer / hi inner) instead of the grey literals
        // a long-retired skin left behind.
        // ★A LIGHT panel wears NO chrome frame at all — no sheen, no outer
        // edge, no inner rim. Every one of those exists to lift a window off a
        // DARK world; over a pale translucent panel any line bright enough to
        // see is brighter than the panel and reads as a seam, and any line
        // dark enough is the skin's darkest token with nothing to stand
        // against. The fill alone separates the window from the world.
        if (sk.bevelChrome && !sk.lightPanel) {
            const float tbB = wp.y + Theme::FrameInsetY() + a_topPad + barH;
            const ImU32 topA = Theme::Col(sk.ink, 0.13f);
            const ImU32 topB = Theme::Col(sk.ink, 0.00f);
            dl->AddRectFilledMultiColor(wp, ImVec2(we.x, tbB), topA, topA, topB, topB);
            // ★The bevel border is TWO lines, and that is the whole grammar: a
            // dark outer edge with a bright line immediately inside it.
            // Collapse it to one and the window stops sitting on the world.
            dl->AddRect(wp, we, Theme::Col(sk.acc, 1.0f), sk.rounding, 0, 2.0f);
            dl->AddRect(ImVec2(wp.x + 2.0f, wp.y + 2.0f),
                ImVec2(we.x - 2.0f, we.y - 2.0f), Theme::Col(sk.hi, 0.95f),
                (std::max)(0.0f, sk.rounding - 1.0f));
        }
        // OATHVEIN: 2px crimson strip + corner-fade border (full border is off)
        if (sk.topStrip) {
            dl->AddRectFilled(wp, ImVec2(we.x, wp.y + 2.0f), IM_COL32(122, 30, 22, 140));
        }
        // ★★INK: the window's edge is four brush marks and four corners, and it
        // REPLACES the light-panel frame below rather than joining it -- a
        // painted edge with a drawn rectangle behind it reads as a mistake, not
        // as two layers. Scaled by the UI scale only, never by the window: a
        // corner is a mark made by a hand, and a hand does not make a bigger
        // mark on a bigger sheet.
        if (Theme::InkChrome()) {
            const float b = Theme::kInkBleed * S;
            Theme::InkFrame(dl, ImVec2(wp.x - b, wp.y - b),
                            ImVec2(we.x + b, we.y + b),
                            Theme::Col(sk.ink, 0.79f), 96.0f * Theme::Scale());
        }
        // window corner-fade border — suppressed when a torn frame is drawn
        // (the torn texture is the border; slots/items still use cornerFade)
        if (sk.cornerFade && !sk.tornFrame) {
            const float top = wp.y + (sk.topStrip ? 2.0f : 0.0f);
            Theme::CornerFade(dl, ImVec2(wp.x, top), we, Theme::Acc(0.55f));
        }

        // ★A light panel gets ONE opaque frame, rounded, drawn on the window's
        // own edge. Two things make a docked pair read as a single line rather
        // than a doubled seam: the snap overlaps neighbours by exactly one
        // stroke (SnapPos), so the two frames land on the SAME pixels, and the
        // colour is opaque, so drawing it twice there changes nothing. A
        // translucent frame would darken at every join — which is the doubled
        // edge this is meant to remove.
        // ★Not under a TORN frame. That frame's edge is ragged by design and a
        // square stroke around it cuts the corners off the illusion — the
        // texture's own edge is the border there.
        // ★...and not under an INK frame either, for the same reason the torn
        // one is exempt: a painted edge with a ruled rectangle behind it reads
        // as a mistake rather than as two layers.
        if (sk.lightPanel && !sk.tornFrame && !Theme::InkChrome()) {
            // ★Draw on the window's OWN rect, with no inset. ImGui fills the
            // background across exactly wp..we; insetting the frame by half a
            // stroke left the outermost half-pixel of that fill uncovered, and
            // on the rounded corners — where the fill's arc is struck from wp
            // and the frame's from wp+inset — it showed as a squared-off ear
            // of panel outside the curve. Centred on the edge, the stroke
            // straddles it and the fill has nowhere to peek out.
            dl->AddRect(wp, we, Theme::WinBorder(), Theme::WinRounding(),
                        0, Theme::BorderPx());
            // ★A window gets the LIT line on all four sides — not the
            // button's lit/shaded pair. Buttons are small and want to look
            // pressable; a window is a large face, and a dark line along its
            // bottom and right reads as a second frame rather than as depth.
            // Even all round, it is a highlight just inside the frame: the
            // panel gets an edge instead of a direction.
            // ★One AddRect, not four AddLines — the corners then follow the
            // radius instead of leaving four gaps. ImGui strikes the path
            // half a pixel inside the rect it is given, so integer coords put
            // this line on a pixel centre (split across two, a 1px bevel goes
            // grey and vague). One stroke in from the frame, which straddles
            // wp..we for the reason above.
            const float b  = Theme::BorderPx();
            const float r  = Theme::WinRounding();
            dl->AddRect(ImVec2(wp.x + b, wp.y + b), ImVec2(we.x - b, we.y - b),
                        Theme::BevelLit(true), (r > b) ? r - b : 0.0f, 0, 1.0f);
        }
        dl->PopClipRect();
    }

    void WinManager::TitleBar(const std::string& a_key, const char* a_label, float a_reserveRight,
                              bool a_centerTitle)
    {
        auto& w = Ensure(a_key);
        // ★What ApplyNext already charged this window's height for. Reading it
        // rather than taking it again is the whole point: one argument, one
        // number, no way to pay for one and spend the other.
        const float a_topPad = w.topPad;
        w.pos = ImGui::GetWindowPos();
        w.size = ImGui::GetWindowSize();
        w.lastSeen = ImGui::GetFrameCount();

        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const float barH = TitleBarH();

        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = w.pos;
        const ImVec2 we(wp.x + w.size.x, wp.y + w.size.y);
        PaintGround(dl, wp, we, a_key, a_topPad, barH);
        dl->PushClipRect(ImVec2(wp.x - Theme::kTornOut * S,
                                wp.y - Theme::kTornOut * S),
                         ImVec2(we.x + Theme::kTornOut * S,
                                we.y + Theme::kTornOut * S), false);
        // (no title rule on a light panel — same reason as the frame above)
        if (!sk.lightPanel) {
            const float ly = wp.y + Theme::FrameInsetY() + a_topPad + barH;
            dl->AddLine(ImVec2(wp.x + Theme::FrameInsetX(), ly),
                ImVec2(we.x - Theme::FrameInsetX(), ly),
                Theme::Acc(sk.cornerFade ? 0.10f : 0.25f));
        }

        // title: tracked uppercase
        ImFont* font = ImGui::GetFont();
        // whole pixels only — a fractional size bakes its own face (rule 102)
        // ★DESIGN UNITS, applied by the helper — passing titleSize*S asked
        // for 24·S², which at 4K is a 54px name in a 51px bar.
        // ★★Theme::FontTitle, not SnapPx: the title is the one string the
        // text-size setting leaves alone, because TitleBarH below it is
        // layout and does not grow. The reason lives with the helper.
        const float fontSize = Theme::FontTitle();
        const float spacing = sk.titleSpacing * S;
        // tornFrame: nudge the title in so it clears the ragged frame edge
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float textW = TrackedTextWidth(font, fontSize, a_label, spacing);
        // ★HALF the inset. FrameInsetY is how far the FRAME eats in, and the
        // title was starting below all of it — on a torn skin that is 24px, so
        // the name sat visibly low in its own bar while the bar's lower half
        // stayed empty. The title belongs to the bar, not under the frame.
        // ★a_topPad is added WHOLE, not halved like the inset. The inset is how
        // far the frame eats in (the title belongs to the bar, so it only
        // clears half of it); this is clearance the caller has already paid for
        // in its window height, so every pixel of it goes above the name.
        const float ty = TitleTextY(a_key, fontSize);
        float tx = a_centerTitle ? wp.x + (w.size.x - textW) * 0.5f
                                 : wp.x + 12.0f * S + insX;
        // ★★INK: a brush mark laid UNDER the name, in the accent at half
        // strength. The reference marks its title that way and nothing else on
        // the window is coloured, so this one stroke is where the eye starts.
        // ★Sized from the TEXT, not from the bar: a mark that ran the bar's
        // width would be a highlight bar, and the thing it is imitating is a
        // stroke someone drew across a word.
        if (Theme::InkChrome() && textW > 1.0f) {
            // ★Sized from the TEXT and clamped to the window. The plate is a
            // mark behind a word, not a bar across the bar.
            const float padL = (std::min)(fontSize * 0.42f, tx - wp.x - 2.0f);
            const float x0 = tx - (std::max)(0.0f, padL);
            const float x1 = (std::min)(tx + textW + fontSize * 0.42f,
                                        wp.x + w.size.x - 4.0f);
            const float mh = fontSize * 1.62f;
            const float my = ty + fontSize * 0.50f - mh * 0.5f;
            if (x1 - x0 > 4.0f) {
                Theme::InkTitleMark(dl, ImVec2(x0, my), ImVec2(x1, my + mh),
                                    Theme::Col(sk.sel, 0.60f));
            }
        }
        // ★★★THE TITLE STOPS AT THE FRAME. The clip in force here is the
        // window plus kTornOut on every side -- deliberately, so a ragged skin
        // can bleed its edges outward -- and the title was inheriting it. But
        // a bag window is sized from its COLUMN COUNT, with no thought for how
        // long the name is: a 2x2 pouch is about 110px and "Ancient Nord
        // Burial Urn" is not. The name ran past the frame, over the bleed
        // allowance, and on a docked bag over the neighbouring window, ending
        // mid-letter wherever the bleed ran out. The ink title plate beside it
        // has always clamped to the frame; the text simply never did.
        dl->PushClipRect(ImVec2(wp.x + insX, wp.y),
                         ImVec2(we.x - insX, we.y), true);
        if (sk.titleGlow) {
            // poor-man's bloom: 4 offset passes under the main text.
            // ★1.0.5: the right-fading 1px UNDERLINE that used to follow is
            // gone, on every window and both skins that carry this flag
            // (Fable Crimson and Parchment Crimson — nothing else sets it).
            // The glow stays; only the rule under the name went.
            const ImU32 glow = Theme::Col(sk.hi, 0.12f);
            const float o = 1.0f;
            DrawTrackedText(dl, font, fontSize, ImVec2(tx - o, ty), glow, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx + o, ty), glow, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty - o), glow, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty + o), glow, a_label, spacing);
        }
        // ★The title draws its OWN outline instead of leaving it to the
        // draw-data pass. That pass has to recognise a glyph from finished
        // vertex data, and it does not reach this text — the title is the one
        // string on screen that MUST be outlined (white ink on a pale panel),
        // so it says so itself rather than depending on a heuristic.
        // ★InkNeedsOutline(), not lightPanel. A pale skin whose ink is DARK
        // (parchment) got a black edge on near-black letters and the title
        // came out as one solid lump — colour, edge and shadow all landing in
        // the same value.
        if (Theme::InkNeedsOutline()) {
            const ImU32 sh = IM_COL32(0, 0, 0, 255);   // same edge as every label
            const float o = 1.0f;                      // see Theme::TextOutlined
            DrawTrackedText(dl, font, fontSize, ImVec2(tx - o, ty), sh, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx + o, ty), sh, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty - o), sh, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty + o), sh, a_label, spacing);
        }
        DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty),
            sk.titleGlow ? Theme::Col(sk.hi, 1.0f) : Theme::TitleInk(), a_label, spacing);
        dl->PopClipRect();   // ★the title's own clamp

        dl->PopClipRect();   // back to the window's normal clip

        ImGui::SetCursorScreenPos(wp);
        const float stripW = (std::max)(40.0f, w.size.x - a_reserveRight);
        // the drag strip covers the pad too — it is title-bar space, and a
        // 14px dead band along the window's top edge would be the one place the
        // window cannot be picked up by
        ImGui::InvisibleButton(("##titlebar_" + a_key).c_str(),
                               ImVec2(stripW, insY + a_topPad + barH));
        if (ImGui::IsItemActivated() && !m_dragLock && !m_drag.active) {
            StartDrag(a_key);
        }

        // Content begins under the strip (inset in for tornFrame skins).
        // ★★THIS is the left margin — not WindowPadding. Every managed window
        // has its cursor placed here after the title bar, which overrides
        // whatever padding the style or the window itself asked for. Changing
        // WindowPadding narrowed the right edge (the width is computed from it)
        // while the left stayed at a hard-coded 12, so the content drifted
        // off-centre instead of tightening.
        ImGui::SetCursorScreenPos(
            ImVec2(wp.x + Theme::PadX() * S + insX, wp.y + insY + a_topPad + barH + 8.0f * S));
    }

    // ---- drag machinery (JS startWinDrag / mousemove / mouseup) ----

    std::vector<std::string> WinManager::SubtreeOf(const std::string& a_key, bool a_openOnly) const
    {
        // every window whose parent chain reaches a_key (open ones only for a
        // drag; a resize passes false so closed docked windows travel too)
        std::vector<std::string> out;
        for (const auto& w : m_wins) {
            if (w.key == a_key || w.key == "main" || (a_openOnly && !IsOpen(w))) continue;
            std::string cur = w.parent;
            int hops = 0;
            while (!cur.empty() && hops++ < 32) {
                if (cur == a_key) { out.push_back(w.key); break; }
                if (cur == "main") break;
                const Win* p = nullptr;
                for (const auto& c : m_wins) {
                    if (c.key == cur) { p = &c; break; }
                }
                cur = p ? p->parent : std::string();
            }
        }
        return out;
    }

    void WinManager::StartDrag(const std::string& a_key)
    {
        auto& w = Ensure(a_key);
        m_drag = {};
        m_drag.active = true;
        m_drag.key = a_key;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        m_drag.grab = ImVec2(mouse.x - w.pos.x, mouse.y - w.pos.y);

        // the dragged window carries its whole subtree
        m_drag.extMin = ImVec2(0.0f, 0.0f);
        m_drag.extMax = w.size;
        for (const auto& k : SubtreeOf(a_key)) {
            if (auto* f = Find(k)) {
                const ImVec2 off(f->pos.x - w.pos.x, f->pos.y - w.pos.y);
                m_drag.followers.push_back({ k, off });
                m_drag.extMin.x = (std::min)(m_drag.extMin.x, off.x);
                m_drag.extMin.y = (std::min)(m_drag.extMin.y, off.y);
                m_drag.extMax.x = (std::max)(m_drag.extMax.x, off.x + f->size.x);
                m_drag.extMax.y = (std::max)(m_drag.extMax.y, off.y + f->size.y);
            }
        }
    }

    float WinManager::ContactLen(ImVec2 a_min, ImVec2 a_max, ImVec2 b_min, ImVec2 b_max)
    {
        constexpr float eps = 1.5f;
        const float vo = (std::min)(a_max.y, b_max.y) - (std::max)(a_min.y, b_min.y);
        const float ho = (std::min)(a_max.x, b_max.x) - (std::max)(a_min.x, b_min.x);
        float len = 0.0f;
        if ((std::fabs(a_max.x - b_min.x) <= eps || std::fabs(a_min.x - b_max.x) <= eps) && vo > eps)
            len = (std::max)(len, vo);
        if ((std::fabs(a_max.y - b_min.y) <= eps || std::fabs(a_min.y - b_max.y) <= eps) && ho > eps)
            len = (std::max)(len, ho);
        return len;
    }

    ImVec2 WinManager::Magnetize(ImVec2 a_pos, ImVec2 a_size,
                                 const std::vector<std::string>& a_excluded) const
    {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        float x = a_pos.x, y = a_pos.y;
        const float w = a_size.x, h = a_size.y;

        // NEAREST candidate wins on each axis (last-wins let a window 10px away
        // steal the snap from the one being docked against)
        struct Best { float d = kMagnet; float v = 0.0f; bool has = false; };
        Best bx, by;
        auto consider = [](Best& best, float cur, float snapped) {
            const float dist = std::fabs(cur - snapped);
            if (dist < best.d) { best.d = dist; best.v = snapped; best.has = true; }
        };
        consider(bx, x, 0.0f);
        consider(bx, x, disp.x - w);
        consider(by, y, 0.0f);
        consider(by, y, disp.y - h);

        for (const auto& t : m_wins) {
            if (!t.posKnown || !IsOpen(t)) continue;
            bool skip = t.key == m_drag.key;
            for (const auto& ex : a_excluded) {
                if (t.key == ex) { skip = true; break; }
            }
            if (skip) continue;

            const float tl = t.pos.x, tt = t.pos.y;
            const float tr = t.pos.x + t.size.x, tb = t.pos.y + t.size.y;
            // x snaps only while vertically overlapping the target (and vice
            // versa), so docked windows slide along each other without letting go
            const bool vOverlap = y < tb + kMagnet && y + h > tt - kMagnet;
            const bool hOverlap = x < tr + kMagnet && x + w > tl - kMagnet;
            // ★Docking OVERLAPS by one frame stroke instead of butting up
            // against the neighbour. Edge-to-edge leaves two frames side by
            // side and the join reads twice as thick; overlapped, they land on
            // the same pixels and (being opaque) come out as one line.
            // ov is 0 for every unframed skin, so those still dock flush.
            const float ov = Theme::BorderOverlap();
            if (vOverlap) {
                consider(bx, x, tr - ov);        // my left | its right
                consider(bx, x, tl - w + ov);    // my right | its left
                consider(bx, x, tl);             // left edges align
                consider(bx, x, tr - w);         // right edges align
            }
            if (hOverlap) {
                consider(by, y, tb - ov);
                consider(by, y, tt - h + ov);
                consider(by, y, tt);
                consider(by, y, tb - h);
            }
        }
        if (bx.has) x = bx.v;
        if (by.has) y = by.v;
        return ImVec2(x, y);
    }

    bool WinManager::BeginConfirmPopup(const std::string& a_key, const char* a_imguiId,
                                       const char* a_title, ImVec2 a_size)
    {
        // ★The pad is added HERE rather than at each call site: every popup goes
        // through this one door, and a caller that sized its own body has no
        // business also knowing about the title's margin.
        const float topPad = Theme::TitleTopPad();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        // ★★Open AT THE CURSOR, not in the middle of the screen. A confirm
        // popup is answered and dismissed immediately, so the trip from the
        // cursor to its buttons is the entire cost of using one -- and on a
        // wide display, going to the centre and back is most of that cost.
        //
        // ★Only on the frame it OPENS. ApplyNext runs every frame the popup is
        // up, so feeding it the live cursor would drag the window around behind
        // the mouse. IsOpen answers "was this drawn in the last couple of
        // frames", which is exactly "is this a re-open".
        //
        // ★posKnown is cleared as well: ApplyNext only takes the default
        // position the first time it sees a key, and without this the popup
        // would land at the cursor once and then return to that same spot
        // forever after. A popup has no position worth remembering.
        auto*      w    = Find(a_key);
        const bool open = w && IsOpen(*w);
        ImVec2     def(( disp.x - a_size.x) * 0.5f, (disp.y - a_size.y) * 0.5f);
        if (!open) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            // ★Cursor lands 70px into the popup, horizontally centred -- below
            // the title bar (34 * scale), so the pointer is never on the drag
            // handle: the click that opened the popup is still held, and a
            // cursor on the bar would start moving the window with the mouse.
            // ★Scaled, like every other measurement here. A fixed 70 would
            // drift into the bar as the UI scale goes up.
            def = ImVec2(m.x - a_size.x * 0.5f, m.y - 70.0f * Theme::Scale());
            // ★The WHOLE popup has to be on screen -- ApplyNext only keeps the
            // title bar reachable, which is right for a window the player parks
            // but would cut the buttons off a popup opened near an edge.
            if (disp.x > a_size.x) {
                def.x = (std::max)(0.0f, (std::min)(disp.x - a_size.x, def.x));
            }
            if (disp.y > a_size.y) {
                def.y = (std::max)(0.0f, (std::min)(disp.y - a_size.y, def.y));
            }
            if (w) w->posKnown = false;
        }
        ApplyNext(a_key, def, a_size, Anchor::kTopLeft, topPad);
        ImGui::Begin(a_imguiId, nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        TitleBar(a_key, a_title, 0.0f, true);
        // outside click cancels (settings pattern) — never on the opening
        // frame: a popup first drawn on the SAME frame as the opening click
        // would read that click as "outside" and instantly close
        return !ImGui::IsWindowAppearing() &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
               !ImGui::IsWindowHovered();
    }

    void WinManager::Update()
    {
        if (!m_drag.active) return;

        auto* w = Find(m_drag.key);
        if (!w) { m_drag.active = false; return; }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            ImVec2 p(mouse.x - m_drag.grab.x, mouse.y - m_drag.grab.y);

            std::vector<std::string> excl;
            excl.reserve(m_drag.followers.size());
            for (const auto& f : m_drag.followers) excl.push_back(f.key);
            p = Magnetize(p, w->size, excl);

            // whole-group containment (left/top win if the group is larger
            // than the screen)
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            p.x = (std::max)(-m_drag.extMin.x, (std::min)(disp.x - m_drag.extMax.x, p.x));
            p.y = (std::max)(-m_drag.extMin.y, (std::min)(disp.y - m_drag.extMax.y, p.y));

            w->pos = p;
            for (const auto& f : m_drag.followers) {
                if (auto* fw = Find(f.key)) {
                    fw->pos = ImVec2(p.x + f.off.x, p.y + f.off.y);
                }
            }
        } else {
            EndDrag();
        }
    }

    void WinManager::EndDrag()
    {
        auto* w = Find(m_drag.key);
        if (!w) { m_drag.active = false; return; }

        const ImVec2 rMin = w->pos;
        const ImVec2 rMax(w->pos.x + w->size.x, w->pos.y + w->size.y);
        const ImVec2 c0(rMin.x + w->size.x * 0.5f, rMin.y + w->size.y * 0.5f);

        auto isMine = [&](const std::string& key) {
            for (const auto& f : m_drag.followers) {
                if (f.key == key) return true;
            }
            return false;
        };

        if (m_drag.key != "main") {
            // released snapped onto a window -> that window adopts it; released
            // in the open -> the link breaks. Own subtree can't adopt (no cycles).
            // Among everything we ended flush against, adopt the window whose
            // CENTER is nearest (contact length ties constantly).
            std::string parent;
            float best = FLT_MAX;
            for (const auto& t : m_wins) {
                if (t.key == m_drag.key || !t.posKnown || !IsOpen(t) || isMine(t.key)) continue;
                const ImVec2 tMax(t.pos.x + t.size.x, t.pos.y + t.size.y);
                if (ContactLen(rMin, rMax, t.pos, tMax) <= 0.0f) continue;
                const float d = std::hypot(t.pos.x + t.size.x * 0.5f - c0.x,
                                           t.pos.y + t.size.y * 0.5f - c0.y);
                if (d < best) { best = d; parent = t.key; }
            }
            w->parent = parent;   // "" clears the link
        } else {
            // MAIN snapped onto a bag: main can never be a child, so the intent
            // inverts — that bag's whole tree becomes main's (and rides with it)
            std::string target;
            float best = FLT_MAX;
            for (const auto& t : m_wins) {
                if (t.key == "main" || !t.posKnown || !IsOpen(t) || isMine(t.key)) continue;
                const ImVec2 tMax(t.pos.x + t.size.x, t.pos.y + t.size.y);
                if (ContactLen(rMin, rMax, t.pos, tMax) <= 0.0f) continue;
                const float d = std::hypot(t.pos.x + t.size.x * 0.5f - c0.x,
                                           t.pos.y + t.size.y * 0.5f - c0.y);
                if (d < best) { best = d; target = t.key; }
            }
            if (!target.empty()) {
                std::string root = target;
                int hops = 0;
                while (hops++ < 32) {
                    auto* r = Find(root);
                    if (!r || r->parent.empty() || r->parent == "main") break;
                    root = r->parent;
                }
                if (auto* r = Find(root)) r->parent = "main";
            }
        }

        m_drag.active = false;
        Save();
    }
}
