qt_add_executable(qtllm-mcp-filesystem
    src/main.cpp
)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(qtllm-mcp-filesystem PRIVATE nlohmann_json::nlohmann_json)
if(MSVC)
    set_property(TARGET qtllm-mcp-filesystem PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

set(qtllm_filesystem_package_relative_dir
    "mcp/builtin/qtllm.filesystem/${PROJECT_VERSION}"
)
set(qtllm_filesystem_package_build_dir
    "$<TARGET_FILE_DIR:qtllm-mcp-filesystem>/${qtllm_filesystem_package_relative_dir}"
)
add_custom_command(TARGET qtllm-mcp-filesystem POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${qtllm_filesystem_package_build_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:qtllm-mcp-filesystem>"
        "${qtllm_filesystem_package_build_dir}/$<TARGET_FILE_NAME:qtllm-mcp-filesystem>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/mcp-package.json"
        "${qtllm_filesystem_package_build_dir}/mcp-package.json"
    VERBATIM
)

install(TARGETS qtllm-mcp-filesystem
    RUNTIME DESTINATION "${qtllm_filesystem_package_relative_dir}"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/mcp-package.json"
    DESTINATION "${qtllm_filesystem_package_relative_dir}"
)
