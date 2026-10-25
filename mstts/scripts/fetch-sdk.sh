#!/usr/bin/env bash
# 从 nuget.org 拉取微软语音 SDK 的 macOS 原生库与头文件，做成 universal (arm64+x86_64) 放到
# third_party/ms-speech/。只下 3 个包：
#   Microsoft.CognitiveServices.Speech                       (core)
#   Microsoft.CognitiveServices.Speech.Extension.Embedded.TTS (离线神经 TTS 扩展)
#   Microsoft.CognitiveServices.Speech.Extension.ONNX.Runtime (onnxruntime 扩展)
#
# 注意：1.53.0 的 Embedded.TTS 在 nuspec 里声明依赖 ONNX.Runtime 1.53.0，
# 但 nuget 上并没有发布 1.53.0 的 ONNX.Runtime（最高 1.52.0），实测 1.52.0 可正常配合。
set -euo pipefail

CORE_VER="${CORE_VER:-1.53.0}"
TTS_VER="${TTS_VER:-1.53.0}"
ORT_VER="${ORT_VER:-1.52.0}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/tmp/sdk-download"
OUT="$ROOT/third_party/ms-speech"
mkdir -p "$WORK" "$OUT/include" "$OUT/lib"

fetch() { # $1=package id  $2=version
  local id="$1" ver="$2" slug
  slug="$(echo "$id" | tr '[:upper:]' '[:lower:]')"
  local dst="$WORK/$slug-$ver.nupkg"
  if [ -s "$dst" ] && unzip -tqq "$dst" >/dev/null 2>&1; then
    echo "  cached  $dst"
    return
  fi
  echo "  fetch   $slug $ver"
  curl -fL --retry 3 --retry-delay 2 -o "$dst" \
    "https://api.nuget.org/v3-flatcontainer/$slug/$ver/$slug.$ver.nupkg"
}

echo "== 下载 nuget 包"
fetch Microsoft.CognitiveServices.Speech "$CORE_VER"
fetch Microsoft.CognitiveServices.Speech.Extension.Embedded.TTS "$TTS_VER"
fetch Microsoft.CognitiveServices.Speech.Extension.ONNX.Runtime "$ORT_VER"

echo "== 解包"
mkdir -p "$WORK/x"
unzip -o -q "$WORK/microsoft.cognitiveservices.speech-$CORE_VER.nupkg" \
  'runtimes/osx-arm64/native/*' 'runtimes/osx-x64/native/*' 'build/native/include/*' -d "$WORK/x"
unzip -o -q "$WORK/microsoft.cognitiveservices.speech.extension.embedded.tts-$TTS_VER.nupkg" \
  'runtimes/osx-arm64/native/*' 'runtimes/osx-x64/native/*' -d "$WORK/x"
unzip -o -q "$WORK/microsoft.cognitiveservices.speech.extension.onnx.runtime-$ORT_VER.nupkg" \
  'runtimes/osx-arm64/native/*' 'runtimes/osx-x64/native/*' -d "$WORK/x"

echo "== 合成 universal 二进制"
for name in libMicrosoft.CognitiveServices.Speech.core.dylib \
            libMicrosoft.CognitiveServices.Speech.extension.embedded.tts.dylib \
            libMicrosoft.CognitiveServices.Speech.extension.onnxruntime.dylib; do
  lipo -create \
    "$WORK/x/runtimes/osx-arm64/native/$name" \
    "$WORK/x/runtimes/osx-x64/native/$name" \
    -output "$OUT/lib/$name"
  lipo -archs "$OUT/lib/$name" | sed "s|^|  $name: |"
done

echo "== 头文件"
rsync -a "$WORK/x/build/native/include/" "$OUT/include/"

cat > "$OUT/VERSION" <<EOF
core=$CORE_VER
embedded_tts=$TTS_VER
onnx_runtime=$ORT_VER
fetched=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

echo "== 完成：$OUT"
du -sh "$OUT"/* 2>/dev/null || true
