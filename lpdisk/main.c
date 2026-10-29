/*
 * leopard-disk — LeopardOS 用户态 GPT/MBR 分区工具（gdisk 式）
 * 分区表当文件处理；只依赖内核既有原语；原生 XAPI + 裸 syscall。
 */
#include "xtuiapi.h"

#define SECTOR_SIZE         512U
#define GPT_SIGNATURE       "EFI PART"
#define GPT_HEADER_LBA      1ULL
#define GPT_NUM_ENTRIES     128U
#define GPT_ENTRY_SIZE      128U
#define GPT_ENTRY_LBA       2ULL
#define GPT_FIRST_USABLE    34ULL
#define MBR_TYPE_EFI        0xEE

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define SEEK_SET 0
#define SEEK_END 2

#define IOPARTSCAN 0x5462

typedef struct {
        uint8_t  signature[8];
        uint32_t revision;
        uint32_t header_size;
        uint32_t header_crc32;
        uint32_t reserved;
        uint64_t my_lba;
        uint64_t alternate_lba;
        uint64_t first_usable_lba;
        uint64_t last_usable_lba;
        uint8_t  disk_guid[16];
        uint64_t partition_entry_lba;
        uint32_t num_partition_entries;
        uint32_t size_of_partition_entry;
        uint32_t partition_entry_array_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
        uint8_t  partition_type_guid[16];
        uint8_t  unique_partition_guid[16];
        uint64_t starting_lba;
        uint64_t ending_lba;
        uint64_t attributes;
        uint16_t partition_name[36];
} __attribute__((packed)) gpt_entry_t;

typedef struct {
        uint8_t  flags;
        uint8_t  start_head;
        uint16_t start_sector_cyl;
        uint8_t  type;
        uint8_t  end_head;
        uint16_t end_sector_cyl;
        uint32_t start_lba;
        uint32_t sectors_limit;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
        uint8_t      bootstrap[446];
        mbr_entry_t  dpte[4];
        uint16_t     signature;
} __attribute__((packed)) mbr_t;

typedef struct {
        gpt_header_t hdr;
        gpt_entry_t *entries;
        uint64_t total_lba;
} gpt_disk_t;

static uint32_t crc32_tab[256];
static uint64_t rng_state;

static const uint8_t GUID_ESP[16]   = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const uint8_t GUID_LINUX[16] = {0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};
static const uint8_t GUID_SWAP[16]  = {0x6D,0xFD,0x57,0x06,0xAB,0xA4,0xC4,0x43,0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F};
static const uint8_t GUID_LVM[16]   = {0x79,0xD3,0xD6,0xE6,0x07,0xF5,0xC2,0x44,0xA2,0x3C,0x23,0x8F,0x2A,0x3D,0xF9,0x28};

static const char *CANDIDATES[] = {
        "sata0","sata1","sata2","sata3","sata4","sata5","sata6","sata7",
        "nvme0","nvme1","nvme2","nvme3",
        "ide0","ide1","ide2","ide3", NULL
};

static void     crc32_init(void);
static uint32_t crc32_calc(const void *, size_t);
static void     guid_to_string(const uint8_t *, char *);
static void     guid_random(uint8_t *);
static int      cmd_list(void);
static void     dump_mbr(const mbr_t *);
static int      cmd_show(const char *);
static int      gpt_load(int, gpt_disk_t *);
static int      gpt_save(int, const gpt_disk_t *);
static void     gpt_init_empty(gpt_disk_t *, uint64_t);
static int      cmd_create(const char *);
static int      cmd_add(int, char *[]);
static int      cmd_del(const char *, const char *);
static int      cmd_rescan(const char *);
static void     usage(void);

static inline int fd_open(const char *path, uint64_t flags)
{
        return (int)(int64_t)enter_syscall(SYS_OPEN, (uint64_t)path, flags, 0644, 0, 0, 0);
}

static inline int64_t fd_read(int fd, void *buf, uint64_t n)
{
        return (int64_t)enter_syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf, n, 0, 0, 0);
}

static inline int64_t fd_write(int fd, const void *buf, uint64_t n)
{
        return (int64_t)enter_syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, n, 0, 0, 0);
}

static inline int64_t fd_lseek(int fd, int64_t off, uint64_t whence)
{
        return (int64_t)enter_syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)off, whence, 0, 0, 0);
}

static inline int fd_close(int fd)
{
        return (int)enter_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
}

