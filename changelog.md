## v1.8

<details>
<summary><b>Click to expand Version 1.8 Changelog</b></summary>

### Stability & Memory
- Tuned the Ultralight engine configuration to dramatically cut per-view memory usage. JavaScriptCore now sizes its heaps conservatively instead of scaling to full system RAM (override_ram_size capped at 1 GB), the initial JS heaps were shrunk (large 32 MB → 8 MB, small 1 MB → 512 KB), the WebCore resource cache was reduced (64 MB → 32 MB), the back/forward page cache was disabled, and the renderer thread count was capped at 2.
- Resolved a class of out-of-memory crashes in heavy load orders where the framework's memory footprint, alongside many active views, starved the game's Scaleform UI heap (Scaleform allocation failures leading to a CTD).

### Reliability Watchdog
- Added a health monitor for every hosted plugin view. Every 30 seconds it logs a summary (view count, visible/hidden/quarantined, faulting views, process working set) plus a detail line per problem view, so a misbehaving plugin is easy to identify from the log.
- Views that repeatedly fail crash-recovery are now quarantined — isolated from the renderer so a single broken view can no longer destabilize the framework or the game. The view handle stays valid, so the owning plugin keeps working and can recreate it.
- Under high process-memory pressure, the render buffers (CPU pixel buffer and GPU texture) of hidden views are reclaimed automatically and regenerated losslessly when the view is shown again.

### Input & Interaction
- Added click-through for transparent regions of a focused view: a click over an empty (transparent) area now passes to the game — for example to interact with a 3D model rendered behind the interface — while a drag that began over real UI content stays captured until release.
- Fixed mouse input capture by retaining the focus menu's modal flag; without it, the menu beneath could steal mouse input through the game's device layer.

</details>

---

## v1.7

<details>
<summary><b>Click to expand Version 1.7 Changelog</b></summary>

### Framework Improvements
- Added new NotificationSystem for displaying in-game notifications.
- Enhanced InputHandler functionality and reliability.
- Improved ViewManager architecture and view lifecycle handling.
- Updated communication layer between C++, JavaScript, and game systems.
- Finalized transition to xmake as the sole build system.
- Removed legacy CMake support.

### Build & Deployment
- Consolidated build.bat, deploy.bat, setup.bat, and verify-deploy.bat into a single build-and-deploy.bat script.
- Added automatic GitHub version checking for SDK dependencies.
- Added automatic Ultralight SDK extraction during setup/build.
- Improved deployment error handling and validation.
- Finalized xmake project configuration.

### Example Plugin Improvements
- Fixed Event Log copy functionality by sending the full log through the C++ bridge instead of relying on the browser Clipboard API.
- Fixed Copy button state reset issues.
- Added C++ → JavaScript callback support for sendDataToF4SE.
- Event Log now displays framework callback messages.
- Enabled scrolling for Event Log container via overflow-y: auto.
- Improved optional API guards and error reporting.
- Added additional logging throughout the example plugin.

### Papyrus Bridge & Property System
- Updated FormID resolution to use TESDataHandler for plugin-aware lookups.
- Property reads now correctly account for plugin load order offsets.
- Property writes now correctly account for plugin load order offsets.
- Removed unsupported property write functionality from the example plugin.
- Clarified Papyrus Bridge limitations and supported workflows.

### Documentation
- Added a comprehensive Tutorial tab featuring 9 guided sections covering framework basics, view creation, communication APIs, event handling, Papyrus integration, notifications, common patterns, best practices, and troubleshooting.
- Added:
  - CHANGELOG.md
  - limitations.md
  - papyrus-bridge.md
  - translations.md
  - modern-frameworks.md
- Expanded documentation for:
  - View lifecycle behavior
  - HTML view support
  - JavaScript support matrix
  - API reference examples

### New Assets
- Added notification-banner.html component.
- Added new example and reference documentation assets.

### Refactoring
- Cleaned up API comments throughout the framework.
- Improved code organization and maintainability.
- General documentation and comment consistency improvements.

### Breaking Changes
- Removed CMake support.
- xmake is now the only supported build system.

</details>

---

## Version 1.6

