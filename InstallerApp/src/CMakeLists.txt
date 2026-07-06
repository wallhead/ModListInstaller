add_library(installer_core
  app/PackageDiscovery.cpp
  downloader/LibtorrentDownloader.cpp
  extractor/SevenZipExtractor.cpp
  logging/Logger.cpp
  manifest/Json.cpp
  manifest/Manifest.cpp
  paths/PathValidator.cpp
  tracker/TrackerProvider.cpp
  verifier/Sha256.cpp
  verifier/Verifier.cpp
)

target_include_directories(installer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(installer_core PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(installer_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(installer_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MODLIST_USE_LIBTORRENT)
  find_package(LibtorrentRasterbar CONFIG REQUIRED)
  target_compile_definitions(installer_core PUBLIC MODLIST_HAVE_LIBTORRENT=1)
  target_link_libraries(installer_core PUBLIC LibtorrentRasterbar::torrent-rasterbar)
endif()

add_executable(modlist-installer app/main.cpp)
target_link_libraries(modlist-installer PRIVATE installer_core)

if(WIN32)
  set(WEBVIEW2_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/Microsoft.Web.WebView2" CACHE PATH "Microsoft.Web.WebView2 SDK root")
  add_executable(modlist-installer-gui WIN32 ui/WinInstallerApp.cpp ui/WebViewHost.cpp ../resources/app.rc)
  target_include_directories(modlist-installer-gui PRIVATE ../resources)
  if(EXISTS "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h")
    target_include_directories(modlist-installer-gui PRIVATE "${WEBVIEW2_SDK_DIR}/build/native/include")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      target_link_directories(modlist-installer-gui PRIVATE "${WEBVIEW2_SDK_DIR}/build/native/x64")
    else()
      target_link_directories(modlist-installer-gui PRIVATE "${WEBVIEW2_SDK_DIR}/build/native/x86")
    endif()
    target_link_libraries(modlist-installer-gui PRIVATE WebView2LoaderStatic)
  else()
    message(FATAL_ERROR "Microsoft.Web.WebView2 SDK not found. Run scripts/restore-webview2.ps1 first.")
  endif()
  target_link_libraries(modlist-installer-gui PRIVATE installer_core comctl32 shlwapi ole32 oleaut32 shell32 advapi32)
  add_custom_command(TARGET modlist-installer-gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/../ui"
      "$<TARGET_FILE_DIR:modlist-installer-gui>/data/ui")
endif()
