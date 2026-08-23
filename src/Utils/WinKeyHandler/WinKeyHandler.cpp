#include "WinKeyHandler.h"

namespace WinKeyHandler {
    void GetUltralightModifiers(ultralight::KeyEvent& event) {
        event.modifiers = 0;
        if (GetKeyState(VK_MENU) < 0) {
            event.modifiers |= ultralight::KeyEvent::kMod_AltKey;
        }
        if (GetKeyState(VK_CONTROL) < 0) {
            event.modifiers |= ultralight::KeyEvent::kMod_CtrlKey;
        }
        if (GetKeyState(VK_SHIFT) < 0) {
            event.modifiers |= ultralight::KeyEvent::kMod_ShiftKey;
        }
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) {
            event.modifiers |= ultralight::KeyEvent::kMod_MetaKey;
        }
    }

    ultralight::KeyEvent CreateKeyEvent(ultralight::KeyEvent::Type type, WPARAM wParam, LPARAM lParam) {
        const bool isSystemKey =
            GetKeyState(VK_MENU) < 0 &&
            (wParam == VK_TAB || wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_SPACE ||
             (wParam >= VK_F1 && wParam <= VK_F24));

        return ultralight::KeyEvent(type, static_cast<uintptr_t>(wParam), static_cast<intptr_t>(lParam), isSystemKey);
    }
}
