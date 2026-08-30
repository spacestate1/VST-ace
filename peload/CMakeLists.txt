cmake_minimum_required(VERSION 3.16)
project(peload C CXX ASM)

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

if(NOT PKG_CONFIG_EXECUTABLE)
  set(PKG_CONFIG_EXECUTABLE pkg-config)
endif()

# pkgconfig32(<prefix> <module>...) -- pkg-config, for the 32-bit build.
#
# The 32-bit .pc files live outside the default pkg-config path, and where
# depends on the distribution: /usr/lib32/pkgconfig on Arch, the i386 multiarch
# triplet's own directory (/usr/lib/i386-linux-gnu/pkgconfig) on Debian and
# Ubuntu. Ask each in turn with PKG_CONFIG_LIBDIR pointed at it and take the
# first that answers, so a machine carrying either layout builds -- and so the
# 64-bit .pc files are never the ones that answer.
#
# Sets <prefix>_OK (0 on success, reading the same way as execute_process's own
# RESULT_VARIABLE), <prefix>_CFLAGS, <prefix>_LIBS, and <prefix>_DIR for the
# directory that answered.
#
# PKG_CONFIG_LIBDIR is process-wide rather than scoped to this function, so it
# is put back afterwards -- and *unset* rather than set empty when it started
# unset, because pkg-config reads an empty one as a search path with nothing in
# it and would fail every later query in this tree.
function(pkgconfig32 prefix)
  set(_had_libdir FALSE)
  if(DEFINED ENV{PKG_CONFIG_LIBDIR})
    set(_had_libdir TRUE)
    set(_saved "$ENV{PKG_CONFIG_LIBDIR}")
  endif()

  set(_ok 1)
  set(_cflags "")
  set(_libs "")
  set(_dir_found "")
  foreach(_dir /usr/lib32/pkgconfig /usr/lib/i386-linux-gnu/pkgconfig)
    if(NOT IS_DIRECTORY "${_dir}")
      continue()
    endif()
    set(ENV{PKG_CONFIG_LIBDIR} "${_dir}")
    execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags ${ARGN}
                    OUTPUT_VARIABLE _cflags OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _ok ERROR_QUIET)
    if(_ok EQUAL 0)
      execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --libs ${ARGN}
                      OUTPUT_VARIABLE _libs OUTPUT_STRIP_TRAILING_WHITESPACE
                      RESULT_VARIABLE _ok ERROR_QUIET)
      set(_dir_found "${_dir}")
      break()
    endif()
  endforeach()

  if(_had_libdir)
    set(ENV{PKG_CONFIG_LIBDIR} "${_saved}")
  else()
    unset(ENV{PKG_CONFIG_LIBDIR})
  endif()

  set(${prefix}_OK     "${_ok}"        PARENT_SCOPE)
  set(${prefix}_CFLAGS "${_cflags}"    PARENT_SCOPE)
  set(${prefix}_LIBS   "${_libs}"      PARENT_SCOPE)
  set(${prefix}_DIR    "${_dir_found}" PARENT_SCOPE)
endfunction()

# The window layer and the DirectWrite shim need 32-bit FreeType, and there is
# no useful peload32 without them -- so it is decided here, before the target
# exists, rather than left to fail the link.
#
# Skipped rather than fatal, for the same reason the -m32 probe above is: a
# machine can easily have a 32-bit toolchain and no 32-bit FreeType at all --
# a Fedora box with glibc-devel.i686 pulled in by something else is the usual
# way to land here -- and a FATAL_ERROR there takes pestudio and the 64-bit
# loader down with a target neither of them needs. The .deb build insists on
# peload32 separately, in packaging/debian/rules, where a missing
# build-dependency is still the obvious explanation.
set(BUILD_PELOAD32 FALSE)
if(NOT HAVE_M32_TOOLCHAIN)
  message(STATUS "peload32 skipped (needs a -m32 toolchain and 32-bit glibc: "
                 "lib32-glibc, or gcc-multilib on Debian/Ubuntu)")
else()
  pkgconfig32(FT32 freetype2)
  if(FT32_OK EQUAL 0)
    set(BUILD_PELOAD32 TRUE)
    message(STATUS "peload32: 32-bit freetype2 from ${FT32_DIR}")
  else()
    message(STATUS "peload32 skipped (needs 32-bit freetype2: lib32-freetype2, "
                   "or libfreetype-dev:i386 on Debian/Ubuntu)")
  endif()
endif()

if(BUILD_PELOAD32)
  add_executable(peload32 pe32.c)
  # plain C, and AUTOMOC is on for pestudio's sake
  #
  # SKIP_BUILD_RPATH: the 32-bit .pc files answer with -L/usr/lib/i386-linux-gnu
  # on Debian and -L/usr/lib32 on Arch, and CMake writes any link directory it
  # does not recognise as a system one into the binary as RUNPATH. Both are
  # already in ld.so's own multiarch search path, so the entry finds nothing
  # that would not be found anyway -- and lintian rejects the .deb over it
  # (custom-library-search-path). The 64-bit peload beside it has no RUNPATH
  # for the same reason: its -L paths are ones CMake already counts as system.
  set_target_properties(peload32 PROPERTIES AUTOMOC OFF SKIP_BUILD_RPATH TRUE)
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

  # 32-bit FreeType, already resolved above -- the target only exists because
  # the query succeeded.
  separate_arguments(FT32_CFLAGS_LIST NATIVE_COMMAND "${FT32_CFLAGS}")
  separate_arguments(FT32_LIBS_LIST   NATIVE_COMMAND "${FT32_LIBS}")
  target_compile_options(peload32 PRIVATE ${FT32_CFLAGS_LIST})
  target_link_libraries(peload32 PRIVATE ${FT32_LIBS_LIST})

  # A live editor window needs 32-bit Xlib. There is no x11.pc under
  # /usr/lib32/pkgconfig on Arch, but lib32-libx11 ships the .so, and the X11
  # headers are architecture-independent -- so look for the library directly.
  # Debian/Ubuntu ship it under the i386 multiarch triplet instead.
  find_library(X11_32 NAMES X11 PATHS /usr/lib32 /usr/lib/i386-linux-gnu NO_DEFAULT_PATH)
  if(X11_32)
    target_compile_definitions(peload32 PRIVATE PELOAD32_X11=1)
    target_link_libraries(peload32 PRIVATE ${X11_32})
    message(STATUS "peload32: live editor window enabled")
  else()
    message(STATUS "peload32: no editor window (needs lib32-libx11)")
  endif()

  # Live playback needs 32-bit PipeWire and ALSA. One query for both, so either
  # one missing costs the same thing: a render-only peload32. That still serves
  # the bridge, which carries audio over shared memory and lets the 64-bit host
  # reach the sound server -- what it drops is standalone `peload32 --play`.
  pkgconfig32(PW32 libpipewire-0.3 alsa)
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
endif()

# win32_arity.h is generated from the mingw-w64 i686 import libraries; it is
# checked in, so regenerate only on request. Outside the block above because it
# needs none of it -- python3 and the mingw import libraries, not a 32-bit
# toolchain -- and the arity table is what the 64-bit stubs are checked against
# too, so it should stay reachable on a machine that cannot build peload32.
add_custom_target(arity
  COMMAND ${CMAKE_COMMAND} -E echo "regenerating win32_arity.h"
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_arity.py
  COMMENT "Regenerate the Win32 stdcall arity table")
