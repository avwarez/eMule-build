cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# eMule sources live in the "emule" submodule (avwarez/eMule @ v0.72a, never
# edited in place) at emule/srchybrid, not in this directory - this file
# exists only so the srchybrid/CMakeLists.txt logic from the original
# (unsplit) eMule-build layout has somewhere to live outside the submodule.
# Every path below is therefore anchored on EMULE_SRC_DIR instead of
# CMAKE_CURRENT_SOURCE_DIR.
# ---------------------------------------------------------------------------
set(EMULE_SRC_DIR "${CMAKE_SOURCE_DIR}/emule/srchybrid")

# ---------------------------------------------------------------------------
# Source list - read directly from emule.vcxproj (<ClCompile Include="...">)
# so this file can never drift from what the original Visual Studio project
# actually builds. 244 files as of the v0.72a restructure.
# ---------------------------------------------------------------------------
file(STRINGS "${EMULE_SRC_DIR}/emule.vcxproj" _clcompile_lines
     REGEX "<ClCompile Include=\"[^\"]+\"")

set(EMULE_SOURCES "")
foreach(_line IN LISTS _clcompile_lines)
    string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" _src "${_line}")
    string(REPLACE "\\" "/" _src "${_src}")
    list(APPEND EMULE_SOURCES "${EMULE_SRC_DIR}/${_src}")
endforeach()

list(LENGTH EMULE_SOURCES _n_sources)
message(STATUS "eMule: ${_n_sources} source files read from emule.vcxproj")

# ---------------------------------------------------------------------------
# Platform (Win32/x64/ARM64) - still needed below for the XP_BUILD/SAFESEH/
# arch-specific compile and link options.
# ---------------------------------------------------------------------------
if(CMAKE_VS_PLATFORM_NAME)
    set(EMULE_PLATFORM_DIR "${CMAKE_VS_PLATFORM_NAME}")
elseif(CMAKE_GENERATOR_PLATFORM)
    set(EMULE_PLATFORM_DIR "${CMAKE_GENERATOR_PLATFORM}")
else()
    set(EMULE_PLATFORM_DIR "x64")
    message(WARNING "eMule: could not determine target platform (Win32/x64/ARM64) from the generator; "
                     "defaulting to x64. Pass -A <platform> (Visual Studio generator) to be explicit.")
endif()

# $<COMPILE_LANGUAGE:RC> in target_compile_definitions() is silently ignored by
# CMake's Visual Studio generator (confirmed: a decoy define through that
# mechanism never made it into the generated .vcxproj, in any config, with or
# without combining it via AND with a $<CONFIG:...>) - CMAKE_RC_FLAGS is used
# instead, which does work (it's how CMake's own default "-DWIN32" ends up in
# every ResourceCompile item, and "-D_DEBUG" via CMAKE_RC_FLAGS_DEBUG for Debug).
#
# _VC80_UPGRADE=0x0710: MSBuild's own rc.exe invocation for emule.vcxproj always
# passes this (confirmed with a direct MSBuild build of the vcxproj - "/D
# _VC80_UPGRADE=0x0710" is present verbatim in both its Debug and Release
# ResourceCompile command lines), injected by the Microsoft.CPP.UpgradeFromVC71.props
# property sheet imported in the vcxproj; CMake has no equivalent import, so this
# replicates its effect directly. Without it, emule.rc's
# "#if !(defined(_VC80_UPGRADE) && (_VC80_UPGRADE >= 0x0700) && (_VC80_UPGRADE < 0x0800))"
# guard evaluates true and tries to embed a legacy CREATEPROCESS_MANIFEST_RESOURCE_ID
# pointing at a "res\emule.manifest" that has never existed in this repo (RC2135) -
# dead code intentionally disabled in the real project, since the modern
# per-platform manifest is instead merged in at link time (see target_link_options'
# /MANIFESTINPUT below).
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -D_VC80_UPGRADE=0x0710")

# CMAKE_MFC_FLAG (a variable, not a target property - "MFC_FLAG" is not a real
# CMake target property at all; set_target_properties() silently accepts and
# ignores unknown property names, so that never actually enabled MFC linkage,
# confirmed by an actual build attempt failing with "unresolved external
# symbol WinMain": libcmt.lib's own CRT startup expects the user to supply
# WinMain, which is exactly what MFC's static library would otherwise provide,
# via AfxWinMain, if UseOfMfc were actually set). 1 = static MFC (matches
# UseOfMfc=Static in the vcxproj); must be set before add_executable().
set(CMAKE_MFC_FLAG 1)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_executable(emule WIN32 ${EMULE_SOURCES} "${EMULE_SRC_DIR}/emule.rc")

