# qtLLM 0.2.0 Agent MCP 开发计划

## 1. 版本目标

qtLLM 0.2.0 的目标是在现有本地聊天能力上增加受控的 Agent
执行循环。用户通过自然语言描述任务，本地模型选择合适的 MCP
工具，Qt 应用完成权限检查和工具调用，再将执行结果交回模型，直到模型
给出最终答复或任务被终止。

MCP 只负责工具发现和调用，不直接承担 Agent 调度。模型只能提出工具调用
请求，是否允许及如何执行始终由 Qt 应用决定。

核心流程：

```text
用户输入
  -> 模型生成结构化决策
  -> Qt 校验工具、参数和权限
  -> 必要时等待用户确认
  -> 调用 MCP 工具
  -> 将工具结果加入隐藏上下文
  -> 模型继续决策或生成最终答复
```

## 2. 版本范围

### 2.1 0.2.0 包含

- 支持通过对话选择和调用 MCP 工具。
- 支持 stdio MCP Server。
- 支持 `initialize`、`tools/list` 和 `tools/call`。
- 单个 Agent 任务最多连续调用 5 次工具。
- 只读工具可以按本地策略自动执行。
- 修改、删除、命令执行和外部发布等操作需要用户确认。
- 工具执行状态、摘要和错误可以在界面中查看。
- 用户可以取消正在进行的模型生成或 MCP 调用。
- 工具结果自动回填模型并形成最终答复。
- MCP 配置、工具白名单和风险策略保存在本地。

### 2.2 0.2.0 不包含

- 多 Agent 协作。
- 后台定时或无人值守任务。
- 并行执行多个 Agent 任务。
- MCP 插件市场及自动下载第三方 Server。
- 默认放开任意命令、任意路径或任意网络访问。
- Streamable HTTP MCP；该能力放入后续版本。

## 3. 总体架构

Agent 调度放在 Qt 主程序的 application 层。推理 worker 保持职责单一，
只负责模型加载、结构化生成、普通文本生成和取消推理，不直接连接 MCP，
也不执行文件、命令或网络操作。

```text
MainWindow
    |
    v
AgentController
    |--- AgentPromptBuilder
    |--- AgentRun / AgentEvent
    |--- ToolRegistry
    |--- ToolPolicy
    |
    |--- WorkerClient ------> qtllm-worker ------> llama.cpp
    |
    +--- McpClientManager
            +--- StdioMcpTransport ------> MCP Server
```

建议增加以下模块：

```text
app/
  application/
    include/
      AgentController.hpp
      AgentPromptBuilder.hpp
      AgentRun.hpp
      ChatController.hpp
    src/
      AgentController.cpp
      AgentPromptBuilder.cpp
      AgentRun.cpp
      ChatController.cpp

  infrastructure/
    mcp/
      include/
        McpClientManager.hpp
        McpServerProcess.hpp
        McpTransport.hpp
        StdioMcpTransport.hpp
        ToolPolicy.hpp
        ToolRegistry.hpp
      src/
        McpClientManager.cpp
        McpServerProcess.cpp
        StdioMcpTransport.cpp
        ToolPolicy.cpp
        ToolRegistry.cpp

shared/
  agent/
    include/
      AgentEvent.hpp
      ToolCall.hpp
      ToolDefinition.hpp
      ToolResult.hpp
    src/
      AgentEvent.cpp
```

## 4. Agent 状态机

每次用户任务创建独立的 `agentRunId`，模型生成请求继续使用现有
`requestId`。二者不能混用，因为一次 Agent 任务可能包含多次推理和多次
MCP 调用。

```text
Idle
  -> Deciding
  -> WaitingForApproval
  -> ExecutingTool
  -> Deciding
  -> GeneratingAnswer
  -> Completed
```

任何运行状态都可以进入：

```text
Cancelled
Failed
```

取消 Agent 时需要同时取消当前模型请求和 MCP 请求。已经取消或被新任务
替代的 `agentRunId` 收到迟到响应时，必须忽略该响应。

## 5. 结构化模型输出

Agent 决策不能通过正则表达式或模糊文本猜测。模型每一步只能生成两类
JSON 对象。

调用工具：

```json
{
  "action": "call_tool",
  "tool": "filesystem.read_file",
  "arguments": {
    "path": "D:/project/README.md"
  }
}
```

完成任务：

```json
{
  "action": "final",
  "content": "任务已经处理完成。"
}
```

worker 使用 llama.cpp grammar 约束顶层 JSON 结构。Qt 应用在执行前还要
使用工具的 JSON Schema 校验 `arguments`。JSON 解析或 Schema 校验失败
时，允许模型进行一次受限修复；再次失败则结束任务，不能猜测参数后执行。

工具调用 ID 由 Qt 应用生成，不信任模型自行生成的 ID。

## 6. IPC v3

现有 JSONL IPC 升级为版本 3。`hello` 响应增加能力声明，客户端据此判断
worker 是否支持结构化生成。

