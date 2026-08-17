# qtLLM MCP Host 改进工作计划

## 1. 文档状态与目标

- 状态：实施中；阶段 0 至阶段 2 已于 2026-08-17 完成，下一步为阶段 3。
- 基线日期：2026-08-17。
- qtLLM 基线提交：`0432f45a0642aaafb9293d6d242c00907dfc9d3e`。
- 参考实现：Pi Agent Harness，提交
  `d3ab2af969d64997338253c9151190aa1bc33580`。
- 产品定位：qtLLM 是通用 MCP Host，不是 coding agent。
- 核心目标：让本地模型安全、稳定地发现和使用不同来源、不同能力集合的 MCP
  Server，而不是在 Qt 主进程中增加一套代码或文件操作工具。

本文是 `docs/agent-mcp-development-plan.md` 完成后的增量计划。旧文档记录
0.2.0 的建设过程；后续 MCP 实现方向以本文和当前 `readme.md` 为准。

## 2. 结论摘要

### 2.1 Pi 内置工具和 qtLLM MCP 不是同一种架构

Pi 的 `read`、`write`、`edit`、`grep`、`bash` 等工具与 qtLLM filesystem MCP
在用户可见功能上有重叠，但执行边界不同：

| 对比项 | Pi 内置工具 | qtLLM MCP 工具 |
| --- | --- | --- |
| 注册方式 | Agent Harness 进程内注册 `AgentTool` | MCP Server 通过 `tools/list` 发布 |
| 调用方式 | Harness 直接调用 TypeScript 工具函数 | Host 通过 JSON-RPC 调用 Server |
| 文件/命令执行 | Node.js 文件 API 或子进程 | 独立 MCP Server 进程 |
| 结果格式 | Pi 自有 tool result 类型 | MCP content blocks、`structuredContent` 和错误语义 |
| 权限边界 | Pi 的 hook、扩展和运行环境策略 | qtLLM 本地 `ToolPolicy` 最终授权 |
| 复用方式 | 可替换 operations 后端，但不是 MCP 协议 | 任意兼容 Server 可通过 transport 接入 |

因此不把 Pi 的文件工具复制进 qtLLM，也不把文件或命令执行迁入 Qt 主进程。
Pi 只用于借鉴 Agent 循环、事件、取消、结果截断和测试分层。filesystem MCP
继续作为参考 Server，而不是产品能力边界。

### 2.2 这些改动会增加什么能力

会增加 Agent 的外部能力，但不会直接提高模型本身的推理水平：

- 能力广度：可接入更多标准 MCP Server，以及 Tools、Resources、Prompts、Roots
  等不同 MCP 能力。
- 执行可靠性：分页、动态目录、取消、超时、丰富结果和单 Server 故障隔离能减少
  “工具存在但不可用”或结果丢失。
- 安全可控性：Server 声明和内容始终是不可信输入，最终授权仍由本地策略和用户
  决定。
- 可诊断性：用户能够分辨模型决策失败、Host 协议失败、Server 失败和工具业务
  失败。

Agent 的实际上限仍同时受本地模型的工具选择能力、上下文容量、MCP Server 质量
和授权策略影响。MCP Host 扩展的是“能连接和可靠执行什么”，不是模型智力。

## 3. 当前基线与缺口

### 3.1 已具备

- `McpClientManager` 已支持多个配置项，并完成 stdio Server 的启动和停止。
- 已支持 `initialize`、`notifications/initialized`、`tools/list` 和 `tools/call`。
- `ToolRegistry` 保存带 `serverId` 命名空间的工具，并在调用前校验参数。
- `ToolPolicy` 独立于 Server，未知工具默认不自动信任。
- `StdioMcpTransport` 已实现请求超时、本地取消、迟到响应忽略、stderr 诊断和
  1 MiB 单消息保护。
- 内置 filesystem Server 以独立 C++20 子进程运行，并限制授权根目录。
- Fake MCP Server 已覆盖基础发现、调用、远端错误、超时和取消。
- `McpHostRuntime` 已通过抽象 transport 管理逐 Server 生命周期、pending request、
  状态和能力快照；`McpClientManager` 保留为兼容 facade。
