set(sky_metal_hd_source "${CMAKE_CURRENT_LIST_DIR}/metal_hd_presenter.mm")
set(sky_metal_hd_frameworks "-framework Foundation" "-framework Metal" "-framework QuartzCore")
target_sources(nds_runner PRIVATE "${sky_metal_hd_source}")
target_compile_definitions(nds_runner PRIVATE SKY_HAVE_METAL_HD=1)
set_source_files_properties("${sky_metal_hd_source}" TARGET_DIRECTORY nds_runner
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
set_target_properties(nds_runner PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
target_link_libraries(nds_runner PRIVATE ${sky_metal_hd_frameworks})

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_package(SDL3 CONFIG REQUIRED)
    add_executable(metal_hd_presenter_test
        "${PROJECT_SOURCE_DIR}/tests/metal_hd_presenter_test.cpp" "${sky_metal_hd_source}")
    set_source_files_properties("${sky_metal_hd_source}" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(metal_hd_presenter_test PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(metal_hd_presenter_test PRIVATE sky_runtime_enhancements SDL3::SDL3
        ${sky_metal_hd_frameworks})
    add_test(NAME metal_hd_presenter_test COMMAND metal_hd_presenter_test)
    set_tests_properties(metal_hd_presenter_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
unset(sky_metal_hd_source)
unset(sky_metal_hd_frameworks)
