cmake_minimum_required(VERSION 3.24)
project(sons_of_sparta_pc VERSION 0.1.0 LANGUAGES C CXX)

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
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=>")
if (NOT WIN32)
    # Non-PIE: host heap and TLS handed to guest code must stay below 1 TiB.
    set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fno-pie>")
    add_link_options(-no-pie)
endif()

if (NOT PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "project VERSION must start with MAJOR.MINOR.PATCH")
endif()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/sos_version.h CONTENT "#define SOS_VERSION_NUMBERS \
${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0\n#define SOS_FILE_VERSION_STRING \
\"${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0\"\n#define SOS_VERSION_STRING \"${PROJECT_VERSION}\"\n")

find_package(SDL3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)

# Keyboard bindings shared by the launcher, the runtime pad and the video core.
add_library(sos_keys STATIC src/sos_keys.c)
target_include_directories(sos_keys PUBLIC src)
target_link_libraries(sos_keys PUBLIC SDL3::SDL3)
target_compile_options(sos_keys PRIVATE -Wall -Wextra -Werror)

# The stage the process is in, for the crash report: used by the launcher, the loader and the window thread.
add_library(sos_startup_phase STATIC src/startup_phase.c
    src/system_report.c)
target_include_directories(sos_startup_phase PUBLIC src)
target_compile_options(sos_startup_phase PRIVATE -Wall -Wextra -Werror)

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
    src/guest_entry.c
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
    src/redzone_analysis.c
    src/redzone_cache.c
    src/startup_profile.c
    src/sos_skip_intro.c
    src/sos_hermes_unlock.c
    src/sos_pit_unlock.c
    src/sos_exit_game.c
    src/runtime_module.c
    src/runtime_history.c
    src/dcp_keys.c
    src/dcp_mouse.c
    src/runtime_online.cpp
    src/runtime_json.cpp
    src/isa_emulate.c)
set(SOS_NOTO_SANS ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSans-Regular.ttf)
set(SOS_NOTO_SANS_TC ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSansTC-Regular.otf)
set_source_files_properties(src/runtime_font.c PROPERTIES
    COMPILE_DEFINITIONS "SOS_NOTO_SANS_PATH=\"${SOS_NOTO_SANS}\";SOS_NOTO_SANS_TC_PATH=\"${SOS_NOTO_SANS_TC}\""
    OBJECT_DEPENDS "${SOS_NOTO_SANS};${SOS_NOTO_SANS_TC}")

if (WIN32)
    enable_language(RC)
    # Relink when the application icon, the manifest or the version changes.
    set_source_files_properties(src/sons_of_sparta.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/packaging/icons/app-icon.ico;${CMAKE_SOURCE_DIR}/src/sons_of_sparta.manifest;${CMAKE_BINARY_DIR}/generated/sos_version.h")
    set(SOS_PLATFORM_SOURCES src/compat_win.c src/crash_guard.c src/runtime_log.c src/runtime_session_log.c
        src/sons_of_sparta.rc)
else()
    set(SOS_PLATFORM_SOURCES src/runtime_log_posix.c src/runtime_session_log.c src/fault_signal_posix.c)
endif()

add_executable(sons_of_sparta WIN32 src/loader.c ${SOS_RUNTIME_SOURCES} ${SOS_PLATFORM_SOURCES})
target_include_directories(sons_of_sparta PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
target_compile_options(sons_of_sparta PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
if (WIN32)
    target_compile_definitions(sons_of_sparta PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_options(sons_of_sparta PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    target_link_libraries(sons_of_sparta PRIVATE sos_launcher dcpgpu sos_atrac9 SDL3::SDL3 Freetype::Freetype onecore bcrypt ws2_32 -pthread)
else()
    target_link_options(sons_of_sparta PRIVATE -rdynamic)
    target_link_libraries(sons_of_sparta PRIVATE sos_launcher dcpgpu sos_atrac9 SDL3::SDL3 Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Links every object of dcpgpu with the loader's runtime, so undefined and duplicate symbols show although a static library
# hides them (tools/test.ps1). Not part of the default build.
set(SOS_SMOKE_PLATFORM_SOURCES ${SOS_PLATFORM_SOURCES})
list(FILTER SOS_SMOKE_PLATFORM_SOURCES EXCLUDE REGEX "[.]rc$")
add_executable(dcpgpu_link_smoke EXCLUDE_FROM_ALL src/gpu_link_smoke.cpp ${SOS_RUNTIME_SOURCES} ${SOS_SMOKE_PLATFORM_SOURCES})
target_include_directories(dcpgpu_link_smoke PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
if (WIN32)
    target_compile_definitions(dcpgpu_link_smoke PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_libraries(dcpgpu_link_smoke PRIVATE sos_launcher_core "$<LINK_LIBRARY:WHOLE_ARCHIVE,dcpgpu>" sos_keys sos_atrac9 SDL3::SDL3 Freetype::Freetype onecore bcrypt ws2_32 -pthread)
else()
    target_link_libraries(dcpgpu_link_smoke PRIVATE sos_launcher_core "$<LINK_LIBRARY:WHOLE_ARCHIVE,dcpgpu>" sos_keys sos_atrac9 SDL3::SDL3 Freetype::Freetype Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# The launcher reads its images next to the executable; each copy is a build output, so an image-only change syncs.
set(SOS_LAUNCHER_ASSET_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/launcher")
set(SOS_LAUNCHER_ASSET_COPIES)
foreach(asset header-art.png sponsor-heart.png)
    set(asset_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/${asset}")
    set(asset_copy "${SOS_LAUNCHER_ASSET_DIR}/${asset}")
    add_custom_command(OUTPUT "${asset_copy}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SOS_LAUNCHER_ASSET_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${asset_source}" "${asset_copy}"
        COMMAND ${CMAKE_COMMAND} -E touch_nocreate "${asset_copy}"
        DEPENDS "${asset_source}"
        VERBATIM)
    list(APPEND SOS_LAUNCHER_ASSET_COPIES "${asset_copy}")
endforeach()
add_custom_target(sos_launcher_assets DEPENDS ${SOS_LAUNCHER_ASSET_COPIES})
add_dependencies(sons_of_sparta sos_launcher_assets)
if (WIN32)
    # The launcher's updater installs through this helper, which must sit beside the executable.
    add_dependencies(sons_of_sparta trg-updater)
endif()