static inline int fd_ioctl(int fd, uint64_t req, void *arg)
{
        return (int)enter_syscall(SYS_IOCTL, (uint64_t)fd, req, (uint64_t)arg, 0, 0, 0);
}

static inline int sector_read(int fd, uint64_t lba, void *buf)
{
        if (fd_lseek(fd, (int64_t)(lba * SECTOR_SIZE), SEEK_SET) < 0) return -1;
        return (fd_read(fd, buf, SECTOR_SIZE) == (int64_t)SECTOR_SIZE) ? 0 : -1;
}

static inline int sector_write(int fd, uint64_t lba, const void *buf)
{
        if (fd_lseek(fd, (int64_t)(lba * SECTOR_SIZE), SEEK_SET) < 0) return -1;
        return (fd_write(fd, buf, SECTOR_SIZE) == (int64_t)SECTOR_SIZE) ? 0 : -1;
}

static inline int range_read(int fd, uint64_t offset, void *buf, uint64_t n)
{
        if (fd_lseek(fd, (int64_t)offset, SEEK_SET) < 0) return -1;
        return (fd_read(fd, buf, n) == (int64_t)n) ? 0 : -1;
}

static inline int range_write(int fd, uint64_t offset, const void *buf, uint64_t n)
{
        if (fd_lseek(fd, (int64_t)offset, SEEK_SET) < 0) return -1;
        return (fd_write(fd, buf, n) == (int64_t)n) ? 0 : -1;
}

static inline uint64_t disk_size_bytes(int fd)
{
        int64_t sz = fd_lseek(fd, 0, SEEK_END);
        return (sz < 0) ? 0 : (uint64_t)sz;
}

static inline void rng_seed(void)
{
        rng_state = (uint64_t)enter_syscall(XAPI_GET_TIME_NANO, 0, 0, 0, 0, 0, 0);
        if (rng_state == 0) rng_state = 0x9E3779B97F4A7C15ULL;
}

static inline uint64_t rng_next(void)
{
        uint64_t x = rng_state;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        rng_state = x;
        return x;
}

static inline void name_utf16_to_ascii(const uint16_t u[36], char out[37])
{
        for (int i = 0; i < 36; i++) out[i] = (char)(u[i] & 0x7F);
        out[36] = '\0';
}

static inline void name_ascii_to_utf16(const char *s, uint16_t u[36])
{
        size_t n = strlen(s);
        if (n > 36) n = 36;
        for (size_t i = 0; i < 36; i++) u[i] = (i < n) ? (uint16_t)s[i] : 0;
}

static inline const uint8_t *type_guid_by_code(const char *code)
{
        if (strcmp(code, "ef00") == 0) return GUID_ESP;
        if (strcmp(code, "8200") == 0) return GUID_SWAP;
        if (strcmp(code, "8e00") == 0) return GUID_LVM;
        return GUID_LINUX;
}

static inline const char *type_name_by_guid(const uint8_t g[16])
{
        if (memcmp(g, GUID_ESP, 16) == 0)   return "EFI System";
        if (memcmp(g, GUID_SWAP, 16) == 0)  return "Linux swap";
        if (memcmp(g, GUID_LVM, 16) == 0)   return "Linux LVM";
        if (memcmp(g, GUID_LINUX, 16) == 0) return "Linux filesystem";
        return "unknown";
}

static inline void gpt_free(gpt_disk_t *g)
{
        if (g->entries) {
                free(g->entries);
                g->entries = NULL;
        }
}

int main(int argc, char *argv[], char *envp[])
{
        (void)envp;
        crc32_init();
        rng_seed();

        if (argc < 2) {
                usage();
                return 1;
        }
        const char *cmd = argv[1];

        if (strcmp(cmd, "list") == 0)      return cmd_list();
        if (strcmp(cmd, "show") == 0) {
                if (argc < 3) {
                        usage();
                        return 1;
                }
                return cmd_show(argv[2]);
        }
        if (strcmp(cmd, "create") == 0) {
                if (argc < 3) {
                        usage();
                        return 1;
                }
                return cmd_create(argv[2]);
        }
        if (strcmp(cmd, "add") == 0) {
                if (argc < 5) {
                        usage();
                        return 1;
                }
                return cmd_add(argc, argv);
        }
        if (strcmp(cmd, "del") == 0) {
                if (argc < 4) {
                        usage();
                        return 1;
                }
                return cmd_del(argv[2], argv[3]);
        }
        if (strcmp(cmd, "rescan") == 0) {
                if (argc < 3) {
                        usage();
                        return 1;
                }
                return cmd_rescan(argv[2]);
        }
        usage();
        return 1;
}

