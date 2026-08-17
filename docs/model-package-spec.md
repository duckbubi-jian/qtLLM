# qtLLM Model Package Specification

This document defines the model package format currently supported by qtLLM.
The runtime and model weights are distributed separately. Each package
represents one logical model and one quantization or fine-tuning variant.

## Package layout

A package is a directory containing a UTF-8 `manifest.json`, one or more GGUF
files, a `preset.json`, and a non-empty license file:

```text
qwen-4b-q4km/
|- manifest.json
|- model.gguf
|- preset.json
|- LICENSE.txt
`- README.md                 # optional
```

Large GGUF models may be split into files such as
`model-00001-of-00003.gguf`. All shards belong to the same logical model and
must be listed in `modelFiles` in load order. A package must not contain an
undeclared GGUF file.

Packages contain data and metadata only. They must not contain executables,
libraries, scripts, plugins, or other code intended to be run by qtLLM.

Recommended archive names are:

```text
qtLLM-runtime-1.0.0.exe
qtLLM-model-qwen-4b-q4km-1.0.0.zip
```

## manifest.json

Schema version 1 requires the following fields:

```json
{
  "schemaVersion": 1,
  "id": "qwen-4b-q4km",
  "displayName": "Qwen 4B Q4_K_M",
  "packageVersion": "1.0.0",
  "modelRevision": "fixed-conversion-revision",
  "engine": "llama.cpp",
  "format": "gguf",
  "modelFiles": [
    {
      "path": "model.gguf",
      "sizeBytes": 4680000000,
      "sha256": "lowercase-64-character-sha256"
    }
  ],
  "minimumRuntimeVersion": "0.1.0",
  "recommendedRamGb": 16,
  "defaultContextSize": 8192,
  "presetFile": "preset.json",
  "licenseFile": "LICENSE.txt",
  "upstream": {
    "publisher": "Model publisher",
    "modelId": "upstream-model-id",
    "revision": "fixed-upstream-revision",
    "source": "https://official-source.example/model"
  }
}
```

Field requirements:

| Field | Requirement |
| --- | --- |
| `schemaVersion` | Must be `1`. |
| `id` | Unique package identifier using lowercase ASCII letters, numbers, `.`, `_`, or `-`. |
| `packageVersion` | Version of this package, independent of the model revision. |
| `modelRevision` | Fixed revision of the packaged weights or conversion. |
| `engine` / `format` | Must be `llama.cpp` / `gguf`. |
| `modelFiles` | One to 128 GGUF files. Every entry needs a safe relative `path`, exact `sizeBytes`, and lowercase SHA-256. |
| `minimumRuntimeVersion` | Numeric dotted runtime version, for example `0.1.0`. |
| `defaultContextSize` | Must match `preset.json.contextSize`. |
| `presetFile` / `licenseFile` | Safe relative paths to existing files inside the package. The license file must not be empty. |
| `upstream` | Must identify the publisher, upstream model ID, fixed revision, and source URL or reference. |

The runtime checks the manifest, package ID, runtime compatibility, declared
paths, file sizes, GGUF headers, and package contents before loading a model.
It computes SHA-256 for every declared model file during verification.

## preset.json

`preset.json` contains recommended inference parameters only. It must contain:

```json
{
  "contextSize": 8192,
  "maxOutputTokens": 2048,
  "temperature": 0.6,
  "topP": 0.95,
  "topK": 40,
  "repeatPenalty": 1.05
}
```

`maxOutputTokens` must be smaller than `contextSize`. Chat templates are read
from GGUF metadata; this package format does not provide a separate template
override.

## Discovery and verification

When a model-library directory is selected, qtLLM scans its immediate child
directories. A child containing `manifest.json` is treated as a model package.
The normal flow is:

1. Parse and validate `manifest.json`.
1. Resolve all referenced paths as package-relative files.
1. Check the GGUF files, sizes, headers, preset, license, and package contents.
1. Hash the model files and compare their SHA-256 values.
1. Add the package to the model list only after verification succeeds.

The UI currently selects a package directory and loads the first declared GGUF
file. llama.cpp handles additional shards in the same directory. Runtime
settings, conversations, and logs are stored outside the package.

Users may also select a `.gguf` file directly. Direct GGUF mode does not use a
manifest or provenance metadata, uses runtime defaults, and is always shown as
an unverified model.

## Safety requirements

- Reject absolute paths, `..` traversal, and links that resolve outside the package.
- Reject executable content, including EXE, DLL, shared libraries, scripts, and plugins.
- Reject undeclared GGUF files.
- Enforce package file-count and extraction-size limits before installation.
- Never execute package-provided files.
- Treat a verified package as internally consistent with its manifest; this is
  not publisher authentication. Release tooling should provide any required
  signature or distribution trust separately.

Model packages may be shipped beside the runtime in an offline bundle, but the
runtime and each model package remain independently versioned and replaceable.
