# Agent Task Runtime Refactor

## Purpose

This document defines the next Agent runtime refactor after ordered plans were
moved into serial `ExecutionTask` workers. The goal is to make every model-driven
unit an independent task with its own context, lifecycle, timing, and result,
while reducing `AgentController` to a scheduler and asynchronous transport
adapter.

The baseline implementation began in commit `a7355a1`, which introduced serial
`ExecutionTask` execution. Commits `502cc67` and `e561a8b` separated the three
task phases and moved tool-result semantics into execution tasks. This document
tracks the remaining separation of responsibilities; it does not replace
[`mcp-agent-runtime-plan.md`](mcp-agent-runtime-plan.md).

## Target Architecture

```text
AgentController
    |
    +-- owns ordered task queue and current index
    +-- forwards model, approval, and tool transport events
    +-- projects aggregate run progress
    |
    v
AgentTask
    |
    +-- PlanningTask  -> task list or direct completion
    +-- ExecutionTask -> one planned outcome or direct request and its MCP calls
    +-- SummaryTask   -> final user-facing answer
```

Only one task may be active. A later `ExecutionTask` cannot receive model tokens,
invoke tools, consume tool results, or become current until every earlier task
has reached a terminal state accepted by the scheduler.

## Responsibility Boundary

| Component | Owns | Must Not Own |
| --- | --- | --- |
| `AgentTask` | Common lifecycle, messages, token buffer, timing, cancellation, failure, and snapshots | MCP-specific task semantics |
| `PlanningTask` | Deciding whether a plan is needed, validating outcome-level granularity, and producing task specifications | Executing later plan steps or creating one task per tool call |
| `ExecutionTask` | One requested outcome or direct request, including its dependent MCP calls, allowed-tool boundary, code-owned evidence, completion checks, and bounded repair | Advancing the queue or operating on a later outcome |
| `SummaryTask` | Building the final answer from immutable completed-task snapshots | Calling MCP tools or changing completed steps |
| `AgentController` | Queue order, active task, run-level state, and routing asynchronous callbacks to the active task | Interpreting task actions, reviewing step completion, or choosing recovery prompts |
| `AgentToolRuntime` | Tool catalog lookup, risk classification, schema validation, policy and approval state, dispatch, cancellation, polling, and normalized tool results | Understanding plan structure or deciding whether a task is complete |

The task owns the semantic MCP workflow. `AgentToolRuntime` owns provider-neutral
tool contracts and transport state, while the controller routes asynchronous
Qt callbacks. That routing detail must not give the controller authority to
choose a tool, alter arguments, review evidence, or advance a task.
`PlanningTask` may inspect the supplied catalog when constructing tasks, but
only `ExecutionTask` may produce a `CallTool` directive.

## Runtime Invariants

1. At most one `AgentTask` is active.
1. Each planned task represents an independently verifiable result explicitly
   requested by the user. Task boundaries do not come from numbering or tool
   count; causally dependent calls without their own requested result remain
   inside the outcome task.
1. Every model completion and tool result is routed to the task that initiated
   it.
1. A tool action is valid only when the active task accepts it.
1. Every accepted tool call and result is bound by code to the active
   `ExecutionTask` and its task-local evidence range.
1. The scheduler advances only after the active task returns a terminal
   `Completed` result.
1. Task state and evidence assignment are maintained by code. The model emits
   only ordinary task actions and may use `final` to claim the current outcome
   is complete; it never emits task IDs, completion markers, evidence IDs, or
   queue status.
1. A tool-backed task accepts `final` only when its own evidence range contains
   successful terminal evidence, its latest operation is terminal, and the
   ledger has no unresolved verification. The task then attaches evidence and
   returns `Completed` without another model review turn.
1. Aggregate progress exposes the active decision stage, such as argument
   correction, task-boundary review, completion validation, or next-action
   selection, without exposing hidden model reasoning.
1. Completed-task snapshots are immutable inputs to later tasks.
1. `SummaryTask` cannot invoke tools.
1. Provider-specific MCP names, tool names, argument fields, and domain rules
   never appear in the generic scheduler.
