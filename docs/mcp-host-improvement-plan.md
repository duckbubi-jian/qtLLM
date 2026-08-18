# qtLLM MCP Host Roadmap

## Document Rules

- This document lists only unfinished work. Remove items when they are complete.
- Refer to
  [`mcp-protocol-compatibility.md`](mcp-protocol-compatibility.md) for the
  currently implemented capabilities.
- Refer to
  [`mcp-agent-runtime-plan.md`](mcp-agent-runtime-plan.md) for the controlled
  Agent loop, small-model tool contracts, runtime state, and delivery order.
  That plan changes qtLLM only; external MCP Server changes are out of scope.
- The product target is a general-purpose, offline-first stdio MCP Host, not a
  coding agent. Reusing a coding-agent-style control loop does not add built-in
  coding tools or coding-specific workflows.
- HTTP, authentication, web search, and memory are outside the current roadmap.

## Implementation Order

1. P2: Real-model Agent and MCP acceptance testing.
1. P3: Server-initiated requests.
1. P4: Release-grade offline conformance.

The current stdio MCP Host should not be considered validated in real model
workflows until P2 is complete. P3 introduces new authorization boundaries and
requires a separate review, but it must not block P2.

## P2: Real-Model Agent and MCP Acceptance Testing

### Model Matrix

- Use a lightweight model to validate the low-latency path, such as 9B Q8.
- Use a larger model to validate complex tool decisions, such as 27B Q4_K_M.
- Validate automatic multi-GPU placement and custom Layer Split with the CUDA
  package. Validate the no-CUDA dependency path with the CPU package.

### Scenarios

- Discover filesystem MCP tools, read multiple files, and produce a combined
  answer.
- Create, edit, move, and read files in sequence, then verify that the final
  answer matches the filesystem state.
- Start a long-running task and verify that the Agent continues polling while
  the status tool returns `running`, `pending`, or `queued`, without the
  duplicate-call guard incorrectly treating the run as stalled.
- Verify that status polling does not busy-wait and that Stop works while
  waiting for the next poll, generating a decision, or executing a tool.
- Validate Allow once, Always allow, Deny, and Stop independently.
- Validate tool timeouts, Server exits, invalid arguments, oversized results,
  and late responses.
- Verify that the model can repair an invalid Agent Action without bypassing
  local validation.
- Verify that context compaction during a long task preserves completed steps
  and the required tool evidence.

### Measurements

- Record the model, quantization, context length, CPU/GPU placement, and MCP
  Server version.
- Record first-attempt tool selection success, multi-step task completion,
  average repair count, and cancellation latency.
- For long-running tasks, record the status query count, polling interval,
  terminal-state classification, and total wait time.
- Record model loading time, time to first token, tool round-trip time, and
  total time to the final answer.

### Acceptance Criteria

- Both models complete read-only, multi-step mutation, denial, and cancellation
  scenarios.
- Both models continue polling structured non-terminal results until a terminal
  state and do not call the status tool again after the terminal result.
- Every failure is classified as a model decision, Host protocol,
  authorization, Server, or tool business-logic failure.
- Tool results, diagnostics, and final answers do not expose redacted fields.

## P3: Server-Initiated Requests

### Sampling

- Implement parsing for `sampling/createMessage`, deny it by default, and
  require explicit per-Server enablement.
- Give Sampling an independent context, token budget, time budget, and
  concurrency budget.
- Sampling may invoke only model generation. It must not enter the Agent tool
  loop or create recursive tool calls.
- Limit recursion depth and per-Server concurrency. Support cancellation and
  ignore late results.
- Show the request source, summary, budget, and approval result in the UI.

### Elicitation

- Implement elicitation for the currently negotiated protocol version and keep
  it disabled by default.
- Support only a limited set of form fields validated against a local schema.
- Passwords, tokens, and other sensitive input must not enter normal logs or MCP
  configuration files.
- A Server must not inherit another Server's authorization, roots, or
  elicitation results.

### Acceptance Criteria

- Return a standard JSON-RPC error when a capability is not enabled.
- Once enabled, every sensitive request is traceable, cancellable, and subject
  to deterministic budgets.
- Add regression tests for malicious recursion, prompt injection, oversized
  requests, and Server exit after approval.

## P4: Release-Grade Offline Conformance

### Work Items

- Build a test matrix covering multiple Servers, protocol versions, and failure
  combinations.
- Run offline stdio acceptance tests from installed CPU and CUDA packages.
- Verify that upgrades preserve Server configuration, tool allowlists,
  authorized roots, and per-Server settings.
- Verify that an application, worker, or MCP Server crash does not terminate
  unrelated processes and that child processes are cleaned up.
- Complete third-party Server configuration migration, privacy boundaries, and
  release troubleshooting documentation.
- Continue excluding model weights, NVIDIA CUDA DLLs, user configuration, and
  test credentials from release packages.

### Acceptance Criteria

- Installed CPU and CUDA packages pass the same offline MCP conformance suite.
- Default tests do not load a real model, access the public network, or depend
  on absolute paths from the development machine.
- A Server crash, timeout, malformed message, or notification storm does not
  affect other Servers or the Qt UI.

## Explicitly Deferred

- Streamable HTTP, SSE, HTTP authentication, OAuth, and Host-level network
  credential management.
- Web-search MCP Servers and any Host-provided network capability.
- SQLite conversation persistence, long-term memory, and conversation recovery
  after application restart.
- Built-in `read`, `edit`, or `bash` tools in the Qt main process.
- Multi-Agent workflows, subagents, skills, and coding-agent-specific
  workflows.
- Other inference backends and non-GGUF model formats.

Deferred items may return to the roadmap only after an explicit product-scope
change.
