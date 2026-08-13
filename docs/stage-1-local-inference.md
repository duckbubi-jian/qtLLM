# 阶段一：本地推理验证

## 当前范围

阶段一先验证 CPU 上的单轮本地推理闭环：

- 使用 vcpkg overlay 固定 `llama.cpp b9030` 及其配套 `ggml 0.10.2`。
- 从仓库外加载 GGUF，不提交或复制模型权重。
- 使用 GGUF 内置 chat template 生成单轮回复。
- 将生成文本流式写入 stdout，将诊断和性能数据写入 stderr。
- 支持 Ctrl+C 取消 CPU 推理。

Qt 主程序与 worker 的 JSONL IPC、多轮上下文和模型包解析属于下一阶段，不在本验证程序中提前实现。

## 构建

使用 MSVC x64 环境及 vcpkg toolchain 配置：

```powershell
cmake -S . -B cmake-build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=<VCPKG_ROOT>/scripts/buildsystems/vcpkg.cmake
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
```

项目的 `vcpkg-configuration.json` 会自动启用仓库内 overlay port。不要删除 overlay 后单独升级系统 `ggml`，因为 llama.cpp 与 ggml 的内部 API 需要按同一上游提交配套。

## 运行

使用命令参数传入 UTF-8 提示词：

```powershell
qtllm-worker.exe `
  --model D:\qtLLM-models\deepseek-r1-distill-qwen-7b-q4km\model.gguf `
  --prompt "请用三句话说明什么是本地大模型" `
  --context-size 8192 `
  --max-tokens 1024 `
  --threads 8
```

也可以通过 stdin 传入提示词：

```powershell
"你好" | qtllm-worker.exe --model D:\models\model.gguf
```

本阶段是 CPU runtime，因此 `--gpu-layers` 保持为 `0`。后续发布 Vulkan 或 CUDA runtime 时复用同一参数接口。

## 验证记录

每台目标机器至少记录：

| 项目 | 记录值 |
| --- | --- |
| CPU / GPU | |
| 内存 / 显存 | |
| Windows 版本 | |
| 模型包 ID 和 SHA-256 | |
| 模型加载时间 | |
| Prompt token 数 | |
| 生成 token 数 | |
| 生成速度（token/s） | |
| 峰值内存 | |
| 首 token 延迟 | |

当前 worker 会在 stderr 输出 `model_load_ms`、`prompt_eval_ms`、`first_token_ms`、`prompt_tokens`、`generated_tokens` 和 `generation_tokens_per_second`。`first_token_ms` 从 prompt 预填开始计时，不包含模型加载；峰值内存将在性能采集模块中补充。

## 阶段验收

准备经过许可证确认的 GGUF 后执行：

1. 中文问答、英文问答、代码生成和长文本各至少 10 次。
1. 连续完成至少 50 次进程级生成，不崩溃。
1. 验证错误模型路径、损坏 GGUF、上下文超限和 Ctrl+C。
1. 在目标客户的低配和推荐配置机器上记录上述指标。

在真实模型完成这些测试前，阶段一只能视为“代码闭环完成”，不能视为模型选型验收完成。
