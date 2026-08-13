vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ggml-org/llama.cpp
    REF b${VERSION}
    SHA512 ef5e21b61ca2961004fc57ad9d4a07191458df4f1749e71a9dc96d653676a6d68d43b7b8c74ebb235f6dffe5c064330cb1124887bc5c119876d7292543321945
    HEAD_REF master
)

# llama.cpp and ggml evolve in lockstep. Building the bundled ggml prevents
# ABI/API mismatches with an independently versioned system ggml package.
vcpkg_check_features(OUT_FEATURE_OPTIONS feature_options
    FEATURES
        cuda GGML_CUDA
)

if("cuda" IN_LIST FEATURES)
    vcpkg_find_cuda(OUT_CUDA_TOOLKIT_ROOT cuda_toolkit_root)
    list(APPEND feature_options
        "-DCMAKE_CUDA_COMPILER=${NVCC}"
        "-DCMAKE_CUDA_ARCHITECTURES=86"
        "-DCUDAToolkit_ROOT=${cuda_toolkit_root}"
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${feature_options}
        -DGGML_BACKEND_DL=OFF
        -DGGML_CCACHE=OFF
        -DGGML_NATIVE=OFF
        -DLLAMA_ALL_WARNINGS=OFF
        -DLLAMA_BUILD_COMMON=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_BUILD_SERVER=OFF
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_TOOLS=OFF
        -DLLAMA_CURL=OFF
        -DLLAMA_USE_SYSTEM_GGML=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_Git=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME ggml
    CONFIG_PATH "lib/cmake/ggml"
    DO_NOT_DELETE_PARENT_CONFIG_PATH
)
vcpkg_cmake_config_fixup(
    PACKAGE_NAME llama
    CONFIG_PATH "lib/cmake/llama"
)
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

if(EXISTS "${CURRENT_PACKAGES_DIR}/bin/convert_hf_to_gguf.py")
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
    file(RENAME
        "${CURRENT_PACKAGES_DIR}/bin/convert_hf_to_gguf.py"
        "${CURRENT_PACKAGES_DIR}/tools/${PORT}/convert-hf-to-gguf.py"
    )
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
