#include "FocusMenu.h"

#include <atomic>

namespace {
std::atomic<bool> g_vanillaCursorHidden{false};

bool SetVanillaCursorVisible(bool visible)
{
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;
    auto cursorMenu = ui->GetMenu("CursorMenu");
    if (!cursorMenu || !cursorMenu->uiMovie) return false;
    cursorMenu->uiMovie->SetVisible(visible);
    return true;
}
}

RE::IMenu* FocusMenu::Creator([[maybe_unused]] const RE::UIMessage& message)
{
    auto* menu = new FocusMenu();
    if (!menu->IsValid()) {
        delete menu;
        return nullptr;
    }
    return menu;
}

FocusMenu::FocusMenu()
{
    using Context = RE::UserEvents::INPUT_CONTEXT_ID;
    using MenuFlag = RE::UI_MENU_FLAGS;

    auto* scaleformManager = RE::BSScaleformManager::GetSingleton();
    if (!scaleformManager || !scaleformManager->LoadMovieEx(*this, "Interface/CursorMenu.swf") || !uiMovie) {
        logger::error("FocusMenu: failed to load CursorMenu.swf");
        return;
    }

    uiMovie->SetVisible(false);
    menuFlags.set(MenuFlag::kUsesCursor, MenuFlag::kModal, MenuFlag::kAllowSaving,
                  MenuFlag::kAdvancesUnderPauseMenu, MenuFlag::kRendersUnderPauseMenu);
    depthPriority = static_cast<RE::UI_DEPTH_PRIORITY>(13);
    inputContext = Context::kCursor;
}

void FocusMenu::AdvanceMovie([[maybe_unused]] float interval, [[maybe_unused]] std::uint64_t currentTime) {}

RE::UI_MESSAGE_RESULTS FocusMenu::ProcessMessage(RE::UIMessage& message)
{
    if (message.menu == MENU_NAME) return RE::UI_MESSAGE_RESULTS::kHandled;
    return RE::UI_MESSAGE_RESULTS::kPassOn;
}

bool FocusMenu::IsOpen()
{
    auto* ui = RE::UI::GetSingleton();
    return ui && ui->GetMenuOpen(MENU_NAME);
}

void FocusMenu::Open()
{
    auto* tasks = F4SE::GetTaskInterface();
    if (!tasks) return;

    tasks->AddUITask([] {
        if (!g_vanillaCursorHidden.load(std::memory_order_acquire)) {
            if (SetVanillaCursorVisible(false)) {
                g_vanillaCursorHidden.store(true, std::memory_order_release);
            }
        }

        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (queue && !IsOpen()) queue->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kShow);
    });
}

void FocusMenu::Close()
{
    auto* tasks = F4SE::GetTaskInterface();
    if (!tasks) return;

    tasks->AddUITask([] {
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (queue && IsOpen()) queue->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);

        if (g_vanillaCursorHidden.exchange(false, std::memory_order_acq_rel)) {
            SetVanillaCursorVisible(true);
        }
    });
}
