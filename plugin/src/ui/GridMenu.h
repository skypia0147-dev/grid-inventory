#pragma once

#include <imgui.h>

namespace FUI
{
    // Ported from ModExplorerMenu (Modex) by patchulidev — ModexGUIMenu.
    // https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
    //
    // The kCustomRendering menu whose PostDisplay is the game's UI render
    // pass — the only stage where Inventory3DManager::Render() actually
    // draws (MODEX_TRANSITION_PLAN §2 root cause). Also relays Scaleform
    // input events into ImGui.
    class GridInventoryMenu : public RE::IMenu
    {
    public:
        static constexpr std::string_view MENU_NAME = "GridInventoryMenu";

        static void RegisterMenu();
        static void FlushInputState();

        // F1: the titlebar ✕ plays MenuClose itself before UIRoot::Close();
        // this suppresses OnHide's fallback so the sound doesn't double up.
        static void MarkCloseSfxPlayed();

        void PostDisplay() override;
        void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;
        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override;

    private:
        void OnShow();
        void OnHide();

        static RE::IMenu* Creator();

        static void ProcessScaleformEvent(const RE::BSUIScaleformData* a_data);
        static void OnMouseEvent(RE::GFxEvent* a_event, bool a_down);
        static void OnMouseWheelEvent(RE::GFxEvent* a_event);
        static void OnKeyEvent(RE::GFxEvent* a_event, bool a_down);
        static void OnCharEvent(RE::GFxEvent* a_event);
        static void ForceCursor();

        static ImGuiKey GFxKeyToImGuiKey(RE::GFxKey::Code a_keyCode);

        // ★★★THE TAIL VR'S ENGINE WRITES INTO, AND THE REASON A VR PLAYER
        // CRASHED. RE::IMenu is 0x40 bytes on VR and 0x30 on SE/AE
        // (STATIC_ASSERT_SIZE(IMenu, 0x30, 0x30, 0x40, 0x30)), and a cross-VR
        // CommonLibSSE build compiles the 0x30 layout — the VR-only fields sit
        // behind `#if defined(EXCLUSIVE_SKYRIM_VR)` and are simply absent. So
        // `new GridInventoryMenu()` asked the heap for 0x30 bytes while the VR
        // engine went on writing unk30/unk34/menuName into 0x30..0x3F: sixteen
        // bytes past the end of the allocation, straight into whatever block
        // followed.
        //
        // The crash log that named it: `mov [rax], r8` with
        // rax = 0x6E654D79726F746E, which is "ntoryMen" little-endian — bytes
        // 8..15 of "GridInventoryMenu". A pointer field had been read out of a
        // string buffer, which is what a smashed neighbouring block looks like.
        //
        // Declared in VR's own order so the offsets land exactly where the
        // engine expects (0x30 / 0x34 / 0x38), rather than as opaque padding:
        // the BSFixedString then also releases its ref when we are destroyed.
        // SE/AE never touch these — the cost there is sixteen bytes per menu.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> _vrUnk30{ RE::UI_MENU_Unk09::kNone };
        std::byte                                     _vrUnk34{ std::byte{ 1 } };
        RE::BSFixedString                             _vrMenuName{};
    };

    static_assert(sizeof(GridInventoryMenu) >= 0x40,
        "VR's IMenu is 0x40 -- the engine writes past a 0x30 allocation");
}
