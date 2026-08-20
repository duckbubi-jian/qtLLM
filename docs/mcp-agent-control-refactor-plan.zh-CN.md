# qtLLM MCP Agent 控制流程重构计划

## 1. 背景

qtLLM 当前已经具备 MCP Server 启动、初始化、工具发现、参数校验、权限审批、
调用执行、超时和结果归一化等基础能力。当前主要可靠性问题不在 MCP JSON-RPC
协议本身，而在模型面对的 Agent 控制协议过重：模型需要在多个回合中严格输出
`task_plan`、`call_tool`、`review_tool_call`、`review_plan_step` 和 `final`，并同步
维护 `plan_step_id`、`allowed_tools`、`completes_plan_step` 等运行时状态。

真实运行日志表明，这些控制要求会导致以下失败：

- 模型遗漏或写错 `plan_step_id`；
- 控制字段被错误放进工具参数，触发 JSON Schema 校验失败；
- 计划阶段遗漏恢复工具，执行阶段又被 `allowed_tools` 阻止纠错；
- 工具已经成功，但模型审查没有认可现有证据；
- 模型重复调用已经成功的工具，最终触发停滞保护；
- 计划和审查回合破坏消息角色交替；
- 大型工具目录和控制提示长期占用 14,000 至 21,000 个 prompt token。

本计划不删除安全检查，而是将当前的“模型强制自审”迁移为：

> Host 强制验证每一次执行；只有无法程序化判断的语义问题才请求模型审查。

本文档是 [`mcp-agent-runtime-plan.md`](mcp-agent-runtime-plan.md) 和
[`agent-task-runtime-refactor.md`](agent-task-runtime-refactor.md) 的后续控制流程
修订计划。迁移完成前，旧文档仍用于解释当前实现；完成后应同步更新旧文档，
避免它们继续把模型审查动作描述为目标架构。

## 2. 目标

1. 将模型输出协议收敛为“调用工具、最终回答、明确阻塞”三类动作。
1. 由 Host 保存计划、步骤、调用、证据、重试和完成状态。
1. 用确定性验证替代常规的 `review_tool_call` 和 `review_plan_step`。
1. 保留高风险操作审批、参数校验、重复调用保护和不确定副作用保护。
1. 对工具目录实施选择、延迟加载和按需展开，降低首轮上下文占用。
1. 保证本地小模型即使不支持原生 function calling，也只需遵循最小 JSON
   grammar。
1. 改善可观察性，使一次失败可以归因到模型、Host、传输、Server 或验证规则。

## 3. 非目标

- 不在本次重构中引入多 Agent、长期记忆或后台自主任务。
- 不把 ShonDy 或其他业务域规则硬编码到通用 Agent 调度器。
- 不依赖 MCP Server 提供的描述或 annotations 作为安全授权依据。
- 不要求一次性删除当前任务类；迁移期间允许新旧控制循环并存。
- 不以增大上下文窗口或更换更大模型代替控制流程修复。

## 4. 设计原则

### 4.1 模型负责语义，Host 负责状态

模型负责理解用户意图、选择下一项工具、填写参数和组织最终回答。Host 负责：

- 当前任务及步骤；
- 工具调用 ID 和消息配对；
- 参数与权限校验；
- 调用是否已经执行；
- 证据记录和资源版本；
- 重试、轮询、超时和取消；
- 当前操作是否满足确定性完成条件；
- 是否允许结束整个任务。

模型不得通过输出字段直接修改 Host 已记录的执行状态。

### 4.2 取消模型强制自审，不取消强制验证

每个工具结果都必须经过 Host 验证。验证分为三层：

| 层级 | 执行者 | 适用范围 | 示例 |
| --- | --- | --- | --- |
| 协议与安全校验 | Host | 所有调用 | 工具存在、schema、授权、审批、超时、结果 ID |
| 确定性效果验证 | Host/检查工具 | 可程序化判断的效果 | 写后读、字段比较、任务状态轮询 |
| 条件式语义审查 | 模型 | 无法程序化判断的业务语义 | 结果是否满足自然语言约束 |

条件式语义审查不是默认状态，也不得负责推进计划、选择恢复工具或重写执行
历史。

### 4.3 副作用不确定时禁止盲目重放

写操作已经发出、但传输在收到最终响应前失败时，Host 必须将结果记为
`Uncertain`。后续只能先读取远端状态或交给用户处理，不能自动重放同一调用。

### 4.4 逐步迁移，可度量，可回滚

