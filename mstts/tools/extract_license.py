#!/usr/bin/env python3
"""从 Android 版 libspeexdsp.so 中取出「设备 license」字符串。

背景：MultiTTS 的离线微软语音包（*.bin / *.dat）是用 license 派生的密钥做过加密的，
官方 macOS / Android 运行时都需要同一把钥匙才能解密出 ONNX 模型。
Android 侧由 libspeexdsp.so 里的
    Java_org_nobody_multitts_tts_jni_SpeexBridge_getLicense(JNIEnv*, jclass, jint)
现场解出这串字符（里层是控制流平坦化 + MBA 混淆的字符串还原）。

本脚本用 Unicorn 直接把这段 ARM64 代码跑起来，hook JNI 的 NewStringUTF 拿到结果，
不需要 Android 设备，也不需要手工逆推混淆算法。

用法：
    python3 tools/extract_license.py <libspeexdsp.so 路径> [type]
        type = 0 (默认，返回 license 密钥)
        type = 1 / 2（返回对应语种的授权声明文本）

依赖：
    python3 -m pip install unicorn pyelftools

输出：
    例如 ZCjZ7nHDSLvf4gpELteM4AnzaWUjTpn7UkV7D@vvksl0w1SNgon6d1905WANbktDc9S39oaA4r29HJNayXvTq8fJsq
    （mstts 内部会自动补 "Key:" 前缀）
"""
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.arm64_const import *

SYMBOL = "Java_org_nobody_multitts_tts_jni_SpeexBridge_getLicense"

STUB_BASE = 0x40000000   # 导入函数桩
JNI_BASE = 0x41000000    # JNI 虚表桩
HEAP_BASE = 0x50000000
STACK_BASE = 0x60000000
STACK_SIZE = 0x00100000
ENV_FAKE = 0x70000000


