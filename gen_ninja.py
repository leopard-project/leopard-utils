#!/usr/bin/env python3
"""leopard-utils 统一构建 + xbps 打包（core-utils 合集）。

一个脚本构建全部工具，产物独立：
    out/<elf>.elf                 各工具二进制
    out/<pkgname>-1.0_1.x86_64.xbps  各工具 xbps 包

对外二进制名与包名保持不变（历史兼容，上层 initrc/getty 零改动）。
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent          # leopard-utils/
SDK  = ROOT / ".." / "leopard-sdk"              # SDK 根（目录名若不同，改这里）
INC  = SDK / "include"
OBJ  = SDK / "obj-tui"
OUT  = ROOT / "out"

# 每个工具的元数据：dir=源码目录，elf=二进制名，pkgname=xbps 包名（历史名不变）
TOOLS = [
    dict(dir="poweroff",    elf="poweroff.elf",    pkgname="poweroff",
         desc="Leopard power off/reboot",           install="apps/system/poweroff.elf"),
    dict(dir="initctl",     elf="initctl.elf",     pkgname="initctl",
         desc="Leopard service manager",             install="apps/system/initctl.elf"),
    dict(dir="lpdisk",      elf="lpdisk.elf",      pkgname="leopard-disk",
         desc="Leopard GPT/MBR partition tool",      install="apps/system/lpdisk.elf"),
    dict(dir="mkfs-fat32",  elf="mkfs-fat32.elf",  pkgname="mkfs-fat32",
         desc="Leopard FAT32 formatter",             install="apps/system/mkfs-fat32.elf"),
]

# SDK 运行时，链接顺序：crt0 入口 → 运行时 → 应用 → liballoc
RUNTIME  = ["crt0.o", "libsys.cpp.o", "xgui_stubs.cpp.o",
            "constart.cpp.o", "xtuiapi.cpp.o", "krlibc.cpp.o"]
LIBALLOC = OBJ / "liballoc-x86_64.a"


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


def emit_package(L, t, elf):
    pkg  = t["pkgname"]
    inst = t["install"]
    out  = OUT / f"{pkg}-1.0_1.x86_64.xbps"

    args = f"-A x86_64 -n {pkg}-1.0_1 -s \"{t['desc']}\""
    files = [(elf, inst)]

    # 每个工具独立 .pkg 目录，避免并发打包互相覆盖
    stage = f".pkg/{t['dir']}"
    cmd = f"rm -rf {stage}"
    for src, dst in files:
        cmd += f" && mkdir -p {stage}/$$(dirname {dst}) && cp {esc(src)} {stage}/{dst}"
    cmd += f" && cd out && xbps-create --compression gzip {args} ../{stage}"

    L += [f"rule package-{t['dir']}", f"  command = {cmd}", ""]
    L += [f"build {esc(out)}: package-{t['dir']} {' '.join(esc(s) for s, _ in files)}"]
    L += [f"build pack-{t['dir']}: phony {esc(out)}"]


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

    all_pkgs = []
    for t in TOOLS:
        src = ROOT / t["dir"] / "main.c"
        obj = OUT / t["dir"] / "main.c.o"
        elf = OUT / t["elf"]

        rule = "cc" if str(src).endswith(".c") else "cxx"
        L += [f"build {esc(obj)}: {rule} {esc(src)}", ""]

        ins = [OBJ / n for n in RUNTIME] + [obj] + [LIBALLOC]
        L += [f"build {esc(elf)}: link {' '.join(esc(o) for o in ins)}", ""]

        emit_package(L, t, elf)
        L += [f"build build-{t['dir']}: phony {esc(elf)}"]
        all_pkgs.append(f"pack-{t['dir']}")
        L += [""]

    L += ["build build: phony " + " ".join(f"build-{t['dir']}" for t in TOOLS)]
    L += ["build pack: phony " + " ".join(all_pkgs)]
    L += ["default build"]

    L += ["rule clean", "  command = rm -rf out .pkg", "build clean: clean"]

    (ROOT / "build.ninja").write_text("\n".join(L) + "\n", encoding="utf-8")
    print("已生成 build.ninja（leopard-utils）。用法：ninja build / ninja pack")


if __name__ == "__main__":
    main()
