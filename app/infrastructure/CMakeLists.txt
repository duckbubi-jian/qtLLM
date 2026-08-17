qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    infrastructure_sources
    infrastructure_headers
    infrastructure_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    infrastructure_include_directories
)

add_library(qtllm_app_infrastructure STATIC
    ${infrastructure_sources}
    ${infrastructure_headers}
    ${infrastructure_forms}
)
target_include_directories(qtllm_app_infrastructure
    PUBLIC
        ${infrastructure_include_directories}
)
target_link_libraries(qtllm_app_infrastructure
    PUBLIC
        Qt6::Core
        qtllm_mcp_host
        qtllm_shared
)
