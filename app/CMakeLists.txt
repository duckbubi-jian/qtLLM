qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    app_sources
    app_headers
    app_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    app_include_directories
)
qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/infrastructure"
    infrastructure_sources
    infrastructure_headers
    infrastructure_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/infrastructure"
    infrastructure_include_directories
)

list(REMOVE_ITEM app_sources ${infrastructure_sources})
list(REMOVE_ITEM app_headers ${infrastructure_headers})
list(REMOVE_ITEM app_forms ${infrastructure_forms})

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
        qtllm_shared
)

qt_add_executable(qtLLM
    ${app_sources}
    ${app_headers}
    ${app_forms}
)

add_dependencies(qtLLM qtllm-worker)

target_include_directories(qtLLM
    PRIVATE
        ${app_include_directories}
)

target_link_libraries(qtLLM
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        qtllm_app_infrastructure
        qtllm_shared
)

set_target_properties(qtLLM PROPERTIES
    WIN32_EXECUTABLE TRUE
)

install(TARGETS qtLLM
    RUNTIME DESTINATION .
)
