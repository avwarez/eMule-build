cmake_minimum_required(VERSION 3.20)
project(eMule VERSION 0.60 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# This build targets the original MFC/Windows application, as built today by
# srchybrid/emule.vcxproj (MSVC + static MFC/ATL). It is Windows-only.
#
# This branch builds eMule 0.60d rather than 0.72a. The two differ in more
# than source: 0.60d predates the library sweep the 0.72a release carried out
# ("updated MbedTLS to version 4.0, removed CxImage and libpng" is the whole
# of that commit's second paragraph), so two dependencies exist here that
# exist on no other branch - CxImage and libpng - and mbedtls is pinned to the
# 2.28 series whose API this source actually speaks. 0.60d also ships no ARM64
# manifest and its project file declares Win32 and x64 only, which is what the
# CI matrix on this branch builds.
#
# All third-party dependencies (mbedtls, cryptopp, id3lib, zlib, libpng,
# cximage, miniupnpc, resizablelib) are vendored as vanilla git submodules -
# see each add_subdirectory() below and the corresponding *_build/
# CMakeLists.txt for the ones with no CMake support of their own.
# ---------------------------------------------------------------------------

if(NOT WIN32)
    message(FATAL_ERROR "eMule (this target) is an MFC/Windows application and can only be built on Windows with MSVC.")
endif()

# ---------------------------------------------------------------------------
# Every dependency here is meant to link statically against the MSVC CRT
# (matching emule.vcxproj's own UseOfMfc=Static philosophy - MSVC_RUNTIME_LIBRARY
# is set to "MultiThreaded$<$<CONFIG:Debug>:Debug>" on every *_build target
# below). zlib, libpng, miniupnpc and mbedtls have their own upstream
# CMakeLists.txt that we don't edit, and none of them set this explicitly, so
# without this they fall back to CMake's own default (dynamic CRT, /MD) -
# confirmed by an actual build attempt: LNK2001 on __imp_close/__imp_read/
# __imp_open/__imp_write/__imp_setbuf/__imp__memicmp (DLL-import CRT thunks
# that only exist when linking the dynamic CRT) from zs.lib/libminiupnpc.lib/
# tfpsacrypto.lib, mixed into an otherwise fully statically-linked emule.exe.
# CMP0091 (default NEW here - cmake_minimum_required is well past 3.15, the
# policy's introduction) makes CMAKE_MSVC_RUNTIME_LIBRARY, set before any
# target is created, the default for every target in every add_subdirectory()
# below unless a target overrides it itself - already the case for cryptopp,
# id3lib, cximage, resizablelib and zlibstatic.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Submodule patching
#
# Every submodule in this repo is vanilla and is never edited in place. When
# one of them needs a source change - an upstream bug, an incompatibility
# with a newer dependency, or a construct the C++ standard we compile at has
# removed - the fix lives as a .patch file in that submodule's *_build/
# directory here, and is applied at configure time by the function below.
#
# Idempotent: `git apply --check --reverse` succeeds exactly when a patch is
# already applied, so re-configuring an existing build tree is a no-op
# instead of a failure.
#
# --ignore-whitespace: these patches are generated against the repository's
# own bytes (LF line endings), but a Windows checkout with core.autocrlf=true
# - the default on GitHub's windows runner images - rewrites the working tree
# to CRLF, and git apply then refuses the LF context. Confirmed by an actual
# reproduction: with a CRLF working tree and core.autocrlf=false, every patch
# here fails with "patch does not apply"; --ignore-whitespace makes all of
# them apply in both cases. The CI workflow additionally pins
# core.autocrlf=false so the tree matches the patches byte for byte. Patches
# within one directory are applied in sorted filename order; give them a
# numeric prefix if two of them ever have to touch the same hunk in a defined
# sequence.
#
# emule_build/ started empty on this branch. The patches on v0.72a are written
# against 0.72a's bytes and describe 0.72a's faults; not one of them applies to
# this tree, and carrying them across unread would say things about this build
# that nobody has checked. What 0.60d turns out to need is written here from
# what building 0.60d actually reports, under the same numbering:
#   00-89  the code does not compile as-is (a dependency's API changed, or
#          the C++ standard we build at removed the construct).
#   90-94  the code compiles but /Wall flags it; these bring the warning
#          count back to zero.
#   95-96  the code compiles and is warning-clean but is not correct on
#          every target we build, or in every environment we run it in
#          (see each patch's own subject).
#   97     the code is wrong on every target, on input or on timing a peer
#          or a web client controls - the band that is worth reporting
#          upstream.
#   98     corrections to the shipped language resources - source data for
#          the `languages` target rather than code, so they change no binary.
#   99     reserved for temporary diagnostics - a patch that makes a build
#          report something rather than fix it, deleted once it has been read.
#
# Four files so far, all in the first band, and every one of them is a place
# where something outside this source moved after 0.60d was released. Three of
# the four have a later eMule release as their reference: the same problem
# reached the eMule devs in time for 0.72a and the fix here is theirs, copied
# rather than invented.
#
#   00-miniupnpc-include-path corrects where three headers are looked for.
#          0.60d asks for "miniupnpc\miniupnpc.h" and its two neighbours;
#          miniupnpc moved its public headers into an include/ subdirectory,
#          and the pinned 2.3.3 has them at miniupnpc/include/miniupnpc.h with
#          nothing at the old path. 0.72a's copy of this file already spells it
#          "miniupnpc\include\miniupnpc.h", so this patch is that change and
#          nothing else, and it goes away the day 0.60d stops being built
#          against a miniupnpc this new.
#
#   10-mfc-interface-part-noexcept takes the noexcept off nine method
#          definitions - and off exactly those nine. An MFC interface part
#          declares QueryInterface, AddRef and Release itself, from inside the
#          BEGIN_INTERFACE_PART macro, and from VS2022 17.7 onwards it declares
#          them without an exception specification; 0.60d defines all three
#          with noexcept, which is a different exception specification from the
#          declaration and therefore C2382. Every other method of those same
#          interface parts is declared by eMule's own STDMETHOD lines, whose
#          spelling the definitions match, and those are left alone. The fix is
#          not a deletion but the macro 0.72a introduced for this, NOEXCEPT,
#          defined in emule_site_config.h to expand to noexcept below 17.7 and
#          to nothing from 17.7 on - taken verbatim, so the file still builds
#          on the older toolset it was written for.
#
#   20-mediainfo-floating-point-abs replaces six calls to abs() whose argument
#          is a double or a float. Integer abs() overloads for long and
#          __int64 were added to the UCRT's stdlib.h, and a floating-point
#          argument is now an ambiguous call rather than a silent conversion.
#          The four double sites become fabs and the two float sites become
#          fabsf with the division done in float, which is what 0.72a does at
#          the same six places. Note what the old code actually computed: abs()
#          on a ratio truncated it to an integer before taking its magnitude,
#          so every aspect ratio between -1 and 1 came out as 0. The compiler
#          error is the reason to touch these lines; it is not the whole of
#          what was wrong with them.
#
#   30-mbedtls-api-mismatches fixes three calls that do not compile against
#          any released mbedtls, this one or the one 0.60d shipped against.
#          mbedtls_pk_rsa() takes the context by value and is handed a
#          pointer - the fix is one character. mbedtls_x509_crt_parse_file()
#          and mbedtls_pk_parse_keyfile() take a const char* and are handed
#          the CString the preferences hold, which in a Unicode build is wide;
#          they get a UTF-8 copy instead. eMule-build already carries the
#          second of these as a patch on v0.72a, where it is spelled the same
#          way, because the same two lines are still there.
# ---------------------------------------------------------------------------
find_package(Git REQUIRED)

function(emule_apply_patches _submodule _patch_dir)
    file(GLOB _patches "${CMAKE_SOURCE_DIR}/${_patch_dir}/*.patch")
    list(SORT _patches)
    foreach(_patch IN LISTS _patches)
        get_filename_component(_patch_name "${_patch}" NAME)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --check --reverse "${_patch}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/${_submodule}"
            RESULT_VARIABLE _already_applied
            OUTPUT_QUIET ERROR_QUIET
        )
        if(_already_applied EQUAL 0)
            message(STATUS "${_submodule}: ${_patch_dir}/${_patch_name} already applied")
        else()
            message(STATUS "${_submodule}: applying ${_patch_dir}/${_patch_name}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace "${_patch}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/${_submodule}"
                RESULT_VARIABLE _apply_result
            )
            if(NOT _apply_result EQUAL 0)
                message(FATAL_ERROR "${_submodule}: failed to apply ${_patch_dir}/${_patch_name}")
            endif()
        endif()
    endforeach()
