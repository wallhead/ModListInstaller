set(build_identity_dir "${CMAKE_CURRENT_BINARY_DIR}/build_identity/$<CONFIG>")
set(build_identity_header "${build_identity_dir}/ModlistBuildIdentity.generated.h")
add_custom_target(installer_build_identity
  COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${CMAKE_CURRENT_SOURCE_DIR}/.."
    "-Doutput_file=${build_identity_header}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/common/BuildIdentity.cmake"
  BYPRODUCTS "${build_identity_header}"
  VERBATIM)

add_library(installer_core
  app/PackageDiscovery.cpp
  common/BuildIdentity.cpp
  downloader/LibtorrentDownloader.cpp
  extractor/PipelinedSevenZipExtractor.cpp
  extractor/SevenZipExtractor.cpp
  logging/Logger.cpp
  manifest/Json.cpp
  manifest/Manifest.cpp
  paths/WindowsFileMove.cpp
  paths/PathValidator.cpp
  tracker/TrackerProvider.cpp
  verifier/Sha256.cpp
  verifier/Verifier.cpp
)

add_dependencies(installer_core installer_build_identity)
target_include_directories(installer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(installer_core PRIVATE ../resources "${build_identity_dir}")
target_compile_features(installer_core PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(installer_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(installer_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_sources(installer_core PRIVATE paths/FolderCleanup.cpp)
  target_link_libraries(installer_core PUBLIC bcrypt ole32 oleaut32)
endif()

if(MODLIST_USE_LIBTORRENT)
  find_package(LibtorrentRasterbar CONFIG REQUIRED)
  target_compile_definitions(installer_core PUBLIC MODLIST_HAVE_LIBTORRENT=1)
  target_link_libraries(installer_core PUBLIC LibtorrentRasterbar::torrent-rasterbar)
endif()

add_executable(modlist-installer app/main.cpp)
target_link_libraries(modlist-installer PRIVATE installer_core)

if(WIN32)
  set_property(SOURCE ../resources/app.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../ui/style.css;${CMAKE_CURRENT_SOURCE_DIR}/../ui/strings.json")
  add_executable(modlist-installer-gui WIN32
    ui/WinInstallerApp.cpp
    ui/NativeInstallerView.cpp
    ui/NativeStrings.cpp
    ../resources/app.rc)
  target_include_directories(modlist-installer-gui PRIVATE ../resources)
  if(MSVC)
    target_compile_options(modlist-installer-gui PRIVATE /utf-8)
    # app.rc already embeds the DPI-aware application manifest.
    target_link_options(modlist-installer-gui PRIVATE /MANIFEST:NO)
  endif()
  target_link_libraries(modlist-installer-gui PRIVATE installer_core
    user32 gdi32 d2d1 dwrite dwmapi uxtheme comctl32 shlwapi
    ole32 oleaut32 shell32 advapi32)
endif()
