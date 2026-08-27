#include "game/BagFilter.h"

#include "ui/Lang.h"

#include <fstream>

namespace FUI::BagFilter
{
    namespace
    {
        constexpr const char* kIniPath = "Data/SKSE/Plugins/GridInventory_bagfilters.ini";

        // PLAN_TYPED_BAGS.md §1, PLAN_BOOK_BAG.md. Five of the seven resolve off
        // the record TYPE via the category (book joined as the seventh -- all
        // five of its categories are record-type driven), so they are exact for
        // any mod's content. Only ore and hide lean on keywords — merchants
        // need those keywords too, so mod authors do attach them, but that is
        // the half that can miss.
        // `vendor:` names the keyword that identifies WHO SELLS this bag —
        // ★the merchant who trades the things the bag holds. Deriving the shop
        // from the filter means no merchant table to maintain and mod-added
        // shops are covered for free (their vendorSellBuyList carries the same
        // vanilla keywords, because merchants need them to trade at all).
        constexpr const char* kDefaults = R"(
alchemy = cat:ingredient, vendor:VendorItemIngredient
soulgem = cat:soulgem, vendor:VendorItemSoulGem
potion  = cat:potion, cat:poison, vendor:VendorItemPotion
key     = cat:key, kwd:VendorItemKey, frm:Skyrim.esm|0x00000A, vendor:VendorItemClutter
ore     = cat:misc_ore, cat:misc_ingot, vendor:VendorItemOreIngot
hide    = cat:misc_hide, kwd:VendorItemAnimalPart, vendor:VendorItemAnimalHide
book    = cat:book, cat:book_note, cat:book_skill, cat:book_spell, cat:scroll, vendor:VendorItemClutter
)";
        // ★The ore pouch names BOTH ore categories. Splitting misc_ingot out of
        // misc_ore silently narrowed this rule — the pouch kept its name and
        // its icon and simply stopped taking ingots. Any future category split
        // has to be walked past this table for the same reason.
        // ★kwd:VendorItemKey is not redundant with cat:key. Some vanilla keys
        // are MISC records, not KEYM — the two Skull Keys, found by the phase-0
        // sweep — so a rule that reads the record type alone misses them.
        // ★frm: names ONE form outright — the lockpick (user request: it
        // belongs in the key pouch) has no category and no keyword of its own,
        // so neither of the other two term kinds can reach it.

        struct Rule
        {
            std::string              id;
            std::vector<std::string> cats;
            // keywords resolved to forms ONCE; HasKeywordString() re-walks a
            // string table on every call and this runs over whole inventories
            std::vector<RE::BGSKeyword*> kwds;
            std::vector<std::string>     kwdNames;   // kept for the log
            std::vector<RE::FormID>      forms;      // frm: exact forms (runtime IDs)
            RE::BGSKeyword*              vendor = nullptr;   // who stocks this bag
        };

        std::vector<Rule> g_rules;
        bool              g_loaded = false;
        const std::string kNone;

        std::function<std::string(RE::TESBoundObject*)> g_categoryOf;

        // FormID -> filter id. Cleared never: a form's classification cannot
        // change within a session.
        std::unordered_map<RE::FormID, std::string> g_cache;

        [[nodiscard]] std::string Trim(std::string_view a_s)
        {
            const auto b = a_s.find_first_not_of(" \t\r\n");
            if (b == std::string_view::npos) return {};
            const auto e = a_s.find_last_not_of(" \t\r\n");
            return std::string(a_s.substr(b, e - b + 1));
        }

