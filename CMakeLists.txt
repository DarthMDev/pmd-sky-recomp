cmake_minimum_required(VERSION 3.23)
project(SkyRecompiled LANGUAGES C CXX)

if(APPLE AND (NOT DEFINED NDS_SDL_BACKEND OR NDS_SDL_BACKEND STREQUAL "SDL3"))
    enable_language(OBJCXX)
endif()

enable_testing()
add_subdirectory(runtime runner_build)
