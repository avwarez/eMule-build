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
# mbedtls - vanilla git submodule (mbedtls/, with nested tf-psa-crypto/ and
# framework/ submodules), pinned to v4.2.0. Never edited in place; see
# mbedtls_overrides/README.md for how eMule's two required customizations
# (threading_alt.h and enabling MBEDTLS_THREADING_ALT) are supplied from
# outside the submodule instead, using mechanisms mbedtls itself documents
# for exactly this purpose.
# ---------------------------------------------------------------------------
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/mbedtls_overrides/mbedtls/emule_mbedtls_user_config.h"
    CACHE FILEPATH "" FORCE)
# mbedTLS 4.0 cross-checks the legacy config against the PSA crypto config
# and errors ("MBEDTLS_THREADING_ALT must be configured in psa/crypto_config.h")
# if a threading option is set on only one side - confirmed by an actual
# build attempt with only MBEDTLS_USER_CONFIG_FILE set above.
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/mbedtls_overrides/mbedtls/emule_psa_crypto_user_config.h"
    CACHE FILEPATH "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Don't build mbedtls's own example programs" FORCE)

# GEN_FILES defaults OFF on Windows (mbedtls's CMakeLists.txt assumes a
# release tarball there, which ships its generated sources pre-built). A git
# submodule checkout has none of them, so this must be forced ON - confirmed
# necessary by an actual configure attempt (failed on missing
# psa_crypto_driver_wrappers_no_static.c / error.c / ssl_debug_helpers_generated.c
# without it). Requires Python 3 on PATH.
set(GEN_FILES ON CACHE BOOL "Generate mbedtls's auto-generated sources (required for a submodule checkout)" FORCE)

# Directory-scoped (not target_include_directories): threading.c, compiled
# deep inside tf-psa-crypto's own CMake targets, needs to see this override
# directory too - include_directories() here is inherited by everything
# add_subdirectory()'d below, without having to chase mbedtls's internal
# target names across versions.
include_directories("${CMAKE_SOURCE_DIR}/mbedtls_overrides/mbedtls")

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
# miniupnpc - vanilla git submodule (miniupnp/, miniupnp/miniupnp pinned to
# tag miniupnpc_2_3_3). That's the upstream monorepo (also contains
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
# branch v0.72a), never edited in place. Two upstream mbedtls-4.0 compile
# errors in emule/srchybrid/WebSocket.cpp (CString->CStringA path conversion
# for mbedtls_x509_crt_parse_file()/mbedtls_pk_parse_keyfile(), and
# mbedtls_sha1() having moved to a private header) are fixed by applying
# patches/websocket-mbedtls4.patch below instead, since v0.72a itself stays
# untouched. Idempotent: `git apply --check --reverse` succeeds once the
# patch is already applied, so a second configure run doesn't reapply it.
# ---------------------------------------------------------------------------
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --check --reverse "${CMAKE_SOURCE_DIR}/patches/websocket-mbedtls4.patch"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/emule"
    RESULT_VARIABLE _websocket_patch_already_applied
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT _websocket_patch_already_applied EQUAL 0)
    message(STATUS "eMule: applying patches/websocket-mbedtls4.patch to emule/srchybrid/WebSocket.cpp")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply "${CMAKE_SOURCE_DIR}/patches/websocket-mbedtls4.patch"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/emule"
        RESULT_VARIABLE _websocket_patch_result
    )
    if(NOT _websocket_patch_result EQUAL 0)
        message(FATAL_ERROR "eMule: failed to apply patches/websocket-mbedtls4.patch to emule/srchybrid/WebSocket.cpp")
    endif()
else()
    message(STATUS "eMule: patches/websocket-mbedtls4.patch already applied")
endif()

add_subdirectory(srchybrid_build)
