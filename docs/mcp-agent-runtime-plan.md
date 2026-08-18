# qtLLM Agent Runtime Design and Delivery Plan

## Purpose

qtLLM needs a coding-agent-style control loop to complete stateful tool
workflows reliably. It does not need to become a coding agent. A coding agent
operates on files, patches, commands, and test results; qtLLM operates on
externally supplied tools, domain objects, resource identifiers, and
long-running jobs. The execution domain differs, but the control problem is the
same:

1. Understand the requested outcome.
1. Select one useful action.
1. Validate and authorize it locally.
1. Execute it and observe the structured result.
1. Update task state and evidence.
1. Continue, repair, poll, verify, or finish.

This plan changes qtLLM only. Existing MCP Servers, tool schemas, protocol
behavior, and domain workflows are fixed external inputs and are not delivery
items. This document defines the internal runtime and the order in which to
strengthen it for small local models. MCP transport and protocol conformance
remain documented in
[`mcp-protocol-compatibility.md`](mcp-protocol-compatibility.md), while
unfinished Host-level protocol work remains in
[`mcp-host-improvement-plan.md`](mcp-host-improvement-plan.md).

## Product Boundary

The change boundary is the qtLLM repository. No MCP Server source, Server tool
definition, Server configuration, domain workflow, or protocol extension is
required by this plan. Adopting a coding-agent-style loop does not add built-in
shell, patch, repository, or compiler tools. It also does not add memory,
network access, multi-Agent orchestration, or Shondy-specific workflows to
qtLLM.

The division of responsibility is:

| Component | Responsibility |
| --- | --- |
| Model | Interpret intent, choose the next supplied tool, fill its arguments, and write the final answer. |
| Agent runtime | Own the loop, task state, retries, polling, evidence, verification, and termination. |
| qtLLM tool adapter | Discover existing tools, validate schemas, enforce policy, execute calls, and preserve results. |
| External tool provider | Remain unchanged; its current schema, descriptions, results, and limitations are treated as input. |

External descriptions, annotations, examples, instructions, and results remain
untrusted guidance. qtLLM may summarize existing metadata but does not require
providers to add any. Guidance cannot override local schema validation,
authorization, roots, or tool policy.

## Comparison With a Coding Agent

The useful part of a coding agent such as Pi is the iterative runtime, not its
specific file tools.

| Coding agent | MCP Agent runtime |
| --- | --- |
| Read, edit, build, test | Discover, call, read back, poll |
| File paths and diffs | UUIDs, parent-child selectors, and object revisions |
| Compiler and test failures | Validation, protocol, Server, and domain failures |
| Command process state | MCP request and long-running job state |
| Verify with tests or a diff | Verify with structured results and object read-back |
| Filesystem sandbox | Tool policy, authorized roots, and Server capabilities |

Both need an explicit action protocol, bounded repair, cancellation, context
compaction, an evidence ledger, duplicate-call protection, and deterministic
completion rules.

## Current qtLLM Baseline

The current implementation already provides most of the outer loop:

- `AgentController` accepts only structured `task_plan`, `call_tool`,
  `review_completion`, or `final` actions.
- The Host validates tool names and arguments before execution and applies
  `ToolPolicy` independently from the model.
- Tool calls support approval, cancellation, timeout handling, and ignored late
  results.
- Structured `running`, `pending`, and `queued` results enter a delayed polling
  path instead of being treated as completion.
- Canonical call signatures reject repeated successful calls and unchanged
  retries after failures, while permitting legitimate status polling.
- Tool evidence, task steps, context compaction, and completion review prevent
  a successful intermediate call from ending a multi-step request early.
- A malformed completion review can fall back to an already prepared answer
  only when successful terminal evidence covers every tool-required step.
- The prompt contains a separately budgeted compact tool index that preserves
  qualified names, required nesting, local references, discriminators, exact
  enum and const values, and union branches when full schemas are omitted.
- Local argument validation supports local `$ref`, `const`, type unions,
  `allOf`, `anyOf`, and `oneOf`; rejected calls receive a focused contract and
  use a retry counter independent from malformed Agent actions.
- Tool results carry a normalized outcome and side-effect state. A dispatched
  mutation with an uncertain remote result stops instead of being repeated,
  while a read-only transport failure receives at most one exact retry.

The first four implementation milestones close the largest gaps between tool
discovery, execution, and verified state. Remaining work is to validate the
loop against real local models.

## Failure Pattern To Fix

A representative geometry-inlet workflow exposed the following loop:

