/*
 * mkfs-fat32 — LeopardOS 用户态 FAT32 格式化工具（UNIX 思维：一件事做好）。
 *
 * 直接 open /dev/<dev> 后按 LBA 写 FAT32 结构：
 *   引导扇区(BPB) + FSInfo + 备份引导扇区 + 双 FAT + 根目录簇。
 * 不碰分区表（那是 lpdisk 的事），不依赖内核 mkfs API。
 * 原生 XAPI + enter_syscall 薄封装，不用 musl。
 */
#include "xtuiapi.h"

#define SECTOR_SIZE         512U
#define RESERVED_SECTORS    32U
#define NUM_FATS            2U
#define ROOT_CLUSTER        2U
#define FSINFO_SECTOR       1U
#define BACKUP_BOOT_SECTOR  6U
#define O_RDWR              2
#define SEEK_SET            0
#define SEEK_END            2

/* ---- FAT32 BPB（90 字节，打包）---- */
typedef struct {
    uint8_t  jump[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t sectors_per_fat16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint32_t sectors_per_fat32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed)) bpb_t;

/* ---- POSIX 薄封装 ---- */
static int fd_open(const char *path, uint64_t flags) {
    return (int)(int64_t)enter_syscall(SYS_OPEN, (uint64_t)path, flags, 0644, 0, 0, 0);
}
static int64_t fd_write(int fd, const void *buf, uint64_t n) {
    return (int64_t)enter_syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, n, 0, 0, 0);
}
static int64_t fd_lseek(int fd, int64_t off, uint64_t whence) {
    return (int64_t)enter_syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)off, whence, 0, 0, 0);
}
static int fd_close(int fd) {
    return (int)enter_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
}

static int sector_write(int fd, uint64_t lba, const void *buf) {
    if (fd_lseek(fd, (int64_t)(lba * SECTOR_SIZE), SEEK_SET) < 0) return -1;
    return (fd_write(fd, buf, SECTOR_SIZE) == (int64_t)SECTOR_SIZE) ? 0 : -1;
}

static uint64_t disk_size_bytes(int fd) {
    int64_t sz = fd_lseek(fd, 0, SEEK_END);
    return (sz < 0) ? 0 : (uint64_t)sz;
}

/* ---- PRNG（卷 ID）---- */
static uint64_t rng_state;
static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

/* ---- 工具 ---- */
static void pad11(uint8_t dst[11], const char *src) {
    memset(dst, ' ', 11);
    if (src == NULL) return;
    size_t n = strlen(src);
    if (n > 11) n = 11;
    for (size_t i = 0; i < n; i++) dst[i] = (uint8_t)src[i];
}

static uint8_t cluster_sectors(uint64_t total_sectors) {
    uint64_t mb = total_sectors >> 11; /* sectors / 2048 = MiB（512B 扇区）*/
    if (mb < 260)  return 8;   /* < 260MiB → 4KiB 簇 */
    if (mb < 8192) return 8;   /* < 8GiB   → 4KiB 簇 */
    if (mb < 16384) return 16; /* < 16GiB  → 8KiB 簇 */
    if (mb < 32768) return 32; /* < 32GiB  → 16KiB 簇 */
    return 64;                  /* ≥ 32GiB  → 32KiB 簇 */
}

