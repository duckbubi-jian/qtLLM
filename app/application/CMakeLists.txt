qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    application_sources
    application_headers
    application_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    application_include_directories
)

add_library(qtllm_app_application STATIC
    ${application_sources}
    ${application_headers}
    ${application_forms}
)
target_include_directories(qtllm_app_application
    PUBLIC
        ${application_include_directories}
)
target_link_libraries(qtllm_app_application
    PUBLIC
        Qt6::Core
        qtllm_app_infrastructure
        qtllm_shared
)
