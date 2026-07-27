#include "PCH.h"

#include "Menus/FocusMenu/FocusMenu.h"

RE::IMenu* FocusMenu::Creator(
    [[maybe_unused]] const RE::UIMessage& message)
{
    auto menu = std::unique_ptr<FocusMenu>(new FocusMenu());
    return menu->IsValid() ? menu.release() : nullptr;
}

FocusMenu::FocusMenu()
{
    using MenuFlag = RE::UI_MENU_FLAGS;

    const auto scaleform = RE::BSScaleformManager::GetSingleton();
    if (!scaleform ||
        !scaleform->LoadMovieEx(*this, "Interface/CursorMenu.swf") ||
        !uiMovie) {
        logger::error(
            "PrismaUI focus menu could not load CursorMenu.swf");
        return;
    }

    uiMovie->SetVisible(false);
    menuFlags.set(
        MenuFlag::kUsesCursor,
        MenuFlag::kModal,
        MenuFlag::kAllowSaving,
        MenuFlag::kAdvancesUnderPauseMenu,
        MenuFlag::kRendersUnderPauseMenu);
    depthPriority = static_cast<RE::UI_DEPTH_PRIORITY>(13);
    inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kCursor;
}

void FocusMenu::AdvanceMovie(
    [[maybe_unused]] float interval,
    [[maybe_unused]] std::uint64_t currentTime)
{}

RE::UI_MESSAGE_RESULTS FocusMenu::ProcessMessage(
    RE::UIMessage& message)
{
    return message.menu == MENU_NAME ?
        RE::UI_MESSAGE_RESULTS::kHandled :
        RE::UI_MESSAGE_RESULTS::kPassOn;
}

bool FocusMenu::IsOpen() noexcept
{
    const auto ui = RE::UI::GetSingleton();
    return ui && ui->GetMenuOpen(MENU_NAME);
}

void FocusMenu::Open() noexcept
{
    try {
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks) {
            return;
        }
        tasks->AddUITask([] {
            const auto queue = RE::UIMessageQueue::GetSingleton();
            if (queue && !IsOpen()) {
                queue->AddMessage(
                    MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kShow);
            }
        });
    } catch (...) {
        logger::error("Could not queue PrismaUI focus-menu open");
    }
}

void FocusMenu::Close() noexcept
{
    try {
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks) {
            return;
        }
        tasks->AddUITask([] {
            const auto queue = RE::UIMessageQueue::GetSingleton();
            if (queue && IsOpen()) {
                queue->AddMessage(
                    MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide);
            }
        });
    } catch (...) {
        logger::error("Could not queue PrismaUI focus-menu close");
    }
}