新循环先放在运行时开关后，通过固定场景和真实模型进行对照。每个阶段都要有
独立验收条件；任一阶段回滚不得关闭参数校验、审批或副作用保护。

## 5. 目标控制流程

```text
用户请求
   |
   v
Host 建立任务状态，可选地产生内部计划
   |
   v
选择少量相关工具并向模型提供完整 schema
   |
   v
模型输出 call_tool / final / blocked
   |
   +-- final --> Host 检查未完成任务和证据 --> 接受或给出缺失事实
   |
   +-- blocked --> Host 检查是否存在确定性恢复路径 --> 阻塞或继续
   |
   v
Host 校验工具、参数、策略和审批
   |
   v
执行 MCP tools/call
   |
   v
结果归一化并写入 Evidence Ledger
   |
   +-- running/pending --> Host 等待并轮询
   +-- validation/domain error --> 将最小修复信息交给模型
   +-- uncertain mutation --> 读取状态，禁止盲目重放
   +-- success --> 执行确定性验证
                         |
                         +-- 通过 --> 更新任务状态
                         +-- 缺少证据 --> Host 调用检查工具或请求下一动作
                         +-- 仅语义可判定 --> 条件式模型审查
```

## 6. 最小模型动作协议

### 6.1 目标动作

模型正常执行阶段只允许以下动作：

```json
{"action":"call_tool","tool":"server.tool","arguments":{}}
```

```json
{"action":"final","content":"面向用户的最终回答"}
```

```json
{"action":"blocked","reason":"缺少的事实或外部条件"}
```

`call_tool` 不再包含以下字段：

- `plan_step_id`；
- `completes_plan_step`；
- 模型生成的调用审批或审查结果；
- 模型生成的证据序号。

Host 在接收动作时，将调用绑定到当前内部任务和唯一 `tool_call_id`。

### 6.2 原生工具调用与 grammar 兼容

- 模型模板支持原生 tool/function calling 时，使用原生 tool definitions、
  `tool_call_id` 和 tool result role。
- 不支持原生工具调用时，继续使用 llama.cpp grammar，但 grammar 只描述上述三个
  动作，不描述计划和审查状态机。
- 两条路径必须进入同一个 Host `ToolCallRequest`，后续校验和执行逻辑不得分叉。

## 7. Host 验证模型

### 7.1 统一证据记录

建议为每次已执行调用保存不可变证据：

```cpp
struct EvidenceRecord
{
    QString callId;
    QString taskId;
    QString toolName;
    QJsonObject arguments;
    ToolOperationKind operationKind;
    ToolOutcome outcome;
    SideEffectState sideEffectState;
    QJsonValue structuredResult;
    QList<ResourceRevision> resources;
    QDateTime startedAt;
    QDateTime finishedAt;
};
```

模型看到的是经过限长的证据摘要。完整结构由 Host 保存，不应在每个回合重复写入
system prompt。

### 7.2 验证结果

```cpp
enum class VerificationStatus
{
    Satisfied,
    NeedsInspection,
    NeedsSemanticReview,
    Failed,
    Uncertain
};
```

验证器只返回状态和可执行的下一步要求，不直接生成模型提示文本。

### 7.3 通用验证矩阵

| 操作类型 | 工具成功后 | 完成条件 |
| --- | --- | --- |
| 纯读取 | 校验结构化结果有效 | 已取得用户请求的事实或明确不存在 |
| 创建对象 | 读取返回的 ID/revision；必要时 get/list | 对象存在且关键字段匹配 |
| 修改对象 | 优先读取新 revision，否则自动 read-back | 目标字段与期望值一致 |
| 删除对象 | 必要时自动 read-back | 对象不存在或 Server 明确返回幂等完成 |
| 启动长任务 | 记录 job ID 并轮询 | 进入成功终态，而不是仅“启动成功” |
| 文件写入 | read-back、大小/hash 或明确 revision | 内容或版本匹配 |
| UI 聚焦等瞬时操作 | 接受明确成功结果 | Server 明确完成；不虚构持久状态 |
| 复杂自然语言约束 | 先完成所有可确定性检查 | 仅剩语义判断时调用 reviewer |

验证规则必须来自通用操作元数据、结构化结果和受信任的本地配置。MCP
`annotations` 可以作为提示，但不得单独决定授权或是否跳过验证。

### 7.4 条件式语义审查

只有 `VerificationStatus::NeedsSemanticReview` 才创建 reviewer 请求。reviewer
输入限制为：

