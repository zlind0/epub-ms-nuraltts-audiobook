#!/bin/bash
# 构建内嵌的 mstts(源码在 mstts/,与 tauri 一同编译),产物同步到
# src-tauri/resources/mstts/(bundler 从这里打进 App)。
# 依赖:Xcode 命令行工具里的 clang++(无其他依赖,SDK 已 vendor 在 mstts/third_party)。
set -e
cd "$(dirname "$0")/.."
make -C mstts
mkdir -p src-tauri/resources/mstts/lib
cp -f mstts/build/mstts src-tauri/resources/mstts/mstts
cp -f mstts/build/lib/*.dylib src-tauri/resources/mstts/lib/
echo "OK mstts -> src-tauri/resources/mstts/"
