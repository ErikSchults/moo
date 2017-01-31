#include "vfs.h"
#include "debug.h"
#include "string.h"
#include "ring.h"
#include "liballoc.h"
#include "task.h"
#include "mutex.h"
#include "libc.h"
#include "errno.h"
#include <stdbool.h>

#define MAX_FS_TYPES_COUNT 10

static struct vfs_fs_type *registered_fs[MAX_FS_TYPES_COUNT];
static vfs_node_t *vfs_root = 0;
static mutex_t vfs_mutex = 0;

/**
 * Must be used only for debug.
 * Function isn't thread safe.
 */
void print_vfs_tree(struct vfs_node *node, uint32_t level)
{
    if (node == NULL) {
        node = vfs_root;
    }
    if (node != NULL) {
        for(uint32_t i = 0; i < level; i++) {
            debug("  ");
        }
        debug("%s\n", node->name);
        struct vfs_node *iterator = node->children;
        if ((node->mode & S_MNT) == S_MNT) {
            iterator = ((struct vfs_node*)node->obj)->children;
        }
        while(iterator != NULL) {
            print_vfs_tree(iterator, level + 1);
            iterator = (struct vfs_node*)iterator->list.next;
        }
    }
}

/**
 * Function isn't thread safe.
 */
static struct vfs_fs_type *get_fs(char *name)
{
    for(uint16_t i = 0; i < MAX_FS_TYPES_COUNT; i++) {
        if (strcmp(registered_fs[i]->name, name) == 0) {
            return registered_fs[i];
        }
    }
    return NULL;
}

/**
 * Function isn't thread safe.
 */
static struct vfs_node *lookup(char *path)
{
    debug("[vfs] (pid: %i) lookup (%s)\n", get_pid(), path);
    char *token = strtok_r(path, "/", &path);
    struct vfs_node *node = vfs_root;
    while (token != NULL && strlen(token) > 0 && node != NULL) {
        if ((node->mode & S_IFLNK) == S_IFLNK) {
            char *str = strdup(node->obj);
            node = lookup(str);
            kfree(str);
        }

        if ((node->mode & S_IFDIR) != S_IFDIR) {
            return NULL;
        }

        node = node->ops->lookup(node, token);

        if (node == NULL) {
            return NULL;
        }
        if ((node->mode & S_MNT) == S_MNT) {
            node = node->obj;
        }
        token = strtok_r(path, "/", &path);
    }
    return node;
}

/**
 * Function isn't thread safe.
 */
int create_vfs_node(char *path, mode_t mode, vfs_file_operations_t *file_ops, void *obj, vfs_node_t **out)
{
    debug("[vfs] (pid: %i) create_vfs_node (%s)\n", get_pid(), path);
    if (vfs_root == NULL) {
        return -EFAULT;
    }
    char *chopped = strrchr(path, '/');
    if (chopped == NULL) {
        debug("create_vfs_node failed because of empty node name\n");
        return -EFAULT;
    }
    *chopped++ = '\0';

    // todo: fix stupid if
    struct vfs_node *parent = 0;
    if (strlen(path) == 0) {
        parent = lookup("/");
    } else {
        parent = lookup(path);
    }

    if (parent == NULL) {
        return -ENOENT;
    }
    if ((parent->mode & S_IFDIR) != S_IFDIR) {
        return -ENOTDIR;
    }
    if (parent->ops == NULL || parent->ops->lookup == NULL) {
        debug("create_vfs_node failed because parent node has no lookup func\n");
        return -EFAULT;
    }
    if (parent->ops->lookup(parent, chopped) != NULL) {
        return -EEXIST;
    }

    return parent->ops->create_node(parent, chopped, mode, file_ops, obj, out);
}

int register_fs(struct vfs_fs_type *fs)
{
    debug("[vfs] (pid: %i) register fs (%s)\n", get_pid(), fs->name);
    mutex_lock(&vfs_mutex);
    if (get_fs(fs->name) != NULL) {
        mutex_release(&vfs_mutex);
        return -EEXIST;
    }

    for(uint16_t i = 0; i < MAX_FS_TYPES_COUNT; i++) {
        if (registered_fs[i] == NULL) {
            registered_fs[i] = fs;
            debug("[vfs] filesystem type registration: %s\n", fs->name);
            mutex_release(&vfs_mutex);
            return 0;
        }
    }

    mutex_release(&vfs_mutex);
    return -ENOMEM;
}

