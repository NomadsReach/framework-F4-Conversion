#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/String.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

namespace PrismaUI::Core {
typedef uint64_t PrismaViewId;
}

namespace PrismaUI::Listeners {
using namespace ultralight;

class MyLoadListener : public LoadListener {
public:
    explicit MyLoadListener(Core::PrismaViewId id);
    ~MyLoadListener() override;

    void OnBeginLoading(View* caller, uint64_t frameId, bool isMainFrame, const String& url) override;
    void OnFinishLoading(View* caller, uint64_t frameId, bool isMainFrame, const String& url) override;
    void OnFailLoading(View* caller, uint64_t frameId, bool isMainFrame, const String& url,
                       const String& description, const String& errorDomain, int errorCode) override;
    void OnWindowObjectReady(View* caller, uint64_t frameId, bool isMainFrame, const String& url) override;
    void OnDOMReady(View* caller, uint64_t frameId, bool isMainFrame, const String& url) override;

private:
    Core::PrismaViewId viewId_;
};

class MyViewListener : public ViewListener {
public:
    explicit MyViewListener(Core::PrismaViewId id);
    ~MyViewListener() override;

    void OnAddConsoleMessage(View* caller, const ConsoleMessage& message) override;
    RefPtr<View> OnCreateChildView(View* caller, const String& openerUrl, const String& targetUrl,
                                   bool isPopup, const IntRect& popupRect) override;
    RefPtr<View> OnCreateInspectorView(View* caller, bool isLocal, const String& inspectedUrl) override;

private:
    Core::PrismaViewId viewId_;
};

class MyUltralightLogger : public Logger {
public:
    ~MyUltralightLogger() override;
    void LogMessage(LogLevel logLevel, const String& message) override;
};

}
