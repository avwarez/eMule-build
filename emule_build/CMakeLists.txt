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

# ---------------------------------------------------------------------------
# C++23 - the point of this branch. emule.vcxproj sets no <LanguageStandard>
# at all, so the baseline being left behind here is MSVC's own default,
# C++14: this single line jumps three rounds of standard-library removals
# (C++17, C++20, C++23) at once.
#
# Deliberately a target_compile_features() on emule and NOT a top-level
# CMAKE_CXX_STANDARD: the latter would drag cryptopp, id3lib and
# resizablelib along too (C++98-era codebases with no reason to move), and
# would fight mbedtls/tf-psa-crypto, which pin their own CMAKE_CXX_STANDARD
# 11 for their C++ bits. Scoped like this, every dependency keeps building
# at the MSVC default and only eMule's own 244 translation units change
# standard - which also keeps any build break attributable to one target.
#
# CMake maps cxx_std_23 to /std:c++23preview on toolsets that have it and to
# /std:c++latest otherwise; both are fine here, since nothing in this build
# depends on the standard being final rather than preview.
# ---------------------------------------------------------------------------
target_compile_features(emule PRIVATE cxx_std_23)

# KNOWN LIMITATION: emule.vcxproj targets PlatformToolset v141_xp for Win32/x64
# (a Windows-XP-compatible VS2017 toolset) and v145 for ARM64; id3lib.vcxproj
# wants v141, ResizableLib.vcxproj wants v140_xp. None of those toolsets are
# installed here - every target in this build instead uses whatever toolset
# ships with the installed VS generator (confirmed: only MSVC 14.50/14.51 are
# present, no v14x_xp/v141/v140_xp component). Accepted as-is for now; install
# the matching "Windows XP support for C++" / VS2017/VS2015 toolset components
# via the Visual Studio Installer and set PlatformToolset (top-level
# CMakeLists.txt, before add_subdirectory(emule_build)/(id3lib_build)/
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
# Several source files include their dependency by submodule-relative path -
# e.g. EncryptedStreamSocket.h does #include "cryptopp/integer.h", WebServer.h
# does #include "zlib/zlib.h" - so CMAKE_SOURCE_DIR (this repo's root, where
# cryptopp/, zlib/, id3lib/, mbedtls/, miniupnp/, resizablelib/ all live as
# sibling submodules) has to be on the include path, not emule/ (the eMule
# submodule's own root, which holds none of them).
# UPnPImplMiniLib.cpp does #include "miniupnpc\include\miniupnpc.h" - the
# miniupnpc submodule lives at eMule-build/miniupnp/miniupnpc (miniupnp/miniupnp
# is the upstream monorepo; "miniupnpc" is one of its subdirectories, not the
# submodule's own directory name), so eMule-build/miniupnp is on the include
# path here too, to make that one "miniupnpc/..." segment in the #include resolve.
target_include_directories(emule PRIVATE
    ${EMULE_SRC_DIR}
    ${CMAKE_SOURCE_DIR}
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
#
# /Wall applies to headers pulled in from third parties too, and that is where
# almost all of the noise comes from. Measured on run 32569043402 (6 configs):
# 14826 warning lines, of which 6524 from Crypto++ headers, 5732 from MFC/ATL/
# CRT headers and ~1900 from the Windows SDK - 96% of the total, none of it
# actionable here. Only 605 lines (146 unique warnings) came from srchybrid.
# /external:W0 silences the external ones while /Wall stays fully in force on
# our own translation units. Crypto++ is reached through quoted includes
# (#include "cryptopp/rsa.h"), so anglebrackets alone does not cover it and the
# directory has to be named explicitly.
target_compile_options(emule PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Wall>
    $<$<CXX_COMPILER_ID:MSVC>:/external:anglebrackets>
    $<$<CXX_COMPILER_ID:MSVC>:/external:W0>
    $<$<CXX_COMPILER_ID:MSVC>:/external:I${CMAKE_SOURCE_DIR}/cryptopp>
    # C4755 comes from atlmfc/atlalloc.h but is emitted by the optimizer during
    # code generation, where the warning is no longer attributed to the include
    # that produced it - so /external:W0 above does not reach it (confirmed by a
    # build). It reports that an inlined comparison in CStringT cannot take one
    # of its branches, which is not actionable in a header we do not own.
    $<$<CXX_COMPILER_ID:MSVC>:/wd4755>
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

# Scanner.cpp is generated by flex (see the regeneration target near the end of
# this file), so its warnings must not be fixed by editing it: C5033 ('register'
# is no longer a supported storage class, 17 hits) and the C5262 fall-through at
# line 1966 are both flex's own output and would come back the next time the
# lexer is regenerated. Suppressed per file instead, leaving the rest of the
# codebase under the full /Wall set.
set_source_files_properties("${EMULE_SRC_DIR}/Scanner.cpp" PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd5033;/wd5262>"
)

# ESPERIMENTO C4191 (braccio C): /wd4191 per singolo sorgente. Le message map
# MFC producono una C4191 per mappa (22 in tutto) e finora nessuna soppressione
# le ha chiuse: ne' /wd4191 sulla riga di comando del progetto, ne' la
# #pragma warning(disable:4191) di Stdafx.h:93, ne' la stessa pragma ripetuta
# dopo gli include MFC. Questa run confronta tre meccanismi su tre file:
#   A ChatSelector.cpp        push/disable/pop attorno alla message map (patch 94)
#   B ClientDetailDialog.cpp  warning(suppress:4191) sulla riga (patch 94)
#   C ChatWnd.cpp             /wd4191 per sorgente (qui)
# Gli altri 19 file restano di controllo. Da rimuovere una volta noto l'esito.
set_source_files_properties("${EMULE_SRC_DIR}/ChatWnd.cpp" PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4191>"
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