1. Cancellation and late-result rejection remain deterministic at the run and
   task boundaries.

## Common Task API

The shared base is `AgentTask`; `ExecutionTask` is its concrete model-and-tool
execution type. This keeps the class aligned with `AgentTask::Kind::Execution`.

The public API should expose a small lifecycle and return value-based
directives. Exact names may change during implementation, but responsibilities
must remain stable.

```cpp
class AgentTask
{
   public:
    enum class Kind
    {
        Planning,
        Execution,
        Summary
    };

    enum class Status
    {
        Pending,
        Running,
        WaitingForModel,
        WaitingForApproval,
        WaitingForTool,
        Completed,
        Blocked,
        Cancelled,
        Failed
    };

    virtual ~AgentTask() = default;

    [[nodiscard]] virtual TaskDirective start(const TaskContext&) = 0;
    [[nodiscard]] virtual TaskDirective completeGeneration(
        const QByteArray& output) = 0;
    [[nodiscard]] virtual TaskDirective receiveToolResult(
        const agent::ToolResult& result) = 0;
    [[nodiscard]] virtual TaskDirective resolveApproval(bool approved) = 0;

    [[nodiscard]] TaskSnapshot snapshot() const;
    void receiveToken(const QByteArray& bytes);
    void cancel();
};
```

Common status must stay coarse. Details such as `AwaitingReview`,
`AwaitingCallReview`, evidence ranges, and repair counters remain private to
`ExecutionTask`. UI-specific detail is exposed as snapshot text rather than by
expanding the shared state machine for one subclass.

## Task Directives

A task communicates with the scheduler through one result type. The scheduler
must not inspect raw model actions to infer what should happen next.

```cpp
struct TaskDirective
{
    enum class Type
    {
        Generate,
        CallTool,
        TasksCreated,
        Continue,
        Completed,
        Blocked,
        Failed
    };

    Type type = Type::Continue;
    std::optional<agent::Action> toolAction;
    std::vector<TaskSpecification> tasks;
    QString content;
    QString code;
    QString detail;
};
```

Approval is a tool-runtime concern. A `CallTool` directive may lead to local
validation, automatic execution, or a user approval request. The resolution
must be returned to the same task before it can continue.

## Concrete Tasks

### PlanningTask

`PlanningTask` owns the initial model decision when planning is required. It
has two successful outcomes:

- `TasksCreated`: returns an ordered list of validated `TaskSpecification`
  values.
- `Completed`: returns a final response when no execution task is necessary.

Planning must not create mutable runtime tasks until the complete plan has
passed structural validation. Task IDs, descriptions, tool requirements, and
allowed-tool declarations become immutable after queue construction.

The existing inexpensive heuristic may bypass `PlanningTask` for requests that
do not need a plan. The scheduler then creates one direct execution task or a
summary-only task. Avoid requiring an extra model turn for every trivial
request solely to preserve architectural symmetry.

### ExecutionTask

Each `ExecutionTask` owns either one immutable plan specification or one direct
request, together with an independent model conversation. It is responsible
for:

- activating from immutable prior-task context;
- requesting and parsing model actions;
- rejecting actions assigned to another plan step;
- validating the task's allowed-tool boundary;
- initiating tool-call review when required;
- accepting normalized MCP results;
- maintaining task-local evidence;
- reviewing terminal evidence immediately after a completing tool result;
- performing bounded, task-local repair;
- returning `Completed`, `Blocked`, or `Failed`.

The task must never expose a method that advances the global plan index.

### SummaryTask

`SummaryTask` is created only after all required execution tasks are terminal.
It receives:

- the original user request;
- immutable task snapshots;
- compact successful evidence;
- blocked or failed task details when the run permits a partial answer.

It has no tool catalog and rejects every tool action. Its only successful
terminal output is the final user-facing answer. This removes the current
special case where the last `ExecutionTask` changes into a final-answer
generator.

## Timing And Progress

Every `AgentTask` records:

