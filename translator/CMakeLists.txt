cmake_minimum_required(VERSION 3.23)
project(SkyRecompiledTranslator LANGUAGES CXX)

get_filename_component(SKY_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SKY_RECOMPILER_OUTPUT_DIR "${SKY_REPO_ROOT}/build/recompiler" CACHE PATH
    "Directory for the offline recompiler executable")

enable_testing()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/ndsrecomp/recompiler" ndsrecomp)
set_target_properties(nds_recompile PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${SKY_RECOMPILER_OUTPUT_DIR}>")
