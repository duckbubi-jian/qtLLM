# Agent Progress UI Plan

## Purpose

The Agent currently exposes too little useful progress while a run is active.
The primary activity message cycles through labels such as `Thinking...`,
`Using a tool...`, and `Preparing the answer...`. More detailed events are
written to the activity log, but the plan, current step, evidence, and next
action are not presented as a coherent progress view.

This plan adds a structured, user-facing Agent progress surface inside qtLLM.
It improves observability of the control loop without exposing the model's
private chain of thought or changing any MCP Server, tool schema, or protocol.

## Implementation Status

Implemented in qtLLM on 2026-08-18:

- the application layer emits bounded `AgentProgressSnapshot` updates for
  plans, state transitions, tool activity, evidence, recovery, and terminal
  outcomes;
- `AgentProgressWidget` renders elapsed time, ordered plan steps, the current
  operation, waiting state, counters, and expandable recent activity;
- `MainWindow` keeps the structured progress item beside the final answer and
  collapses it after completion, cancellation, or failure;
- progress text is bounded and redacted before display, and late non-terminal
  snapshots cannot overwrite a terminal summary;
- controller and UI tests cover planning, tools, evidence, approval, recovery,
  cancellation, completion, failure, narrow layouts, and long text.

The existing Activity tab remains the detailed diagnostic record. No MCP
Server or MCP protocol change was required.

## Product Boundary

The progress view must show what the runtime has accepted, executed, observed,
and is waiting for. It must not present hidden model reasoning as a transcript.
The runtime remains the source of truth for state, tool execution, evidence,
approval, retries, and completion.

The change is limited to qtLLM:

- no MCP Server changes;
- no new MCP protocol messages;
- no new external tool requirements;
- no raw prompt, hidden reasoning, or unrestricted tool output in the primary
  progress view.

## Current Foundation

The runtime already has most of the information required for this feature:

- `AgentController` owns the run state and emits `AgentEvent` records;
- `AgentRun::completionSteps` stores the accepted task plan;
- tool start and finish events carry the qualified tool name and structured
  result summary;
- the ledger tracks resources, verification, and evidence revisions;
- `MainWindow` already renders a transient activity message and an activity
  log.

The main gap is a stable presentation model between controller events and the
current transient activity message.

## Target Experience

While an Agent run is active, the conversation should contain a compact
progress view similar to:

```text
Agent working                          00:18
Plan: 3 steps, 1 complete

  [done] Check the workspace
  [run ] Create the inlet
  [     ] Verify the inlet

Current: calling shondy.create_inlet
Waiting for the tool result...

Recent activity  v
  14:22:10  Tool completed
```

The view should make these questions answerable without reading raw logs:

1. What is the Agent doing now?
1. What has it already completed?
1. What is the next unfinished step?
1. Is it waiting for approval, a tool result, or a model response?
1. Why did it retry, stop, or fail?

After completion, cancellation, or failure, the progress view should collapse
to a short run summary and remain expandable from the conversation. It should
not disappear before the user can inspect the outcome.

## Data Contract

### Progress snapshot

Introduce a UI-facing run snapshot owned by the application layer. It should
contain only data needed for rendering:

- run id and elapsed time;
- current `AgentRun::State`;
- plan steps with stable id, description, and status;
- current step id, when known;
- current operation summary and waiting reason;
- completed tool count and evidence count;
- bounded recent event summaries;
- terminal code and message after completion or failure.

The snapshot should be derived from the controller's existing run state and
events. The UI must not inspect inference messages or reconstruct state from
free-form log text.

### Event additions

Extend `AgentEvent` only where existing event types cannot express the state.
Candidate events are:

- `TaskPlanAccepted`;
- `TaskStepUpdated`;
- `EvidenceUpdated`;
- `RecoveryStarted`;
- `RunWaiting`.

Each event should carry a short human-readable message plus bounded structured
data. Existing `RunStarted`, `ToolStarted`, `ToolFinished`, `ApprovalRequested`,
`Warning`, `Completed`, `Cancelled`, and `Failed` events remain compatible.

### Redaction and bounds

- Reuse the existing sensitive-value redaction helper.
- Show tool names and safe argument summaries, not arbitrary full arguments by
  default.
