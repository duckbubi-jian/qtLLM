qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    shared_sources
    shared_headers
    shared_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    shared_include_directories
)

add_library(qtllm_shared STATIC
    ${shared_sources}
    ${shared_headers}
    ${shared_forms}
)

target_include_directories(qtllm_shared
    PUBLIC
        ${shared_include_directories}
)

target_link_libraries(qtllm_shared
    PUBLIC
        Qt6::Core
)
