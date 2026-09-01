#include "game/WornLedger.h"

#include "ui/Grid.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

namespace FUI::WornLedger
{
    namespace
    {
        enum class State { pending, worn, doffing };

        struct Entry
        {
            RE::FormID    form = 0;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            int           hand = 0;    // 0 none / 1 right / 2 left
            int           units = 1;   // what the action moved (data only)
            State         state = State::pending;
            std::chrono::steady_clock::time_point when{};
        };
        std::vector<Entry> g_entries;
        bool               g_have = false;

        bool Tracked(RE::TESBoundObject* a_obj)
        {
            // The equip event also announces spells, shouts and scroll casts;
            // the ledger is about the things the BOARD accounts for.
            return a_obj && (a_obj->Is(RE::FormType::Armor) ||
                             a_obj->Is(RE::FormType::Weapon) ||
                             a_obj->Is(RE::FormType::Light) ||
                             a_obj->Is(RE::FormType::Ammo));
        }

        // ★★★AMMO IS POOLED, AND THE POOL IS NOT STABLE.
        //
        // Everything else here is one entry per worn LIST, and for everything
        // else that holds. Arrows do not: the engine merges them, so equipping
        // three tilefuls can leave one worn list of 200 or three of 99/49/52 --
        // both measured, in the same session. A ledger counting entries against
        // a list count that moves on its own can only ever be wrong.
        //
        // So an ammo form gets ONE entry, units summed, and the audit asks
        // whether the quiver is on the back rather than how many lists it took
        // to say so. The count itself is the engine's to keep; the board reads
        // it there (Equip.cpp sums the worn lists for the doll).
        [[nodiscard]] bool IsAmmo(RE::FormID a_form)
        {
            auto* f = RE::TESForm::LookupByID(a_form);
            return f && f->Is(RE::FormType::Ammo);
        }

        bool TrackedForm(RE::FormID a_form)
        {
            auto* form = RE::TESForm::LookupByID(a_form);
            return Tracked(form ? form->As<RE::TESBoundObject>() : nullptr);
        }

        // engine truth: worn LISTS per form, with the identity read off each
        // list so a rebaseline starts with real entries, not blanks
        std::vector<Entry> EngineWalk()
        {
            std::vector<Entry> out;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return out;   // 원칙 4
            for (auto& [obj, pair] : player->GetInventory()) {
                if (!Tracked(obj) || pair.first <= 0) continue;
                auto* entry = pair.second.get();
                if (!entry || !entry->extraLists) continue;
                for (auto* xl : *entry->extraLists) {
                    if (!xl) continue;
                    const bool wornL = xl->HasType<RE::ExtraWornLeft>();
                    if (!wornL && !xl->HasType<RE::ExtraWorn>()) continue;
                    Entry e;
                    e.form = obj->GetFormID();
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                        e.uid = xu->uniqueID;
                    }
                    e.sig   = Grid::InstanceSigOf(xl);
                    e.hand  = wornL ? 2 : 1;
                    e.units = (std::max)(1, static_cast<int>(xl->GetCount()));
                    e.state = State::worn;
                    out.push_back(std::move(e));
                }
            }
            return out;
        }