static int canonicalize_path(char *path, char *out, uint32_t size)
{
    char *copy = strdup(path);
    ring_t *ring = create_ring(MAX_PATH_DEPTH);
    int err = 0;

    char *token;

    if (copy[0] != '/') {
        char *base = strdup(current_process->cur_dir);
        while((token = strtok_r(base, "/", &base))) {
            if (strlen(token) > 0) {
                if (strcmp(token, VFS_CURRENT_DIR) == 0) {
                    continue;
                } else if (strcmp(token, VFS_PARENT_DIR) == 0) {
                    ring->head_pop(ring);
                } else {
                    ring->push(ring, token);
                }
            }
        }
        kfree(base);
    }

    while((token = strtok_r(copy, "/", &copy))) {
        if (strlen(token) > 0) {
            if (strcmp(token, VFS_CURRENT_DIR) == 0) {
                continue;
            } else if (strcmp(token, VFS_PARENT_DIR) == 0) {
                ring->head_pop(ring);
            } else {
                ring->push(ring, token);
            }
        }
    }

    *out++ = '/';
    uint8_t separate = false;
    char *part = ring->pop(ring);
    uint32_t copied = 0;
    while(part != NULL) {
        if (separate == true) {
            *out++ = '/';
        }
        uint32_t len = strlen(part);
        memcpy(out, part, len);
        out += len;
        copied += len;
        if (copied >= (size - 1)) { // -1 because of EOS char in result string
            err = -ENOMEM;
            goto cleanup;
        }

        separate = true;
        part = ring->pop(ring);
    }
    *out = '\0';

cleanup:
    kfree(copy);
    while(ring->pop(ring));
    ring->free(ring);
    return err;
}

int mkdir(char *path)
{
    debug("[vfs] (pid: %i) mkdir (%s)\n", get_pid(), path);
    int err = 0;
    char *canonical = kmalloc(MAX_PATH_LENGTH);
    err = canonicalize_path(path, canonical, MAX_PATH_LENGTH);
    if (err) {
        goto cleanup;
    }

    mutex_lock(&vfs_mutex);
    err = create_vfs_node(canonical, S_IFDIR, NULL, NULL, NULL);
    mutex_release(&vfs_mutex);
cleanup:
    kfree(canonical);
    return err;
}

int symlink(char *path, char *target_path)
{
    int err = 0;
    char *canonical = kmalloc(MAX_PATH_LENGTH);
    err = canonicalize_path(path, canonical, MAX_PATH_LENGTH);
    if (err) {
        goto cleanup;
    }
    char *target = kmalloc(MAX_PATH_LENGTH);
    err = canonicalize_path(target_path, target, MAX_PATH_LENGTH);
    if (err) {
        goto cleanup;
    }

    uint32_t len = strlen(target);
    char *save = kmalloc(len);
    memcpy(save, target, len);

    mutex_lock(&vfs_mutex);
    err = create_vfs_node(canonical, S_IFLNK, NULL, save, NULL);
    mutex_release(&vfs_mutex);
cleanup:
    kfree(canonical);
    if (target > 0) kfree(target);
    if (save > 0 && err) kfree(save);
    return err;
}

int sys_read(file_descriptor_t fd, void *buf, uint32_t size)
{
    if (fd >= MAX_OPENED_FILES || fd < 0 || current_process->files[fd] == NULL) {
        return -EBADF;
    }

    vfs_file_t *file = current_process->files[fd];
    if (file->ops == NULL || file->ops->read == NULL) {
        return -EPERM;
    }

    int err = file->ops->read(file, buf, size, &file->pos);
    return err;
}

int sys_write(file_descriptor_t fd, void *buf, uint32_t size)
{
    if (fd >= MAX_OPENED_FILES || fd < 0 || current_process->files[fd] == NULL) {
        return -EBADF;
    }

    vfs_file_t *file = current_process->files[fd];
    if (file->ops == NULL || file->ops->write == NULL)
    {
        return -EPERM;
    }
    int err = file->ops->write(file, buf, size, &file->pos);
    return err;
}

file_descriptor_t sys_open(char *path)
{
    mutex_lock(&vfs_mutex);
    int err = 0;
    vfs_file_t *file = 0;
    char *canonical = kmalloc(MAX_PATH_LENGTH);
    err = canonicalize_path(path, canonical, MAX_PATH_LENGTH);
    if (err) {
        goto mutex_cleanup;
    }

    struct vfs_node *node = lookup(canonical);
    if (node == NULL) {
        err = -ENOENT;
        goto mutex_cleanup;
    }

    for(uint32_t i = 0; i < MAX_OPENED_FILES; i++) {
        if (current_process->files[i] == NULL) {
            file = kmalloc(sizeof(vfs_file_t));
            memset(file, 0, sizeof(vfs_file_t));
            file->ops = node->file_ops;
            file->node = node;
            file->pid = current_process->id;
            if (file->ops && file->ops->open) {
                err = file->ops->open(file, 0);
                if (err) {
                    goto mutex_cleanup;
                }
            }

            current_process->files[i] = file;
            err = i;
            goto mutex_cleanup;
        }
    }

mutex_cleanup:
    mutex_release(&vfs_mutex);
    if (file != NULL && err < 0) kfree(file);
    kfree(canonical);
    return err;
}