1. The model found that no inlet existed.
1. It queried a nested `inletArray` model without the required parent selector.
1. It selected `create_inlet` but placed `name` at the wrong level.
1. It used `geometryFile` where the tool schema required `geometry_file` and
   supplied an unnecessary `null` value.
1. It tried to list a nested model that was not independently listable.
1. It repeated discovery calls instead of repairing the failed contract.

This is not only a model-capability failure. The Host exposed a valid but hard
to execute contract, then gave the model too little failed-tool-specific
information to recover. The runtime should make the valid next action easier
than blind schema exploration.

## Target Loop

```text
user request
    |
    v
plan or choose next unfinished outcome
    |
    v
select tool from compact catalog
    |
    v
load exact contract -> validate -> authorize
    |                    |
    |                    +-> targeted contract repair
    v
execute MCP call
    |
    v
classify result
    |
    +-> in progress -> wait -> poll
    +-> validation or domain error -> repair or choose another tool
    +-> uncertain side effect -> inspect state, do not replay blindly
    +-> success -> update resources and evidence
                              |
                              v
                   verify requested effect when required
                              |
                              v
                  next step or completion review
```

The model chooses the next action. The controller owns every transition and
must never rely on the model to remember whether a call was executed, whether a
job is terminal, or whether evidence satisfies a task step.

## Runtime Invariants

1. One model decision can request at most one external operation.
1. No MCP call is sent before exact-name, schema, and policy validation pass.
1. A failed local validation is not recorded as an executed tool operation.
1. A successful mutation proves only that call succeeded; it does not prove
   the requested end state without appropriate verification.
1. A non-terminal result cannot satisfy a completion step.
1. A side-effecting call with an uncertain outcome is not automatically
   replayed.
1. An identical successful call is rejected unless it is an explicitly
   recognized status poll.
1. Every identifier reused by the model is traceable to user input or recorded
   tool evidence.
1. Context compaction preserves unfinished steps, current jobs, resource
   identities, and evidence required for completion.
1. The run ends only with verified completion, a concrete blocker, user
   cancellation, or an unrecoverable bounded failure.

## Tool Contract Delivery

### Tier 0: Compact Index

Every allowed tool must remain discoverable even when full schemas do not fit
the prompt budget. The always-present index should contain:

- qualified tool name;
- one-line purpose;
- required top-level arguments;
- top-level argument types;
- discriminator names and exact enum values;
- a concise call shape for common branches;
- whether the tool is read-only, side-effecting, or long-running when locally
  known;
- explicit omitted-full-schema status.

The index must list omitted tool names, not only an omitted count. It is derived
entirely from the `ToolDefinition` objects qtLLM already receives. Facts derived
from JSON Schema are authoritative for validation. Semantic hints derived from
descriptions or annotations remain untrusted guidance.

### Tier 1: Exact Selected Contract

Before correcting an invalid call, the prompt must include the selected tool's
complete input schema and a bounded executable summary. For complex `oneOf` or
discriminated-union schemas, the summary should show:

- the discriminator path;
- every exact discriminator value;
- required fields for each branch;
- the correct top-level nesting;
- one minimal structural example per branch when qtLLM can derive it without
  inventing semantic values;
- guidance to omit optional fields instead of sending `null` unless the schema
  explicitly permits null.

Simple tools can still be called directly from the compact index. This avoids
adding a new public Agent action type or forcing an extra inference for every
call.

### Tier 2: Validation-Aware Repair

A local validation failure should produce a structured repair message with:

- failed qualified tool name;
- exact JSON path and failed schema keyword;
- concise validator message;
- rejected arguments;
- exact contract for that tool;
- the smallest valid structural example available;
- an instruction to change the invalid portion rather than repeat discovery.

Repair is bounded per failed signature. Authorization, transport, protocol,
Server, and domain failures must not be presented as schema failures.

## Result Classification

The runtime should normalize each tool result into one controller-owned
outcome:

| Outcome | Controller behavior |
| --- | --- |
| `Succeeded` | Record terminal evidence and update known state. |
| `InProgress` | Record non-terminal evidence, schedule a poll, and remain cancellable. |
| `ValidationFailed` | Do not execute; inject the exact contract and request a corrected action. |
| `Denied` | Stop or request a different non-equivalent action; never retry unchanged. |
| `TransportFailed` | Treat side effects as uncertain unless the request is known not to have been sent. |
| `ProtocolFailed` | Do not reinterpret malformed protocol data as domain success. |
| `ServerFailed` | Preserve Server identity and diagnostics; retry only under an explicit bounded policy. |
| `ToolFailed` | Preserve structured domain error details and require changed arguments or workflow. |
| `Cancelled` | End local waiting and ignore late responses. |

