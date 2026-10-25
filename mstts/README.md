# mstts —— macOS 上的离线微软神经语音

把 `../multitts-instance` 里那套「微软离线 Embedded 神经 TTS」原样搬到 macOS：
**同一批语音包、同一批声音（晓晓 / 云希 / 云哲 / Aria / Jenny / Guy / Ryan / Neerja / Prabhat）**，
用微软官方的 macOS 原生库（Speech SDK 1.53 的 Embedded TTS 扩展）直接跑。
不联网、不调系统 TTS、不需要 Android 模拟器，就是一个 `mstts` 命令 + 一个本地 HTTP 服务。

```
$ ./build/mstts say "春眠不觉晓，处处闻啼鸟。" --play
[mstts] 晓晓 · 4.90s 音频 · 合成 189 ms
```

## 现状（本机已实测）

| 项 | 结果 |
|---|---|
| 语音 | 9 个全部合成成功（`mstts doctor`） |
| 输出 | 24kHz / 16bit / 单声道 PCM（离线引擎的物理上限），可存 WAV 或直接播 |
| 速度 | 单句 60–190 ms；1928 个汉字（392 秒音频）合成耗时 7.3 秒 ≈ **53× 实时** |
| 体积 | 可执行文件 ~1 MB + 3 个 dylib 共 71 MB（universal：arm64 + x86_64） |
| 依赖 | 系统自带（CoreAudio/AudioToolbox）+ Xcode 命令行工具里的 clang++ |

## 快速开始

```bash
cd mstts-macos

./scripts/fetch-sdk.sh          # 拉微软 macOS 原生库（仓库里已带 third_party/，可跳过）
make                            # → build/mstts
./scripts/install-voices.sh     # 语音包装到 ~/Library/Application Support/mstts/voices
                                # 加 --symlink 可只建链接，省 385MB

./build/mstts doctor            # 9 个语音逐个自检
./build/mstts say "春眠不觉晓，处处闻啼鸟。" --play
```

不想装语音包也行：`--voices ../multitts-instance/app/src/main/assets/msvoice` 直接指过去。

## 命令行

```
mstts list [--json]                 语音表（含是否已安装）
mstts say <文本...> [选项]          合成（默认：终端里播放；被管道时写 WAV 到 stdout）
mstts say -f <文件> [选项]          从文件读文本（长文本自动按句切块）
mstts ssml <文件|-> [选项]          直接喂 SSML（可用 <prosody>/<emphasis>/<break> 等）
mstts serve [--port 8765]           本地 HTTP 服务（OpenAI 兼容，见下）
mstts doctor                        逐语音自检并报耗时
```

常用选项：

| 选项 | 说明 |
|---|---|
| `-v, --voice` | 语音，写 code（`zh-CN-XiaoxiaoNeural`）或短名（`晓晓`、`Aria`）都行 |
| `-r, --rate` / `-p, --pitch` | 语速 / 音调倍数，1.0 = 原样（`1.1` = +10%） |
| `-o, --out` | 输出 WAV，`-` 表示 stdout |
| `--raw` | 输出裸 PCM（24kHz/16bit/mono） |
| `--play` | 用 CoreAudio 直接播（不落临时文件） |
| `--voices` | 语音包根目录（默认 `$MSTTS_VOICES` 或 `~/Library/Application Support/mstts/voices`） |
| `--chunk` | 长文本切块上限，默认 450 字 |

示例：

```bash
# 长文本 → wav（一本一章几千字也没问题）
./build/mstts say -f chapter1.txt -v 晓晓 -r 1.1 -o chapter1.wav

# 管道
echo "The quick brown fox." | ./build/mstts say - -v Aria -o fox.wav
./build/mstts say "..." --raw | ffmpeg -f s16le -ar 24000 -ac 1 -i - out.mp3

# SSML：精细控制停顿和重音
cat > t.ssml <<'EOF'
<speak version='1.0' xml:lang='zh-CN'><voice name='zh-CN-XiaoxiaoNeural'>
  <prosody rate='-15%' pitch='+5%'>这是一段<emphasis level='strong'>重点</emphasis>，
  <break time='500ms'/>后面是停顿后的内容。</prosody>
</voice></speak>
EOF
./build/mstts ssml t.ssml -o t.wav
```

## HTTP 服务

`mstts serve` 起一个本地服务（默认 `127.0.0.1:8765`），浏览器打开 `http://127.0.0.1:8765/`
就是一个能试听的小页面。接口：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/v1/audio/voices` | 语音列表（id/短名/语言/性别/是否安装） |
| GET | `/health` | 健康检查 |
| POST | `/v1/audio/speech` | OpenAI 兼容：`{"input","voice","speed","response_format":"wav"\|"pcm"}` |
| GET | `/say?text=...&voice=...&rate=1.0&pitch=1.0` | 浏览器/curl 直接拿 WAV |

```bash
./build/mstts serve --port 8765 &

curl -s http://127.0.0.1:8765/v1/audio/voices | jq .
curl -s -X POST http://127.0.0.1:8765/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -d '{"input":"你好，这是通过 HTTP 合成的语音。","voice":"云希","speed":1.1}' \
  -o out.wav
