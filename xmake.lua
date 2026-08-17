-- PrismaUI_F4 / xmake.lua
--
-- Runtime-specific build supporting Fallout 4 Original, Next Gen, and VR.
--
-- Requirements:
--   1. CommonLibF4 submodule at lib/commonlibf4 (initialized via git submodule update --recursive)
--   2. CommonLibF4VR submodule at lib/commonlibf4vr for PRISMA_TARGET=vr
--   3. Ultralight SDK 1.4.0 auto-extracted to build/ultralight-1.4.0/ by build tools
--   4. ArthurHub F4VR-CommonFramework path in F4VR_COMMON_FRAMEWORK_PATH for VR
--
-- PRISMA_TARGET accepts og, ng, or vr and defaults to ng.

set_xmakever("3.0.0")
set_project("PrismaUI_F4")
set_version("1.0.0")
set_languages("c++23")

add_rules("mode.release", "mode.releasedbg", "mode.debug")

-- minhook + DirectXTK from xmake package registry
add_requires("minhook")
add_requires("directxtk")

local PRISMA_TARGET = string.lower(os.getenv("PRISMA_TARGET") or "ng")
if PRISMA_TARGET ~= "og" and PRISMA_TARGET ~= "ng" and PRISMA_TARGET ~= "vr" then
    os.raise("PRISMA_TARGET must be one of: og, ng, or vr")
end

if PRISMA_TARGET == "vr" then
    includes("lib/commonlibf4vr")
else
    -- The flat CommonLib target propagates this exact spdlog configuration.
    add_requires("spdlog v1.16.0", { configs = { header_only = false, wchar = true, std_format = true } })
    -- CommonLibF4 supports the flat OG and NG runtimes in one DLL.
    includes("lib/commonlibf4")
end

-- Ultralight 1.4.0 SDK
-- Auto-extracted to build/ultralight-1.4.0/ by build tools if missing
local UL_ROOT      = path.join(os.scriptdir(), "build", "ultralight-1.4.0")
local UL_INCLUDE   = UL_ROOT .. "/include"
local UL_LIB       = UL_ROOT .. "/lib"
local UL_BIN       = UL_ROOT .. "/bin"
local UL_RESOURCES = UL_ROOT .. "/resources"

if not os.isdir(UL_INCLUDE) then
    print("ERROR: Ultralight SDK not found at: " .. UL_ROOT)
    print("The build tools automatically extract the SDK if missing.")
    print("If running xmake directly, extract: external/ultralight-sdk-1.4.0-win-x64.7z")
    os.exit(1)
end

do
if PRISMA_TARGET == "vr" then
    includes("ports/fo4vr")
