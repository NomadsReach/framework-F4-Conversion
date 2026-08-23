#pragma once

#include <windows.h>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

namespace WinKeyHandler {
    using namespace ultralight::KeyCodes;

    int WinKeyToUltralightKey(UINT winKey);
    std::string GetUltralightKeyIdentifier(int ulKey);
    void GetUltralightModifiers(ultralight::KeyEvent& event);
    ultralight::KeyEvent CreateKeyEvent(ultralight::KeyEvent::Type type, WPARAM wParam, LPARAM lParam);
}