示例：

```json
{
  "protocolVersion": 3,
  "requestId": "hello-1",
  "type": "hello",
  "payload": {
    "workerVersion": "0.2.0",
    "capabilities": {
      "structuredGeneration": true,
      "grammar": true
    }
  }
}
```

`generate` 增加以下字段：

```json
{
  "responseMode": "text",
  "messages": [],
  "grammar": ""
}
```

`responseMode` 可取：

- `text`：保持现有流式聊天行为。
- `agent_action`：缓冲结构化决策，不直接显示为助手回答。

Agent 决策应设置较小的输出上限，例如 256 tokens。普通最终回答继续使用
模型 preset 的输出上限和流式显示。

## 7. MCP 客户端

0.2.0 优先实现 stdio transport，以符合当前 Windows 本地应用和离线优先
的定位。

每个 MCP Server 配置至少包含：

- 稳定的本地 `serverId`。
- 可执行程序绝对路径。
- 参数数组。
- 明确允许传递的环境变量。
- 工作目录。
- 是否启用。
- 工具白名单。
- 启动、初始化和调用超时。

启动进程时必须直接向 `QProcess` 设置程序与参数，不得拼接命令后交给
shell。Server 的 stderr 作为诊断日志读取，stdout 只处理 MCP JSON-RPC
消息。

工具名称在本地注册为 `serverId.toolName`，避免不同 Server 的工具重名。
MCP Server 断开、协议错误或异常退出时，只终止当前工具调用，不得导致
Qt 主程序或推理 worker 崩溃。

## 8. 工具策略与安全边界

MCP 提供的工具描述和工具结果都视为不可信输入。本地 `ToolPolicy` 是最终
授权来源，不能只依据 Server 声明决定是否自动执行。

默认策略：

| 操作类型 | 默认行为 |
| --- | --- |
| 读取、查询、搜索 | 允许自动执行 |
| 创建报告、导出新文件 | 展示摘要后执行 |
| 修改文件、发送网络请求 | 执行前确认 |
| 删除文件、执行命令、发布内容 | 必须明确确认 |

运行限制：

- 每个 Agent 任务最多调用 5 次工具。
- 单次 MCP 调用默认超时 30 秒。
- Agent 总运行时间默认不超过 2 分钟。
- 单个工具结果默认最多向模型传递 64 KB。
- 二进制结果不得直接写入提示词。
- 相同工具和相同参数连续调用超过两次时按循环异常终止。
- 未注册、未启用或不在白名单中的工具不得执行。
- 用户拒绝授权后，不允许模型通过其他同类工具绕过拒绝。

## 9. 对话与 Agent 历史

用户可见的聊天记录和 Agent 内部执行事件需要分开管理：

- `ConversationMessage` 保存用户可见的 user/assistant 内容。
- `AgentEvent` 保存工具请求、审批、执行结果、错误和耗时。
- `AgentPromptBuilder` 将必要的 Agent 事件转换为临时推理上下文。

第一版可以将隐藏工具交互编码为一对普通聊天模板可识别的消息：

```text
assistant: {"action":"call_tool", ...}
user: <tool_result name="filesystem.read_file">...</tool_result>
```

这些隐藏消息不得直接加入聊天 UI，也不能作为普通助手答案持久化。上下文
裁剪必须以完整工具事务为单位，不能留下缺少对应结果的工具调用。

## 10. UI 计划

主界面增加“聊天”和“Agent”模式切换。聊天模式保持现有体验，Agent 模式
展示任务执行状态。

需要提供：

- 当前状态：分析、等待确认、执行工具、整理答案、完成或失败。
- 当前工具名称及简短操作摘要。
- 危险操作确认对话框，明确展示工具、目标和关键参数。
- 可展开的执行详情，显示工具参数、耗时、结果摘要和错误。
- 统一的停止按钮，取消整个 Agent 任务。
- MCP Server 管理页面，显示连接状态和可用工具。

原始 JSON、完整工具结果和内部提示词默认不直接显示在聊天正文中。

## 11. 分阶段开发计划

### 阶段 1：应用层重构

工作内容：

- 将当前 `MainWindow` 中的会话组织和生成控制移入 application 层。
- 增加 `ChatController`，确保现有聊天功能保持不变。
- 定义 `AgentController`、`AgentRun` 和 `AgentEvent` 基础接口。

验收标准：

- 普通聊天、流式输出、取消和多轮上下文测试全部通过。
- `MainWindow` 不再直接拼接模型请求历史。
- 尚未启用 MCP 时，应用行为与 0.1.0 一致。

预计：2 个工作日。

### 阶段 2：结构化推理与 IPC v3

工作内容：

- 升级 JSONL IPC 到版本 3。
- `hello` 增加能力协商。
- `generate` 增加 `responseMode` 和 grammar 支持。
- 增加 Agent Action JSON 解析和验证。

验收标准：

