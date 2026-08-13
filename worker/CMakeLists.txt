find_package(llama CONFIG REQUIRED)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    worker_sources
    worker_headers
    worker_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    worker_include_directories
)

qt_add_executable(qtllm-worker
    ${worker_sources}
    ${worker_headers}
    ${worker_forms}
)

target_include_directories(qtllm-worker
    PRIVATE
        ${worker_include_directories}
)

target_link_libraries(qtllm-worker
    PRIVATE
        Qt6::Core
        llama
        qtllm_shared
)

install(TARGETS qtllm-worker
    RUNTIME DESTINATION .
)
