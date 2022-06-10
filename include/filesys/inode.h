#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"
#include "filesys/fat.h"

struct bitmap;

void inode_init (void);
bool inode_create (cluster_t, off_t, bool, bool);
struct inode *inode_open (cluster_t);
struct inode *inode_reopen (struct inode *);
cluster_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool inode_is_sym (const struct inode *);
bool inode_is_dir (const struct inode *);
bool inode_is_in_root_dir (const struct inode *);
bool inode_is_root_dir (const struct inode *);
int inode_open_cnt (const struct inode *);
off_t inode_items (const struct inode *);
void inode_items_incr(struct inode *);
void inode_items_decr(struct inode *);

#endif /* filesys/inode.h */