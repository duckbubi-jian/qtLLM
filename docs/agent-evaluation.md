# Agent Evaluation Runner

`qtllm-agent-eval` runs the production `WorkerClient`, `AgentController`, and
MCP Host without the Qt Widgets UI. It is the M5 harness for comparing local
models and inference configurations. The runner does not provide tools or
modify an MCP Server; it uses the same stdio Server configuration and tool
validation path as qtLLM.

The executable is built when `BUILD_TESTING` is enabled, but it is not a CTest
test because it requires a local model and explicit MCP Server processes.

## Run A Suite

Create an MCP configuration JSON array using the same object format as
qtLLM's MCP settings. Relative `program` and `workingDirectory` paths are
resolved from the configuration file's directory:

```json
[
  {
    "serverId": "fake",
    "program": "D:/src/qtLLM/cmake-build-release-cpu/qtllm-fake-mcp-server.exe",
    "arguments": [],
    "environment": {},
    "enabled": true,
    "useInstructions": true
  }
]
```

Run the checked-in smoke suite from the build directory:

```powershell
.\qtllm-agent-eval.exe `
  --model D:\models\model.gguf `
  --mcp-config D:\eval\fake-mcp.json `
  --suite D:\src\qtLLM\tests\agent_evaluation\fixtures\smoke-suite.json `
  --workspace D:\eval\workspace `
  --repeat 3 `
  --output D:\eval\smoke-report.json
```

Use `--gpu-layers 0` for a CPU run and the default `--gpu-layers -1` for
automatic placement. `--worker`, `--startup-timeout-ms`, and
`--run-timeout-ms` override worker discovery and time budgets when necessary.
`--workspace` defaults to the current directory and is included in the same
Agent context field used by the UI.

## Suite Format

Each scenario supplies one isolated prompt. `approval` controls the Host policy
for that run:

- `allow`: allow every selected tool without an approval round trip.
- `deny`: deny the selected tool before execution.
- `approve`: require approval, then approve it automatically.
- `reject`: require approval, then reject it automatically.
- `policy`: use the normal `ToolPolicy`; approval requests are approved.

Assertions operate on the terminal `AgentRunMetrics` JSON. `equals` compares
JSON values exactly; `maximum` and `minimum` compare numeric values:

```json
{
  "schemaVersion": 1,
  "scenarios": [
    {
      "name": "read_once",
      "prompt": "Read the requested resource and report its contents.",
      "approval": "policy",
      "expect": {
        "equals": {
          "state": "completed",
          "firstToolChoice": "filesystem.read_text_file"
        },
        "maximum": {
          "validationRepairs": 1,
          "duplicateMutationActions": 0
        },
        "minimum": {
          "successfulToolResults": 1
        }
      }
    }
  ]
}
```

The report records model identity, preset, model-load placement and duration,
repeat number, terminal metrics, inference metrics for every decision, MCP Host
diagnostics, elapsed time, and the complete Agent event trace. Exit code `0`
means every assertion passed, `1` means at least one scenario was rejected,
and `2` means configuration, startup, or report writing failed.

## M5 Matrix

Run every suite separately for the lightweight and larger model, then repeat
the matrix for CPU and CUDA placement. Keep generated reports outside the
repository because they contain local paths, model names, prompts, tool
arguments, and MCP results. Compare failures by `failureCategory` before tuning
catalog size or repair budgets; do not add scenario-specific prompt rules.

## Remaining M5 Work

The runner, terminal metrics, report format, and two-scenario smoke suite are
implemented. Real-model acceptance and tuning remain open. Continue in this
order:

1. Add provider-neutral Fake MCP suites for read-only multi-file work,
   multi-step mutation with read-back, targeted argument repair, structured
   polling, approval and rejection, cancellation, timeout, late response, and
   context compaction. Keep each scenario isolated and assert final state as
   well as Agent metrics.
1. Extend reporting where measurements are not yet explicit: tool round-trip
   time, observed polling intervals and terminal wait time, approval or
   cancellation latency, and time to first token. Preserve the existing report
   schema versioning when fields are added.
1. Expose the same automatic, single-device, and custom Layer Split placement
   choices used by qtLLM so the runner can exercise CPU, CUDA automatic
   placement, and custom multi-GPU placement without a separate code path.
1. Run every suite at least three times with a lightweight model and a larger
   model. Record the model file, quantization, context size, placement, MCP
   Server version, and runner revision with each report.
1. Classify every rejected run as model decision, Host validation or protocol,
   authorization, transport, Server behavior, or tool business logic before
   changing code. Tune only general catalog, repair, polling, and compaction
   budgets; never add Server names, tool names, domain fields, or scenario text
   to production decision logic.

M5 is complete only when both model classes pass the read-only, mutation,
polling, denial, cancellation, recovery, and compaction suites without
duplicate mutation, post-terminal polling, leaked redacted data, or
provider-specific Host behavior. P3 Server-initiated Sampling and Elicitation
start only after this gate is met.
