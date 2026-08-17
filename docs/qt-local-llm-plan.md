# Qt 本地大模型产品开发计划

## 1. 产品目标

开发一个面向 Windows 10/11 x64 的本地 AI 桌面助手：

- 使用 Qt 6 Widgets 和 C++20。
- 集成 DeepSeek 系列本地模型，断网可用。
- 支持流式输出、中止生成和多轮对话。
- 聊天数据仅保存在本机。
- 支持 CPU，并逐步支持 NVIDIA、AMD 和 Intel GPU。
- 应用与模型独立安装、升级和卸载。
- 一个模型包只包含一个逻辑模型；框架可以管理多个独立模型包。
- 支持指定外部模型库目录，也支持直接选择兼容的 GGUF 文件。
- 架构和许可证满足后续闭源商业发布需要。

首版不做模型训练、账号系统、联网搜索、Agent、插件市场和复杂知识库。

## 2. 首版技术基线

| 项目 | 选型 |
| --- | --- |
| 操作系统 | Windows 10/11 x64 |
| 开发语言 | C++20 |
| UI | Qt 6 Widgets |
| 构建 | CMake + MSVC 2019/v142（CUDA 11.8） |
| 推理引擎 | llama.cpp，固定到明确版本或提交 |
| 推荐验证模型 | DeepSeek-R1-Distill-Qwen-7B |
| 模型格式 | GGUF |
| 推荐量化 | Q4_K_M |
| 会话存储 | SQLite |
| 进程通信 | QProcess + JSON Lines |
| 推理后端 | CPU 与 CUDA 分离发布；CUDA 11.8/RTX 3090 目标为 compute capability 8.6，并支持 CUDA 单卡优先、多卡自动放置 |
| 安装程序 | Inno Setup 或 WiX Toolset |
| 自动测试 | Qt Test + CTest |

推荐的 7B Q4_K_M 模型文件预计约 4 至 5 GB。最低建议 16 GB 内存，推荐 32 GB。实际要求以目标硬件测试结果为准。框架安装包不预置模型权重。

CUDA 包仅携带项目自身的 `ggml-cuda.dll`，不再分发 NVIDIA 的 CUDA DLL。
GPU 用户需要预装兼容的 NVIDIA 驱动和 CUDA Toolkit 11.8，并保证 CUDA
运行库可由 `PATH` 找到；CPU 包继续保持无 CUDA 依赖。安装器依赖收集规则必须
排除 `cudart64_110.dll`、`cublas64_11.dll`、`cublasLt64_11.dll` 和
`nvcuda.dll`。

## 3. 总体架构

```text
qtLLM.exe
├── UI
│   ├── MainWindow
│   ├── ChatPage
│   ├── ConversationSidebar
│   ├── ModelManagerPage
│   └── SettingsPage
├── Application
│   ├── ChatService
│   ├── ConversationService
│   ├── ModelManager
│   └── RuntimeManager
├── Infrastructure
│   ├── SQLiteRepository
│   ├── SettingsStore
│   ├── LogManager
│   └── WorkerClient
└── QProcess / JSONL
        ↓
llm-worker.exe
├── llama.cpp
├── ModelLoader
├── ChatTemplate
├── GenerationSession
└── CPU / Vulkan / CUDA Backend
```

`llm-worker.exe` 是产品内部组件。推理使用独立进程，可以隔离模型崩溃和显存不足，也方便独立升级推理引擎及切换硬件后端。

### 分离发布模型

正式发布物分为三种：

```text
qtLLM-runtime-<runtime-version>.exe
qtLLM-model-<model-id>-<package-version>.zip
qtLLM-offline-<edition>-<version>.zip
```

- 框架包包含 Qt 应用、worker、推理引擎和运行库，不包含模型权重。
- 一个模型包只表示一个逻辑模型。不同量化或微调版本分别打包。
- 分片 GGUF 可以在一个包内包含多个分片，但仍视为一个逻辑模型。
- 离线套装可以同时交付框架安装器和一个模型包，但安装后分别更新和卸载。
- 正式模型包必须包含清单、推荐参数、许可证、来源和文件哈希。
- 用户直接选择的 GGUF 可以运行，但在界面中标记为“未验证模型”。

模型存储分为托管安装和外部引用。托管包由框架安装、校验、更新和卸载；用户指定的外部目录默认只读，框架不得移动、覆盖或删除其内容。详细契约见 [模型包规范](model-package-spec.md)。

建议工程目录：

```text
qtLLM/
├── app/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── ui/
│   │   ├── include/
│   │   ├── src/
│   │   └── ui/
│   ├── application/
│   └── infrastructure/
│       ├── include/
│       └── src/
├── worker/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── include/
│   └── src/
├── shared/
│   ├── protocol/
│   │   ├── include/
│   │   └── src/
│   └── models/
│       └── include/
├── tests/
│   ├── client/src/
│   ├── protocol/src/
│   └── worker/src/
├── third_party/
├── packaging/
├── docs/
└── CMakeLists.txt
```