- 模型只能生成合法的 `call_tool` 或 `final` 顶层结构。
- 非法 JSON 不会触发任何外部操作。
- 旧版 client/worker 组合能明确报告协议不兼容。

预计：3 个工作日。

### 阶段 3：stdio MCP 客户端

工作内容：

- 实现 MCP JSON-RPC 请求和响应关联。
- 实现 Server 启动、初始化、工具发现、工具调用和关闭。
- 实现超时、取消、stderr 日志和异常退出处理。
- 建立 MCP Server 本地配置格式。

验收标准：

- 可以连接测试 MCP Server 并列出工具。
- 可以调用工具并获得结构化结果。
- Server 崩溃不会导致主程序退出。
- 超时和取消后不会接受迟到结果。

预计：4 个工作日。

### 阶段 4：工具注册与权限策略

工作内容：

- 实现工具命名空间、白名单和启用状态。
- 接入 JSON Schema 参数验证。
- 实现工具风险分类和确认策略。
- 限制输出大小、执行时间和可访问范围。

验收标准：

- 未注册工具、错误参数和越权调用全部被拒绝。
- 危险工具未经确认不能执行。
- 用户拒绝后 Agent 正确结束或调整方案，不会绕过策略。

预计：3 个工作日。

### 阶段 5：Agent 执行循环

工作内容：

- 串联模型决策、审批、MCP 调用、结果回填和最终回答。
- 增加最大步数、总超时、重复调用检测和错误恢复。
- 实现完整 Agent 取消语义。
- 分离可见对话历史和隐藏执行历史。

验收标准：

- 能完成至少包含两次工具调用的任务。
- 工具错误能交回模型，由模型解释或选择安全的替代方案。
- 达到最大步数或检测到循环时明确终止。
- 最终对话中不出现内部 JSON 或未经处理的工具输出。

预计：4 个工作日。

### 阶段 6：Agent UI

工作内容：

- 增加聊天/Agent 模式切换。
- 增加 Agent 状态和工具执行详情。
- 增加权限确认对话框和 MCP Server 管理界面。
- 将停止按钮扩展为取消整个 Agent 任务。

验收标准：

- 用户可以清楚看到软件正在执行什么操作。
- 确认窗口能显示准确的目标和参数。
- UI 在模型推理和 MCP 调用期间保持响应。

预计：3 个工作日。

### 阶段 7：测试、打包与文档

工作内容：

- 增加可控的 Fake MCP Server 测试程序。
- 增加协议、状态机、安全策略、异常和 UI 测试。
- 验证 MCP 子进程随应用正确启动和退出。
- 补充 MCP 配置、权限和故障排查文档。
- 生成 0.2.0 CPU 与 CUDA 测试安装包。

验收标准：

- 默认测试不依赖网络和真实第三方 MCP Server。
- CPU 与 CUDA 包均可完成 Agent 验收场景。
- 卸载或升级不会删除用户的 MCP 配置和执行记录。

预计：4 个工作日。

单人全职开发总计约 3 至 4 周。

## 12. 测试重点

- MCP 初始化、协议版本不兼容和初始化超时。
- `tools/list` 和 `tools/call` 的正常、错误及乱序响应。
- Server 输出非法 JSON、混入普通 stdout 文本或中途退出。
- 模型输出非法 JSON、未知工具或不符合 Schema 的参数。
- 工具描述或结果中的提示注入。
- 用户同意、拒绝、关闭确认框和等待超时。
- 推理期间取消、工具执行期间取消和任务切换。
- 连续多步工具调用、重复调用和死循环检测。
- 工具结果截断、超大结果和二进制内容。
- Agent 失败后普通聊天仍然可用。
- 工具调用不会污染正常对话历史。

## 13. 0.2.0 验收场景

用户输入：

> 检查这个项目里的 TODO，读取相关文件，然后给我整理一份实施建议。

预期行为：

1. 模型选择允许的文件查询与读取工具。
1. Qt 应用根据只读策略自动执行工具。
1. 工具结果作为隐藏上下文交回模型。
1. 模型根据实际文件内容生成实施建议。
1. UI 展示简洁的执行过程和最终答复。

涉及修改的场景：

> 根据建议修改配置文件。

预期行为：

1. 模型提出具体工具调用。
1. Qt 应用展示将要修改的文件、操作和关键参数。
1. 用户确认后才执行修改。
1. 用户拒绝时不得执行，也不得通过其他工具绕过拒绝。
1. 执行结果返回模型，由模型说明完成情况或失败原因。

## 14. 后续版本候选

- Streamable HTTP MCP transport。
- MCP Server 配置导入、导出和健康检查。
- 基于任务的工具集合筛选，减少本地模型上下文占用。
- 可恢复的 Agent 运行记录和 SQLite 会话持久化。
- 文件修改预览、差异展示和可回滚操作。
- 后台任务、计划任务和人工接管机制。
- 针对不同模型的工具调用模板和能力评测。
