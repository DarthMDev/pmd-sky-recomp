cmake_minimum_required(VERSION 3.23)
project(SkyRecompiledTranslator LANGUAGES CXX)

set(SKY_RECOMPILER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH
    "Directory for the offline recompiler executable")

enable_testing()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/ndsrecomp/recompiler" ndsrecomp)
set_target_properties(nds_recompile PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SKY_RECOMPILER_OUTPUT_DIR}")
