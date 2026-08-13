add_library(qtllm_shared INTERFACE)

target_include_directories(qtllm_shared
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(qtllm_shared
    INTERFACE
        Qt6::Core
)
