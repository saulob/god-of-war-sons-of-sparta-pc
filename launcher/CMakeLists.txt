# The launcher: TRG Launcher (third_party/trg-launcher, fetched by tools\build.ps1) in a window of its own that is
# closed before the loader creates the game window.
set(TRG_DIR ${PROJECT_SOURCE_DIR}/third_party/trg-launcher)
if (NOT EXISTS ${TRG_DIR}/include/trg/launcher.h)
    message(FATAL_ERROR "TRG Launcher is missing: run tools\\build.ps1, which fetches it (gpu\\deps.txt).")
endif()
set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/gpu/third_party/imgui)

# TRG's settings file, platform helpers and HTTPS downloads do not need Dear ImGui.
add_library(trg_core STATIC ${TRG_DIR}/src/settings.cpp ${TRG_DIR}/src/platform.cpp ${TRG_DIR}/src/download.cpp)
target_include_directories(trg_core PUBLIC ${TRG_DIR}/include)
target_compile_definitions(trg_core PRIVATE _CRT_SECURE_NO_WARNINGS)
if (WIN32)
    target_link_libraries(trg_core PRIVATE shell32 ole32 comdlg32 winhttp)
else()
    target_link_libraries(trg_core PRIVATE Threads::Threads)
endif()

# The launcher draws with the Dear ImGui compiled into the video core library.
add_library(trg_launcher STATIC ${TRG_DIR}/src/launcher.cpp ${TRG_DIR}/src/headers.cpp)
target_include_directories(trg_launcher PUBLIC ${IMGUI_DIR})
target_compile_definitions(trg_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(trg_launcher PUBLIC trg_core)

# Written for C++17: in C++20 and later u8string() is a different type.
set_target_properties(trg_core trg_launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(sos_imgui_sdl STATIC ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(sos_imgui_sdl PUBLIC ${IMGUI_DIR})
target_link_libraries(sos_imgui_sdl PUBLIC SDL3::SDL3)

# Game files (check, PKG unpacking, install, boot image), the trophies (src/trophies) and the settings file: no window
# needed (tests link only this).
find_package(ZLIB REQUIRED)
add_library(sos_launcher_core STATIC boot_image.cpp bug_report.cpp bug_report_http.cpp build_diagnostics.cpp
    controller_icons.cpp controller_prompts.c prompt_dialog.c file_browser.cpp
    game_files.cpp
    game_install.cpp game_process.cpp game_profile.cpp header_art.cpp log_scrubber.cpp options.cpp page_icons.cpp paths.cpp
    pkg_extract.cpp resolution.cpp settings.cpp trophy_content.cpp localization/ui_text.cpp
    updater/json.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_data.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_key.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_pack.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_runtime.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_service.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_store.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_view.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trp_archive.cpp)
target_include_directories(sos_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(sos_launcher_core PUBLIC SOS_PORT_VERSION="${PROJECT_VERSION}")
target_link_libraries(sos_launcher_core PUBLIC trg_core SDL3::SDL3 sos_keys sos_startup_phase PRIVATE ZLIB::ZLIB)
# Hashes and ciphers: CNG on Windows, OpenSSL's libcrypto elsewhere.
if (WIN32)
    target_sources(sos_launcher_core PRIVATE crypto_win.cpp)
    target_link_libraries(sos_launcher_core PRIVATE bcrypt winhttp)
else()
    find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
    target_sources(sos_launcher_core PRIVATE crypto_openssl.cpp)
    target_link_libraries(sos_launcher_core PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endif()

# The sidebar's page icons (assets/launcher/icons/*.svg) as a table of byte arrays, page_icons.cpp.
file(GLOB SOS_PAGE_ICONS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/assets/launcher/icons/*.svg)
list(SORT SOS_PAGE_ICONS)
set(SOS_PAGE_ICON_LIST ${CMAKE_BINARY_DIR}/generated/page_icons.txt)
file(CONFIGURE OUTPUT ${SOS_PAGE_ICON_LIST} CONTENT "${SOS_PAGE_ICONS}")
set(SOS_PAGE_ICON_SOURCE ${CMAKE_BINARY_DIR}/generated/page_icon_data.cpp)
add_custom_command(OUTPUT ${SOS_PAGE_ICON_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${SOS_PAGE_ICON_LIST} -DOUTPUT=${SOS_PAGE_ICON_SOURCE} -DSYMBOL=kPageIconFiles
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    DEPENDS ${SOS_PAGE_ICON_LIST} ${SOS_PAGE_ICONS} ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    COMMENT "Embedding the page icons")
target_sources(sos_launcher_core PRIVATE ${SOS_PAGE_ICON_SOURCE})

add_library(sos_launcher STATIC bug_report_page.cpp host.cpp launcher.cpp package_browser.cpp problem_dialog.cpp shift_key.cpp
    trophy_page.cpp update_config.cpp
    updater/github_release.cpp updater/sha256.cpp updater/third_party/miniz/miniz.c
    updater/update_controller.cpp updater/update_http.cpp updater/update_job.cpp updater/update_paths.cpp
    updater/update_platform.cpp updater/update_ui.cpp updater/update_zip.cpp updater/version.cpp)
set_source_files_properties(updater/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(sos_launcher PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/gpu/shim)
target_link_libraries(sos_launcher PUBLIC sos_launcher_core trg_launcher sos_imgui_sdl ${CMAKE_DL_LIBS})

# Windows helper that replaces the launcher after it exits; not part of the Linux package.
if (WIN32)
    add_executable(trg-updater WIN32 updater/helper_main.cpp updater/install_transaction.cpp updater/sha256.cpp
        updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp)
    target_include_directories(trg-updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(trg-updater PRIVATE shell32)
    target_link_options(trg-updater PRIVATE -static)
    set_target_properties(trg-updater PROPERTIES OUTPUT_NAME "trg-updater" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(trg-updater PRIVATE -Wall -Wextra)
endif()

set_target_properties(sos_launcher_core sos_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_options(sos_launcher_core PRIVATE -Wall -Wextra)
target_compile_options(sos_launcher PRIVATE -Wall -Wextra)
