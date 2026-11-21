cmake_minimum_required(VERSION 3.20)
project(Kapsel LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure with a Windows x64 toolchain.")
endif()

set(KAPSEL_VERSION "0.0.0-dev" CACHE STRING "Version displayed by the application")
if(KAPSEL_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    set(KAPSEL_FILE_VERSION "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
else()
    set(KAPSEL_FILE_VERSION "0,0,0,0")
endif()
configure_file(packaging/windows/version.h.in generated/version.h @ONLY)
configure_file(packaging/windows/kapsel.manifest generated/kapsel.manifest COPYONLY)

include(FetchContent)
FetchContent_Declare(libvterm
    URL https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz
    URL_HASH SHA256=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0)
FetchContent_MakeAvailable(libvterm)
add_library(vterm STATIC
    ${libvterm_SOURCE_DIR}/src/encoding.c
    ${libvterm_SOURCE_DIR}/src/keyboard.c
    ${libvterm_SOURCE_DIR}/src/mouse.c
    ${libvterm_SOURCE_DIR}/src/parser.c
    ${libvterm_SOURCE_DIR}/src/pen.c
    ${libvterm_SOURCE_DIR}/src/screen.c
    ${libvterm_SOURCE_DIR}/src/state.c
    ${libvterm_SOURCE_DIR}/src/unicode.c
    ${libvterm_SOURCE_DIR}/src/vterm.c)
target_include_directories(vterm PUBLIC ${libvterm_SOURCE_DIR}/include)
target_compile_definitions(vterm PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(Kapsel WIN32
    src/app/main.cpp
    src/app/app_window.cpp
    src/app/settings.cpp
    src/terminal/terminal_view.cpp
    src/virtualization/vm_session.cpp
    src/virtualization/qmp_client.cpp
    packaging/windows/kapsel.rc)
target_compile_features(Kapsel PRIVATE cxx_std_17)
target_compile_definitions(Kapsel PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00)
target_include_directories(Kapsel PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(Kapsel PRIVATE vterm comdlg32 dwmapi gdi32 shell32 advapi32 bcrypt)
if(MINGW)
    target_compile_options(vterm PRIVATE -Wno-attributes)
    target_compile_options(Kapsel PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
    target_link_options(Kapsel PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS Kapsel RUNTIME DESTINATION .)
install(FILES "${libvterm_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME libvterm.txt)
