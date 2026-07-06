cmake_minimum_required(VERSION 3.20)

project(ModlistInstaller VERSION 0.1 LANGUAGES CXX)

option(MODLIST_USE_LIBTORRENT "Build the libtorrent-rasterbar downloader backend" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
