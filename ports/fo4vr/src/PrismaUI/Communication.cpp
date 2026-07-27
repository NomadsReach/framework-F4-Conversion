#include "PCH.h"

#include "PrismaUI/Communication.h"

#include "PrismaUI/ViewManager.h"

namespace PrismaUI::Communication
{
    namespace
    {
        constexpr auto nativeBindingProperty =
            "__prismaUI_nativeCallbackBinding";
        constexpr std::size_t maximumCallbackArgumentBytes =
            1024u * 1024u;

        struct NativeCallbackBinding
        {
            Core::PrismaViewId viewId = 0;
            std::string name;
            ultralight::View* ownerView = nullptr;
            JSGlobalContextRef ownerContext = nullptr;
        };

        void FinalizeBinding(JSObjectRef object) noexcept
        {
            delete static_cast<NativeCallbackBinding*>(
                JSObjectGetPrivate(object));
        }

        [[nodiscard]] JSClassRef BindingClass() noexcept
        {
            static const auto value = []() noexcept {
                JSClassDefinition definition{};
                definition.className =
                    "PrismaUINativeCallbackBinding";
                definition.finalize = FinalizeBinding;
                return JSClassCreate(&definition);
            }();
            return value;
        }

        [[nodiscard]] JSStringRef BindingProperty() noexcept
        {
            static const auto value =
                JSStringCreateWithUTF8CString(nativeBindingProperty);
            return value;
        }

        class ProtectedValue final
        {
        public:
            ProtectedValue(
                JSContextRef context,
                JSValueRef value) noexcept :
                context_(context),
                value_(value)
            {
                if (context_ && value_) {
                    JSValueProtect(context_, value_);
                }
            }

            ~ProtectedValue() noexcept
            {
                if (context_ && value_) {
                    JSValueUnprotect(context_, value_);
                }
            }

            ProtectedValue(const ProtectedValue&) = delete;
            ProtectedValue& operator=(const ProtectedValue&) = delete;

        private:
            JSContextRef context_ = nullptr;
            JSValueRef value_ = nullptr;
        };

        void SetException(
            JSContextRef context,
            JSValueRef* exception,
            const char* message) noexcept
        {
            if (!context || !exception || !message) {
                return;
            }
            const auto text =
                JSStringCreateWithUTF8CString(message);
            if (!text) {
                return;
            }
            *exception = JSValueMakeString(context, text);
            JSStringRelease(text);
        }

        [[nodiscard]] JSValueRef Undefined(
            JSContextRef context) noexcept
        {
            return context ? JSValueMakeUndefined(context) : nullptr;
        }

        [[nodiscard]] bool BindOne(
            JSContextRef context,
            JSObjectRef globalObject,
            ultralight::View* ownerView,
            const Core::JSCallbackData& callback)
        {
            const auto bindingClass = BindingClass();
            const auto bindingProperty = BindingProperty();
            if (!context ||
                !globalObject ||
                !ownerView ||
                !bindingClass ||
                !bindingProperty ||
                callback.name.empty()) {
                return false;
            }

            auto binding = std::make_unique<NativeCallbackBinding>();
            binding->viewId = callback.viewId;
            binding->name = callback.name;
            binding->ownerView = ownerView;
            binding->ownerContext =
                JSContextGetGlobalContext(context);
            if (!binding->ownerContext) {
                return false;
            }

            const auto bindingObject = JSObjectMake(
                context,
                bindingClass,
                binding.get());
            if (!bindingObject) {
                return false;
            }
            binding.release();
            const ProtectedValue protectedBinding(
                context,
                bindingObject);

            const auto functionName =
                JSStringCreateWithUTF8CString(callback.name.c_str());
            if (!functionName) {
                return false;
            }

            const auto functionObject =
                JSObjectMakeFunctionWithCallback(
                    context,
                    functionName,
                    InvokeCppCallback);
            if (!functionObject) {
                JSStringRelease(functionName);
                return false;
            }
            const ProtectedValue protectedFunction(
                context,
                functionObject);

            JSValueRef propertyException = nullptr;
            constexpr auto attributes =
                static_cast<JSPropertyAttributes>(
                    kJSPropertyAttributeReadOnly |
                    kJSPropertyAttributeDontEnum |
                    kJSPropertyAttributeDontDelete);

            const auto originalPrototype =
                JSObjectGetPrototype(context, functionObject);
            const ProtectedValue protectedPrototype(
                context,
                originalPrototype);
            JSObjectSetPrototype(
                context,
                functionObject,
                JSValueMakeNull(context));
            if (!JSValueIsNull(
                    context,
                    JSObjectGetPrototype(context, functionObject))) {
                JSStringRelease(functionName);
                return false;
            }

            JSObjectSetProperty(
                context,
                functionObject,
                bindingProperty,
                bindingObject,
                attributes,
                &propertyException);
            JSObjectSetPrototype(
                context,
                functionObject,
                originalPrototype);
            if (propertyException) {
                JSStringRelease(functionName);
                return false;
            }

            const auto attachedBinding = JSObjectGetProperty(
                context,
                functionObject,
                bindingProperty,
                &propertyException);
            if (propertyException ||
                attachedBinding != bindingObject ||
                JSObjectGetPrototype(context, functionObject) !=
                    originalPrototype) {
                JSStringRelease(functionName);
                return false;
            }

            JSObjectSetProperty(
                context,
                globalObject,
                functionName,
                functionObject,
                kJSPropertyAttributeNone,
                &propertyException);
            JSStringRelease(functionName);
            return propertyException == nullptr;
        }