根 CMake 递归发现 `app`、`worker`、`shared` 和 `tests` 中的 `.cpp`、
`.hpp` 与 `.ui` 文件。功能目录保持稳定，新增代码放入对应功能目录下的
`include`、`src` 或 `ui`，不需要手工维护 target 文件清单。

## 4. 开发阶段

### 阶段一：技术验证（第 1 周）

当前状态：CPU 推理代码闭环已完成，真实 DeepSeek GGUF 的性能和稳定性验收待执行。操作与记录方式见 [阶段一本地推理验证](stage-1-local-inference.md)。

任务：

- 引入并固定一个明确版本的 llama.cpp。
- 准备 DeepSeek 7B GGUF 测试模型。
- 实现命令行版 `llm-worker`。
- 完成模型加载、聊天模板、流式生成和停止生成。
- 测量内存、首 token 延迟和生成速度。
- 分别验证纯 CPU 和 Vulkan。
- 检查中文、英文、代码和长文本输出。

验收标准：

- 连续完成至少 50 次对话而不崩溃。
- 可以及时中止生成。
- 模型加载失败或内存不足时返回明确错误。
- 形成不同硬件上的性能基线。

### 阶段二：工程骨架（第 2 周）

当前状态：版本 2 JSONL IPC、持久 worker、Qt `QProcess` 客户端、模型加载、
流式 token、取消、结构化多轮历史与上下文自动裁剪已完成。模型包目录和裸 GGUF
两种加载模式、后台 SHA-256、校验缓存及推荐 preset 应用已经接入。协议单元测试和
worker 进程集成测试已接入 CTest；本地 DeepSeek 1.5B Q4_K_M 已验证取消后继续
生成、多轮消息和真实 tokenizer 裁剪，14B Q4_K_M 已通过正式包哈希验证。最后
成功加载的模型选择路径已持久化到运行目录；日志、其余设置持久化、模型包签名、
worker 崩溃自动恢复和 CI 仍待补充。操作与协议边界见
[阶段二 JSONL IPC](stage-2-jsonl-ipc.md)。

任务：

- 清理现有 Demo，建立 `app`、`worker`、`shared` 和 `tests`。
- 定义版本化 IPC 协议。
- 建立日志、配置和错误码系统。
- 接入 Qt Test、CTest 和基础 CI。
- Qt 负责启动、监控和关闭 worker。

首版协议至少包含：

```text
hello
load_model
unload_model
generate
cancel
get_status
token
generation_finished
error
```

消息结构示例：

```json
{
  "protocolVersion": 2,
  "requestId": "uuid",
  "type": "generate",
  "payload": {}
}
```

验收标准：

- Qt 可以可靠地启动、监控和关闭 worker。
- worker 异常退出时 Qt 主程序不崩溃。
- 请求和响应可通过 `requestId` 对应。
- 不完整 JSON 和协议版本不匹配有明确处理。

### 阶段三：聊天 MVP（第 3 至 4 周）

> TODO：SQLite 会话持久化与应用重启后的历史恢复暂缓。本阶段当前只实现运行期间
> 的多轮上下文；详细边界和后续验收项见 [`TODO.md`](../TODO.md)。

任务：

- 实现主窗口、会话侧栏和聊天页面。
- 实现用户消息、AI 消息和流式输出。
- 支持停止、重新生成、复制和删除消息。
- 支持创建、重命名和删除会话。
- 使用 SQLite 保存会话及消息。
- 支持温度、最大生成长度和上下文长度等设置。
- 应用重启后恢复历史会话。
- 实现超出上下文限制后的历史裁剪策略。

验收标准：

- 支持稳定的连续多轮对话。
- UTF-8 中文流式输出不会乱码或截断。
- 推理期间 UI 始终保持响应。
- 中途退出不会破坏数据库。
- 同一时刻只允许一个受控生成任务。

### 阶段四：模型管理（第 5 至 6 周）

任务：

- 扫描用户指定模型库的一级子目录。
- 导入“一包一模型”的正式模型包。
- 支持直接选择本地 GGUF，并标记为未验证模型。
- 展示模型名称、大小、量化方式和上下文能力。
- 支持安装、删除和切换模型。
- 实现断点续传、SHA-256 校验和磁盘空间检查。
- 将应用版本与模型版本分离。
- 首次启动提供推荐模型安装流程。
- 保存模型来源、许可证和哈希。
- 区分框架托管模型与外部只读模型。

模型清单示例：

