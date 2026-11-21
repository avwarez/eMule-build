cmake_minimum_required(VERSION 3.20)
project(eMule VERSION 0.72 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# This build targets the original MFC/Windows application, as built today by
# srchybrid/emule.vcxproj (MSVC + static MFC/ATL). It is Windows-only.
# ---------------------------------------------------------------------------

if(NOT WIN32)
    message(FATAL_ERROR "eMule (this target) is an MFC/Windows application and can only be built on Windows with MSVC.")
endif()

# ---------------------------------------------------------------------------
# Every dependency here is meant to link statically against the MSVC CRT
# (matching emule.vcxproj's own UseOfMfc=Static philosophy - MSVC_RUNTIME_LIBRARY
# is set to "MultiThreaded$<$<CONFIG:Debug>:Debug>" on every *_build target
# below). zlib, libplum and mbedtls/tf-psa-crypto have their own upstream
# CMakeLists.txt that we don't edit, and none of them set this explicitly, so
# without this they fall back to CMake's own default (dynamic CRT, /MD).
# CMP0091 (default NEW here - cmake_minimum_required is well past 3.15, the
# policy's introduction) makes CMAKE_MSVC_RUNTIME_LIBRARY, set before any
# target is created, the default for every target in every add_subdirectory()
# below unless a target overrides it itself - already the case for cryptopp,
# id3lib and resizablelib.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Control Flow Guard: /guard:cf on every C and C++ translation unit of every
# target below, ours and the upstream ones alike (directory-scoped, like the
# runtime above). The linker builds the table of valid indirect-call targets
# only from objects compiled with it. Not on MASM sources (Crypto++'s .asm on
# Win32/x64): ml/ml64 have no such option. The link side, with /CETCOMPAT, is
# in emule_build/CMakeLists.txt.
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/guard:cf>")
endif()

# ---------------------------------------------------------------------------
# Submodule patching
#
# No submodule in this repo is ever edited in place. When
# one of them needs a source change - an upstream bug, an incompatibility
# with a newer dependency, or a construct the C++ standard we compile at has
# removed - the fix lives as a .patch file in that submodule's *_build/
# directory here, and is applied at configure time by the function below.
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
#   00-87  the code does not compile as-is (a dependency's API changed, or
#          the C++ standard we build at removed the construct).
#   88     legacy features taken out: code that only ever ran on Windows
#          versions we no longer target, or that nothing calls any more.
#          Each patch takes out one thing: a feature, down to its resources
#          and ini keys (the translated strings stay, the language DLLs still
#          carry them); the Windows version detection together with every
#          branch taken on it (88x), as on every supported Windows each of
#          them goes the same way; the run-time lookups of APIs every
#          supported Windows has (88zf).
#   89     features added. 89a puts libplum in the place of miniupnpc:
#          one library that tries PCP, NAT-PMP and UPnP-IGD in turn and
#          keeps the mappings alive on its own. 89b reads the Windows
#          Firewall policy at startup and every minute after, and logs
#          whether it lets inbound connections reach the TCP and UDP ports,
#          and what stops them; while a port is blocked it shows a bar under
#          the toolbar, whose menu opens the ports through netsh, run as
#          administrator for that alone, or hides the bar.
#          89c adds PowerShare, stored in known.met the way MorphXT stores
#          it: a complete file is marked from the Shared Files list, and the
#          requests for it are served ahead of every ordinary one - unless
#          many clients have the file complete already, or its upload
#          priority was set to low or very low by hand. 89f writes what
#          PowerShare does to the debug log, and 89g keeps a PowerShare
#          upload going past the session limit while no other PowerShare
#          request or friend slot is waiting for the slot.
#          89n lets two LowIDs on the same eD2K server reach each other
#          through UDP hole punching, with natd (below) and public STUN
#          servers; nothing changes towards a HighID, nor between clients
#          where TCP works, and Kad plays no part in it. At startup, after
#          UPnP and before the server and Kad, natd's NAT discovery runs on
#          the client UDP port (the connect buttons wait for it, at most a
#          minute); the login announces Lugdunum's NAT callback (SRVCAP_
#          NATTRAVERSAL) when the client can be punched to, and a LowID
#          keeps the LowID sources on its server it can punch to instead of
#          parking them as LowID->LowID. The requester's request goes from
#          the client UDP port to the server as Lugdunum's OP_NAT_CALLBACK_*
#          (obfuscated with the server's UDP key, which a request waits for
#          just after the login; the lower ID starts, the token comes from
#          both IDs); the server forwards it with the endpoint it saw, and
#          the source sends its own request back the same way before
#          punching, so both punch endpoints seen a moment ago. An
#          endpoint-independent NAT, one with random ports in a block
#          (mobile CGNs) or sequential ports can be punched to: the punch
#          sprays the ports nearest to the endpoint it punches, more each
#          round. Unanswered requests go once more; a timed-out punch fails
#          the connection as TCP does, and the source is asked again after
#          2, 5, 15 and 29 minutes. natd hands over the path and the eD2K
#          stream runs over it in eMule's own datagrams (KCP, kcp/ below) on
#          the client UDP port, under a CClientReqSocket given a
#          CNatdSocketLayer: keepalives at natd's interval, dead after three
#          silent ones (then the public endpoint is checked again), FINs at
#          the end and when eMule closes.
#   90-94  the code compiles but /Wall flags it; these bring the warning
#          count back to zero.
#   95-96  the code compiles and is warning-clean but is not correct on
#          every target we build.
#   97     the code is wrong on every target; a file here goes away when the
#          same change reaches a released v0.72x.
#   98     corrections to the shipped language resources - source data for
#          the `languages` target rather than code.
#   99     reserved for temporary diagnostics - a patch that makes a build
#          report something rather than fix it; whatever lands here is meant
#          to be deleted once it has been read.
# ---------------------------------------------------------------------------
find_package(Git REQUIRED)

function(emule_apply_patches _submodule _patch_dir)
    file(GLOB _patches "${CMAKE_SOURCE_DIR}/${_patch_dir}/*.patch")
    list(SORT _patches)
    set(_src "${CMAKE_SOURCE_DIR}/${_submodule}")

    # Re-configuring a tree that already carries the whole series: undo the
    # patches newest-first on a throwaway index, never on the files. If every
    # one of them reverses, the series is in place and there is nothing to do.
    # The per-patch check below cannot tell on its own once two patches edit
    # neighbouring lines - reversing the older one alone then fails on the
    # context the newer one removed, and it would be applied a second time.
    # On a fresh checkout the newest patch already fails to reverse, so this
    # costs one git call and changes nothing.
    list(LENGTH _patches _n_patches)
    if(_n_patches GREATER 0)
        set(_index "${CMAKE_BINARY_DIR}/emule_apply_patches_${_submodule}.index")
        file(REMOVE "${_index}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env "GIT_INDEX_FILE=${_index}"
                    ${GIT_EXECUTABLE} -c core.autocrlf=false add -A
            WORKING_DIRECTORY "${_src}"
            RESULT_VARIABLE _snapshot_result
            OUTPUT_QUIET ERROR_QUIET
        )
        set(_series_applied FALSE)
        if(_snapshot_result EQUAL 0)
            set(_series_applied TRUE)
            set(_newest_first ${_patches})
            list(REVERSE _newest_first)
            foreach(_patch IN LISTS _newest_first)
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E env "GIT_INDEX_FILE=${_index}"
                            ${GIT_EXECUTABLE} apply --cached --ignore-whitespace --reverse "${_patch}"
                    WORKING_DIRECTORY "${_src}"
                    RESULT_VARIABLE _reverse_result
                    OUTPUT_QUIET ERROR_QUIET
                )
                if(NOT _reverse_result EQUAL 0)
                    set(_series_applied FALSE)
                    break()
                endif()
            endforeach()
        endif()
        file(REMOVE "${_index}")
        if(_series_applied)
            message(STATUS "${_submodule}: all ${_n_patches} patches in ${_patch_dir} already applied")
            return()
        endif()
    endif()

    foreach(_patch IN LISTS _patches)
        get_filename_component(_patch_name "${_patch}" NAME)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --check --reverse "${_patch}"
            WORKING_DIRECTORY "${_src}"
            RESULT_VARIABLE _already_applied
            OUTPUT_QUIET ERROR_QUIET
        )
        if(_already_applied EQUAL 0)
            message(STATUS "${_submodule}: ${_patch_dir}/${_patch_name} already applied")
        else()
            message(STATUS "${_submodule}: applying ${_patch_dir}/${_patch_name}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace "${_patch}"
                WORKING_DIRECTORY "${_src}"
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
emule_apply_patches(resizablelib resizablelib_build)

# ---------------------------------------------------------------------------
# mbedtls - vanilla git submodule (mbedtls/, with nested tf-psa-crypto/ and
# framework/ submodules), pinned to v4.3.0. Never edited in place. eMule
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
# zlib - vanilla git submodule (zlib/, madler/zlib pinned to v1.4). Has its
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
# zlib ships no Visual Studio project; its own MSVC build (win32/Makefile.msc)
# compiles at -W3, which is also what CMake passed by default before policy
# CMP0092. zlib's CMakeLists.txt sets that policy NEW and names no warning level
# of its own, so it is stated here to keep the same level as upstream intends.
target_compile_options(zlibstatic PRIVATE $<$<C_COMPILER_ID:MSVC>:/W3>)

# ---------------------------------------------------------------------------
# id3lib - git submodule (id3lib/, avwarez/id3lib pinned to v3.9.1),
# built via our own id3lib_build/CMakeLists.txt since the upstream repo ships
# no CMake support of its own (see that file for why).
# ---------------------------------------------------------------------------
add_subdirectory(id3lib_build)

# ---------------------------------------------------------------------------
# libplum - git submodule (libplum/, avwarez/libplum pinned to master-mapping-refresh:
# upstream master, which already has the UPnP 725/714 fixes, plus the two
# commits of the still open upstream PR #59 - UPnP mappings are checked with
# GetSpecificPortMappingEntry every recheck period and made again only when
# gone, and plum_refresh_mapping() checks one at once), for the port mapping
# emule_build/89a adds (PCP, NAT-PMP and UPnP-IGD). Has its own CMakeLists.txt.
# Two things in it must not reach the rest of this build:
# - option(BUILD_SHARED_LIBS ... ON) would put ON in the cache, and every
#   add_library() without STATIC/SHARED added after it would become a DLL.
#   Set OFF first, which is what an unset BUILD_SHARED_LIBS already means.
# - the shared plum target and the example program are built by default;
#   EXCLUDE_FROM_ALL keeps them out, and plum-static (already EXCLUDE_FROM_ALL
#   upstream) is still built because emule links it. It declares PLUM_STATIC
#   PUBLIC, so eMule's #include of plum.h does not ask for __declspec(dllimport).
# Upstream builds it on MSVC with no warning level of its own (CMP0092 NEW):
# stated as /W3 here, as for zlib.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Every vendored library is built static" FORCE)
set(PLUM_NO_EXAMPLE ON CACHE BOOL "Don't build libplum's own example program" FORCE)
add_subdirectory(libplum EXCLUDE_FROM_ALL)
target_compile_options(plum-static PRIVATE $<$<C_COMPILER_ID:MSVC>:/W3>)

# ---------------------------------------------------------------------------
# natd - git submodule (natd/, avwarez/natd pinned to v0.6.0), the NAT discovery and UDP
# hole punching that emule_build/89n adds between two LowIDs: sans-I/O C++17
# (eMule moves the datagrams and the time), no dependencies. eMule also uses
# two of its platform adapters: the Winsock sockets of the discovery samples,
# and the resolution of the STUN server names on a thread of its own. It ends
# where the hole is open: what goes over the punched
# path is eMule's (KCP, below). Has its own CMakeLists.txt; its examples
# and tests stay out. Built at the standard eMule is built at, so that its
# headers mean the same in its own translation units and in eMule's.
# ---------------------------------------------------------------------------
set(NATD_EXAMPLES OFF CACHE BOOL "Don't build natd's example tools" FORCE)
set(NATD_TESTS OFF CACHE BOOL "Don't build natd's tests" FORCE)
add_subdirectory(natd EXCLUDE_FROM_ALL)
target_compile_features(natd PRIVATE cxx_std_23)

# ---------------------------------------------------------------------------
# KCP - vanilla git submodule (kcp/, skywind3000/kcp pinned to 2.1.1, MIT): the
# transport of the eD2K stream over a punched path (CNatdKcpTransport in NatTraversal.cpp,
# which includes "kcp/ikcp.h" through CMAKE_SOURCE_DIR). One C file, built here.
# ---------------------------------------------------------------------------
enable_language(C)
add_library(kcp STATIC kcp/ikcp.c)
target_compile_options(kcp PRIVATE $<$<C_COMPILER_ID:MSVC>:/W3>)

# ---------------------------------------------------------------------------
# ResizableLib - git submodule (resizablelib/, avwarez/resizablelib pinned to
# branch 20260917, which carries the High DPI work done on top of v1.5.3),
# built via our own resizablelib_build/CMakeLists.txt since the upstream repo
# ships no CMake support of its own (see that file for why). That file reads
# the source list out of ResizableLib.vcxproj rather than listing it, so the
# file this branch adds - ResizableDialogEx.cpp - is picked up on its own.
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
