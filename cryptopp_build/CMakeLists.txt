cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Crypto++ (weidai11/cryptopp @ CRYPTOPP_8_9_0, vanilla submodule at
# eMule/cryptopp) has no CMakeLists.txt of its own - only cryptlib.vcxproj
# (MSVC) and GNUmakefile. Rather than pull in a third-party CMake wrapper,
# this file is our own, transcribed directly from cryptlib.vcxproj's
# Debug/Release (static lib) configuration - the DLL-Import configs and
# cryptest/cryptdll projects are not needed here and were skipped. As with
# mbedtls, the submodule itself is never edited.
# ---------------------------------------------------------------------------
set(CRYPTOPP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cryptopp")

# integer.cpp/zdeflate.cpp call stdext::make_checked_array_iterator()/
# make_unchecked_array_iterator() unconditionally on any CRYPTOPP_MSC_VERSION
# (== _MSC_VER) >= 1500/1600 - non-standard MSVC STL (Dinkumware) extensions
# cryptopp assumes every such MSVC ships. Newer MSVC STL releases have removed
# them entirely (confirmed by an actual CI build attempt: C2653/C3861, "not a
# class or namespace name" / "identifier not found", on a toolset newer than
# the one this project is otherwise developed against, which still has them).
# Redefining CRYPTOPP_MSC_VERSION itself to dodge this would be wrong - it
# gates plenty of other, legitimate feature detection that should still see
# the real (and genuinely more capable) compiler version. Instead, probe
# directly for just these two functions at configure time, and only force-
# include our own compatibility shim (msvc_stdext_compat.h - a raw-pointer
# passthrough; both functions only ever existed to silence the old "Checked
# Iterators" MSVC debug feature, not to add real bounds enforcement cryptopp's
# logic depends on) when they're genuinely missing - inert on a toolset (like
# this project's own dev machine, as of this writing) where they still exist.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
    #include <iterator>
    int main() {
        int a[1] = {0};
        (void)stdext::make_unchecked_array_iterator(a);
        (void)stdext::make_checked_array_iterator(a, 1u);
        return 0;
    }
" CRYPTOPP_HAS_MSVC_ARRAY_ITERATOR_SHIMS)

if(CMAKE_VS_PLATFORM_NAME)
    set(_cryptopp_platform "${CMAKE_VS_PLATFORM_NAME}")
else()
    set(_cryptopp_platform "x64")
endif()

# Source list - read directly from cryptlib.vcxproj so this can never drift.
# Order matters for the first 3 (cryptlib.cpp, cpu.cpp, integer.cpp - see the
# vcxproj's own "The order of the first three matters" comment), so they're
# read as an ordered slice rather than alphabetically re-sorted.
file(STRINGS "${CRYPTOPP_SRC_DIR}/cryptlib.vcxproj" _clcompile_lines
     REGEX "<ClCompile Include=\"[^\"]+\\.cpp\"")

set(CRYPTOPP_SOURCES "")
foreach(_line IN LISTS _clcompile_lines)
    string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" _src "${_line}")
    list(APPEND CRYPTOPP_SOURCES "${_src}")
endforeach()

# eccrypto.cpp/eprecomp.cpp: <ExcludedFromBuild>true</ExcludedFromBuild>
# unconditionally in the vcxproj (superseded by ecp.cpp/ec2n.cpp - kept out
# of the build there too). pch.cpp: only `#include "pch.h"`, a stub whose
# sole purpose is to be the MSBuild-recognized PCH-creation translation
# unit; target_precompile_headers() below creates CMake's own PCH object
# instead, so this stub is compiled by neither.
list(REMOVE_ITEM CRYPTOPP_SOURCES eccrypto.cpp eprecomp.cpp pch.cpp)

# neon_simd.cpp (ARM64 only): defines CPU_ProbeARMv7()/CPU_ProbeNEON(), called
# unconditionally from cpu.cpp's ARM feature-detection path. cryptlib.vcxproj
# never lists this file at all - confirmed (diffed cryptopp/*.cpp against the
# vcxproj's own ClCompile list) - because upstream never shipped an ARM64/ARM
# MSVC config for it to belong to (same reason the MASM block below skips
# ARM64), so it can't be "read from the vcxproj" the way everything else here
# is; it has to be added by hand for the one platform that actually needs it.
# Without it: LNK2001 unresolved CPU_ProbeARMv7/CPU_ProbeNEON, confirmed by an
# actual build attempt.
if(_cryptopp_platform STREQUAL "ARM64")
    list(APPEND CRYPTOPP_SOURCES neon_simd.cpp)
endif()

list(TRANSFORM CRYPTOPP_SOURCES PREPEND "${CRYPTOPP_SRC_DIR}/")

list(LENGTH CRYPTOPP_SOURCES _n_sources)
message(STATUS "cryptopp: ${_n_sources} source files read from cryptlib.vcxproj")

# ---------------------------------------------------------------------------
# MASM sources (rdrand/rdseed hardware RNG support, low-level x64 helpers).
# cryptlib.vcxproj only defines Win32/x64 configurations - no ARM64 - so
# these are skipped there, consistent with upstream never having shipped an
# ARM64 config for them.
# ---------------------------------------------------------------------------
set(CRYPTOPP_ASM_SOURCES "")

if(_cryptopp_platform STREQUAL "Win32" OR _cryptopp_platform STREQUAL "x64")
    enable_language(ASM_MASM)
    set(CRYPTOPP_ASM_RDSOURCES
        "${CRYPTOPP_SRC_DIR}/rdrand.asm"
        "${CRYPTOPP_SRC_DIR}/rdseed.asm"
    )
    list(APPEND CRYPTOPP_ASM_SOURCES ${CRYPTOPP_ASM_RDSOURCES})

    # cryptlib.vcxproj's custom-build commands for these two files (only) add
    # /Cx (preserve case of user identifiers - matters for the extern "C"
    # symbols the C++ side links against) and, Win32 only, /safeseh (the final
    # emule.exe link also passes /SAFESEH for Win32 - an object not marked
    # safeseh-compatible would make that link fail with LNK2026). Every
    # asm custom-build command also passes /W3 /Zi unconditionally (not
    # config-gated in the vcxproj). _cryptopp_platform is a plain configure-
    # time variable (not a generator expression) so a plain if() is enough.
    if(_cryptopp_platform STREQUAL "Win32")
        set_source_files_properties(${CRYPTOPP_ASM_RDSOURCES} PROPERTIES COMPILE_OPTIONS "/Cx;/Zi;/W3;/safeseh")
    else()
        set_source_files_properties(${CRYPTOPP_ASM_RDSOURCES} PROPERTIES COMPILE_OPTIONS "/Cx;/Zi;/W3")
    endif()

    if(_cryptopp_platform STREQUAL "x64")
        set(CRYPTOPP_ASM_X64SOURCES
            "${CRYPTOPP_SRC_DIR}/x64dll.asm"
            "${CRYPTOPP_SRC_DIR}/x64masm.asm"
        )
        list(APPEND CRYPTOPP_ASM_SOURCES ${CRYPTOPP_ASM_X64SOURCES})
        # x64dll.asm/x64masm.asm's own custom-build commands don't pass /Cx
        # (only rdrand.asm/rdseed.asm's do) - just /W3 /Zi, same as above.
        set_source_files_properties(${CRYPTOPP_ASM_X64SOURCES} PROPERTIES COMPILE_OPTIONS "/Zi;/W3")
    endif()

    # cryptlib.vcxproj passes _M_X86/_M_X64 explicitly on the ml/ml64 command
    # line (its default preprocessor set doesn't include it the way ClCompile's
    # does) - CMake's ASM_MASM flags need the same. APPEND (not overwriting)
    # onto the COMPILE_OPTIONS list already set per-file above.
    if(_cryptopp_platform STREQUAL "Win32")
        set_property(SOURCE ${CRYPTOPP_ASM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/D_M_X86")
    else()
        set_property(SOURCE ${CRYPTOPP_ASM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/D_M_X64")
    endif()
endif()

add_library(cryptopp STATIC ${CRYPTOPP_SOURCES} ${CRYPTOPP_ASM_SOURCES})

# C++23, matching the emule target (emule_build/CMakeLists.txt). Not cosmetic:
# eMule's own translation units include cryptopp headers and parse them at
# C++23, so building the library itself at the MSVC default (C++14) would mean
# every inline function, template and class in those headers has two different
# definitions inside one binary - an ODR violation whose visible failure mode
# is exactly the noexcept-in-the-type-system mismatch that already showed up
# inside eMule (see emule_build/80-noexcept-in-function-type.patch).
#
# Safe for a crypto library specifically because Crypto++'s own feature gates
# key off CRYPTOPP_MSC_VERSION, not the language flag: config_cxx.h guards
# every CRYPTOPP_CXX11/14/17 macro with "(CRYPTOPP_MSC_VERSION >= 1900) ||
# (__cplusplus >= ...)", and no file in the submodule consults _MSVC_LANG at
# all (verified: zero occurrences). Raising the standard therefore flips no
# #if and changes no compiled code path here.
target_compile_features(cryptopp PRIVATE cxx_std_23)

if(CRYPTOPP_ASM_SOURCES)
    # The VS generator's MASM custom-build step (unlike ClCompile) does not
    # create its own output directory: for a source outside this
    # CMakeLists.txt's own tree, the object path resolves to
    # <IntDir>/__/cryptopp/<file>.obj ("__" is CMake's escape for the ".."
    # in the relative path) and ml64.exe fails with "cannot open file"
    # because that directory doesn't exist yet. Confirmed by an actual
    # build attempt; this pre-build step creates it first.
    add_custom_command(TARGET cryptopp PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:cryptopp>/../cryptopp.dir/$<CONFIG>/__/cryptopp"
    )
endif()

set_target_properties(cryptopp PROPERTIES
    OUTPUT_NAME "cryptlib"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    # WholeProgramOptimization=true. The vcxproj sets it under "All
    # Configurations", Debug included; we deliberately depart from that and
    # keep it to Release. In Debug the linker was discarding /INCREMENTAL to
    # honour the /LTCG that cryptlib.lib's IL objects force on the emule.exe
    # link (LNK4075), so Debug paid for whole-program optimization *and* lost
    # incremental linking - the worst of both for a debug build.
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
    # No /SAFESEH here: lib.exe does not accept it and answered with LNK4044
    # ("unrecognized option; ignored") on both Win32 configurations. What the
    # emule.exe link actually needs is that the hand-written asm objects be
    # marked safeseh-compatible, and that comes from /safeseh on the assembler
    # itself (see the custom-build options further up). The exe link keeps its
    # own /SAFESEH in emule_build/CMakeLists.txt, where it is a real option.
)

target_include_directories(cryptopp PUBLIC "${CRYPTOPP_SRC_DIR}")

target_compile_definitions(cryptopp PRIVATE
    USE_PRECOMPILED_HEADERS
    $<$<CONFIG:Release>:NDEBUG>
    # ARM64 only: config_asm.h's CRYPTOPP_ARM_ACLE_HEADER auto-detection
    # (triggered by __ARM_ARCH/__aarch64__/__ARM_ACLE, which this MSVC ARM64
    # toolset defines) assumes any such compiler ships <arm_acle.h> the way
    # GCC/Clang do - confirmed by an actual build attempt (C1083, header not
    # found) that this toolset's include dir has no arm_acle.h at all (only
    # arm_neon.h/arm64_neon.h/arm64intr.h/arm_sve.h). Forcing this to 0 here
    # (before config_asm.h's own "#if !defined(...)" guard runs) skips that
    # include; cryptopp's arm_simd.h/adv_simd.h etc. all have a parallel
    # CRYPTOPP_MSC_VERSION-guarded intrinsics path that's used instead - no
    # loss of ARM64 crypto acceleration.
    $<$<STREQUAL:${_cryptopp_platform},ARM64>:CRYPTOPP_ARM_ACLE_HEADER=0>
)

# COMPILE_LANGUAGE:CXX guards every flag here - without it, these (pure
# C++-compiler) options also get handed to ml64.exe/ml.exe for the .asm
# sources above, which rejects all of them (A4018 "invalid command-line
# option"). Confirmed by an actual build attempt.
target_compile_options(cryptopp PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/W4>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/wd4231>>   # DisableSpecificWarnings in the vcxproj
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/wd4251>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/wd4275>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/wd4355>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/wd4505>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/RTC1>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/Oi>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Oy>>              # OmitFramePointers
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Oi>>
    # Optimization=MaxSpeed in the vcxproj (cryptlib.vcxproj's Release
    # <ClCompile> block sets this explicitly, unlike e.g. emule.vcxproj's
    # Release block, which leaves Optimization unset and relies on MSBuild's
    # own default). CMake's CMAKE_CXX_FLAGS_RELEASE already includes /O2,
    # so this is a no-op in practice - spelled out here instead of left
    # implicit, since the vcxproj itself is explicit about it.
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Ob2>>             # InlineFunctionExpansion=AnySuitable
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gy>>              # FunctionLevelLinking
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GF>>              # StringPooling
    # X86 Configuration in the vcxproj: EnableEnhancedInstructionSet=SSE2,
    # for the whole Win32 platform (x64 has no such override - SSE2 is the
    # x64 baseline already).
    $<$<COMPILE_LANGUAGE:CXX>:$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<STREQUAL:${_cryptopp_platform},Win32>>:/arch:SSE2>>
)

if(NOT CRYPTOPP_HAS_MSVC_ARRAY_ITERATOR_SHIMS)
    target_compile_options(cryptopp PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/FI${CMAKE_CURRENT_SOURCE_DIR}/msvc_stdext_compat.h>>
    )
endif()

# chacha_avx.cpp: EnableEnhancedInstructionSet=AdvancedVectorExtensions2 for
# this one file only (excluded on the pre-2013 toolsets the vcxproj still
# guards for; not applicable to any toolset we target).
# /arch: is an x86 switch; on ARM64 cl answered D9002 ("ignoring unknown
# option") once per configuration. The file still compiles there, just
# through its generic path.
if(_cryptopp_platform STREQUAL "Win32" OR _cryptopp_platform STREQUAL "x64")
    set_source_files_properties("${CRYPTOPP_SRC_DIR}/chacha_avx.cpp" PROPERTIES
        COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2>"
    )
endif()

target_precompile_headers(cryptopp PRIVATE "${CRYPTOPP_SRC_DIR}/pch.h")
# dll.cpp/iterhash.cpp: <PrecompiledHeader /> (NotUsing) in the vcxproj.
set_source_files_properties(
    "${CRYPTOPP_SRC_DIR}/dll.cpp"
    "${CRYPTOPP_SRC_DIR}/iterhash.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)
