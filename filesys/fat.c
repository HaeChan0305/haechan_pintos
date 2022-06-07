#include "filesys/fat.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include <string.h>

/* Should be less than DISK_SECTOR_SIZE */
struct fat_boot {
	unsigned int magic;
	unsigned int sectors_per_cluster; /* Fixed to 1 */
	unsigned int total_sectors;
	unsigned int fat_start;
	unsigned int fat_clusters; /* Size of FAT in sectors. */
	unsigned int fat_empty; /* # of empty clusters. */
	unsigned int root_dir_cluster;
};

/* FAT FS */
struct fat_fs {
	struct fat_boot bs;
	unsigned int *fat;
	unsigned int fat_length;
	cluster_t data_start;
	cluster_t last_clst;
	struct lock write_lock;
};

static struct fat_fs *fat_fs;
static struct lock fat_lock;

void fat_boot_create (void);
void fat_fs_init (void);

void
fat_init (void) {
	fat_fs = calloc (1, sizeof (struct fat_fs));
	if (fat_fs == NULL)
		PANIC ("FAT init failed");

	// Read boot sector from the disk
	unsigned int *bounce = malloc (DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT init failed");
	disk_read (filesys_disk, FAT_BOOT_SECTOR, bounce);
	memcpy (&fat_fs->bs, bounce, sizeof (fat_fs->bs));
	free (bounce);

	// Extract FAT info
	if (fat_fs->bs.magic != FAT_MAGIC)
		fat_boot_create ();
	fat_fs_init ();
	lock_init(&fat_lock);
}

void
fat_open (void) {
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT load failed");

	// Load FAT directly from the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_read = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_clusters; i++) {
		bytes_left = fat_size_in_bytes - bytes_read;
		if (bytes_left >= DISK_CLUSTER_SIZE) {
			for(size_t j = 0; j < SECTORS_PER_CLUSTER; j++){
				disk_read(filesys_disk, fat_fs->bs.fat_start + i * SECTORS_PER_CLUSTER + j,
			           		buffer + bytes_read);
				bytes_read += DISK_SECTOR_SIZE;
			}
		} else {
			uint8_t *bounce = malloc (DISK_CLUSTER_SIZE);
			if (bounce == NULL)
				PANIC ("FAT load failed");
			for(size_t j = 0; j < SECTORS_PER_CLUSTER; j++)
				disk_read(filesys_disk, fat_fs->bs.fat_start + i * SECTORS_PER_CLUSTER + j,
			           		bounce + j * DISK_SECTOR_SIZE);
			memcpy (buffer + bytes_read, bounce, bytes_left);
			bytes_read += bytes_left;
			free (bounce);
		}
	}
}

void
fat_close (void) {
	// Write FAT boot sector
	uint8_t *bounce = calloc (1, DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT close failed");
	memcpy (bounce, &fat_fs->bs, sizeof (fat_fs->bs));
	disk_write (filesys_disk, FAT_BOOT_SECTOR, bounce);
	free (bounce);

	// Write FAT directly to the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_wrote = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_clusters; i++) {
		bytes_left = fat_size_in_bytes - bytes_wrote;
		if (bytes_left >= DISK_CLUSTER_SIZE) {
			for(size_t j = 0; j < SECTORS_PER_CLUSTER; j++){
				disk_write(filesys_disk, fat_fs->bs.fat_start + i * SECTORS_PER_CLUSTER + j,
							buffer + bytes_wrote);
				bytes_wrote += DISK_SECTOR_SIZE;
			}
		} else {
			bounce = calloc (1, DISK_CLUSTER_SIZE);
			if (bounce == NULL)
				PANIC ("FAT close failed");
			memcpy (bounce, buffer + bytes_wrote, bytes_left);
			for(size_t j = 0; j < SECTORS_PER_CLUSTER; j++)
				disk_write(filesys_disk, fat_fs->bs.fat_start + i * SECTORS_PER_CLUSTER + j,
			           		bounce + j * DISK_SECTOR_SIZE);
			bytes_wrote += bytes_left;
			free (bounce);
		}
	}
}

