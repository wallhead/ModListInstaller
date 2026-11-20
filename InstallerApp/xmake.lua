set_project("ModlistInstaller")
set_version("0.3.11")

add_rules("mode.debug", "mode.release")
set_languages("c++20")

-- The portable package must not require an installed VC++ redistributable.
-- Keep the existing optional release-built static dependencies on the same CRT.
if is_plat("windows") then
    set_runtimes("MT")
end

option("use_libtorrent")
    set_default(false)
    set_showmenu(true)
    set_description("Build with libtorrent-rasterbar support when the library is available")
option_end()

local function vcpkg_root()
    return os.getenv("VCPKG_ROOT") or path.join(os.getenv("USERPROFILE") or "", "vcpkg")
end

local function add_vcpkg_libtorrent()
    local root = vcpkg_root()
    local installed = path.join(root, "installed", "x64-windows-static")
    add_includedirs(path.join(installed, "include"), {public = true})
    add_linkdirs(path.join(installed, "lib"), {public = true})
    add_defines("MODLIST_HAVE_LIBTORRENT", "BOOST_ALL_NO_LIB", "OPENSSL_STATIC", "TORRENT_ABI_VERSION=3", {public = true})
    add_links(
        "torrent-rasterbar",
        "libssl",
        "libcrypto",
        "iconv",
        "charset",
        "boost_chrono-vc145-mt-x64-1_91",
        "boost_date_time-vc145-mt-x64-1_91",
        "boost_random-vc145-mt-x64-1_91",
        "boost_context-vc145-mt-x64-1_91")
    add_syslinks("ws2_32", "iphlpapi", "crypt32", "bcrypt", "advapi32", "user32", "shell32")
end