```json
{
  "schemaVersion": 1,
  "id": "deepseek-r1-distill-qwen-7b-q4km",
  "displayName": "DeepSeek R1 Distill Qwen 7B Q4_K_M",
  "packageVersion": "1.0.0",
  "modelRevision": "upstream-revision-or-commit",
  "engine": "llama.cpp",
  "format": "gguf",
  "modelFiles": [
    {
      "path": "model.gguf",
      "sizeBytes": 4680000000,
      "sha256": "..."
    }
  ],
  "minimumRuntimeVersion": "0.1.0",
  "recommendedRamGb": 16,
  "defaultContextSize": 32768,
  "presetFile": "preset.json",
  "licenseFile": "LICENSE.txt"
}
```

验收标准：

- 应用安装包无需捆绑 5 GB 模型。
- 一个模型包中不会混入多个可独立选择的模型。
- 下载中断后可以继续。
- 损坏或未知模型不会被加载。
- 删除托管模型前检查其是否正在使用。
- 外部引用模型不会被框架删除或覆盖。

### 阶段五：硬件适配和稳定性（第 7 至 8 周）

任务：

- 检测 CPU 指令集、内存、显卡和显存。
- 根据硬件推荐 CPU、Vulkan 或 CUDA 后端。
- 自动估算模型权重、KV Cache、计算缓冲区和安全余量，并据此确定 GPU
  offload 层数。
- 支持 CUDA 多卡设备发现、显存感知的层切分和用户手动覆盖。
- 处理内存不足、显存不足和模型加载超时。
- 支持 worker 崩溃后重新启动。
- 添加模型加载进度和运行状态。
- 完成长对话、频繁取消和反复加载测试。
- 提供脱敏诊断信息导出。

#### CUDA 显卡适配与多卡放置

多卡能力由 llama.cpp 提供，qtLLM 负责设备发现、显存预算、默认策略、IPC
配置和诊断信息。自动模式不得因为检测到多张显卡就无条件占用全部设备；能在一张
显卡上留足上下文空间时优先单卡，只有单卡无法容纳时才启用多卡。

| 硬件与显存条件 | 自动模式的默认放置 |
| --- | --- |
| 没有可用 GPU | CPU；若用户选择了 CUDA，则返回明确的后端不可用错误 |
| 只有一张可用 GPU | 单卡运行，并根据预算确定全量或部分 GPU offload |
| 多张 GPU，任一单卡可容纳完整预算 | 选择剩余显存充足且满足安全余量的单卡 |
| 单卡不可容纳，但多卡合计可容纳 | 使用 `LLAMA_SPLIT_MODE_LAYER`，按扣除安全余量后的可用显存分配模型层和 KV Cache |
| 多卡合计仍不可容纳 | 降低 GPU offload、使用 CPU 内存，或在不满足最低性能要求时拒绝加载并给出建议 |

实现边界：

- 枚举全部 CUDA GPU，记录设备名称、总显存、空闲显存和可用后端能力；设备列表
  只在末尾添加一次空指针终止符。
- 默认使用 `LLAMA_SPLIT_MODE_LAYER`。`LLAMA_SPLIT_MODE_ROW` 或 tensor parallel
  仅在 NVLink 或经过验证的高速互联环境中显式启用，不能作为通用默认值。
- 自动比例基于每张卡扣除桌面占用和安全余量后的显存计算，不假设同型号显卡的
  可用显存相同。作为显示卡使用的 GPU 0 必须保留额外余量。
- 显存预算必须包含量化权重、模型加载缓冲区、目标上下文长度对应的 KV Cache、
  计算缓冲区和运行时波动；不能只用 GGUF 文件大小判断是否可加载。
- `load_model` IPC 增加 `placementMode`（`auto`、`single`、`multi`）、可选设备列表
  和可选切分比例。默认值为 `auto`，旧客户端只发送 `gpuLayers` 时保持兼容。
- worker 状态和模型加载结果返回实际使用的设备、切分模式、每卡分配比例及显存
  预算，日志不得只报告 GPU 0。
- 自动加载失败时可以按确定顺序重试较保守的放置方案，但不得静默改变用户明确
  指定的设备或切分模式。
- UI 默认只展示“自动、单卡、多卡”三级选择；设备编号和切分比例放入高级设置，
  避免普通用户必须理解 llama.cpp 参数。

首个多卡基线使用两张 RTX 3090 24 GB：小模型在一张卡可容纳时保持单卡；
`Qwen3-Coder-Next 80B-A3B Q3_K_M` 等单卡无法容纳的模型使用 Layer Split，
并为默认上下文保留足够 KV Cache。约 45 GB 的 Q4 权重不能仅因两卡标称合计
48 GB 就判定可用，必须以实际空闲显存和上下文创建成功为准。

验收标准：