endfunction()

# Applied up front, before any add_subdirectory() below - emule_build's own
# CMakeLists.txt reads emule.vcxproj at configure time, so a patch must never
# land after the directory that consumes it has already been processed.
# An empty (or patch-less) *_build directory is simply a no-op here, so these
# calls stay valid whether or not that submodule currently needs anything.
emule_apply_patches(emule emule_build)
emule_apply_patches(cryptopp cryptopp_build)
emule_apply_patches(id3lib id3lib_build)
emule_apply_patches(cximage cximage_build)
emule_apply_patches(resizablelib resizablelib_build)
emule_apply_patches(miniupnp miniupnpc_build)

# ---------------------------------------------------------------------------
# mbedtls - vanilla git submodule (mbedtls/), pinned to mbedtls-2.28.10 on this
# branch rather than to the 4.2.0 that v0.72a uses. That is not a preference,
# it is what the source asks for: 0.60d includes "mbedtls/config.h", a file
# that exists in the 2.x series and in no later one, and WebSocket.cpp reads
# ssl->p_bio directly, a field that 3.0 moved behind MBEDTLS_PRIVATE. Both
# would have to be rewritten to build against 4.x - which is exactly the work
# the 0.72a release did, and it is 0.72a's, not ours to redo here.
#
# What that costs is worth stating plainly: the 2.28 series reached end of
# life at the end of 2024 and receives no further fixes. This branch exists to
# find out whether 0.60d still builds, not to ship a client; moving it to a
# supported mbedtls means porting the five files above, and that is a decision
# on its own, not a side effect of this one.
#
# eMule needs one customization on top of the submodule, supplied from outside
# it via an extension point mbedTLS itself documents for this purpose, so
# there is nothing to re-apply after updating the submodule to a newer tag:
# mbedtls_build/emule_mbedtls_user_config.h defines MBEDTLS_THREADING_ALT,
# wired in below via MBEDTLS_USER_CONFIG_FILE. threading_alt.h (that option's
# actual implementation) is not duplicated here - see the include_directories()
# call below.
# ---------------------------------------------------------------------------
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/mbedtls_build/emule_mbedtls_user_config.h"
    CACHE FILEPATH "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Don't build mbedtls's own example programs" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "Don't build mbedtls's own test suites" FORCE)