```

因为接口和 OpenAI 的 `/v1/audio/speech` 对得上，任何支持自定义 `base_url` 的朗读/有声书工具
（包括 `epub2audio` 那类流程）都可以直接把 `base_url` 指过来，`api_key` 随便填。

## 目录结构

```
mstts-macos/
├── Makefile                  构建（只依赖 clang++）
├── CMakeLists.txt            可选：给 IDE / cmake 用户
├── src/
│   ├── voices.cpp            9 个语音表 + 各语音 license
│   ├── ssml.cpp              SSML 组装、XML 转义、长文本按句切分
│   ├── engine.cpp            微软 Embedded TTS 封装（文本/SSML → PCM）
│   ├── player.cpp            CoreAudio AudioQueue 播放（不落临时文件）
│   ├── wav.cpp               WAV 封装、读写
│   ├── json.cpp              迷你 JSON（只解析顶层标量，HTTP 用）
│   ├── http_server.cpp       本地 HTTP 服务 + 内置试听页
│   └── main.cpp              CLI
├── tests/unit.cpp            纯函数单测（make test）
├── scripts/
│   ├── fetch-sdk.sh          从 nuget.org 取 macOS dylib + 头文件，lipo 成 universal
│   ├── install-voices.sh     装语音包（复制 / 符号链接 / 从 zip 解）
│   └── smoke-test.sh         端到端冒烟
├── tools/extract_license.py  从 APK 的 libspeexdsp.so 取设备 license（见 docs/原理.md）
├── tools/probe_formats.cpp   逐格式探测引擎真实输出（make probe-formats）
├── third_party/ms-speech/    微软原生库 + 头文件（由 fetch-sdk.sh 生成）
└── docs/原理.md              为什么能跑、模型格式、license 怎么来的
```

## 与 Android 版的对应关系

| Android（multitts-instance） | 这里 |
|---|---|
| `engine/MsVoices.kt` | `src/voices.cpp` |
| `engine/Ssml.kt`（SsmlBuilder + Chunker） | `src/ssml.cpp` |
| `engine/MsEmbeddedEngine.kt` | `src/engine.cpp` |
| `audio/DirectPlayer.kt`（AudioTrack） | `src/player.cpp`（AudioQueue） |
| `audio/WavUtil.kt` | `src/wav.cpp` |
| `ui/MainActivity.kt` | `src/main.cpp` + `src/http_server.cpp` |

调用配方完全一致：`EmbeddedSpeechConfig::FromPath(语音目录)` →
`SetSpeechSynthesisVoice(语音名, license)` → `Raw24Khz16BitMonoPcm` → `SpeakSsml` → `AudioDataStream`。

## 增删语音

语音包就是 `<voices>/<code>/` 一个目录。要加新语音（比如语音包 zip 里的德语 Conrad）：

1. 把目录放到 `~/Library/Application Support/mstts/voices/de-DE-ConradNeural/`；
2. 在 `src/voices.cpp` 的 `buildVoices()` 里加一行（code、短名、语言、性别、`Tokens.xml`
   里的 Name、以及该包的 license）；
3. `make`，然后 `./build/mstts doctor` 验证。

注意不同语音包的 license 不一样（大部分共用设备 license，`zh-TW-YunJheNeural` 用自带 inline key），
换错会报 `WRONG_DECRYPTION_KEY`。

## 已知限制

- 只支持 macOS 11+（用到的 SDK dylib 是 1.53 的 macOS 构建）。
- **采样率只有 24kHz**：这不是本工程的偷懒，是离线（Embedded）引擎的硬限制。
  39 种 `SpeechSynthesisOutputFormat` 逐一试过，只有 24k PCM 能用；请求 44.1k/48k 会直接报
  `EMBEDDED_TTS_ERROR_AUDIO_FORMAT_NOT_SUPPORTED: Request sample rate (48000) is not supported,
  supported sample rate is 24000`。语音包本身也是 24k 训出来的，上采样到 48k 只是把每个样本
  插值一遍，不会多出任何信息，所以这里不做假高清。想复现：

  ```bash
  make probe-formats
  ```

  代码里也不再有散落的 `24000`：格式只在 `Engine::kSampleRate/kChannels/kBitsPerSample`
  定义一次，WAV/HTTP/播放器全部按传入值工作（单测会验证 24k/44.1k/48k 的 WAV 头都正确，
  证明这一层没有绑死采样率）。
- 语速/音调走 SSML `prosody`，别超过 ±50% 太多，否则会有金属感。
- **语言要匹配**：用英文语音读中文会合成出空音频（直接报错提示），反之亦然。
  中文语音（晓晓/云希）读英文单词可以，会带口音。
- 微软 SDK 内部有全局状态，合成是串行的（`engine.cpp` 里加了锁）。53× 实时，
  串行对个人使用足够；HTTP 服务里多个请求会排队。
- 只做 TTS，不含语音识别。

## 排错

| 现象 | 处理 |
|---|---|
| `语音包缺失：...` | 跑 `./scripts/install-voices.sh`，或 `--voices <目录>` 指到已有语音包 |
| `Invalid embedded speech synthesis model key` | license 与语音包不匹配；`MSTTS_LICENSE=Key:...` 覆盖，或看 `docs/原理.md` |
| `dyld: Library not loaded: @rpath/libMicrosoft...` | `build/lib/` 被删了：重新 `make`；安装场景用 `make install` |
| 播放没声音 | 系统输出设备/音量；先 `-o out.wav` 用 QuickTime 确认能出声 |
| 合成出来没声音但也不报错 | 文本语言和语音语言不匹配，换对应语种语音 |

## 许可与用途

- 微软的语音模型、SDK 各有其许可条款（语音包内 `AppxManifest.xml` / 微软 EULA）。
  本工程只是把车载/安卓上的离线语音接到 macOS，**仅供个人学习自用，不要分发语音包或商用**。
- 工程自身的代码（src/、scripts/、tools/）随便改。

细节（为什么 macOS 能跑、模型怎么加密的、license 从哪来）见 [docs/原理.md](docs/原理.md)。
