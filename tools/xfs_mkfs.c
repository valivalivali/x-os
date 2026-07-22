/* xfs_mkfs — host-side tool to format and seed X OS disk images.
 *
 * Usage: xfs_mkfs <disk.img> <size_mb> [host_path:xfs_path ...]
 *
 * Creates a raw disk image, formats it with XFS, creates the full
 * directory hierarchy, and copies the given files into it.
 *
 * Compiled with the host compiler (clang on macOS) — no kernel deps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- XFS on-disk constants (must match kernel/fs/xfs.h) ---- */

#define XFS_MAGIC       0x58465331   /* "XFS1" */
#define XFS_VERSION     1
#define XFS_BLOCK_SIZE  4096
#define XFS_NAME_MAX    64

#define XFS_DENT_FILE   0
#define XFS_DENT_DIR    1

/* ---- On-disk structures (must match kernel/fs/xfs.c) ---- */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t root_block;
    uint64_t bitmap_blocks;
    uint64_t data_start;
    uint64_t checksum;
} xfs_super_t;

typedef struct {
    char     name[XFS_NAME_MAX];
    uint32_t size;
    uint16_t flags;           /* bit 0 = is_directory */
    uint16_t block_count;
    uint32_t data_blocks[900];
    uint32_t checksum;
} xfs_inode_t;

typedef struct {
    char     name[XFS_NAME_MAX];
    uint32_t inode_block;
    uint32_t size;
    uint16_t flags;
    uint16_t reserved;
} xfs_dirent_t;

/* ---- Globals ---- */

static FILE *g_img = NULL;
static uint64_t g_total = 0;
static uint64_t g_bitmap_blocks = 0;
static uint64_t g_data_start = 0;
static uint64_t g_root_block = 0;
static uint64_t g_next_free = 0;  /* next block to try for allocation */

/* ---- Block I/O ---- */

static void block_read(uint64_t lba, void *buf) {
    fseek(g_img, (long)(lba * XFS_BLOCK_SIZE), SEEK_SET);
    fread(buf, 1, XFS_BLOCK_SIZE, g_img);
}

static void block_write(uint64_t lba, const void *buf) {
    fseek(g_img, (long)(lba * XFS_BLOCK_SIZE), SEEK_SET);
    fwrite(buf, 1, XFS_BLOCK_SIZE, g_img);
}

/* ---- Bitmap ---- */

static void bitmap_set(uint64_t block, bool used) {
    uint64_t byte = block / 8;
    uint64_t bit  = block % 8;
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(1 + byte / XFS_BLOCK_SIZE, buf);
    if (used) buf[byte % XFS_BLOCK_SIZE] |= (1 << bit);
    else      buf[byte % XFS_BLOCK_SIZE] &= ~(1 << bit);
    block_write(1 + byte / XFS_BLOCK_SIZE, buf);
}

static bool bitmap_get(uint64_t block) {
    uint64_t byte = block / 8;
    uint64_t bit  = block % 8;
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(1 + byte / XFS_BLOCK_SIZE, buf);
    return (buf[byte % XFS_BLOCK_SIZE] >> bit) & 1;
}

static uint64_t alloc_block(void) {
    for (uint64_t b = g_next_free; b < g_total; b++) {
        if (!bitmap_get(b)) {
            bitmap_set(b, true);
            g_next_free = b + 1;
            return b;
        }
    }
    return 0;
}

/* ---- Inode helpers ---- */

static void read_inode(uint64_t block, xfs_inode_t *inode) {
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(block, buf);
    memcpy(inode, buf, sizeof(xfs_inode_t));
}

static void write_inode(uint64_t block, const xfs_inode_t *inode) {
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(block, buf);
    memcpy(buf, inode, sizeof(xfs_inode_t));
    block_write(block, buf);
}

/* ---- Path resolution ---- */

