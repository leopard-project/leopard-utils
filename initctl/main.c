#include "xtuiapi.h"

/* initctl —— initrc 服务控制：list / enable <svc> / disable <svc>
 * initrc 格式：每行 "respawn <路径>"（也兼容 echo/sleep/裸命令，见 leopard-rc 的 str2srv）
 */

#define CFG_PATH "/system/config/initrc"
#define BUF_CAP  (16 * 1024)

typedef struct { const char *name; const char *path; } SvcEntry;
static const SvcEntry k_services[] = {
    { "leopard-getty", "/apps/leopard/leopard-getty.elf" },
};
#define K_SERVICE_COUNT (sizeof(k_services) / sizeof(k_services[0]))

static const char *svc_path(const char *name)
{
    for (size_t i = 0; i < K_SERVICE_COUNT; i++)
        if (strcmp(k_services[i].name, name) == 0)
            return k_services[i].path;
    return NULL;
}

/* 读整个 initrc 到 buf（以 \0 结尾），返回字节数；文件不存在返回 0 */
static uint64_t read_initrc(char *buf, size_t cap)
{
    memset(buf, 0, cap);
    XFILE *fp = xapi_OpenFile((WSTR)CFG_PATH);
    if (fp == NULL)
        return 0;
    uint64_t len = fp->length;
    if (len >= cap)
        len = cap - 1;
    uint64_t got = xapi_ReadFile((WSTR)CFG_PATH, buf, len, 0);
    xapi_CloseFile(fp);
    buf[got] = '\0';
    return got;
}

/* 写回 initrc：先删再写，避免旧内容比新内容长时残留 */
static void write_initrc(const char *buf, size_t len)
{
    xapi_DeleteFile((WSTR)CFG_PATH);
    xapi_WriteFile((WSTR)CFG_PATH, (char *)buf, len, 0);
}

/* 判断一行（不含换行，textlen 字节）是否为 "respawn <path>" */
static int line_is_respawn(const char *line, size_t textlen, const char *path)
{
    if (textlen < 8 || strncmp(line, "respawn ", 8) != 0)
        return 0;
    const char *p = line + 8;
    size_t plen = textlen - 8;
    while (plen > 0 && (p[plen - 1] == '\r' || p[plen - 1] == ' ' || p[plen - 1] == '\t'))
        plen--;
    return plen == strlen(path) && strncmp(p, path, plen) == 0;
}

static int has_respawn(const char *buf, const char *path)
{
    const char *line = buf;
    while (*line) {
        const char *nl = strchr(line, '\n');
        size_t textlen = nl ? (size_t)(nl - line) : strlen(line);
        if (line_is_respawn(line, textlen, path))
            return 1;
        if (!nl)
            break;
        line = nl + 1;
    }
    return 0;
}

int main(int argc, char *argv[], char *envp[])
{
    (void)envp;
    const char *cmd = argc > 1 ? argv[1] : NULL;

    if (cmd == NULL || strcmp(cmd, "list") == 0) {
        char buf[BUF_CAP];
        uint64_t n = read_initrc(buf, sizeof(buf));
        if (n == 0)
            xapi_Printf("(empty)\n");
        else
            xapi_Printf("%s", buf);
        return 0;
    }

    if (strcmp(cmd, "enable") == 0 || strcmp(cmd, "disable") == 0) {
        if (argc < 3) {
            xapi_Printf("usage: initctl %s <service>\n", cmd);
            return 1;
        }
        const char *path = svc_path(argv[2]);
        if (path == NULL) {
            xapi_Printf("initctl: unknown service %s\n", argv[2]);
            return 1;
        }

        char buf[BUF_CAP];
        uint64_t n = read_initrc(buf, sizeof(buf));

        if (strcmp(cmd, "enable") == 0) {
            if (has_respawn(buf, path)) {
                xapi_Printf("already enabled: %s\n", argv[2]);
                return 0;
            }
            snprintf(buf + n, sizeof(buf) - n, "respawn %s\n", path);
            write_initrc(buf, strlen(buf));
            xapi_Printf("enabled: %s\n", argv[2]);
        } else {
            char out[BUF_CAP];
            size_t olen = 0;
            const char *line = buf;
            while (*line && olen < sizeof(out) - 1) {
                const char *nl = strchr(line, '\n');
                size_t textlen = nl ? (size_t)(nl - line) : strlen(line);
                if (!line_is_respawn(line, textlen, path)) {
                    size_t copylen = nl ? textlen + 1 : textlen;
                    memcpy(out + olen, line, copylen);
                    olen += copylen;
                }
                if (!nl)
                    break;
                line = nl + 1;
            }
            out[olen] = '\0';
            write_initrc(out, olen);
            xapi_Printf("disabled: %s\n", argv[2]);
        }
        return 0;
    }

    xapi_Printf("usage: initctl [list | enable <service> | disable <service>]\n");
    return 1;
}
