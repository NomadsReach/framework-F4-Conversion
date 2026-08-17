// PrismaUI_F4/src/Menus/FocusMenu/FocusMenu.cpp
#include "FocusMenu.h"

RE::IMenu* FocusMenu::Creator([[maybe_unused]] const RE::UIMessage& a_message)
{
    auto menu = new FocusMenu();
    if (!menu->IsValid()) {
        delete menu;
        return nullptr;
    }
    return menu;
}

FocusMenu::FocusMenu()
{
    using Context  = RE::UserEvents::INPUT_CONTEXT_ID;
    using MenuFlag = RE::UI_MENU_FLAGS;

    auto scaleformManager = RE::BSScaleformManager::GetSingleton();
    if (!scaleformManager) {
        logger::error("FocusMenu: BSScaleformManager singleton is null");
        return;
    }

    // F4: LoadMovieEx takes IMenu& and a file path string; sets uiMovie internally
    const bool success = scaleformManager->LoadMovieEx(*this, "Interface/CursorMenu.swf");

    if (!success || !this->uiMovie) {
        logger::error("FocusMenu: Failed to load Interface/CursorMenu.swf");
        return;
    }

    uiMovie->SetVisible(false);

    this->menuFlags.set(
        MenuFlag::kUsesCursor,
        MenuFlag::kModal,  // REQUIRED: without it the menu beneath steals mouse input
                           // via the game's device layer (not blockable by the WndProc hook).
        MenuFlag::kAllowSaving,
        MenuFlag::kAdvancesUnderPauseMenu,
        MenuFlag::kRendersUnderPauseMenu);

    // depthPriority is stl::enumeration<UI_DEPTH_PRIORITY, uint8_t>; cast from int
    this->depthPriority = static_cast<RE::UI_DEPTH_PRIORITY>(13);
    this->inputContext  = Context::kCursor;
}

// F4 signature: second param is uint64_t (not uint32_t as in Skyrim)
void FocusMenu::AdvanceMovie([[maybe_unused]] float a_interval,
                              [[maybe_unused]] std::uint64_t a_currentTime)
{
    // Fallout continuously restores the vanilla CursorMenu while a cursor-using menu is active.
    // Keep suppression on the Scaleform/UI thread instead of touching uiMovie from DXGI Present.
    auto ui = RE::UI::GetSingleton();
    if (!ui) {
        return;
    }

    auto cursorMenu = ui->GetMenu("CursorMenu");
    if (cursorMenu && cursorMenu->uiMovie) {
        cursorMenu->uiMovie->SetVisible(false);
    }
}

RE::UI_MESSAGE_RESULTS FocusMenu::ProcessMessage(RE::UIMessage& a_message)
{
    if (a_message.menu == FocusMenu::MENU_NAME) {
        return RE::UI_MESSAGE_RESULTS::kHandled;
    }
    return RE::UI_MESSAGE_RESULTS::kPassOn;
}

bool FocusMenu::IsOpen()
{
    auto ui = RE::UI::GetSingleton();
    return ui && ui->GetMenuOpen(MENU_NAME);  // F4: GetMenuOpen (not IsMenuOpen)
}

void FocusMenu::Open()
{
    logger::debug("FocusMenu::Open requested");
    F4SE::GetTaskInterface()->AddUITask([=] {
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

void FocusMenu::Close()
{
    logger::debug("FocusMenu::Close requested");
    F4SE::GetTaskInterface()->AddUITask([=] {
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