- 原始目标的相关片段；
- 已执行操作的结构化证据摘要；
- 需要判断的单一问题。

reviewer 只允许输出：

```json
{"verdict":"pass","reason":"证据如何满足目标"}
```

或：

```json
{"verdict":"fail","reason":"缺少或冲突的事实"}
```

reviewer 不允许调用工具、修改任务状态或输出新的计划。Host 根据 verdict 和本地
规则决定继续、失败或请求更多证据。

### 7.5 人工举证与可恢复阻塞

确定性回读不可用、字段缺失或 mutation 最终状态不确定时，Host 不应让模型反复
“自我举证”，也不应直接把 run 标记为失败。运行时进入可恢复的
`WaitingForVerification` 状态，并显示独立的临时举证输入框。用户可以：

1. 提交临时举证并恢复当前 `ExecutionTask`；
1. 明确接受当前未验证结果；
1. 终止任务并进入最终 `Blocked` 状态。

账本必须区分三种已解除阻塞的证据等级：

- `verified`：Host 通过结构化结果或 read-back 确定性验证；
- `attested`：用户提交了临时举证；
- `accepted`：用户明确接受当前结果，但没有补充确定性证据。

工具已发出但最终结果未知时，验证记录先进入未解除的 `uncertain` 状态；它只能由
匹配的确定性回读、用户临时举证或用户明确接受来解除。

`attested` 和 `accepted` 可以解除完成阻塞，但不能显示或记录为 Host
`verified`。临时举证正文只进入当前任务上下文；持久账本保存摘要、长度和 hash，
不复制用户正文。若后续取得正确 read-back，`attested` 或 `accepted` 可以升级为
`verified`。风险审批仍是工具执行前的独立流程，不得与执行后的人工举证合并。

人工举证卡必须明确展示当前步骤、mutation 工具、evidence sequence、目标名称或
稳定 ID、Host 判定的问题，以及需要用户检查的字段和期望值。一次提交或接受只能解除
卡片当前展示的 evidence sequence；同一步存在多条未验证 mutation 时，Host 必须逐条
继续阻塞和询问，不能用一条笼统确认批量解除。

## 8. 分阶段实施

### 阶段 0：建立基线和运行时开关

目标：在改变行为前得到可对照的数据，并确保新循环可以快速关闭。

工作项：

1. 增加 `agentControlMode = legacy | host_verified` 配置，默认先保持 `legacy`。
1. 为每次 run 记录模型决策次数、工具调用次数、审查次数、无效动作次数、修复
   次数、prompt token、压缩次数和终止原因。
1. 建立固定场景集：单次读取、一次修改、多对象修改、创建后修改、长任务轮询、
   schema 修复、工具不可用、超时、不确定副作用、用户取消。
1. 保存当前模型和参数下至少 30 次重复运行的基线。

验收条件：

- 指标可以按 run 和场景查询；
- legacy 行为不变；
- 每个失败都能归到明确的终止原因。

回滚：关闭 `host_verified`，不删除新增指标。

### 阶段 1：实现 Host Evidence Ledger 和确定性验证

目标：先建立替代能力，再移除模型审查。

工作项：

1. 将工具结果统一写入 `EvidenceRecord`。
1. 引入 provider-neutral `VerificationPolicy` 和 `VerificationResult`。
1. 为读取、修改、创建、删除和长任务实现通用规则。
1. 支持 Host 发起的 read-back/poll 调用，并与模型调用分别计数。
1. 保留当前模型审查作为 shadow comparison：其结果只记录，不推进状态。
1. 对 uncertain mutation 强制进入检查或阻塞路径。

验收条件：

- 所有工具调用都有唯一证据记录；
- 修改和创建场景可以在不依赖 `review_plan_step` 的情况下完成；
- 长任务不会在非终态完成；
- uncertain mutation 不会被自动重放；
- shadow reviewer 与确定性验证不一致时有可检索日志。

回滚：重新启用 legacy 审查推进，Evidence Ledger 保留为只读观测。

### 阶段 2：简化模型动作和任务状态

目标：模型不再维护 Host 计划状态。

工作项：

1. 为 `host_verified` 添加最小动作 parser 和 grammar。
1. Host 自动把 `call_tool` 绑定到当前 task ID 和 call ID。
1. 删除新模式对 `plan_step_id`、`completes_plan_step`、
   `review_tool_call`、`review_plan_step` 的要求。
