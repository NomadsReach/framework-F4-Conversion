from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8-sig")


def require(text: str, token: str, label: str) -> None:
    if token not in text:
        raise AssertionError(f"missing {label}: {token}")


def forbid(text: str, token: str, label: str) -> None:
    if token in text:
        raise AssertionError(f"forbidden {label}: {token}")


def main() -> None:
    main_cpp = read("src/main.cpp")
    core = read("src/PrismaUI/Core.cpp")
    view_manager = read("src/PrismaUI/ViewManager.cpp")
    operations = read("src/PrismaUI/ViewOperationQueue.cpp")
    renderer = read("src/PrismaUI/ViewRenderer.cpp")
    input_handler = read("src/PrismaUI/InputHandler.cpp")
    api = read("src/API/API.cpp")
    papyrus = read("src/PrismaUI/PapyrusBridge.cpp")
    hooks = read("src/Hooks/Hooks.cpp")
    focus_menu = read("src/Menus/FocusMenu/FocusMenu.cpp")

    require(main_cpp, "GameThreadDispatcher::CaptureCurrentThread();", "game-thread capture")
    require(core, "RenderRetirement::Drain();", "render retirement drain")
    require(view_manager, "GameThreadDispatcher::DropView(viewId);", "destroy tombstone")
    require(view_manager, "isDestroying.compare_exchange_strong", "logical destroy")
    require(operations, "SingleThreadExecutor::Priority::HIGH", "lifecycle priority")
    require(renderer, "mapped.RowPitch < rowBytes", "D3D row bound")
    require(api, "GameThreadDispatcher::Dispatch(", "verified API callback dispatch")
    require(papyrus, "GameThreadDispatcher::Dispatch(", "verified Papyrus dispatch")
    require(hooks, "RetryInputInstall();", "input install retry")
    require(focus_menu, "void FocusMenu::AdvanceMovie", "focus menu advance")

    forbid(renderer, "CursorMenu", "Present-side Scaleform cursor access")
    forbid(view_manager, "textureView->Release()", "off-thread view SRV release")
    forbid(view_manager, "texture->Release()", "off-thread view texture release")
    forbid(input_handler, "resize(utf8Length - 1)", "clipboard overflow pattern")
    forbid(input_handler, "g_eventQueue.empty()", "unlocked input queue read")
    forbid(papyrus, "GetTaskInterface", "unverified Papyrus game dispatch")
    forbid(hooks, "MH_ALL_HOOKS", "global MinHook mutation")


if __name__ == "__main__":
    main()
