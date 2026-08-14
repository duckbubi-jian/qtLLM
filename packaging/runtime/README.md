# qtLLM Runtime

This directory is a portable qtLLM runtime. It contains the desktop
application, worker process, inference backend, Qt libraries and required Qt
plugins. Model weights are distributed separately.

## Run

1. Keep all installed files and directories together.
1. Start `qtLLM.exe`.
1. Select a qtLLM model package directory or a standalone GGUF file.

`qtLLM.ini` is created beside the executable after the application stores its
first setting. It is user-specific and is intentionally not included in the
runtime package.

## NVIDIA GPU build

The GPU runtime contains qtLLM's `ggml-cuda.dll`, but intentionally does not
redistribute NVIDIA CUDA libraries. The target computer must provide:

- a compatible NVIDIA driver;
- CUDA Toolkit 11.8;
- CUDA's `bin` directory on `PATH` so `cudart64_110.dll`, `cublas64_11.dll`
  and `cublasLt64_11.dll` can be resolved;
- `nvcuda.dll`, supplied by the NVIDIA driver.

The CPU runtime does not require CUDA.

Third-party license notices are available in `licenses/`.
