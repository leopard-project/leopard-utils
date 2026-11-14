# PROJECT KNOWLEDGE BASE — leopard-utils

LeopardOS core-utils 合集：单用途用户态 CLI 工具统一仓库，由 4 个原单仓合并而来
（poweroff / initctl / lpdisk=leopard-disk / mkfs-fat32 / mkfs-ext4 / mount / umount / sync / reboot）。

## 打包模型（单包，coreutils 惯例）

一个仓库打**一个包** `leopard-utils`，内含全部工具二进制，install 到 `apps/system/`。
不按工具拆包。

## 工具与构建

统一构建脚本 `gen_ninja.py`，`TOOLS` 列表驱动。每个工具：

- 源码：`<dir>/main.c`（单文件，8 空格缩进，c-coding-style）
- 二进制：`out/<elf>.elf`

    python3 gen_ninja.py && ninja build   # 全部
    ninja pack                            # 单包 leopard-utils
    ninja build-<dir>                     # 单工具

- 静态 ELF，依赖 `../XJ380-SDK`（SDK 目录名若不同，改 gen_ninja.py 顶部 SDK 变量）。
- 新增工具：在 `TOOLS` 加一项（dir/elf/install）+ 建 `<dir>/main.c` 即可，自动进单包。

## 各工具要点

### poweroff
关机/重启。调 `xapi_PowerAction(XPOWER_SHUTDOWN/XPOWER_REBOOT)`，内核侧
`XAPI_POWER_ACTION` 已接线 `power_shutdown()`/`power_reboot()`（`driver/power.cpp`）。

### initctl
initrc 服务控制（`list`/`enable <svc>`/`disable <svc>`），操作 `/system/config/initrc`
（leopard-rc 启动时读取）。内置服务表含 `leopard-getty` → `/apps/leopard/leopard-getty.elf`。

### lpdisk（原 leopard-disk）
用户态 GPT/MBR 分区工具（gdisk 式）。红线：
- **分区表 = 文件**：直接 open `/dev/<dev>` 按 LBA 读写，不新增内核分区 syscall。
- **只依赖内核既有原语**：块设备节点 + LBA 读写 + 重扫 ioctl（`IOPARTSCAN`）。

### mount / umount / sync / reboot
- `mount [-t fatfs|ext4] device mountpoint`、`umount mountpoint`，直接走 SYS_MOUNT/SYS_UMOUNT2。
- `sync` 走 SYS_SYNC(162)；`reboot` 走 `xapi_PowerAction(XPOWER_REBOOT)`。
- 这些是系统管理命令；普通文件命令在 `leopard-coreutils`。

### mkfs-fat32
用户态 FAT32 格式化。红线：
- 直接 open `/dev/<dev>` 按 LBA 写 FAT32 结构，不碰分区表（那是 lpdisk 的事），不依赖内核 mkfs API。
- 默认 512 字节扇区，簇大小按卷大小自动选（< 8GiB 用 4KiB 簇）。

## RED LINES

- 单包模型：一个仓库一个 `leopard-utils` 包，不按工具拆包。
- 每个工具单用途，不互相耦合；新工具优先并进本仓，避免再零散单仓。
- 只依赖内核既有原语，不新增 syscall；不自己提权。
- 内核只收原语，业务全在用户态工具里。
- LICENSE 统一 GPL v2（poweroff 原为 Apache 2.0，已随合并统一为 GPL v2）。
