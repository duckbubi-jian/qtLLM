add_test(NAME worker_help COMMAND qtllm-worker --help)
add_test(NAME worker_version COMMAND qtllm-worker --version)
add_test(NAME worker_requires_model
    COMMAND ${CMAKE_COMMAND}
        -DWORKER=$<TARGET_FILE:qtllm-worker>
        -DCASE=requires_model
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AssertWorkerFailure.cmake
)

add_test(NAME worker_rejects_invalid_number
    COMMAND ${CMAKE_COMMAND}
        -DWORKER=$<TARGET_FILE:qtllm-worker>
        -DCASE=invalid_context_size
        -DMODEL=${CMAKE_SOURCE_DIR}/CMakeLists.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AssertWorkerFailure.cmake
)

add_test(NAME worker_rejects_invalid_model
    COMMAND ${CMAKE_COMMAND}
        -DWORKER=$<TARGET_FILE:qtllm-worker>
        -DCASE=invalid_model
        -DMODEL=${CMAKE_SOURCE_DIR}/CMakeLists.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AssertWorkerFailure.cmake
)
