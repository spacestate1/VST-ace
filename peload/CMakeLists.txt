cmake_minimum_required(VERSION 3.16)
project(peload C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# The loader itself: plain C, no Wine, no Windows headers.
# vst3.c is compiled twice: SysV vtables for native Linux bundles, Microsoft
# x64 vtables for Windows images. Its internals are static, so the two objects
# coexist; only the sv_/ms_ entry points are external.
add_library(vst3_sysv OBJECT vst3.c)
add_library(vst3_msabi OBJECT vst3.c)
target_compile_definitions(vst3_msabi PRIVATE V3_MSABI=1)
foreach(t vst3_sysv vst3_msabi)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

target_link_libraries(vst3_sysv PRIVATE PkgConfig::FREETYPE)
target_link_libraries(vst3_msabi PRIVATE PkgConfig::FREETYPE)

# The macOS side: a Mach-O loader plus the framework shims. In-process, because
# macOS x86-64 is System V -- the same ABI this host already uses.
add_library(pehost STATIC pehost.c vst3_dispatch.c bridge_client.c patch.c
            machoload.c macshim.c macvdsp.c macfound.c macobjc.c macns.c
            macquartz.c macmetal.c macobjc_msgsend.S macvsthost.c macau.c
            # Classic Mac OS: a PowerPC interpreter, the PEF loader, the system
            # libraries a Classic plug-in imports, QuickDraw pictures, and the
            # VST 1.0 glue on top.
            ppc.c pefload.c cfmlib.c pict.c pefvst.c
            $<TARGET_OBJECTS:vst3_sysv> $<TARGET_OBJECTS:vst3_msabi>)
target_include_directories(pehost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# librt for shm_open on the bridge's shared region
target_link_libraries(pehost PUBLIC Threads::Threads m dl rt PkgConfig::FREETYPE)

# The isolation helper: hosts one plugin for another process, so a plugin that
# faults costs a helper instead of the host. Same protocol as peload32 --serve.
add_executable(peserve peserve.c)
target_link_libraries(peserve PRIVATE pehost)

# CLI: inspect and render a plugin to WAV.
add_executable(peload peload_cli.c)
target_link_libraries(peload PRIVATE pehost)

# Qt6 GUI: browse, load, tweak and play plugins live.
find_package(Qt6 COMPONENTS Widgets QUIET)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(ALSA IMPORTED_TARGET alsa)
# FreeType backs the DirectWrite shim's glyph lookup and metrics.
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)

if(Qt6_FOUND AND PIPEWIRE_FOUND AND ALSA_FOUND)
  set(CMAKE_AUTOMOC ON)
  add_executable(pestudio qtgui/main.cpp qtgui/midiio.cpp)
  target_link_libraries(pestudio PRIVATE pehost Qt6::Widgets PkgConfig::PIPEWIRE PkgConfig::ALSA)
  target_include_directories(pestudio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
else()
  message(STATUS "pestudio skipped (needs Qt6Widgets, libpipewire-0.3 and alsa)")
endif()

# ---------------------------------------------------------------------------
# The i386 loader.
#
# A process cannot execute both widths, so this is a separate program rather
# than a mode of peload: pe32.c is a self-contained loader plus VST2 host built
# with -m32. It shares winstubs.h with the 64-bit side (via winstubs32.h, which
# selects the stdcall convention and leaves the GUI layer out), so a fix to a
# stub benefits both.
#
# Needs 32-bit libc/libpthread -- on Arch that is lib32-glibc. If they are
# missing the target is skipped rather than breaking the build.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-m32")
check_c_source_compiles("#include <pthread.h>\nint main(void){return (int)pthread_self();}"
                        HAVE_M32_TOOLCHAIN)
unset(CMAKE_REQUIRED_FLAGS)

if(HAVE_M32_TOOLCHAIN)
  add_executable(peload32 pe32.c)
  # plain C, and AUTOMOC is on for pestudio's sake
  set_target_properties(peload32 PROPERTIES AUTOMOC OFF)
  target_include_directories(peload32 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # -mincoming-stack-boundary=2: the i386 Windows ABI only guarantees a 4-byte
  # aligned stack, while GCC assumes 16 and freely emits movaps against stack
  # slots. Guest code calling into a stub therefore faults the first time any
  # host function -- ours, or FreeType's deep inside it -- spills an XMM
  # register. Telling GCC the incoming alignment makes it realign where needed.
  # (x86-64 needs nothing here: both ABIs already guarantee 16.)
  target_compile_options(peload32 PRIVATE -m32 -mincoming-stack-boundary=2)
  target_link_options(peload32 PRIVATE -m32)
  target_link_libraries(peload32 PRIVATE pthread m)

  # The window layer and the DirectWrite shim need 32-bit FreeType. Their .pc
  # files are under /usr/lib32/pkgconfig, which the default pkg-config path does
  # not cover, so query them with PKG_CONFIG_LIBDIR pointed there.
  set(_pc_saved32 "$ENV{PKG_CONFIG_LIBDIR}")
  set(ENV{PKG_CONFIG_LIBDIR} /usr/lib32/pkgconfig)
  execute_process(COMMAND pkg-config --cflags freetype2
                  OUTPUT_VARIABLE FT32_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE FT32_OK ERROR_QUIET)
  execute_process(COMMAND pkg-config --libs freetype2
                  OUTPUT_VARIABLE FT32_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  set(ENV{PKG_CONFIG_LIBDIR} "${_pc_saved32}")
  if(NOT FT32_OK EQUAL 0)
    message(FATAL_ERROR "peload32 needs 32-bit freetype2 (lib32-freetype2)")
  endif()
  separate_arguments(FT32_CFLAGS_LIST NATIVE_COMMAND "${FT32_CFLAGS}")
  separate_arguments(FT32_LIBS_LIST   NATIVE_COMMAND "${FT32_LIBS}")
  target_compile_options(peload32 PRIVATE ${FT32_CFLAGS_LIST})
  target_link_libraries(peload32 PRIVATE ${FT32_LIBS_LIST})

  # A live editor window needs 32-bit Xlib. There is no x11.pc under
  # /usr/lib32/pkgconfig on Arch, but lib32-libx11 ships the .so, and the X11
  # headers are architecture-independent -- so look for the library directly.
  find_library(X11_32 NAMES X11 PATHS /usr/lib32 NO_DEFAULT_PATH)
  if(X11_32)
    target_compile_definitions(peload32 PRIVATE PELOAD32_X11=1)
    target_link_libraries(peload32 PRIVATE ${X11_32})
    message(STATUS "peload32: live editor window enabled")
  else()
    message(STATUS "peload32: no editor window (needs lib32-libx11)")
  endif()

  # Live playback needs 32-bit PipeWire and ALSA. Their .pc files live under
  # /usr/lib32/pkgconfig, which the default pkg-config path does not cover, so
  # query them explicitly rather than picking up the 64-bit ones.
  set(_pc_saved "$ENV{PKG_CONFIG_LIBDIR}")
  set(ENV{PKG_CONFIG_LIBDIR} /usr/lib32/pkgconfig)
  execute_process(COMMAND pkg-config --cflags libpipewire-0.3 alsa
                  OUTPUT_VARIABLE PW32_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE PW32_OK ERROR_QUIET)
  execute_process(COMMAND pkg-config --libs libpipewire-0.3 alsa
                  OUTPUT_VARIABLE PW32_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  set(ENV{PKG_CONFIG_LIBDIR} "${_pc_saved}")
  if(PW32_OK EQUAL 0)
    separate_arguments(PW32_CFLAGS_LIST NATIVE_COMMAND "${PW32_CFLAGS}")
    separate_arguments(PW32_LIBS_LIST   NATIVE_COMMAND "${PW32_LIBS}")
    target_compile_definitions(peload32 PRIVATE PELOAD32_AUDIO=1)
    target_compile_options(peload32 PRIVATE ${PW32_CFLAGS_LIST})
    target_link_libraries(peload32 PRIVATE ${PW32_LIBS_LIST})
    message(STATUS "peload32: live playback enabled")
  else()
    message(STATUS "peload32: render only (needs lib32-pipewire and lib32-alsa-lib)")
  endif()
  # win32_arity.h is generated from the mingw-w64 i686 import libraries; it is
  # checked in, so regenerate only on request.
  add_custom_target(arity
    COMMAND ${CMAKE_COMMAND} -E echo "regenerating win32_arity.h"
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_arity.py
    COMMENT "Regenerate the Win32 stdcall arity table")
else()
  message(STATUS "peload32 skipped (needs a -m32 toolchain: lib32-glibc)")
endif()