# mbedtls turns compiler warnings into errors by default, MSVC included. The
# 2.28 series was last released against the compilers of 2024; this build uses
# whatever toolset the runner image ships, and a warning that series never saw
# is not a reason to fail a build we are not changing the sources of.
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "A new compiler's new warnings are not build failures here" FORCE)

# threading_alt.h (eMule's own mbedtls_platform_mutex_t/condition_variable_t
# implementation on top of Win32 CRITICAL_SECTION, consumed as an unqualified
# #include "threading_alt.h" by both mbedtls's threading.c and
# srchybrid/TLSthreading.h) is not vendored in this repo: the eMule devs
# already ship the exact same file inside the emule submodule itself, at
# emule/mbedtls/include/mbedtls/threading_alt.h - a leftover from the
# project's pre-CMake build setup, where the mbedtls tree was checked out at
# that path and this file dropped into it. Pointing include_directories()
# straight at it avoids a second copy to keep in sync. Directory-scoped (not
# target_include_directories): threading.c, compiled deep inside mbedtls's own
# CMake targets, needs to see it too - include_directories() here is inherited
# by everything add_subdirectory()'d below, without having to chase mbedtls's
# internal target names across versions.
include_directories("${CMAKE_SOURCE_DIR}/emule/mbedtls/include/mbedtls")

# ---------------------------------------------------------------------------
# Crypto++ - vanilla git submodule (cryptopp/, weidai11/cryptopp pinned to
# CRYPTOPP_8_9_0), built via our own cryptopp_build/CMakeLists.txt since the
# upstream repo ships no CMake support of its own (see that file for why).
# ---------------------------------------------------------------------------
add_subdirectory(cryptopp_build)

# ---------------------------------------------------------------------------
# zlib - vanilla git submodule (zlib/, madler/zlib pinned to v1.3.2). Has its
# own CMakeLists.txt, so add_subdirectory() alone is enough; only the static
# lib is needed (matches every other dependency here - MFC linked static) and
# ZLIB_INSTALL would otherwise add install() rules we don't want for a
# vendored submodule. Built before id3lib_build and libpng: both link against
# this zlib (zlibstatic target) rather than bringing their own copy - id3lib
# would otherwise use its bundled zlib 1.1.4, see id3lib_build/CMakeLists.txt.
# ---------------------------------------------------------------------------
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "eMule only needs the static zlib lib" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "eMule only needs the static zlib lib" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Don't build zlib's own example programs" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "Don't add install() rules for a vendored submodule" FORCE)
add_subdirectory(zlib)

