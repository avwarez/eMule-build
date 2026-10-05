# Standalone build for the per-symbol behaviour probe.
#
# Not wired into the top-level CMakeLists.txt, like the other reproducers:
#
#   cmake -S symrepro -B build-symrepro -A x64
#   cmake --build build-symrepro --config Release
#
# The exe calls every Win32 symbol eMule imports (outside the GUI DLLs) the
# way eMule calls it, and prints canonical text; the same exe is run on
# Windows and under Wine and the outputs are diffed by compare.py.
cmake_minimum_required(VERSION 3.20)

project(symrepro CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "symrepro must be built with MSVC, the compiler eMule is built with.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(symrepro
    harness.cpp
    t_string.cpp
    t_path.cpp
    t_file.cpp
    t_sys.cpp
    t_net.cpp
    t_ole.cpp
    t_crypt.cpp
    t_inet.cpp
    symrepro.rc
)

target_compile_definitions(symrepro PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

# /EHa: the harness catches SEH exceptions around every test, and some tests
# raise them on purpose.
target_compile_options(symrepro PRIVATE /W3 /EHa /utf-8 /permissive- /std:c++17)

target_link_libraries(symrepro PRIVATE
    ws2_32 wsock32 iphlpapi kernel32 user32 gdi32 advapi32 shell32 shlwapi
    ole32 oleaut32 uuid urlmon wininet crypt32 version winmm
)

set_target_properties(symrepro PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

# A resource-only DLL, the shape of eMule's lang\*.dll.
add_library(srlang SHARED lang.rc)
set_target_properties(srlang PROPERTIES LINKER_LANGUAGE CXX LINK_FLAGS "/NOENTRY")
add_dependencies(symrepro srlang)
