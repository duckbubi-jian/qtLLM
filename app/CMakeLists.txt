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

foreach(app_module IN ITEMS infrastructure application ui)
    qtllm_collect_module_files(
        "${CMAKE_CURRENT_SOURCE_DIR}/${app_module}"
        app_module_sources
        app_module_headers
        app_module_forms
    )
    list(REMOVE_ITEM app_sources ${app_module_sources})
    list(REMOVE_ITEM app_headers ${app_module_headers})
    list(REMOVE_ITEM app_forms ${app_module_forms})
endforeach()

add_subdirectory(infrastructure)
add_subdirectory(application)
add_subdirectory(ui)

qt_add_executable(qtLLM
    ${app_sources}
    ${app_headers}
    ${app_forms}
)

qt_add_resources(qtLLM qtllm_resources
    PREFIX "/qtllm"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/qtllm-icon.png"
)

if(WIN32)
    target_sources(qtLLM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/qtllm.rc"
    )
endif()

add_dependencies(qtLLM qtllm-worker qtllm-mcp-filesystem)

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
