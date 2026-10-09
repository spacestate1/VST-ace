cmake_minimum_required(VERSION 3.16)
project(studio LANGUAGES C CXX)

# studio -- the session shell: one window, a tab per synth plug-in (each a
# HostWidget out of peload's qthost) plus the tracker's tab.
#
# ---- what this build is, for the About box --------------------------------
#
# The same arrangement peload/CMakeLists.txt and gui/CMakeLists.txt carry: the
# version is overridable from the environment because the packaging scripts
# supply it, and the commit comes from the environment because a packaged
# build has no .git to ask.
if(DEFINED ENV{VSTACE_VERSION})
  set(VSTACE_VERSION "$ENV{VSTACE_VERSION}")
else()
  set(VSTACE_VERSION "0.3.1")
endif()

if(DEFINED ENV{SOURCE_DATE_EPOCH})
  execute_process(
    COMMAND date -u -d "@$ENV{SOURCE_DATE_EPOCH}" +%Y-%m-%d
    OUTPUT_VARIABLE VSTACE_BUILD_DATE OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT VSTACE_BUILD_DATE)
  string(TIMESTAMP VSTACE_BUILD_DATE "%Y-%m-%d" UTC)
endif()

if(DEFINED ENV{VSTACE_GIT})
  set(VSTACE_GIT "$ENV{VSTACE_GIT}")
else()
  execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE VSTACE_GIT OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT VSTACE_GIT)
  set(VSTACE_GIT "")
endif()

add_compile_definitions(
  VSTACE_VERSION="${VSTACE_VERSION}"
  VSTACE_BUILD_DATE="${VSTACE_BUILD_DATE}"
  VSTACE_GIT="${VSTACE_GIT}")

# C++20 because qthost's headers are what peload compiles as C++20. The C
# standard here decides nothing itself -- the libraries carry their own
# directory-scoped one (C11 in peload, C99 in tracker), which is why neither
# is repeated here.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(Qt6 COMPONENTS Widgets QUIET)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(ALSA IMPORTED_TARGET alsa)

if(Qt6_FOUND AND PIPEWIRE_FOUND AND ALSA_FOUND)
  set(CMAKE_AUTOMOC ON)

  # qthost (the per-plug-in host) and pehost out of peload, trkcore out of
  # tracker. EXCLUDE_FROM_ALL on both: their own programs -- pestudio,
  # peload, trktest -- are not wanted here, only the libraries, built when
  # studio links them. Link the qthost target rather than the archive: its
  # PUBLIC compile definitions (PEHOST_HAVE_X11 among them) must reach every
  # file that includes hostwindow.h, or the header declares a different
  # EditorHost than the library built.
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../peload peload-build EXCLUDE_FROM_ALL)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tracker tracker-build EXCLUDE_FROM_ALL)

  add_executable(studio qt/main.cpp sessfile.c recentfiles.c reaperlink.c
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tracker/qt/trackerwidget.h)
  target_include_directories(studio PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/../peload/qtgui
      ${CMAKE_CURRENT_SOURCE_DIR}/../tracker/qt)
  target_link_libraries(studio PRIVATE qthost trkcore Qt6::Widgets PkgConfig::PIPEWIRE)
else()
  message(STATUS "studio skipped (needs Qt6Widgets, libpipewire-0.3 and alsa)")
endif()

# studiogtk -- the same shell in GTK: each synth tab is a plugview out of
# gui/'s gtkhost (what dwstudio is one of), the tracker tab is tracker/gtk's
# trackerview, embedded through the trkgtk library. gui/ is pulled in as a
# subdirectory for gtkhost; it sees pehost already exists (added above) and
# does not add peload a second time.
pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
pkg_check_modules(X11 IMPORTED_TARGET x11)
if(GTK4_FOUND AND X11_FOUND AND PIPEWIRE_FOUND AND ALSA_FOUND AND TARGET trkgtk)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gui gui-build EXCLUDE_FROM_ALL)
  add_executable(studiogtk gtk/main.c sessfile.c recentfiles.c mixbus.c reaperlink.c)
  target_include_directories(studiogtk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(studiogtk PRIVATE gtkhost trkgtk audioout
      PkgConfig::PIPEWIRE PkgConfig::ALSA m pthread)
else()
  message(STATUS "studiogtk skipped (needs gtk4, x11, libpipewire-0.3 and alsa)")
endif()
