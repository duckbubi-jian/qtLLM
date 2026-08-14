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

`models/deepseek-r1-distill-qwen-7b-q4km` and
`models/deepseek-r1-distill-qwen-14b-q4km` contain the pinned metadata used for
the CUDA development baselines. They contain no model weights and must each be
combined with their hash-verified `model.gguf` only outside the source
repository.
