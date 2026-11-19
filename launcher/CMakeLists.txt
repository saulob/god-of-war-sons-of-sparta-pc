# The launcher: TRG Launcher (third_party/trg-launcher) in a window of its own that is closed before the loader creates
# the game window.
set(TRG_DIR ${PROJECT_SOURCE_DIR}/third_party/trg-launcher)
if (NOT EXISTS ${TRG_DIR}/include/trg/launcher.h)
    message(FATAL_ERROR "TRG Launcher is missing: run tools\\build.ps1, which fetches it (gpu\\deps.txt).")
endif()
set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/gpu/third_party/imgui)

add_library(trg_core STATIC ${TRG_DIR}/src/settings.cpp ${TRG_DIR}/src/platform.cpp ${TRG_DIR}/src/download.cpp)
target_include_directories(trg_core PUBLIC ${TRG_DIR}/include)
target_compile_definitions(trg_core PRIVATE _CRT_SECURE_NO_WARNINGS)
if (WIN32)
    target_link_libraries(trg_core PRIVATE shell32 ole32 comdlg32 winhttp)
else()
    target_link_libraries(trg_core PRIVATE Threads::Threads)
endif()

add_library(trg_launcher STATIC ${TRG_DIR}/src/launcher.cpp ${TRG_DIR}/src/headers.cpp)
target_include_directories(trg_launcher PUBLIC ${IMGUI_DIR})
target_compile_definitions(trg_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(trg_launcher PUBLIC trg_core)

set_target_properties(trg_core trg_launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(sos_imgui_sdl STATIC ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(sos_imgui_sdl PUBLIC ${IMGUI_DIR})
target_link_libraries(sos_imgui_sdl PUBLIC SDL3::SDL3)

find_package(ZLIB REQUIRED)
add_library(sos_launcher_core STATIC boot_image.cpp build_diagnostics.cpp file_browser.cpp game_files.cpp
    game_install.cpp game_profile.cpp paths.cpp pkg_extract.cpp)
target_include_directories(sos_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(sos_launcher_core PUBLIC trg_core SDL3::SDL3 PRIVATE ZLIB::ZLIB)
if (WIN32)
    target_sources(sos_launcher_core PRIVATE crypto_win.cpp)
    target_link_libraries(sos_launcher_core PRIVATE bcrypt winhttp)
else()
    find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
    target_sources(sos_launcher_core PRIVATE crypto_openssl.cpp)
    target_link_libraries(sos_launcher_core PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endif()

add_library(sos_launcher STATIC launcher.cpp host.cpp options.cpp settings.cpp)
target_include_directories(sos_launcher PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/gpu/shim)
target_link_libraries(sos_launcher PUBLIC sos_launcher_core trg_launcher sos_imgui_sdl ${CMAKE_DL_LIBS})
set_target_properties(sos_launcher sos_launcher_core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