static void crc32_init(void)
{
        for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++)
                        c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
                crc32_tab[i] = c;
        }
}

static uint32_t crc32_calc(const void *data, size_t len)
{
        const uint8_t *p = (const uint8_t *)data;
        uint32_t c = 0xFFFFFFFFU;
        for (size_t i = 0; i < len; i++)
                c = crc32_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFU;
}

static void guid_to_string(const uint8_t g[16], char out[37])
{
        snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
                 g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static void guid_random(uint8_t out[16])
{
        for (int i = 0; i < 16; i += 8) {
                uint64_t r = rng_next();
                out[i+0] = (uint8_t)(r);
                out[i+1] = (uint8_t)(r >> 8);
                out[i+2] = (uint8_t)(r >> 16);
                out[i+3] = (uint8_t)(r >> 24);
                out[i+4] = (uint8_t)(r >> 32);
                out[i+5] = (uint8_t)(r >> 40);
                out[i+6] = (uint8_t)(r >> 48);
                out[i+7] = (uint8_t)(r >> 56);
        }
        out[6] = (uint8_t)((out[6] & 0x0F) | 0x40);
        out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);
}

static int cmd_list(void)
{
        xapi_Printf("Block devices:\n");
        int found = 0;
        for (int i = 0; CANDIDATES[i] != NULL; i++) {
                char path[64];
                snprintf(path, sizeof(path), "/dev/%s", CANDIDATES[i]);
                int fd = fd_open(path, O_RDONLY);
                if (fd < 0) continue;
                uint64_t sz = disk_size_bytes(fd);
                fd_close(fd);
                if (sz == 0) continue;
                found++;
                xapi_Printf("  /dev/%-8s  %llu bytes  (%llu sectors)\n",
                            CANDIDATES[i], (unsigned long long)sz,
                            (unsigned long long)(sz / SECTOR_SIZE));
        }
        if (!found) xapi_Printf("  (no block devices found)\n");
        return 0;
}

static void dump_mbr(const mbr_t *mbr)
{
        xapi_Printf("MBR partitions (legacy):\n");
        for (int i = 0; i < 4; i++) {
                const mbr_entry_t *e = &mbr->dpte[i];
                if (e->start_lba == 0 || e->sectors_limit == 0) continue;
                xapi_Printf("  #%d  type=0x%02X  start=%u  sectors=%u  (%s)\n",
                            i + 1, e->type, e->start_lba, e->sectors_limit,
                            (e->flags & 0x80) ? "boot" : "");
        }
}

