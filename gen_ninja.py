#!/usr/bin/env python3
"""leopard-utils 统一构建 + xbps 打包（core-utils 合集）。

一个仓库、一个包、多个二进制（GNU coreutils 惯例）：
    out/<elf>.elf                        各工具二进制
    out/leopard-utils-1.0_1.x86_64.xbps  单包，含全部工具

install 路径均在 apps/system/ 下，与各工具历史安装位置一致。
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent          # leopard-utils/
OUT  = ROOT / "out"

# 每个工具的元数据：dir=源码目录，elf=二进制名，install=包内安装路径
TOOLS = [
    dict(dir="poweroff",   elf="poweroff.elf",   install="apps/system/poweroff.elf"),
    dict(dir="clear",      elf="clear.elf",      install="apps/system/clear.elf"),
    dict(dir="initctl",    elf="initctl.elf",    install="apps/system/initctl.elf"),
    dict(dir="lpdisk",     elf="lpdisk.elf",     install="apps/system/lpdisk.elf"),
    dict(dir="mkfs-fat32", elf="mkfs-fat32.elf", install="apps/system/mkfs-fat32.elf"),
    dict(dir="mkfs-ext4",  elf="mkfs-ext4.elf",  install="apps/system/mkfs-ext4.elf",
         extra=["format.cpp"]),
    dict(dir="mount",      elf="mount.elf",      install="apps/system/mount.elf"),
    dict(dir="umount",     elf="umount.elf",     install="apps/system/umount.elf"),
    dict(dir="sync",       elf="sync.elf",       install="apps/system/sync.elf"),
    dict(dir="reboot",     elf="reboot.elf",     install="apps/system/reboot.elf"),
]

PKGNAME = "leopard-utils"
PKGVER  = "1.0_2"
ARCH    = "x86_64"
DESC    = "LeopardOS core utilities"

# XACT CRT（../leopard-sdk/out/crt）：启动对象 + 静态运行时 + 分配器
CRT_ROOT = ROOT / ".." / "leopard-sdk"
INC      = CRT_ROOT / "out" / "crt" / "include"
LIB      = CRT_ROOT / "out" / "crt" / "lib" / "x86_64"
CRT_OBJS = [LIB / "crt0.o", LIB / "start.o"]
CRT_LIBS = [LIB / "libxact.a", LIB / "liballoc.a"]

def rel(p):
    p = Path(p)
    if p.is_absolute():
        try:
            return str(p.relative_to(ROOT))
        except ValueError:
            return str(p)
    return str(p)


def esc(p):
    return rel(p).replace("$", "$$").replace(":", "$:").replace(" ", "$ ")


def main():
    cc   = os.environ.get("CC",  "clang")
    cxx  = os.environ.get("CXX", "clang++")
    ld   = os.environ.get("LD",  "ld")
    opt  = "-O" + os.environ.get("OPT_LEVEL", "2")
    col  = os.environ.get("DIAGNOSTICS_COLOR", "-fdiagnostics-color=always")

    common = (f"{col} {opt} -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign "
              "-fno-stack-protector -fno-exceptions -fno-strict-aliasing -fshort-wchar -nostdinc")
    c_flags   = f"{common} -std=c11   -I {rel(INC)} -I . -MMD -MP"
    cpp_flags = f"{common} -std=c++11 -I {rel(INC)} -I . -Wno-write-strings -MMD -MP"

    L = ["# leopard-utils 构建 + xbps 打包", "ninja_required_version = 1.10",
         f"cc = {cc}", f"cxx = {cxx}", f"ld = {ld}",
         f"c_flags = {c_flags}", f"cpp_flags = {cpp_flags}", ""]
    L += [
        "rule cc",
        "  command = mkdir -p $$(dirname $out) && $cc $c_flags -MF $out.d -c $in -o $out",
        "  depfile = $out.d", "  deps = gcc", "",
        "rule cxx",
        "  command = mkdir -p $$(dirname $out) && $cxx $cpp_flags -MF $out.d -c $in -o $out",
        "  depfile = $out.d", "  deps = gcc", "",
        "rule link",
        "  command = $ld -Ttext=0x200000 $in -o $out", "",
    ]

    elfs = []
    for t in TOOLS:
        main = ROOT / t["dir"] / "main.c"
        if not main.exists():
            main = ROOT / t["dir"] / "main.cpp"
        objs = []
        sources = [main] + [ROOT / t["dir"] / extra for extra in t.get("extra", [])]
        for src in sources:
            obj = OUT / t["dir"] / (src.name + ".o")
            rule = "cc" if str(src).endswith(".c") else "cxx"
            L += [f"build {esc(obj)}: {rule} {esc(src)}", ""]
            objs.append(obj)

        elf = OUT / t["elf"]
        ins = CRT_OBJS + objs + CRT_LIBS
        L += [f"build {esc(elf)}: link {' '.join(esc(o) for o in ins)}", ""]
        L += [f"build build-{t['dir']}: phony {esc(elf)}"]
        elfs.append(elf)
        L += [""]

    pkg_out = OUT / f"{PKGNAME}-{PKGVER}.{ARCH}.xbps"
    args = f"-A {ARCH} -n {PKGNAME}-{PKGVER} -s \"{DESC}\""
    files = [(OUT / t["elf"], t["install"]) for t in TOOLS]

    stage = ".pkg"
    cmd = f"rm -rf {stage}"
    for src, dst in files:
        cmd += f" && mkdir -p {stage}/$$(dirname {dst}) && cp {esc(src)} {stage}/{dst}"
    cmd += f" && cd out && xbps-create --compression gzip {args} ../{stage}"

    L += ["rule package", f"  command = {cmd}", ""]
    L += [f"build {esc(pkg_out)}: package {' '.join(esc(s) for s, _ in files)}"]
    L += [f"build pack: phony {esc(pkg_out)}"]

    L += ["build build: phony " + " ".join(f"build-{t['dir']}" for t in TOOLS)]
    L += ["default build"]

    L += ["rule clean", "  command = rm -rf out .pkg", "build clean: clean"]

    (ROOT / "build.ninja").write_text("\n".join(L) + "\n", encoding="utf-8")
    print("已生成 build.ninja（leopard-utils 单包）。用法：ninja build / ninja pack")


if __name__ == "__main__":
    main()