1. 将 plan 变为 Host 的内部进度结构；规划可选，简单请求直接进入执行循环。
1. 当模型提前 `final` 时，Host 返回具体缺失事实，不要求模型输出审查动作。
1. 将 allowed tools 从计划生成的封闭集合改为 Host 的动态候选集合；安全权限仍由
   ToolPolicy 决定。

验收条件：

- 新模式中不再出现 plan step ID 格式错误；
- 一次读取任务通常只需要一次工具决策和一次最终回答；
- 恢复工具不会因为规划器遗漏而被禁止；
- 没有 evidence 时不能完成需要工具的任务；
- 有充分 evidence 时不需要额外模型审查即可完成。

回滚：配置切回 legacy parser 和任务状态机。

### 阶段 3：工具选择与延迟加载

目标：避免把完整工具目录注入每一个模型回合。

工作项：

1. 首轮仅提供命名空间、工具名称和单行用途索引。
1. 根据用户请求、当前任务和历史证据选择约 8 至 20 个候选工具。
1. 只展开候选工具的完整 input schema。
1. 候选不足时允许 Host 扩展目录或提供 `search_tools/load_tools`。
1. 缓存按 server revision 和过滤策略生成的工具定义。
1. 处理名称冲突、Server 不可用和单个 Server 启动失败，不影响其他 Server。

验收条件：

- 完整 schema 总量有明确上限；
- 16k context 模型不会仅因工具目录和控制提示挤掉输出预算；
- 所有允许的工具仍可通过目录扩展发现；
- 工具选择失败可以恢复，而不是直接结束任务。

回滚：恢复完整目录注入，但保留最小动作协议。

### 阶段 4：统一消息所有权和模型适配

目标：消除消息交替错误，并让原生和 grammar 模式共享同一运行时。

工作项：

1. 引入内部 transcript event，而不是由任务对象直接拼接 user/assistant 消息。
1. 模型适配器负责把事件序列序列化成具体 chat template。
1. 原生模式保持 assistant tool call 与 tool result 的 call ID 配对。
1. grammar 模式由适配器生成严格交替的兼容消息。
1. 上下文压缩只压缩已完成证据摘要，不删除未完成任务和活动 job ID。

验收条件：

- 固定场景和 soak test 中没有消息角色交替失败；
- 工具结果不会绑定到错误的调用；
- 压缩后仍能轮询现有 job 或引用已有资源；
- 同一个运行时测试可覆盖原生与 grammar 两种适配器。

回滚：按模型配置切回旧 transcript builder。

### 阶段 5：MCP 生命周期和安全加固

目标：补齐控制循环之外仍会影响可靠性和安全性的 Host 行为。

工作项：

1. 请求超时时先发送 `notifications/cancelled`，再结束本地 pending 状态。
1. 取消全部请求时也逐项通知仍在运行的 Server 请求。
1. MCP 子进程使用最小环境白名单；敏感变量必须在 Server 配置中显式授权。
1. 收到 `notifications/tools/list_changed` 时更新 catalog revision。
1. 活动 run 使用的工具被删除或 schema 改变时，重新验证下一调用；不能静默继续
   使用旧定义。
1. 将 annotations 明确标记为不受信任提示，不允许其覆盖本地风险和验证策略。

验收条件：

- 超时请求在 fake server 中收到 cancellation notification；
- 默认子进程环境不包含未授权 token/secret；
- 工具目录变化不会导致调用已删除工具或旧 schema；
- 单个 Server 故障不会终止不依赖该 Server 的任务。

回滚：生命周期刷新可以关闭；环境收紧必须提供显式兼容名单，不能回退为无条件
继承全部敏感变量。

### 阶段 6：灰度默认和删除旧协议

目标：验证新循环稳定后，停止维护两套模型控制协议。

工作项：

1. 对支持的模型默认启用 `host_verified`，保留用户级回退开关一个发布周期。
1. 比较完成率、无效动作率、平均决策数、prompt token 和安全终止率。
1. 修复剩余模型模板兼容问题。
1. 删除 legacy 的模型审查 prompt、grammar、parser 分支和不可达状态。
1. 更新两份旧设计文档、用户配置说明和故障排查文档。

验收条件：

- 核心场景在目标小模型上连续运行达到发布门槛；
- 没有 uncertain mutation 重放和权限绕过；
- `invalid_agent_action` 明显低于 legacy，且不存在消息交替失败；
- 初始控制提示和工具目录不超过有效上下文的 20%；
- legacy 回退周期内没有发现只能由旧强制审查保障的正确性行为。

