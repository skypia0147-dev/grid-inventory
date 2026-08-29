#include "ui/Sfx.h"
#include "PCH.h"

#include "api/HostApi.h"
#include "ui/Grid.h"
#include "ui/LootBarter.h"
#include "ui/UIRoot.h"

namespace FUI::HostApi
{
    namespace
    {
        // ---- host -> provider services -----------------------------------
        //
        // Plain C functions: the table crosses a DLL boundary, so nothing here
        // may capture, allocate, or hand back an STL type.

        void Svc_RequestRebuild()
        {
            Grid::RequestRebuild();   // sets a flag; the next frame re-collects
        }

        bool Svc_IsMenuOpen()
        {
            // ★★★NOT IsBoardLive, and the header two files over says so in as
            // many words: "the grid stays OPEN throughout ... and
            // IsMenuOpen("GridInventoryMenu") keeps answering true".
            //
            // Answering liveness here broke that promise, and the field found
            // it immediately. A client suppressed us, read this back, saw
            // false, concluded the menu it was living over had closed, and
            // shut its own window 17ms later -- five times out of five. The
            // grid had not closed at all; we told it that it had. A client
            // cannot be asked to distinguish "you are suppressed, by you"
            // from "the player closed the inventory" through one boolean that
            // says false in both cases.
            //
            // So this answers the engine's question -- is the session on the
            // stack -- which is the one the ABI documents and the one that
            // survives suppression.
            return UIRoot::IsSessionOpen();
        }

        // Grant-time tile snapshot. Counts only the TRUE cells of a polyomino
        // mask, under the definition resolved right now (per-form override >
        // model-shared > category preset), so it follows the user's live EDIT
        // changes. A provider that rolls persistent state from this must call
        // it AT the roll and freeze the answer -- see PLAN 2-B-1.
        std::uint32_t Svc_CellSpanOf(std::uint32_t a_base)
        {
            auto* form = RE::TESForm::LookupByID(a_base);
            if (!form) return 0;
            auto* obj = form->As<RE::TESBoundObject>();
            if (!obj) return 0;
            return static_cast<std::uint32_t>(Grid::CellSpanOf(obj));
        }

        // The host owns the container and barter windows, so it is the only
        // thing that knows whether the transfer in flight is loot or a purchase.
        std::uint32_t Svc_PartnerKind()
        {
            using M = LootBarter::Mode;
            switch (LootBarter::CurrentMode()) {
            case M::kLoot:
            case M::kSteal:       return GridInvAPI::kPartnerContainer;
            case M::kBarter:      return GridInvAPI::kPartnerMerchant;
            case M::kPickpocket:  return GridInvAPI::kPartnerPickpocket;
            default:              return GridInvAPI::kPartnerNone;
            }
        }

        std::uint32_t Svc_PartnerRef()
        {
            auto* p = LootBarter::Partner();
            return p ? p->GetFormID() : 0u;
        }

        constinit GridInvAPI::HostServices g_services{
            sizeof(GridInvAPI::HostServices),
            GridInvAPI::kABIVersion,
            &Svc_RequestRebuild,
            &Svc_IsMenuOpen,
            &Svc_CellSpanOf,
            &Svc_PartnerKind,
            &Svc_PartnerRef,
        };

        // ---- registered providers ----------------------------------------
        //
        // One slot today. A copy is kept (not the caller's pointer) because the
        // ABI says pointer arguments are borrowed for the duration of the call.
        GridInvAPI::Provider g_provider{};
        bool                 g_haveProvider = false;

        void Notify(const char* a_text)
        {
            FUI::Sfx::Notify(a_text);
        }