- Limit each event summary and the retained event list.
- Treat tool descriptions and result text as untrusted content; render them as
  text or sanitized Markdown only.
- Never display hidden reasoning, system prompts, access tokens, or raw
  transport payloads in the primary view.

## Delivery Milestones

### P0: Progress contract

- Add a small progress snapshot type in the application layer.
- Add a `progressChanged` signal or equivalent run-scoped update path.
- Emit plan acceptance, step transitions, waiting reasons, evidence updates,
  recovery, and terminal state changes.
- Keep existing event logging unchanged for diagnostics.
- Add controller tests for snapshot transitions and redaction boundaries.

### P1: Minimal progress widget

- Add an `AgentProgressWidget` to the UI module.
- Render current state, elapsed time, plan summary, and step statuses.
- Render a bounded recent-activity section with expand/collapse behavior.
- Keep the widget dimensions stable while text and steps update.
- Preserve the current stop and approval workflows.

### P2: MainWindow integration

- Replace the current single-purpose thinking activity message with the
  progress widget for Agent runs.
- Feed it from structured snapshots, not from `event.message` parsing.
- Keep the existing activity log available as a secondary diagnostic view.
- Preserve automatic scrolling without forcing the user to the bottom when
  they are inspecting earlier conversation content.
- Keep a collapsed final summary after `runFinished`.

### P3: Robustness and polish

- Add UI tests for plan rendering, tool transitions, approval waiting, retry,
  failure, cancellation, and completion.
- Test long step descriptions, many steps, long tool names, and narrow window
  sizes.
- Verify that sensitive values are redacted in both collapsed and expanded
  states.
- Verify that late tool results cannot update a finished run's progress view.
- Add accessible labels and keyboard-operable expand/collapse controls.

## State Mapping

| Runtime state or event | Progress presentation |
| --- | --- |
| `Deciding` / `DecisionStarted` | `Choosing next action` |
| `TaskPlanAccepted` | Show or update the step list |
| `WaitingForApproval` / `ApprovalRequested` | `Waiting for your approval` |
| `ExecutingTool` / `ToolStarted` | Show tool name and current step |
| `ToolFinished` | Mark result and update evidence summary |
| `GeneratingAnswer` / `AnswerStarted` | `Preparing the final answer` |
| `Warning` / `RecoveryStarted` | Show bounded retry or recovery reason |
| `Completed` | Show completed summary and verified steps |
| `Cancelled` | Show cancellation summary |
| `Failed` | Show terminal category, message, and last step |

## Acceptance Criteria

The feature is ready when:

1. A task plan becomes visible immediately after the controller accepts it.
1. The current step and recent tool activity update without parsing log text.
1. Approval, polling, retries, evidence, cancellation, and failure have
   distinct user-visible states.
1. The final answer remains visually separate from progress details.
1. No hidden reasoning, secret, or unbounded raw MCP payload is displayed.
1. The view remains usable at normal desktop and narrow window sizes.
1. Existing Agent, UI, and MCP host tests remain green.
1. A finished run leaves an inspectable, collapsed summary in the conversation.

## Non-Goals

- Exposing the model's chain of thought.
- Replacing the existing diagnostic activity log.
- Adding a new planning model or changing Agent action semantics.
- Changing MCP schemas, Server behavior, or tool execution policy.
- Building a general workflow editor or task-management subsystem.

## Implementation Map

Likely files for the implementation are:

- `shared/agent/include/AgentEvent.hpp` for event vocabulary;
- `app/application/include/AgentRun.hpp` for run-owned progress state;
- `app/application/include/AgentController.hpp` and its implementation for
  snapshot production and update signals;
- `app/ui/include/AgentProgressWidget.hpp` and its implementation for the
  presentation;
- `app/ui/include/MainWindow.hpp` and `MainWindow.cpp` for integration;
- `tests/agent_controller` for state and redaction tests;
- `tests/ui` for rendering and interaction tests.

## Recommended Order

Implement P0 and P1 first, then validate the experience with a real
multi-step MCP run before expanding the widget. P2 and P3 should follow only
after the snapshot semantics are stable; otherwise the UI will encode behavior
that still belongs in the controller.
