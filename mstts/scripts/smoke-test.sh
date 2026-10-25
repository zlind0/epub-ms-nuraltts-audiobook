#!/usr/bin/env bash
# 端到端冒烟：语音表 → 中英文合成 → HTTP 服务。产物落在 tmp/smoke/。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mstts"
OUT="$ROOT/tmp/smoke"
PORT="${PORT:-8798}"
mkdir -p "$OUT"

[ -x "$BIN" ] || { echo "先 make"; exit 1; }

echo "== 1/4 语音表"
"$BIN" list

echo "== 2/4 中文（晓晓）"
"$BIN" say "春眠不觉晓，处处闻啼鸟。夜来风雨声，花落知多少。" -o "$OUT/zh.wav"
afinfo "$OUT/zh.wav" | grep -E "Data format|estimated duration"

echo "== 3/4 英文（Aria）+ 语速 1.2"
"$BIN" say "The quick brown fox jumps over the lazy dog." -v Aria -r 1.2 -o "$OUT/en.wav"
afinfo "$OUT/en.wav" | grep -E "Data format|estimated duration"

echo "== 4/4 HTTP 服务 (:$PORT)"
"$BIN" serve --port "$PORT" > "$OUT/serve.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null; then break; fi
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" && echo
curl -sf -X POST "http://127.0.0.1:$PORT/v1/audio/speech" \
  -H 'Content-Type: application/json' \
  -d '{"input":"通过 HTTP 合成的语音。","voice":"云希","speed":1.0}' \
  -o "$OUT/http.wav" -w "POST /v1/audio/speech → %{http_code} %{size_download} bytes\n"
afinfo "$OUT/http.wav" | grep -E "estimated duration"
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
trap - EXIT

echo
echo "全部通过 ✓  产物在 $OUT"
