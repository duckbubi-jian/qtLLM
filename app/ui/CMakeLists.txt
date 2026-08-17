qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ui_sources
    ui_headers
    ui_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ui_include_directories
)

add_library(qtllm_app_ui STATIC
    ${ui_sources}
    ${ui_headers}
    ${ui_forms}
)
set_target_properties(qtllm_app_ui PROPERTIES
    AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/forms"
)
target_include_directories(qtllm_app_ui
    PUBLIC
        ${ui_include_directories}
)
target_link_libraries(qtllm_app_ui
    PUBLIC
        Qt6::Concurrent
        Qt6::Widgets
        qtllm_app_application
        qtllm_app_infrastructure
)
