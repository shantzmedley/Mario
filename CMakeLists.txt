cmake_minimum_required(VERSION 3.24)

project(MarioEditor VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)

option(MARIO_BUILD_EDITOR "Build the SDL3 + Dear ImGui editor" ON)
option(MARIO_BUILD_TESTS "Build Mario automated tests" ${BUILD_TESTING})

get_filename_component(
    _mario_default_syncvibe_root
    "${CMAKE_CURRENT_SOURCE_DIR}/../SyncVibe"
    ABSOLUTE
)
set(
    MARIO_SYNCVIBE_ROOT
    "${_mario_default_syncvibe_root}"
    CACHE PATH
    "Path to a SyncVibe checkout"
)
if(EXISTS "${MARIO_SYNCVIBE_ROOT}/CMakeLists.txt")
    set(_mario_network_default ON)
else()
    set(_mario_network_default OFF)
endif()
option(
    MARIO_ENABLE_NETWORK_PLAY
    "Build the SyncVibe-backed local network Play mode"
    ${_mario_network_default}
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(mario_core
    cpp/src/core/LevelIO.cpp
    cpp/src/core/ProjectPaths.cpp
    cpp/src/core/World.cpp
)
add_library(Mario::Core ALIAS mario_core)
target_include_directories(mario_core PUBLIC cpp/include)
target_link_libraries(mario_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(mario_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(mario_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(mario_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MARIO_ENABLE_NETWORK_PLAY)
    if(NOT TARGET SyncVibe::Runtime)
        if(NOT EXISTS "${MARIO_SYNCVIBE_ROOT}/CMakeLists.txt")
            message(FATAL_ERROR
                "MARIO_ENABLE_NETWORK_PLAY=ON but MARIO_SYNCVIBE_ROOT does not contain "
                "CMakeLists.txt: ${MARIO_SYNCVIBE_ROOT}")
        endif()

        if(NOT DEFINED CACHE{SYNCVIBE_BUILD_RUNTIME})
            set(SYNCVIBE_BUILD_RUNTIME ON CACHE BOOL
                "Build the headless SyncVibe runtime")
        endif()
        if(NOT DEFINED CACHE{SYNCVIBE_BUILD_DEMOS})
            set(SYNCVIBE_BUILD_DEMOS OFF CACHE BOOL
                "Build SyncVibe legacy demos")
        endif()
        if(NOT DEFINED CACHE{SYNCVIBE_BUILD_RENDERER})
            set(SYNCVIBE_BUILD_RENDERER OFF CACHE BOOL
                "Build the SyncVibe legacy renderer")
        endif()
        if(NOT DEFINED CACHE{SYNCVIBE_BUILD_CLI})
            set(SYNCVIBE_BUILD_CLI OFF CACHE BOOL
                "Build the SyncVibe legacy CLI")
        endif()
        if(NOT DEFINED CACHE{SYNCVIBE_BUILD_TESTS})
            set(SYNCVIBE_BUILD_TESTS OFF CACHE BOOL
                "Build SyncVibe tests")
        endif()

        add_subdirectory(
            "${MARIO_SYNCVIBE_ROOT}"
            "${CMAKE_CURRENT_BINARY_DIR}/_syncvibe"
            EXCLUDE_FROM_ALL
        )
        if(NOT TARGET SyncVibe::Runtime)
            message(FATAL_ERROR
                "MARIO_ENABLE_NETWORK_PLAY requires SyncVibe::Runtime. "
                "Ensure SYNCVIBE_BUILD_RUNTIME is enabled.")
        endif()
    endif()
endif()

add_library(mario_play cpp/src/play/PlaySession.cpp)
add_library(Mario::Play ALIAS mario_play)
target_include_directories(mario_play PUBLIC cpp/include)
target_compile_features(mario_play PUBLIC cxx_std_20)
target_compile_definitions(mario_play PRIVATE
    MARIO_ENABLE_NETWORK_PLAY=$<BOOL:${MARIO_ENABLE_NETWORK_PLAY}>
)
if(MARIO_ENABLE_NETWORK_PLAY)
    target_link_libraries(mario_play PRIVATE SyncVibe::Runtime)
endif()
if(MSVC)
    target_compile_options(mario_play PRIVATE /W4 /permissive-)
else()
    target_compile_options(mario_play PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(
    mario_editor_integration
    cpp/src/editor/EditorPlayIntegration.cpp
)
add_library(Mario::EditorIntegration ALIAS mario_editor_integration)
target_include_directories(mario_editor_integration PUBLIC cpp/include)
target_link_libraries(
    mario_editor_integration
    PUBLIC Mario::Core Mario::Play
)
target_compile_features(mario_editor_integration PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(mario_editor_integration PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        mario_editor_integration PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MARIO_BUILD_TESTS)
    add_executable(mario_core_tests cpp/tests/CoreTests.cpp)
    target_link_libraries(mario_core_tests PRIVATE Mario::Core)
    target_compile_definitions(mario_core_tests PRIVATE
        MARIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME mario_core_tests COMMAND mario_core_tests)

    add_test(
        NAME mario_cmake_embedding_hygiene
        COMMAND ${CMAKE_COMMAND}
            -DMARIO_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DSMOKE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/cmake/embedding
            -DSMOKE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/cmake-embedding-smoke/$<CONFIG>
            -DNLOHMANN_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}
            -DTEST_GENERATOR=${CMAKE_GENERATOR}
            -DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/cmake/EmbeddingSmoke.cmake
    )
    set_tests_properties(mario_cmake_embedding_hygiene PROPERTIES TIMEOUT 180)

    if(MARIO_ENABLE_NETWORK_PLAY)
        add_executable(
            mario_network_play_tests
            cpp/tests/NetworkPlayTests.cpp
        )
        target_link_libraries(
            mario_network_play_tests
            PRIVATE Mario::Core Mario::Play Mario::EditorIntegration
        )
        target_compile_definitions(mario_network_play_tests PRIVATE
            MARIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )
        add_test(NAME mario_network_play_tests COMMAND mario_network_play_tests)
        set_tests_properties(mario_network_play_tests PROPERTIES TIMEOUT 20)
    endif()
endif()

if(MARIO_BUILD_EDITOR)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(MARIO_PKG_CONFIG_EXECUTABLE pkg-config)
        if(NOT MARIO_PKG_CONFIG_EXECUTABLE)
            message(FATAL_ERROR
                "Building mario_editor on Linux requires pkg-config and SDL3 "
                "desktop development packages. On Ubuntu/WSL run "
                "'bash scripts/setup-wsl.sh' before configuring.")
        endif()

        execute_process(
            COMMAND ${MARIO_PKG_CONFIG_EXECUTABLE} --exists x11
            RESULT_VARIABLE _mario_has_x11
        )
        execute_process(
            COMMAND ${MARIO_PKG_CONFIG_EXECUTABLE} --exists wayland-client
            RESULT_VARIABLE _mario_has_wayland
        )
        if(NOT _mario_has_x11 EQUAL 0 AND NOT _mario_has_wayland EQUAL 0)
            message(FATAL_ERROR
                "Building mario_editor on Linux requires X11 or Wayland "
                "development packages. On Ubuntu/WSL run "
                "'bash scripts/setup-wsl.sh' before configuring.")
        endif()
    endif()

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG 877399b2b2cf21e67554ed9046410f268ce1d1b2
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG 1aea4d1bb204ef748457e9031c9bb6229b204c6d
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG 9600522de53973336fa033a289858a4354cbb04c
    )
    FetchContent_MakeAvailable(SDL3 imgui stb)

    add_library(mario_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    )
    target_include_directories(mario_imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(mario_imgui PUBLIC SDL3::SDL3)
    target_compile_definitions(mario_imgui PUBLIC IMGUI_ENABLE_DOCKING)

    add_executable(mario_editor
        cpp/src/editor/EditorApp.cpp
        cpp/src/editor/main.cpp
    )
    target_include_directories(mario_editor PRIVATE
        cpp/src/editor
        ${stb_SOURCE_DIR}
    )
    target_link_libraries(mario_editor PRIVATE
        Mario::Core
        Mario::EditorIntegration
        Mario::Play
        mario_imgui
        SDL3::SDL3
    )
    target_compile_definitions(mario_editor PRIVATE
        MARIO_DEFAULT_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(mario_editor PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(mario_editor PRIVATE /W4 /permissive-)
        set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT mario_editor)
    else()
        target_compile_options(mario_editor PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(MARIO_BUILD_TESTS AND MARIO_ENABLE_NETWORK_PLAY)
        add_test(
            NAME mario_network_smoke
            COMMAND mario_editor
                --project "${CMAKE_CURRENT_SOURCE_DIR}"
                --network-smoke-test
        )
        set_tests_properties(mario_network_smoke PROPERTIES TIMEOUT 20)
    endif()
endif()