- Server 停止或失败时只撤销自身能力；目录刷新失败保留上一份有效工具快照并进入
  `Degraded`。
- Tools 已支持 cursor 分页、动态目录合并刷新、output schema、annotations、丰富
  结果、类型化通知限流和协议取消。

### 3.2 关键缺口

1. Host 只能处理 Server response/notification，不能路由带 id 的 Server 发起请求，
   因而无法支撑 Roots、sampling 或用户输入请求。
1. Server instructions 会进入 Agent 上下文，但缺少独立的来源标记、启用策略和
   prompt injection 边界。
1. MCP 配置主要面向 stdio；尚无 Streamable HTTP、认证、凭据引用和网络域名
   策略。
1. 测试集中在单个基础 Fake Server，尚未形成按协议能力和故障类型组织的
   conformance matrix。

## 4. 范围边界

### 4.1 本计划必须完成

- 通用 Host 生命周期、协议版本协商和 capability model。
- 多 Server 注册、隔离、命名空间、状态和原子能力快照。
- Tools 完整兼容：发现、分页、动态更新、调用、结果、取消和诊断。
- 本地权限策略、数据大小限制、来源标记和不可信内容边界。
- 不依赖真实模型的 MCP conformance 测试套件。
- 面向用户的 Server 配置、状态、能力和故障诊断。

### 4.2 分里程碑完成

- Resources、Prompts 和 Roots。
- Streamable HTTP、认证和凭据管理。
- Server 发起的 sampling、用户输入请求及其他需要反向请求的能力。

### 4.3 不属于 MCP Host 主线

- 在 Qt 主进程内新增 `read`、`edit`、`bash` 等内置工具。
- 复制 Pi 的 TypeScript 工具、扩展运行时或 coding-agent 产品功能。
- 多 Agent、子 Agent、分支会话、skills 或代码生成专用工作流。
- SQLite 对话持久化、会话 UI 重构和模型推理算法升级。

这些功能可以独立规划，但不能阻塞 MCP Host 的协议兼容性。根据当前产品决策，
记忆功能、SQLite 对话持久化和会话恢复均暂停，不纳入本计划后续版本。

## 5. 目标架构

```text
MainWindow / AgentController / future workflow consumers
                         |
                         v
                   McpHostFacade
                         |
         +---------------+----------------+
         |               |                |
         v               v                v
  ServerRegistry   CapabilityIndex   InvocationRouter
         |               |                |
         |               +-------> ToolPolicy
         |                                |
         +---------> McpHostRuntime <-----+
                         |
          +--------------+---------------+
          |              |               |
          v              v               v
  StdioTransport  StreamableHttp   future transport
          |              |
          v              v
      MCP Server      MCP Server

McpHostRuntime ---> ContentNormalizer ---> bounded Host events / diagnostics
```

### 5.1 `McpHostFacade`

为 Agent 和 UI 提供稳定、与 transport 无关的接口：

- 查询 Server 状态和已协商能力。
- 获取不可变的 Tools、Resources 和 Prompts 快照。
- 发起工具调用、资源读取、Prompt 获取和取消。
- 订阅有界、已脱敏、带 `serverId` 和 request id 的事件。

消费者不能拿到 `QProcess`、HTTP client 或具体 transport 指针。

### 5.2 `McpHostRuntime`

负责协议和生命周期，不承担模型决策：

- 执行启动、initialize、initialized、Ready、停止和重连流程。
- 校验协商版本并保存 client/server capabilities、server info 和 instructions。
- 路由 Host 请求、Server response、notification 和 Server 发起请求。
- 对每个 Server 独立维护 pending requests、超时、取消和健康状态。
- 在协议层和消费层之间规范化内容、错误和事件。

### 5.3 `ServerRegistry` 与 `CapabilityIndex`

- Server 状态至少包含 `Stopped`、`Starting`、`Initializing`、`Ready`、
  `Degraded`、`Failed` 和 `Stopping`。
