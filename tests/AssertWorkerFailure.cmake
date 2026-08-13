if(NOT DEFINED WORKER OR NOT DEFINED CASE)
    message(FATAL_ERROR "WORKER and CASE are required")
endif()

if(CASE STREQUAL "requires_model")
    set(arguments --prompt hello)
    set(expected_result 2)
    set(expected_error "Missing required --model option")
elseif(CASE STREQUAL "invalid_context_size")
    if(NOT DEFINED MODEL)
        message(FATAL_ERROR "MODEL is required for invalid_context_size")
    endif()
    set(arguments --model "${MODEL}" --prompt hello --context-size invalid)
    set(expected_result 2)
    set(expected_error "Invalid --context-size value")
elseif(CASE STREQUAL "invalid_model")
    if(NOT DEFINED MODEL)
        message(FATAL_ERROR "MODEL is required for invalid_model")
    endif()
    set(arguments --model "${MODEL}" --prompt hello)
    set(expected_result 1)
    set(expected_error "Unable to load GGUF model")
else()
    message(FATAL_ERROR "Unknown worker failure case: ${CASE}")
endif()

execute_process(
    COMMAND "${WORKER}" ${arguments}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error
)

if(NOT result EQUAL expected_result)
    message(FATAL_ERROR "Expected exit code ${expected_result}, got ${result}\nstderr: ${standard_error}")
endif()
if(NOT standard_output STREQUAL "")
    message(FATAL_ERROR "Expected empty stdout, got: ${standard_output}")
endif()
if(NOT standard_error MATCHES "${expected_error}")
    message(FATAL_ERROR "Expected stderr to match '${expected_error}', got: ${standard_error}")
endif()
