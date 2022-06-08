#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

/* Disk used for file system. */
extern struct disk *filesys_disk;

/* Sturcture for contain file and dir both. */
struct item{
    bool is_dir;
    union{
        struct file *file;
        struct dir *dir;
    };
};

struct dir *accessing_path(const char *, char **, bool);

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
bool filesys_create_dir (const char *path);
struct file *filesys_open (const char *path);
struct item *filesys_open_item (const char *path);
bool filesys_remove (const char *name);

void item_close(struct item *item);
struct item *item_duplicate(struct item *item);
#endif /* filesys/filesys.h */
