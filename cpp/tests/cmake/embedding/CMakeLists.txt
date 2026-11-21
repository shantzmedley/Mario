cmake_minimum_required(VERSION 3.24)

project(MarioEmbeddingSmoke LANGUAGES CXX)

if(NOT DEFINED MARIO_SOURCE_DIR)
    message(FATAL_ERROR "MARIO_SOURCE_DIR is required")
endif()

set(SYNCVIBE_BUILD_TESTS ON CACHE BOOL "Parent-owned SyncVibe test setting")
set(MARIO_BUILD_EDITOR OFF CACHE BOOL "" FORCE)
set(MARIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MARIO_ENABLE_NETWORK_PLAY ON CACHE BOOL "" FORCE)

add_library(parent_syncvibe_runtime INTERFACE)
add_library(SyncVibe::Runtime ALIAS parent_syncvibe_runtime)

add_subdirectory("${MARIO_SOURCE_DIR}" mario)

if(NOT SYNCVIBE_BUILD_TESTS)
    message(FATAL_ERROR
        "Mario overwrote the parent-owned SYNCVIBE_BUILD_TESTS cache setting")
endif()
