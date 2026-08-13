# 阶段二：JSONL IPC 与持久 Worker

## 当前范围

Qt 主程序通过 `QProcess` 启动 `qtllm-worker --ipc`。worker 在进程内持有一个
已加载的 GGUF 模型，可以执行多次生成，无需每轮重新加载权重。

运行时与模型保持分离：

- runtime 包包含 Qt 应用、worker、llama.cpp 和运行库，不包含模型权重。
- 模型包或外部目录包含 GGUF；当前可直接指定 `model.gguf`。
- Qt 只向 worker 传递模型绝对路径，不复制、移动或修改外部模型。
- worker 收到 `unload_model` 或进程退出时释放模型。

当前聊天窗口是协议验证界面，还不包含会话数据库、多轮历史拼接和完整模型包
清单解析。这些属于后续聊天 MVP 与模型管理阶段。

## 协议

IPC 使用 stdin/stdout 上一行一个 JSON 对象的 JSON Lines 格式。stderr 仅用于
诊断，不得混入协议输出。每条消息都包含：

```json
{
  "protocolVersion": 1,
  "requestId": "unique-request-id",
  "type": "generate",
  "payload": {}
}
```

客户端请求：

- `hello`
- `load_model`
- `unload_model`
- `generate`
- `cancel`
- `get_status`

worker 响应或事件：

- `hello`
- `model_loaded`
- `model_unloaded`
- `generation_started`
- `token`
- `generation_finished`
- `status`
- `error`

`requestId` 将响应和流式事件关联到原始请求。`token.payload.data` 是 Base64，
解码后才是模型产生的原始 UTF-8 字节。这样即使单个 token 截断了一个 UTF-8
字符，JSON 消息仍保持有效，Qt 端可以在字节流层完成拼接。

同一 worker 同时只允许一个生成任务。生成在独立线程执行，因此主事件循环仍能
处理 `cancel` 和 `get_status`。加载或卸载模型时如果正在生成，会返回 `busy`。

## 构建与普通测试

使用 MSVC x64 开发环境构建后运行：

```powershell
cmake -S . -B cmake-build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=<VCPKG_ROOT>/scripts/buildsystems/vcpkg.cmake
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
```

普通 CTest 不加载模型，覆盖：

- JSONL 编解码与协议版本数值边界。
- Qt `WorkerClient` 默认查找、启动、握手和关闭 worker。
- worker 握手。
- 非法 JSON。
- 协议版本不匹配。
- 未加载模型时生成。
- 空闲状态查询。
- 原有命令行参数错误路径。

## 真实模型测试

`QTLLM_TEST_MODEL` 可以指向 GGUF 文件，也可以指向包含 `model.gguf` 的模型包
目录：

```powershell
$env:QTLLM_TEST_MODEL = "D:\models\deepseek-package"
ctest --test-dir cmake-build-release --output-on-failure `
  -R "^worker_jsonl_ipc$"
```

真实模型用例在同一个 worker 进程内依次验证：

1. `hello` 握手。
1. `load_model` 加载外部 GGUF。
1. 接收流式 token 后发送 `cancel`。
1. 收到取消完成事件。
1. 不重新加载模型，直接执行第二次生成。

未设置 `QTLLM_TEST_MODEL` 时，该用例的模型生命周期部分会跳过，协议错误路径仍
正常执行。

## 当前验收记录

2026-08-13 使用本地 `DeepSeek-R1-Distill-Qwen-1.5B Q4_K_M` CPU 模型完成：

- worker 握手和版本检查通过。
- 模型加载通过。
- Base64 token 流通过。
- 生成取消通过。
- 同一模型不重新加载的第二次生成通过。
- 无模型 CTest 共 8 项全部通过。

该记录验证架构链路，不代表商业模型许可证、7B 性能、多轮对话质量或全硬件矩阵
已经验收。