        void OnRegisterProvider(SKSE::MessagingInterface::Message* a_msg)
        {
            // ★★MEASURE THE WHOLE STRUCT, NOT JUST THE TWO FIELDS WE CHECK.
            // This read eight bytes and then trusted offsets 16..47 -- name and
            // three function pointers -- from a buffer that was never proven to
            // hold them. structSize and abiVersion both live inside the first
            // eight, so a short payload sailed past the version gate below. And
            // the listener takes EVERY sender, so any plugin that happens to
            // reuse our message type arrives here with its own payload.
            if (!a_msg->data || a_msg->dataLen < sizeof(GridInvAPI::Provider)) {
                logger::error("[API] REFUSED '{}': payload is {} bytes, a Provider is {}",
                              a_msg->sender ? a_msg->sender : "<unknown>",
                              a_msg->data ? a_msg->dataLen : 0u,
                              sizeof(GridInvAPI::Provider));
                return;
            }
            const auto* p = static_cast<const GridInvAPI::Provider*>(a_msg->data);
            const char* who = a_msg->sender ? a_msg->sender : "<unknown>";

            // Two independent checks: a version bump catches an intentional
            // break, the size catches a struct edited without the bump.
            // Refusal is a log line plus one notification -- never a CTD.
            if (p->abiVersion != GridInvAPI::kABIVersion ||
                p->structSize != sizeof(GridInvAPI::Provider)) {
                logger::error("[API] REFUSED '{}': abiVersion {} (need {}), structSize {} (need {})",
                              who, p->abiVersion, GridInvAPI::kABIVersion,
                              p->structSize, sizeof(GridInvAPI::Provider));
                Notify("Grid Inventory: extension version mismatch - not loaded");
                return;
            }
            if (!p->GetOverlay || !p->GetTooltipLines || !p->OfferDrop) {
                logger::error("[API] REFUSED '{}': null function pointer", who);
                Notify("Grid Inventory: extension is incomplete - not loaded");
                return;
            }
            if (g_haveProvider) {
                logger::warn("[API] '{}' ignored: a provider is already registered ('{}')",
                             who, g_provider.name ? g_provider.name : "?");
                return;
            }

            g_provider     = *p;   // copy; the caller's pointer is borrowed only
            g_haveProvider = true;
            logger::info("[API] provider registered: '{}' (from '{}') abi={}",
                         p->name ? p->name : "<unnamed>", who, p->abiVersion);
        }

        // This listener sees EVERY sender, so it must only ever act on our own
        // 4CC message types -- never on a lifecycle number.
        void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
        {
            if (!a_msg) return;
            if (a_msg->type == GridInvAPI::kMsgRegisterProvider) {
                OnRegisterProvider(a_msg);
                return;
            }
            if (a_msg->type == GridInvAPI::kMsgSuppressUI) {
                // ★Same size check every ABI struct gets: a sender built
                // against a different header is refused rather than read.
                const auto* p = static_cast<const GridInvAPI::SuppressUI*>(a_msg->data);
                if (!p || a_msg->dataLen < sizeof(GridInvAPI::SuppressUI) ||
                    p->structSize != sizeof(GridInvAPI::SuppressUI)) {
                    logger::warn("[API] suppress: malformed payload -- ignored");
                    return;
                }
                // ★kClient: this sender named itself, so it OWNS the hold --
                // the safety net stops second-guessing it against the menu
                // stack (which cannot see a window that is not a menu), and
                // nothing expires it. Its own release, our close and a load
                // are the only ways back. The obligation that buys is spelled
                // out where clients read it, in GridInventoryAPI.h.
                UIRoot::Suppress(p->suppress != 0,
                                 a_msg->sender ? a_msg->sender : "api",
                                 UIRoot::SuppressBy::kClient);
                return;
            }
        }
    }

    void Install()
    {
        static_assert(sizeof(GridInvAPI::HostServices) == 48, "ABI drift");
        // sender == nullptr: no filter, so a provider's message actually lands.
        // Registered under a DIFFERENT sender key than the lifecycle listener,
        // which is what lets both coexist.
        const bool ok = SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
        logger::info("[API] ABI listener registered: {}", ok ? "ok" : "FAILED");
    }

