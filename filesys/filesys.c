#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"
#include "filesys/fat.h"
#include "threads/thread.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	thread_current()->curr_dir = dir_open_root();

	fat_open ();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	dir_close(thread_current()->curr_dir);
	fat_close ();
#else
	free_map_close ();
#endif
}

char **
parsing_path(const char *path, int *argc_){
	/* Count nubmer of tokens. */
	char *c = (char *)path;
	int tokens = (*c == '/') ? 0 : 1;
	while(*c != '\0'){
		if(*c == '/') tokens++;
		c++;
	}

	/* Token length validation check. */
	if(strlen(path)/tokens > 14)
		return NULL;

	/* Parsing PATH. */
	int argc = 0 ;
    char *token;
    char *save_ptr;
	char **result = (char **)malloc(tokens * sizeof(char *));
	if(result == NULL) 
		return NULL;

    for (token = strtok_r ((char *)path, "/", &save_ptr); token != NULL;
    	 token = strtok_r (NULL, "/", &save_ptr)){
			if(strlen(token) > 14)
				return result;
			result[argc++] = token;
	}
	ASSERT(tokens == argc);

	*argc_ = tokens;
	return result;
}

/* Check upper directories exist and access.
   If TO_END is true, access to end of path. Otherwise access to upper dir.
   If TO_END is false, store lowest directory or file name in LOWEST. 
   Return right upper directory struct if it exist, otherwise NULL. */
struct dir *
accessing_path(const char *path, char **lowest, bool to_end){
	ASSERT(path != NULL);
	ASSERT(!((lowest == NULL) ^ to_end));

	if(*path == '\0')
		return NULL;

	/* Set current directory. */
	struct dir *curr_dir;
	curr_dir = (*path == '/') ? dir_open_root() : dir_reopen(thread_current()->curr_dir);
	if(curr_dir == NULL)
		return NULL;

	/* Parsing path */
	int argc;
	char **parsed = parsing_path(path, &argc);
	if(parsed == NULL) goto err;

	/* Access proper directory. */
	int i;
	struct inode *inode_dir;
	for(i = 0; i < argc - 1 + (int)to_end; i++){
		if(!dir_lookup(curr_dir, parsed[i], &inode_dir))
			goto err;

		dir_close(curr_dir);
		curr_dir = dir_open(inode_dir);
		if(curr_dir == NULL)
			goto err;
	}

	/* Store lowest directory name. */
	if(!to_end){
		*lowest = (char *)malloc(strlen(parsed[i]) + 1);
		if(*lowest == NULL)
			goto err;
		strlcpy(*lowest, parsed[i], strlen(parsed[i]) + 1);
	}

	free(parsed);
	return curr_dir;

err:
	free(parsed);
	dir_close(curr_dir);
	return NULL;
}

/* Creates a file named LOWEST in given PATH with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create_file(const char *path, off_t initial_size) {
	/* Access given PATH and store final file name in LOWEST. */
	char *lowest;
	struct dir *upper_dir = accessing_path(path, &lowest, false);
	if(upper_dir == NULL)
		return false;

	/* Create file. */
	cluster_t inode_cluster = EMPTY;
	bool success = (upper_dir != NULL
			&& fat_create_chain_multiple(1, &inode_cluster, EMPTY)
			&& inode_create (inode_cluster, initial_size, false)
			&& dir_add (upper_dir, lowest, inode_cluster));
	if (!success && inode_cluster != EMPTY){
		printf("filesys_create_file() fail\n");
		fat_remove_chain(inode_cluster, EMPTY);
	}

	dir_close (upper_dir);
	free(lowest);
	return success;
}

bool
filesys_create_dir(const char *path) {
	/* Access given PATH and store final file name in LOWEST. */
	char *lowest;
	struct dir *upper_dir = accessing_path(path, &lowest, false);
	if(upper_dir == NULL)
		return false;

	/* Create file. */
	cluster_t inode_cluster = EMPTY;
	cluster_t upper_cluster = inode_get_inumber(dir_get_inode(upper_dir));
	bool success = (upper_dir != NULL
			&& fat_create_chain_multiple(1, &inode_cluster, EMPTY)
			&& dir_create (inode_cluster, upper_cluster, 16)
			&& dir_add (upper_dir, lowest, inode_cluster));
	if (!success && inode_cluster != EMPTY){
		printf("filesys_create_file() fail\n");
		fat_remove_chain(inode_cluster, EMPTY);
	}

	dir_close (upper_dir);
	free(lowest);
	return success;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct item *
filesys_open(const char *path){
	/* Access proper path and make INODE. */
	char *lowest = NULL;
	struct item *item = NULL;
	struct inode *inode = NULL;
	
	struct dir *upper_dir = accessing_path(path, &lowest, false);
	if(upper_dir == NULL)
		goto err;
	
	dir_lookup (upper_dir, lowest, &inode);
	dir_close (upper_dir);
		
	if(inode == NULL)
		goto err;

	/* Determine lowest item in path, directory or file. */
	item = (struct item *)malloc(sizeof(struct item));
	if(item == NULL)
		goto err;

	if(inode_is_dir(inode)){
		item->is_dir = true;
		item->dir = dir_open(inode);
		if(item->dir == NULL)
			goto err;
	}
	else{
		item->is_dir = false;
		item->file = file_open(inode);
		if(item->file == NULL)
			goto err;
	}

	free(lowest);
	return item;

err:
	free(lowest);
	free(item);
	printf("filesys_open fail\n");
	return NULL;
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) {
	char *lowest;
	struct dir *upper_dir = accessing_path(path, &lowest, false);
	if(upper_dir == NULL)
		return false;

	dir_remove(upper_dir, lowest);
	dir_close(upper_dir);

	return true;
}

void
item_close(struct item *item){
	if(item != NULL)
		item->is_dir ? dir_close(item->dir) : file_close(item->file);
	
	free(item);
}

struct item *
item_duplicate(struct item *item){
	ASSERT(item != NULL);

	struct item *nitem = (struct item *)malloc(sizeof(struct item));
	if(nitem == NULL) return NULL;

	nitem->is_dir = item->is_dir;

	if(item->is_dir)
		nitem->dir = dir_duplicate(item->dir);
	else
		nitem->file = file_duplicate(item->file);

	return nitem;
}

/* Formats the file system. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	if (!dir_create(ROOT_DIR_CLUSTER, ROOT_DIR_CLUSTER, 16))
		PANIC ("root directory creation failed");
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}
