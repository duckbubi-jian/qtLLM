qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    mcp_host_sources
    mcp_host_headers
    mcp_host_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    mcp_host_include_directories
)

add_library(qtllm_mcp_host STATIC
    ${mcp_host_sources}
    ${mcp_host_headers}
    ${mcp_host_forms}
)
target_include_directories(qtllm_mcp_host
    PUBLIC
        ${mcp_host_include_directories}
)
target_link_libraries(qtllm_mcp_host
    PUBLIC
        Qt6::Core
        qtllm_shared
)
