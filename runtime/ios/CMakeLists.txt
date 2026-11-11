cmake_minimum_required(VERSION 3.23)
project(SkyRecompiledIOS LANGUAGES C CXX OBJC OBJCXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=iOS")
endif()

if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(SKY_BUNDLE_PLATFORM iPhoneSimulator)
else()
    set(SKY_BUNDLE_PLATFORM iPhoneOS)
endif()

set(SKY_NDSRECOMP_DIR "" CACHE PATH "Patched ndsrecomp checkout")
set(SKY_SDL_DIR "" CACHE PATH "Pinned SDL source directory")
set(SKY_IOS_OUTPUT_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Directory for the iOS app")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# Use Apple GameController without requesting raw Bluetooth access.
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
add_subdirectory("${SKY_SDL_DIR}" sdl EXCLUDE_FROM_ALL)

# The iOS Metal presenter uses the software renderer at native resolution.
set(NDS_ENABLE_COMPUTE_RENDERER OFF CACHE BOOL "" FORCE)
set(NDS_BOOTSTRAP_FIRMWARE ON CACHE BOOL "" FORCE)
foreach(bank IN ITEMS NDS_TITLE_BANK_DIR NDS_TITLE_BANK_DIRS
                      NDS_SM64DS_BANK_DIR NDS_PREBUILT_BANK_ARCHIVE)
    if(${bank})
        message(FATAL_ERROR "${bank} cannot be used in the ROM-free iOS build")
    endif()
endforeach()
add_subdirectory("${SKY_NDSRECOMP_DIR}/runner" runner EXCLUDE_FROM_ALL)

add_library(sky_runtime_enhancements STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../gpu2d/widescreen_ppu.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../camera/dungeon_camera.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../camera/subpixel_interpolator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../shaders/weather_shader_pipeline.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../ui/settings_overlay.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../ui/remaster_hud.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../ui/guest_ui_state.cpp"
    "${SKY_NDSRECOMP_DIR}/../recomp-ui/src/common/recomp_runtime_ui.c"
)
target_compile_features(sky_runtime_enhancements PUBLIC cxx_std_20)
target_include_directories(sky_runtime_enhancements PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../include"
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${SKY_NDSRECOMP_DIR}/../recomp-ui/src"
)
target_link_libraries(nds_runner PRIVATE sky_runtime_enhancements)

target_sources(nds_runner PRIVATE Launcher.mm RomCompiler.cpp)
target_include_directories(nds_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(nds_runner PRIVATE SKY_IOS=1)
target_compile_options(nds_runner PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_link_libraries(nds_runner PRIVATE "-framework UIKit" "-framework UniformTypeIdentifiers")
target_link_options(nds_runner PRIVATE -Wl,-stack_size,0x4000000)
set_target_properties(nds_runner PROPERTIES
    OUTPUT_NAME SkyRecompiled
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED YES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    RUNTIME_OUTPUT_DIRECTORY "${SKY_IOS_OUTPUT_DIR}")
