set_project("ModlistInstaller")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")
set_languages("c++20")

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
    if is_plat("windows") then
        set_runtimes("MT")
    end
end

local function add_webview2_sdk()
    local sdk = path.join(os.projectdir(), "third_party", "webview2", "Microsoft.Web.WebView2")
    add_includedirs(path.join(sdk, "build", "native", "include"))
    if is_arch("x86") then
        add_linkdirs(path.join(sdk, "build", "native", "x86"))
    elseif is_arch("arm64") then
        add_linkdirs(path.join(sdk, "build", "native", "arm64"))
    else
        add_linkdirs(path.join(sdk, "build", "native", "x64"))
    end
    add_links("WebView2LoaderStatic")
end

target("installer_core")
    set_kind("static")
    add_includedirs("src", "resources", {public = true})
    add_files(
        "src/app/PackageDiscovery.cpp",
        "src/downloader/LibtorrentDownloader.cpp",
        "src/extractor/SevenZipExtractor.cpp",
        "src/logging/Logger.cpp",
        "src/manifest/Json.cpp",
        "src/manifest/Manifest.cpp",
        "src/paths/PathValidator.cpp",
        "src/tracker/TrackerProvider.cpp",
        "src/verifier/Sha256.cpp",
        "src/verifier/Verifier.cpp")

    if is_plat("windows") then
        add_cxxflags("/W4", "/permissive-", {tools = "cl"})
    else
        add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    end

    if has_config("use_libtorrent") then
        add_vcpkg_libtorrent()
    end
target_end()

target("modlist-installer")
    set_kind("binary")
    if has_config("use_libtorrent") and is_plat("windows") then
        set_runtimes("MT")
    end
    set_rundir("$(projectdir)")
    add_files("src/app/main.cpp")
    add_deps("installer_core")
target_end()

target("modlist-installer-gui")
    set_kind("binary")
    if has_config("use_libtorrent") and is_plat("windows") then
        set_runtimes("MT")
    end
    set_rundir("$(projectdir)")
    add_includedirs("resources")
    add_files("src/ui/WinInstallerApp.cpp", "src/ui/WebViewHost.cpp")
    add_deps("installer_core")
    add_webview2_sdk()
    if is_plat("windows") then
        add_files("resources/app.rc")
        add_syslinks("user32", "gdi32", "comdlg32", "shell32", "ole32", "oleaut32", "comctl32", "shlwapi", "advapi32")
        add_ldflags("/SUBSYSTEM:WINDOWS", {tools = "link"})
    end
    after_build(function (target)
        os.cp("ui", path.join(target:targetdir(), "data", "ui"))
    end)
target_end()

target("installer_core_tests")
    set_kind("binary")
    if has_config("use_libtorrent") and is_plat("windows") then
        set_runtimes("MT")
    end
    set_rundir("$(projectdir)")
    add_files("tests/test_main.cpp")
    add_deps("installer_core")
target_end()
