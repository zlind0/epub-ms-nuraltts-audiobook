# EPUB电子书音频生成器(epub-mstts-audio)

macOS 上的离线电子书转有声书工具(Tauri):打开一本 EPUB → 左侧折叠式章节多选 →
右侧预览正文/试听 → 下方选输出目录/音色/语速/音调/线程池/码率 → 每个 heading
生成一个 `0001-章节标题.m4a`。

TTS 引擎是 App 包内自带的微软离线神经语音(晓晓/Jenny,
24kHz/16bit/单声道,约 53× 实时)。音频编码按 tts 库输出的最高规格直出:
**不重采样**(保持 24kHz)、**不立体声化**(保持单声道),用系统 `afconvert`
压成 AAC-LC m4a,默认 64k(界面可调 32–128k);标签(title/album/artist/track)
由内置 MP4 box 读写直接写入,不重编码、不依赖 ffmpeg。

## 运行前准备(已内置,无需操作)

TTS 引擎 **`mstts` 由内嵌源码(`mstts/`,原 `mstts-macos` 项目)与 App 一同编译**,
构建脚本见 `scripts/build-mstts.sh`(只需 Xcode 命令行工具里的 clang++,
微软 SDK 已 vendor 在 `mstts/third_party`)。**晓晓 / Jenny 两个语音包**随包
携带(`src-tauri/resources/voices/`)。外部只剩两项系统依赖:**`afconvert`**
(macOS 自带,m4a 编码必需)与 **WebView**(Tauri 壳,系统自带)。首次启动无需
装语音包;本机如装过完整 9 语音(`~/Library/Application Support/mstts/voices`)
会自动沿用,`$MSTTS_VOICES` 可强制指定目录,`$MSTTS_BIN` 可强制指定 mstts。

## 开发 / 打包

```bash
npm install
npm run tauri dev     # 开发运行
npm run tauri build   # 打包 .app/.dmg
```

## 链路(与 aurora EbookExport 同口径)

- 解析:NCX > EPUB3 nav > 正文 h1/h2 > 逐章;空章节(封面图页等)直接丢弃重映射。
- 切段:每条目录覆盖直到下一条,段首播报标题;空段跳过,不产空文件。
- 并行:段间串行、段内以 `<p>` 为维度并行——起 N 个 `mstts serve` 子进程组成
  TTS 池(微软 SDK 进程内串行,多进程即多路并行;实测 2 路 0.43s 出 13s 音频),
  每块独立合成 raw PCM → 按序拼接 → 写 WAV → `afconvert` 直出 m4a。
- 输出:默认输出目录即 epub 书本所在目录,转换时在其下新建与书文件名
  (basename,去扩展名)同名的文件夹,所选章节全放里面。
- 折叠:自适应深度,默认展开可见条目 ≤200(与 aurora 目录页同一算法:
  取满足条件的最大 level 深度);转换中每条目录的背景即该条进度条。
