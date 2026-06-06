/* XFS — X OS File System
 * Simple extent-based filesystem for a modern OS.
 */

#include "kernel/fs/xfs.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/memory/heap.h"
#include "kernel/sched/sched.h"

/* -------------------------------------------------------------------------- */
/* On-disk structures */

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

/* -------------------------------------------------------------------------- */
/* Runtime state */

static block_dev_t *g_dev = NULL;
static uint64_t g_total = 0;
static uint64_t g_bitmap_blocks = 0;
static uint64_t g_data_start = 0;
static uint64_t g_root_block = 0;

typedef struct {
    uint32_t pid;
    uint32_t inode_block;
    uint32_t offset;      /* current read/write position */
    uint32_t flags;
    bool     used;
} xfs_fd_entry_t;

static xfs_fd_entry_t g_fds[XFS_MAX_FDS];

/* -------------------------------------------------------------------------- */
/* Bitmap helpers (1 bit per block, 1 = used) */

static void bitmap_set(uint64_t block, bool used) {
    uint64_t byte = block / 8;
    uint64_t bit  = block % 8;
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(g_dev, 1 + byte / XFS_BLOCK_SIZE, 1, buf);
    if (used) buf[byte % XFS_BLOCK_SIZE] |= (1 << bit);
    else      buf[byte % XFS_BLOCK_SIZE] &= ~(1 << bit);
    block_write(g_dev, 1 + byte / XFS_BLOCK_SIZE, 1, buf);
}

static bool bitmap_get(uint64_t block) {
    uint64_t byte = block / 8;
    uint64_t bit  = block % 8;
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(g_dev, 1 + byte / XFS_BLOCK_SIZE, 1, buf);
    return (buf[byte % XFS_BLOCK_SIZE] >> bit) & 1;
}

static uint64_t alloc_block(void) {
    for (uint64_t b = g_data_start; b < g_total; b++) {
        if (!bitmap_get(b)) {
            bitmap_set(b, true);
            return b;
        }
    }
    return 0; /* out of space */
}

static void free_block(uint64_t block) {
    if (block >= g_total) return;
    bitmap_set(block, false);
}

/* -------------------------------------------------------------------------- */
/* Inode helpers */

static void read_inode(uint64_t block, xfs_inode_t *inode) {
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(g_dev, block, 1, buf);
    memcpy(inode, buf, sizeof(xfs_inode_t));
}

static void write_inode(uint64_t block, const xfs_inode_t *inode) {
    uint8_t buf[XFS_BLOCK_SIZE];
    block_read(g_dev, block, 1, buf);
    memcpy(buf, inode, sizeof(xfs_inode_t));
    block_write(g_dev, block, 1, buf);
}

/* -------------------------------------------------------------------------- */
/* Path resolution — split on '/' */

