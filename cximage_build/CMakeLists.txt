cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# CxImage (avwarez/CxImage @ branch 7.02-lite-refresh-2026, vanilla submodule
# at cximage/) has no CMakeLists.txt of its own - only
# CxImage/cximage.vcxproj (MSVC). This file is our own, transcribed from that
# vcxproj's Release/Debug configurations, same approach as cryptopp_build,
# id3lib_build and resizablelib_build. The submodule itself is never edited.
#
# eMule needs this library from 0.60a to 0.60d and no longer from 0.72a - the
# release note on the 0.72a commit is explicit about it ("removed CxImage and
# libpng"), which is why this directory exists on this branch and on no other.
# Five files use it: CaptchaGenerator.cpp builds the captcha bitmap,
# FrameGrabThread.cpp turns grabbed video frames into images, BaseClient.cpp
# encodes and decodes the preview images two clients exchange, and
# KnownFile.cpp/SearchList.cpp/PreviewDlg.cpp carry the types those two pass
# around. Only two formats are actually named in that code: BMP, and PNG for
# everything that crosses the wire.
# ---------------------------------------------------------------------------
set(CXIMAGE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cximage/CxImage")

# Source list - read directly from cximage.vcxproj so this can never drift
# (same technique as emule_build/CMakeLists.txt and the other *_build files).
# The list is all 32 sources, including the ones for formats this build turns
# off: each of those is a single #if CXIMAGE_SUPPORT_<FORMAT> around the whole
# file, so they compile to empty translation units rather than dragging in a
# decoder we have no library for. That is how the upstream project builds its
# own "lite" configuration too.
file(STRINGS "${CXIMAGE_SRC_DIR}/cximage.vcxproj" _clcompile_lines
     REGEX "<ClCompile Include=\"[^\"]+\\.cpp\"")

set(CXIMAGE_SOURCES "")
foreach(_line IN LISTS _clcompile_lines)
    string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" _src "${_line}")
    string(REPLACE "\\" "/" _src "${_src}")
    list(APPEND CXIMAGE_SOURCES "${CXIMAGE_SRC_DIR}/${_src}")
endforeach()

list(LENGTH CXIMAGE_SOURCES _n_sources)
message(STATUS "cximage: ${_n_sources} source files read from cximage.vcxproj")

add_library(cximage STATIC ${CXIMAGE_SOURCES})

# PUBLIC: eMule includes it as "CxImage/xImage.h" - the "CxImage/" segment is
# the real directory name inside the submodule, so the parent of that
# directory (the submodule root) is what goes on the include path. The
# spelling in eMule's own sources differs from the file on disk in one letter
# (xImage.h vs ximage.h); that resolves on Windows, which is the only target
# this project builds for, and is the eMule sources' spelling, not ours.
target_include_directories(cximage PUBLIC "${CXIMAGE_SRC_DIR}/..")

target_compile_definitions(cximage PUBLIC
    # ximacfg.h in this branch guards the PNG switch with #ifndef, precisely so
    # the build system can turn it on without editing the submodule. eMule
    # needs it: BaseClient.cpp both encodes and decodes preview images as PNG.
    # PUBLIC, not PRIVATE - ximapng.h is reachable from ximage.h, and a
    # consumer that parsed it with the switch off would see a different class
    # layout than the one in this library.
    CXIMAGE_SUPPORT_PNG=1
)

target_compile_definitions(cximage PRIVATE
    # WIN32 is not something the compiler defines on its own (that is _WIN32);
    # ximadef.h tests for it to decide whether it is on Windows at all, and
    # without it the library compiles its POSIX fallback typedefs instead.
    # MSVC project files define it on x64 as well, which is why there is no
    # platform condition here.
    WIN32
    _LIB
    _CRT_SECURE_NO_DEPRECATE
    # CharacterSet=Unicode in every configuration of the vcxproj. It has to
    # match emule.exe: CxImage::Save/Load take const TCHAR*, so a narrow build
    # of this library would export names emule.exe cannot resolve - the same
    # failure mode already documented in resizablelib_build/CMakeLists.txt.
    UNICODE
    _UNICODE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# WarningLevel=Level3 in the vcxproj. This is a 2011 codebase we do not own
# and do not patch; /Wall here would report thousands of lines that nobody is
# going to act on, and would bury the ones from eMule itself.
target_compile_options(cximage PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/RTC1>
)

set_target_properties(cximage PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# libpng, for the one format above that needs a codec. ximapng.h reaches into
# libpng's private headers by relative path ("../../libpng/pngpriv.h"), so the
# submodule has to sit at the top of this repo under exactly that name - it
# does - but pnglibconf.h is generated into the build tree, and that directory
# arrives here only through png_static's own interface.
target_link_libraries(cximage PRIVATE png_static)
