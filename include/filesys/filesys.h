#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

/* Disk used for file system. */
extern struct disk *filesys_disk;

/* Sturct for contain file and dir both. */
struct item{
    bool is_dir;
    union{
        struct file *file;
        struct dir *dir;
    };
};

void filesys_init (bool format);
void filesys_done (void);
struct dir *accessing_path(const char *, char **lowest, bool);
bool filesys_create_file(const char *path, off_t initial_size);
bool filesys_create_dir(const char *path);
struct item *filesys_open (const char *name);
bool filesys_remove (const char *name);
void item_close(struct item *item);
struct item *item_duplicate(struct item *);

#endif /* filesys/filesys.h */