static uint64_t resolve_path(const char *path, xfs_inode_t *out) {
    if (!path || path[0] != '/') return 0;

    uint64_t current = g_root_block;
    read_inode(current, out);
    if (!(out->flags & 1)) return 0; /* root not a directory */

    if (path[1] == '\0') return current; /* path is just "/" */

    char buf[XFS_NAME_MAX];
    const char *p = path + 1;

    for (;;) {
        /* copy next path component into buf */
        size_t i = 0;
        while (p[i] && p[i] != '/' && i < XFS_NAME_MAX - 1) {
            buf[i] = p[i];
            i++;
        }
        buf[i] = '\0';

        /* scan directory entries */
        bool found = false;
        uint64_t next_block = 0;
        for (uint32_t db = 0; db < out->block_count && !found; db++) {
            uint32_t b = out->data_blocks[db];
            if (!b) continue;
            uint8_t sector[XFS_BLOCK_SIZE];
            block_read(g_dev, b, 1, sector);
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

/* -------------------------------------------------------------------------- */
/* Format */

bool xfs_format(block_dev_t *dev) {
    if (!dev) return false;
    g_dev = dev;
    g_total = dev->block_count;

    /* Bitmap: 1 bit per block. ceil(total/8) bytes. */
    g_bitmap_blocks = (g_total + 8 * XFS_BLOCK_SIZE - 1) / (8 * XFS_BLOCK_SIZE);
    if (g_bitmap_blocks < 1) g_bitmap_blocks = 1;
    g_data_start = 1 + g_bitmap_blocks;
    g_root_block = g_data_start;

    /* Clear everything */
    uint8_t zero[XFS_BLOCK_SIZE];
    memset(zero, 0, XFS_BLOCK_SIZE);
    for (uint64_t i = 1; i < g_total; i++) {
        block_write(dev, i, 1, zero);
    }

    /* Superblock */
    uint8_t sb_buf[XFS_BLOCK_SIZE];
    memset(sb_buf, 0, XFS_BLOCK_SIZE);
    xfs_super_t *sb = (xfs_super_t *)sb_buf;
    sb->magic = XFS_MAGIC;
    sb->version = XFS_VERSION;
    sb->total_blocks = g_total;
    sb->free_blocks = g_total - (g_data_start + 2); /* root inode + data */
    sb->root_block = g_root_block;
    sb->bitmap_blocks = g_bitmap_blocks;
    sb->data_start = g_data_start;
    block_write(dev, 0, 1, sb_buf);

    /* Bitmap: mark used blocks */
    for (uint64_t b = 0; b < g_data_start + 2; b++) {
        bitmap_set(b, true);
    }

    /* Root directory inode */
    uint8_t root_buf[XFS_BLOCK_SIZE];
    memset(root_buf, 0, XFS_BLOCK_SIZE);
    xfs_inode_t *root = (xfs_inode_t *)root_buf;
    strcpy(root->name, "/");
    root->flags = 1; /* directory */
    root->block_count = 1;
    root->data_blocks[0] = g_root_block + 1;
    block_write(dev, g_root_block, 1, root_buf);

    kprintf("[xfs] formatted %lu blocks, root at %lu\n", g_total, g_root_block);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Mount */

bool xfs_mount(block_dev_t *dev) {
    if (!dev) return false;
    uint8_t sb_buf[XFS_BLOCK_SIZE];
    block_read(dev, 0, 1, sb_buf);
    xfs_super_t *sb = (xfs_super_t *)sb_buf;
    if (sb->magic != XFS_MAGIC) {
        kputs("[xfs] mount: bad magic\n");
        return false;
    }
    g_dev = dev;
    g_total = sb->total_blocks;
    g_bitmap_blocks = sb->bitmap_blocks;
    g_data_start = sb->data_start;
    g_root_block = sb->root_block;
    memset(g_fds, 0, sizeof(g_fds));
    kprintf("[xfs] mounted %lu blocks, root=%lu\n", g_total, g_root_block);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Open */

int xfs_open(const char *path, uint32_t flags) {
    if (!path || path[0] != '/') return -1;

    xfs_inode_t inode;
    uint64_t ino = resolve_path(path, &inode);

    /* Create if requested and doesn't exist */
    if (!ino && (flags & XFS_O_CREAT)) {
        /* Find parent directory */
        const char *name = path + 1;
        const char *last_slash = path;
        for (const char *p = path; *p; p++) {
            if (*p == '/') last_slash = p;
        }
        if (last_slash == path) {
            /* Create in root */
            ino = g_root_block;
        } else {
            char parent_path[XFS_NAME_MAX];
            size_t plen = (size_t)(last_slash - path);
            if (plen >= XFS_NAME_MAX) plen = XFS_NAME_MAX - 1;
            memcpy(parent_path, path, plen);
            parent_path[plen] = '\0';
            ino = resolve_path(parent_path, &inode);
            if (!ino) return -1;
        }
        name = last_slash + 1;
        if (*name == '\0') return -1;

        /* Allocate inode + data block for new file */
        uint64_t new_ino = alloc_block();
        if (!new_ino) return -1;
        uint64_t new_data = alloc_block();
        if (!new_data) { free_block(new_ino); return -1; }

        xfs_inode_t new_file;
        memset(&new_file, 0, sizeof(new_file));
        strncpy(new_file.name, name, XFS_NAME_MAX - 1);
        new_file.size = 0;
        new_file.flags = 0;
        new_file.block_count = 1;
        new_file.data_blocks[0] = new_data;
        write_inode(new_ino, &new_file);

        /* Add dirent to parent */
        read_inode(ino, &inode);
        uint8_t sector[XFS_BLOCK_SIZE];
        bool added = false;
        for (uint32_t db = 0; db < inode.block_count && !added; db++) {
            uint32_t b = inode.data_blocks[db];
            block_read(g_dev, b, 1, sector);
            int max_dents = XFS_BLOCK_SIZE / sizeof(xfs_dirent_t);
            xfs_dirent_t *ents = (xfs_dirent_t *)sector;
            for (int d = 0; d < max_dents; d++) {
                if (ents[d].inode_block == 0) {
                    strncpy(ents[d].name, name, XFS_NAME_MAX - 1);
                    ents[d].inode_block = (uint32_t)new_ino;
                    ents[d].size = 0;
                    ents[d].flags = 0;
                    block_write(g_dev, b, 1, sector);
                    added = true;
                    break;
                }
            }
        }
        if (!added) {
            free_block(new_data);
            free_block(new_ino);
            return -1;
        }

        ino = new_ino;
        read_inode(ino, &inode);
    }

    if (!ino) return -1;

    /* Allocate fd */
    uint32_t pid = proc_current()->pid;
    for (int i = 0; i < XFS_MAX_FDS; i++) {
        if (!g_fds[i].used) {
            g_fds[i].used = true;
            g_fds[i].pid = pid;
            g_fds[i].inode_block = (uint32_t)ino;
            g_fds[i].offset = 0;
            g_fds[i].flags = flags;
            return i;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Close */

void xfs_close(int fd) {
    if (fd < 0 || fd >= XFS_MAX_FDS) return;
    if (!g_fds[fd].used) return;
    g_fds[fd].used = false;
}

/* -------------------------------------------------------------------------- */
/* Read */

int xfs_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= XFS_MAX_FDS) return -1;
    xfs_fd_entry_t *f = &g_fds[fd];
    if (!f->used || f->pid != proc_current()->pid) return -1;
    if (!buf || count == 0) return 0;

    xfs_inode_t inode;
    read_inode(f->inode_block, &inode);
    if (f->offset >= inode.size) return 0;

    size_t remain = inode.size - f->offset;
    if (count > remain) count = remain;

    size_t done = 0;
    while (done < count) {
        uint32_t file_off = f->offset + (uint32_t)done;
        uint32_t block_idx = file_off / XFS_BLOCK_SIZE;
        uint32_t block_off = file_off % XFS_BLOCK_SIZE;
        if (block_idx >= inode.block_count) break;

        uint32_t b = inode.data_blocks[block_idx];
        if (!b) break;

        uint8_t sector[XFS_BLOCK_SIZE];
        block_read(g_dev, b, 1, sector);

        size_t chunk = XFS_BLOCK_SIZE - block_off;
        if (chunk > count - done) chunk = count - done;
        memcpy((uint8_t *)buf + done, sector + block_off, chunk);
        done += chunk;
    }

    f->offset += (uint32_t)done;
    return (int)done;
}

/* -------------------------------------------------------------------------- */
/* Write */

int xfs_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= XFS_MAX_FDS) return -1;
    xfs_fd_entry_t *f = &g_fds[fd];
    if (!f->used || f->pid != proc_current()->pid) return -1;
    if (!buf || count == 0) return 0;
    if (!(f->flags & (XFS_O_WRONLY | XFS_O_RDWR))) return -1;

    xfs_inode_t inode;
    read_inode(f->inode_block, &inode);

    size_t done = 0;
    while (done < count) {
        uint32_t file_off = f->offset + (uint32_t)done;
        uint32_t block_idx = file_off / XFS_BLOCK_SIZE;
        uint32_t block_off = file_off % XFS_BLOCK_SIZE;

        /* Allocate new block if needed */
        if (block_idx >= inode.block_count) {
            if (inode.block_count >= 240) break; /* max direct blocks */
            uint64_t nb = alloc_block();
            if (!nb) break;
            inode.data_blocks[inode.block_count] = (uint32_t)nb;
            inode.block_count++;
        }

        uint32_t b = inode.data_blocks[block_idx];
        if (!b) break;

        uint8_t sector[XFS_BLOCK_SIZE];
        if (block_off != 0 || (count - done) < XFS_BLOCK_SIZE) {
            block_read(g_dev, b, 1, sector);
        } else {
            memset(sector, 0, XFS_BLOCK_SIZE);
        }

        size_t chunk = XFS_BLOCK_SIZE - block_off;
        if (chunk > count - done) chunk = count - done;
        memcpy(sector + block_off, (const uint8_t *)buf + done, chunk);
        block_write(g_dev, b, 1, sector);
        done += chunk;
    }

    uint32_t new_size = f->offset + (uint32_t)done;
    if (new_size > inode.size) inode.size = new_size;
    f->offset = new_size;
    write_inode(f->inode_block, &inode);

    return (int)done;
}

/* -------------------------------------------------------------------------- */
/* Mkdir */

int xfs_mkdir(const char *path) {
    if (!path || path[0] != '/') return -1;

    /* Check if already exists */
    xfs_inode_t inode;
    if (resolve_path(path, &inode)) return -1;

    /* Find parent */
    const char *name = path + 1;
    const char *last_slash = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    uint64_t parent_ino;
    if (last_slash == path) {
        parent_ino = g_root_block;
    } else {
        char parent_path[XFS_NAME_MAX];
        size_t plen = (size_t)(last_slash - path);
        if (plen >= XFS_NAME_MAX) plen = XFS_NAME_MAX - 1;
        memcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
        parent_ino = resolve_path(parent_path, &inode);
        if (!parent_ino) return -1;
    }
    name = last_slash + 1;
    if (*name == '\0') return -1;

    uint64_t new_ino = alloc_block();
    if (!new_ino) return -1;
    uint64_t new_data = alloc_block();
    if (!new_data) { free_block(new_ino); return -1; }

    xfs_inode_t new_dir;
    memset(&new_dir, 0, sizeof(new_dir));
    strncpy(new_dir.name, name, XFS_NAME_MAX - 1);
    new_dir.flags = 1; /* directory */
    new_dir.block_count = 1;
    new_dir.data_blocks[0] = (uint32_t)new_data;
    write_inode(new_ino, &new_dir);

    /* Add dirent to parent */
    read_inode(parent_ino, &inode);
    uint8_t sector[XFS_BLOCK_SIZE];
    bool added = false;
    for (uint32_t db = 0; db < inode.block_count && !added; db++) {
        uint32_t b = inode.data_blocks[db];
        block_read(g_dev, b, 1, sector);
        int max_dents = XFS_BLOCK_SIZE / sizeof(xfs_dirent_t);
        xfs_dirent_t *ents = (xfs_dirent_t *)sector;
        for (int d = 0; d < max_dents; d++) {
            if (ents[d].inode_block == 0) {
                strncpy(ents[d].name, name, XFS_NAME_MAX - 1);
                ents[d].inode_block = (uint32_t)new_ino;
                ents[d].size = 0;
                ents[d].flags = 1;
                block_write(g_dev, b, 1, sector);
                added = true;
                break;
            }
        }
    }
    if (!added) {
        free_block(new_data);
        free_block(new_ino);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Readdir */

int xfs_readdir(int fd, xfs_dirent_t *entries, int max_entries) {
    if (fd < 0 || fd >= XFS_MAX_FDS) return -1;
    xfs_fd_entry_t *f = &g_fds[fd];
    if (!f->used || f->pid != proc_current()->pid) return -1;
    if (!entries || max_entries <= 0) return 0;

    xfs_inode_t inode;
    read_inode(f->inode_block, &inode);
    if (!(inode.flags & 1)) return -1; /* not a directory */

    int count = 0;
    for (uint32_t db = 0; db < inode.block_count && count < max_entries; db++) {
        uint32_t b = inode.data_blocks[db];
        if (!b) continue;
        uint8_t sector[XFS_BLOCK_SIZE];
        block_read(g_dev, b, 1, sector);
        int max_dents = XFS_BLOCK_SIZE / sizeof(xfs_dirent_t);
        xfs_dirent_t *ents = (xfs_dirent_t *)sector;
        for (int d = 0; d < max_dents && count < max_entries; d++) {
            if (ents[d].inode_block != 0) {
                entries[count++] = ents[d];
            }
        }
    }
    return count;
}
