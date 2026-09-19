cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# eMule sources live in the "emule" submodule (avwarez/eMule @ v0.60d, never
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
# actually builds. 246 files on v0.60d.
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
# Platform - still needed below for the SAFESEH and per-file options, and for
# naming the manifest. 0.60d's project file declares Win32 and x64 and nothing
# else, and ships a manifest for each of those two and no other; ARM64 arrived
# with 0.72a. A configure for a platform this branch has no manifest for stops
# here rather than failing later at the link step with a missing file.
# ---------------------------------------------------------------------------
if(CMAKE_VS_PLATFORM_NAME)
    set(EMULE_PLATFORM_DIR "${CMAKE_VS_PLATFORM_NAME}")
elseif(CMAKE_GENERATOR_PLATFORM)
    set(EMULE_PLATFORM_DIR "${CMAKE_GENERATOR_PLATFORM}")
else()
    set(EMULE_PLATFORM_DIR "x64")
    message(WARNING "eMule: could not determine target platform (Win32/x64) from the generator; "
                     "defaulting to x64. Pass -A <platform> (Visual Studio generator) to be explicit.")
endif()

if(NOT EMULE_PLATFORM_DIR STREQUAL "Win32" AND NOT EMULE_PLATFORM_DIR STREQUAL "x64")
    message(FATAL_ERROR
        "eMule 0.60d builds for Win32 and x64 only - emule.vcxproj declares no other platform, "
        "and res/ holds no emule${EMULE_PLATFORM_DIR}.manifest. ARM64 exists from 0.72a onwards.")
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
# C++14, which is MSVC's own default and therefore what emule.vcxproj - which
# sets no <LanguageStandard> at all - has always been compiled at. Named here
# rather than left implicit so that a future toolset moving its default does
# not silently change what this branch builds.
#
# This is the one place where this branch deliberately does not follow v0.72a.
# There, raising the standard to C++23 is the point of the exercise, and the
# 00-89 band of emule_build/ exists to carry the source changes three rounds
# of standard-library removals require. Those patches are written against
# 0.72a and say nothing about this tree. Building 0.60d at its own standard
# asks one question at a time: whether this source still compiles with a
# current toolset and current dependencies. Raising it afterwards is a second
# question, and it will need its own patches, measured here.
# ---------------------------------------------------------------------------
target_compile_features(emule PRIVATE cxx_std_14)

# KNOWN LIMITATION: emule.vcxproj targets PlatformToolset v141_xp (a
# Windows-XP-compatible VS2017 toolset) for both of its platforms;
# id3lib.vcxproj wants v141, ResizableLib.vcxproj wants v140_xp. None of those toolsets are
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

# mbedtls's include dirs and the MBEDTLS_USER_CONFIG_FILE define
# (which is what makes MBEDTLS_THREADING_ALT visible here too, not just when
# the mbedtls library itself is built) both come in transitively via linking
# against the mbedtls target below - it declares them PUBLIC. Only the
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
# CxImage is reached the same way but needs no entry here: five source files
# include it as "CxImage/xImage.h", and the cximage target published the
# submodule root for exactly that - see cximage_build/CMakeLists.txt.
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
    # Unconditional on this branch: both of 0.60d's platforms set it, and it
    # is the ARM64 configuration added in 0.72a that leaves it out.
    XP_BUILD
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
    mbedtls
    cryptopp
    cximage
    id3lib
    zlibstatic
    miniupnpc::miniupnpc
    resizablelib
)

