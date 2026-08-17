# 阶段一：本地推理验证

## 当前范围

阶段一验证 CPU 与单卡 CUDA 上的本地推理闭环：

- 使用 vcpkg overlay 固定 `llama.cpp b9030` 及其配套 `ggml 0.10.2`。
- 从仓库外加载 GGUF，不提交或复制模型权重。
- 使用 GGUF 内置 chat template 生成单轮回复。
- 将生成文本流式写入 stdout，将诊断和性能数据写入 stderr。
- 支持 Ctrl+C 取消推理。
- CUDA 11.8 runtime 使用 Visual Studio 2019/v142 构建，针对 RTX 3090
  （compute capability 8.6），运行时只选择 GPU 0。

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

CUDA runtime 与 CPU runtime 使用独立构建目录和发布包。在 *x64 Native
Tools Command Prompt for VS 2019* 中设置 `VCPKG_ROOT` 后执行：

```powershell
cmake --preset cuda-release-vs2019 `
  -DCMAKE_CXX_COMPILER="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/MSVC/14.29.30133/bin/Hostx64/x64/cl.exe" `
  -DCMAKE_C_COMPILER="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/MSVC/14.29.30133/bin/Hostx64/x64/cl.exe" `
  -DCMAKE_CUDA_HOST_COMPILER="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/MSVC/14.29.30133/bin/Hostx64/x64/cl.exe"
cmake --build --preset cuda-release-vs2019
ctest --test-dir cmake-build-cuda --output-on-failure
```

根 `CMakeLists.txt` 会自动将 Windows 的 vcpkg triplet 固定为仓库内的
`x64-windows-vs2019`，因此 CLion 的 CMake options 只需添加
`-DQTLLM_ENABLE_CUDA=ON`。CLion Toolchain 仍须选择 Visual Studio 2019；切换
Toolchain 后需要重置 CMake 缓存。项目会拒绝使用其他版本的 MSVC，避免主程序和
依赖库混用不同工具链。

CUDA 发布包包含项目自身的 `ggml-cuda.dll`，但不打包 NVIDIA 提供的
`cudart64_110.dll`、`cublas64_11.dll`、`cublasLt64_11.dll` 和
`nvcuda.dll`。目标电脑必须预先安装兼容的 NVIDIA 驱动和 CUDA Toolkit 11.8，
并确保进程可通过 `PATH`（通常是 `%CUDA_PATH%\bin`）找到 CUDA 运行库。
CPU 发布包不依赖 CUDA。GPU 版无法启动时，优先检查驱动、CUDA 版本和 `PATH`。

项目的 `vcpkg-configuration.json` 会自动启用仓库内 overlay port。不要删除 overlay 后单独升级系统 `ggml`，因为 llama.cpp 与 ggml 的内部 API 需要按同一上游提交配套。

## 运行

使用命令参数传入 UTF-8 提示词：

```powershell
qtllm-worker.exe `
  --model D:\qtLLM-models\deepseek-r1-distill-qwen-7b-q4km\model.gguf `
  --prompt "请用三句话说明什么是本地大模型" `
  --context-size 32768 `
  --max-tokens 4096 `
  --threads 8
```

也可以通过 stdin 传入提示词：

```powershell
"你好" | qtllm-worker.exe --model D:\models\model.gguf
```

CPU runtime 使用 `--gpu-layers 0`。CUDA runtime 默认使用
`--gpu-layers -1 --gpu-mode auto`，枚举全部可用 CUDA GPU，并由 llama.cpp 按
空闲显存执行 Layer Split；没有可用 CUDA 设备时自动回退 CPU。`--gpu-mode cpu`
可让 CUDA 构建在本次运行中使用 CPU，`single` 与 `custom` 模式分别配合
`--gpu-devices` 和 `--tensor-split` 指定设备与权重。运行时 CPU 模式不替代独立
CPU 发布包，后者仍不依赖 CUDA。模型权重仍放在仓库外，一个模型包只包含一个逻辑模型。当前轻量默认模型为
`DeepSeek-R1-Distill-Qwen-7B Q4_K_M`；配备 24 GB 显存时可选择质量更高、速度
较低的 `DeepSeek-R1-Distill-Qwen-14B Q4_K_M`。模型目录例如
`D:\qtLLM-models\deepseek-r1-distill-qwen-7b-q4km` 和
`D:\qtLLM-models\deepseek-r1-distill-qwen-14b-q4km`。

该 7B 模型在 RTX 3090 上的产品默认值为 32768 token 上下文，并为单次回答
预留 4096 token。系统提示词、历史消息、本次输入和回答预留共同占用上下文。
worker 只会按完整问答轮次裁剪旧历史，绝不会截断最新用户输入；如果最新输入仍然
放不下，会返回实际 prompt token、回答预留和上下文上限，要求用户缩短输入、降低
回答上限或增大上下文。

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

2026-08-13 的开发机基线如下。该结果用于确认 CUDA 链路和单卡约束，不替代
完整的硬件兼容性、稳定性和回答质量验收。

| 项目 | 实测值 |
| --- | --- |
| 模型 | DeepSeek-R1-Distill-Qwen-7B Q4_K_M |
| 模型 SHA-256 | `731ece8d06dc7eda6f6572997feb9ee1258db0784827e642909d9b565641937b` |
| 构建环境 | CUDA 11.8、Visual Studio 2019/v142 |
| 上下文长度 | 32768 |
| 推理设备 | GPU 0，RTX 3090 24 GB |
| 模型加载时间 | 约 2.31 秒 |
| 首 token 延迟 | 约 42 ms |
| 生成速度 | 约 119.77 token/s |
| GPU 0 基线 / 峰值显存 | 1239 / 7765 MiB |
| GPU 0 新增峰值显存 | 约 6526 MiB |
| Worker 峰值工作集 / 私有内存 | 4552.8 / 7127.7 MiB |
| GPU 1 显存 | 0 MiB |

2026-08-14 使用同一台开发机和 32K 产品配置验证了 14B 增强包。该次短提示
最多允许生成 512 token，模型自行在 298 token 结束。

| 项目 | 实测值 |
| --- | --- |
| 模型 | DeepSeek-R1-Distill-Qwen-14B Q4_K_M |
| 模型 SHA-256 | `0b319bd0572f2730bfe11cc751defe82045fad5085b4e60591ac2cd2d9633181` |
| 构建环境 | CUDA 11.8、Visual Studio 2019/v142 |
| 上下文长度 | 32768 |
| 推理设备 | GPU 0，RTX 3090 24 GB |
| 模型加载时间 | 约 4.04 秒 |
| Prompt token / 生成 token | 16 / 298 |
| Prompt 计算 / 首 token 延迟 | 49 / 51 ms |
| 生成速度 | 约 65.96 token/s |
| GPU 0 基线 / 峰值显存 | 1155 / 16033 MiB |
| GPU 0 新增峰值显存 | 约 14878 MiB |
| Worker 峰值工作集 / 私有内存 | 8543.4 / 15457.4 MiB |
| GPU 1 峰值显存 | 0 MiB |

## 阶段验收

准备经过许可证确认的 GGUF 后执行：

1. 中文问答、英文问答、代码生成和长文本各至少 10 次。
1. 连续完成至少 50 次进程级生成，不崩溃。
1. 验证错误模型路径、损坏 GGUF、上下文超限和 Ctrl+C。
1. 在目标客户的低配和推荐配置机器上记录上述指标。

在真实模型完成这些测试前，阶段一只能视为“代码闭环完成”，不能视为模型选型验收完成。
