cmake_minimum_required(VERSION 3.20)
project(eMule VERSION 0.72 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# This build targets the original MFC/Windows application, as built today by
# srchybrid/emule.vcxproj (MSVC + static MFC/ATL). It is Windows-only.
#
# All third-party dependencies (mbedtls, cryptopp, id3lib, zlib, miniupnpc,
# resizablelib) are vendored as vanilla git submodules - see each
# add_subdirectory() below and the corresponding *_build/CMakeLists.txt for
# the ones with no CMake support of their own.
# ---------------------------------------------------------------------------

if(NOT WIN32)
    message(FATAL_ERROR "eMule (this target) is an MFC/Windows application and can only be built on Windows with MSVC.")
endif()

# ---------------------------------------------------------------------------
# Every dependency here is meant to link statically against the MSVC CRT
# (matching emule.vcxproj's own UseOfMfc=Static philosophy - MSVC_RUNTIME_LIBRARY
# is set to "MultiThreaded$<$<CONFIG:Debug>:Debug>" on every *_build target
# below). zlib, miniupnpc and mbedtls/tf-psa-crypto have their own upstream
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
# id3lib, resizablelib and zlibstatic.
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
# core.autocrlf=false so the tree matches the patches byte for byte. Patches within one directory are applied in sorted
# filename order; give them a numeric prefix if two of them ever have to
# touch the same hunk in a defined sequence.
#
# The numeric prefixes in emule_build/ also group the patches by why they
# exist, which is the first thing you want to know when one of them stops
# applying after a submodule bump:
#   00-89  the code does not compile as-is (a dependency's API changed, or
#          the C++ standard we build at removed the construct).
#   90-94  the code compiles but /Wall flags it; these bring the warning
#          count back to zero.
#   95-98  the code compiles and is warning-clean but is not correct on
#          every target we build (see each patch's own subject).
#   99     reserved for temporary diagnostics - a patch that makes a build
#          report something rather than fix it. Nothing lives here now;
#          whatever lands here is meant to be deleted once it has been read.
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
# The first four are the C++ submodules whose headers eMule actually includes:
# eMule is the only target that compiles at C++23 (see emule_build/
# CMakeLists.txt), but cryptopp/id3lib/resizablelib headers get pulled into
# its translation units and are therefore parsed under that same standard.
# miniupnp is plain C and is otherwise built vanilla (see further down); it is
# listed here only so its one warning fix has somewhere to live.
emule_apply_patches(emule emule_build)
emule_apply_patches(cryptopp cryptopp_build)
emule_apply_patches(id3lib id3lib_build)
emule_apply_patches(resizablelib resizablelib_build)
emule_apply_patches(miniupnp miniupnpc_build)

# ---------------------------------------------------------------------------
# mbedtls - vanilla git submodule (mbedtls/, with nested tf-psa-crypto/ and
# framework/ submodules), pinned to v4.2.0. Never edited in place. eMule
# needs two customizations on top of it, both supplied from outside the
# submodule via extension points mbedTLS itself documents for this purpose,
# so there is nothing to re-apply after updating the submodule to a newer
# tag:
# - mbedtls_build/emule_mbedtls_user_config.h defines MBEDTLS_THREADING_ALT,
#   wired in below via MBEDTLS_USER_CONFIG_FILE.
# - threading_alt.h (MBEDTLS_THREADING_ALT's actual implementation) is not
#   duplicated here - see the include_directories() call below.
# ---------------------------------------------------------------------------
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/mbedtls_build/emule_mbedtls_user_config.h"
    CACHE FILEPATH "" FORCE)
# mbedTLS 4.0 cross-checks the legacy config against the PSA crypto config
# and errors ("MBEDTLS_THREADING_ALT must be configured in psa/crypto_config.h")
# if a threading option is set on only one side - confirmed by an actual
# build attempt with only MBEDTLS_USER_CONFIG_FILE set above.
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/mbedtls_build/emule_psa_crypto_user_config.h"
    CACHE FILEPATH "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Don't build mbedtls's own example programs" FORCE)

# GEN_FILES defaults OFF on Windows (mbedtls's CMakeLists.txt assumes a
# release tarball there, which ships its generated sources pre-built). A git
# submodule checkout has none of them, so this must be forced ON - confirmed
# necessary by an actual configure attempt (failed on missing
# psa_crypto_driver_wrappers_no_static.c / error.c / ssl_debug_helpers_generated.c
# without it). Requires Python 3 on PATH.
set(GEN_FILES ON CACHE BOOL "Generate mbedtls's auto-generated sources (required for a submodule checkout)" FORCE)

# threading_alt.h (eMule's own mbedtls_platform_mutex_t/condition_variable_t
# implementation on top of Win32 CRITICAL_SECTION, consumed as an unqualified
# #include "threading_alt.h" by both mbedtls/tf-psa-crypto's threading.c and
# srchybrid/TLSthreading.h) is not vendored in this repo: the eMule devs
# already ship the exact same file inside the emule submodule itself, at
# emule/mbedtls/tf-psa-crypto/include/mbedtls/threading_alt.h - a leftover
# from the project's pre-CMake build setup. Pointing include_directories()
# straight at it avoids a second copy to keep in sync. Directory-scoped (not
# target_include_directories): threading.c, compiled deep inside
# tf-psa-crypto's own CMake targets, needs to see it too - include_directories()
# here is inherited by everything add_subdirectory()'d below, without having
# to chase mbedtls's internal target names across versions.
include_directories("${CMAKE_SOURCE_DIR}/emule/mbedtls/tf-psa-crypto/include/mbedtls")

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
# vendored submodule. Built before id3lib_build: id3lib links against this
# zlib (zlibstatic target) instead of its own bundled zlib 1.1.4 copy - see
# id3lib_build/CMakeLists.txt.
# ---------------------------------------------------------------------------
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "eMule only needs the static zlib lib" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "eMule only needs the static zlib lib" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Don't build zlib's own example programs" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "Don't add install() rules for a vendored submodule" FORCE)
add_subdirectory(zlib)

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
# ResizableLib - vanilla git submodule (resizablelib/, ppescher/resizablelib
# pinned to v1.5.3), built via our own resizablelib_build/CMakeLists.txt
# since the upstream repo ships no CMake support of its own (see that file
# for why).
# ---------------------------------------------------------------------------
add_subdirectory(resizablelib_build)

add_subdirectory(mbedtls)

# ---------------------------------------------------------------------------
# eMule sources - vanilla git submodule (emule/, avwarez/eMule pinned to
# branch v0.72a), never edited in place. Everything it needs on top of that
# tag is supplied as patch files under emule_build/, applied by
# emule_apply_patches() near the top of this file.
# ---------------------------------------------------------------------------
add_subdirectory(emule_build)