`ToolOutcome` is the controller-facing classification and
`ToolSideEffectState` distinguishes not-dispatched, dispatched, succeeded,
known-failed, and uncertain requests. `ToolFailureKind` remains available for
transport diagnostics.

## Run-Scoped Resource And Workflow State

Conversation text is not a reliable database. The controller should maintain a
small run-scoped ledger alongside tool evidence:

```text
ResourceRef
  serverId
  kind
  name
  stableId or UUID
  parentId
  sourceEvidenceSequence

JobRef
  serverId
  tool
  jobId
  status
  lastPollAt
  sourceEvidenceSequence

TaskStep
  id
  description
  requiresTool
  status
  evidenceSequences
  verificationState
```

Identifiers extracted from existing structured results retain provenance and
are never treated as new authorization. The compacted progress snapshot should
expose the ledger so the model can reuse an existing case, session, geometry
UUID, or parent selector without repeating `list` and `describe` calls. This
requires no change to the producer's result format; qtLLM records only fields
that are already present.

This is short-lived execution state, not cross-conversation memory.

## Mutation Verification

Verification should be proportional to risk and contract support:

- Read-only queries need no additional read-back.
- A create or edit call should be followed by an existing read-back operation
  when its result provides a stable identifier and the current catalog already
  contains an applicable inspection tool.
- A high-level task tool may return the created object and terminal state in
  one structured result; that can be sufficient when the output schema proves
  the requested fields.
- When no reliable verification path exists, completion review must describe
  that limitation instead of inventing success.

qtLLM must not invent verification mappings or add Shondy-specific conditionals.
The controller marks a mutation as awaiting verification, exposes the known
resource state to the model, and accepts a read-back selected from the existing
catalog. When no reliable existing verification path is available, completion
review reports that limitation instead of claiming success.

## Fixed External Tool Assumptions

The runtime must work with the tool providers as they exist today:

- Tool names, schemas, descriptions, enum values, and nesting may be complex or
  inconsistent with nearby descriptive APIs.
- A common user intent may require several low-level calls; no new task-level
  convenience tool is assumed.
- Results may omit a stable identifier or a dedicated verification path.
- Idempotency may be unknown, so qtLLM must not replay an uncertain mutation.
- Long-running state is recognized only from current structured results; no new
  status endpoint or result field is required.
- Missing domain semantics remain unknown. qtLLM improves contract presentation
  and recovery but does not guess business rules.

These constraints are acceptance inputs, not requests for MCP Server changes.

## Delivery Plan

### M1: Contract Fixture And Compact Catalog

Status: implemented.

Deliverables:

- Capture the inlet failure as a deterministic Agent-controller fixture using
  the existing complex discriminated-union schema. The fixture must not require
  an external MCP package change.
- Add a compact tool index that includes every allowed qualified name within a
  separate budget.
- Report omitted full-schema names explicitly.
- Add a bounded schema summarizer for required arguments, enum values,
  discriminators, and branch call shapes.

Primary code areas:

- `ToolCatalogBuilder`
- `AgentPromptBuilder`
- `ToolDefinition`
- Agent controller catalog tests

Acceptance:

- Every allowed tool remains name-discoverable when full schemas exceed the
  catalog budget.
- The `create_inlet` summary exposes `spec.name`, `source.type`, and the exact
  `geometry_file` discriminator without exceeding its budget.
- Catalog ordering and output remain deterministic valid JSON.

### M2: Structured Validation Repair

Status: implemented. The production validation boundary now carries the tool
name, instance path, schema path, validation keyword, and readable message.
Focused contract correction, local union validation, and an independent
bounded validation-repair path are also implemented.

Deliverables:

- Replace the boolean-plus-string validation boundary with a structured issue
  containing tool name, instance path, schema path or keyword, and message.
- Inject the failed tool's exact bounded contract into the next correction
  prompt.
- Track validation repair separately from malformed Agent-action repair and
  stagnation recovery.
- Prevent repeated `list` or `describe` calls when the correction can be made
  from the supplied contract.

Primary code areas:

- `AgentController::ValidateToolHandler`
- MCP registry argument validation
- `AgentPromptBuilder::correctionMessage`
- Agent controller validation-repair tests

Acceptance:

