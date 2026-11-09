set(SKY_RUNNER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH
    "Directory for the SkyRecompiled runner executable")

if(NOT DEFINED NDS_RECOMP_UI_ROOT AND NOT DEFINED ENV{RECOMP_UI_ROOT})
    set(NDS_RECOMP_UI_ROOT "${PROJECT_SOURCE_DIR}/third_party/recomp-ui" CACHE PATH
        "Path to the pinned recomp-ui checkout")
endif()

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/ndsrecomp/runner" ndsrecomp)
set_target_properties(nds_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SKY_RUNNER_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SKY_RUNNER_OUTPUT_DIR}")
