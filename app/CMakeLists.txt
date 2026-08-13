qt_add_executable(qtLLM
    main.cpp
    ui/MainWindow.cpp
    ui/MainWindow.hpp
)

target_link_libraries(qtLLM
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        qtllm_shared
)

set_target_properties(qtLLM PROPERTIES
    WIN32_EXECUTABLE TRUE
)

install(TARGETS qtLLM
    RUNTIME DESTINATION .
)