static int cmd_show(const char *dev)
{
        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", dev);
        int fd = fd_open(path, O_RDONLY);
        if (fd < 0) {
                xapi_Printf("open %s failed\n", path);
                return 1;
        }
        uint64_t sz = disk_size_bytes(fd);
        if (sz < SECTOR_SIZE) {
                xapi_Printf("disk too small\n");
                fd_close(fd);
                return 1;
        }
        uint64_t total_lba = sz / SECTOR_SIZE;

        uint8_t lba0[SECTOR_SIZE];
        if (sector_read(fd, 0, lba0) != 0) {
                xapi_Printf("read LBA0 failed\n");
                fd_close(fd);
                return 1;
        }

        int is_gpt = 0;
        const mbr_t *mbr = (const mbr_t *)lba0;
        if (mbr->signature == 0xAA55 && mbr->dpte[0].type == MBR_TYPE_EFI) is_gpt = 1;

        if (is_gpt) {
                gpt_header_t hdr;
                uint8_t hdr_buf[SECTOR_SIZE];
                if (sector_read(fd, GPT_HEADER_LBA, hdr_buf) != 0) {
                        fd_close(fd);
                        return 1;
                }
                memcpy(&hdr, hdr_buf, sizeof(gpt_header_t));
                if (memcmp(hdr.signature, GPT_SIGNATURE, 8) != 0) {
                        xapi_Printf("GPT signature invalid\n");
                        fd_close(fd);
                        return 1;
                }
                char disk_guid[37];
                guid_to_string(hdr.disk_guid, disk_guid);
                xapi_Printf("GPT: %s  (%llu sectors)\n", path, (unsigned long long)total_lba);
                xapi_Printf("  disk GUID:      %s\n", disk_guid);
                xapi_Printf("  entries:        %u @ LBA %llu (size %u)\n",
                            hdr.num_partition_entries, (unsigned long long)hdr.partition_entry_lba,
                            hdr.size_of_partition_entry);
                xapi_Printf("  first usable:   %llu\n", (unsigned long long)hdr.first_usable_lba);
                xapi_Printf("  last usable:    %llu\n", (unsigned long long)hdr.last_usable_lba);
                xapi_Printf("  backup header:  LBA %llu\n", (unsigned long long)hdr.alternate_lba);

                uint64_t esize = hdr.size_of_partition_entry;
                uint64_t n = hdr.num_partition_entries;
                uint64_t arr_bytes = esize * n;
                uint8_t *arr = (uint8_t *)malloc((size_t)arr_bytes);
                if (!arr) {
                        fd_close(fd);
                        return 1;
                }
                if (range_read(fd, hdr.partition_entry_lba * SECTOR_SIZE, arr, arr_bytes) != 0) {
                        xapi_Printf("read entries failed\n");
                        free(arr);
                        fd_close(fd);
                        return 1;
                }
                xapi_Printf("Partitions:\n");
                int cnt = 0;
                for (uint64_t j = 0; j < n; j++) {
                        const gpt_entry_t *e = (const gpt_entry_t *)(arr + j * esize);
                        int used = 0;
                        for (int k = 0; k < 16; k++) {
                                if (e->partition_type_guid[k]) {
                                        used = 1;
                                        break;
                                }
                        }
                        if (!used) continue;
                        char name[37], pguid[37];
                        name_utf16_to_ascii(e->partition_name, name);
                        guid_to_string(e->unique_partition_guid, pguid);
                        cnt++;
                        xapi_Printf("  #%-3llu %-17s start=%-10llu end=%-10llu size=%-10llu  %s\n",
                                    (unsigned long long)(j + 1), type_name_by_guid(e->partition_type_guid),
                                    (unsigned long long)e->starting_lba, (unsigned long long)e->ending_lba,
                                    (unsigned long long)(e->ending_lba - e->starting_lba + 1), name);
                        xapi_Printf("         GUID: %s\n", pguid);
                }
                if (!cnt) xapi_Printf("  (empty)\n");
                free(arr);
        } else {
                xapi_Printf("Disk %s (%llu sectors):\n", path, (unsigned long long)total_lba);
                dump_mbr(mbr);
        }
        fd_close(fd);
        return 0;
}

static int gpt_load(int fd, gpt_disk_t *g)
{
        memset(g, 0, sizeof(*g));
        uint64_t sz = disk_size_bytes(fd);
        g->total_lba = sz / SECTOR_SIZE;
        if (g->total_lba < GPT_FIRST_USABLE + 34) return -1;
        uint8_t hdr_buf[SECTOR_SIZE];
        if (sector_read(fd, GPT_HEADER_LBA, hdr_buf) != 0) return -1;
        memcpy(&g->hdr, hdr_buf, sizeof(gpt_header_t));
        if (memcmp(g->hdr.signature, GPT_SIGNATURE, 8) != 0) return -1;
        uint64_t esize = g->hdr.size_of_partition_entry;
        uint64_t n = g->hdr.num_partition_entries;
        if (esize < sizeof(gpt_entry_t) || n == 0 || n > 1024) return -1;
        uint64_t arr_bytes = esize * n;
        g->entries = (gpt_entry_t *)malloc((size_t)arr_bytes);
        if (!g->entries) return -1;
        memset(g->entries, 0, (size_t)arr_bytes);
        if (range_read(fd, g->hdr.partition_entry_lba * SECTOR_SIZE, g->entries, arr_bytes) != 0) {
                free(g->entries);
                g->entries = NULL;
                return -1;
        }
        return 0;
}