- 所有能力按 `serverId` 分区；同名工具使用 `serverId.toolName` 隔离。
- 能力刷新先构建并校验新快照，全部成功后原子替换。
- Server 删除、崩溃或禁用时，只撤销该 Server 的能力和 pending requests。
- 快照带 revision；Agent 一次决策使用固定 revision，避免调用时目录漂移。

### 5.4 Transport 边界

统一接口至少覆盖：

```text
start / stop
sendRequest / sendResponse / sendNotification
messageReceived
transportStateChanged
cancelLocalWait
```

`McpHostRuntime` 只依赖抽象 transport。stdio 实现继续保持 stdout 仅 JSON-RPC、
stderr 仅诊断；Streamable HTTP 后续复用相同消息路由和状态机。

## 6. MCP 能力路线

### 6.1 P0：Tools 和 Host 核心

1. 建立明确的协议兼容矩阵。保留现有 `2024-11-05` 兼容路径，并只在测试覆盖后
   增加新的协议版本；不使用“无条件接受 Server 返回版本”的宽松策略。
1. initialize 保存 protocol version、server info、instructions 和 capabilities；
   未进入 `Ready` 前拒绝能力调用。
1. 支持 `tools/list` cursor 分页。只有全部页面成功、工具名和 schema 校验通过后
   才替换快照。
1. 处理工具目录变化 notification；刷新失败时保留上一有效快照并将 Server 标记
   为 `Degraded`。
1. `ToolDefinition` 完整保存 input schema、可选 output schema、annotations 和来源
   Server。annotations 只作 UI 风险提示，不是授权依据。
1. 统一表示文本、图片、音频（协议版本支持时）、嵌入资源、资源链接和
   `structuredContent`；未知 block 显式标记，不静默丢失。
1. `structuredContent` 支持任意合法 JSON value，至少完整覆盖 object 和 array；
   output schema 校验失败是可诊断协议问题，不覆盖 Server 的原始错误语义。
1. progress、logging 和已声明 notification 进入有界事件队列。未知 notification
   安全忽略并记录，notification 风暴按 Server 限速。
1. 取消先结束本地等待，再按协商能力发送协议取消通知；迟到结果按 request id
   丢弃，不能交给 Agent。
1. 一个 Server 的超时、非法消息、stdout 污染、崩溃或重启不能影响其他 Server、
   模型 worker 或 Qt UI。

### 6.2 P1：Resources、Prompts 和 Roots

- Resources：支持发现、模板、分页、读取、订阅和更新通知；按 MIME type、大小和
  URI scheme 做呈现及安全限制。
- Prompts：支持发现、分页、参数校验和获取；必须由用户或上层工作流显式选择，
  Server 不能静默改写本地 system prompt。
- Roots：只返回用户明确授权给该 Server 的根；工作区选择不自动授权所有 Server，
  Server 也不能通过路径别名扩大范围。
- completion 等关联能力在 Resources/Prompts 基础模型稳定后按协议矩阵增加。

### 6.3 P2：Streamable HTTP 和认证

- 增加 Streamable HTTP transport，不在 Manager 中复制一套 Tools/Resources 逻辑。
- 对连接、会话、断线恢复、重定向、代理和证书错误定义明确状态。
- 凭据配置使用安全存储引用，不把 token 明文写入普通 MCP 配置或日志。
- 网络 Server 使用域名 allowlist、TLS 要求、重定向限制和每 Server 带宽上限。
- OAuth 或其他认证流程必须在 UI 中显示目标 Server、权限范围和凭据归属。

### 6.4 P3：Server 发起请求

- 新增 `InboundRequestRouter`，区分 response、notification 和带 id 的 Server request。
- sampling 请求默认拒绝；启用后仍由本地模型选择、预算、内容过滤和用户策略控制，
  Server 不能直接控制模型或继承 Host 的其他 Server 权限。
- 用户输入请求必须显示来源 Server、请求原因、字段和敏感性，不允许后台静默获取。
- 每类反向请求独立配置并设置递归深度、次数、超时和取消上限，防止 Server 形成
  Host -> Server -> Host 的无限调用链。

## 7. 安全模型

### 7.1 授权优先级