    void Broadcast()
    {
        // Both sides register in SKSEPluginLoad, so ordering does not matter:
        // we announce, they answer (or answered already).
        static GridInvAPI::HostReady ready{
            sizeof(GridInvAPI::HostReady),
            GridInvAPI::kABIVersion,
            &g_services,
        };
        SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgHostReady, &ready, sizeof(ready), nullptr);
        logger::info("[API] host ready broadcast (abi {})", GridInvAPI::kABIVersion);
    }

    void BroadcastCostume(int a_tab, const RE::FormID* a_forms, std::uint32_t a_count)
    {
        // ★★THE BUFFER IS OURS AND IT IS REUSED. The ABI says `pieces` is
        // borrowed for the duration of the callback, so a static buffer is
        // exactly right -- and it means announcing a costume allocates nothing
        // after the first time. Dispatch is synchronous: every listener has
        // read what it needs before this returns.
        static std::vector<GridInvAPI::ItemKey> s_pieces;
        s_pieces.clear();
        if (a_forms && a_count > 0) {
            s_pieces.reserve(a_count);
            for (std::uint32_t i = 0; i < a_count; ++i) {
                GridInvAPI::ItemKey k{};
                k.owner = 0x00000014;   // the player
                k.base  = a_forms[i];
                k.uid   = 0;            // a costume names a FORM, not a stack unit
                s_pieces.push_back(k);
            }
        }

        GridInvAPI::CostumeState st{
            sizeof(GridInvAPI::CostumeState),
            GridInvAPI::kABIVersion,
            a_tab,
            static_cast<std::uint32_t>(s_pieces.size()),
            s_pieces.empty() ? nullptr : s_pieces.data(),
        };
        SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgCostumeState, &st, sizeof(st), nullptr);
        logger::info("[API] costume state broadcast: tab {} ({} piece(s))",
                     a_tab, st.pieceCount);
        // ★★NAME THEM. The count alone cannot be checked against anything --
        // "7 piece(s)" is true whether we sent the right seven or seven of
        // something else, and the FormIDs are the part a listener acts on.
        // Asked directly ("is it really sending which items?"), and the honest
        // answer was that the log could not show it.
        for (const auto& k : s_pieces) {
            const auto* form = RE::TESForm::LookupByID(k.base);
            const char* name = form ? form->GetName() : nullptr;
            logger::info("[API]   {:08X} '{}'", k.base,
                         (name && *name) ? name : "?");
        }
    }

    std::uint32_t ProviderCount()
    {
        return g_haveProvider ? 1u : 0u;
    }

    const GridInvAPI::Overlay* Overlay(const GridInvAPI::ItemKey& a_key)
    {
        if (!g_haveProvider) return nullptr;
        static GridInvAPI::Overlay s_out{};
        s_out = {};
        if (!g_provider.GetOverlay(g_provider.self, &a_key, &s_out)) return nullptr;
        if (s_out.count == 0) return nullptr;
        if (s_out.count > GridInvAPI::kMaxBadges) {
            // A misbehaving provider must not walk us off the array.
            s_out.count = static_cast<std::uint8_t>(GridInvAPI::kMaxBadges);
        }
        return &s_out;
    }

    std::uint32_t TooltipLines(const GridInvAPI::ItemKey& a_key,
                               GridInvAPI::TooltipLine* a_out, std::uint32_t a_capacity)
    {
        if (!g_haveProvider || !a_out || a_capacity == 0) return 0;
        const auto n = g_provider.GetTooltipLines(g_provider.self, &a_key, a_out, a_capacity);
        return (std::min)(n, a_capacity);
    }

    GridInvAPI::DropVerdict OfferDrop(const GridInvAPI::DropQuery& a_query)
    {
        if (!g_haveProvider) return GridInvAPI::kDropReject;
        const auto v = g_provider.OfferDrop(g_provider.self, &a_query);
        return (v <= GridInvAPI::kDropBlocked)
                   ? static_cast<GridInvAPI::DropVerdict>(v)
                   : GridInvAPI::kDropReject;
    }
}
