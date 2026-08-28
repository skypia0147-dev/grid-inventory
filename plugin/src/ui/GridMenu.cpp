#include "ui/GridMenu.h"
#include "game/Census.h"
#include "game/DeltaWatch.h"
#include "game/Ledger.h"
#include "game/WornLedger.h"
#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/ItemPreview.h"
#include "ui/Sfx.h"
#include "ui/UIRoot.h"

// Ported from ModExplorerMenu (Modex) by patchulidev — UIMenuImpl.cpp.
// https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
// Scaleform-event input relay originally credited to cyfewlp (SimpleIME).
// Changes vs upstream: no hotkey listeners / InputManager / UserConfig;
// gameplay-control toggling deliberately omitted (input policy per PLAN_B §6).

namespace RE
{
    class GFxCharEvent : public RE::GFxEvent
    {
    public:
        GFxCharEvent() = default;

        explicit GFxCharEvent(UINT32 a_wcharCode, UINT8 a_keyboardIndex = 0) :
            GFxEvent(EventType::kCharEvent), wcharCode(a_wcharCode), keyboardIndex(a_keyboardIndex)
        {}

        // @members
        std::uint32_t wcharCode{};      // 04
        std::uint32_t keyboardIndex{};  // 08
    };

    static_assert(sizeof(GFxCharEvent) == 0x0C);
}

namespace
{
    struct
    {
        RE::GFxKey::Code gfxCode;
        ImGuiKey         imGuiKey;
    } GFxCodeToImGuiKeyTable[] = {
        { RE::GFxKey::kAlt,          ImGuiMod_Alt            },
        { RE::GFxKey::kControl,      ImGuiMod_Ctrl           },
        { RE::GFxKey::kShift,        ImGuiMod_Shift          },
        { RE::GFxKey::kCapsLock,     ImGuiKey_CapsLock       },
        // Don't send tab: upstream notes a bug when tab closes the menu
        { RE::GFxKey::kHome,         ImGuiKey_Home           },
        { RE::GFxKey::kEnd,          ImGuiKey_End            },
        { RE::GFxKey::kPageUp,       ImGuiKey_PageUp         },
        { RE::GFxKey::kPageDown,     ImGuiKey_PageDown       },
        { RE::GFxKey::kComma,        ImGuiKey_Comma          },
        { RE::GFxKey::kPeriod,       ImGuiKey_Period         },
        { RE::GFxKey::kSlash,        ImGuiKey_Slash          },
        { RE::GFxKey::kBackslash,    ImGuiKey_Backslash      },
        { RE::GFxKey::kQuote,        ImGuiKey_Apostrophe     },
        { RE::GFxKey::kBracketLeft,  ImGuiKey_LeftBracket    },
        { RE::GFxKey::kBracketRight, ImGuiKey_RightBracket   },
        { RE::GFxKey::kReturn,       ImGuiKey_Enter          },
        { RE::GFxKey::kEqual,        ImGuiKey_Equal          },
        { RE::GFxKey::kMinus,        ImGuiKey_Minus          },
        { RE::GFxKey::kEscape,       ImGuiKey_Escape         },
        { RE::GFxKey::kLeft,         ImGuiKey_LeftArrow      },
        { RE::GFxKey::kUp,           ImGuiKey_UpArrow        },
        { RE::GFxKey::kRight,        ImGuiKey_RightArrow     },
        { RE::GFxKey::kDown,         ImGuiKey_DownArrow      },
        { RE::GFxKey::kSpace,        ImGuiKey_Space          },
        { RE::GFxKey::kBackspace,    ImGuiKey_Backspace      },
        { RE::GFxKey::kDelete,       ImGuiKey_Delete         },
        { RE::GFxKey::kInsert,       ImGuiKey_Insert         },
        { RE::GFxKey::kKP_Multiply,  ImGuiKey_KeypadMultiply },
        { RE::GFxKey::kKP_Add,       ImGuiKey_KeypadAdd      },
        { RE::GFxKey::kKP_Enter,     ImGuiKey_KeypadEnter    },
        { RE::GFxKey::kKP_Subtract,  ImGuiKey_KeypadSubtract },
        { RE::GFxKey::kKP_Decimal,   ImGuiKey_KeypadDecimal  },
        { RE::GFxKey::kKP_Divide,    ImGuiKey_KeypadDivide   },
        { RE::GFxKey::kVoidSymbol,   ImGuiKey_None           },
    };