        const char* NameOf(RE::FormID a_form)
        {
            const auto* f = RE::TESForm::LookupByID(a_form);
            const char* n = f ? f->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        std::map<RE::FormID, int> CountByForm(const std::vector<Entry>& a_v,
                                              State a_state)
        {
            std::map<RE::FormID, int> out;
            for (const auto& e : a_v) {
                if (e.state == a_state) ++out[e.form];
            }
            return out;
        }

        const char* StateName(State a_s)
        {
            switch (a_s) {
            case State::pending:  return "pending";
            case State::doffing:  return "doffing";
            default:              return "worn";
            }
        }

        // ★★★NAME WHAT DRIFTED, not just how far.
        //
        // The audit reported "ledger 10 vs engine 1" and stopped there, and
        // four sessions of that produced no diagnosis at all: a count cannot
        // say WHICH ten, and the drift never survives the audit that finds it
        // (the bend below rewrites the books). ★The ledger has carried uid,
        // sig, hand and a timestamp since B4-2b for exactly this reason --
        // identity plus a lifecycle -- and the one place it mattered was
        // printing counts. So a mismatch now prints both sides in full.
        //
        // ★★AGE IS THE PART THAT ACCUSES. Entries that all arrived within a
        // second of each other are one burst -- a loadout apply, a costume
        // swap, a quiver going on. Entries spread over minutes are a slow
        // leak: worn entries never expire (only pending and doffing do), so a
        // single missed unequip event sits in the books until the next audit
        // bends them, and the shape "many ledger, one engine" is what a leak
        // that cannot self-heal looks like after a long session.
        void ReportForm(const char* a_when, RE::FormID a_form,
                        const std::vector<Entry>& a_engine)
        {
            const auto now = std::chrono::steady_clock::now();
            for (const auto& e : g_entries) {
                if (e.form != a_form) continue;
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - e.when).count();
                logger::warn("[WORN] @{}   ledger: {} uid {:04X} sig {:04X} "
                             "hand {} units {} age {}ms", a_when,
                             StateName(e.state), e.uid, e.sig, e.hand,
                             e.units, age);
            }
            for (const auto& e : a_engine) {
                if (e.form != a_form) continue;
                logger::warn("[WORN] @{}   engine: uid {:04X} sig {:04X} "
                             "hand {} units {}", a_when, e.uid, e.sig,
                             e.hand, e.units);
            }
        }
    }

    void NotePending(RE::FormID a_form, std::uint16_t a_uid, std::uint16_t a_sig,
                     int a_hand, int a_units)
    {
        if (!g_have || !TrackedForm(a_form)) return;
        // ★A second quiverful joins the one already spoken for -- the engine
        // will pool them and a second entry would have nothing to answer to.
        if (IsAmmo(a_form)) {
            for (auto& e : g_entries) {
                if (e.form != a_form) continue;
                e.units += (std::max)(1, a_units);
                e.when   = std::chrono::steady_clock::now();
                return;
            }
        }
        Entry e;
        e.form  = a_form;
        e.uid   = a_uid;
        e.sig   = a_sig;
        e.hand  = a_hand;
        e.units = (std::max)(1, a_units);
        e.state = State::pending;
        e.when  = std::chrono::steady_clock::now();
        g_entries.push_back(std::move(e));
    }

    void OnEquip(RE::FormID a_form)
    {
        if (!g_have || !TrackedForm(a_form)) return;
        // Our own request first -- the pending entry carries the identity the
        // event cannot (rule 2). The event names no hand either, so form is
        // the whole key; with two pendings of one form the oldest lands
        // first, which is also the order the engine ran them.
        for (auto& e : g_entries) {
            if (e.form == a_form && e.state == State::pending) {
                e.state = State::worn;
                e.when  = std::chrono::steady_clock::now();
                return;
            }
        }
        // ★An ammo form already accounted for stays ONE entry: the engine
        // pooled the arrivals, so a second event about the same quiver is the
        // same quiver, not another one.
        if (IsAmmo(a_form)) {
            for (auto& e : g_entries) {
                if (e.form != a_form) continue;
                e.state = State::worn;
                e.when  = std::chrono::steady_clock::now();
                return;
            }
        }
        // Nobody asked: the engine wore it on its own authority (a loadout
        // apply, a script, vanilla favourites). A real entry, identity
        // unknown -- exactly what rule 2 predicts an event can carry.
        Entry e;
        e.form  = a_form;
        e.state = State::worn;
        e.when  = std::chrono::steady_clock::now();
        g_entries.push_back(std::move(e));
    }

    void NoteDoffing(RE::FormID a_form, int a_hand)
    {
        if (!g_have || !TrackedForm(a_form)) return;
        // the HAND names the unit exactly while it is on the body (one item
        // per hand) -- prefer it, fall back to any worn entry of the form
        Entry* pick = nullptr;
        for (auto& e : g_entries) {
            if (e.form != a_form || e.state != State::worn) continue;
            if (a_hand != 0 && e.hand == a_hand) { pick = &e; break; }
            if (!pick) pick = &e;
        }
        if (!pick) {
            // lifting something the ledger never saw worn is the same class
            // of finding OnUnequip logs -- record a doffing entry anyway so
            // the retire below still has its counterpart
            logger::warn("[WORN] doffing of {:08X} '{}' the ledger never saw "
                         "worn", a_form, NameOf(a_form));
            Entry e;
            e.form  = a_form;
            e.hand  = a_hand;
            e.state = State::doffing;
            e.when  = std::chrono::steady_clock::now();
            g_entries.push_back(std::move(e));
            return;
        }
        pick->state = State::doffing;
        pick->when  = std::chrono::steady_clock::now();
    }

    bool Doffing(RE::FormID a_form)
    {
        for (const auto& e : g_entries) {
            if (e.form == a_form && e.state == State::doffing) return true;
        }
        return false;
    }

    void OnUnequip(RE::FormID a_form)
    {
        if (!g_have || !TrackedForm(a_form)) return;
        // ★A QUIVER COMES OFF WHOLE. The ammo unequip takes every worn list
        // in one action, and the engine may report that as one event or
        // several; either way what is left on the back is nothing, so the
        // form's single entry retires rather than being decremented by a
        // count nobody can pair up.
        if (IsAmmo(a_form)) {
            std::erase_if(g_entries, [&](const Entry& e) { return e.form == a_form; });
            return;
        }
        // ★Doffing entries retire FIRST: an unequip we asked for answers our
        // own request before it answers anything else -- the same rule the
        // container ledger runs on (a confirmation retires its own entry).
        for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
            if (it->form == a_form && it->state == State::doffing) {
                g_entries.erase(it);
                return;
            }
        }
        // Then one worn entry of the form. The event cannot name which; the
        // OLDEST goes, mirroring OnEquip's order so a same-form pair cycles
        // instead of starving one entry.
        for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
            if (it->form == a_form && it->state == State::worn) {
                g_entries.erase(it);
                return;
            }
        }
        // ...a pending that got unequipped before its equip event would be a
        // genuine finding; so is an unequip of something never seen worn.
        logger::warn("[WORN] unequip of {:08X} '{}' the ledger never saw worn",
                     a_form, NameOf(a_form));
    }

    void Rebaseline(const char* a_why)
    {
        g_entries = EngineWalk();
        g_have = true;
        logger::info("[WORN] rebaseline ({}) -- {} worn list(s)", a_why,
                     g_entries.size());
    }

    void Audit(const char* a_when)
    {
        if (!g_have) {
            Rebaseline(a_when);
            return;
        }
        const auto engine = EngineWalk();
        auto       eByForm = CountByForm(engine, State::worn);
        // ★doffing counts WITH worn here: the engine still wears a unit whose
        // unequip is in flight, so the audit must expect it on both sides
        std::map<RE::FormID, int> lByForm;
        for (const auto& e : g_entries) {
            if (e.state == State::worn || e.state == State::doffing) {
                ++lByForm[e.form];
            }
        }

        // ★PRESENCE, NOT COUNT, FOR AMMO. See IsAmmo: the engine's list
        // count for a quiver moves on its own, so comparing it to anything is
        // comparing to noise. Both sides collapse to "on the back or not",
        // which is the question that has an answer.
        for (auto& [f, n] : eByForm) if (IsAmmo(f) && n > 0) n = 1;
        for (auto& [f, n] : lByForm) if (IsAmmo(f) && n > 0) n = 1;

        int bad = 0;
        for (const auto& [f, n] : eByForm) {
            const auto it = lByForm.find(f);
            const int  mine = it == lByForm.end() ? 0 : it->second;
            if (mine != n) {
                ++bad;
                logger::warn("[WORN] @{} MISMATCH {:08X} '{}': ledger {} vs "
                             "engine {}", a_when, f, NameOf(f), mine, n);
                ReportForm(a_when, f, engine);
            }
        }
        for (const auto& [f, n] : lByForm) {
            if (!eByForm.contains(f)) {
                ++bad;
                logger::warn("[WORN] @{} MISMATCH {:08X} '{}': ledger {} vs "
                             "engine 0", a_when, f, NameOf(f), n);
                ReportForm(a_when, f, engine);
            }
        }

        // lifecycle residue: a pending that never met its event. In-flight
        // ones are legitimate for a few frames; an old one means an equip
        // request the engine silently refused (or an event we missed) and
        // must not sit in the books forever.
        const auto now = std::chrono::steady_clock::now();
        int stuck = 0;
        std::erase_if(g_entries, [&](const Entry& e) {
            if (e.state != State::pending && e.state != State::doffing) return false;
            if (now - e.when < std::chrono::seconds(2)) return false;
            ++stuck;
            // a stale pending is an equip the engine silently refused; a
            // stale doffing is an unequip that never landed -- either way the
            // request's window is long over and the entry must not sit in
            // the books (the engine-bend below re-counts the unit correctly)
            logger::warn("[WORN] @{} ★stale {} {:08X} '{}' (uid {:04X} "
                         "sig {:04X} hand {}) -- dropped", a_when,
                         e.state == State::pending ? "pending" : "doffing",
                         e.form, NameOf(e.form), e.uid, e.sig, e.hand);
            return true;
        });

        if (bad == 0 && stuck == 0) {
            logger::info("[WORN] @{} ok -- {} worn list(s) agree", a_when,
                         engine.size());
        }
        if (bad > 0) {
            // observation mode: the engine is still the authority. Bend, so
            // the next divergence is counted from a clean baseline instead of
            // echoing this one.
            g_entries = engine;
            logger::warn("[WORN] @{} ★{} mismatch(es) -- ledger bent to the "
                         "engine (observation mode)", a_when, bad);
        }
    }
}
