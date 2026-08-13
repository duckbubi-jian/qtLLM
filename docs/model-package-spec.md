# qtLLM 模型包规范

## 1. 设计约束

qtLLM 的运行框架与模型权重分离发布。一个模型包必须且只能表示一个逻辑模型：

- 不同模型分别打包。
- 同一模型的不同量化版本分别打包。
- 同一模型的不同微调版本分别打包。
- GGUF 因文件大小而分片时，可以在一个包内包含多个分片。
- 模型包只包含数据、配置、文档和许可证，不包含 EXE、DLL、脚本或插件。

推荐发布物命名：

```text
qtLLM-runtime-1.0.0.exe
qtLLM-model-deepseek-r1-distill-qwen-7b-q4km-1.0.0.zip
qtLLM-model-qwen-4b-q4km-1.0.0.zip
```

## 2. 模型包目录

标准模型包解压后只有一个顶层目录：

```text
deepseek-r1-distill-qwen-7b-q4km/
├── manifest.json
├── model.gguf
├── preset.json
├── LICENSE.txt
└── README.md
```

分片模型可以使用：

```text
model-00001-of-00003.gguf
model-00002-of-00003.gguf
model-00003-of-00003.gguf
```

这些分片必须共同组成一个模型，不得作为多个可独立选择的模型展示。

## 3. manifest.json

`manifest.json` 是正式模型包的必需文件，使用 UTF-8 编码。Schema v1 示例：

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
      "sha256": "replace-with-64-lowercase-hex-characters"
    }
  ],
  "minimumRuntimeVersion": "0.1.0",
  "recommendedRamGb": 16,
  "defaultContextSize": 8192,
  "presetFile": "preset.json",
  "licenseFile": "LICENSE.txt",
  "upstream": {
    "publisher": "DeepSeek",
    "modelId": "DeepSeek-R1-Distill-Qwen-7B",
    "revision": "replace-with-fixed-revision",
    "source": "replace-with-official-source"
  }
}
```

版本字段职责：

| 字段 | 含义 |
| --- | --- |
| `schemaVersion` | 清单格式版本 |
| `packageVersion` | qtLLM 模型包的发布版本 |
| `modelRevision` | 上游权重或转换产物的固定版本 |
| `minimumRuntimeVersion` | 能加载此包的最低框架版本 |

`id` 在一个模型库中必须唯一。模型文件大小必须与 `sizeBytes` 一致，SHA-256 必须在首次安装和用户主动校验时验证。

## 4. preset.json

`preset.json` 保存推荐推理参数，不保存用户会话数据：

```json
{
  "contextSize": 8192,
  "maxOutputTokens": 2048,
  "temperature": 0.6,
  "topP": 0.95,
  "topK": 40,
  "repeatPenalty": 1.05
}
```

Chat template 优先读取 GGUF 元数据。只有在模型验证阶段确认元数据缺失或错误时，才允许未来的 Schema 版本显式覆盖模板。

## 5. 模型库发现规则

用户可以在设置中指定模型库根目录，例如：

```text
D:\qtLLM-models\
├── deepseek-r1-distill-qwen-7b-q4km\
│   ├── manifest.json
│   └── model.gguf
└── qwen-4b-q4km\
    ├── manifest.json
    └── model.gguf
```

框架只扫描根目录的一级子目录，每个一级子目录视为一个候选模型包。发现流程为：

1. 读取 `manifest.json`，检查 JSON 和 `schemaVersion`。
1. 检查包 ID 是否重复及运行时版本是否兼容。
1. 验证所有路径均为包目录内的相对路径。
1. 检查文件存在、大小、GGUF 格式及架构支持情况。
1. 校验 SHA-256，成功后标记为“已验证模型包”。
1. 读取推荐参数并将模型加入可选列表。

框架还允许用户直接选择兼容的 `.gguf` 文件。此模式不要求清单，但必须标记为“未验证模型”，不承诺来源、许可证、推荐参数或运行时兼容性。

## 6. 托管与外部模型

模型采用两种存放模式：

- 托管安装：框架将验证后的包原子地安装到应用模型库，可以执行更新和卸载。
- 外部引用：用户指定已有模型目录，框架只读扫描，不移动、不覆盖、不删除其中内容。

托管安装应先解压至临时目录，完成全部校验后再重命名到最终目录。失败或取消时不得留下可被扫描到的半成品模型包。

用户会话、设置和日志不得写入模型包，应保存在：

```text
%LOCALAPPDATA%\<Company>\qtLLM\
├── conversations.db
├── settings.json
└── logs\
```

## 7. 安全边界

- 拒绝绝对路径、`..` 路径穿越和指向包外部的符号链接。
- 模型包不执行任何随包提供的代码或命令。
- 拒绝模型包中的 EXE、DLL、BAT、CMD、PS1、JS 和其他可执行内容。
- 解压前检查声明大小、实际压缩大小和可用磁盘空间。
- 限制文件数量和解压总大小，防止恶意压缩包耗尽磁盘。
- 模型加载在 `qtllm-worker` 进程中完成，异常不得带崩主界面。
- 正式发布的模型包必须附带准确的许可证、来源和固定上游版本。

## 8. 发布形态

| 版本 | 内容 | 更新方式 |
| --- | --- | --- |
| 框架版 | Qt 应用、worker、推理引擎、运行库 | 独立更新框架 |
| 模型包 | 一个逻辑模型及其元数据和许可证 | 独立更新单个模型 |
| 离线套装 | 框架安装器和一个独立模型包 | 安装后仍分别管理 |

即使通过同一个离线介质交付，框架和模型也不能合并为不可拆分的安装组件。
