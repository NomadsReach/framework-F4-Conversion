set_xmakever("3.0.0")
set_project("PrismaUI_F4")
set_version("1.0.0")
set_languages("c++23")

add_rules("mode.release", "mode.releasedbg", "mode.debug")
add_requires("minhook")
add_requires("directxtk")

local PRISMA_TARGET = string.lower(os.getenv("PRISMA_TARGET") or "ng")
if PRISMA_TARGET ~= "og" and PRISMA_TARGET ~= "ng" and PRISMA_TARGET ~= "vr" then
    os.raise("PRISMA_TARGET must be one of: og, ng, vr")
end

if PRISMA_TARGET == "vr" then
    includes("lib/commonlibf4vr")
else
    add_requires("spdlog v1.16.0", { configs = { header_only = false, wchar = true, std_format = true } })
    includes("lib/commonlibf4")
end

local UL_ROOT = path.join(os.scriptdir(), "build", "ultralight-1.4.0")
local UL_INCLUDE = UL_ROOT .. "/include"
local UL_LIB = UL_ROOT .. "/lib"

if not os.isdir(UL_INCLUDE) then
    print("ERROR: Ultralight SDK not found at: " .. UL_ROOT)
    os.exit(1)
end

if PRISMA_TARGET == "vr" then
    includes("ports/fo4vr")
else

target("PrismaUI_F4")
    set_kind("shared")
    set_filename("PrismaUI_F4.dll")
    set_symbols("debug")

    add_deps("commonlibf4")
    add_includedirs("src")
    add_files("src/**.cpp", "src/resource.rc")
    set_pcxxheader("src/PCH.h")

    add_includedirs(UL_INCLUDE)
    add_linkdirs(UL_LIB)
    add_links("AppCore", "Ultralight", "UltralightCore", "WebCore")
    add_shflags(
        "/DELAYLOAD:UltralightCore.dll",
        "/DELAYLOAD:WebCore.dll",
        "/DELAYLOAD:Ultralight.dll",
        "/DELAYLOAD:AppCore.dll",
        { force = true }
    )
    add_links("delayimp")

    add_packages("minhook", "directxtk", "spdlog")
    add_syslinks("comctl32", "shell32", "imm32", "bcrypt", "version", "psapi")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_UNICODE", "UNICODE")

    if is_plat("windows") then
        add_cxflags("/permissive-", "/wd4200", "/wd4201", "/wd4324")
        add_cxflags("/Gy", "/Gw", "/Zc:inline", "/d2ReducedOptimizeHugeFunctions")
        add_cxflags("/EHa", "/bigobj", "/FS")
        add_cxflags("/wd4711")
        set_policy("build.warning", false)
        add_cxflags("/Zi", { release = true })
        add_ldflags("/DEBUG", "/OPT:REF", "/OPT:ICF", { release = true, force = true })
    end

    after_build(function(target)
        local proj = os.scriptdir()
        local ver = "1.0.0"
        local game_ver = os.getenv("PRISMA_TARGET") or "ng"
        local distdir = path.join(proj, "dist", "PrismaUI_F4_" .. ver .. "_" .. string.upper(game_ver))

        local f4se_plugins = path.join(distdir, "F4SE", "plugins")
        os.mkdir(f4se_plugins)
        os.cp(target:targetfile(), f4se_plugins)

        local assets_src = path.join(proj, "assets")
        if os.isdir(assets_src) then
            os.cp(assets_src, path.join(distdir, "PrismaUI_F4"))
        end

        local ul_res = path.join(os.scriptdir(), "build", "ultralight-1.4.0", "resources")
        local ul_bin = path.join(os.scriptdir(), "build", "ultralight-1.4.0", "bin")
        if os.isdir(ul_res) then
            os.cp(ul_res, path.join(distdir, "PrismaUI_F4", "resources"))
        end
        if os.isdir(ul_bin) then
            os.cp(ul_bin, path.join(distdir, "PrismaUI_F4", "libs"))
        end

        local mods_path = os.getenv("XSE_FO4_MODS_PATH")
        if mods_path then
            local mod_root = path.join(mods_path, "PrismaUI_F4")
            local plugins_dir = path.join(mod_root, "F4SE", "Plugins")
            os.mkdir(plugins_dir)
            os.cp(target:targetfile(), path.join(plugins_dir, target:filename()))
        end
    end)

target("PrismaUI-F4-Example-Plugin")
    set_kind("shared")
    set_languages("c++23")
    set_filename("PrismaUI-F4-Example.dll")

    add_rules("commonlibf4.plugin", {
        name = "PrismaUI-F4-Example-Plugin",
        author = "PrismaUI",
        version = "1.0.0"
    })

    add_includedirs("example-f4se-plugin/src")
    add_files("example-f4se-plugin/src/**.cpp")
    set_pcxxheader("example-f4se-plugin/src/PCH.h")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_UNICODE", "UNICODE")

    if is_plat("windows") then
        add_cxflags("/permissive-", "/wd4200", "/wd4201", "/wd4324")
        add_syslinks("Version", "Ole32", "OleAut32", "User32", "bcrypt", "crypt32")
    end
end
end