<details>
<summary><b>Click to expand Version 1.6 Changelog</b></summary>

### Papyrus Bridge — `window.prisma`

<details>
<summary><b>💡 What is the Papyrus Bridge? (Click to expand)</b></summary>

Up until now, creating an interface with PrismaUI required writing a dedicated C++ plugin, managing view lifetimes, and handling multi-threading. The Papyrus Bridge completely removes that barrier for Papyrus modders.

PrismaUI now automatically injects a `window.prisma` object into every single HTML view out of the box. No C++ required, no compilers, and no extra configuration. You can now read and write live game states directly from JavaScript.

**The Workflow:**
1. Declare your Auto properties on a Quest script like you normally would.
2. Place your HTML/JS assets inside `Data/PrismaUI_F4/views/`.
3. Call `prisma.getProperty` or `prisma.setProperty` directly from your frontend code.

**Quick JavaScript Examples:**

```js
// Read and write a live Papyrus Auto property on a quest script
const damage = await prisma.getProperty("MyMod.esp", "800", "MyMod_Quest", "DamageScale");
prisma.setProperty("MyMod.esp", "800", "MyMod_Quest", "DamageScale", 2.5);

// Read and write a TESGlobal value
const val = await prisma.getGlobal("MyMod.esp", "801");
prisma.setGlobal("MyMod.esp", "801", 3.0);
```

#### Best Practices & Limitations:

- **Pull-Based Architecture:** The system is currently pull-based. If Papyrus updates a property internally, the UI won't update automatically. For standard config menus, simply poll the values using a standard JavaScript `setInterval`. A push/subscribe architecture is planned for a future release.
- **Supported Data Types:** Version 1 matches `float`, `int`, and `bool` properties. Strings and arrays are not yet supported.
- **Host Selection:** Always host your target script properties on a persistent **Quest form** rather than a cell-local reference. If the script host unloads from game memory, the bridge will safely return `null` and log a warning to prevent crashes.
- **VM Availability:** Ensure reads are executed after the Papyrus VM is fully initialized (`kPostLoadGame`). Menus invoked mid-game naturally bypass this constraint.

**New Feature Highlights:**

- Implemented the "Direct Papyrus VM Integration": every HTML view now has `window.prisma` injected automatically before `OnDomReadyCallback` fires, removing the need for C++ wrappers in consumer plugins.
- Added `getGlobal(esp, formId)` and `setGlobal(esp, formId, value)` to read/write `TESGlobal` float values.
- Added `getProperty(esp, formId, scriptName, propName)` and `setProperty(...)` to read/write Papyrus Auto properties (float, int, bool) via handle-scoped VM lookup.
- Architecture: promise-based async reads; writes are fire-and-forget. All reads return `null` on failure (e.g., missing plugin, wrong form, VM not ready) and will not throw errors.
- Threading: JavaScript handlers execute on the Ultralight thread, while game data access is safely dispatched via `AddTask`.
- Documentation: Updated `api-reference.md` and `getting-started.md` to reflect the new integration.

</details>

### Example Plugin — Full API Guide HTML

- Rewrote `example-f4se-plugin/view/index.html` from a basic demo into a 3-tab interactive guide:
  - **Papyrus Bridge tab:** Live `getGlobal`/`setGlobal`/`getProperty`/`setProperty` testing with inputs, result display, and null-return references.
  - **C++ Bridge tab:** Documents `Invoke`, `InteropCall`, `BindUIEvent`, and `RegisterJSListener` with a live focus badge demo and send-to-plugin input.
  - **Event Log tab:** Features a unified, color-coded bridge activity log and a `console.log` test button.


### Migration to NewCommonLib (xmake)

Both the example plugin and the core `PrismaUI_F4.dll` have migrated from `cmake`/`CommonLibF4_OG` to the new xmake-based `NewCommonLib`.

#### Example Plugin (`example-f4se-plugin`)

