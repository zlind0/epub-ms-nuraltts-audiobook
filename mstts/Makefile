# mstts —— macOS 离线微软神经 TTS
#
#   make            编译出 build/mstts
#   make test       跑单测（纯函数，秒级）
#   make doctor     逐语音自检（需要语音包）
#   make probe-formats  逐格式探测引擎真实输出（会跑 39 种格式，约 20s）
#   make install    装到 /usr/local（可用 PREFIX=... 改）
#
# 依赖只有 Xcode 命令行工具里的 clang++。清理请自行删除 build/ 目录。

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  ?=
PREFIX   ?= /usr/local

MS       := third_party/ms-speech
INCLUDES := -Isrc -I$(MS)/include/cxx_api -I$(MS)/include/c_api
MS_LIBS  := $(MS)/lib/libMicrosoft.CognitiveServices.Speech.core.dylib \
            $(MS)/lib/libMicrosoft.CognitiveServices.Speech.extension.embedded.tts.dylib \
            $(MS)/lib/libMicrosoft.CognitiveServices.Speech.extension.onnxruntime.dylib
FRAMEWORKS := -framework AudioToolbox -framework CoreFoundation
LIBS     := $(MS_LIBS) $(FRAMEWORKS) -lpthread
RPATH    := -Wl,-rpath,@executable_path/lib

CORE_SRC := src/engine.cpp src/http_server.cpp src/json.cpp src/player.cpp \
            src/ssml.cpp src/voices.cpp src/wav.cpp
CORE_OBJ := $(CORE_SRC:src/%.cpp=build/obj/%.o)
DEP      := $(CORE_OBJ:.o=.d) build/obj/main.d build/obj/unit.d

.PHONY: all test doctor install probe-formats

all: build/mstts

build/obj/%.o: src/%.cpp
	@mkdir -p build/obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

build/obj/unit.o: tests/unit.cpp
	@mkdir -p build/obj
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

build/mstts: build/obj/main.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) $(RPATH) -o $@
	@mkdir -p build/lib
	@cp -f $(MS_LIBS) build/lib/
	@echo "✓ $@"

build/mstts_tests: build/obj/unit.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) $(RPATH) -o $@
	@echo "✓ $@"

test: build/mstts_tests
	@./build/mstts_tests

# 逐格式探测引擎真实输出（为什么只有 24k）
build/probe_formats: tools/probe_formats.cpp build/obj/voices.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) $^ $(MS_LIBS) $(FRAMEWORKS) $(RPATH) -o $@
	@mkdir -p build/lib
	@cp -f $(MS_LIBS) build/lib/
	@echo "✓ $@"

probe-formats: build/probe_formats
	@./build/probe_formats $(ARGS)

doctor: build/mstts
	@./build/mstts doctor

install: build/mstts
	@mkdir -p $(PREFIX)/bin $(PREFIX)/lib
	@cp -f build/mstts $(PREFIX)/bin/mstts
	@cp -f $(MS_LIBS) $(PREFIX)/lib/
	@install_name_tool -add_rpath @executable_path/../lib $(PREFIX)/bin/mstts 2>/dev/null || true
	@echo "✓ 安装到 $(PREFIX)/bin/mstts（依赖库在 $(PREFIX)/lib）"

-include $(DEP)
