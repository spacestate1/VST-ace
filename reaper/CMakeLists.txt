cmake_minimum_required(VERSION 3.16)
project(reaper_tracker LANGUAGES C CXX)

# reaper_tracker.so -- the tracker's song, imported into REAPER as tracks
# with MIDI items. An extension: REAPER loads it from UserPlugins/ and it
# runs inside REAPER, so it uses REAPER's own API rather than any file
# format in between. It links the tracker's song reader and note walker
# (core/song.c, core/midiexport.c) and nothing of the ALSA engine.
#
#   ./fetch-sdk.sh                       the SDK and WDL headers, once
#   cmake -S . -B build && cmake --build build
#   cmake --install build                copies it to ~/.config/REAPER/UserPlugins

set(REAPER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reaper-sdk" CACHE PATH "reaper-sdk checkout")
set(WDL_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL"        CACHE PATH "WDL checkout")
if(NOT EXISTS "${REAPER_SDK_DIR}/sdk/reaper_plugin.h" OR NOT EXISTS "${WDL_DIR}/WDL/swell/swell.h")
  message(FATAL_ERROR "REAPER SDK / WDL not found: run reaper/fetch-sdk.sh, or pass -DREAPER_SDK_DIR= and -DWDL_DIR=")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(TRK_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../tracker/core)
add_library(reaper_tracker MODULE reaper_tracker.cpp ${TRK_CORE}/song.c ${TRK_CORE}/midiexport.c)
set_target_properties(reaper_tracker PROPERTIES PREFIX "")      # REAPER wants reaper_*.so
# reaper_plugin.h includes "../WDL/swell/swell.h"; with <wdl>/WDL as an
# include root that resolves to <wdl>/WDL/swell/swell.h.
target_include_directories(reaper_tracker PRIVATE ${REAPER_SDK_DIR}/sdk ${WDL_DIR}/WDL ${TRK_CORE})
target_compile_options(reaper_tracker PRIVATE -Wall -Wextra -Wno-format-truncation
                       $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>)
target_link_libraries(reaper_tracker PRIVATE m)

install(TARGETS reaper_tracker LIBRARY DESTINATION "$ENV{HOME}/.config/REAPER/UserPlugins")
