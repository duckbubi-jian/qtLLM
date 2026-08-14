cmake_minimum_required(VERSION 3.31)

# Keep the application and all vcpkg dependencies on the VS 2019/v142 ABI.
# These variables must be set before project() loads the vcpkg toolchain.
if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-vs2019" CACHE STRING
        "qtLLM Windows vcpkg triplet" FORCE)
    set(VCPKG_OVERLAY_TRIPLETS
        "${CMAKE_CURRENT_LIST_DIR}/vcpkg-overlay-triplets" CACHE STRING
        "qtLLM vcpkg overlay triplets" FORCE)
endif()

option(QTLLM_ENABLE_CUDA "Build llama.cpp with the NVIDIA CUDA backend" OFF)
if(QTLLM_ENABLE_CUDA)
    list(APPEND VCPKG_MANIFEST_FEATURES cuda)
endif()

if(QTLLM_ENABLE_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 86 CACHE STRING
            "CUDA architectures for the target NVIDIA GPU")
    endif()
    if(CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        message(FATAL_ERROR "QTLLM_ENABLE_CUDA requires CMAKE_CUDA_ARCHITECTURES")
    endif()
endif()

project(qtLLM VERSION 0.1.0 LANGUAGES CXX)

if(MSVC AND (MSVC_VERSION LESS 1920 OR MSVC_VERSION GREATER_EQUAL 1930))
    message(FATAL_ERROR
        "qtLLM requires the Visual Studio 2019/v142 compiler. "
        "Select a Visual Studio 2019 toolchain and reset the CMake cache.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(Qt6 6.5 REQUIRED COMPONENTS Concurrent Core Gui Widgets)
qt_standard_project_setup()

set(QTLLM_MODULE_DIRECTORIES
    app
    shared
    worker
    tests
)

set(QTLLM_ALL_SOURCES)
set(QTLLM_ALL_HEADERS)
set(QTLLM_ALL_FORMS)
foreach(module IN LISTS QTLLM_MODULE_DIRECTORIES)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp"
    )
    file(GLOB_RECURSE module_headers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.hpp"
    )
    file(GLOB_RECURSE module_forms CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.ui"
    )
    list(APPEND QTLLM_ALL_SOURCES ${module_sources})
    list(APPEND QTLLM_ALL_HEADERS ${module_headers})
    list(APPEND QTLLM_ALL_FORMS ${module_forms})
endforeach()

function(qtllm_collect_module_files module_directory sources_var headers_var forms_var)
    file(TO_CMAKE_PATH "${module_directory}" module_prefix)
    set(module_sources)
    set(module_headers)
    set(module_forms)

    foreach(source IN LISTS QTLLM_ALL_SOURCES)
        file(TO_CMAKE_PATH "${source}" normalized_source)
        if(normalized_source MATCHES "^${module_prefix}/")
            list(APPEND module_sources "${source}")
        endif()
    endforeach()
    foreach(header IN LISTS QTLLM_ALL_HEADERS)
        file(TO_CMAKE_PATH "${header}" normalized_header)
        if(normalized_header MATCHES "^${module_prefix}/")
            list(APPEND module_headers "${header}")
        endif()
    endforeach()
    foreach(form IN LISTS QTLLM_ALL_FORMS)
        file(TO_CMAKE_PATH "${form}" normalized_form)
        if(normalized_form MATCHES "^${module_prefix}/")
            list(APPEND module_forms "${form}")
        endif()
    endforeach()

    set(${sources_var} ${module_sources} PARENT_SCOPE)
    set(${headers_var} ${module_headers} PARENT_SCOPE)
    set(${forms_var} ${module_forms} PARENT_SCOPE)
endfunction()

function(qtllm_collect_include_directories module_directory output_var)
    file(TO_CMAKE_PATH "${module_directory}" module_prefix)
    set(include_directories)
    foreach(header IN LISTS QTLLM_ALL_HEADERS)
        file(TO_CMAKE_PATH "${header}" normalized_header)
        if(NOT normalized_header MATCHES "^${module_prefix}/")
            continue()
        endif()
        if(normalized_header MATCHES "^(.*/include)(/.*)?$")
            list(APPEND include_directories "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES include_directories)
    set(${output_var} ${include_directories} PARENT_SCOPE)
endfunction()

add_subdirectory(shared)
add_subdirectory(app)
add_subdirectory(worker)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
