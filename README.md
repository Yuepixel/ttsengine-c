# TtsEngine-C

用 C++17 + GGML 实现的 Qwen3-TTS 本地推理服务（Windows / CUDA），提供一个与常见
Python/FastAPI TTS 服务兼容的 HTTP 接口，并附带一个 Win32 桌面控制面板。

A C++17 + GGML inference server for Qwen3-TTS on Windows/CUDA. It exposes a
small HTTP API (WAV and streaming PCM), a voice registry (designed + cloned
voices) and a Win32 control panel.

> 非官方项目，与阿里巴巴 / Qwen 团队、以及上游 [qwentts.cpp](https://github.com/ServeurpersoCom/qwentts.cpp) 作者均无隶属关系。
> Unofficial project, not affiliated with Alibaba/Qwen or the upstream authors.

---

## 1. 功能

- `/tts`：整段合成，返回 24 kHz / 16-bit / 单声道 WAV。
- `/tts/pcm`：边生成边返回 PCM（`QTTS` 帧协议），首包延迟远低于整段合成。
- 音色库：`VoiceDesign`（自然语言描述生成音色）与 `Base` 音色克隆（参考音频 + 转写）。
- 内置 44 个中文音色描述（只有文本预设，不含任何音频）。
- 语速：`rate >= 1.0` 做 WSOLA 压缩；`rate < 1.0` **不降速拉伸**（见 §7）。
- Win32 GUI：启动/停止服务、状态与日志、音色数量、试音、配置编辑；另有
  无窗口启动器 `ttsc-launch.exe` 与控制台入口 `ttsc-server.exe`。

## 2. 目录结构

```
src/
  ttsc.h          公共类型与 Engine/ServerContext 接口
  engine.cpp      上游 C ABI 的封装（加载模型、合成、音色设计/克隆、取消）
  wsola.cpp       时域变速（压缩方向）
  voice_store.cpp 音色注册表（voices.json + 每个音色的参考音频/隐向量）
  http_api.cpp    HTTP 端点（cpp-httplib + yyjson）
  wav.cpp         WAV 读写
  config.cpp      ini 解析（键名与旧 Python 版兼容）
  perf.cpp        /perf 统计
  main.cpp        控制台服务入口（ttsc-server）
  gui_main.cpp    Win32 面板（ttsc-gui）
  launch_main.cpp 无窗口启动器（ttsc-launch）
tests/            单元测试（ttsc-tests，纯 CPU 可跑）
tools/            构建、部署、基准与音色预设生成脚本
```

## 3. 构建

### 3.1 依赖

- Windows 10/11 x64
- Visual Studio 2022 BuildTools（MSVC 14.4x，含 Win10/11 SDK）
- CMake ≥ 3.20、Ninja
- CUDA Toolkit 12.8 或 13.x（实测 13.0，`sm_120` / RTX 50 系）
- 显存：Q8_0 双模型约 7 GB 峰值（16 GB 卡余量充足）

CUDA 工具链可以不在默认路径：`tools/build.ps1` 默认读取 `$env:CUDA_PATH`，
也可用 `-CudaRoot D:\your\cuda` 指定。

### 3.2 拉取上游引擎

上游 `qwentts.cpp` 不随本仓库分发（保留其独立许可与历史），由脚本检出到
`third_party/`：

```powershell
pwsh -NoProfile -File tools\fetch_upstream.ps1
```

脚本固定检出 commit `5916bf7`（2026-10-16）。升级上游时改脚本里的 `-Ref`，
重新构建并跑测试。

### 3.3 编译与测试

```powershell
pwsh -NoProfile -File tools\build.ps1                # CPU + CUDA? 默认 CUDA
pwsh -NoProfile -File tools\build.ps1 -Cpu           # 纯 CPU（慢，仅验证）
pwsh -NoProfile -File tools\build.ps1 -Clean         # 先清构建目录

.\build-cuda\ttsc-tests.exe                          # 单元测试
```

产物：`build-cuda\ttsc-server.exe`、`ttsc-gui.exe`、`ttsc-launch.exe`、
`ttsc-tests.exe`、`qwen-tts.exe` 与 `ggml*.dll`。

> 注意：`ggml.dll → ggml-cuda.dll → cublas64_13.dll` 是加载期导入链。
> 直接用资源管理器双击 GUI 时，请让 CUDA 的 `bin` 在 `PATH` 里（安装器会设置
> `CUDA_PATH`），或改用 `ttsc-launch.exe` / `run.cmd`。

## 4. 模型

需要两个 GGUF（默认放在 `models/`，可用 `models_root` 改）：

| 文件 | 用途 |
|---|---|
| `qwen-talker-1.7b-base-Q8_0.gguf` | 合成 + 音色克隆 |
| `qwen-talker-1.7b-voicedesign-Q8_0.gguf` | 文本描述生成音色 |
| `qwen-tokenizer-12hz-Q8_0.gguf` | 12 Hz 声码器（两个模式共用） |

获取方式（择一）：

1. 直接下载上游预转换的 GGUF：<https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF>
   （国内可用 ModelScope 镜像或 `hf-mirror.com`）。
2. 自行转换：在上游仓库执行 `checkpoints.sh → convert.py → quantize.sh`，
   详见上游 README 的 *Model conversion*。

**模型权重不随本仓库分发。** Qwen3-TTS 模型与 12 Hz Tokenizer 均由 Qwen 团队以
Apache-2.0 发布，使用前请阅读其模型卡与许可。

## 5. 运行

```powershell
# 控制台（排障用）
.\build-cuda\ttsc-server.exe -c .\ttsserver.ini

# 桌面面板
.\build-cuda\ttsc-gui.exe
```

`ttsserver.ini` 示例见 [`ttsserver.ini.example`](ttsserver.ini.example)。常用键：

| 键 | 说明 |
|---|---|
| `models_root` | GGUF 所在目录（默认 `models`） |
| `data_dir` | 音色库目录（默认 `data`） |
| `host` / `port` | 监听地址，默认 `127.0.0.1:8081` |
| `device` | 例如 `CUDA0`；留空为自动 |
| `max_batch` | 引擎内部批上限（默认 2） |
| `max_queue` | 排队上限，超出返回 429 |
| `stream` | 是否对外提供 `/tts/pcm`（`0/1`） |
| `time_stretch` | 是否启用 `rate` 变速 |
| `synth_timeout` | 单请求合成预算（秒），超时 `/tts` 返回 504 |
| `debug` | 每个请求打印一行摘要（含 rate/字数/耗时） |
| `language` | 默认语言，如 `Chinese` |

未知键会被忽略：同一份 ini 可以直接在旧 Python 服务与本服务之间共用。

部署到独立目录（复制 exe/dll、生成 `run.cmd`、可选桌面快捷方式）：

```powershell
pwsh -NoProfile -File tools\make_deploy.ps1 -Ini .\ttsserver.ini
```

## 6. HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/health` | 状态、版本、音色数、`stream_protocol`、队列 |
| GET | `/perf` | 最近合成的耗时统计（`perf=1` 时） |
| GET | `/voices` | 音色列表 |
| POST | `/voices/new` | `{id, description, script?, language?}` 生成音色 |
| POST | `/voices/clone` | `{id, ref_audio_base64, ref_text?, language?}` 克隆音色 |
| DELETE | `/voices/{id}` | 删除音色并清理缓存 |
| POST | `/tts` | `{text, voice, language?, rate?, instruct?}` → WAV |
| POST | `/tts/pcm` | 同上 → `QTTS` 流（`rate != 1` 且未开 `stream_stretch` 时 422） |

示例：

```bash
curl -s http://127.0.0.1:8081/health

curl -s -X POST http://127.0.0.1:8081/tts \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好，世界。","voice":"q3m_01","language":"Chinese","rate":1.0}' \
  -o out.wav
```

错误体沿用 FastAPI 形状：`{"detail": "..."}`。

`/tts/pcm` 线格式（小端）：

```
16 B 头 : "QTTS" | u8 ver=1 | u8 bits=16 | u8 channels=1 | u8 flags=0
          | u32 sample_rate | u32 total=0
N B 体  : int16 单声道 PCM 分块
8 B 尾  : "QEND" | u32 total_samples
```

客户端中途断开时服务端会取消该请求（不再消耗解码步），不影响后续请求。

## 7. 已知限制（有意为之）

- **`rate < 1` 不做降速拉伸。** 多种时域/频域伸缩算法（WSOLA 多参数、
  audiotsm、Rubberband、相位声码器）在主观试听中都被判为比原音差，
  因此 `rate` 下限为 1.0，只压缩不拉伸。
- `/tts/pcm` 与 `rate != 1` 的组合默认返回 422（与旧 Python 服务一致）；
  需要时用 `stream_stretch = 1` 打开。
- 单进程、单 GPU：并发超过 `max_batch` 会排队，超过 `max_queue` 直接 429。
- 没有鉴权，默认只监听 `127.0.0.1`；不要直接暴露到公网。
- GUI 是 Windows 专用；服务端与测试可在 CPU 后端编译运行。

## 8. 实测（参考值）

RTX 5080 16 GB / Q8_0 / CUDA 13.0，单句 30 汉字的稳态数据：

| 指标 | 数值 |
|---|---|
| 引擎合成总耗时 | ≈ 0.98 s（8.3 s 音频） |
| RTF（引擎） | ≈ 0.12 |
| 首帧延迟（引擎） | ≈ 60 ms |
| `/tts` 端到端（HTTP） | ≈ 0.85 s |

不同显卡、驱动与量化等级差异很大，请以 `tools/bench-cli.ps1` 自测为准。

## 9. 许可与致谢

本仓库代码以 **MIT** 发布，见 [LICENSE](LICENSE)。

第三方组件与出处：

| 组件 | 版本 / 提交 | 许可 |
|---|---|---|
| [qwentts.cpp](https://github.com/ServeurpersoCom/qwentts.cpp)（上游推理引擎） | `5916bf7` | MIT |
| [ggml](https://github.com/ggml-org/ggml)（随上游，CUDA/CPU 后端） | 上游子模块 | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib)（随上游 vendor） | 0.46.1 | MIT |
| [yyjson](https://github.com/ibireme/yyjson)（随上游 vendor） | 随上游 | MIT |
| Qwen3-TTS 1.7B / 12 Hz Tokenizer（模型权重，不随仓库分发） | Qwen 团队 | Apache-2.0 |

详细声明见 [NOTICE](NOTICE) 与 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 10. 贡献

欢迎 issue / PR。提交前请至少跑通 `ttsc-tests.exe`；涉及引擎行为的改动请附
可复现的命令与听感/性能数据，避免只给结论。

---

本文档由 DeepSeek V4.1 Flash AI 辅助完成。