        [[nodiscard]] NativeCallbackBinding* FindBinding(
            JSContextRef context,
            JSObjectRef function) noexcept
        {
            if (!context || !function) {
                return nullptr;
            }

            JSValueRef propertyException = nullptr;
            const auto bindingValue = JSObjectGetProperty(
                context,
                function,
                BindingProperty(),
                &propertyException);
            if (propertyException ||
                !bindingValue ||
                !JSValueIsObjectOfClass(
                    context,
                    bindingValue,
                    BindingClass())) {
                return nullptr;
            }

            const auto bindingObject = JSValueToObject(
                context,
                bindingValue,
                &propertyException);
            if (propertyException || !bindingObject) {
                return nullptr;
            }
            return static_cast<NativeCallbackBinding*>(
                JSObjectGetPrivate(bindingObject));
        }

        [[nodiscard]] bool Post(
            SingleThreadExecutor::Priority priority,
            std::function<void()> operation) noexcept
        {
            auto& worker = Core::GetRuntime().worker;
            const auto accepted =
                worker.TryPost(priority, std::move(operation));
            if (!accepted) {
                logger::error(
                    "PrismaUI worker rejected a communication operation");
            }
            return accepted;
        }

        [[nodiscard]] std::string ExceptionText(
            JSContextRef context,
            JSValueRef exception) noexcept
        {
            if (!context || !exception) {
                return "<unknown JavaScript exception>";
            }
            try {
                const auto string =
                    JSValueToStringCopy(context, exception, nullptr);
                if (!string) {
                    return "<unprintable JavaScript exception>";
                }
                const auto maximum =
                    JSStringGetMaximumUTF8CStringSize(string);
                const auto bounded =
                    (std::min)(maximum, std::size_t{4096});
                std::vector<char> buffer(bounded);
                if (!buffer.empty()) {
                    JSStringGetUTF8CString(
                        string,
                        buffer.data(),
                        buffer.size());
                }
                JSStringRelease(string);
                return buffer.empty() ?
                    "<empty JavaScript exception>" :
                    std::string(buffer.data());
            } catch (...) {
                return "<failed to format JavaScript exception>";
            }
        }
    }

    void Invoke(
        Core::PrismaViewId viewId,
        const ultralight::String& script,
        std::function<void(std::string)> callback) noexcept
    {
        if (!ViewManager::IsValid(viewId)) {
            if (callback) {
                callback({});
            }
            return;
        }

        auto rejectionCallback = callback;
        const auto accepted = Post(
            SingleThreadExecutor::Priority::LOW,
            [viewId, script, callback = std::move(callback)]() mutable {
                std::string result;
                const auto view = Core::FindView(viewId);
                if (view &&
                    !view->destroying.load(std::memory_order_acquire) &&
                    view->ultralightView) {
                    try {
                        const auto value =
                            view->ultralightView->EvaluateScript(
                                script,
                                nullptr);
                        const auto& utf8 = value.utf8();
                        result.assign(utf8.data(), utf8.length());
                    } catch (const std::exception& exception) {
                        logger::error(
                            "View [{}] script evaluation failed: {}",
                            viewId,
                            exception.what());
                    } catch (...) {
                        logger::error(
                            "View [{}] script evaluation failed",
                            viewId);
                    }
                }
                if (callback) {
                    callback(std::move(result));
                }
            });
        if (!accepted && rejectionCallback) {
            rejectionCallback({});
        }
    }

