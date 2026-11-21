if(NOT DEFINED MARIO_SOURCE_DIR OR
   NOT DEFINED SMOKE_SOURCE_DIR OR
   NOT DEFINED SMOKE_BINARY_DIR OR
   NOT DEFINED NLOHMANN_JSON_SOURCE_DIR OR
   NOT DEFINED TEST_GENERATOR)
    message(FATAL_ERROR "Embedding smoke test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${SMOKE_BINARY_DIR}")

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${SMOKE_SOURCE_DIR}"
    -B "${SMOKE_BINARY_DIR}"
    -G "${TEST_GENERATOR}"
    "-DMARIO_SOURCE_DIR=${MARIO_SOURCE_DIR}"
    "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}"
)
if(DEFINED TEST_GENERATOR_PLATFORM AND
   NOT TEST_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${TEST_GENERATOR_PLATFORM}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Mario embedding configure failed:\n${configure_output}\n${configure_error}")
endif()