```text
本地禁止规则
  > 用户对 Server/能力的启用状态
  > 本地 ToolPolicy 和单次审批
  > MCP Server annotations / instructions
```

Server 自报 read-only、idempotent 或 safe 不能自动获得执行权。第三方未知工具和
命令执行类工具默认要求审批，用户可以禁用或移出 allowlist。

### 7.2 不可信输入

- 工具描述、Server instructions、Prompt、Resource 和工具结果均携带来源标记。
- Server instructions 不直接拼接为本地最高优先级 system prompt；默认作为受限的
  Server 上下文，可由用户查看和禁用。
- JSON schema 需要深度、属性数量、字符串长度和总字节数上限。
- content block 按类型和字节数限制；截断必须生成结构化元数据，不能产生半个 JSON。
- 日志、事件和 UI 摘要统一脱敏，不记录环境变量值、认证头或完整敏感 payload。

### 7.3 filesystem MCP 定位

- 继续作为 stdio、能力发现、授权、调用、取消、结果限制和进程隔离的参考 Server。
- 文件读写始终发生在独立 Server 中，不进入 Qt 主进程。
- 授权根由 Host 明确传入，Server 自己再做 canonical path 和越界校验。
- `read`、`grep`、`edit` 或 `bash` 是否齐全，不作为通用 MCP Host 的完成标准。
- 不随内置 filesystem Server 提供任意命令执行；第三方 command MCP 可以接入，但
  必须服从本地高风险策略。

## 8. 分阶段实施

### 阶段 0：协议基线与防护（已完成：2026-08-17）

- 固化当前 stdio 行为和 `2024-11-05` 回归用例。
- 建立 initialize/protocol/capability 领域类型和兼容矩阵文档。content block
  与统一 error 类型在阶段 2 结果规范化时一并落地，避免提前引入未使用类型。
- 修复工具目录按字符串截断导致非法 JSON 的风险，改为完整工具级预算。
- 扩展 Fake Server 脚本，使测试场景可按 capability 和故障开关组合。

验收：现有 Server 行为不变；工具目录始终为合法 JSON；后续协议功能可在不加载
模型的情况下测试。

完成记录：

- `ToolCatalogBuilder` 已按稳定顺序和完整工具定义执行字节预算，不再截断序列化
  后的 JSON。
- initialize 结果现在校验协议版本、Server 信息和 capability 对象，并保留协商
  结果。
- 未声明 Tools capability 或返回不兼容协议版本的 Server 会在 Host 边界被拒绝。
- Fake Server 已支持协议版本和 Tools capability 开关；MCP 与 Agent Controller
  定向测试通过。

### 阶段 1：Host Runtime 与多 Server 生命周期（已完成，2026-08-17）

- 以 `McpClientManager` 作为兼容 facade，新增 `McpHostRuntime`、
  `McpServerRegistry` 和 Server 状态机。
- 将 Manager 对 `StdioMcpTransport` 的具体依赖收敛到 transport factory。
- 严格校验 initialize 协商结果，保存完整 capabilities 和来源信息。
- 实现每 Server pending request、故障隔离、受控重启和原子能力撤销。

验收：两个 Server 可独立启动、失败和恢复；未 Ready 的 Server 不可调用；一个
Server 崩溃不影响另一个 Server 或普通聊天。

完成记录：

- runtime 只依赖 `McpTransport` 和 transport factory，stdio 实现已收敛到默认工厂。
- 状态快照包含协议版本、capabilities、Server 信息、instructions、工具数、revision
  和最近错误，并通过 facade 对外发布。
- 停止、失败和移除会撤销对应 Server 的工具和能力；失败刷新保留上一份有效目录。
- fake transport 回归覆盖正常状态序列、停止撤销、重启、多 Server 同名工具隔离、
  单 Server 崩溃隔离和非法状态跳转。

### 阶段 2：Tools conformance（已完成，2026-08-17）

- 实现分页、动态目录、快照 revision、output schema 和 annotations。
- 引入通用 content block 与 JSON `structuredContent`。
- 接入 progress/logging notification、协议取消和迟到结果过滤。
- 完成工具调用错误分类：本地校验、授权拒绝、transport、协议和业务错误。