        void ParseInto(std::istream& a_in, const char* a_where)
        {
            std::string line;
            while (std::getline(a_in, line)) {
                const auto t = Trim(line);
                if (t.empty() || t[0] == ';' || t[0] == '#') continue;
                const auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                Rule r;
                r.id = Trim(std::string_view(t).substr(0, eq));
                if (r.id.empty()) continue;

                std::string_view rest(t.c_str() + eq + 1);
                size_t p = 0;
                while (p <= rest.size()) {
                    const auto c = rest.find(',', p);
                    const auto tok = Trim(rest.substr(p, c == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : c - p));
                    if (tok.rfind("cat:", 0) == 0) {
                        r.cats.push_back(tok.substr(4));
                    } else if (tok.rfind("kwd:", 0) == 0) {
                        const auto name = tok.substr(4);
                        auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(name);
                        r.kwdNames.push_back(name);
                        if (kw) {
                            r.kwds.push_back(kw);
                        } else {
                            // ★Say it out loud. A mistyped keyword otherwise
                            // just means "this bag quietly takes less", which
                            // reads as a routing bug months later. (Vanilla
                            // itself ships VendorItemFireword, not -wood.)
                            SKSE::log::warn("[BAGFILTER] {}: keyword '{}' not found - rule ignored",
                                r.id, name);
                        }
                    } else if (tok.rfind("frm:", 0) == 0) {
                        // frm:Plugin.esp|0xID — one exact form. For things a
                        // category or keyword cannot single out (the lockpick).
                        const auto spec = tok.substr(4);
                        const auto bar = spec.find('|');
                        RE::TESBoundObject* obj = nullptr;
                        if (bar != std::string::npos) {
                            const auto plugin = Trim(std::string_view(spec).substr(0, bar));
                            RE::FormID id = 0;
                            try {
                                id = static_cast<RE::FormID>(
                                    std::stoul(Trim(std::string_view(spec).substr(bar + 1)),
                                               nullptr, 16));
                            } catch (...) {}
                            if (id != 0) {
                                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                                    // ★NOT LookupForm<TESBoundObject>: the
                                    // template form checks Is(T::FORMTYPE),
                                    // and TESBoundObject is an abstract base
                                    // with no form type of its own — so it
                                    // matched NOTHING and the lockpick term
                                    // was dropped at load with only a warning
                                    // to show for it. Look the form up
                                    // untyped, then cast.
                                    if (auto* form = dh->LookupForm(id, plugin)) {
                                        obj = form->As<RE::TESBoundObject>();
                                    }
                                }
                            }
                        }
                        if (obj) {
                            r.forms.push_back(obj->GetFormID());
                            SKSE::log::info("[BAGFILTER] {}: form '{}' -> {:08X} '{}'",
                                r.id, spec, obj->GetFormID(),
                                obj->GetName() ? obj->GetName() : "?");
                        } else {
                            SKSE::log::warn("[BAGFILTER] {}: form '{}' not found - term ignored",
                                r.id, spec);
                        }
                    } else if (tok.rfind("vendor:", 0) == 0) {
                        const auto name = tok.substr(7);
                        r.vendor = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(name);
                        if (!r.vendor) {
                            SKSE::log::warn("[BAGFILTER] {}: vendor keyword '{}' not found"
                                            " - no merchant will stock this bag", r.id, name);
                        }
                    } else if (!tok.empty()) {
                        SKSE::log::warn("[BAGFILTER] {}: unknown term '{}'", r.id, tok);
                    }
                    if (c == std::string_view::npos) break;
                    p = c + 1;
                }
                if (r.cats.empty() && r.kwds.empty() && r.forms.empty()) {
                    SKSE::log::warn("[BAGFILTER] {}: no usable terms - skipped", r.id);
                    continue;
                }
                SKSE::log::info("[BAGFILTER] {:<8} {} category rule(s), {} keyword(s), {} form(s)  [{}]",
                    r.id, r.cats.size(), r.kwds.size(), r.forms.size(), a_where);
                g_rules.push_back(std::move(r));
            }
        }
    }

    void SetCategoryResolver(std::function<std::string(RE::TESBoundObject*)> a_fn)
    {
        g_categoryOf = std::move(a_fn);
    }

    void Load()
    {
        if (g_loaded) return;
        g_loaded = true;
        if (std::ifstream in(kIniPath); in) {
            ParseInto(in, "ini");
        }
        if (g_rules.empty()) {
            std::istringstream in{ std::string(kDefaults) };
            ParseInto(in, "built-in");
        }
        SKSE::log::info("[BAGFILTER] {} filter(s) active", g_rules.size());
    }

    const std::string& FilterOf(RE::TESBoundObject* a_obj)
    {
        if (!a_obj || g_rules.empty()) return kNone;
        if (const auto it = g_cache.find(a_obj->GetFormID()); it != g_cache.end()) {
            return it->second;
        }

        const std::string cat = g_categoryOf ? g_categoryOf(a_obj) : std::string{};
        // BGSKeywordForm is a base, not a form type, so As<> cannot reach it;
        // skyrim_cast walks the real RTTI (the pattern already used in Fallback)
        const auto* kwf = skyrim_cast<RE::BGSKeywordForm*>(a_obj);

        const std::string* hit = &kNone;
        for (const auto& r : g_rules) {
            bool match = std::find(r.cats.begin(), r.cats.end(), cat) != r.cats.end();
            if (!match && !r.forms.empty()) {
                match = std::find(r.forms.begin(), r.forms.end(),
                                  a_obj->GetFormID()) != r.forms.end();
            }
            if (!match && kwf) {
                for (auto* kw : r.kwds) {
                    if (kwf->HasKeyword(kw)) { match = true; break; }
                }
            }
            if (match) { hit = &r.id; break; }
        }
        return g_cache.emplace(a_obj->GetFormID(), *hit).first->second;
    }

    const char* DisplayName(const std::string& a_filter)
    {
        // ★What the player reads, not the internal id. The tooltips printed the
        // id straight ("Holds ore only"), which in Korean came out as
        // "ore 분류만..." — an English token wedged into a translated sentence.
        // A filter someone adds in the ini has no string of its own and still
        // falls back to its id, which is the best that can be said about a name
        // we have never seen.
        struct Row { const char* id; Lang::Str key; };
        static constexpr Row kRows[] = {
            { "alchemy", Lang::Str::FilterAlchemy },
            { "ore",     Lang::Str::FilterOre },
            { "hide",    Lang::Str::FilterHide },
            { "potion",  Lang::Str::FilterPotion },
            { "soulgem", Lang::Str::FilterSoulGem },
            { "key",     Lang::Str::FilterKey },
        };
        for (const auto& r : kRows) {
            if (a_filter == r.id) return Lang::T(r.key);
        }
        return a_filter.c_str();
    }

    RE::BGSKeyword* VendorKeyword(const std::string& a_filter)
    {
        for (const auto& r : g_rules) {
            if (r.id == a_filter) return r.vendor;
        }
        return nullptr;
    }

    int Count() { return static_cast<int>(g_rules.size()); }

    const char* Id(int a_index)
    {
        return (a_index >= 0 && a_index < Count())
                   ? g_rules[static_cast<size_t>(a_index)].id.c_str()
                   : "";
    }

    void DumpFormDatabase()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        Load();

        // ★The inventory dump only ever sees what the player happens to be
        // carrying, which left 2 of 6 filters with no sample at all on the
        // first run. Sweeping the whole load order answers the question the
        // inventory cannot: does a mod's ore item match the rule, whether or
        // not anyone owns one yet.
        std::map<std::string, int> tally;
        std::vector<std::string>   suspects;
        std::vector<std::string>   byName;
        int total = 0;

        // ★A keyword probe cannot see the WORST case: an item with no keywords
        // at all. A mod that adds "Mithril Ore" as a bare MISC lands in (none)
        // and the check above stays silent, which reads as "the rules cover
        // everything". Reading the name catches those. False positives are
        // fine here — this list is for a human to look at once, not for the
        // routing to act on.
        struct NameProbe { const char* filter; const char* needle; };
        static constexpr NameProbe kNameProbes[] = {
            { "ore", "Ore" }, { "ore", "Ingot" },
            { "hide", "Hide" }, { "hide", "Pelt" }, { "hide", "Leather" },
            { "hide", "Fur" }, { "hide", "Tusk" }, { "hide", "Scales" },
            { "soulgem", "Soul Gem" },
            { "key", "Key" },
        };

        auto sweep = [&](auto& a_arr) {
            for (auto* f : a_arr) {
                auto* obj = f ? f->template As<RE::TESBoundObject>() : nullptr;
                if (!obj) continue;
                // playable content only; the arrays are full of template and
                // internal forms that nobody can ever hold
                const char* nm = obj->GetName();
                if (!nm || !*nm) continue;
                ++total;
                const auto& fl = FilterOf(obj);
                ++tally[fl.empty() ? "(none)" : fl];
                if (!fl.empty()) continue;

                const std::string_view name(nm);
                for (const auto& np : kNameProbes) {
                    if (name.find(np.needle) == std::string_view::npos) continue;
                    if (byName.size() < 60) {
                        byName.push_back(fmt::format("{} ({:08X}) reads like '{}'",
                            nm, obj->GetFormID(), np.filter));
                    }
                    break;
                }

                const auto* kwf = skyrim_cast<RE::BGSKeywordForm*>(obj);
                if (!kwf) continue;
                for (const char* probe : { "VendorItemOreIngot", "VendorItemAnimalHide",
                                           "VendorItemAnimalPart", "VendorItemIngredient",
                                           "VendorItemSoulGem", "VendorItemPotion",
                                           "VendorItemPoison", "VendorItemKey" }) {
                    if (!kwf->HasKeywordString(probe)) continue;
                    if (suspects.size() < 60) {
                        suspects.push_back(fmt::format("{} ({:08X}) has {}",
                            nm, obj->GetFormID(), probe));
                    }
                    break;
                }
            }
        };

        sweep(dh->GetFormArray<RE::TESObjectMISC>());
        sweep(dh->GetFormArray<RE::IngredientItem>());
        sweep(dh->GetFormArray<RE::TESSoulGem>());
        sweep(dh->GetFormArray<RE::AlchemyItem>());
        sweep(dh->GetFormArray<RE::TESKey>());

        SKSE::log::info("[BAGFILTER] ==== load-order sweep ({} named forms) ====", total);
        for (const auto& [id, n] : tally) {
            SKSE::log::info("[BAGFILTER]   {:<8} {:5d} forms", id, n);
        }
        if (suspects.empty()) {
            SKSE::log::info("[BAGFILTER]   nothing unclaimed carries a vendor keyword");
        } else {
            SKSE::log::warn("[BAGFILTER]   {} unclaimed form(s) with a vendor keyword"
                            " (first {}):", suspects.size(), suspects.size());
            for (const auto& s : suspects) SKSE::log::warn("[BAGFILTER]     {}", s);
        }
        if (byName.empty()) {
            SKSE::log::info("[BAGFILTER]   nothing unclaimed reads like a bag item");
        } else {
            SKSE::log::warn("[BAGFILTER]   {} unclaimed form(s) whose NAME suggests a bag"
                            " (keyword-less misses look like this):", byName.size());
            for (const auto& s : byName) SKSE::log::warn("[BAGFILTER]     {}", s);
        }
        SKSE::log::info("[BAGFILTER] =========================================");
    }

    void DumpPlayerInventory()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        Load();

        std::map<std::string, std::pair<int, int>> tally;   // filter -> {kinds, units}
        // Items that landed in NO filter but look like they should have. This
        // is the actual product of phase 0 — a rule that is silently too narrow
        // costs nothing until the bags exist, and everything afterwards.
        std::vector<std::string> suspects;

        auto inv = p->GetInventory();
        for (auto& [obj, data] : inv) {
            if (!obj) continue;
            const int count = data.first;
            if (count <= 0) continue;
            const auto& f = FilterOf(obj);
            auto& t = tally[f.empty() ? "(none)" : f];
            t.first += 1;
            t.second += count;

            if (!f.empty()) continue;
            const auto* kwf = skyrim_cast<RE::BGSKeywordForm*>(obj);
            if (!kwf) continue;
            for (const char* probe : { "VendorItemOreIngot", "VendorItemAnimalHide",
                                       "VendorItemAnimalPart", "VendorItemIngredient",
                                       "VendorItemSoulGem", "VendorItemPotion",
                                       "VendorItemPoison", "VendorItemKey" }) {
                if (!kwf->HasKeywordString(probe)) continue;
                suspects.push_back(fmt::format("{} ({:08X}) has {} but no filter matched",
                    obj->GetName() ? obj->GetName() : "?", obj->GetFormID(), probe));
                break;
            }
        }

        SKSE::log::info("[BAGFILTER] ---- inventory classification ----");
        for (const auto& [id, t] : tally) {
            SKSE::log::info("[BAGFILTER]   {:<8} {:4d} kinds  {:5d} units", id, t.first, t.second);
        }
        if (suspects.empty()) {
            SKSE::log::info("[BAGFILTER]   no unclassified items carry a vendor keyword");
        } else {
            SKSE::log::warn("[BAGFILTER]   {} item(s) a rule may be missing:", suspects.size());
            for (const auto& s : suspects) SKSE::log::warn("[BAGFILTER]     {}", s);
        }
        SKSE::log::info("[BAGFILTER] ----------------------------------");
    }
}
