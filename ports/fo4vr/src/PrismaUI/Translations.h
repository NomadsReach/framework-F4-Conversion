#pragma once

#include <memory>
#include <string>

namespace ultralight
{
    class View;
}

namespace PrismaUI::Core
{
    struct PrismaView;
}

namespace PrismaUI::Translations
{
    [[nodiscard]] std::shared_ptr<const std::string> LoadTranslationScript(
        const std::string& pluginName);

    [[nodiscard]] bool InjectForCurrentWindow(
        Core::PrismaView& view,
        ultralight::View* ultralightView) noexcept;
}