- 没有独立显卡的机器仍然可以使用。
- GPU 后端不可用时自动回退 CPU。
- 双 GPU 环境下，小模型默认不占用第二张卡；单卡放不下的模型可以自动切换到
  Layer Split 并成功创建目标上下文。
- 显卡型号、显存容量或当前占用不一致时，切分比例按实际可用显存计算，不能平均
  分配后导致某一张卡溢出。
- 用户强制选择单卡、多卡或指定设备时，实际放置与设置一致，并在失败时给出可
  操作的显存或上下文调整建议。
- 推理异常不会导致聊天记录丢失。
- 连续运行 8 小时没有明显的持续内存增长。
- worker 退出后应用可恢复，不需要整体重启。

### 阶段六：商业发布准备（第 9 至 10 周）

任务：

- 生成 Release 安装包。
- 分别生成框架安装包和单模型包。
- 可选生成包含二者但仍可拆分管理的离线套装。
- 使用 `windeployqt` 收集 Qt 运行库。
- 配置安装、覆盖升级和卸载流程。
- 对 EXE、DLL 和安装包进行代码签名。
- 添加隐私政策和最终用户许可协议。
- 生成 `THIRD_PARTY_NOTICES` 和 SBOM。
- 审核 Qt、llama.cpp、模型及其他依赖许可证。
- 在全新 Windows 虚拟机上测试安装。
- 建立应用更新与模型更新机制。

验收标准：

- 新机器不安装开发环境也能运行。
- 安装、覆盖升级和卸载均通过测试。
- 升级后保留用户数据；卸载时由用户选择是否删除数据。
- 发布物可以追溯到源码提交、依赖版本和模型哈希。
- 所有分发组件都有明确许可证记录。
- 更新框架时不需要重新下载模型，更新模型时不需要重装框架。

## 5. 测试计划

单元测试：

- IPC 消息编解码。
- UTF-8 token 拼接。
- 上下文裁剪。
- 模型清单验证。
- 模型包路径穿越和非法文件类型拦截。
- 多分片模型完整性和顺序验证。
- SQLite 增删改查。
- 配置和数据库迁移。

集成测试：

- worker 启动和关闭。
- 模型加载和卸载。
- 流式生成与取消。
- worker 崩溃恢复。
- 模型文件损坏。
- 重复模型 ID 和不兼容运行时版本。
- 外部模型目录只读保护。
- 磁盘、内存或显存不足。
- CUDA 自动放置决策、设备过滤和显存切分比例。
- 单卡加载失败后切换 Layer Split，以及多卡上下文创建失败时的错误处理。

发布测试：

- Windows 10 和 Windows 11。
- 纯 CPU 机器。
- NVIDIA、AMD 和 Intel 显卡。
- NVIDIA 单卡、双卡、不同显存容量组合，以及 GPU 0 被桌面程序占用的场景。
- 双 RTX 3090 的单卡优先和 Layer Split 性能、显存占用与长时间稳定性基线。
- 16 GB 和 32 GB 内存。
- 中文用户名与中文安装路径。
- 无网络、弱网络和下载中断。
- 安装、升级、降级和卸载。

## 6. 商业许可检查点

正式确定模型和发布包前，必须审核到准确文件版本：

- DeepSeek 原始模型许可证。
- 蒸馏所用 Qwen 基础模型许可证。
- GGUF 转换和量化文件的来源及再分发条款。
- llama.cpp 及其依赖许可证。
- Qt 动态链接和 LGPL 合规要求，或 Qt 商业许可证。
- 字体、图标、Markdown 组件和安装器许可证。

商业发布优先自行从官方权重生成 GGUF，并记录来源、版本、转换参数和 SHA-256，不直接分发来源不明确的社区量化文件。

## 7. 里程碑

| 里程碑 | 时间 | 交付结果 |
| --- | --- | --- |
| M1 | 第 1 周 | DeepSeek 本地推理验证 |
| M2 | 第 2 周 | Qt 与 worker 通信完成 |
| M3 | 第 4 周 | 可用聊天 MVP |
| M4 | 第 6 周 | 模型安装与管理完成 |
| M5 | 第 8 周 | 硬件适配与稳定性完成 |
| M6 | 第 10 周 | 可安装的商业候选版本 |

单人开发预计约 10 周。正式商业首发应额外预留 2 至 4 周，用于多硬件测试、许可证审核和问题修复。

## 8. 下一步

首先执行阶段一：固定 llama.cpp 版本，跑通 `DeepSeek-R1-Distill-Qwen-7B Q4_K_M`，并在目标客户使用的典型电脑上记录模型加载时间、首 token 延迟、生成速度、内存和显存占用。确认 7B 模型符合目标硬件条件后，再开始完整 UI 和数据层建设。
