# Standalone build for the overlapped-send specimen.
#
# Deliberately NOT wired into the top-level CMakeLists.txt, for the same reason
# wsrepro/ is not: this is a test case, not part of eMule, and it must stay
# buildable (and breakable) without touching the application build.
#
#   cmake -S ovrepro -B build-ovrepro -A x64
#   cmake --build build-ovrepro --config Release
#
# MSVC only: the point of the exercise is to run eMule's exact constructs,
# which means MFC.
cmake_minimum_required(VERSION 3.20)

project(ovrepro CXX)

if(NOT MSVC)
    message(FATAL_ERROR "ovrepro reproduces MFC/Winsock constructs and can only be built on Windows with MSVC.")
endif()

# Static MFC and the static runtime, matching emule_build/CMakeLists.txt, so
# the produced exe is a single file that can be dropped anywhere - including
# straight into a Wine prefix - with no redistributable to install first.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MFC_FLAG 1)

add_executable(ovrepro ovrepro.cpp)

target_compile_definitions(ovrepro PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
)

target_link_libraries(ovrepro PRIVATE ws2_32)

# Console subsystem: the harness reports on stdout and is meant to be run from
# a shell and piped into a file.
set_target_properties(ovrepro PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)
