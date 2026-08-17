#include "FocusMenu.h"

RE::IMenu* FocusMenu::Creator([[maybe_unused]] const RE::UIMessage& message) {
    auto menu = new FocusMenu();
    if (!menu->IsValid()) {
        delete menu;
        return nullptr;
    }
    return menu;
}

FocusMenu::FocusMenu() {
    using Context = RE::UserEvents::INPUT_CONTEXT_ID;
    using MenuFlag = RE::UI_MENU_FLAGS;

    auto scaleformManager = RE::BSScaleformManager::GetSingleton();
    if (!scaleformManager) {
        logger::error("FocusMenu: BSScaleformManager singleton is null");
        return;
    }

    const bool success = scaleformManager->LoadMovieEx(*this, "Interface/CursorMenu.swf");
    if (!success || !uiMovie) {
        logger::error("FocusMenu: Failed to load Interface/CursorMenu.swf");
        return;
    }

    uiMovie->SetVisible(false);

    this->menuFlags.set(
        MenuFlag::kUsesCursor,
        MenuFlag::kModal,  // Prevents the underlying menu from receiving device-layer mouse input.
        MenuFlag::kAllowSaving,
        MenuFlag::kAdvancesUnderPauseMenu,
        MenuFlag::kRendersUnderPauseMenu);

    this->depthPriority = static_cast<RE::UI_DEPTH_PRIORITY>(13);
    this->inputContext = Context::kCursor;
}

void FocusMenu::AdvanceMovie([[maybe_unused]] float interval, [[maybe_unused]] std::uint64_t currentTime) {}

RE::UI_MESSAGE_RESULTS FocusMenu::ProcessMessage(RE::UIMessage& message) {
    if (message.menu == MENU_NAME) {
        return RE::UI_MESSAGE_RESULTS::kHandled;
    }
    return RE::UI_MESSAGE_RESULTS::kPassOn;
}

bool FocusMenu::IsOpen() {
    auto ui = RE::UI::GetSingleton();
    return ui && ui->GetMenuOpen(MENU_NAME);
}

void FocusMenu::Open() {
    logger::debug("FocusMenu::Open requested");
    F4SE::GetTaskInterface()->AddUITask([] {
        auto msgQ = RE::UIMessageQueue::GetSingleton();
        if (msgQ && !IsOpen()) {
            logger::debug("FocusMenu::Open: sending kShow");
            msgQ->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kShow);
        } else if (!msgQ) {
            logger::warn("FocusMenu::Open: UIMessageQueue is null");
        } else {
            logger::debug("FocusMenu::Open: already open, skipping");
        }
    });
}

void FocusMenu::Close() {
    logger::debug("FocusMenu::Close requested");
    F4SE::GetTaskInterface()->AddUITask([] {
        auto msgQ = RE::UIMessageQueue::GetSingleton();
        if (msgQ && IsOpen()) {
            logger::debug("FocusMenu::Close: sending kHide");
            msgQ->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);
        } else if (!msgQ) {
            logger::warn("FocusMenu::Close: UIMessageQueue is null");
        } else {
            logger::debug("FocusMenu::Close: already closed, skipping");
        }
    });
}
