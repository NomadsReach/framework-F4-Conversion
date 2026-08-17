#pragma once

#include <RE/Fallout.h>

class FocusMenu : public RE::IMenu {
public:
    static constexpr std::string_view MENU_NAME = "PrismaUI_FocusMenu";

    // Fallout 4 uses uint64_t for the current-time parameter.
    void AdvanceMovie(float interval, std::uint64_t currentTime) override;
    RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& message) override;

    static RE::IMenu* Creator(const RE::UIMessage& message);
    static void Open();
    static void Close();
    static bool IsOpen();

    bool IsValid() const { return uiMovie != nullptr; }

private:
    FocusMenu();
};