static uint64_t resolve_path(const char *path, xfs_inode_t *out) {
    if (!path || path[0] != '/') return 0;
    uint64_t current = g_root_block;
    read_inode(current, out);
    if (!(out->flags & 1)) return 0;
    if (path[1] == '\0') return current;

    char buf[XFS_NAME_MAX];
    const char *p = path + 1;
    for (;;) {
        size_t i = 0;
        while (p[i] && p[i] != '/' && i < XFS_NAME_MAX - 1) buf[i] = p[i], i++;
        buf[i] = '\0';

        bool found = false;
        uint64_t next_block = 0;
        for (uint32_t db = 0; db < out->block_count && !found; db++) {
            uint32_t b = out->data_blocks[db];
            if (!b) continue;
            uint8_t sector[XFS_BLOCK_SIZE];
            block_read(b, sector);
            int max_dents = XFS_BLOCK_SIZE / sizeof(xfs_dirent_t);
            xfs_dirent_t *ents = (xfs_dirent_t *)sector;
            for (int d = 0; d < max_dents; d++) {
                if (ents[d].inode_block == 0) continue;
                if (strcmp(ents[d].name, buf) == 0) {
                    next_block = ents[d].inode_block;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return 0;
        current = next_block;
        read_inode(current, out);
        p += i;
        if (*p == '/') p++;
        if (*p == '\0') return current;
    }
}

/* ---- Directory creation ---- */

static int xfs_mkdir(const char *path) {
    xfs_inode_t check;
    if (resolve_path(path, &check)) return 0;  /* already exists */

    const char *name = path + 1;
    const char *last_slash = path;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    uint64_t parent_ino;
    xfs_inode_t pinode;
    if (last_slash == path) {
        parent_ino = g_root_block;
    } else {
        char parent_path[512];
        size_t plen = (size_t)(last_slash - path);
        if (plen >= sizeof(parent_path)) plen = sizeof(parent_path) - 1;
        memcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
        parent_ino = resolve_path(parent_path, &pinode);
        if (!parent_ino) {
            fprintf(stderr, "xfs_mkfs: parent not found for %s\n", path);
            return -1;
        }
    }
    name = last_slash + 1;
    if (*name == '\0') return -1;

    uint64_t new_ino = alloc_block();
    if (!new_ino) return -1;
    uint64_t new_data = alloc_block();
    if (!new_data) return -1;

    xfs_inode_t new_dir;
    memset(&new_dir, 0, sizeof(new_dir));
    strncpy(new_dir.name, name, XFS_NAME_MAX - 1);
    new_dir.flags = 1;
    new_dir.block_count = 1;
    new_dir.data_blocks[0] = (uint32_t)new_data;
    write_inode(new_ino, &new_dir);

    /* Zero the directory data block */
    uint8_t zero[XFS_BLOCK_SIZE];
    memset(zero, 0, XFS_BLOCK_SIZE);
    block_write(new_data, zero);

    /* Add dirent to parent */
    read_inode(parent_ino, &pinode);
    uint8_t sector[XFS_BLOCK_SIZE];
    bool added = false;
    for (uint32_t db = 0; db < pinode.block_count && !added; db++) {
        uint32_t b = pinode.data_blocks[db];
        if (!b) continue;
        block_read(b, sector);
        int max_dents = XFS_BLOCK_SIZE / sizeof(xfs_dirent_t);
        xfs_dirent_t *ents = (xfs_dirent_t *)sector;
        for (int d = 0; d < max_dents; d++) {
            if (ents[d].inode_block == 0) {
                strncpy(ents[d].name, name, XFS_NAME_MAX - 1);
                ents[d].inode_block = (uint32_t)new_ino;
                ents[d].size = 0;
                ents[d].flags = 1;
                block_write(b, sector);
                added = true;
                break;
            }
        }
    }
    if (!added) {
        /* Need to allocate another directory data block for parent */
        if (pinode.block_count >= 900) return -1;
        uint64_t nb = alloc_block();
        if (!nb) return -1;
        memset(sector, 0, XFS_BLOCK_SIZE);
        xfs_dirent_t *ents = (xfs_dirent_t *)sector;
        strncpy(ents[0].name, name, XFS_NAME_MAX - 1);
        ents[0].inode_block = (uint32_t)new_ino;
        ents[0].size = 0;
        ents[0].flags = 1;
        block_write(nb, sector);
        pinode.data_blocks[pinode.block_count] = (uint32_t)nb;
        pinode.block_count++;
        write_inode(parent_ino, &pinode);
    }
    return 0;
}

/* ---- File creation and write ---- */

static int xfs_write_file(const char *xfs_path, const void *data, size_t size) {
    /* Check if file already exists — if so, truncate it */
    xfs_inode_t inode;
    uint64_t ino = resolve_path(xfs_path, &inode);

    if (!ino) {
        /* Create the file — parent dir must already exist */
        const char *name = xfs_path + 1;
        const char *last_slash = xfs_path;
        for (const char *p = xfs_path; *p; p++) if (*p == '/') last_slash = p;

        uint64_t parent_ino;
        xfs_inode_t pinode;
        if (last_slash == xfs_path) {
            parent_ino = g_root_block;
        } else {
            char parent_path[512];
            size_t plen = (size_t)(last_slash - xfs_path);
            if (plen >= sizeof(parent_path)) plen = sizeof(parent_path) - 1;
            memcpy(parent_path, xfs_path, plen);
            parent_path[plen] = '\0';
            parent_ino = resolve_path(parent_path, &pinode);
            if (!parent_ino) {
                fprintf(stderr, "xfs_mkfs: parent not found for %s\n", xfs_path);
                return -1;
            }
        }
        name = last_slash + 1;
        if (*name == '\0') return -1;

        ino = alloc_block();
        if (!ino) return -1;
        uint64_t new_data = alloc_block();
        if (!new_data) return -1;

        memset(&inode, 0, sizeof(inode));
        strncpy(inode.name, name, XFS_NAME_MAX - 1);
        inode.flags = 0;
        inode.block_count = 1;
        inode.data_blocks[0] = (uint32_t)new_data;
        write_inode(ino, &inode);

        /* Zero the data block */
        uint8_t zero[XFS_BLOCK_SIZE];
        memset(zero, 0, XFS_BLOCK_SIZE);
        block_write(new_data, zero);

        /* Add dirent to parent */
        read_inode(parent_ino, &pinode);
        uint8_t sector[XFS_BLOCK_SIZE];
        bool added = false;
        for (uint32_t db = 0; db < pinode.block_count && !added; db++) {
            uint32_t b = pinode.data_blocks[db];
            if (!b) continue;
            block_read(b, sector);
            int max_dents = XFS_BLOCK_SIZE / sizeof(xfs_dirent_t);
            xfs_dirent_t *ents = (xfs_dirent_t *)sector;
            for (int d = 0; d < max_dents; d++) {
                if (ents[d].inode_block == 0) {
                    strncpy(ents[d].name, name, XFS_NAME_MAX - 1);
                    ents[d].inode_block = (uint32_t)ino;
                    ents[d].size = 0;
                    ents[d].flags = 0;
                    block_write(b, sector);
                    added = true;
                    break;
                }
            }
        }
        if (!added) {
            if (pinode.block_count >= 900) return -1;
            uint64_t nb = alloc_block();
            if (!nb) return -1;
            memset(sector, 0, XFS_BLOCK_SIZE);
            xfs_dirent_t *ents = (xfs_dirent_t *)sector;
            strncpy(ents[0].name, name, XFS_NAME_MAX - 1);
            ents[0].inode_block = (uint32_t)ino;
            ents[0].size = 0;
            ents[0].flags = 0;
            block_write(nb, sector);
            pinode.data_blocks[pinode.block_count] = (uint32_t)nb;
            pinode.block_count++;
            write_inode(parent_ino, &pinode);
        }
        read_inode(ino, &inode);
    }

    /* Write data blocks */
    size_t blocks_needed = (size + XFS_BLOCK_SIZE - 1) / XFS_BLOCK_SIZE;
    if (blocks_needed == 0) blocks_needed = 1;
    if (blocks_needed > 900) {
        fprintf(stderr, "xfs_mkfs: file %s too large (%zu bytes)\n", xfs_path, size);
        return -1;
    }

    /* Allocate new blocks if needed */
    while (inode.block_count < blocks_needed) {
        uint64_t nb = alloc_block();
        if (!nb) {
            fprintf(stderr, "xfs_mkfs: out of space writing %s\n", xfs_path);
            return -1;
        }
        inode.data_blocks[inode.block_count] = (uint32_t)nb;
        inode.block_count++;
    }

    /* Write data */
    size_t done = 0;
    while (done < size) {
        uint32_t block_idx = (uint32_t)(done / XFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)(done % XFS_BLOCK_SIZE);
        uint32_t b = inode.data_blocks[block_idx];
        uint8_t sector[XFS_BLOCK_SIZE];
        if (block_off != 0 || (size - done) < XFS_BLOCK_SIZE) {
            block_read(b, sector);
        } else {
            memset(sector, 0, XFS_BLOCK_SIZE);
        }
        size_t chunk = XFS_BLOCK_SIZE - block_off;
        if (chunk > size - done) chunk = size - done;
        memcpy(sector + block_off, (const uint8_t *)data + done, chunk);
        block_write(b, sector);
        done += chunk;
    }

    inode.size = (uint32_t)size;
    write_inode(ino, &inode);
    return 0;
}

/* ---- Format ---- */

static void xfs_format(uint64_t total_blocks) {
    g_total = total_blocks;
    g_bitmap_blocks = (g_total + 8 * XFS_BLOCK_SIZE - 1) / (8 * XFS_BLOCK_SIZE);
    if (g_bitmap_blocks < 1) g_bitmap_blocks = 1;
    g_data_start = 1 + g_bitmap_blocks;
    g_root_block = g_data_start;
    g_next_free = g_data_start + 2;

    /* Zero everything */
    uint8_t zero[XFS_BLOCK_SIZE];
    memset(zero, 0, XFS_BLOCK_SIZE);
    for (uint64_t i = 0; i < g_total; i++)
        block_write(i, zero);

    /* Superblock */
    uint8_t sb_buf[XFS_BLOCK_SIZE];
    memset(sb_buf, 0, XFS_BLOCK_SIZE);
    xfs_super_t *sb = (xfs_super_t *)sb_buf;
    sb->magic = XFS_MAGIC;
    sb->version = XFS_VERSION;
    sb->total_blocks = g_total;
    sb->free_blocks = g_total - (g_data_start + 2);
    sb->root_block = g_root_block;
    sb->bitmap_blocks = g_bitmap_blocks;
    sb->data_start = g_data_start;
    block_write(0, sb_buf);

    /* Bitmap: mark used blocks */
    for (uint64_t b = 0; b < g_data_start + 2; b++)
        bitmap_set(b, true);

    /* Root directory inode */
    uint8_t root_buf[XFS_BLOCK_SIZE];
    memset(root_buf, 0, XFS_BLOCK_SIZE);
    xfs_inode_t *root = (xfs_inode_t *)root_buf;
    strcpy(root->name, "/");
    root->flags = 1;
    root->block_count = 1;
    root->data_blocks[0] = (uint32_t)(g_root_block + 1);
    block_write(g_root_block, root_buf);

    /* Zero root directory data block */
    memset(zero, 0, XFS_BLOCK_SIZE);
    block_write(g_root_block + 1, zero);
}

/* ---- Core directories — created once at format time ---- */

static void create_core_dirs(void) {
    /* Only what the OS actually uses. No legacy baggage. */
    xfs_mkdir("/bin");
    xfs_mkdir("/sbin");
    xfs_mkdir("/Applications");
    xfs_mkdir("/Applications/Terminal.app");
    xfs_mkdir("/Applications/Terminal.app/Contents");
    xfs_mkdir("/Applications/Terminal.app/Contents/Xos");
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <disk.img> <size_mb> [host_path:xfs_path ...]\n",
                argv[0]);
        return 1;
    }

    const char *img_path = argv[1];
    int size_mb = atoi(argv[2]);
    if (size_mb < 1) size_mb = 4;

    uint64_t total_blocks = (uint64_t)size_mb * 1024 * 1024 / XFS_BLOCK_SIZE;

    /* Create the disk image */
    g_img = fopen(img_path, "wb+");
    if (!g_img) {
        fprintf(stderr, "xfs_mkfs: cannot create %s\n", img_path);
        return 1;
    }

    /* Extend to full size */
    uint8_t zero[XFS_BLOCK_SIZE];
    memset(zero, 0, XFS_BLOCK_SIZE);
    for (uint64_t i = 0; i < total_blocks; i++)
        fwrite(zero, 1, XFS_BLOCK_SIZE, g_img);
    fflush(g_img);

    /* Format */
    xfs_format(total_blocks);
    printf("[xfs_mkfs] formatted %lu blocks (%d MB)\n",
           (unsigned long)total_blocks, size_mb);

    /* Create core directories */
    create_core_dirs();

    /* Copy files into pre-created directories */
    int n_files = 0;
    for (int i = 3; i < argc; i++) {
        char *arg = argv[i];
        char *colon = strchr(arg, ':');
        if (!colon) {
            fprintf(stderr, "xfs_mkfs: skipping malformed arg '%s'\n", arg);
            continue;
        }
        *colon = '\0';
        const char *host_path = arg;
        const char *xfs_path = colon + 1;

        FILE *f = fopen(host_path, "rb");
        if (!f) {
            fprintf(stderr, "xfs_mkfs: cannot open %s — skipping\n", host_path);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize <= 0) {
            fprintf(stderr, "xfs_mkfs: empty file %s — skipping\n", host_path);
            fclose(f);
            continue;
        }
        uint8_t *buf = malloc((size_t)fsize);
        if (!buf) {
            fprintf(stderr, "xfs_mkfs: out of memory for %s\n", host_path);
            fclose(f);
            continue;
        }
        fread(buf, 1, (size_t)fsize, f);
        fclose(f);

        if (xfs_write_file(xfs_path, buf, (size_t)fsize) == 0) {
            printf("[xfs_mkfs] %-40s → %s (%ld bytes)\n",
                   host_path, xfs_path, fsize);
            n_files++;
        }
        free(buf);
    }

    /* Update superblock free_blocks count */
    uint8_t sb_buf[XFS_BLOCK_SIZE];
    block_read(0, sb_buf);
    xfs_super_t *sb = (xfs_super_t *)sb_buf;
    uint64_t used = 0;
    for (uint64_t b = 0; b < g_total; b++)
        if (bitmap_get(b)) used++;
    sb->free_blocks = g_total - used;
    block_write(0, sb_buf);

    printf("[xfs_mkfs] done: %d files, %lu/%lu blocks used\n",
           n_files, (unsigned long)used, (unsigned long)g_total);

    fclose(g_img);
    return 0;
}