set_target_properties(emule PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    OUTPUT_NAME "emule"
    VS_DPI_AWARE "OFF"                          # EnableDpiAwareness=false in the vcxproj (both configs)
)

# KNOWN LIMITATION: emule.vcxproj targets PlatformToolset v141_xp for Win32/x64
# (a Windows-XP-compatible VS2017 toolset) and v145 for ARM64; id3lib.vcxproj
# wants v141, ResizableLib.vcxproj wants v140_xp. None of those toolsets are
# installed here - every target in this build instead uses whatever toolset
# ships with the installed VS generator (confirmed: only MSVC 14.50/14.51 are
# present, no v14x_xp/v141/v140_xp component). Accepted as-is for now; install
# the matching "Windows XP support for C++" / VS2017/VS2015 toolset components
# via the Visual Studio Installer and set PlatformToolset (top-level
# CMakeLists.txt, before add_subdirectory(srchybrid_build)/(id3lib_build)/
# (resizablelib_build)) if XP-compatible code generation is ever required.

# Stdafx.h directly #includes <afximpl.h>, an MFC *implementation* header
# under ATLMFC\src\mfc - not part of the public ATLMFC\include tree that
# MFC_FLAG wires in automatically. The vcxproj covers this via the
# MSBuild-only $(VC_ATLMFC_SourcePath) property; CMake has no equivalent, so
# it's reconstructed from the compiler path. Confirmed necessary by an actual
# build: without it, PCH generation fails with C1083 on afximpl.h.
if(MSVC)
    get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)   # .../bin/HostX64/x64
    get_filename_component(_cl_dir "${_cl_dir}" DIRECTORY)              # .../bin/HostX64
    get_filename_component(_cl_dir "${_cl_dir}" DIRECTORY)              # .../bin
    get_filename_component(MSVC_TOOLSET_ROOT "${_cl_dir}" DIRECTORY)    # .../<toolset version>
    set(ATLMFC_SRC_MFC "${MSVC_TOOLSET_ROOT}/ATLMFC/src/mfc")
endif()

# mbedtls/tf-psa-crypto include dirs and the MBEDTLS_USER_CONFIG_FILE define
# (which is what makes MBEDTLS_THREADING_ALT visible here too, not just when
# the mbedtls library itself is built) both come in transitively via linking
# against MbedTLS::mbedtls below - it declares them PUBLIC. Only the
# threading_alt.h override directory is added explicitly, via
# include_directories() in the top-level CMakeLists.txt.
# UPnPImplMiniLib.cpp does #include "miniupnpc\include\miniupnpc.h" - the
# miniupnpc submodule lives at eMule-build/miniupnp/miniupnpc (miniupnp/miniupnp
# is the upstream monorepo; "miniupnpc" is one of its subdirectories, not the
# submodule's own directory name), so eMule-build/miniupnp is on the include
# path here to make that one "miniupnpc/..." segment in the #include resolve.
target_include_directories(emule PRIVATE
    ${EMULE_SRC_DIR}
    ${CMAKE_SOURCE_DIR}/emule
    ${CMAKE_SOURCE_DIR}/miniupnp
    ${ATLMFC_SRC_MFC}
)

target_compile_definitions(emule PRIVATE
    SUPPORT_LARGE_FILES
    UNICODE
    _UNICODE
    $<$<NOT:$<STREQUAL:${EMULE_PLATFORM_DIR},ARM64>>:XP_BUILD>
    $<$<CONFIG:Release>:NDEBUG>
)
# _DEBUG is deliberately NOT in the list above for C++ compilation: emule.vcxproj's
# Debug <ClCompile> block does not define it either (confirmed - absent from its
# PreprocessorDefinitions), so MFC's ASSERT/VERIFY/TRACE stay compiled out even in
# a Debug build, matching the original project. (Its Debug <ResourceCompile> block
# DOES set "/D _DEBUG" - see CMAKE_RC_FLAGS above add_executable().)

# WarningLevel=EnableAllWarnings in the vcxproj -> /Wall (very noisy: enables
# every MSVC warning, including ones off by default at /W4). Kept faithful
# to the original project rather than toned down.
target_compile_options(emule PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Wall>
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:throwingNew>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<STREQUAL:${EMULE_PLATFORM_DIR},ARM64>>:/Zc:enumTypes>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/RTC1>
    $<$<CXX_COMPILER_ID:MSVC>:/Oi>                                    # IntrinsicFunctions (both configs)
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gy>           # FunctionLevelLinking
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Oy>           # OmitFramePointers
)
# ASSUMPTION: the vcxproj's Release <ClCompile> block does not set <Optimization>
# explicitly. CMake's default CMAKE_CXX_FLAGS_RELEASE already includes /O2 /DNDEBUG,
# which we take as the intended behavior rather than overriding it.