target_link_options(emule PRIVATE
    /DELAYLOAD:gdiplus.dll
    # 0.60d delay-loads three DLLs; msimg32.dll joins the list in 0.72a.
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

# ---------------------------------------------------------------------------
# Language DLLs
#
# eMule ships its UI translations as resource-only satellite DLLs, one per
# locale, loaded at runtime by I18n.cpp (::LoadLibrary on "<lang>\<locale>.dll"
# next to emule.exe). Each lives in emule/srchybrid/lang/ as its own tiny
# Visual Studio project (<locale>.vcxproj) whose entire content is a single
# ResourceCompile of <locale>.rc - no C++ at all: ConfigurationType is
# DynamicLibrary with NoEntryPoint=true (i.e. /NOENTRY), so the resulting DLL
# has only a resource section and no entry point/CRT.
#
# Rather than track the 43 vcxproj files (they differ only by locale name and
# GUID), every lang/*.rc is turned into one such DLL here by globbing the
# directory - a locale added upstream is picked up automatically. The DLL is
# named exactly <locale>.dll (TargetName=$(MSBuildProjectName) in the original
# projects, which is the locale) so the I18n.cpp loader finds it, and is placed
# in a lang/ subdirectory next to emule.exe to match that loader's layout.
#
# These are built by the dedicated `languages` aggregate target, not by the
# default (all) build: EXCLUDE_FROM_ALL keeps `--target emule` as lean as it
# was, while `cmake --build ... --target languages` produces the DLLs.
#
# Not every locale in lang/ is shipped. eMule resolves a string that is absent
# (or empty) in the language DLL by falling back to the English resource in
# emule.exe - GetResString(), I18n.cpp - so an out-of-date translation degrades
# into a half-English UI rather than into an error. EMULE_LANGUAGES therefore
# lists the locales whose .rc was audited against emule.rc and found aligned
# with the current string table; the rest are still globbed and reported, but
# no DLL is produced for them, so eMule shows those users a fully English UI
# instead of a partly stale one. The excluded set is ba_BA, es_AS, fr_BR,
# lv_LV, mt_MT, ug_CN, va_ES and va_ES_RACV: each carries the strings added
# since its last translation pass as commented-out English placeholders, and
# lv_LV is only ~39% translated. Setting -DEMULE_LANGUAGES=ALL restores the
# old behaviour and builds every lang/*.rc found.
#
# afxres.h (the only include not reachable relative to the .rc itself -
# ..\Resource.h, ..\Version.h and lang.rc2 all resolve against the rc file's
# own lang/ directory) lives in the toolset's ATLMFC\include, reconstructed
# from the compiler path exactly like the afximpl.h case for emule above. The
# original vcxproj files carry no UseOfMfc, so the DLLs never link MFC - only
# the RC preprocessor needs that header, for its IDS_/ID_ resource constants.
# ---------------------------------------------------------------------------
set(EMULE_LANG_DIR "${EMULE_SRC_DIR}/lang")

set(EMULE_LANGUAGES
    ar_AE bg_BG ca_ES cz_CZ da_DK de_DE el_GR es_ES_T et_EE fa_IR fi_FI fr_FR
    gl_ES he_IL hu_HU it_IT jp_JP ko_KR lt_LT nb_NO nl_NL nn_NO pl_PL pt_BR
    pt_PT ro_RO ru_RU sl_SI sq_AL sv_SE tr_TR ua_UA vi_VN zh_CN zh_TW
    CACHE STRING "Locales to build language DLLs for, or ALL for every lang/*.rc")

file(GLOB _lang_rcs "${EMULE_LANG_DIR}/*.rc")

set(_lang_available "")
foreach(_lang_rc IN LISTS _lang_rcs)
    get_filename_component(_locale "${_lang_rc}" NAME_WE)
    list(APPEND _lang_available "${_locale}")
endforeach()

# A locale named here but missing from the submodule means the list has drifted
# from lang/ (renamed or dropped upstream) - fail loudly rather than silently
# shipping one DLL fewer than intended.
if(NOT EMULE_LANGUAGES STREQUAL "ALL")
    foreach(_locale IN LISTS EMULE_LANGUAGES)
        if(NOT _locale IN_LIST _lang_available)
            message(FATAL_ERROR
                "eMule: EMULE_LANGUAGES lists '${_locale}' but ${EMULE_LANG_DIR}/${_locale}.rc does not exist")
        endif()
    endforeach()
endif()

set(_lang_selected "")
set(_lang_skipped "")
foreach(_lang_rc IN LISTS _lang_rcs)
    get_filename_component(_locale "${_lang_rc}" NAME_WE)
    if(EMULE_LANGUAGES STREQUAL "ALL" OR _locale IN_LIST EMULE_LANGUAGES)
        list(APPEND _lang_selected "${_lang_rc}")
    else()
        list(APPEND _lang_skipped "${_locale}")
    endif()
endforeach()

add_custom_target(languages COMMENT "Building the selected eMule language resource DLLs")

foreach(_lang_rc IN LISTS _lang_selected)
    get_filename_component(_locale "${_lang_rc}" NAME_WE)   # e.g. it_IT
    set(_lang_target "lang_${_locale}")

    add_library(${_lang_target} SHARED EXCLUDE_FROM_ALL "${_lang_rc}")
    set_target_properties(${_lang_target} PROPERTIES
        LINKER_LANGUAGE CXX               # no C++ sources - CXX only picks the linker driver
        OUTPUT_NAME "${_locale}"          # <locale>.dll, as the I18n.cpp loader expects
        PREFIX ""                         # never a "lib" prefix
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:emule>/lang"
    )
    # /NOENTRY = NoEntryPoint=true in the vcxproj: a resource-only DLL with no
    # startup code, so no CRT/entry point is required or referenced.
    # /MANIFEST:NO: these DLLs carry no manifest (the original projects embed
    # none), and CMake would otherwise inject its default one.
    target_link_options(${_lang_target} PRIVATE /NOENTRY /MANIFEST:NO)

    if(MSVC AND MSVC_TOOLSET_ROOT)
        target_include_directories(${_lang_target} PRIVATE "${MSVC_TOOLSET_ROOT}/ATLMFC/include")
    endif()

    add_dependencies(languages ${_lang_target})
endforeach()

list(LENGTH _lang_selected _n_langs)
list(LENGTH _lang_skipped _n_skipped)
message(STATUS "eMule: ${_n_langs} language DLLs wired into the 'languages' target")
if(_n_skipped GREATER 0)
    string(REPLACE ";" " " _lang_skipped_str "${_lang_skipped}")
    message(STATUS "eMule: ${_n_skipped} locales not shipped (English fallback): ${_lang_skipped_str}")
endif()
