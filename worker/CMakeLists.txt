find_package(llama CONFIG REQUIRED)

qt_add_executable(qtllm-worker
    LlamaEngine.cpp
    LlamaEngine.hpp
    WorkerOptions.cpp
    WorkerOptions.hpp
    main.cpp
)

target_link_libraries(qtllm-worker
    PRIVATE
        Qt6::Core
        llama
        qtllm_shared
)

install(TARGETS qtllm-worker
    RUNTIME DESTINATION .
)