static int gpt_save(int fd, const gpt_disk_t *g)
{
        uint64_t total = g->total_lba;
        gpt_header_t hdr = g->hdr;
        uint64_t esize = hdr.size_of_partition_entry;
        uint64_t n = hdr.num_partition_entries;
        uint64_t arr_bytes = esize * n;
        uint64_t arr_sectors = (arr_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;

        hdr.partition_entry_array_crc32 = crc32_calc(g->entries, (size_t)arr_bytes);
        hdr.header_crc32 = 0;
        hdr.header_crc32 = crc32_calc(&hdr, sizeof(hdr));

        uint8_t mbr[SECTOR_SIZE];
        memset(mbr, 0, sizeof(mbr));
        mbr_t *pm = (mbr_t *)mbr;
        pm->dpte[0].type = MBR_TYPE_EFI;
        pm->dpte[0].start_lba = 1;
        uint64_t protect = (total - 1 < 0xFFFFFFFFULL) ? (total - 1) : 0xFFFFFFFFULL;
        pm->dpte[0].sectors_limit = (uint32_t)protect;
        pm->signature = 0xAA55;

        if (sector_write(fd, 0, mbr) != 0) return -1;
        uint8_t hdr_buf[SECTOR_SIZE];
        memset(hdr_buf, 0, sizeof(hdr_buf));
        memcpy(hdr_buf, &hdr, sizeof(gpt_header_t));
        if (sector_write(fd, GPT_HEADER_LBA, hdr_buf) != 0) return -1;
        if (range_write(fd, hdr.partition_entry_lba * SECTOR_SIZE, g->entries, arr_bytes) != 0) return -1;

        uint64_t backup_entries_lba = total - 1 - arr_sectors;
        if (range_write(fd, backup_entries_lba * SECTOR_SIZE, g->entries, arr_bytes) != 0) return -1;

        gpt_header_t backup = hdr;
        backup.my_lba = total - 1;
        backup.alternate_lba = GPT_HEADER_LBA;
        backup.partition_entry_lba = backup_entries_lba;
        backup.header_crc32 = 0;
        backup.header_crc32 = crc32_calc(&backup, sizeof(backup));
        memset(hdr_buf, 0, sizeof(hdr_buf));
        memcpy(hdr_buf, &backup, sizeof(gpt_header_t));
        if (sector_write(fd, total - 1, hdr_buf) != 0) return -1;

        return 0;
}

static void gpt_init_empty(gpt_disk_t *g, uint64_t total_lba)
{
        memset(g, 0, sizeof(*g));
        g->total_lba = total_lba;
        uint64_t arr_bytes = (uint64_t)GPT_NUM_ENTRIES * GPT_ENTRY_SIZE;
        uint64_t arr_sectors = (arr_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;

        gpt_header_t *h = &g->hdr;
        memcpy(h->signature, GPT_SIGNATURE, 8);
        h->revision = 0x00010000;
        h->header_size = 92;
        h->my_lba = GPT_HEADER_LBA;
        h->alternate_lba = total_lba - 1;
        h->first_usable_lba = GPT_FIRST_USABLE;
        h->last_usable_lba = total_lba - 1 - arr_sectors - 1;
        guid_random(h->disk_guid);
        h->partition_entry_lba = GPT_ENTRY_LBA;
        h->num_partition_entries = GPT_NUM_ENTRIES;
        h->size_of_partition_entry = GPT_ENTRY_SIZE;

        g->entries = (gpt_entry_t *)malloc((size_t)arr_bytes);
        memset(g->entries, 0, (size_t)arr_bytes);
}

static int cmd_create(const char *dev)
{
        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", dev);
        int fd = fd_open(path, O_RDWR);
        if (fd < 0) {
                xapi_Printf("open %s failed\n", path);
                return 1;
        }
        uint64_t total_lba = disk_size_bytes(fd) / SECTOR_SIZE;
        if (total_lba < GPT_FIRST_USABLE + 34) {
                xapi_Printf("disk too small for GPT\n");
                fd_close(fd);
                return 1;
        }
        gpt_disk_t g;
        gpt_init_empty(&g, total_lba);
        int rc = gpt_save(fd, &g);
        gpt_free(&g);
        fd_close(fd);
        if (rc != 0) {
                xapi_Printf("write GPT failed\n");
                return 1;
        }
        xapi_Printf("Created empty GPT on %s (%llu sectors)\n", path, (unsigned long long)total_lba);
        return 0;
}

static int cmd_add(int argc, char *argv[])
{
        const char *dev = argv[2];
        uint64_t start = (uint64_t)strtol(argv[3], NULL, 0);
        uint64_t end   = (uint64_t)strtol(argv[4], NULL, 0);
        const char *type = "8300";
        const char *name = "";
        for (int i = 5; i < argc; i++) {
                if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) type = argv[++i];
                else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        }

        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", dev);
        int fd = fd_open(path, O_RDWR);
        if (fd < 0) {
                xapi_Printf("open %s failed\n", path);
                return 1;
        }

        gpt_disk_t g;
        if (gpt_load(fd, &g) != 0) {
                xapi_Printf("load GPT failed (not a GPT disk?)\n");
                fd_close(fd);
                return 1;
        }
        if (start < g.hdr.first_usable_lba || end > g.hdr.last_usable_lba || start > end) {
                xapi_Printf("LBA range out of usable area (%llu..%llu)\n",
                            (unsigned long long)g.hdr.first_usable_lba,
                            (unsigned long long)g.hdr.last_usable_lba);
                gpt_free(&g);
                fd_close(fd);
                return 1;
        }
        gpt_entry_t *e = NULL;
        for (uint64_t j = 0; j < g.hdr.num_partition_entries; j++) {
                gpt_entry_t *c = (gpt_entry_t *)((uint8_t *)g.entries + j * g.hdr.size_of_partition_entry);
                int used = 0;
                for (int k = 0; k < 16; k++) {
                        if (c->partition_type_guid[k]) {
                                used = 1;
                                break;
                        }
                }
                if (!used) {
                        e = c;
                        break;
                }
        }
        if (!e) {
                xapi_Printf("no free entry slot\n");
                gpt_free(&g);
                fd_close(fd);
                return 1;
        }

        memset(e, 0, g.hdr.size_of_partition_entry);
        memcpy(e->partition_type_guid, type_guid_by_code(type), 16);
        guid_random(e->unique_partition_guid);
        e->starting_lba = start;
        e->ending_lba = end;
        name_ascii_to_utf16(name, e->partition_name);

        int rc = gpt_save(fd, &g);
        gpt_free(&g);
        fd_close(fd);
        if (rc != 0) {
                xapi_Printf("write GPT failed\n");
                return 1;
        }
        xapi_Printf("Added partition start=%llu end=%llu type=%s\n",
                    (unsigned long long)start, (unsigned long long)end, type);
        return 0;
}

static int cmd_del(const char *dev, const char *idxstr)
{
        uint64_t idx = (uint64_t)strtol(idxstr, NULL, 0);
        if (idx == 0) {
                xapi_Printf("invalid index\n");
                return 1;
        }
        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", dev);
        int fd = fd_open(path, O_RDWR);
        if (fd < 0) {
                xapi_Printf("open %s failed\n", path);
                return 1;
        }
        gpt_disk_t g;
        if (gpt_load(fd, &g) != 0) {
                xapi_Printf("load GPT failed\n");
                fd_close(fd);
                return 1;
        }
        if (idx > g.hdr.num_partition_entries) {
                xapi_Printf("index out of range\n");
                gpt_free(&g);
                fd_close(fd);
                return 1;
        }
        gpt_entry_t *e = (gpt_entry_t *)((uint8_t *)g.entries + (idx - 1) * g.hdr.size_of_partition_entry);
        memset(e, 0, g.hdr.size_of_partition_entry);
        int rc = gpt_save(fd, &g);
        gpt_free(&g);
        fd_close(fd);
        if (rc != 0) {
                xapi_Printf("write GPT failed\n");
                return 1;
        }
        xapi_Printf("Deleted partition #%llu\n", (unsigned long long)idx);
        return 0;
}

static int cmd_rescan(const char *dev)
{
        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", dev);
        int fd = fd_open(path, O_RDWR);
        if (fd < 0) {
                xapi_Printf("open %s failed\n", path);
                return 1;
        }
        int rc = fd_ioctl(fd, IOPARTSCAN, NULL);
        fd_close(fd);
        if (rc != 0) {
                xapi_Printf("rescan ioctl failed (rc=%d)\n", rc);
                return 1;
        }
        xapi_Printf("Rescanned partitions on %s\n", path);
        return 0;
}

static void usage(void)
{
        xapi_Printf(
                "lpdisk — GPT/MBR partition tool\n"
                "\n"
                "  lpdisk list\n"
                "  lpdisk show <dev>\n"
                "  lpdisk create <dev>\n"
                "  lpdisk add <dev> <start> <end> [--type ef00|8300|8200|8e00] [--name <str>]\n"
                "  lpdisk del <dev> <index>\n"
                "  lpdisk rescan <dev>\n"
                "\n"
                "  <dev> is e.g. sata0, nvme1, ide0.\n"
                "  create/add/del rewrite the partition table (destructive).\n");
}