    void RegisterJSListener(
        Core::PrismaViewId viewId,
        const std::string& functionName,
        Core::SimpleJSCallback callback) noexcept
    {
        if (!ViewManager::IsValid(viewId) ||
            functionName.empty() ||
            functionName.size() > 256 ||
            !callback) {
            return;
        }

        try {
            auto& runtime = Core::GetRuntime();
            {
                std::lock_guard lock(runtime.jsCallbacksMutex);
                runtime.jsCallbacks[
                    std::make_pair(viewId, functionName)] = {
                        .viewId = viewId,
                        .name = functionName,
                        .callback = std::move(callback)
                    };
            }
            (void)Post(
                SingleThreadExecutor::Priority::LOW,
                [viewId] { (void)BindJSCallbacks(viewId); });
        } catch (...) {
            logger::error(
                "View [{}] could not register JavaScript listener '{}'",
                viewId,
                functionName);
        }
    }

    bool BindJSCallbacks(Core::PrismaViewId viewId) noexcept
    {
        auto& runtime = Core::GetRuntime();
        if (!runtime.worker.IsWorkerThread()) {
            try {
                return runtime.worker
                    .submit_with_priority(
                        SingleThreadExecutor::Priority::HIGH,
                        [viewId] { return BindJSCallbacks(viewId); })
                    .get();
            } catch (...) {
                return false;
            }
        }

        try {
            const auto view = Core::FindView(viewId);
            if (!view ||
                view->destroying.load(std::memory_order_acquire) ||
                !view->ultralightView) {
                return false;
            }

            std::vector<Core::JSCallbackData> callbacks;
            {
                std::lock_guard lock(runtime.jsCallbacksMutex);
                for (const auto& [key, callback] :
                     runtime.jsCallbacks) {
                    if (key.first == viewId) {
                        callbacks.push_back(callback);
                    }
                }
            }
            if (callbacks.empty()) {
                return true;
            }
            if (!view->windowObjectReady.load(
                    std::memory_order_acquire)) {
                return false;
            }

            const auto lockedContext =
                view->ultralightView->LockJSContext();
            if (!lockedContext || !lockedContext->ctx()) {
                return false;
            }
            const auto context = lockedContext->ctx();
            const auto global = JSContextGetGlobalObject(context);

            bool allBound = true;
            for (const auto& callback : callbacks) {
                if (!BindOne(
                        context,
                        global,
                        view->ultralightView.get(),
                        callback)) {
                    allBound = false;
                    logger::error(
                        "View [{}] failed to bind JavaScript callback '{}'",
                        viewId,
                        callback.name);
                }
            }
            return allBound;
        } catch (...) {
            return false;
        }
    }

    void InteropCall(
        Core::PrismaViewId viewId,
        const std::string& functionName,
        const std::string& argument) noexcept
    {
        if (!ViewManager::IsValid(viewId) ||
            functionName.empty() ||
            functionName.size() > 256) {
            return;
        }

        (void)Post(
            SingleThreadExecutor::Priority::LOW,
            [viewId, functionName, argument] {
                const auto view = Core::FindView(viewId);
                if (!view ||
                    view->destroying.load(std::memory_order_acquire) ||
                    !view->ultralightView) {
                    return;
                }

                const auto lockedContext =
                    view->ultralightView->LockJSContext();
                if (!lockedContext || !lockedContext->ctx()) {
                    return;
                }
                const auto context = lockedContext->ctx();
                const auto global =
                    JSContextGetGlobalObject(context);
                JSValueRef exception = nullptr;

                const JSRetainPtr<JSStringRef> name =
                    adopt(JSStringCreateWithUTF8CString(
                        functionName.c_str()));
                const auto value = JSObjectGetProperty(
                    context,
                    global,
                    name.get(),
                    &exception);
                if (exception) {
                    logger::error(
                        "View [{}] could not resolve '{}': {}",
                        viewId,
                        functionName,
                        ExceptionText(context, exception));
                    return;
                }
                if (!JSValueIsObject(context, value)) {
                    return;
                }

                const auto function =
                    JSValueToObject(context, value, nullptr);
                if (!function ||
                    !JSObjectIsFunction(context, function)) {
                    return;
                }

                const JSRetainPtr<JSStringRef> argumentString =
                    adopt(JSStringCreateWithUTF8CString(
                        argument.c_str()));
                const JSValueRef arguments[]{
                    JSValueMakeString(
                        context,
                        argumentString.get())
                };
                JSObjectCallAsFunction(
                    context,
                    function,
                    global,
                    1,
                    arguments,
                    &exception);
                if (exception) {
                    logger::error(
                        "View [{}] JavaScript call '{}' failed: {}",
                        viewId,
                        functionName,
                        ExceptionText(context, exception));
                }
            });
    }