- Invalid arguments never reach the MCP Server.
- The inlet fixture reaches a schema-valid `create_inlet` action after at most
  one targeted correction.
- An unchanged invalid signature is rejected without consuming an MCP call.

### M3: Resource Ledger And Verification

Status: implemented. The controller now keeps bounded run-scoped resource,
job, and mutation-verification records and carries them through tool results,
context compaction, and completion review.

Deliverables:

- Add run-scoped resource and job records with evidence provenance.
- Preserve the records in context compaction and completion review.
- Add verification-step tracking driven by current tool evidence and the
  existing catalog.
- Reuse known UUIDs and parent-child relationships instead of rediscovering
  them through repeated calls.

Primary code areas:

- `AgentRun`
- `AgentContextCompactor`
- tool-evidence construction
- completion-review validation

Acceptance:

- Compaction preserves every identifier required by an unfinished task.
- A mutation step cannot be marked satisfied while its declared verification
  is pending or failed.
- The inlet fixture does not repeat successful model-discovery calls.

### M4: Unified Outcome And Retry Policy

Status: Implemented.

Deliverables:

- Normalize success, progress, cancellation, validation, authorization,
  transport, protocol, Server, and tool outcomes.
- Represent uncertain side effects explicitly.
- Apply retry rules by outcome and side-effect risk.
- Keep polling cancellable, delayed, and exempt only from the precise duplicate
  rule needed for the active status operation.

Primary code areas:

- `ToolResult`
- `ToolResultStatus`
- `AgentController::receiveToolResult`
- MCP request lifecycle and cancellation tests

Acceptance:

- A transport timeout after dispatch never blindly repeats a side-effecting
  call.
- Running jobs poll until one terminal result, then stop polling.
- Cancellation during generation, approval, execution, or poll delay produces
  no later state transition from a stale response.

Implemented policy:

- Host request failures preserve Server/tool identity and classify validation,
  cancellation, transport, protocol, Server, and tool outcomes.
- A dispatched mutation or unknown-risk tool stops on an uncertain remote
  failure. The user receives an explicit verification requirement instead of
  another mutating call.
- A read-only transport failure is retried once with the identical action. A
  second failure returns to normal decision recovery, where unchanged calls
  remain protected by the duplicate-failure rule.
- Only the exact active in-progress call can bypass duplicate-call rejection;
  polling waits at least one second and a terminal response clears poll state.

### M5: Real-Model Acceptance And Tuning

Deliverables:

- Run the existing lightweight and larger local-model matrix against simple,
  nested, mutation, polling, denial, and recovery scenarios.
- Record first-attempt tool choice, schema-valid argument rate, targeted repair
  count, redundant discovery count, duplicate mutation count, total tool calls,
  and completion-review success.
- Tune catalog and repair budgets from measured failures rather than adding
  domain-specific prompt rules.

Acceptance:

- Both model classes complete the common workflows defined in the existing Host
  acceptance plan without changing an external tool provider.
- The small model completes the inlet fixture with no duplicate mutation, no
  invalid nested listing, and no more than one validation repair.
- Failures are attributable to model decision, Host contract delivery,
  authorization, transport/protocol, Server behavior, or domain logic.

## Implementation Order And Dependencies

```text
M1 compact contracts
    |
    v
M2 targeted validation repair
    |
    v
M3 resource ledger and verification
    |
    v
M4 unified outcomes and retry policy
    |
    v
M5 real-model acceptance
```

M1 and M2 are the highest-value work for small models and should precede
additional autonomy or memory. M3 and M4 make longer workflows reliable. M5 is
the release gate and feeds measured corrections back into the earlier
milestones.

## Explicitly Deferred

- Any MCP Server source, tool schema, result format, configuration, or domain
  workflow change.
- New MCP protocol capabilities, transports, or Server-side metadata.
- Built-in coding tools, shell execution, repository editing, and test runners.
- Cross-conversation or persistent Agent memory.
- Multi-Agent delegation and skills.
- HTTP MCP transport, authentication, web search, and Host network tools.
- Generic automatic workflow synthesis from arbitrary prose-only Server
  descriptions.
- Hard-coded Shondy object types, selector grammars, or workflow names in the
  qtLLM Agent runtime.

## Completion Criteria

This plan is complete when qtLLM can use both a small and a larger local model
to execute representative workflows against unchanged external tools with
deterministic local validation, bounded targeted repair, correct asynchronous
behavior, resource reuse, mutation verification, and evidence-backed
termination. Success is measured by the final external state and recorded
evidence, not by whether the model produces a plausible narrative.