void
fat_create (void) {
	// Create FAT boot
	fat_boot_create ();
	fat_fs_init ();

	// Create FAT table
	// Initialize FAT entry to EMPTY(0xFFFFFFFF)
	fat_fs->fat = malloc(fat_fs->fat_length * sizeof(cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT creation failed");
	memset(fat_fs->fat, 0xFF, fat_fs->fat_length * sizeof(cluster_t));

	// Set up ROOT_DIR_CLST
	fat_put (ROOT_DIR_CLUSTER, EOChain);

	// Fill up ROOT_DIR_CLUSTER region with 0
	uint8_t *buf = calloc (SECTORS_PER_CLUSTER, DISK_SECTOR_SIZE);
	if (buf == NULL)
		PANIC ("FAT create failed due to OOM");
	
	disk_write_clst(filesys_disk, ROOT_DIR_CLUSTER, buf);
	free (buf);
}

void
fat_boot_create (void) {
	unsigned int fat_clusters =
	    (disk_size (filesys_disk) - 1)
	    / (DISK_SECTOR_SIZE / sizeof (cluster_t) * SECTORS_PER_CLUSTER + 1) + 1;

	unsigned int fat_empty = 
		(disk_size(filesys_disk) - (1 + fat_clusters)) / SECTORS_PER_CLUSTER;

	fat_fs->bs = (struct fat_boot){
	    .magic = FAT_MAGIC,
	    .sectors_per_cluster = SECTORS_PER_CLUSTER,
	    .total_sectors = disk_size (filesys_disk),
	    .fat_start = 1,
	    .fat_clusters = fat_clusters,
		.fat_empty = fat_empty,
	    .root_dir_cluster = ROOT_DIR_CLUSTER,
	};
}

void
fat_fs_init (void) {
	fat_fs->data_start = 1 + fat_fs->bs.fat_clusters;
	fat_fs->fat_length = (disk_size(filesys_disk) - fat_fs->data_start) / SECTORS_PER_CLUSTER;
	fat_fs->last_clst = fat_fs->fat_length; //consider deleting.
	lock_init(&fat_fs->write_lock);
}

/*----------------------------------------------------------------------------*/
/* FAT handling                                                               */
/*----------------------------------------------------------------------------*/
/* More efficient implementation. 
   It points 0 to (fat_length - 1) as index of fat. */
static cluster_t clst_hand = 0;

/* Find empty fat entry and return its index. */
cluster_t
allocate_clst(void){
	cluster_t new_clst = EMPTY;

	if(fat_fs->bs.fat_empty == 0) 
		return EMPTY;

	while(1){
		ASSERT(clst_hand >= 0 && clst_hand < fat_fs->fat_length);

		if(fat_get(clst_hand) == EMPTY){
			new_clst = clst_hand;
			clst_hand = (clst_hand + 1) % fat_fs->fat_length;
			break;
		}

		clst_hand = (clst_hand + 1) % fat_fs->fat_length;
	}

	fat_fs->bs.fat_empty--;
	return new_clst;
}

/* Add a cluster to the chain.
 * If CLST is EMPTY, start a new chain.
 * Returns EMPTY if fails to allocate a new cluster. */
cluster_t
fat_create_chain (cluster_t clst) {
	ASSERT(clst == EMPTY || fat_get(clst) == EOChain);

	cluster_t new_clst = allocate_clst();
	if(new_clst == EMPTY){
		printf("No empty entry\n");
		return EMPTY;
	}

	ASSERT(new_clst >= 0 && new_clst < fat_fs->fat_length);
	lock_acquire(&fat_lock);

	if(clst != EMPTY)
		fat_put(clst, new_clst);
	fat_put(new_clst, EOChain);

	lock_release(&fat_lock);
	return new_clst;
}

/* Allocate and make a chain CLUSTERS clusters from fat and stores 
   the first into *START.
   If PCLST is EMPTY, start a new chain.
   Else link it to PCLST.
   Returns true if successful, false if empty fat entries are less
   than CLUSTERS. */
bool
fat_create_chain_multiple(size_t clusters, cluster_t *start, cluster_t pclst){
	/* If CLUSTERS == 0, return true. */
	if(clusters == 0){
		*start = EMPTY;
		return true;
	}

	/* If empty fat entries are less than CLUSTERS. */
	if(fat_fs->bs.fat_empty < clusters){
		printf("fat_empty : %d\nclusters : %d\n", fat_fs->bs.fat_empty, clusters);
		printf("fat_create_chain_multiple: no empty fat entry\n");
		return false;
	}

	/* Otherwise.(General case) */
	cluster_t clst = pclst;
	for(size_t i = 0; i < clusters; i++){
		clst = fat_create_chain(clst);
		ASSERT(clst != EMPTY);

		/* store the first into *START. */
		if(i == 0)
			*start = clst;
	}

	return true;
}


/* Remove the chain of clusters starting from CLST.
 * If PCLST is EMPTY, assume CLST as the start of the chain. */
void
fat_remove_chain (cluster_t clst, cluster_t pclst) {
	ASSERT(clst < fat_fs->fat_length && clst >= 0);
	ASSERT(pclst == EMPTY || fat_get(pclst) == clst);

	lock_acquire(&fat_lock);

	cluster_t nclst = clst;
	while(nclst != EOChain){
		nclst = fat_get(clst);
		ASSERT(nclst != EMPTY);

		fat_put(clst, EMPTY);
		clst = nclst;

		fat_fs->bs.fat_empty++;
	}

	if(pclst != EMPTY) 
		fat_put(pclst, EOChain);

	lock_release(&fat_lock);
}

/* Update a value in the FAT table. */
void
fat_put (cluster_t clst, cluster_t val) {
	ASSERT(clst < fat_fs->fat_length && clst >= 0);
	ASSERT(val == EOChain || val == EMPTY || (val < fat_fs->fat_length && val >= 0));

	fat_fs->fat[clst] = val;
}

/* Fetch a value in the FAT table. */
cluster_t
fat_get (cluster_t clst) {
	ASSERT(clst < fat_fs->fat_length && clst >= 0);
	
	return fat_fs->fat[clst];
}

/* Covert a cluster # to a sector number. */
disk_sector_t
cluster_to_sector (cluster_t clst) {
	return SECTORS_PER_CLUSTER * clst + fat_fs->data_start;
}
