cmake_minimum_required(VERSION 3.23)
project(SkyRecompiled LANGUAGES C CXX)

# SkyRecompiled root entry point for CMakePresets
add_subdirectory(third_party/ndsrecomp/runner runner_build)