int sys_close(file_descriptor_t fd)
{
    if (fd >= MAX_OPENED_FILES || fd < 0 || current_process->files[fd] == NULL) {
        return -EBADF;
    }

    vfs_file_t *file = current_process->files[fd];
    if (file->ops != NULL && file->ops->close != NULL) {
        int err = file->ops->close(file);
        if (err) {
            return err;
        }
    }
    current_process->files[fd] = NULL;
    kfree(file);
    return 0;
}

int fstat(file_descriptor_t fd, struct stat *buf)
{
    return 0;
}

int fcntl(int fd, int cmd, int arg)
{
    if (fd >= MAX_OPENED_FILES || fd < 0 || current_process->files[fd] == NULL) {
        return -EBADF;
    }

    switch (cmd) {
        case F_DUPFD:
            for(uint32_t i = cmd; i < MAX_OPENED_FILES; i++) {
                if (current_process->files[i] == NULL) {
                    current_process->files[i] = current_process->files[fd];
                    return i;
                }
            }
            return -EMFILE;
        break;
        case F_DUPFD_CLOEXEC:
            for(uint32_t i = cmd; i < MAX_OPENED_FILES; i++) {
                if (current_process->files[i] == NULL) {
                    current_process->files[i] = current_process->files[fd];
                    current_process->files[i]->flags |= FD_CLOEXEC;
                    return i;
                }
            }
        break;
        case F_GETFD:
            return current_process->files[fd]->flags;
        break;
        case F_SETFD:
            return current_process->files[fd]->flags = arg;
        break;
        default:
            debug("unknown fcntl cmd %i\n", cmd);
            return -EINVAL;
        break;
    }

    return -EINVAL;
}

int stat_fs(char *path, struct stat *buf)
{
    if (vfs_root == NULL) {
        return -EFAULT;
    }

    int err = 0;
    char *canonical = kmalloc(MAX_PATH_LENGTH);
    err = canonicalize_path(path, canonical, MAX_PATH_LENGTH);
    if (err) {
        goto cleanup;
    }

    mutex_lock(&vfs_mutex);
    struct vfs_node *node = lookup(canonical);
    if (node == NULL) {
        err = -ENOENT;
        goto mutex_cleanup;
    }
    memset(buf, 0, sizeof(struct stat));
    buf->st_blksize = VFS_BLOCK_SIZE;
    buf->st_blocks = ALIGN(node->size, VFS_BLOCK_SIZE) / VFS_BLOCK_SIZE;
    buf->st_size = node->size;
    buf->st_mode = node->mode;

mutex_cleanup:
    mutex_release(&vfs_mutex);
cleanup:
    kfree(canonical);
    return err;
}

int mount_fs(char *path, char *fs_name, struct ata_device *dev)
{
    int err = 0;
    char *canonical = kmalloc(MAX_PATH_LENGTH);
    err = canonicalize_path(path, canonical, MAX_PATH_LENGTH);
    if (err) {
        goto cleanup;
    }

    mutex_lock(&vfs_mutex);
    struct vfs_fs_type *fs = get_fs(fs_name);
    if (fs == NULL) {
        err = -ENOENT;
        goto mutex_cleanup;
    }

    struct vfs_super *super = fs->ops->read_super(dev);
    struct vfs_node *local_root = super->ops->spawn(super, "/", S_IFDIR);

    if (strcmp(canonical, "/") == 0) {
        if (vfs_root != NULL) {
            err = -EBUSY;
            goto mutex_cleanup;
        }
        vfs_root = local_root;
        debug("[vfs] vfs_root initialized\n");
    } else {
        err = create_vfs_node(canonical, S_MNT, NULL, local_root, NULL);
    }

mutex_cleanup:
    mutex_release(&vfs_mutex);
cleanup:
    kfree(canonical);
    return err;
}

int chdir(char *path)
{
    assert(strlen(path) < MAX_PATH_LENGTH);

    int err = 0;
    char *canonical = kmalloc(MAX_PATH_LENGTH);
    err = canonicalize_path(path, canonical, MAX_PATH_LENGTH);
    if (err) {
        goto cleanup;
    }

    struct stat st;
    err = stat_fs(path, &st);
    if (err) {
        goto cleanup;
    }
    if ((st.st_mode & S_IFDIR) != S_IFDIR) {
        err = -ENOTDIR;
        goto cleanup;
    }
    strcpy(current_process->cur_dir, path);

cleanup:
    kfree(canonical);
    return err;
}
