#pragma once

#include "PrismaUI/Core.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace PrismaUI
{
    class ImeHelper final
    {
    public:
        using CommitTextCallback = void (*)(
            Core::PrismaViewId,
            std::wstring_view) noexcept;

        ImeHelper() = default;
        ~ImeHelper() = default;

        ImeHelper(const ImeHelper&) = delete;
        ImeHelper& operator=(const ImeHelper&) = delete;

        void Initialize(
            HWND window,
            CommitTextCallback commitText) noexcept;
        void Shutdown() noexcept;

        void SetFocusedView(Core::PrismaViewId viewId) noexcept;
        void SetTextInputFocused(bool focused) noexcept;
        void SetAssociation(bool enabled) noexcept;
        void ClearState(Core::PrismaViewId viewId) noexcept;

        [[nodiscard]] bool HandleControlMessage(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam,
            LRESULT& result) noexcept;
        [[nodiscard]] bool HandleImeMessage(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam) noexcept;
        void SuppressNativeUi(UINT message, LPARAM& lParam) noexcept;

        [[nodiscard]] static UINT AssociationMessage() noexcept;

    private:
        void ApplyAssociationOnWindowThread(bool enabled) noexcept;
        void PublishState(bool active) noexcept;
        [[nodiscard]] std::wstring ReadComposition(
            HIMC context,
            DWORD kind,
            std::size_t maximumCharacters) const;
        [[nodiscard]] std::string BuildStateScript(
            HIMC context,
            bool active) const;

        HWND window_ = nullptr;
        HIMC ownedContext_ = nullptr;
        HIMC previousContext_ = nullptr;
        CommitTextCallback commitText_ = nullptr;

        std::atomic<Core::PrismaViewId> focusedView_ = 0;
        std::atomic<bool> textInputFocused_ = false;
        std::atomic<bool> associated_ = false;
        std::mutex associationMutex_;
    };
}
