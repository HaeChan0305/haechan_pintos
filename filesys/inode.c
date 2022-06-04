#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_CLUSTER_SIZE bytes long. */
struct inode_disk {
	cluster_t start;                	/* First data cluster. */
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	//uint32_t unused[125];               /* Not used. */
	uint32_t unused[253];               /* Not used. */
};

/* In-memory inode. */
struct inode {
	struct list_elem elem;              /* Element in inode list. */
	cluster_t cluster;                  /* Cluster number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	struct inode_disk data;             /* Inode content. */
};

/* Returns the number of clusters to allocate for an inode SIZE
 * bytes long. */
static inline size_t
bytes_to_clusters (off_t size) {
	size_t clusters = DIV_ROUND_UP (size, DISK_CLUSTER_SIZE);
	return (clusters == 0) ? 1 : clusters;
	//return clusters;
}

/* Returns the disk cluster that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */
static cluster_t
byte_to_cluster(const struct inode *inode, off_t pos){
	ASSERT (inode != NULL);
	if (pos < inode->data.length){
		cluster_t result = inode->data.start;
		for(int i = 0; i <  pos / DISK_CLUSTER_SIZE; i++){
			result = fat_get(result);
			ASSERT(result != EOChain);
		}

		return result;
	}

	else
		return -1;
}

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to cluster CLUSTER on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
bool
inode_create (cluster_t cluster, off_t length) {
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT(length >= 0); /* WARNING!!! Consider case of (length == 0) again. */
	ASSERT(cluster != EMPTY);

	/* If this assertion fails, the inode structure is not exactly
	 * one cluster in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_CLUSTER_SIZE);

	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		size_t clusters = bytes_to_clusters(length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		if(fat_create_chain_multiple(clusters, &disk_inode->start)){
			disk_write_clst(filesys_disk, cluster, disk_inode);
			if(clusters > 0){
				static char zeros[DISK_CLUSTER_SIZE];
				size_t i;

				cluster_t clst_idx = disk_inode->start;
				for (i = 0; i < clusters; i++){
					disk_write_clst(filesys_disk, clst_idx, zeros);
					clst_idx = fat_get(clst_idx);
				}
			}
			success = true;
		}
		free (disk_inode);
	}
	return success;
}

/* Reads an inode from CLUSTER
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (cluster_t cluster) {
	struct list_elem *e;
	struct inode *inode;

	/* Check whether this inode is already open. */
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e)) {
		inode = list_entry (e, struct inode, elem);
		if (inode->cluster == cluster) {
			inode_reopen (inode);
			return inode; 
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front (&open_inodes, &inode->elem);
	inode->cluster = cluster;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	disk_read_clst(filesys_disk, inode->cluster, &inode->data);
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode) {
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* Returns INODE's inode number. */
cluster_t
inode_get_inumber (const struct inode *inode) {
	return inode->cluster;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			fat_remove_chain(inode->cluster, EMPTY);
			fat_remove_chain(inode->data.start, EMPTY);
		}
		// else
		// 	disk_write_clst(filesys_disk, inode->cluster, &inode->data);

		free (inode); 
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void
inode_remove (struct inode *inode) {
	ASSERT (inode != NULL);
	inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;

	while (size > 0) {
		/* Disk cluster to read, starting byte offset within cluster. */
		cluster_t cluster_idx = byte_to_cluster(inode, offset);
		int cluster_ofs = offset % DISK_CLUSTER_SIZE;

		/* Bytes left in inode, bytes left in cluster, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int cluster_left = DISK_CLUSTER_SIZE - cluster_ofs;
		int min_left = inode_left < cluster_left ? inode_left : cluster_left;

		/* Number of bytes to actually copy out of this cluster. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (cluster_ofs == 0 && chunk_size == DISK_CLUSTER_SIZE) {
			/* Read full cluster directly into caller's buffer. */
			disk_read_clst(filesys_disk, cluster_idx, buffer + bytes_read); 
		} else {
			/* Read cluster into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_CLUSTER_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read_clst(filesys_disk, cluster_idx, bounce);
			memcpy (buffer + bytes_read, bounce + cluster_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free (bounce);

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset) {
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;

	if (inode->deny_write_cnt)
		return 0;

	while (size > 0) {
		/* Cluster to write, starting byte offset within cluster. */
		cluster_t cluster_idx = byte_to_cluster(inode, offset);
		int cluster_ofs = offset % DISK_CLUSTER_SIZE;

		/* Bytes left in inode, bytes left in cluster, lesser of the two. */
		printf("\n\n%d\n%d\n", inode_length (inode), offset);
		off_t inode_left = inode_length (inode) - offset;
		int cluster_left = DISK_CLUSTER_SIZE - cluster_ofs;
		int min_left = inode_left < cluster_left ? inode_left : cluster_left;

		/* Number of bytes to actually write into this cluster. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0){
			printf("\n\ngotloqkf\n\n");
			break;
		}

		if (cluster_ofs == 0 && chunk_size == DISK_CLUSTER_SIZE) {
			/* Write full cluster directly to disk. */
			disk_write_clst(filesys_disk, cluster_idx, buffer + bytes_written); 
		} else {
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_CLUSTER_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the cluster contains data before or after the chunk
			   we're writing, then we need to read in the cluster
			   first.  Otherwise we start with a cluster of all zeros. */
			if (cluster_ofs > 0 || chunk_size < cluster_left) 
				disk_read_clst(filesys_disk, cluster_idx, bounce);
			else
				memset (bounce, 0, DISK_CLUSTER_SIZE);
			memcpy (bounce + cluster_ofs, buffer + bytes_written, chunk_size);
			disk_write_clst(filesys_disk, cluster_idx, bounce); 
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free (bounce);

	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
	void
inode_deny_write (struct inode *inode) 
{
	inode->deny_write_cnt++;
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) {
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode) {
	return inode->data.length;
}
