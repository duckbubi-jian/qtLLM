cmake_minimum_required(VERSION 3.31)

project(qtLLM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

add_subdirectory(shared)
add_subdirectory(app)
add_subdirectory(worker)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
