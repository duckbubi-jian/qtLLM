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

See `examples/model-package` for the package layout and
`docs/model-package-spec.md` for validation rules.
