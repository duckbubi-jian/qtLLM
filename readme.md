# qtLLM

qtLLM is a Windows desktop shell for running local GGUF language models. The
application runtime and model weights are distributed separately.

## Distribution model

- `qtLLM-runtime-<version>.exe` contains the Qt application, `qtllm-worker`,
  the inference engine, and required runtime libraries. It contains no model
  weights.
- `qtLLM-model-<model-id>-<package-version>.zip` contains exactly one logical
  model, its preset, hashes, documentation, and license.
- An offline bundle may ship both artifacts together, but installs and updates
  them as separate components.

The application can scan a model-library directory, import a model package, or
open a compatible `.gguf` file directly. Direct GGUF files are shown as
unverified models because they have no package manifest or recorded provenance.

See [the development plan](docs/qt-local-llm-plan.md) and
[the model package specification](docs/model-package-spec.md).

## Build

Configure CMake with the vcpkg toolchain:

```text
-DCMAKE_TOOLCHAIN_FILE=<YOUR_VCPKG_ROOT>/scripts/buildsystems/vcpkg.cmake
```
