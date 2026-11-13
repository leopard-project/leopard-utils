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
SDK  = ROOT / ".." / "leopard-sdk"              # SDK 根（目录名若不同，改这里）
INC  = SDK / "include"
OBJ  = SDK / "obj-tui"
OUT  = ROOT / "out"

# 每个工具的元数据：dir=源码目录，elf=二进制名，install=包内安装路径
TOOLS = [
    dict(dir="poweroff",   elf="poweroff.elf",   install="apps/system/poweroff.elf"),
    dict(dir="initctl",    elf="initctl.elf",    install="apps/system/initctl.elf"),
    dict(dir="lpdisk",     elf="lpdisk.elf",     install="apps/system/lpdisk.elf"),
    dict(dir="mkfs-fat32", elf="mkfs-fat32.elf", install="apps/system/mkfs-fat32.elf"),
]

PKGNAME = "leopard-utils"
PKGVER  = "1.0_1"
ARCH    = "x86_64"
DESC    = "LeopardOS core utilities"

# SDK 运行时，链接顺序：crt0 入口 → 运行时 → 应用 → liballoc
# XACT 2026v5 官方运行时（obj-tui/ 由 leopard-sdk 铺装）
RUNTIME  = ["arch/x86_64/crt0.S.o", "libsys.cpp.o",
            "xtuiapi.cpp.o", "krlibc.cpp.o", "string.cpp.o", "stdlib.cpp.o",
            "stdio.cpp.o", "unistd.cpp.o", "errno.cpp.o", "posix_ret.cpp.o",
            "ctype.cpp.o", "permissions.cpp.o", "posix_fs.cpp.o",
            "posix_time.cpp.o", "posix_ioctl.cpp.o"]
LIBALLOC = OBJ / "liballoc-x86_64.a"
CRT      = SDK / "obj-extra" / "constart.cpp.o"  # Leopard CRT（替换官方 constart）


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
        src = ROOT / t["dir"] / "main.c"
        obj = OUT / t["dir"] / "main.c.o"
        elf = OUT / t["elf"]

        rule = "cc" if str(src).endswith(".c") else "cxx"
        L += [f"build {esc(obj)}: {rule} {esc(src)}", ""]

        ins = [OBJ / n for n in RUNTIME] + [CRT] + [obj] + [LIBALLOC]
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