验收：分页和动态刷新不暴露半完成目录；同名工具按 Server 隔离；丰富结果不被
静默丢失；本地策略始终覆盖 Server annotations。

完成记录：

- `tools/list` 按 cursor 收集全部页面后原子替换；重复 cursor、重复工具或任一页
  失败均拒绝新快照并保留上一版。
- `notifications/tools/list_changed` 仅在已声明 capability 时刷新，并将刷新期间的
  通知风暴合并为一次后续刷新。
- `ToolDefinition` 保留 output schema 和 annotations；成功结果执行 output schema
  校验，校验错误独立诊断，不覆盖 Server 原始业务语义。
- `ToolResult` 保留任意合法 `structuredContent`、原始 content blocks 和未知 block
  类型，并区分 transport、protocol、Server 与工具业务失败。
- progress/logging notification 通过类型化信号发布，并按 Server 限制为每秒 100
  条；本地取消同时发送 `notifications/cancelled`，迟到响应继续丢弃。

### 阶段 3：MCP 控制面与诊断（3 至 5 个工作日）

- UI 展示每个 Server 的状态、transport、协议版本、能力、工具数量和最近错误。
- 支持启用、禁用、重启、刷新能力及按 Server 查看脱敏日志。
- 将 Server instructions、工具风险、allowlist 和授权根显示为独立配置。
- 为错误提供可操作分类，不把所有失败折叠为“工具失败”。

验收：用户不看原始 JSON 也能判断 Server 是否就绪、具备什么能力、为什么失败；
状态变化不阻塞 UI。

### 阶段 4：Resources、Prompts 与 Roots（4 至 6 个工作日）

- 实现三个独立 capability registry 和分页/更新语义。
- 增加 Resource 查看器、Prompt 显式选择和逐 Server Roots 授权。
- 对 MIME、URI、大小、订阅频率和 prompt 注入边界增加测试。

验收：第三方 Server 无需修改 qtLLM 即可提供资源和 Prompt；未授权 Server 不能
获得 Roots；Prompt 不会静默提升为本地 system 指令。

### 阶段 5：Streamable HTTP 与认证（5 至 8 个工作日）

- 实现 transport、连接状态、认证和安全凭据引用；会话恢复按当前产品决策暂停。
- 增加 TLS、域名、重定向、代理、离线和限流故障矩阵。

验收：同一套 Host conformance 用例可运行于 stdio 和 HTTP；网络凭据不进入普通
配置、日志或 Agent 上下文。

### 阶段 6：Server 发起请求（4 至 6 个工作日）

- 实现 inbound request 路由、默认拒绝和显式授权 UI。
- 分别接入 sampling 和用户输入请求，增加预算、递归和取消保护。

验收：未启用能力得到标准错误响应；启用后每次敏感操作可追溯且不能继承无关
Server 权限；递归调用有确定性上限。

### 阶段 7：发布级 conformance（2 至 4 个工作日）

- 建立多 Server、多版本、多 transport 和故障组合矩阵。
- 对 CPU/CUDA 包执行无网络 stdio 验收，并对 HTTP 使用本地 Fake Server 验收。
- 更新配置迁移、隐私、故障排查和第三方 Server 兼容说明。

验收：默认 CTest 不依赖真实模型或公网；安装升级保留 MCP 配置和权限；MCP、
worker 任一单点故障不导致主窗口崩溃。

核心交付为阶段 0 至 3，单人全职预计 11 至 18 个工作日。完整完成阶段 0 至 7
预计 26 至 42 个工作日。阶段 4、5 可在 Host Facade 稳定后并行开发。

## 9. 测试矩阵

### 9.1 生命周期和 JSON-RPC

- initialize 顺序、兼容和不兼容版本、capabilities、instructions、initialized。
- request/response id 类型、乱序、重复、未知和迟到 response。
- 合法/非法 notification、Server request、JSON-RPC error 和 message size。
- 启动失败、超时、正常退出、崩溃、重启、禁用和删除 Server。

### 9.2 Tools

