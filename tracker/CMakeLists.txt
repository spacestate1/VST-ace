cmake_minimum_required(VERSION 3.16)
project(tracker LANGUAGES C CXX)

# tracker -- a pattern sequencer that plays other programs over MIDI.
#
#   tracker       the Qt window
#   tracker-gtk   the same thing in GTK 4
#   trktest       the engine against two ALSA receivers, no window
#
# Both windows are thin over core/, which holds the song, the editor's keys
# and the ALSA engine; neither has any logic the other lacks.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)          # strncasecmp, nanosleep, the POSIX bits
set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

# The drum kits are vst-ace's own sample engine, from c/src.
set(VA_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../c/src)
add_library(trkcore STATIC core/song.c core/engine.c core/editor.c
            ${VA_ENGINE_DIR}/drumkit.c ${VA_ENGINE_DIR}/wav.c ${VA_ENGINE_DIR}/dw_dsp.c)
target_include_directories(trkcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
# Public: the windows' sample-set editors read and write a set through
# drumkit.h's dk_map.
target_include_directories(trkcore PUBLIC ${VA_ENGINE_DIR})
target_link_libraries(trkcore PUBLIC PkgConfig::ALSA Threads::Threads m)
# Truncating a long kit or client name to its field is the intent, and the
# compiler cannot tell; c/'s Makefile turns this off for the same reason.
target_compile_options(trkcore PRIVATE -Wall -Wextra -Wno-format-truncation)

add_executable(trktest test/trktest.c)
target_link_libraries(trktest PRIVATE trkcore m)
target_include_directories(trktest PRIVATE ${VA_ENGINE_DIR})   # wav.h, to write a test kit

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  # trackerwidget.h is the whole UI as a widget a shell can embed; main.cpp is
  # the standalone window around it. Listing the header puts it through moc --
  # it holds every Q_OBJECT class the window uses.
  add_executable(tracker qt/main.cpp qt/trackerwidget.h)
  target_link_libraries(tracker PRIVATE trkcore Qt6::Widgets)
  # The same window driven by a script, offscreen -- see uitest() in main.cpp.
  add_executable(tracker-uitest qt/main.cpp qt/trackerwidget.h)
  target_compile_definitions(tracker-uitest PRIVATE TRACKER_UITEST)
  target_link_libraries(tracker-uitest PRIVATE trkcore Qt6::Widgets)
else()
  message(STATUS "tracker (Qt) skipped: needs Qt6Widgets")
endif()

pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
if(GTK4_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gtk/trackerview.c)
  # The whole UI as a view a shell can embed, out of the same file the
  # standalone wraps in a window -- the mirror of qt/trackerwidget.h, in C.
  # studiogtk (session/) links this and packs the view into a tab.
  add_library(trkgtk STATIC gtk/trackerview.c)
  target_link_libraries(trkgtk PUBLIC trkcore PkgConfig::GTK4)
  target_include_directories(trkgtk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gtk)
  target_compile_options(trkgtk PRIVATE -Wall -Wextra)
  add_executable(tracker-gtk gtk/main.c)
  target_link_libraries(tracker-gtk PRIVATE trkgtk PkgConfig::GTK4)
  target_compile_options(tracker-gtk PRIVATE -Wall -Wextra)
  # Driven by a script under the broadway backend -- see uitest() in
  # trackerview.c. Built from the sources rather than the library so the
  # TRACKER_UITEST drive is compiled in; the library never carries it.
  add_executable(tracker-gtk-uitest gtk/main.c gtk/trackerview.c)
  target_compile_definitions(tracker-gtk-uitest PRIVATE TRACKER_UITEST)
  target_include_directories(tracker-gtk-uitest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gtk)
  target_link_libraries(tracker-gtk-uitest PRIVATE trkcore PkgConfig::GTK4)
else()
  message(STATUS "tracker-gtk skipped: needs gtk4")
endif()
