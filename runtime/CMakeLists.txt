set(SKY_RUNNER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH
    "Directory for the SkyRecompiled runner executable")

if(NOT DEFINED NDS_RECOMP_UI_ROOT AND NOT DEFINED ENV{RECOMP_UI_ROOT})
    set(NDS_RECOMP_UI_ROOT "${PROJECT_SOURCE_DIR}/third_party/recomp-ui" CACHE PATH
        "Path to the pinned recomp-ui checkout")
endif()

add_library(sky_runtime_enhancements STATIC
    "${CMAKE_CURRENT_LIST_DIR}/gpu2d/widescreen_ppu.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/dungeon_camera.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/subpixel_interpolator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shaders/weather_shader_pipeline.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ui/settings_overlay.cpp"
    "${NDS_RECOMP_UI_ROOT}/src/common/recomp_runtime_ui.c"
)
target_compile_features(sky_runtime_enhancements PUBLIC cxx_std_20)
target_include_directories(sky_runtime_enhancements PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${NDS_RECOMP_UI_ROOT}/src"
)

if(DEFINED NDS_GENERATED_DIR)
    set(_sky_gen_dir "${NDS_GENERATED_DIR}")
else()
    set(_sky_gen_dir "${CMAKE_CURRENT_LIST_DIR}/../third_party/ndsrecomp/generated")
endif()

if(EXISTS "${_sky_gen_dir}/arm9_bios.c")
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/ndsrecomp/runner" ndsrecomp)
    target_link_libraries(nds_runner PRIVATE sky_runtime_enhancements)
    set_target_properties(nds_runner PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SKY_RUNNER_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SKY_RUNNER_OUTPUT_DIR}")
endif()

add_executable(pmdsky_bindings_test "${PROJECT_SOURCE_DIR}/tests/pmdsky_bindings_test.cpp")
target_link_libraries(pmdsky_bindings_test PRIVATE sky_runtime_enhancements)
add_test(NAME pmdsky_bindings_test COMMAND pmdsky_bindings_test)

add_executable(milestone3_runtime_test "${PROJECT_SOURCE_DIR}/tests/milestone3_runtime_test.cpp")
target_link_libraries(milestone3_runtime_test PRIVATE sky_runtime_enhancements)
add_test(NAME milestone3_runtime_test COMMAND milestone3_runtime_test)
