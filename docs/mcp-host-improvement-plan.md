# qtLLM MCP Host 后续工作计划

## 文档规则

- 本文只记录尚未完成的工作；完成项直接删除。
- 当前已实现能力以
  [`mcp-protocol-compatibility.md`](mcp-protocol-compatibility.md) 为准。
- 产品目标是通用、离线优先的 stdio MCP Host，不是 coding agent。
- HTTP、认证、网页搜索和记忆功能不进入当前路线。

## 实施顺序

1. P0：测试基线全绿。
1. P1：stdio 第三方兼容与剩余协议能力。
1. P2：真实模型 Agent + MCP 验收。
1. P3：Server 发起请求。
1. P4：离线发布级 conformance。

P0 至 P2 完成后，当前 stdio MCP Host 才能视为稳定可用。P3 涉及新的授权
边界，必须单独评审，不能阻塞 P0 至 P2。

## P0：测试基线全绿

### 工作项

- 修复 `placesModelControlsInComposerAndMergesPrimaryAction` 对本地持久化状态的
  隐式依赖，使 UI 测试在全新配置目录中稳定运行。
- 确保 CPU 和 CUDA 构建目录均可完成完整 CTest，不保留预期失败。
- 将测试产生的配置、日志和 worker 进程限制在隔离的临时目录中。

### 验收

- CPU 与 CUDA 完整构建通过。
- 默认 CTest 全绿，不需要真实模型、网络或用户配置。
- 连续运行测试不会残留 worker 或 MCP Server 进程。

## P1：stdio 第三方兼容与剩余协议能力

### Completion

- 实现 `completion/complete` 请求、结果解析、大小限制和取消。
- 仅在协商版本和 Server `completions` capability 同时允许时调用。
- Completion 结果保留 `hasMore` 等协议字段，不自动写入 Agent 上下文。
- 在 MCP 控制面为 Prompt 参数和 Resource template 参数提供显式补全入口。

### Logging level

- 实现 `logging/setLevel`，日志级别按 Server 独立保存。
- Server 未声明 logging capability 时拒绝调用，不发送探测请求。
- 控制面允许选择级别并显示生效或失败状态。
- 日志仍执行现有的限流、长度限制和敏感信息脱敏。

### Server instructions 边界

- 为 instructions 增加明确的 `serverId` 来源标记和大小预算。
- 每个 Server 可独立启用或禁用 instructions。
- instructions 只能作为不可信上下文，不能覆盖本地 ToolPolicy、授权根或系统
  安全规则。
- UI 显示实际进入 Agent 上下文的 instructions 来源。

### 第三方 stdio 兼容矩阵

- 建立 native、Python 和 Node 风格 Server 的离线 fixture。
- 覆盖可执行文件路径、参数引用、工作目录、环境变量 allowlist 和 Windows
  `.cmd`/包装脚本边界。
- 覆盖 CRLF、分段 JSON、启动噪声、stderr 风暴、超长消息、异常退出、超时和
  进程树关闭。
- 覆盖 `2025-06-18`、`2025-03-26` 和 `2024-11-05` 的能力差异。
- 形成第三方 Server 配置与故障排查文档，但默认测试不下载或启动联网 Server。

### 验收

- Completion、logging level 和 instructions 均有 Fake transport 与真实 stdio
  测试。
- 任一第三方 Server 的异常不会撤销其他 Server 的能力或阻塞 Qt UI。
- Host 本身不发起 HTTP 请求，不需要公网、凭据或真实模型。

## P2：真实模型 Agent + MCP 验收

### 模型矩阵

- 使用一个轻量模型验证低延迟路径，例如 9B Q8。
- 使用一个较大模型验证复杂工具决策，例如 27B Q4_K_M。
- CUDA 包验证自动多卡和自定义 Layer Split；CPU 包验证无 CUDA 依赖路径。

### 场景

- 发现 filesystem MCP 工具并读取多个文件后给出综合答案。
- 连续执行创建、编辑、移动和再次读取，最终答案与文件状态一致。
- 分别验证 Allow once、Always allow、Deny 和 Stop。
- 验证工具超时、Server 退出、无效参数、超大结果和晚到响应。
- 验证模型输出无效 Agent Action 后能够纠错，且不会绕过本地校验。
- 验证长任务压缩上下文后仍保留已完成步骤和必要工具证据。

### 记录

- 模型、量化、上下文长度、CPU/GPU 放置和 MCP Server 版本。
- 首次工具选择成功率、多步骤任务完成率、平均纠错次数和取消响应时间。
- 模型加载、首 token、工具往返和最终答案总耗时。

### 验收

- 两种模型均能完成只读、多步骤修改和拒绝/取消场景。
- 失败必须归类为模型决策、Host 协议、授权、Server 或工具业务错误。
- 工具结果、诊断和最终答案不泄漏已脱敏字段。

## P3：Server 发起请求

### Sampling

- 实现 `sampling/createMessage` 的解析、默认拒绝和逐 Server 显式启用。
- Sampling 使用独立的上下文、token、时间和并发预算。
- Sampling 只调用模型生成，不进入 Agent 工具循环，避免递归工具调用。
- 限制递归深度和同一 Server 的并发请求，支持取消并忽略晚到结果。
- UI 展示请求来源、摘要、预算和审批结果。

### 用户输入

- 实现当前协商版本对应的用户输入请求，并默认关闭。
- 仅支持经过本地 schema 校验的有限表单字段。
- 密码、令牌和其他敏感输入不得进入普通日志或 MCP 配置文件。
- Server 不能继承其他 Server 的授权、roots 或用户输入结果。

### 验收

- 未启用能力时返回标准 JSON-RPC 错误。
- 启用后每次敏感请求都可追溯、可取消且受确定性预算限制。
- 恶意递归、提示注入、超大请求和审批后 Server 退出均有回归测试。

## P4：离线发布级 conformance

### 工作项

- 建立多 Server、多协议版本和多故障组合测试矩阵。
- 在 CPU 与 CUDA 安装目录中运行无网络 stdio 验收。
- 验证升级保留 Server 配置、工具 allowlist、授权 roots 和逐 Server 设置。
- 验证应用、worker 或 MCP Server 崩溃不会带崩其他进程，并能清理子进程。
- 完成第三方 Server 配置迁移、隐私边界和故障排查说明。
- 发布包继续排除模型权重、NVIDIA CUDA DLL、用户配置和测试凭据。

### 验收

- 安装后的 CPU/CUDA 包通过同一套离线 MCP conformance。
- 默认测试不加载真实模型、不访问公网、不依赖开发机绝对路径。
- 任一 Server 的崩溃、超时、非法消息或通知风暴不影响其他 Server 和 Qt UI。

## 明确暂缓

- Streamable HTTP、SSE、HTTP 认证、OAuth 和 Host 级网络凭据管理。
- 网页搜索 MCP 和任何 Host 内置联网能力。
- SQLite 会话持久化、长期记忆和应用重启后的对话恢复。
- 在 Qt 主进程内增加 `read`、`edit`、`bash` 等内置工具。
- 多 Agent、子 Agent、skills 和 coding-agent 专用工作流。
- 其他推理后端和非 GGUF 模型格式。

暂缓项只有在产品范围明确变化后才能重新进入计划。
