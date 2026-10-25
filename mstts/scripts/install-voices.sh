#!/usr/bin/env bash
# 把 9 个微软离线语音包装到 macOS 的默认语音目录。
#
#   ./scripts/install-voices.sh                 # 从 ../multitts-instance 的 assets 复制
#   ./scripts/install-voices.sh --symlink       # 只建符号链接（省 385MB，源目录不能删）
#   ./scripts/install-voices.sh --zip <zip>     # 从「【离线】 微软语音.zip」这类语音包 zip 里取
#   ./scripts/install-voices.sh --src <dir> --dest <dir>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VOICES=(zh-CN-XiaoxiaoNeural zh-CN-YunxiNeural zh-TW-YunJheNeural
        en-US-AriaNeural en-US-JennyNeural en-US-GuyNeural
        en-GB-RyanNeural en-IN-NeerjaNeural en-IN-PrabhatNeural)

DEST="${HOME}/Library/Application Support/mstts/voices"
SRC=""
ZIP=""
MODE="copy"

while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC="$2"; shift 2 ;;
    --dest) DEST="$2"; shift 2 ;;
    --zip) ZIP="$2"; shift 2 ;;
    --symlink) MODE="symlink"; shift ;;
    --copy) MODE="copy"; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$SRC" ] && [ -z "$ZIP" ]; then
  for cand in "$ROOT/../multitts-instance/app/src/main/assets/msvoice" \
              "$ROOT/../multitts/msvoice"; do
    if [ -d "$cand" ]; then SRC="$cand"; break; fi
  done
  if [ -z "$SRC" ]; then
    for cand in "$ROOT/../multitts/【离线】 微软语音.zip"; do
      if [ -f "$cand" ]; then ZIP="$cand"; break; fi
    done
  fi
fi

if [ -z "$SRC" ] && [ -z "$ZIP" ]; then
  echo "找不到语音包来源，请用 --src <目录> 或 --zip <压缩包> 指定" >&2
  exit 1
fi

mkdir -p "$DEST"
echo "目标目录: $DEST"

if [ -n "$ZIP" ]; then
  echo "来源 zip: $ZIP"
  for v in "${VOICES[@]}"; do
    if [ -d "$DEST/$v" ] && [ -f "$DEST/$v/Tokens.xml" ]; then
      echo "  跳过 $v（已存在）"
      continue
    fi
    echo "  解包 $v"
    unzip -o -q "$ZIP" "microsoft/$v/*" -d "$DEST/.unpack"
    mkdir -p "$DEST/$v"
    # zip 里是 microsoft/<voice>/...，去掉一层 microsoft/
    ditto "$DEST/.unpack/microsoft/$v" "$DEST/$v"
  done
  echo "（$DEST/.unpack 是临时目录，可手动删除）"
else
  echo "来源目录: $SRC"
  for v in "${VOICES[@]}"; do
    if [ ! -d "$SRC/$v" ]; then
      echo "  缺少 $v，跳过" >&2
      continue
    fi
    case "$MODE" in
      symlink)
        ln -sfn "$SRC/$v" "$DEST/$v"
        echo "  链接 $v"
        ;;
      *)
        mkdir -p "$DEST/$v"
        ditto "$SRC/$v" "$DEST/$v"
        echo "  复制 $v"
        ;;
    esac
  done
fi

echo
echo "完成，自检：  ./build/mstts doctor --voices \"$DEST\""
