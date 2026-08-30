cmake_minimum_required(VERSION 3.16)
project(dwstudio LANGUAGES C)

# ---- what this build is, for the About boxes ------------------------------
#
# SOURCE_DATE_EPOCH is honoured because Debian and Fedora set it to keep builds
# reproducible; taking the clock instead would make two builds of one source
# differ. Falling back to the clock is right for a developer's own build, which
# is the case where the date is actually worth reading.
set(VSTACE_VERSION "0.1.0")
if(DEFINED ENV{SOURCE_DATE_EPOCH})
  execute_process(
    COMMAND date -u -d "@$ENV{SOURCE_DATE_EPOCH}" +%Y-%m-%d
    OUTPUT_VARIABLE VSTACE_BUILD_DATE OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT VSTACE_BUILD_DATE)
  string(TIMESTAMP VSTACE_BUILD_DATE "%Y-%m-%d" UTC)
endif()

# The commit, when this is a checkout. A release tarball has no .git and simply
# does not name one, rather than the build failing over it.
execute_process(
  COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
  OUTPUT_VARIABLE VSTACE_GIT OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT VSTACE_GIT)
  set(VSTACE_GIT "")
endif()

add_compile_definitions(
  VSTACE_VERSION="${VSTACE_VERSION}"
  VSTACE_BUILD_DATE="${VSTACE_BUILD_DATE}"
  VSTACE_GIT="${VSTACE_GIT}")


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
# A native Linux plug-in editor is an X11 window, so this window has to be
# able to make one and hand over its id -- see plugview.c.
pkg_check_modules(X11 REQUIRED x11)

# pehost, so this window can host real plug-ins the way pestudio does rather
# than only reading preset banks out of them. EXCLUDE_FROM_ALL because peload's
# own programs are not wanted here -- only the library, and only what it pulls
# in with it.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../peload peload-build EXCLUDE_FROM_ALL)

# The same C engine the command-line tools use; the GUI is only a front-end.
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../c/src)
add_executable(dwstudio
    dwstudio.c
    plugview.c
    ${ENGINE_DIR}/pe.c
    ${ENGINE_DIR}/wav.c
    ${ENGINE_DIR}/wavedst.c
    ${ENGINE_DIR}/bank.c
    ${ENGINE_DIR}/dw_wavetable.c
    ${ENGINE_DIR}/dw_dsp.c
    ${ENGINE_DIR}/dw_synth.c
    ${ENGINE_DIR}/fb02.c
    ${ENGINE_DIR}/fm_synth.c
    ${ENGINE_DIR}/juno.c
    ${ENGINE_DIR}/drumkit.c)

target_include_directories(dwstudio PRIVATE ${ENGINE_DIR} ${GTK4_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS} ${PIPEWIRE_INCLUDE_DIRS} ${X11_INCLUDE_DIRS})
# snprintf() truncation cannot actually happen with these path and label
# lengths, but the compiler has no way to prove it -- same as the CLI Makefile.
target_compile_options(dwstudio PRIVATE ${GTK4_CFLAGS_OTHER} -Wall -Wextra
                       -Wno-format-truncation)
target_link_libraries(dwstudio PRIVATE pehost ${GTK4_LIBRARIES} ${ALSA_LIBRARIES} ${PIPEWIRE_LIBRARIES} ${X11_LIBRARIES} m pthread)
target_link_directories(dwstudio PRIVATE ${GTK4_LIBRARY_DIRS} ${PIPEWIRE_LIBRARY_DIRS})
