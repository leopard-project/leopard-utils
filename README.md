# leopard-utils

LeopardOS core-utils 合集：集中收纳**单用途用户态 CLI 工具**（UNIX 思维：一件事做好）。

每个工具仍是独立二进制、独立 xbps 包，对外名字不变——只是从各自的单仓合并到这里统一构建。

## 工具清单

| 目录 | 二进制 | xbps 包名 | 功能 |
|---|---|---|---|
| `poweroff/` | `poweroff.elf` | `poweroff` | 关机/重启（`poweroff [-r]`） |
| `initctl/` | `initctl.elf` | `initctl` | initrc 服务控制（`list`/`enable`/`disable`） |
| `lpdisk/` | `lpdisk.elf` | `leopard-disk` | GPT/MBR 分区工具（gdisk 式） |
| `mkfs-fat32/` | `mkfs-fat32.elf` | `mkfs-fat32` | FAT32 格式化 |

> 注意 `lpdisk` 的包名仍是 `leopard-disk`（历史名），但二进制叫 `lpdisk`。

## 构建

```sh
python3 gen_ninja.py && ninja build      # 构建全部 4 个工具
ninja pack                               # 打全部 4 个 xbps 包
# 也可单工具：
ninja build-poweroff / pack-poweroff
ninja build-initctl  / pack-initctl
ninja build-lpdisk   / pack-lpdisk
ninja build-mkfs-fat32 / pack-mkfs-fat32
```

产物在 `out/`，依赖兄弟目录 `../leopard-sdk`（静态 ELF，无需运行时库）。

## 设计红线

- **每个工具只做一件事**，保持 UNIX 风格，不互相耦合。
- **只依赖内核既有原语**，不新增 syscall（分区表/格式化都直接 open `/dev/<dev>` 按 LBA 读写）。
- **不自己提权**：权限门控将来由内核权限模型负责。
- 内核只收原语，业务全在用户态工具里。
