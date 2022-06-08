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

/* Just for debugging. */
static void
print_parsed(char **parsed, int idx){
	ASSERT(parsed != NULL);
	ASSERT(idx >= 0);

	for(int i = 0; i < idx; i++)
		printf("parsed[%d] : %s\n", i, parsed[i]);
}


static void
free_parsed(char **parsed, int idx){
	ASSERT(parsed != NULL);
	ASSERT(idx >= 0);

	for(int i = 0; i < idx; i++)
		free(parsed[i]);
	free(parsed);
}

static char **
parsing_path(const char *path, int *argc_){
	// /* Validation check. */
	// if(path[strlen(path) - 1] == '/'){
	// 	free(path);
	// 	return NULL;
	// }
		
	/* Count nubmer of tokens. */
	char *c = (char *)path;
	int tokens = (*c == '/') ? 0 : 1;
	while(*c != '\0'){
		if(*c == '/') tokens++;
		c++;
	}
	*argc_ = tokens;
	ASSERT(tokens > 0);

	/* Token length validation check efficiently. */
	if(strlen(path)/tokens > 14){
		//free(path);
		return NULL;
	}

	/* Allocation. */
	char **result = (char **)malloc(tokens * sizeof(char *));
	if(result == NULL){
		//free(path);
		return NULL;
	}
	for(int i = 0; i < tokens; i++){
		result[i] = (char *)malloc(sizeof(char) * 15);
		if(result[i] == NULL){
			free_parsed(result, i);
			//free(path);
			return NULL;
		}
	}	

	/* Parsing PATH. */
	int argc = 0 ;
    char *token;
    char *save_ptr;
    for (token = strtok_r (path, "/", &save_ptr); token != NULL;
    	 token = strtok_r (NULL, "/", &save_ptr)){
		
		/* Token length validation check specifically. */
		if(strlen(token) > 14){
			free_parsed(result, tokens);
			//free(path);
			return NULL;
		}
		strlcpy(result[argc++], token, strlen(token) + 1);
	}
	ASSERT(tokens == argc);
	//free(path);
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
	int argc = 0;
	char **parsed = parsing_path(path, &argc);
	if(parsed == NULL || argc == 0){
		dir_close(curr_dir);
		return NULL;
	}

	/* Access proper directory. */
	struct inode *inode_dir;
	for(int i = 0; i < argc - 1 + (int)to_end; i++){
		if(!dir_lookup(curr_dir, parsed[i], &inode_dir))
			goto err;

		dir_close(curr_dir);
		curr_dir = dir_open(inode_dir);
		if(curr_dir == NULL)
			goto err;
	}
	
	/* Store lowest directory name(last token). */
	if(!to_end){
		*lowest = (char *)malloc(strlen(parsed[argc - 1]) + 1);
		if(*lowest == NULL)
			goto err;
			
		strlcpy(*lowest, parsed[argc - 1], strlen(parsed[argc - 1]) + 1);
	}

	free_parsed(parsed, argc);
	return curr_dir;

err:
	free_parsed(parsed, argc);
	dir_close(curr_dir);
	return NULL;
}

/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size) {
	/* Access given PATH and store final file name in LOWEST. */
	bool success = false;
	char *lowest = NULL;
	struct dir *upper_dir = accessing_path(path, &lowest, false);
	if(lowest == NULL)
		goto done;
	
	/* Create file. */
	cluster_t inode_cluster = EMPTY;
	success = (upper_dir != NULL
			&& fat_create_chain_multiple(1, &inode_cluster, EMPTY)
			&& inode_create (inode_cluster, initial_size, false)
			&& dir_add (upper_dir, lowest, inode_cluster));
	if (!success && inode_cluster != EMPTY)
		fat_remove_chain(inode_cluster, EMPTY);

done:
	dir_close (upper_dir);
	free(lowest);
	//PANIC("success : %d", success);
	return success;
}

bool
filesys_create_dir (const char *path) {
	/* Access given PATH and store final file name in LOWEST. */
	char *lowest = NULL;
	struct dir *upper_dir = accessing_path(path, &lowest, false);
	
	if(upper_dir == NULL){
		free(lowest);
		return false;	
	}

	/* Create file. */
	cluster_t inode_cluster = EMPTY;
	cluster_t upper_cluster = inode_get_inumber(dir_get_inode(upper_dir));
	bool success = (fat_create_chain_multiple(1, &inode_cluster, EMPTY)
					&& dir_create (inode_cluster, upper_cluster, 16)
					&& dir_add (upper_dir, lowest, inode_cluster));
	if (!success && inode_cluster != EMPTY)
		fat_remove_chain(inode_cluster, EMPTY);
	
	dir_close (upper_dir);
	free(lowest);
	return success;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name) {
	struct dir *dir = dir_open_root ();
	struct inode *inode = NULL;

	if (dir != NULL)
		dir_lookup (dir, name, &inode);
	dir_close (dir);

	return file_open (inode);
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) {
	struct dir *dir = dir_open_root ();
	bool success = dir != NULL && dir_remove (dir, name);
	dir_close (dir);

	return success;
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
