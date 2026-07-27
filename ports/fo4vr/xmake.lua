local PROJECT_ROOT = path.absolute(path.join(os.scriptdir(), "..", ".."))
local UL_ROOT = path.join(PROJECT_ROOT, "build", "ultralight-1.4.0")
local UL_INCLUDE = path.join(UL_ROOT, "include")
local UL_SHADER_INCLUDE = path.join(UL_ROOT, "shaders", "hlsl", "bin")
local UL_LIB = path.join(UL_ROOT, "lib")
local UL_BIN = path.join(UL_ROOT, "bin")
local UL_RESOURCES = path.join(UL_ROOT, "resources")
local UL_INSPECTOR = path.join(UL_ROOT, "inspector")
local UL_LICENSE = path.join(UL_ROOT, "license")

local F4VR_COMMON_FRAMEWORK_PATH = os.getenv("F4VR_COMMON_FRAMEWORK_PATH")
if not F4VR_COMMON_FRAMEWORK_PATH or F4VR_COMMON_FRAMEWORK_PATH == "" then
    os.raise(
        "PRISMA_TARGET=vr requires F4VR_COMMON_FRAMEWORK_PATH to point to " ..
        "an ArthurHub F4VR-CommonFramework checkout")
end

local OPENVR_ROOT = path.join(F4VR_COMMON_FRAMEWORK_PATH, "external", "openvr")
local OPENVR_HEADER = path.join(OPENVR_ROOT, "openvr.h")
local OPENVR_LIBRARY = path.join(OPENVR_ROOT, "openvr_api.lib")
if not os.isfile(OPENVR_HEADER) or not os.isfile(OPENVR_LIBRARY) then
    os.raise(
        "F4VR_COMMON_FRAMEWORK_PATH does not contain external/openvr/openvr.h " ..
        "and external/openvr/openvr_api.lib")
end

local function stage_framework(target)
    local version = target:version() or "1.0.0"
    local dist_root = path.join(PROJECT_ROOT, "dist", "PrismaUI_F4_" .. version .. "_VR")
    local plugin_root = path.join(dist_root, "F4SE", "Plugins")
    local framework_root = path.join(dist_root, "PrismaUI_F4")

    if os.exists(dist_root) then
        os.rm(dist_root)
    end
    os.mkdir(plugin_root)
    os.mkdir(framework_root)
    os.cp(target:targetfile(), path.join(plugin_root, "PrismaUI_F4.dll"))
    if os.isfile(target:symbolfile()) then
        os.cp(target:symbolfile(), path.join(plugin_root, "PrismaUI_F4.pdb"))
    end

    local assets = path.join(PROJECT_ROOT, "assets")
    if os.isdir(assets) then
        os.cp(path.join(assets, "*"), framework_root)
    end

    os.cp(UL_BIN, path.join(framework_root, "libs"))
    os.cp(UL_RESOURCES, path.join(framework_root, "resources"))
    os.cp(UL_INSPECTOR, path.join(framework_root, "inspector"))

    local include_root = path.join(framework_root, "include")
    os.mkdir(include_root)
    os.cp(
        path.join(os.scriptdir(), "include", "PrismaUI_F4_API.h"),
        path.join(include_root, "PrismaUI_F4_API.h"))
    os.cp(
        path.join(os.scriptdir(), "include", "PrismaUI_F4VR_API.h"),
        path.join(include_root, "PrismaUI_F4VR_API.h"))

    local license_root = path.join(framework_root, "licenses")
    os.mkdir(license_root)
    local project_license = path.join(PROJECT_ROOT, "LICENSE.md")
    if os.isfile(project_license) then
        os.cp(
            project_license,
            path.join(license_root, "PrismaUI-LICENSE.md"))
    end
    local ultralight_license = path.join(UL_LICENSE, "LICENSE.txt")
    if os.isfile(ultralight_license) then
        os.cp(
            ultralight_license,
            path.join(license_root, "Ultralight-LICENSE.txt"))
    end
    local ultralight_eula = path.join(UL_LICENSE, "EULA.txt")
    if os.isfile(ultralight_eula) then
        os.cp(
            ultralight_eula,
            path.join(license_root, "Ultralight-EULA.txt"))
    end
    local ultralight_notices = path.join(UL_LICENSE, "NOTICES.md")
    if os.isfile(ultralight_notices) then
        os.cp(
            ultralight_notices,
            path.join(license_root, "Ultralight-NOTICES.md"))
    end

    print(
        "FO4VR distribution ready: dist/PrismaUI_F4_" ..
        version ..
        "_VR")
end

