cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# ResizableLib (ppescher/resizablelib @ v1.5.3, vanilla submodule at
# eMule/resizablelib) has no top-level CMakeLists.txt - only
# ResizableLib/ResizableLib.vcxproj (MSVC). This file is our own, transcribed
# from that vcxproj's "... Static Unicode" configurations (static MFC,
# Unicode - matches how srchybrid/CMakeLists.txt builds emule itself), same
# approach as cryptopp_build and id3lib_build. The submodule itself is never
# edited.
#
# The upstream repo groups unrelated demo projects (ResizableDialog/,
# ResizableFormViewMDI/, ...) as siblings of the actual library directory
# (ResizableLib/) - only the latter is built here.
# ---------------------------------------------------------------------------
set(RESIZABLELIB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../resizablelib/ResizableLib")

# Source list - read directly from ResizableLib.vcxproj so this can never
# drift (same technique as srchybrid/CMakeLists.txt, cryptopp_build and
# id3lib_build).
file(STRINGS "${RESIZABLELIB_SRC_DIR}/ResizableLib.vcxproj" _clcompile_lines
     REGEX "<ClCompile Include=\"[^\"]+\\.cpp\"")

set(RESIZABLELIB_SOURCES "")
foreach(_line IN LISTS _clcompile_lines)
    string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" _src "${_line}")
    list(APPEND RESIZABLELIB_SOURCES "${RESIZABLELIB_SRC_DIR}/${_src}")
endforeach()

list(LENGTH RESIZABLELIB_SOURCES _n_sources)
message(STATUS "resizablelib: ${_n_sources} source files read from ResizableLib.vcxproj")

# CMAKE_MFC_FLAG (a variable, not a target property - "MFC_FLAG" is not a real
# CMake target property; set_target_properties() silently accepts and ignores
# unknown property names, so it never actually enabled MFC linkage here) must
# be set before add_library(). 1 = static MFC, matching UseOfMfc=Static in
# every "... Static Unicode" configuration of ResizableLib.vcxproj.
set(CMAKE_MFC_FLAG 1)

add_library(resizablelib STATIC ${RESIZABLELIB_SOURCES})

# PUBLIC: consumers (srchybrid) #include "ResizableLib/ResizableDialog.h"
# etc. - the "ResizableLib/" segment is the real directory name inside the
# submodule, so the parent of that directory (not the directory itself) is
# what needs to be on the include path.
target_include_directories(resizablelib PUBLIC "${RESIZABLELIB_SRC_DIR}/..")

target_compile_definitions(resizablelib PRIVATE
    WIN32
    _WINDOWS
    # CharacterSet=Unicode in every "... Static Unicode" configuration of the
    # vcxproj - MSBuild turns that into automatic UNICODE/_UNICODE defines.
    # Without these, LPCTSTR/TCHAR-using signatures here (e.g.
    # CResizableState::WriteState(LPCTSTR, LPCTSTR)) compile as narrow char,
    # producing different mangled names than what emule.exe (built with
    # UNICODE) expects to link against - confirmed by an actual build attempt
    # (LNK2001 unresolved externals for exactly those signatures).
    UNICODE
    _UNICODE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# WarningLevel=Level4, plus the Debug/Release-specific flags
# (BasicRuntimeChecks/StringPooling/FunctionLevelLinking/InlineFunctionExpansion)
# from the "... Static Unicode" configurations of the vcxproj.
target_compile_options(resizablelib PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/RTC1>          # BasicRuntimeChecks=EnableFastChecks
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GF>          # StringPooling
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gy>          # FunctionLevelLinking
    # InlineFunctionExpansion=OnlyExplicitInline (Release) - overrides CMake's
    # own default of /Ob2 (AnySuitable, from CMAKE_CXX_FLAGS_RELEASE); the
    # last /Ob switch on the command line wins, and target_compile_options are
    # appended after the default flags, so this takes effect.
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Ob1>
)

set_target_properties(resizablelib PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# StdAfx.cpp creates the PCH in the vcxproj (PrecompiledHeader=Create); all
# other files use it. CMake generates its own hidden PCH translation unit
# instead, so StdAfx.cpp here just compiles as an ordinary (trivial) source
# that also happens to use the PCH - same as srchybrid/CMakeLists.txt does
# for emule's own Stdafx.cpp.
target_precompile_headers(resizablelib PRIVATE "${RESIZABLELIB_SRC_DIR}/StdAfx.h")
