# Packaging

qtLLM uses separate runtime and model artifacts:

```text
qtLLM-runtime-<runtime-version>.exe
qtLLM-model-<model-id>-<package-version>.zip
```

One model package represents exactly one logical model. A split GGUF model may
contain multiple weight files, but model variants and quantizations must be
published as separate packages.

The runtime installer must not contain model weights. An offline edition may
place a runtime installer and one model package in the same delivery archive,
while retaining separate installation and update lifecycles.

Publish CPU and NVIDIA GPU runtimes as separate artifacts. The GPU artifact
contains qtLLM's `ggml-cuda.dll`, but does not redistribute NVIDIA CUDA DLLs.
It requires a compatible NVIDIA driver and a user-installed CUDA Toolkit 11.8;
`cudart64_110.dll`, `cublas64_11.dll`, and `cublasLt64_11.dll` must be
resolvable through `PATH`, while the driver supplies `nvcuda.dll`. The CPU
artifact does not require CUDA. The installer and dependency collection steps
must not copy these NVIDIA DLLs into the application directory.

See `examples/model-package` for the package layout and
`docs/model-package-spec.md` for validation rules.

## Runtime installation

Build first, then create a clean portable runtime with CMake's install step:

```powershell
cmake --install cmake-build-release-cuda `
  --prefix dist/qtLLM-runtime-0.1.0-cuda-win-x64
```

Always use a new or empty versioned prefix. CMake updates installed files but
does not remove stale files left by an older runtime layout.

For the CPU build, use its build directory and a `cpu-win-x64` destination.
The install step collects the application, worker, llama/ggml libraries, Qt
libraries, Qt platform plugins, compiler runtime and third-party license
notices. It does not install models, tests, build files, `qtLLM.ini` or NVIDIA
CUDA libraries. The final install script verifies required and prohibited files
and fails when the portable runtime is incomplete.

The resulting directory can be zipped as a portable runtime after validation
on a clean Windows system. It is a staging directory, not a signed installer.

`models/deepseek-r1-distill-qwen-7b-q4km` and
`models/deepseek-r1-distill-qwen-14b-q4km` contain the pinned metadata used for
the CUDA development baselines. They contain no model weights and must each be
combined with their hash-verified `model.gguf` only outside the source
repository.

When a package directory is selected, the application validates its manifest,
paths, file types, sizes, GGUF headers, runtime requirement, license and preset
before hashing model weights in the background. A matching file fingerprint is
cached in `qtLLM.ini`; changing the path, size, modification time or expected
hash forces verification again. This consistency check is not a substitute for
package signing and publisher authentication in a commercial release.
