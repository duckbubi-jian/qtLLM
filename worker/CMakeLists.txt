qt_add_executable(qtllm-worker
    main.cpp
)

target_link_libraries(qtllm-worker
    PRIVATE
        Qt6::Core
        qtllm_shared
)

install(TARGETS qtllm-worker
    RUNTIME DESTINATION .
)
