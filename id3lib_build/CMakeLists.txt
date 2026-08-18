cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# id3lib (avwarez/id3lib @ v3.9.1, vanilla submodule at eMule/id3lib) has no
# CMakeLists.txt of its own - only libprj/id3lib.vcxproj (MSVC, static lib)
# plus autotools. This file is our own, transcribed from that vcxproj, same
# approach as cryptopp_build/CMakeLists.txt. The submodule itself is never
# edited.
#
# id3lib bundles its own private copy of zlib 1.1.4 under id3lib/zlib, but
# that copy is never built here: id3lib's zlib usage (io_decorators.cpp/.h,
# frame_render.cpp - just compress()/uncompress()/Z_OK/uLong, all unchanged
# since 1.1.x) is instead pointed at eMule's own zlib submodule (zlib/,
# pinned to v1.3.2, added to the top-level CMakeLists.txt before this
# directory), via the target_link_libraries(id3lib ... zlibstatic) below -
# one zlib in the final binary instead of two.
# ---------------------------------------------------------------------------
set(ID3LIB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../id3lib")

# ---------------------------------------------------------------------------
# id3lib itself
# ---------------------------------------------------------------------------

# Source list - read directly from libprj/id3lib.vcxproj so this can never
# drift (same technique as srchybrid/CMakeLists.txt and cryptopp_build).
file(STRINGS "${ID3LIB_SRC_DIR}/libprj/id3lib.vcxproj" _clcompile_lines
     REGEX "<ClCompile Include=\"[^\"]+\\.cpp\"")

set(ID3LIB_SOURCES "")
foreach(_line IN LISTS _clcompile_lines)
    string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" _src "${_line}")
    string(REPLACE "\\" "/" _src "${_src}")
    # Paths in the vcxproj are relative to libprj/ (e.g. "..\src\tag.cpp").
    string(REGEX REPLACE "^\\.\\./" "" _src "${_src}")
    list(APPEND ID3LIB_SOURCES "${ID3LIB_SRC_DIR}/${_src}")
endforeach()

list(LENGTH ID3LIB_SOURCES _n_sources)
message(STATUS "id3lib: ${_n_sources} source files read from libprj/id3lib.vcxproj")

# config.h - upstream ships config.h.win32 and documents (libprj/win32.readme.
# first.txt) renaming it to config.h before building on Windows; HAVE_CONFIG_H
# below makes id3lib's sources look for it via `#include <config.h>` (angle
# brackets - resolved via the include path, not the including file's own
# directory). Copied into the build tree at configure time rather than into
# the submodule's working tree, so the submodule stays untouched.
configure_file("${ID3LIB_SRC_DIR}/config.h.win32" "${CMAKE_CURRENT_BINARY_DIR}/id3lib_config/config.h" COPYONLY)

add_library(id3lib STATIC ${ID3LIB_SOURCES})

target_include_directories(id3lib
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/id3lib_config"
        "${ID3LIB_SRC_DIR}"
    PUBLIC
        "${ID3LIB_SRC_DIR}/include"
)

# ID3LIB_LINKOPTION=1 (LINKOPTION_STATIC, see include/id3/globals.h) is PUBLIC:
# any translation unit that includes an id3lib public header - not just this
# library's own sources - must see the same value, or globals.h errors out
# with "read message above or win32.readme.first.txt".
target_compile_definitions(id3lib
    PRIVATE
        _LIB
        HAVE_CONFIG_H
        $<$<CONFIG:Release>:NDEBUG>
        # CharacterSet=Unicode in every configuration of the vcxproj (MSBuild's
        # automatic UNICODE/_UNICODE). id3lib itself never uses TCHAR/LPCTSTR
        # (grep confirms none in include/ or src/), so this has no effect on
        # its own symbols - added for fidelity/consistency only.
        UNICODE
        _UNICODE
    PUBLIC
        ID3LIB_LINKOPTION=1
)

# WarningLevel=Level3, AdditionalOptions and the Debug/Release-specific
# optimization flags (BasicRuntimeChecks/StringPooling/FunctionLevelLinking/
# IntrinsicFunctions/OmitFramePointers) from libprj/id3lib.vcxproj.
target_compile_options(id3lib PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4244>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4018>
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:throwingNew>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/RTC1>          # BasicRuntimeChecks=EnableFastChecks
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GF>          # StringPooling
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gy>          # FunctionLevelLinking
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Oi>          # IntrinsicFunctions
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Oy>          # OmitFramePointers
)

set_target_properties(id3lib PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# PUBLIC: include/id3/io_decorators.h (a public id3lib header) itself
# `#include "zconf.h"` for the uLong typedef, so consumers of id3lib's public
# headers (srchybrid) need zlibstatic's include dirs too, not just id3lib's
# own sources - PUBLIC here propagates both those include dirs and the
# zlibstatic link dependency to anything that links id3lib.
target_link_libraries(id3lib PUBLIC zlibstatic)