## 9. 建议代码边界

| 模块 | 计划职责变化 |
| --- | --- |
| `AgentPromptBuilder` | 删除新模式的计划/审查动作说明；只构建最小决策和条件 reviewer 提示 |
| `AgentAction` | 增加最小 action parser/grammar；迁移完成后删除旧审查动作 |
| `ExecutionTask` | 保存目标和状态，但不再等待模型 step review；消费 Host 验证结果 |
| `PlanningTask` | 规划变为可选，产物不再包含模型必须回传的控制字段 |
| `AgentController` | 维持调度和事件路由，不解析业务语义 |
| `AgentToolRuntime` | 继续负责校验、策略、调用、取消和结果归一化；接入验证器 |
| `ToolCatalogBuilder` | 构建索引、候选完整 schema 和 revision cache |
| `McpHostRuntime` | 暴露 catalog revision，传播 list changed 事件 |
| `StdioMcpTransport` | 超时和 cancel-all 时发送 MCP cancellation notification |
| `McpServerProcess` | 实施最小环境和显式变量授权 |

建议新增的通用组件：

- `AgentTranscript`：保存与模型模板无关的对话事件；
- `EvidenceLedger`：保存不可变工具证据；
- `ToolEffectVerifier`：执行 provider-neutral 确定性验证；
- `ToolSelectionService`：选择和展开当前相关工具；
- `AgentModelAdapter`：原生 tool calling 与 grammar 模式适配。

新增组件不得包含 ShonDy 工具名、字段名或业务对象类型。业务域需要额外验证时，
应通过受信任的本地 provider contract 或可注册 verifier 提供。

## 10. 测试计划

### 10.1 单元测试

- 最小动作 parser 接受三种目标动作并拒绝多余控制字段；
- Evidence Ledger 保持 call/task/resource 关联且不可修改；
- 每类 `VerificationStatus` 转移正确；
- 修改成功但 read-back 不一致时不得完成；
- uncertain mutation 不进入自动重试；
- transcript adapter 始终生成合法角色顺序；
- catalog revision 变化后旧 schema 失效；
- 环境过滤不会泄漏未授权敏感变量。

### 10.2 集成测试

- fake MCP Server 覆盖成功、协议错误、domain error、延迟、超时、取消和 late result；
- tools/list 分页和 list_changed；
- 多 Server 并行启动及单 Server 失败隔离；
- read-back 验证与 long-running poll；
- 工具名称冲突和 schema 动态变化；
- 16k context 下的大型目录延迟加载。

### 10.3 真实模型评估

至少覆盖一个较弱模型和一个较强模型。每个核心场景重复运行至少 30 次，记录：

- 端到端完成率；
- 无效模型动作率；
- 工具参数一次通过率；
- 成功任务的模型决策次数；
- 重复工具调用次数；
- Host 自动验证调用次数；
- 条件式 reviewer 触发率；
- 首轮、峰值和总 prompt token；
- 按终止原因分类的失败数量；
- 用户审批、取消和高风险阻止是否正确。

不能只用 mock 模型输出证明新循环可靠；最终发布门槛必须由真实本地模型运行结果
决定。

## 11. 发布门槛

新模式成为默认值前，必须满足：

1. 固定核心场景不存在权限绕过、错误对象修改或 uncertain mutation 自动重放。
1. 消息角色交替错误为零。
1. 需要工具的任务不能在没有成功证据时完成。
1. 已有充分确定性证据的任务不需要模型自审即可完成。
1. reviewer 只在明确的语义验证状态触发，不能成为每步默认回合。
1. 相对 legacy，任务完成率提高且无效动作、重复调用和平均 prompt token 降低。
1. 16k context 模型保留足够输出预算，不因完整工具目录直接失败。
1. 旧模式回退开关和迁移说明已经过测试。

## 12. 推荐实施顺序

严格按以下顺序推进：

1. 指标和 feature flag；
1. Evidence Ledger；
1. 确定性 verifier；
1. shadow comparison；
1. 最小动作协议；
1. Host 内部任务状态；
1. 工具选择和延迟加载；
1. transcript/model adapter；
1. cancellation、环境和 catalog refresh；
1. 灰度默认；
1. 删除 legacy 审查协议。

不得先删除 `review_tool_call` 或 `review_plan_step`，再补验证能力。正确的迁移判据
不是“审查回合消失”，而是“所有原本依赖模型自审的安全和完成约束已经由 Host
或确定性检查工具覆盖”。