- 单页、多页、空页、坏 cursor、重复名称、中途失败和原子快照。
- list changed、刷新合并、刷新失败及上一有效快照保留。
- input/output schema、annotations、超大 schema 和本地策略优先级。
- 文本、图片、资源、资源链接、结构化对象/数组和未知 content block。
- progress、logging、notification 限流、取消、超时和迟到结果。

### 9.3 Resources、Prompts 与 Roots

- 各能力的单页/分页发现、动态更新和 capability 未声明时的拒绝。
- Resource URI、MIME、大小、订阅/退订和更新风暴。
- Prompt 参数、内容来源、显式选择和注入隔离。
- 零 Roots、多个 Roots、逐 Server 授权、撤销和路径别名越界。

### 9.4 Transport 与安全

- stdio stdout 污染、stderr 风暴、半行、超长行和进程树退出。
- HTTP 断线、重连、TLS、重定向、认证失败、限流和离线。
- 日志脱敏、凭据不落普通配置、payload 上限和每 Server 故障隔离。
- filesystem Server 授权根、allowlist、越界拒绝和修改操作审批回归。

## 10. Pi 借鉴项与非借鉴项

可借鉴：

- `agentLoop` 的独立事件流和 tool call 生命周期。
- 工具参数校验失败后形成可解释结果，而不是让执行层崩溃。
- AbortSignal、子进程树取消、输出累积和显式截断元数据。
- Harness 使用 fake tool/model 的确定性测试分层。

不借鉴：

- 将文件、编辑或 shell 工具作为 Host 进程内默认能力。
- 将 coding tool 名单当作 MCP Host 的能力清单。
- 直接采用 Pi 的 TypeScript runtime、扩展权限或工作目录信任模型。
- 为了复制 coding-agent 体验而推迟 MCP 协议兼容工作。

## 11. 风险与应对

| 风险 | 应对 |
| --- | --- |
| Server capabilities 或 annotations 夸大安全性 | 只作提示，授权始终由本地策略决定 |
| Server instructions 或 Prompt 注入本地指令 | 来源分层、默认受限、显式选择，不进入最高优先级 system prompt |
| 动态能力过多挤占模型上下文 | 原子快照、完整条目级预算、稳定筛选和 revision |
| notification 风暴拖慢 UI | 每 Server 限速、事件合并和有界诊断队列 |
| 图片或资源占用过多内存 | content 类型检查、字节上限和显式截断 |
| 一个 Server 故障扩散 | pending/state/queue 按 Server 隔离，transport 不共享可变状态 |
| 网络 MCP 扩大攻击面 | TLS、域名策略、凭据安全存储、重定向和带宽限制 |
| sampling 形成越权或无限递归 | 默认关闭、显式授权、独立预算和递归深度上限 |
| 协议版本持续演进 | 明确兼容矩阵、版本化 codec 和 Fake Server conformance |

## 12. 完成定义

核心 MCP Host 达到以下条件才视为可发布：

- 第三方 stdio MCP Server 无需修改 qtLLM 代码即可完成协商、发现、授权和调用。
- 多 Server 生命周期、状态、能力和故障彼此隔离。
- Tools 分页、动态更新、重名隔离、取消和丰富结果均有 conformance 覆盖。
- Host 保存并执行已协商 capabilities，不调用 Server 未声明的能力。
- Agent 只通过 `McpHostFacade` 消费工具，不直接依赖 filesystem 或 transport 实现。
- `ToolPolicy` 始终拥有最终授权权力，Server annotations 不可绕过本地规则。
- filesystem MCP 仍是受根目录和本地审批保护的参考 Server。
- 所有原始 MCP 内容有来源和大小边界，日志及 UI 不泄漏凭据和敏感 payload。
- 一个 Server 的崩溃、超时、非法消息或通知风暴不影响其他 Server 和 Qt UI。
- 默认测试不加载真实模型、不依赖公网，并覆盖协议兼容矩阵。

完整 MCP 支撑还要求 Resources、Prompts、Roots 和 Streamable HTTP 里程碑通过各自
验收。sampling 和用户输入请求只有在单独安全评审通过后才作为默认可用能力。
