# leopard-utils

LeopardOS core-utils 合集：集中收纳**单用途用户态 CLI 工具**（UNIX 思维：一件事做好）。

一个仓库、一个包、多个二进制（GNU coreutils 惯例）。

## 工具清单

| 目录 | 二进制 | 功能 |
|---|---|---|
| `poweroff/` | `poweroff.elf` | 关机/重启（`poweroff [-r]`） |
| `initctl/` | `initctl.elf` | initrc 服务控制（`list`/`enable`/`disable`） |
| `lpdisk/` | `lpdisk.elf` | GPT/MBR 分区工具（gdisk 式） |
| `mkfs-fat32/` | `mkfs-fat32.elf` | FAT32 格式化 |

全部装进 `apps/system/`，打成一个包 `leopard-utils`。

## 构建

```sh
python3 gen_ninja.py && ninja build      # 构建全部 4 个工具
ninja pack                               # 打单包 leopard-utils
# 也可单工具构建：
ninja build-poweroff / build-initctl / build-lpdisk / build-mkfs-fat32
```

产物：`out/*.elf` + `out/leopard-utils-1.0_1.x86_64.xbps`。依赖兄弟目录 `../leopard-sdk`（静态 ELF，无需运行时库）。

## 设计红线

- **每个工具只做一件事**，保持 UNIX 风格，不互相耦合。
- **只依赖内核既有原语**，不新增 syscall（分区表/格式化都直接 open `/dev/<dev>` 按 LBA 读写）。
- **不自己提权**：权限门控将来由内核权限模型负责。
- 内核只收原语，业务全在用户态工具里。
