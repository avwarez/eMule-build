# ---------------------------------------------------------------------------
# FindZLIB shim for the vendored libpng.
#
# libpng's own CMakeLists.txt is used as-is (the submodule is never edited),
# and it opens with find_package(ZLIB REQUIRED). Left to itself that looks for
# an installed zlib on the machine, which is not what this build wants and is
# not present on the CI runner either: zlib is a submodule of this repo,
# already configured and built a few lines earlier in the top-level
# CMakeLists.txt, and that is the copy libpng and everything else here has to
# link against - one zlib in the binary, built with the same static CRT as the
# rest.
#
# So this directory goes on CMAKE_MODULE_PATH ahead of CMake's own modules and
# this file answers the question instead of searching for an answer. It is a
# shim, not a finder: it does not look for anything, it reports the target the
# top-level file has already created.
#
# Kept here rather than in the top-level file because find_package() only ever
# looks for a file with this exact name on the module path; there is no way to
# express it inline.
# ---------------------------------------------------------------------------
if(NOT TARGET ZLIB::ZLIB)
    message(FATAL_ERROR
        "libpng_build/FindZLIB.cmake: ZLIB::ZLIB does not exist yet. "
        "The zlib submodule must be add_subdirectory()'d, and the alias created, "
        "before anything that asks for zlib - see the top-level CMakeLists.txt.")
endif()

set(ZLIB_FOUND TRUE)
set(ZLIB_LIBRARY ZLIB::ZLIB)
set(ZLIB_LIBRARIES ZLIB::ZLIB)

# The generated zconf.h lands in the build tree, the rest of the headers stay
# in the source tree, so both directories are needed. These are the same two
# that zlib's own zlibstatic target publishes, spelled out here because its
# INTERFACE_INCLUDE_DIRECTORIES carries build/install generator expressions
# that a plain variable cannot hold.
set(ZLIB_INCLUDE_DIR "${CMAKE_BINARY_DIR}/zlib" "${CMAKE_SOURCE_DIR}/zlib")
set(ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR})