target("installer_core")
    set_kind("static")
    add_includedirs("src", "resources", {public = true})
    add_files(
        "src/app/PackageDiscovery.cpp",
        "src/common/BuildIdentity.cpp",
        "src/downloader/LibtorrentDownloader.cpp",
        "src/extractor/PipelinedSevenZipExtractor.cpp",
        "src/extractor/SevenZipExtractor.cpp",
        "src/logging/Logger.cpp",
        "src/manifest/Json.cpp",
        "src/manifest/Manifest.cpp",
        "src/paths/WindowsFileMove.cpp",
        "src/paths/PathValidator.cpp",
        "src/tracker/TrackerProvider.cpp",
        "src/verifier/Sha256.cpp",
        "src/verifier/Verifier.cpp")

    on_load(function (target)
        target:add("includedirs", path.join(target:autogendir(), "build_identity"))
    end)
    before_build(function (target)
        local function git(...)
            local args = {"--no-optional-locks", "-C", os.projectdir(), ...}
            return try {function ()
                local out, err = os.iorunv("git", args)
                if not err or err == "" then
                    return (out or ""):trim()
                end
            end}
        end

        local id = "unavailable"
        -- An exported source tree inside an unrelated checkout is not a Git build.
        if git("cat-file", "-e", "HEAD:./xmake.lua") then
            local revision = git("rev-parse", "--verify", "HEAD^{commit}")
            if revision and (#revision == 40 or #revision == 64) and revision:match("^[0-9a-f]+$") then
                local status = git("status", "--porcelain=v1", "--untracked-files=normal", "--ignore-submodules=none")
                if status then
                    id = revision .. (status == "" and "" or "-dirty")
                end
            end
        end

        local dir = path.join(target:autogendir(), "build_identity")
        local header = path.join(dir, "ModlistBuildIdentity.generated.h")
        local content = '#define MODLIST_BUILD_ID "' .. id .. '"\n'
        if not os.isfile(header) or io.readfile(header) ~= content then
            os.mkdir(dir)
            io.writefile(header, content)
        end
    end)

    if is_plat("windows") then
        add_files("src/paths/FolderCleanup.cpp", "src/patch/PatchInstaller.cpp", "src/patch/PatchDeletePaths.cpp")
        add_cxxflags("/W4", "/permissive-", {tools = "cl"})
        add_syslinks("bcrypt", "ole32", "oleaut32", {public = true})
    else
        add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    end

    if has_config("use_libtorrent") then
        add_vcpkg_libtorrent()
    end
target_end()

target("modlist-installer")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_files("src/app/main.cpp")
    add_deps("installer_core")
target_end()

target("modlist-installer-gui")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_includedirs("resources")
    add_files("src/ui/WinInstallerApp.cpp", "src/ui/NativeInstallerView.cpp", "src/ui/NativeStrings.cpp")
    add_deps("installer_core")
    if is_plat("windows") then
        add_files("resources/app.rc")
        add_cxxflags("/utf-8", {tools = "cl"})
        add_syslinks("user32", "gdi32", "d2d1", "dwrite", "dwmapi", "uxtheme", "comdlg32", "shell32", "ole32", "oleaut32", "comctl32", "shlwapi", "advapi32")
        add_ldflags("/SUBSYSTEM:WINDOWS", {tools = "link"})
    end
target_end()

target("installer_core_tests")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_files("tests/test_main.cpp", "src/ui/NativeStrings.cpp")
    if is_plat("windows") then
        add_files("resources/app.rc")
    end
    add_deps("installer_core")
target_end()

target("installer_build_identity_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/test_build_identity.cpp")
    add_deps("installer_core")
target_end()

target("pipeline_scheduler_tests")
    set_kind("binary")
    add_includedirs("src")
    add_files("tests/test_pipeline_scheduler.cpp")
target_end()

if is_plat("windows") then
    target("modlist-patch-installer")
        set_kind("binary")
        set_rundir("$(projectdir)")
        add_defines("MODLIST_PATCH_INSTALLER")
        add_files("src/ui/WinInstallerApp.cpp", "src/ui/NativeInstallerView.cpp",
                  "src/ui/NativeStrings.cpp", "resources/app.rc", "resources/PatchManifestResource.rc")
        add_cxxflags("/utf-8", {tools = "cl"})
        add_deps("installer_core")
        add_syslinks("user32", "gdi32", "d2d1", "dwrite", "dwmapi", "uxtheme",
                     "comdlg32", "shell32", "ole32", "oleaut32", "comctl32", "shlwapi", "advapi32")
        add_ldflags("/SUBSYSTEM:WINDOWS", {tools = "link"})
    target_end()

    target("patch_installer_tests")
        set_kind("binary")
        set_rundir("$(projectdir)")
        add_files("tests/test_patch_installer.cpp", "resources/app.rc")
        add_cxxflags("/utf-8", {tools = "cl"})
        add_deps("installer_core")
    target_end()

    target("patch_registry_tests")
        set_kind("binary")
        add_files("tests/test_patch_registry.cpp", "src/ui/NativeInstallerView.cpp",
                  "src/ui/NativeStrings.cpp", "resources/app.rc")
        add_cxxflags("/utf-8", {tools = "cl"})
        add_deps("installer_core")
        add_syslinks("user32", "gdi32", "d2d1", "dwrite", "dwmapi", "uxtheme",
                     "comdlg32", "shell32", "ole32", "oleaut32", "comctl32", "shlwapi", "advapi32")
    target_end()

    target("folder_cleanup_tests")
        set_kind("binary")
        add_files("tests/test_folder_cleanup.cpp")
        add_deps("installer_core")
        add_syslinks("advapi32")
    target_end()

    target("smoke_script_driver")
        set_kind("binary")
        add_files("tests/smoke_script_driver.cpp")
        add_deps("installer_core")
    target_end()

    target("pipeline_smoke")
        set_kind("binary")
        add_files("tests/pipeline_smoke.cpp", "resources/app.rc")
        add_includedirs("resources")
        add_deps("installer_core")
    target_end()

    target("diagnostics_engine")
        set_kind("static")
        add_includedirs("src", "resources", {public = true})
        add_files("src/diagnostics/Workload.cpp", "src/diagnostics/Benchmark.cpp",
                  "src/extractor/PipelinedSevenZipExtractor.cpp", "src/extractor/SevenZipExtractor.cpp",
                  "src/manifest/Json.cpp", "src/manifest/Manifest.cpp", "src/paths/PathValidator.cpp",
                  "src/verifier/Sha256.cpp")
        add_cxxflags("/W4", "/utf-8", {tools = "cl"})
        add_syslinks("bcrypt", "ole32", "oleaut32", "user32", "psapi", "advapi32", {public = true})
    target_end()

    target("modlist-diagnostics")
        set_kind("binary")
        add_files("src/diagnostics/Main.cpp", "resources/app.rc")
        add_deps("diagnostics_engine")
        add_cxxflags("/utf-8", {tools = "cl"})
        add_syslinks("gdi32", "comctl32", "shell32", "comdlg32")
        add_ldflags("/SUBSYSTEM:WINDOWS", {tools = "link"})
    target_end()

    target("diagnostics_integration_tests")
        set_kind("binary")
        add_files("tests/test_diagnostics_integration.cpp", "resources/app.rc")
        add_deps("diagnostics_engine")
        add_cxxflags("/utf-8", {tools = "cl"})
    target_end()

    target("diagnostics_tests")
        set_kind("binary")
        add_includedirs("src")
        add_files("tests/test_diagnostics.cpp", "src/diagnostics/Workload.cpp")
        add_cxxflags("/utf-8", {tools = "cl"})
    target_end()

    target("installer_workflow_tests")
        set_kind("binary")
        set_rundir("$(projectdir)")
        add_files("tests/test_install_workflow.cpp", "src/ui/NativeInstallerView.cpp", "src/ui/NativeStrings.cpp", "resources/app.rc")
        add_includedirs("resources")
        add_cxxflags("/utf-8", {tools = "cl"})
        add_deps("installer_core")
        add_syslinks("user32", "gdi32", "d2d1", "dwrite", "dwmapi", "uxtheme", "comdlg32", "shell32", "ole32", "oleaut32", "comctl32", "shlwapi", "advapi32")
    target_end()

    target("installer_ui_tests")
        set_kind("binary")
        set_rundir("$(projectdir)")
        add_files("tests/test_ui.cpp", "src/ui/NativeInstallerView.cpp", "src/ui/NativeStrings.cpp", "resources/app.rc")
        add_includedirs("resources")
        add_cxxflags("/utf-8", {tools = "cl"})
        add_deps("installer_core")
        add_syslinks("user32", "gdi32", "d2d1", "dwrite", "dwmapi", "uxtheme", "comdlg32", "shell32", "ole32", "oleaut32", "comctl32", "shlwapi", "advapi32")
    target_end()
end
