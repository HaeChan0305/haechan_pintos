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

static char*
get_sym_path(struct inode *inode){
	ASSERT(inode != NULL);
	ASSERT(inode_is_sym(inode));
	ASSERT(!inode_is_dir(inode));

	off_t sym_len = inode_length(inode); 
	char *sym_path = (char *)malloc(sizeof(char) * (sym_len + 1));
	if(sym_path == NULL)
		return NULL;
	
	if(inode_read_at(inode, sym_path, sym_len, 0) != sym_len)
		PANIC("sym_path copy fail");

	return sym_path;
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
parsing_path(const char *path_, int *argc_){
	ASSERT(path_ != NULL && argc_ != NULL);
	if(*path_ == '/')
		PANIC("path : %s\n", path_);
	//ASSERT(*path_ != '/');

	/* Copy to protect origin string. */
	char *path = (char *) malloc (sizeof(char) * (strlen(path_) + 1));
    if(path== NULL)
        return NULL;
    strlcpy(path, path_, strlen(path_) + 1);

	/* Count nubmer of tokens. */
	char *c = (char *)path;
	int tokens = 1;
	while(*c != '\0'){
		if(*c == '/') tokens++;
		c++;
	}
	*argc_ = tokens;
	ASSERT(tokens > 0);

	/* Token length validation check efficiently. */
	if(strlen(path)/tokens > 14){
		free(path);
		return NULL;
	}

	/* Allocation. */
	char **result = (char **)malloc(tokens * sizeof(char *));
	if(result == NULL){
		free(path);
		return NULL;
	}
	
	/* Parsing PATH. */
	int argc = 0 ;
    char *token;
    char *save_ptr;
    for (token = strtok_r (path, "/", &save_ptr); token != NULL;
    	 token = strtok_r (NULL, "/", &save_ptr)){
		
		/* Token length validation check specifically. */
		if(strlen(token) > 14){
			free_parsed(result, argc);
			free(path);
			return NULL;
		}
		else{
			result[argc] = (char *)malloc(sizeof(char) * (strlen(token) + 1));
			if(result[argc] == NULL){
				free_parsed(result, argc);
				free(path);
				return NULL;
			}
			strlcpy(result[argc++], token, strlen(token) + 1);
		}
	}
	ASSERT(tokens == argc);
	free(path);
	return result;
}

/* Check upper directories exist and access.
   If TO_END is true, access to end of path. Otherwise access to upper dir.
   If TO_END is false, store lowest directory or file name in LOWEST. 
   Return right upper directory struct if it exist, otherwise NULL. */
struct dir *
accessing_path(const char *path, char **lowest, bool to_end, bool sym){
	ASSERT(path != NULL);
	ASSERT(!((lowest == NULL) ^ to_end));

	if(*path == '\0')
		return NULL;

	/* Special case. */
	if(strlen(path) == 1 && *path == '/')
		return dir_open_root();

	/* Set current directory. */
	struct dir *curr_dir;
	if(*path == '/'){
		curr_dir = dir_open_root();
		path++;
		if(*path == '/')
			path++;
	}
	else{
		curr_dir = dir_reopen(thread_current()->curr_dir);
	}

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
	int i;
	struct inode *inode_dir;
	for(i = 0; i < argc - 1; i++){
		if(!dir_lookup(curr_dir, parsed[i], &inode_dir))
			goto err;

		dir_close(curr_dir);
		if(inode_is_dir(inode_dir)){
			curr_dir = dir_open(inode_dir);
			if(curr_dir == NULL)
				goto err;
		}
		else{
			if(inode_is_sym(inode_dir)){
				char *sym_path = get_sym_path(inode_dir);
				if(sym_path == NULL)
					goto err;

				curr_dir = accessing_path(sym_path, NULL, true, true);
				free(sym_path);
				if(curr_dir == NULL)
					goto err;
			}
			else{
				inode_close(inode_dir);
				goto err;
			}
		}
	}

	/* If lowest path is symlink. */
	if(sym){
		if(!dir_lookup (curr_dir, parsed[i], &inode_dir))
			goto err;

		if(inode_is_sym(inode_dir)){
			dir_close(curr_dir);
			char *sym_path = get_sym_path(inode_dir);
			if(sym_path == NULL)
				goto err;

			curr_dir = accessing_path(sym_path, lowest, to_end, sym);
			free(sym_path);
			if(curr_dir == NULL || lowest == NULL)
				goto err;

			inode_close(inode_dir);
			free_parsed(parsed, argc);
			return curr_dir;
		}

		inode_close(inode_dir);
	}

	/* Store lowest directory name(last token). */
	if(!to_end){
		*lowest = (char *)malloc(strlen(parsed[i]) + 1);
		if(*lowest == NULL)
			goto err;
	
		strlcpy(*lowest, parsed[i], strlen(parsed[i]) + 1);
	}
	else{
		if(!dir_lookup(curr_dir, parsed[i], &inode_dir))
			goto err;

		dir_close(curr_dir);
		if(inode_is_dir(inode_dir)){
			curr_dir = dir_open(inode_dir);
			if(curr_dir == NULL)
				goto err;
		}
		else{
			inode_close(inode_dir);
			goto err;
		}
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
	ASSERT(path != NULL);

	/* Access given PATH and store final file name in LOWEST. */
	bool success = false;
	char *lowest = NULL;
	struct dir *upper_dir = accessing_path(path, &lowest, false, false);
	if(lowest == NULL)
		goto done;

	/* Create file. */
	cluster_t inode_cluster = EMPTY;
	success = (upper_dir != NULL
			&& fat_create_chain_multiple(1, &inode_cluster, EMPTY)
			&& inode_create (inode_cluster, initial_size, false, false)
			&& dir_add (upper_dir, lowest, inode_cluster));
	if (!success && inode_cluster != EMPTY)
		fat_remove_chain(inode_cluster, EMPTY);

done:
	dir_close (upper_dir);
	free(lowest);
	return success;
}

bool
filesys_create_dir (const char *path) {
	ASSERT(path != NULL);

	/* Access given PATH and store final file name in LOWEST. */
	bool success = false;
	char *lowest = NULL;
	struct dir *upper_dir = accessing_path(path, &lowest, false, false);
	
	if(upper_dir == NULL || lowest == NULL)
		goto done;

	/* Create file. */
	cluster_t inode_cluster = EMPTY;
	cluster_t upper_cluster = inode_get_inumber(dir_get_inode(upper_dir));
	success = (fat_create_chain_multiple(1, &inode_cluster, EMPTY)
			&& dir_create (inode_cluster, upper_cluster, 16)
			&& dir_add (upper_dir, lowest, inode_cluster));
	if (!success && inode_cluster != EMPTY)
		fat_remove_chain(inode_cluster, EMPTY);
	
done:
	dir_close (upper_dir);
	free(lowest);
	return success;
}

int
filesys_symlink_create (const char *target, const char *link_path) {
	ASSERT(target != NULL);
	ASSERT(link_path != NULL);

	/* Access given PATH and store final file name in LOWEST. */
	bool success = false;
	char *lowest = NULL;
	struct dir *upper_dir = accessing_path(link_path, &lowest, false, false);
	if(lowest == NULL)
		goto done;
	
	/* Create file. */
	cluster_t inode_cluster = EMPTY;
	success = (upper_dir != NULL
			&& fat_create_chain_multiple(1, &inode_cluster, EMPTY)
			&& inode_create (inode_cluster, strlen(target) + 1, false, true)
			&& dir_add (upper_dir, lowest, inode_cluster));
	if (!success && inode_cluster != EMPTY)
		fat_remove_chain(inode_cluster, EMPTY);

done:
	dir_close (upper_dir);
	free(lowest);
	if(success){
		struct inode *sym_inode = inode_open(inode_cluster);
		if(inode_write_at(sym_inode, target, strlen(target) + 1, 0) != strlen(target) + 1)
			PANIC("symlink_create: inode_write_at() fail");
		inode_close(sym_inode);
	}
	return success ? 0 : -1;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *path) {
	ASSERT(path != NULL);
	struct file *file = NULL;
	struct item *item = filesys_open_item(path);
	if(item == NULL)
		return NULL;

	/* This function have to deal with only file. */
	if(!item->is_dir)
		file = item->file;
	else
		dir_close(item->dir);

	free(item);
	return file;
}
/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct item *
filesys_open_item(const char *path){
	ASSERT(path != NULL);

	/* Special case : path = "/". */
	if(strlen(path) == 1 && *path == '/'){
		struct item *item = (struct item *)malloc(sizeof(struct item));
		if(item == NULL)
			return NULL;

		item->is_dir = true;
		item->dir = dir_open_root();
		if(item->dir == NULL)
			return NULL;
		
		return item;
	}

	/* Access proper path and make INODE. */
	char *lowest = NULL;
	struct item *item = NULL;
	struct inode *inode = NULL;

	struct dir *upper_dir = accessing_path(path, &lowest, false, true);
	if(upper_dir == NULL || lowest == NULL)
		goto err;
	
	dir_lookup (upper_dir, lowest, &inode);
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

	dir_close(upper_dir);
	free(lowest);
	return item;

err:
	dir_close(upper_dir);
	free(lowest);
	free(item);
	//printf("filesys_open fail\n");
	return NULL;
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) {
	ASSERT(path != NULL);

	/* Special case : path = "/". */
	if(strlen(path) == 1 && *path == '/')
		return false;

	/* Access given PATH and store final file name in LOWEST. */
	bool success = false;
	char *lowest = NULL;
	
	struct dir *upper_dir = accessing_path(path, &lowest, false, false);
	if(upper_dir == NULL || lowest == NULL)
		goto done;
	
	//printf("lowsest : %s\n", lowest);
	success = dir_remove (upper_dir, lowest);
	
done:
	dir_close (upper_dir);
	free(lowest);
	return success;
}

void
item_close(struct item *item){
	if(item != NULL){
		if(item->is_dir){
			ASSERT(item->dir != NULL);
			dir_close(item->dir);
		}
		else{
			ASSERT(item->file != NULL);
			file_close(item->file);
		}

		free(item);
	}
}

struct item *
item_duplicate(struct item *item){
	ASSERT(item != NULL);

	struct item *nitem = (struct item *)malloc(sizeof(struct item));
	if(nitem == NULL) return NULL;

	nitem->is_dir = item->is_dir;

	if(item->is_dir){
		ASSERT(item->dir != NULL);
		nitem->dir = dir_duplicate(item->dir);
	}
	else{
		ASSERT(item->file != NULL);
		nitem->file = file_duplicate(item->file);
	}

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