target("PrismaUI_F4VR")
    set_kind("shared")
    set_filename("PrismaUI_F4.dll")
    set_version("1.0.0")
    set_symbols("debug")

    on_config(function()
        if not has_config("fallout_f4vr") or
            has_config("fallout_f4") or
            has_config("fallout_f4ng") then
            os.raise(
                "The VR graph must be configured with " ..
                "--fallout_f4=n --fallout_f4ng=n --fallout_f4vr=y")
        end
    end)

    before_build(function()
        import(
            "ValidateQueryContract",
            {
                rootdir = path.join(os.scriptdir(), "tests"),
                alias = "validate_query_contract"
            })
        validate_query_contract(
            path.join(os.scriptdir(), "src", "main.cpp"),
            "provider")
        import(
            "ValidateGpuDriverContract",
            {
                rootdir = path.join(os.scriptdir(), "tests"),
                alias = "validate_gpu_driver_contract"
            })
        validate_gpu_driver_contract("self-test")
        validate_gpu_driver_contract(
            path.join(
                os.scriptdir(),
                "src",
                "PrismaUI",
                "D3D11GpuDriver.cpp"))
    end)

    add_deps("commonlibf4-ng")

    add_includedirs("src", "include", UL_INCLUDE, UL_SHADER_INCLUDE, OPENVR_ROOT)
    add_linkdirs(UL_LIB, OPENVR_ROOT)
    add_files("src/**.cpp", "src/resource.rc")
    set_pcxxheader("src/PCH.h")

    add_links(
        "AppCore",
        "Ultralight",
        "UltralightCore",
        "WebCore",
        "openvr_api",
        "delayimp")
    -- spdlog is propagated publicly by the ArthurHub CommonLibF4VR target.
    add_packages("minhook", "directxtk")
    add_syslinks(
        "bcrypt",
        "comctl32",
        "d3d11",
        "d3dcompiler",
        "dxgi",
        "imm32",
        "ole32",
        "shell32",
        "user32",
        "version")

    add_shflags(
        "/DELAYLOAD:UltralightCore.dll",
        "/DELAYLOAD:WebCore.dll",
        "/DELAYLOAD:Ultralight.dll",
        "/DELAYLOAD:AppCore.dll",
        { force = true })

    add_defines(
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
        "UNICODE",
        "_UNICODE",
        "PRISMAUI_FO4VR=1",
        "PRISMAUI_VERSION_MAJOR=1",
        "PRISMAUI_VERSION_MINOR=0",
        "PRISMAUI_VERSION_PATCH=0")

    if is_plat("windows") then
        add_cxflags(
            "/permissive-",
            "/Zc:preprocessor",
            "/wd4200",
            "/wd4201",
            "/wd4324",
            "/wd4715",
            "/Gy",
            "/Gw",
            "/Zc:inline",
            "/d2ReducedOptimizeHugeFunctions",
            "/EHa",
            "/bigobj",
            "/FS")
        set_policy("build.warning", false)
        add_cxflags("/Zi", { release = true })
        add_ldflags("/DEBUG", "/OPT:REF", "/OPT:ICF", { release = true, force = true })
    end

    after_build(stage_framework)

target("PrismaUI_F4VR_DeterministicTests")
    set_kind("binary")
    set_default(false)
    set_group("tests")
    set_languages("c++23")

    add_includedirs("src", "include")
    add_files(
        "tests/DeterministicTests.cpp",
        "src/PrismaUI/WorldPanelGeometry.cpp")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")

    if is_plat("windows") then
        add_cxflags("/permissive-", "/Zc:preprocessor")
        add_syslinks("user32")
    end

target("PrismaUI-F4VR-Example-Plugin")
    set_kind("shared")
    set_filename("PrismaUI-F4-Example.dll")
    set_version("1.0.0")
    set_symbols("debug")

    add_deps("commonlibf4-ng")
    add_includedirs("example", "include")
    add_files("example/**.cpp")
    set_pcxxheader("example/PCH.h")
    add_defines(
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
        "UNICODE",
        "_UNICODE")

    before_build(function()
        import(
            "ValidateQueryContract",
            {
                rootdir = path.join(os.scriptdir(), "tests"),
                alias = "validate_query_contract"
            })
        validate_query_contract(
            path.join(os.scriptdir(), "example", "main.cpp"),
            "example")
    end)

    if is_plat("windows") then
        add_cxflags(
            "/permissive-",
            "/Zc:preprocessor",
            "/wd4200",
            "/wd4201",
            "/wd4324",
            "/FS")
        add_syslinks(
            "bcrypt",
            "ole32",
            "shell32",
            "user32",
            "version")
    end

    after_build(function(target)
        print(
            "[PrismaUI-F4VR-Example] built to: " ..
            target:targetfile())
    end)