```text
createdAt
startedAt
finishedAt
elapsedMilliseconds
```

Elapsed time is calculated from a monotonic timer for runtime behavior. UTC
timestamps may also be recorded for logs. A task snapshot includes:

```cpp
struct TaskSnapshot
{
    QString id;
    AgentTask::Kind kind;
    AgentTask::Status status;
    QString description;
    QString activity;
    qint64 elapsedMilliseconds = 0;
};
```

The progress UI projects these snapshots. It does not infer task completion
from event text. This supports an independent timer for planning, every plan
step, and summary generation.

## Controller End State

After the refactor, the controller loop should be conceptually equivalent to:

```cpp
void AgentController::apply(TaskDirective directive)
{
    switch (directive.type)
    {
        case TaskDirective::Type::Generate:
            requestGenerationFor(currentTask());
            break;
        case TaskDirective::Type::CallTool:
            toolRuntime_.submit(currentTaskId(), *directive.toolAction);
            break;
        case TaskDirective::Type::TasksCreated:
            appendPlanTasks(directive.tasks);
            advance();
            break;
        case TaskDirective::Type::Completed:
            finishCurrentTask(directive.content);
            advance();
            break;
        case TaskDirective::Type::Blocked:
            blockRun(directive.detail);
            break;
        case TaskDirective::Type::Failed:
            failRun(directive.code, directive.detail);
            break;
        case TaskDirective::Type::Continue:
            break;
    }
}
```

The actual code may contain Qt state and metrics plumbing, but all branches
that parse model actions, decide task completion, inspect evidence, or build
repair prompts belong to a task implementation.

## Migration Plan

### M1: Serial ExecutionTask Baseline

Status: complete in `a7355a1`.

- Store ordered tasks as `std::vector<ExecutionTask>`.
- Activate and execute only the current index.
- Give each plan step its own conversation.
- Enforce task ID and tool boundaries in `ExecutionTask`.
- Project completion steps from task snapshots.

### M2: Common AgentTask Lifecycle

Status: complete in `a985caf` and `502cc67`.

- Add the abstract `AgentTask` base.
- Move common messages, token buffering, coarse status, cancellation, and
  timing out of `ExecutionTask`.
- Introduce `TaskDirective`, `TaskContext`, and `TaskSnapshot` value types.
- Keep task-specific review states private.
- Change `AgentRun` ownership to task pointers with stable lifetime, such as
  `std::vector<std::unique_ptr<AgentTask>>`.

Acceptance criteria:

- `ExecutionTask` contains no duplicated common lifecycle implementation.
- The controller reads task state through the base API.
- Every task has an independently testable clock and snapshot.

### M3: Task-Owned Execution Loop

Status: complete for task semantics. `ExecutionTask` owns action parsing,
bounded action and review repair, tool-result interpretation, task-local
recovery guidance, and the decision to start current-step evidence review.
Schema validation, policy, and asynchronous MCP transport remain in the
controller pending extraction of the generic tool runtime adapter.

- Move ordered action handling from `AgentController` into `ExecutionTask`.
- Route generation completion, approval resolution, and normalized tool
  results directly to the active task.
- Replace controller repair functions for ordered execution with task-local
  directives.
- Keep schema validation, tool policy, and physical MCP dispatch in the
  generic tool runtime.

Acceptance criteria:

- The controller never parses an ordered task action.
- The controller never calls `beginPlanStepReview`, chooses a repair prompt,
  or decides that task evidence is sufficient.
- An `ExecutionTask` test can run a complete model/tool/review cycle without an
  `AgentController` fixture.

### M4: PlanningTask And Queue Construction

Status: complete. The production planning turn and plan repair are owned by
`PlanningTask`, queue construction occurs only after its directive, and every
accepted plan is strictly ordered. The legacy unordered-plan execution path has
been removed.

- Move initial task-plan parsing and repair into `PlanningTask`.
- Return validated `TaskSpecification` values through `TasksCreated`.
- Construct the execution queue only after planning completes.
- Preserve the direct path for requests that do not require planning.

Acceptance criteria:

- `AgentController` does not understand `task_plan` model actions.
- An invalid plan is repaired or failed entirely inside `PlanningTask`.
- No execution task is activated before the full ordered plan is accepted.

### M5: SummaryTask

Status: complete in `502cc67`.

- Remove final-answer mode from the last `ExecutionTask`.
- Create `SummaryTask` after all required execution tasks complete.
- Give it compact immutable task and evidence snapshots.
- Reject tool calls from summary generation.

Acceptance criteria:

- The last execution task terminates like every other `ExecutionTask`.
- Final generation is visible and timed as a distinct task.
- Summary generation cannot mutate MCP state.

### M6: Controller Reduction And UI Projection

Status: completed. Per-task timing and Activity task boundaries are projected
from task snapshots/events. The legacy global completion-review state machine,
unordered-plan compatibility path, and controller-owned model token buffer are
removed. Generic tool lookup, risk classification, schema validation, policy
decisions, and task-local duplicate-call protection now live in
`AgentToolRuntime`. Queue construction and task boundaries remain outside it.
Policy-driven approval state also lives in `AgentToolRuntime`; the controller
only projects that state to the UI and forwards the user's decision.
Tool request dispatch, request identity, cancellation, and status-poll
throttling also live in `AgentToolRuntime`; the controller only routes the
completed call to the active task and owns Qt timer wiring.

The controller intentionally retains queue construction from the validated
planning directive and the single serial queue index. Those are scheduler
responsibilities, not provider/tool-runtime responsibilities; no tool call is
executed while the queue is being built.

- Keep obsolete global plan-review and ordered-repair state out of `AgentRun`
  and `AgentController`.
- Project progress directly from task snapshots.
- Display the active task's text activity and elapsed time.
- Preserve aggregate run metrics separately from per-task metrics.

Acceptance criteria:

- The controller is a scheduler and callback router rather than a second task
  state machine.
- Planning, every execution step, and summary have independent elapsed time.
- UI progress remains correct without parsing event strings.

## Test Strategy

### AgentTask Tests

- Common status transitions and cancellation.
- Token size limit and generation failure.
- Monotonic elapsed time before, during, and after execution.
- Immutable terminal snapshots.

### PlanningTask Tests

- Valid plan produces ordered specifications.
- Invalid plan receives one bounded correction.
- Direct completion creates no execution queue.
- Future task details do not leak into an active execution conversation.

### ExecutionTask Tests

- A task cannot call a later task's tool action.
- Shared tool names remain isolated by task ID and arguments.
- Completing evidence is reviewed immediately.
- Pending review stays in the same task.
- Approval and tool results return to the initiating task.
- Failure, cancellation, and uncertain mutation results do not advance the
  queue.

### SummaryTask Tests

- Summary receives every terminal task snapshot in order.
- Summary cannot call tools.
- Failed or blocked tasks are represented accurately.
- Final content is emitted once.

### Controller Tests

- Tasks run strictly in queue order.
- Only one task is active.
- Late model and tool results are ignored.
- A completed task advances exactly once.
- Run cancellation stops the active task and leaves later tasks pending.
- Progress exposes independent task timings.

## Explicit Non-Goals

- Parallel plan execution.
- Recursive or nested sub-agent trees.
- Provider-specific MCP workflows in qtLLM.
- Letting the model modify scheduler state directly.
- Moving schema validation or authorization into model prompts.
- Persisting task conversations across unrelated Agent runs.

## Completion Criteria

The refactor is complete when:

1. `PlanningTask`, `ExecutionTask`, and `SummaryTask` share the common `AgentTask`
   lifecycle.
1. Every model or tool callback is routed to one identifiable active task.
1. `AgentController` contains no step-review semantics or ordered repair
   prompts; it only converts an already validated planning directive into the
   serial queue and advances that queue.
1. All task transitions and queue advancement are enforced by code.
1. Each task exposes an independent elapsed time and progress snapshot.
1. The runtime remains generic across MCP providers.
1. Unit and controller tests cover strict serial ordering and late-result
   rejection.
