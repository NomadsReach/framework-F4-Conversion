#pragma once

#include <RE/Fallout.h>

class FocusMenu final : public RE::IMenu
{
public:
    static constexpr std::string_view MENU_NAME =
        "PrismaUI_FocusMenu";

    void AdvanceMovie(
        float interval,
        std::uint64_t currentTime) override;
    RE::UI_MESSAGE_RESULTS ProcessMessage(
        RE::UIMessage& message) override;

    [[nodiscard]] static RE::IMenu* Creator(
        const RE::UIMessage& message);
    static void Open() noexcept;
    static void Close() noexcept;
    [[nodiscard]] static bool IsOpen() noexcept;

private:
    FocusMenu();

    [[nodiscard]] bool IsValid() const noexcept
    {
        return uiMovie != nullptr;
    }
};