else
target("PrismaUI_F4")
    set_kind("shared")
    set_filename("PrismaUI_F4.dll")
    set_symbols("debug")

    -- Add CommonLibF4 dependency
    add_deps("commonlibf4")

    -- Sources
    add_includedirs("src")
    add_files("src/**.cpp", "src/resource.rc")
    set_pcxxheader("src/PCH.h")

    -- Ultralight SDK
    add_includedirs(UL_INCLUDE)
    add_linkdirs(UL_LIB)
    add_links("AppCore", "Ultralight", "UltralightCore", "WebCore")
    -- Delay-load so Ultralight DLLs can be loaded from a non-system path at runtime.
    -- add_shflags targets the DLL linker (shared library); add_ldflags only hits exe targets.
    add_shflags(
        "/DELAYLOAD:UltralightCore.dll",
        "/DELAYLOAD:WebCore.dll",
        "/DELAYLOAD:Ultralight.dll",
        "/DELAYLOAD:AppCore.dll",
        { force = true }
    )
    add_links("delayimp")

    -- Packages
    add_packages("minhook", "directxtk", "spdlog")

    -- System libs
    add_syslinks("comctl32", "shell32", "imm32", "bcrypt", "version")

    -- Preprocessor
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_UNICODE")

    if is_plat("windows") then
        -- Conformance + suppress known-safe warnings from CommonLib headers
        add_cxflags("/permissive-", "/wd4200", "/wd4201", "/wd4324")
        -- Performance flags (mirrors cmake/CompilerFlags.cmake)
        add_cxflags("/Gy", "/Gw", "/Zc:inline", "/d2ReducedOptimizeHugeFunctions")
        -- Async SEH exceptions — required for _set_se_translator.
        -- /FS serialises PDB writes so parallel compilation doesn't corrupt them.
        add_cxflags("/EHa", "/bigobj", "/FS")
        -- Suppress the D9025 "overriding /EHs with /EHa" warning from xmake's default flags.
        add_cxflags("/wd4711")
        set_policy("build.warning", false)
        -- Dynamic CRT (/MD) — matches xmake precompiled DirectXTK and CommonLib packages.
        -- (cmake build used /MT via triplet; xmake packages ship /MD by default.)
        -- PDB in release
        add_cxflags("/Zi", { release = true })
        add_ldflags("/DEBUG", "/OPT:REF", "/OPT:ICF", { release = true, force = true })
    end

    -- Post-build: populate dist/ layout used by do_build.bat deploy step
    after_build(function(target)
        local proj    = os.scriptdir()
        local ver     = "1.0.0"
        local game_ver = os.getenv("PRISMA_TARGET") or "ng"
        local distdir = path.join(proj, "dist", "PrismaUI_F4_" .. ver .. "_" .. string.upper(game_ver))

        -- DLL
        local f4se_plugins = path.join(distdir, "F4SE", "plugins")
        os.mkdir(f4se_plugins)
        os.cp(target:targetfile(), f4se_plugins)

        -- Assets (views, translations, etc.)
        local assets_src = path.join(proj, "assets")
        if os.isdir(assets_src) then
            os.cp(assets_src, path.join(distdir, "PrismaUI_F4"))
        end

        -- Ultralight 1.4.0 resources + binaries
        local ul_res = path.join(os.scriptdir(), "build", "ultralight-1.4.0", "resources")
        local ul_bin = path.join(os.scriptdir(), "build", "ultralight-1.4.0", "bin")
        if os.isdir(ul_res) then
            os.cp(ul_res, path.join(distdir, "PrismaUI_F4", "resources"))
        end
        if os.isdir(ul_bin) then
            os.cp(ul_bin, path.join(distdir, "PrismaUI_F4", "libs"))
        end

        print("Distribution ready: dist/PrismaUI_F4_" .. ver)

        -- Auto-deploy to custom path if XSE_FO4_MODS_PATH is set
        local mods_path = os.getenv("XSE_FO4_MODS_PATH")
        if mods_path then
            local mod_root = path.join(mods_path, "PrismaUI_F4")
            local plugins_dir = path.join(mod_root, "F4SE", "Plugins")
            os.mkdir(plugins_dir)
            local dll_dest = path.join(plugins_dir, target:filename())
            os.cp(target:targetfile(), dll_dest)
            print("Deployed DLL to " .. dll_dest)
        end
    end)

-- Example plugin: demonstrates PrismaUI integration pattern
target("PrismaUI-F4-Example-Plugin")
    set_kind("shared")
    set_languages("c++23")
    set_filename("PrismaUI-F4-Example.dll")

    add_rules("commonlibf4.plugin", {
        name    = "PrismaUI-F4-Example-Plugin",
        author  = "PrismaUI",
        version = "1.0.0"
    })

    add_includedirs("example-f4se-plugin/src")
    add_files("example-f4se-plugin/src/**.cpp")
    set_pcxxheader("example-f4se-plugin/src/PCH.h")

    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")

    if is_plat("windows") then
        add_cxflags("/permissive-", "/wd4200", "/wd4201", "/wd4324")
        add_syslinks("Version", "Ole32", "OleAut32", "User32", "bcrypt", "crypt32")
    end

    after_build(function(target)
        print("[PrismaUI-F4-Example] built to: " .. target:targetfile())
    end)
end
end