- `xmake.lua`: includes `../NewCommonLib`, implements the `commonlibf4.plugin` rule, and removes dependencies on `boost`, `mmio`, and `fmt`.
- `PCH.h`: removed `spdlog` aliases; logging is now handled via `REX::CRITICAL`/`INFO`.
- `main.cpp`: uses `F4SE_PLUGIN_LOAD` and `F4SE::Init()` to replace manual `F4SEPlugin_Query` and `spdlog` setup.
- `PrismaUI_F4_API.h`: replaced removed `F4SE::WinAPI::` calls with standard `::GetModuleHandleW` and `::GetProcAddress`.
- Fixes: pinned `spdlog` to `v1.16.0` to prevent duplicate `add_requires` errors; switched `REX::ERROR` to `REX::CRITICAL` to avoid Windows `ERROR` macro collisions.

#### Core PrismaUI_F4 DLL

- Build system: replaced `CMakeLists.txt` with a new `xmake.lua`. Pulls `minhook` and `directxtk` from the xmake registry, utilizes the existing extracted Ultralight SDK, and preserves all compiler flags from `CompilerFlags.cmake`.
- `PCH.h`: maps `namespace logger = spdlog` to replace the removed `F4SE::log`.
- `main.cpp`: replaced `F4SEPlugin_Query` and manual log setups with `F4SE_PLUGIN_LOAD` and `InitInfo{logName, trampoline}`.
- `Hooks.h/.cpp`: replaced `<dxgi.h>` SDK types with `REX::W32::IDXGISwapChain` and `DXGI_FORMAT`.
- Direct3D management: updated `Hooks.cpp`, `Core.cpp`, and `ConflictChecker.cpp` to use the `GetRendererData()` free function instead of the removed `RendererData::GetSingleton()`.
- `Core.cpp`: added `reinterpret_cast` bridges to map `REX::W32` COM types to SDK COM types for compatibility with D3D11 and DirectXTK.
- Runtime: switched the runtime library to `/MD` to align with precompiled xmake DirectXTK and `commonlibf4` packages.

</details>

---

## Version 1.5

<details>
<summary><b>Click to expand Version 1.5 Changelog</b></summary>

### Security

