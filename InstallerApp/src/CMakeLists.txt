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
  add_executable(modlist-installer-gui WIN32 ui/WinInstallerApp.cpp ../resources/app.rc)
  target_include_directories(modlist-installer-gui PRIVATE ../resources)
  target_link_libraries(modlist-installer-gui PRIVATE installer_core comctl32)
endif()