# Precompiled header: Stdafx.cpp creates it, all files use it except
# Parser.cpp/Scanner.cpp which declare <PrecompiledHeader /> (NotUsing) in
# the vcxproj.
target_precompile_headers(emule PRIVATE "${EMULE_SRC_DIR}/Stdafx.h")
set_source_files_properties("${EMULE_SRC_DIR}/Parser.cpp" "${EMULE_SRC_DIR}/Scanner.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Scanner.cpp per-file PreprocessorDefinitions from the vcxproj: only Win32
# gets _USE_32BIT_TIME_T; both Win32 and x64 get _CRT_SECURE_NO_DEPRECATE.
# ARM64 gets neither - the vcxproj has no ARM64 condition for this file at all
# (never updated when the ARM64 platform was added), replicated as-is.
set_source_files_properties("${EMULE_SRC_DIR}/Scanner.cpp" PROPERTIES
    COMPILE_DEFINITIONS "$<$<STREQUAL:${EMULE_PLATFORM_DIR},Win32>:_USE_32BIT_TIME_T;>$<$<OR:$<STREQUAL:${EMULE_PLATFORM_DIR},Win32>,$<STREQUAL:${EMULE_PLATFORM_DIR},x64>>:_CRT_SECURE_NO_DEPRECATE>"
)

# ---------------------------------------------------------------------------
# Linking
# ---------------------------------------------------------------------------
target_link_libraries(emule PRIVATE
    ADSIId.lib
    crypt32.lib
    delayimp.lib
    iphlpapi.lib
    version.lib
    winmm.lib
    ws2_32.lib
    MbedTLS::mbedtls
    cryptopp
    id3lib
    zlibstatic
    miniupnpc::miniupnpc
    resizablelib
)

target_link_options(emule PRIVATE
    /DELAYLOAD:gdiplus.dll
    /DELAYLOAD:msimg32.dll
    /DELAYLOAD:oleacc.dll
    /DELAYLOAD:ws2_32.dll
    /NODEFAULTLIB:mpr.lib
    /NODEFAULTLIB:rasapi32.lib
    /NODEFAULTLIB:secur32.lib
    /NODEFAULTLIB:sensapi.lib
    /NODEFAULTLIB:vfw32.lib
    $<$<STREQUAL:${EMULE_PLATFORM_DIR},Win32>:/SAFESEH>
    "/MANIFESTINPUT:${EMULE_SRC_DIR}/res/emule${EMULE_PLATFORM_DIR}.manifest"
    $<$<CONFIG:Release>:/OPT:REF>                                     # OptimizeReferences=true (Release only; Debug's explicit "false" is CMake's default anyway)
    $<$<CONFIG:Release>:/OPT:ICF>                                     # EnableCOMDATFolding=true (Release only)
)
# WIN32 on add_executable() already implies /SUBSYSTEM:WINDOWS.

# ---------------------------------------------------------------------------
# Optional: regenerate Parser.cpp/Scanner.cpp from their grammar/lexer
# sources. Not part of the default build - emule.vcxproj also marks this
# CustomBuild step ExcludedFromBuild=true; the generated .cpp files are
# committed and used as-is.
# ---------------------------------------------------------------------------
find_program(BISON_EXECUTABLE bison)
find_program(FLEX_EXECUTABLE flex)

if(BISON_EXECUTABLE)
    add_custom_target(regen_parser
        COMMAND ${BISON_EXECUTABLE} --no-lines --defines --output "${EMULE_SRC_DIR}/Parser.cpp" "${EMULE_SRC_DIR}/Parser.y"
        WORKING_DIRECTORY ${EMULE_SRC_DIR}
        COMMENT "Regenerating Parser.cpp/.hpp from Parser.y (not part of the default build)"
    )
endif()

if(FLEX_EXECUTABLE)
    add_custom_target(regen_scanner
        COMMAND ${FLEX_EXECUTABLE} -L -o"${EMULE_SRC_DIR}/Scanner.cpp" "${EMULE_SRC_DIR}/Scanner.l"
        WORKING_DIRECTORY ${EMULE_SRC_DIR}
        COMMENT "Regenerating Scanner.cpp from Scanner.l (not part of the default build)"
    )
endif()

# ---------------------------------------------------------------------------
# Known scope gap: the vcxproj also defines a third configuration,
# _SpecialBootstrapNodes (Debug settings + _BOOTSTRAPNODESDAT, always linked
# against the *Debug* third-party libs regardless of $(Configuration)). Not
# implemented as a genuine third CMAKE_CONFIGURATION_TYPES entry here - out
# of scope for a Debug/Release CI build of the mainline application. Add it
# back as a real custom configuration type if that special build is needed.
# ---------------------------------------------------------------------------
