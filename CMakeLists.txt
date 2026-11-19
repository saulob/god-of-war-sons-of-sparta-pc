cmake_minimum_required(VERSION 3.24)
project(sons_of_sparta_pc VERSION 0.2.0 LANGUAGES C CXX)

if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Build with Clang: MSYS2 CLANG64 on Windows.")
endif()
if (NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"))
    message(FATAL_ERROR "Supported targets: Windows x64 and Linux x86_64.")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (WIN32)
    set(CMAKE_C_EXTENSIONS OFF)
else()
    set(CMAKE_C_EXTENSIONS ON)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer>")

find_package(SDL3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)

# The shadPS4-derived video core (Liverpool/Vulkan renderer, shader recompiler, GnmDriver, VideoOut and kernel equeues).
add_subdirectory(gpu)
add_subdirectory(launcher)

# ATRAC9 decoding for libSceAjm (third_party/LibAtrac9, fetched by tools\build.ps1).
set(SOS_ATRAC9_DIR ${CMAKE_SOURCE_DIR}/third_party/LibAtrac9/C/src)
if (NOT EXISTS ${SOS_ATRAC9_DIR}/libatrac9.h)
    message(FATAL_ERROR "LibAtrac9 is missing: run tools\\build.ps1, which fetches it (gpu\\deps.txt).")
endif()
file(GLOB SOS_ATRAC9_SOURCES CONFIGURE_DEPENDS ${SOS_ATRAC9_DIR}/*.c)
add_library(sos_atrac9 STATIC ${SOS_ATRAC9_SOURCES})
target_include_directories(sos_atrac9 PUBLIC ${SOS_ATRAC9_DIR})
target_compile_options(sos_atrac9 PRIVATE -w)
target_link_libraries(sos_atrac9 PUBLIC $<$<NOT:$<BOOL:${WIN32}>>:m>)
set_target_properties(sos_atrac9 PROPERTIES C_EXTENSIONS ON)

set(SOS_RUNTIME_SOURCES
    src/runtime.c
    src/runtime_kernel.c
    src/runtime_posix_socket.c
    src/runtime_memory.c
    src/runtime_thread.c
    src/runtime_mutex.c
    src/runtime_sema.c
    src/runtime_rwlock.c
    src/runtime_fiber.c
    src/runtime_time.c
    src/runtime_rtc.c
    src/runtime_eventflag.c
    src/runtime_file.c
    src/runtime_services.c
    src/runtime_savedata.c
    src/runtime_content.c
    src/runtime_pad.c
    src/runtime_move.c
    src/runtime_audio.c
    src/runtime_audio3d.c
    src/runtime_ajm.c
    src/runtime_users.c
    src/runtime_np.c
    src/runtime_np_toolkit2.c
    src/runtime_font.c
    src/runtime_signal.c
    src/runtime_stop.c
    src/runtime_sos.c
    src/runtime_extra.c
    src/runtime_umtx.c
    src/runtime_redzone.c
    src/runtime_module.c
    src/runtime_history.c
    src/sos_keys.c
    src/dcp_keys.c
    src/dcp_mouse.c
    src/runtime_online.cpp
    src/runtime_json.cpp
    src/trophies/trophy_runtime.cpp
    src/isa_emulate.c
    src/startup_phase.c)
set(SOS_NOTO_SANS ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSans-Regular.ttf)
set(SOS_NOTO_SANS_TC ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSansTC-Regular.otf)
set_source_files_properties(src/runtime_font.c PROPERTIES
    COMPILE_DEFINITIONS "SOS_NOTO_SANS_PATH=\"${SOS_NOTO_SANS}\";SOS_NOTO_SANS_TC_PATH=\"${SOS_NOTO_SANS_TC}\""
    OBJECT_DEPENDS "${SOS_NOTO_SANS};${SOS_NOTO_SANS_TC}")

if (WIN32)
    set(SOS_PLATFORM_SOURCES src/compat_win.c src/crash_guard.c src/runtime_log.c src/runtime_session_log.c)
else()
    set(SOS_PLATFORM_SOURCES src/runtime_log_posix.c src/runtime_session_log.c)
endif()

add_executable(sons_of_sparta src/loader.c ${SOS_RUNTIME_SOURCES} ${SOS_PLATFORM_SOURCES})
target_include_directories(sons_of_sparta PRIVATE src launcher)
target_compile_options(sons_of_sparta PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
if (WIN32)
    target_compile_definitions(sons_of_sparta PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_options(sons_of_sparta PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    target_link_libraries(sons_of_sparta PRIVATE sos_launcher dcpgpu sos_atrac9 SDL3::SDL3 Freetype::Freetype onecore bcrypt ws2_32 -pthread)
else()
    target_link_options(sons_of_sparta PRIVATE -rdynamic)
    target_link_libraries(sons_of_sparta PRIVATE sos_launcher dcpgpu sos_atrac9 SDL3::SDL3 Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