    JSValueRef InvokeCppCallback(
        JSContextRef context,
        JSObjectRef function,
        [[maybe_unused]] JSObjectRef thisObject,
        std::size_t argumentCount,
        const JSValueRef arguments[],
        JSValueRef* exception) noexcept
    {
        try {
            auto& runtime = Core::GetRuntime();
            if (!context ||
                !function ||
                !runtime.worker.IsWorkerThread()) {
                SetException(
                    context,
                    exception,
                    "Native PrismaUI callback rejected");
                return Undefined(context);
            }

            const auto* binding =
                FindBinding(context, function);
            if (!binding ||
                binding->viewId == 0 ||
                !binding->ownerView ||
                !binding->ownerContext ||
                JSContextGetGlobalContext(context) !=
                    binding->ownerContext) {
                SetException(
                    context,
                    exception,
                    "Native PrismaUI callback identity is invalid");
                return Undefined(context);
            }

            const auto view = Core::FindView(binding->viewId);
            if (!view ||
                view->destroying.load(std::memory_order_acquire) ||
                !view->loadingFinished.load(
                    std::memory_order_acquire) ||
                !view->ultralightView ||
                view->ultralightView.get() != binding->ownerView) {
                SetException(
                    context,
                    exception,
                    "Native PrismaUI callback view is retired");
                return Undefined(context);
            }

            const auto liveContext =
                view->ultralightView->LockJSContext();
            if (!liveContext ||
                !liveContext->ctx() ||
                JSContextGetGlobalContext(liveContext->ctx()) !=
                    binding->ownerContext) {
                SetException(
                    context,
                    exception,
                    "Native PrismaUI callback context is stale");
                return Undefined(context);
            }

            std::string argument;
            if (argumentCount > 0) {
                if (!arguments) {
                    SetException(
                        context,
                        exception,
                        "Native PrismaUI callback arguments are invalid");
                    return Undefined(context);
                }

                JSValueRef conversionException = nullptr;
                const JSRetainPtr<JSStringRef> converted =
                    adopt(JSValueToStringCopy(
                        context,
                        arguments[0],
                        &conversionException));
                if (conversionException || !converted) {
                    if (exception && conversionException) {
                        *exception = conversionException;
                    }
                    return Undefined(context);
                }

                const auto maximum =
                    JSStringGetMaximumUTF8CStringSize(
                        converted.get());
                if (maximum > maximumCallbackArgumentBytes) {
                    SetException(
                        context,
                        exception,
                        "Native PrismaUI callback argument is too large");
                    return Undefined(context);
                }
                std::vector<char> buffer(maximum);
                if (!buffer.empty()) {
                    JSStringGetUTF8CString(
                        converted.get(),
                        buffer.data(),
                        buffer.size());
                    argument.assign(buffer.data());
                }
            }

            Core::SimpleJSCallback callback;
            {
                std::lock_guard lock(runtime.jsCallbacksMutex);
                const auto iterator = runtime.jsCallbacks.find(
                    std::make_pair(
                        binding->viewId,
                        binding->name));
                if (iterator != runtime.jsCallbacks.end()) {
                    callback = iterator->second.callback;
                }
            }

            if (!callback ||
                view->destroying.load(std::memory_order_acquire)) {
                SetException(
                    context,
                    exception,
                    "Native PrismaUI callback is no longer registered");
                return Undefined(context);
            }

            try {
                callback(std::move(argument));
            } catch (...) {
                SetException(
                    context,
                    exception,
                    "Native PrismaUI callback failed");
            }
            return Undefined(context);
        } catch (...) {
            SetException(
                context,
                exception,
                "Native PrismaUI callback failed");
            return Undefined(context);
        }
    }
}
