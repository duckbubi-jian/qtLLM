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
qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    ui_sources
    ui_headers
    ui_forms
)
qtllm_collect_include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    ui_include_directories
)

list(REMOVE_ITEM app_sources ${infrastructure_sources})
list(REMOVE_ITEM app_headers ${infrastructure_headers})
list(REMOVE_ITEM app_forms ${infrastructure_forms})
list(REMOVE_ITEM app_sources ${ui_sources})
list(REMOVE_ITEM app_headers ${ui_headers})
list(REMOVE_ITEM app_forms ${ui_forms})

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

add_library(qtllm_app_ui STATIC
    ${ui_sources}
    ${ui_headers}
    ${ui_forms}
)
target_include_directories(qtllm_app_ui
    PUBLIC
        ${ui_include_directories}
)
target_link_libraries(qtllm_app_ui
    PUBLIC
        Qt6::Concurrent
        Qt6::Widgets
        qtllm_app_infrastructure
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
        qtllm_app_ui
        qtllm_shared
)

set_target_properties(qtLLM PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(WIN32 AND TARGET Qt6::QWindowsIntegrationPlugin)
    add_custom_command(TARGET qtLLM POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:qtLLM>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE_DIR:qtLLM>/platforms/$<TARGET_FILE_NAME:Qt6::QWindowsIntegrationPlugin>"
        VERBATIM
    )
endif()