class Runner:
    def __init__(self, path, arg_type=0):
        self.arg_type = arg_type
        self.log = []
        self.result = None
        self._load(path)
        self._setup()

    def _load(self, path):
        with open(path, "rb") as f:
            elf = ELFFile(f)
            self.segs = []
            for p in elf.iter_segments():
                if p["p_type"] == "PT_LOAD":
                    self.segs.append((p["p_vaddr"], p["p_filesz"], p["p_memsz"], p.data()))
            dyn = elf.get_section_by_name(".dynsym")
            self.syms, self.values = [], []
            for s in dyn.iter_symbols():
                self.syms.append(s.name)
                self.values.append(s["st_value"])
            self.func = dict(zip(self.syms, self.values)).get(SYMBOL)
            if not self.func:
                raise SystemExit(f"找不到符号 {SYMBOL}")
            self.relocs = []
            for secname in (".rela.dyn", ".rela.plt"):
                sec = elf.get_section_by_name(secname)
                if not sec:
                    continue
                for r in sec.iter_relocations():
                    self.relocs.append((r["r_offset"], r["r_info_sym"]))

    def _setup(self):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        lo = min(s[0] for s in self.segs)
        hi = max(s[0] + s[2] for s in self.segs)
        base = lo & ~0xFFF
        size = ((hi - base) + 0x4000 + 0xFFF) & ~0xFFF
        self.uc.mem_map(base, size)
        for vaddr, fsz, msz, data in self.segs:
            if fsz:
                self.uc.mem_write(vaddr, data)
            if msz > fsz:
                self.uc.mem_write(vaddr + fsz, b"\x00" * (msz - fsz))
        self.uc.mem_map(STUB_BASE, 0x10000)
        self.uc.mem_map(JNI_BASE, 0x2000)
        self.uc.mem_map(HEAP_BASE, 0x100000)
        self.uc.mem_map(STACK_BASE, STACK_SIZE)
        self.uc.mem_map(ENV_FAKE, 0x1000)

        # 每个导入符号一个桩地址；本 .so 自己定义的符号指回真实地址
        self.stub_names = {}
        for i, name in enumerate(self.syms):
            if not name:
                continue
            addr = STUB_BASE + i * 0x10
            self.stub_names[addr] = name
            for off, symi in self.relocs:
                if symi == i:
                    target = self.values[i] or addr
                    self.uc.mem_write(off, struct.pack("<Q", target))

        self.heap = HEAP_BASE + 0x1000
        self.uc.hook_add(UC_HOOK_CODE, self._hook_code)

    # ---- 工具 ----
    def _str(self, addr, limit=4096):
        out = bytearray()
        for i in range(limit):
            c = self.uc.mem_read(addr + i, 1)[0]
            if c == 0:
                break
            out.append(c)
        return bytes(out)

    def _alloc(self, n):
        p = self.heap
        self.heap = (p + n + 15) & ~15
        return p

    # ---- 导入 / JNI 桩 ----
    def _hook_code(self, uc, address, size, _):
        if JNI_BASE <= address < JNI_BASE + 0x2000:
            idx = (address - JNI_BASE) // 0x10
            if idx == 167:  # NewStringUTF
                self.result = self._str(uc.reg_read(UC_ARM64_REG_X1))
                uc.emu_stop()
            else:
                self.log.append(f"jni[{idx}]")
                uc.reg_write(UC_ARM64_REG_X0, 0)
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
            return
        if not (STUB_BASE <= address < STUB_BASE + 0x10000):
            return
        name = self.stub_names.get(address, "?")
        lr = uc.reg_read(UC_ARM64_REG_LR)
        x0, x1, x2 = (uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        ret = 0
        if name == "malloc":
            ret = self._alloc(x0 or 1)
        elif name == "calloc":
            n = (x0 or 1) * (x1 or 1)
            ret = self._alloc(n)
            uc.mem_write(ret, b"\x00" * n)
        elif name == "realloc":
            ret = x0 or self._alloc(x1 or 1)
        elif name == "free":
            pass
        elif name in ("memcpy", "memmove"):
            uc.mem_write(x0, bytes(uc.mem_read(x1, x2)))
            ret = x0
        elif name == "memset":
            uc.mem_write(x0, bytes([x1 & 0xFF]) * x2)
            ret = x0
        elif name == "memcmp":
            a, b = bytes(uc.mem_read(x0, x2)), bytes(uc.mem_read(x1, x2))
            ret = 0 if a == b else (-1 if a < b else 1)
        elif name == "strlen":
            ret = len(self._str(x0))
        elif name in ("pthread_mutex_lock", "pthread_mutex_unlock", "__cxa_atexit", "__cxa_finalize"):
            pass
        elif name == "time":
            uc.mem_write(x0, struct.pack("<q", 1700000000))
            ret = 1700000000
        else:
            self.log.append(f"import {name}({x0:#x},{x1:#x},{x2:#x})")
        uc.reg_write(UC_ARM64_REG_X0, ret)
        uc.reg_write(UC_ARM64_REG_PC, lr)

    def run(self):
        uc = self.uc
        sp = STACK_BASE + STACK_SIZE - 0x1000
        # 假 JNIEnv：env->functions 指向虚表，每项都指向 JNI 桩
        uc.mem_write(ENV_FAKE, struct.pack("<Q", ENV_FAKE + 0x100))
        for i in range(256):
            uc.mem_write(ENV_FAKE + 0x100 + i * 8, struct.pack("<Q", JNI_BASE + i * 0x10))
        uc.reg_write(UC_ARM64_REG_X0, ENV_FAKE)
        uc.reg_write(UC_ARM64_REG_X1, 0)
        uc.reg_write(UC_ARM64_REG_X2, self.arg_type)  # 第三个参数：type
        uc.reg_write(UC_ARM64_REG_SP, sp)
        uc.emu_start(self.func, 0, 0, 0)
        return self.result


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    so = sys.argv[1]
    arg_type = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    r = Runner(so, arg_type)
    s = r.run()
    if not s:
        print("没取到字符串；可加 -v 看日志", file=sys.stderr)
        return 1
    print(s.decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