# zlib publishes ZLIB::ZLIB only when it builds the shared library, which this
# build turns off; the static target it does build is called zlibstatic and
# carries the aliases ZLIB::ZLIBSTATIC. libpng below asks for zlib by the
# canonical name, so give it one that points at the copy already configured
# above. Guarded rather than assumed, so a future zlib that publishes the
# alias itself does not turn this into a duplicate-target error.
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# ---------------------------------------------------------------------------
# libpng - vanilla git submodule (libpng/, pnggroup/libpng pinned to v1.6.58),
# with its own CMakeLists.txt, used as-is. Needed by CxImage below and, one
# step further on, by the preview images eMule 0.60d exchanges with other
# clients.
#
# The one thing upstream's CMakeLists.txt does that does not fit here is
# find_package(ZLIB REQUIRED), which would go looking for an installed zlib.
# libpng_build/FindZLIB.cmake answers that question with the submodule
# configured just above instead; putting that directory on CMAKE_MODULE_PATH
# is what makes find_package() reach it before CMake's own module.
#
# pnglibconf.h is generated at configure time. Upstream prefers to build it
# with awk and falls back to scripts/pnglibconf.h.prebuilt when there is no
# awk on PATH - which is the case on the Windows runner - and that prebuilt
# file is the stock configuration, which is what this build wants anyway.
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/libpng_build")
set(PNG_SHARED OFF CACHE BOOL "eMule only needs the static libpng" FORCE)
set(PNG_STATIC ON CACHE BOOL "eMule only needs the static libpng" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "Apple-only, and this build is Windows-only" FORCE)
set(PNG_TESTS OFF CACHE BOOL "Don't build libpng's own test programs" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "Don't build libpng's own command line tools" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "Deprecated spelling of PNG_TOOLS; set for older libpng" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "Don't add install() rules for a vendored submodule" FORCE)
add_subdirectory(libpng)

# ---------------------------------------------------------------------------
# CxImage - vanilla git submodule (cximage/, avwarez/CxImage pinned to branch
# 7.02-lite-refresh-2026, which is CxImage 7.02 "lite" with the changes a
# modern MSVC and libpng 1.6 need), built via our own cximage_build/
# CMakeLists.txt since the upstream repo ships no CMake support of its own.
# Built after libpng: that is the one codec this configuration enables.
# ---------------------------------------------------------------------------
add_subdirectory(cximage_build)

# ---------------------------------------------------------------------------
# id3lib - vanilla git submodule (id3lib/, avwarez/id3lib pinned to v3.9.1),
# built via our own id3lib_build/CMakeLists.txt since the upstream repo ships
# no CMake support of its own (see that file for why).
# ---------------------------------------------------------------------------
add_subdirectory(id3lib_build)

# ---------------------------------------------------------------------------
# miniupnpc - near-vanilla git submodule (miniupnp/, miniupnp/miniupnp pinned
# to tag miniupnpc_2_3_3), carrying one warning patch from miniupnpc_build/.
# That's the upstream monorepo (also contains
# miniupnpd, minissdpd, ...) - only its miniupnpc/ subdirectory is built,
# which has its own CMakeLists.txt (add_subdirectory() alone is enough).
# Static lib only, no samples/tests/install, matching every other dependency
# here.
# ---------------------------------------------------------------------------
set(UPNPC_BUILD_STATIC ON CACHE BOOL "eMule only needs the static miniupnpc lib" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "eMule only needs the static miniupnpc lib" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "Don't build miniupnpc's own test executables" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "Don't build miniupnpc's own sample executables" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "Don't add install() rules for a vendored submodule" FORCE)
add_subdirectory(miniupnp/miniupnpc)

# ---------------------------------------------------------------------------
# ResizableLib - git submodule (resizablelib/, avwarez/resizablelib pinned to
# branch 20260917, which carries the High DPI work done on top of v1.5.3),
# built via our own resizablelib_build/CMakeLists.txt since the upstream repo
# ships no CMake support of its own (see that file for why). That file reads
# the source list out of ResizableLib.vcxproj rather than listing it, so the
# file this branch adds - ResizableDialogEx.cpp - is picked up on its own.
# ---------------------------------------------------------------------------
add_subdirectory(resizablelib_build)

# mbedtls 2.28's own CMakeLists.txt opens with cmake_minimum_required(VERSION
# 2.8.12). CMake 3.31 answers that with a deprecation warning; CMake 4 removed
# compatibility with anything below 3.5 outright and refuses to configure at
# all. The runner image decides which of the two we get, and we do not edit
# the submodule, so the documented escape hatch is set for the duration of
# this one add_subdirectory() and taken away again: it says which policy
# version to assume for a project that asks for an older one, and it must not
# be left standing for the rest of the tree, where an old
# cmake_minimum_required is something we would want to hear about.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(mbedtls)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# ---------------------------------------------------------------------------
# eMule sources - vanilla git submodule (emule/, avwarez/eMule pinned to
# branch v0.60d), never edited in place. Anything it needs on top of that
# branch is supplied as patch files under emule_build/, applied by
# emule_apply_patches() near the top of this file.
# ---------------------------------------------------------------------------
add_subdirectory(emule_build)