    ImGuiMouseSource GetMouseSourceFromMessageExtraInfo()
    {
        LPARAM extra_info = ::GetMessageExtraInfo();
        if ((extra_info & 0xFFFFFF80) == 0xFF515700) return ImGuiMouseSource_Pen;
        if ((extra_info & 0xFFFFFF80) == 0xFF515780) return ImGuiMouseSource_TouchScreen;
        return ImGuiMouseSource_Mouse;
    }
}

namespace FUI
{
    void GridInventoryMenu::FlushInputState()
    {
        if (ImGui::GetCurrentContext() == nullptr) return;

        auto& io = ImGui::GetIO();
        io.ClearInputKeys();
        io.ClearInputMouse();   // ★see UIRoot::OnClose -- keys alone leave the buttons down
        io.ClearEventsQueue();
    }

    void GridInventoryMenu::RegisterMenu()
    {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(MENU_NAME, Creator);
            SKSE::log::info("[UI] {} registered", MENU_NAME);
        }
    }

    void GridInventoryMenu::ForceCursor()
    {
        // Keep asking UNLESS we have taken the pointer over ourselves — the
        // engine's arrow is what a pad should be moving, so it has to stay up
        // for that path to work at all.
        if (!UIRoot::WantsGameCursor()) return;
        // ★★★NOT WHILE THE CONSOLE IS UP. Measured: toggling the console with
        // our menu open destroys CursorMenu and remakes it -- the movie comes
        // back at a new address every time -- and this function, seeing it
        // closed, asks for it again. So every frame the engine took the arrow
        // away we handed it straight back, MouseHandler hid it, and the next
        // frame did it again. That blink is a second cursor appearing and
        // vanishing on the console's edges (user report).
        //
        // The probe that settled it: `visible=1 -> after hide 0`, frame after
        // frame. Our hide was working perfectly; we were the ones turning it
        // back on. The console owns the screen while it is up -- stop fighting
        // it, and pick the pointer back up when it leaves.
        if (UIRoot::IsConsoleOpen()) return;
        if (auto* ui = RE::UI::GetSingleton(); ui && !ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
            SKSE::GetTaskInterface()->AddUITask([]() {
                if (const auto mq = RE::UIMessageQueue::GetSingleton()) {
                    mq->AddMessage(RE::CursorMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                }
            });
        }
    }

    void GridInventoryMenu::AdvanceMovie(float a_interval, std::uint32_t a_currentTime)
    {
        // kPausesGame stops the PlayerCharacter::Update hook, so pre-render
        // parking must run here: menus still advance every frame while the
        // game is paused, BEFORE the frame renders.
        UIRoot::Tick();
        // ★Frames, not wall clock (Ledger.h): AdvanceMovie keeps running while
        // the menu pauses the game, which is exactly the clock we want.
        FUI::Ledger::Tick();
        IMenu::AdvanceMovie(a_interval, a_currentTime);
    }

    void GridInventoryMenu::PostDisplay()
    {
        // ★MEASURE WHILE WE ARE ACTUALLY ON SCREEN. The game's cursor is drawn
        // by CursorMenu and lands ON TOP of us -- that is why a tooltip can end
        // up under the arrow. Whether we could ever draw above it depends on a
        // number the engine sets at runtime and CommonLib does not carry.
        // Asked at registration it read "not open": the cursor menu does not
        // exist that early. Once, from here, is the honest place to ask.
        static bool s_depthSaid = false;
        if (!s_depthSaid) {
            s_depthSaid = true;
            if (auto* ui = RE::UI::GetSingleton()) {
                if (const auto cm = ui->GetMenu(RE::CursorMenu::MENU_NAME)) {
                    logger::info("[UI] CursorMenu depthPriority={} (ours={})",
                                 cm->depthPriority, depthPriority);
                } else {
                    logger::info("[UI] CursorMenu is not open even while we render");
                }
            }
        }

        IconCache::GetSingleton()->PreRender();   // icon queue owns the request while busy
        ItemPreview::GetSingleton()->Render();
        IconCache::GetSingleton()->PostRender();  // harvest this frame's capture
        // pixel style: turn a few realistic sprites into dot versions per
        // frame. AFTER PostRender so a capture that just landed is available
        // to derive from on this very frame.
        IconCache::GetSingleton()->TickPixelDerive();
        UIRoot::Render();
        ForceCursor();
    }

    // close-sound guard: the user-event handler plays MenuClose BEFORE the
    // close message; engine-initiated closes (I-key routed internally, menu
    // hopping) bypass that handler entirely — OnHide plays the fallback.
    // The custom descriptor sounds stay audible through the hide transition
    // (only the UI-category vanilla path was swallowed).
    static bool g_closeSfxPlayed = false;

    void GridInventoryMenu::MarkCloseSfxPlayed() { g_closeSfxPlayed = true; }

    void GridInventoryMenu::OnShow()
    {
        g_closeSfxPlayed = false;
        // ★The museum sweep, taken HERE and only here. Donating requires
        // closing the inventory, so a reading taken as the menu opens cannot go
        // stale while it is up -- and the alternative, resolving a reference
        // handle per tile per frame, is the cost this avoids.
        FUI::Lotd::Refresh();
        // ★1.4/B0: the strongest test in the whole step. Everything that
        // happened while the menu was SHUT had to arrive as events; if the
        // running total disagrees with a fresh count here, the engine does not
        // tell us everything and §9 says 1.4 stops.
        FUI::DeltaWatch::Reconcile("menu-open");
        // ★B1 AFTER B0: the form-level audit is the cheaper question and its
        // answer frames the kind-level one. If the totals already disagree,
        // a kind relabel report is describing a board that was wrong anyway.
        // ★B4-1: the census yes is the INTEGRITY GATE for the now-conditional
        // menu-open rebuild. Every count change while the menu was shut raised
        // the rebuild flag through its event -- but a VALUE change (the
        // grindstone) moves no counts and fires no event, so the census is
        // its only witness.
        if (FUI::Census::Take("menu-open")) {
            FUI::Grid::RequestRebuild();
        }
        // B4-2 observation: did the worn ledger stay in step with the engine
        // across the closed-menu stretch on events alone?
        FUI::WornLedger::Audit("menu-open");
        // A save from before the doll-favorite fix can carry a phantom
        // {Hotkey}-only list (one item drawn as two). Retired here, once
        // per open, before the board reads the entry.
        FUI::Grid::HealPhantomHotkeyLists();
        // ★B2 flushes on OPEN, not on close. Closing the menu right after a
        // request reported it outstanding at ONE frame old -- the confirmation
        // was simply still in flight. Waiting until the next open gives every
        // request its full chance, and what is left by then really was never
        // answered.
        FUI::Ledger::Flush("since-last-open");
        FlushInputState();
        UIRoot::OnShow();   // whoosh plays deferred (UIRoot) — at kShow time
                            // the audio path swallowed it
        ItemPreview::GetSingleton()->Begin();
    }

    void GridInventoryMenu::OnHide()
    {
        if (!g_closeSfxPlayed) {
            Sfx::MenuClose();
        }
        g_closeSfxPlayed = false;
        // ...and this one covers the session itself: our own actions, which we
        // are supposed to know about exactly.
        FUI::DeltaWatch::Reconcile("menu-close");
        // The close take RE-BASELINES only -- its diff describes the session
        // the board just handled live, so its verdict is deliberately ignored
        // (acting on it would make every reopen after an active session
        // rebuild for nothing, undoing B4-1).
        (void)FUI::Census::Take("menu-close");
        // ...and across the in-menu session (our own equip UI's requests)
        FUI::WornLedger::Audit("menu-close");
        ItemPreview::GetSingleton()->End();
        UIRoot::OnClose();
    }

    RE::UI_MESSAGE_RESULTS GridInventoryMenu::ProcessMessage(RE::UIMessage& a_message)
    {
        switch (a_message.type.get()) {
        case RE::UI_MESSAGE_TYPE::kUpdate:
            break;
        case RE::UI_MESSAGE_TYPE::kUserEvent: {
            // A book is open on top of us: hands off every key. Swallowing
            // Cancel here would leave the player unable to close the book.
            // ★Same for the console — swallowing Cancel there would leave the
            // player unable to close IT, and our menu must not react to keys
            // aimed at a command line.
            if (UIRoot::IsBookOpen() || UIRoot::IsConsoleOpen()) break;
            if (const auto* data = reinterpret_cast<RE::BSUIMessageData*>(a_message.data)) {
                // While a text field owns the keyboard, EVERY key is text —
                // swallow the whole user-event channel (J opened the Journal
                // mid-typing; ESC unfocuses the field via ImGui instead).
                if (UIRoot::IsTextInputActive()) {
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
                const auto* ue = RE::UserEvents::GetSingleton();
                // A2: ESC ("Cancel") and the inventory hotkey ("Inventory" —
                // translated by the kItemMenu context, vanilla-style) both
                // close; while carrying an item they cancel the carry first.
                if (data->fixedStr == ue->cancel || data->fixedStr == ue->inventory) {
                    if (Grid::IsHolding()) {
                        Grid::CancelHold();
                        Sfx::SelectOff();   // carry cancelled
                    } else if (UIRoot::CloseTopWindow()) {
                        Sfx::SelectOff();   // a sub-window closed
                    } else {
                        // sub-windows/settings/EDIT close first; only then the
                        // inventory. Sound plays BEFORE the close message —
                        // during the hide transition it never became audible.
                        Sfx::MenuClose();
                        g_closeSfxPlayed = true;
                        UIRoot::Close();
                    }
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
                // vanilla-style menu hopping (J = Journal, TAB = Tween etc.)
                // stays available when not typing
            }
            break;
        }
        // ★★★THREE MESSAGES, THREE MEANINGS (contract in UIRoot.h).
        //
        // kHide used to be our close, and closing runs the whole session
        // teardown -- the trash is emptied, the loot session ends, the carry
        // is put down, ui.ini is written. But kHide is also the courtesy every
        // other mod sends to put a window over a menu, and answering it with
        // that teardown is why a MessageBox over the grid throws the player
        // out of the chest they were standing at, and why an overlay mod
        // could not sit on top of us at all (reported by the author of
        // Fitting Room / Menu Studio).
        //
        // So kHide SUPPRESSES: we stay open, stop drawing, stop listening,
        // and everything the session holds survives. kForceHide -- the
        // engine's own "close, no negotiation" -- is our close now, and
        // UIRoot::Close() sends it.
        case RE::UI_MESSAGE_TYPE::kShow:
        case RE::UI_MESSAGE_TYPE::kReshow:
            UIRoot::Suppress(false, "kShow");
            OnShow();
            break;
        case RE::UI_MESSAGE_TYPE::kHide:
            UIRoot::Suppress(true, "kHide");
            return RE::UI_MESSAGE_RESULTS::kHandled;   // ...and STAY on the stack
        case RE::UI_MESSAGE_TYPE::kForceHide:
            OnHide();
            break;
        case RE::UI_MESSAGE_TYPE::kScaleformEvent: {
            // ★★★A SUPPRESSED MENU MUST NOT EAT THE INPUT ITS GUEST NEEDS.
            //
            // Returning kHandled below stops the event dead: nothing under us
            // ever sees it. That is right while we are on screen and wrong the
            // instant we are not -- the poison confirm box appeared with the
            // grid correctly hidden and could not be clicked, because every
            // mouse event was still being swallowed here. The book has always
            // broken out for exactly this reason; suppression joins it.
            if (UIRoot::IsBookOpen() || UIRoot::IsSuppressed()) break;
            auto* scaleformData = reinterpret_cast<RE::BSUIScaleformData*>(a_message.data);
            if (scaleformData && scaleformData->scaleformEvent) {
                ProcessScaleformEvent(scaleformData);
                return RE::UI_MESSAGE_RESULTS::kHandled;
            }
            break;
        }
        default:;
        }

        return IMenu::ProcessMessage(a_message);
    }

    RE::IMenu* GridInventoryMenu::Creator()
    {
        using Flags = RE::UI_MENU_FLAGS;
        auto* menu = new GridInventoryMenu();
        menu->menuFlags.set(Flags::kUpdateUsesCursor, Flags::kUsesCursor);
        menu->menuFlags.set(Flags::kCustomRendering);
        menu->menuFlags.set(Flags::kUsesMenuContext);
        // the engine blocks saving while ANY open menu lacks this flag —
        // the vanilla InventoryMenu carries it (F5 works there), so must we
        menu->menuFlags.set(Flags::kAllowSaving);
        menu->depthPriority = 11;


        // kPausesGame is REQUIRED for the engine's item 3D preview: Modex's own
        // config notes "show3DPreview requires pauseGame; effective only when
        // both are on" — Inventory3DManager::Render() draws nothing while the
        // game runs. Realtime policy (PLAN_B A5) is revisited at B-2 via the
        // icon cache (captures become rare one-shots).
        menu->menuFlags.set(Flags::kPausesGame, Flags::kDisablePauseMenu);

        // kInventory, NOT kMenuMode/kItemMenu: this is the vanilla
        // InventoryMenu's own context — its controlmap section TRANSLATES the
        // bound inventory key into the "Inventory" user event delivered to
        // ProcessMessage (the same channel that brings "Cancel" for ESC).
        // Other contexts simply swallow the key: no event, nothing to receive.
        menu->inputContext.set(Context::kInventory);
        return menu;
    }

    void GridInventoryMenu::ProcessScaleformEvent(const RE::BSUIScaleformData* a_data)
    {
        // ★★The console is a SEPARATE menu drawn over us, and the engine hands
        // the same keystrokes to both — so every character typed into it also
        // landed in whatever ImGui widget had focus (reported: the item search
        // box filled up while entering a console command). Keys stop here while
        // it is up; the MOUSE still passes, because the console does not use it
        // and freezing a window the player can still see would be worse.
        // Not IsBookOpen's treatment: a book HIDES us and we skip the frame
        // entirely, whereas the console leaves our windows on screen.
        const bool console = UIRoot::IsConsoleOpen();
        switch (const auto& fxEvent = a_data->scaleformEvent; fxEvent->type.get()) {
        case RE::GFxEvent::EventType::kMouseDown:
            OnMouseEvent(fxEvent, true);
            break;
        case RE::GFxEvent::EventType::kMouseUp:
            OnMouseEvent(fxEvent, false);
            break;
        case RE::GFxEvent::EventType::kMouseWheel:
            OnMouseWheelEvent(fxEvent);
            break;
        case RE::GFxEvent::EventType::kKeyDown:
            if (console) break;
            OnKeyEvent(fxEvent, true);
            break;
        case RE::GFxEvent::EventType::kKeyUp:
            if (console) break;
            OnKeyEvent(fxEvent, false);
            break;
        case RE::GFxEvent::EventType::kCharEvent:
            if (console) break;
            OnCharEvent(fxEvent);
            break;
        default:
            break;
        }
    }

    void GridInventoryMenu::OnMouseEvent(RE::GFxEvent* a_event, const bool a_down)
    {
        const auto  mouseSource = GetMouseSourceFromMessageExtraInfo();
        const auto* mouseEvent  = reinterpret_cast<RE::GFxMouseEvent*>(a_event);
        auto&       io          = ImGui::GetIO();

        io.AddMouseSourceEvent(mouseSource);
        io.AddMouseButtonEvent(static_cast<int>(mouseEvent->button), a_down);
    }

    void GridInventoryMenu::OnMouseWheelEvent(RE::GFxEvent* a_event)
    {
        const auto* mouseEvent = reinterpret_cast<RE::GFxMouseEvent*>(a_event);
        UIRoot::AddScrollEvent(0.0f, mouseEvent->scrollDelta);
    }

    void GridInventoryMenu::OnKeyEvent(RE::GFxEvent* a_event, const bool a_down)
    {
        const auto* keyEvent = reinterpret_cast<RE::GFxKeyEvent*>(a_event);
        const auto  imguiKey = GFxKeyToImGuiKey(keyEvent->keyCode);
        // (Inventory-key close lives in UIRoot::HandleCloseHotkey — Scaleform
        // key events don't reach a movie-less menu reliably.)

        if (imguiKey == ImGuiKey_PageUp && a_down) {
            UIRoot::AddScrollEvent(0.0f, 1.0f);
            return;
        }
        if (imguiKey == ImGuiKey_PageDown && a_down) {
            UIRoot::AddScrollEvent(0.0f, -1.0f);
            return;
        }

        // Menu-mode WASD arrives PRE-TRANSLATED into arrow GFx codes, which
        // moved the text caret while typing a/d/w/s. While a text field owns
        // the keyboard an arrow only passes if the PHYSICAL arrow key is down
        // (GetAsyncKeyState); the synthetic ones are dropped. Track what we
        // passed so a real arrow's key-up never leaves ImGui a stuck key.
        {
            static bool s_arrowLive[4] = {};
            int ai = -1, vk = 0;
            switch (imguiKey) {
            case ImGuiKey_LeftArrow:  ai = 0; vk = VK_LEFT; break;
            case ImGuiKey_RightArrow: ai = 1; vk = VK_RIGHT; break;
            case ImGuiKey_UpArrow:    ai = 2; vk = VK_UP; break;
            case ImGuiKey_DownArrow:  ai = 3; vk = VK_DOWN; break;
            default: break;
            }
            // ★★★THE RELEASE FOLLOWS THE PRESS, NOT THE FIELD'S STATE.
            //
            // The whole block used to sit behind IsTextInputActive(), so the
            // flag was written only while a field had the keyboard -- and the
            // release was judged by the state at RELEASE time. Press an arrow
            // with no field focused (the down goes straight through, flag
            // still false), then click the search box while holding it, then
            // let go: now the field IS active, the guard runs, the flag says
            // false, and the key-up is dropped. ImGui holds that arrow down
            // for good and repeats it into the field.
            //
            // s_arrowLive now means exactly "we passed a down for this arrow
            // and owe ImGui the up", which is the invariant the note above
            // always claimed.
            if (ai >= 0) {
                if (a_down) {
                    if (UIRoot::IsTextInputActive() &&
                        (GetAsyncKeyState(vk) & 0x8000) == 0) {
                        return;   // WASD-translated while typing: not a real arrow
                    }
                    s_arrowLive[ai] = true;
                } else {
                    if (!s_arrowLive[ai]) return;   // we never passed its down
                    s_arrowLive[ai] = false;
                }
            }
        }

        ImGui::GetIO().AddKeyEvent(imguiKey, a_down);
    }

    void GridInventoryMenu::OnCharEvent(RE::GFxEvent* a_event)
    {
        // Chars come from the chained WndProc now (UIRoot::WndProcThunk) —
        // the movie-less menu never reliably received GFxCharEvents, and
        // feeding both paths would double-type where they DO arrive.
        (void)a_event;
    }

    ImGuiKey GridInventoryMenu::GFxKeyToImGuiKey(const RE::GFxKey::Code a_keyCode)
    {
        ImGuiKey imguiKey = ImGuiKey_None;

        if (a_keyCode >= RE::GFxKey::kA && a_keyCode <= RE::GFxKey::kZ) {
            imguiKey = static_cast<ImGuiKey>(a_keyCode - RE::GFxKey::kA + ImGuiKey_A);
        } else if (a_keyCode >= RE::GFxKey::kF1 && a_keyCode <= RE::GFxKey::kF15) {
            imguiKey = static_cast<ImGuiKey>(a_keyCode - RE::GFxKey::kF1 + ImGuiKey_F1);
        } else if (a_keyCode >= RE::GFxKey::kNum0 && a_keyCode <= RE::GFxKey::kNum9) {
            imguiKey = static_cast<ImGuiKey>(a_keyCode - RE::GFxKey::kNum0 + ImGuiKey_0);
        } else if (a_keyCode >= RE::GFxKey::kKP_0 && a_keyCode <= RE::GFxKey::kKP_9) {
            imguiKey = static_cast<ImGuiKey>(a_keyCode - RE::GFxKey::kKP_0 + ImGuiKey_Keypad0);
        } else {
            for (const auto& [gfxCode, imGuiKey] : GFxCodeToImGuiKeyTable) {
                if (a_keyCode == gfxCode) {
                    imguiKey = imGuiKey;
                    break;
                }
            }
        }

        return imguiKey;
    }
}
