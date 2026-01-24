#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

namespace WinKeyHandler {
	using namespace ultralight::KeyCodes;

	int WinKeyToUltralightKey(UINT win_key);
	std::string GetUltralightKeyIdentifier(int ul_key);
	void GetUltralightModifiers(ultralight::KeyEvent& ev);
	ultralight::KeyEvent CreateKeyEvent(ultralight::KeyEvent::Type type, WPARAM wParam, LPARAM lParam);
}