int main(int argc, char *argv[], char *envp[]) {
    (void)envp;

    if (argc < 2) {
        xapi_Printf("usage: mkfs-fat32 <dev> [label]\n");
        xapi_Printf("  format /dev/<dev> as FAT32 (dev e.g. sata1, sata1p0)\n");
        return 1;
    }
    const char *dev   = argv[1];
    const char *label = (argc > 2) ? argv[2] : NULL;

    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", dev);
    int fd = fd_open(path, O_RDWR);
    if (fd < 0) { xapi_Printf("mkfs-fat32: open %s failed\n", path); return 1; }

    uint64_t size = disk_size_bytes(fd);
    if (size < (16 * 1024 * 1024)) {
        xapi_Printf("mkfs-fat32: volume too small (%llu bytes)\n", (unsigned long long)size);
        fd_close(fd);
        return 1;
    }
    uint64_t total_sectors = size / SECTOR_SIZE;
    uint8_t  spc           = cluster_sectors(total_sectors);
    uint64_t fatsz          = 0;
    uint64_t total_clusters = 0;

    /* FAT32 规范要求 >= 65525 簇；小分区必须缩小簇。64 MiB ESP 用 512B 簇
     * 才有约 130k 簇；否则 FatFs 会按 FAT16 解析并报 FR_NO_FILESYSTEM。 */
    for (;;) {
        for (int pass = 0; pass < 3; pass++) {
            uint64_t data_sectors = total_sectors - RESERVED_SECTORS - (uint64_t)NUM_FATS * fatsz;
            total_clusters = data_sectors / spc;
            fatsz = ((total_clusters + 2) * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
        }
        if (total_clusters >= 65525) break;
        fatsz = 0;
        if (spc == 1) {
            xapi_Printf("mkfs-fat32: volume too small for FAT32 (needs >= ~32 MiB)\n");
            fd_close(fd);
            return 1;
        }
        spc >>= 1;
    }

    xapi_Printf("mkfs-fat32: %s  %llu bytes  spc=%u  clusters=%llu  fatsz=%llu\n",
                path, (unsigned long long)size, spc,
                (unsigned long long)total_clusters, (unsigned long long)fatsz);

    rng_state = (uint64_t)enter_syscall(XAPI_GET_TIME_NANO, 0, 0, 0, 0, 0, 0);
    if (rng_state == 0) rng_state = 0x9E3779B97F4A7C15ULL;
    uint32_t volid = (uint32_t)rng_next();

    /* 1. 引导扇区（BPB） */
    uint8_t boot[SECTOR_SIZE];
    memset(boot, 0, sizeof(boot));
    bpb_t *b = (bpb_t *)boot;
    b->jump[0] = 0xEB; b->jump[1] = 0x58; b->jump[2] = 0x90;
    memcpy(b->oem, "LEOPARD ", 8);
    b->bytes_per_sector     = SECTOR_SIZE;
    b->sectors_per_cluster  = spc;
    b->reserved_sectors     = RESERVED_SECTORS;
    b->num_fats             = NUM_FATS;
    b->root_entries         = 0;
    b->total_sectors16      = 0;
    b->media                = 0xF8;
    b->sectors_per_fat16    = 0;
    b->sectors_per_track    = 63;
    b->num_heads            = 255;
    b->hidden_sectors       = 0;
    b->total_sectors32      = (uint32_t)total_sectors;
    b->sectors_per_fat32    = (uint32_t)fatsz;
    b->ext_flags            = 0;
    b->fs_version           = 0;
    b->root_cluster         = ROOT_CLUSTER;
    b->fsinfo_sector        = FSINFO_SECTOR;
    b->backup_boot_sector   = BACKUP_BOOT_SECTOR;
    b->drive_number         = 0x80;
    b->reserved1            = 0;
    b->boot_signature       = 0x29;
    b->volume_id            = volid;
    pad11(b->volume_label, label ? label : "NO NAME");
    memcpy(b->fs_type, "FAT32   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;
    if (sector_write(fd, 0, boot) != 0) { xapi_Printf("write boot sector failed\n"); fd_close(fd); return 1; }
    if (sector_write(fd, BACKUP_BOOT_SECTOR, boot) != 0) { xapi_Printf("write backup boot failed\n"); fd_close(fd); return 1; }

    /* 2. FSInfo 扇区 */
    uint8_t fsi[SECTOR_SIZE];
    memset(fsi, 0, sizeof(fsi));
    fsi[0] = 0x52; fsi[1] = 0x52; fsi[2] = 0x61; fsi[3] = 0x41;      /* 0x41615252 */
    fsi[484] = 0x72; fsi[485] = 0x72; fsi[486] = 0x41; fsi[487] = 0x61; /* 0x61417272 */
    uint32_t free_clusters = (uint32_t)(total_clusters - 1);
    fsi[488] = (uint8_t)(free_clusters);
    fsi[489] = (uint8_t)(free_clusters >> 8);
    fsi[490] = (uint8_t)(free_clusters >> 16);
    fsi[491] = (uint8_t)(free_clusters >> 24);
    fsi[492] = 3; /* next free cluster */
    fsi[508] = 0x00; fsi[509] = 0x00; fsi[510] = 0x55; fsi[511] = 0xAA; /* 0xAA550000 */
    if (sector_write(fd, FSINFO_SECTOR, fsi) != 0) { xapi_Printf("write fsinfo failed\n"); fd_close(fd); return 1; }

    /* 3. 清零其余保留扇区（2..5, 7..31）*/
    uint8_t zeros[SECTOR_SIZE];
    memset(zeros, 0, sizeof(zeros));
    for (uint64_t s = 2; s < RESERVED_SECTORS; s++) {
        if (s == BACKUP_BOOT_SECTOR) continue; /* 已写备份引导 */
        if (sector_write(fd, s, zeros) != 0) { xapi_Printf("zero reserved failed\n"); fd_close(fd); return 1; }
    }

    /* 4. FAT1 + FAT2（FAT[0]=0x0FFFFFF8, FAT[1]=FAT[2]=0x0FFFFFFF，其余空闲）*/
    uint8_t fat0[SECTOR_SIZE];
    memset(fat0, 0, sizeof(fat0));
    uint32_t *fe = (uint32_t *)fat0;
    fe[0] = 0x0FFFFFF8;
    fe[1] = 0x0FFFFFFF;
    fe[2] = 0x0FFFFFFF;
    uint64_t fat1_lba = RESERVED_SECTORS;
    uint64_t fat2_lba = RESERVED_SECTORS + fatsz;
    if (sector_write(fd, fat1_lba, fat0) != 0) { xapi_Printf("write FAT1 head failed\n"); fd_close(fd); return 1; }
    if (sector_write(fd, fat2_lba, fat0) != 0) { xapi_Printf("write FAT2 head failed\n"); fd_close(fd); return 1; }
    for (uint64_t s = 1; s < fatsz; s++) {
        if (sector_write(fd, fat1_lba + s, zeros) != 0) { xapi_Printf("zero FAT1 failed\n"); fd_close(fd); return 1; }
        if (sector_write(fd, fat2_lba + s, zeros) != 0) { xapi_Printf("zero FAT2 failed\n"); fd_close(fd); return 1; }
    }

    /* 5. 根目录簇（cluster 2）：清零 + 卷标目录项 */
    uint64_t data_start = RESERVED_SECTORS + (uint64_t)NUM_FATS * fatsz;
    uint64_t root_lba   = data_start; /* cluster 2 → data_start + (2-2)*spc */
    for (uint64_t s = 0; s < spc; s++) {
        if (sector_write(fd, root_lba + s, zeros) != 0) { xapi_Printf("zero root dir failed\n"); fd_close(fd); return 1; }
    }
    if (label != NULL) {
        uint8_t entry[32];
        memset(entry, 0, sizeof(entry));
        pad11(entry, label);
        entry[11] = 0x08; /* 卷标属性 */
        if (sector_write(fd, root_lba, entry) != 0) { xapi_Printf("write volume label failed\n"); fd_close(fd); return 1; }
    }

    fd_close(fd);
    xapi_Printf("mkfs-fat32: done (%s formatted as FAT32)\n", path);
    return 0;
}
