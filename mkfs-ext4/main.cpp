// mkfs-ext4 — Leopard user-space ext4 formatter.
// Wraps the first-party Apache-2.0 formatter used by the installer path.

#include "format.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xnet_runtime.h" // xapi_secure_random
#include "xtuiapi.h"      // xapi_GetTime / xapi_WriteFile

namespace
{
struct Io
{
    int fd;
    uint64_t bytes;
};

bool read_block(void *context, uint64_t offset, void *buffer, xjext4::Size size)
{
    Io *io = static_cast<Io *>(context);
    if (lseek(io->fd, (long)offset, SEEK_SET) < 0) return false;
    return read(io->fd, buffer, size) == (long)size;
}

bool write_block(void *context, uint64_t offset, const void *buffer, xjext4::Size size)
{
    Io *io = static_cast<Io *>(context);
    if (lseek(io->fd, (long)offset, SEEK_SET) < 0) return false;
    return write(io->fd, buffer, size) == (long)size;
}

bool flush_block(void *context)
{
    Io *io = static_cast<Io *>(context);
    return fsync(io->fd) == 0;
}

void report_progress(void *, uint64_t completed, uint64_t total)
{
    if (total == 0) return;
    unsigned percent = (unsigned)((completed * 100ULL) / total);
    printf("\rmkfs-ext4: %u%%", percent);
    if (completed >= total) printf("\n");
}

bool uuid_is_zero(const uint8_t *uuid)
{
    for (unsigned i = 0; i < 16; ++i) if (uuid[i] != 0) return false;
    return true;
}
} // namespace

int main(int argc, char *argv[], char *envp[])
{
    (void)envp;

    if (argc < 2)
    {
        printf("usage: mkfs-ext4 <device> [label]\n");
        return 2;
    }

    const char *device_arg = argv[1];
    char        device[96];
    const char *label = "LEOPARD";

    if (device_arg[0] == '/') snprintf(device, sizeof(device), "%s", device_arg);
    else snprintf(device, sizeof(device), "/dev/%s", device_arg);
    const char *uuid_out = nullptr;
    for (int i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--uuid-out") && i + 1 < argc) uuid_out = argv[++i];
    }

    int fd = open(device, O_RDWR);
    if (fd < 0)
    {
        printf("mkfs-ext4: cannot open %s\n", device);
        return 1;
    }

    long long bytes = (long long)lseek(fd, 0, SEEK_END);
    if (bytes <= 0)
    {
        printf("mkfs-ext4: cannot size %s\n", device);
        close(fd);
        return 1;
    }
    /* The formatter's 4 KiB profile ignores a sub-block tail. */
    bytes &= ~4095LL;
    if (bytes < 32LL * 1024 * 1024)
    {
        printf("mkfs-ext4: %s too small (32 MiB minimum)\n", device);
        close(fd);
        return 1;
    }

    Io io = {fd, (uint64_t)bytes};
    xjext4::Ext4FormatDevice backend = {&io, read_block, write_block, flush_block, (uint64_t)bytes};

    xjext4::FormatOptions options = {};
    options.journal_mib = 32;
    options.time_seconds = xapi_GetTime();
    strncpy(options.label, label, sizeof(options.label) - 1);
    options.label[sizeof(options.label) - 1] = '\0';

    xapi_secure_random(options.uuid, sizeof(options.uuid));
    if (uuid_is_zero(options.uuid)) options.uuid[0] = 1;

    xjext4::FormatWorkspace *workspace = (xjext4::FormatWorkspace *)malloc(sizeof(xjext4::FormatWorkspace));
    if (workspace == NULL)
    {
        printf("mkfs-ext4: out of memory\n");
        close(fd);
        return 1;
    }

    xjext4::FormatGeometry geometry = {};
    int error = xjext4::plan_format(backend, options, *workspace, geometry);
    if (error == 0) error = xjext4::format(backend, options, *workspace, geometry, nullptr,
                                            nullptr);
    free(workspace);
    close(fd);

    if (error != 0)
    {
        printf("mkfs-ext4: failed (%d)\n", error);
        return 1;
    }

    char uuid_text[64];
    size_t used = 0;
    used += (size_t)snprintf(uuid_text + used, sizeof(uuid_text) - used, "uuid=");
    for (unsigned i = 0; i < 16; ++i)
        used += (size_t)snprintf(uuid_text + used, sizeof(uuid_text) - used, "%02x", options.uuid[i]);
    used += (size_t)snprintf(uuid_text + used, sizeof(uuid_text) - used, "\n");
    if (uuid_out != nullptr) xapi_WriteFile((WSTR)uuid_out, uuid_text, used, 0);

    printf("mkfs-ext4: %sblocks=%u groups=%u inodes=%u\n", uuid_text,
           geometry.blocks, geometry.groups, geometry.inodes);
    return 0;
}