- **Network Sandbox:** Added an automatic network sandbox applied to all views across all API versions (V1–V4).
- **API Restrictions:** Outbound network APIs (`fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, `Worker`, etc.) are now blocked in every view before page scripts execute and cannot be overridden.
- **`CreateView` Restrictions:** Modified `CreateView` to reject external URLs (`http://`, `https://`). The system now exclusively accepts local paths under `Data/PrismaUI_F4/views/`.
- **Navigation Restrictions:** Blocked `window.open()` and all forms of external navigation.
- **Logging:** All blocked network or navigation attempts are now logged to `PrismaUI_F4.log`.

> [!NOTE]
> There are no changes to the public API. Plugins built against V1–V4 will receive this sandbox automatically with no code changes required.

</details>

---

## Version 1.4

<details>
<summary><b>Click to expand Version 1.4 Changelog</b></summary>

### Documentation Updates

- Updated the API reference, examples, getting-started guide, and view-lifecycle documentation for V4.
- Added `IVPrismaUI4` and `BindUIEvent` documentation across the reference materials.
- Documented `RegisterTranslations` (V3) in both the API reference and examples.
- Updated all development examples to use `IVPrismaUI4` instead of `IVPrismaUI3`.
- Added an "Updating Your Plugin" migration section to `api-reference.md`.
- Updated the DOM Ready callback section to include implementation guidance comparing `BindUIEvent` vs `RegisterJSListener`.

</details>

---

## Version 1.3

<details>
<summary><b>Click to expand Version 1.3 Changelog</b></summary>

### Runtime Compatibility

- **Next-Gen F4SE Support:** Added the `g_pluginVersionData` export for F4SE 0.7.1+. Plugins now register through `PluginVersionData` alongside or in place of the legacy `F4SEPlugin_Query` path, allowing F4SE to validate the plugin prior to loading.
- **Address Library:** Added `UsesAddressLibrary(true)` support for automatic `.bin` lookup based on the game's runtime version string (e.g., `version-1-11-221-0.bin`).
- **CommonLibF4 Layouts:** Added an `IsLayoutDependent(true)` declaration for plugins utilizing `RE::` types from CommonLibF4.
- **Version Management:** An empty `CompatibleVersions` array now signals compatibility with all future runtime versions, removing the need for manual plugin updates after minor game patches.
- **Backwards Compatibility:** Legacy `F4SEPlugin_Query` and `F4SEPlugin_Load` exports remain intact; pre-0.7.1 versions of F4SE will ignore `g_pluginVersionData` and fall back to the classic loading path.
- **Validation:** Game versions below `1.10.162` are now explicitly rejected in `F4SEPlugin_Query` due to incompatible pre-Next-Gen hooks.
- **Address Bins:** The `AddressBins/` directory now ships with `.bin` files covering runtimes back to `1.10.130` out of the box.

### Core Framework

- **Rendering Engine:** Integrated an Ultralight-powered HTML/CSS/JS rendering layer into DXGI `Present` and `ResizeBuffers` using MinHook vtable patching.
- **API Exporting:** Added versioned plugin API exports (`IVPrismaUI1`, `IVPrismaUI2`, `IVPrismaUI3`) exposed via `RequestPluginAPI` using `GetProcAddress`, eliminating link-time dependencies for consumer plugins.
- **View Lifecycle:** Added complete view lifecycle management functions:
  - `CreateView` / `Destroy`
  - `Show` / `Hide`
  - `Focus` / `Unfocus`
  - `IsValid` / `SetOrder`

- **JavaScript Interop:**
  - Added `InteropCall` for high-frequency C++ to JavaScript communication.
  - Added `Invoke` for one-shot JavaScript execution with callback support.
  - Added `RegisterJSListener` to expose named window-level JavaScript functions directly to C++.

- **Console Logging:** Added `RegisterConsoleCallback` (V2) to pipe JavaScript console messages (`log`, `warn`, `error`, `debug`, `info`) directly into the C++ log.
- **Localization:** Added `RegisterTranslations` (V3), which automatically loads translation files from `Data\\Interface\\Translations\\<plugin>_<lang>.txt` and injects `window.L10N` and `window.t()` into every view on page load.
- **Debugging Tools:** Added inspector view support via `CreateInspectorView`, `SetInspectorVisibility`, and `SetInspectorBounds`.

</details>

---

## Version 1.2

<details>
<summary><b>Click to expand Version 1.2 Changelog</b></summary>

### PrismaDesigner Refactor

- **Canvas Refactor:** Overhauled canvas item handling; inserted elements are now managed through an internal reference-based system.
- **Stability:** General stability and architectural cleanup across the designer pipeline to establish foundational work for future editor and layout features.

</details>

---

## Version 1.1

<details>
<summary><b>Click to expand Version 1.1 Changelog</b></summary>

### Bug Fixes

- **Windows 10 Crashes:** Resolved crashing issues on Windows 10 by downgrading Ultralight from the `1.4.1-dev` pre-release build to the `1.4.0` stable release. The pre-release build contained a heap corruption issue in `WebCore.dll` that caused random crashes within minutes across unrelated DLLs (such as `usvfs`, `ConsoleAutocomplete`, and `Qt6Network`). Windows 11 systems were unaffected.
- **Hook Conflicts:** Fixed a hook conflict caused by enabling all MinHook hooks globally (`MH_ALL_HOOKS`) during startup. PrismaUI now selectively enables only its own Direct3D hooks (`Present` and `ResizeBuffers`).

### Improvements

- **Render Loop:** Added the required `RefreshDisplay()` call to the render loop per Ultralight `1.4.0` specifications. CSS animations, scrolling, and `requestAnimationFrame()` now function correctly.

### PrismaDesigner Updates

- **Typography:** Added the *Overseer* Fallout-style font to the text element dropdown. Exported HTML automatically embeds this font as base64 so that exported views are entirely self-contained.
- **Heading Element:** Added a new Heading element to the palette. Headings default to the *Overseer* font, a 48px font size, and an Amber color palette, allowing titles to be dropped directly into layouts.

> [!TIP]
> If you experienced crashes on Windows 10, update `PrismaUI_F4` and replace the binaries inside `PrismaUI_F4\libs\` with the DLLs included in this release.

</details>
