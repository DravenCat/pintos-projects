#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  // initialize the buffer cache
  init_cache();

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  // write back from the buffer cache to disk
  write_back_cache();

  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) // name = "/a/b"   name = "b"
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_get_wd(name);
  char *filename = filesys_get_target(name);

  // cannot create a file with invalid name
  if (filename != NULL)
  {
    if (filename[0] == DOT || strcmp(filename, SLASH_STR) == 0)
      return false;
  }

  // creating  a file
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, filename, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  free(filename);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  if (strlen(name) <= 0 ) return NULL;

  struct dir *dir = dir_get_wd(name);
  char *filename = filesys_get_target(name);
  struct dir *root = dir_open_root();
  struct inode *inode = NULL;

  if (strcmp(filename, PARENT_DIR) == 0)
    return false;

  if (dir != NULL)
  { 
    // open the current directory
    if (strcmp(filename, CURRENT_DIR) == 0)
    {
      dir_close(root);
      return (struct file *) dir;
    }
    // open the root
    if (strlen(filename) == 0 && inode_get_inumber(dir_get_inode(dir)) == inode_get_inumber(dir_get_inode(root)))
    {
      dir_close(dir);
      return (struct file *) root;
    }
    //normal open
    dir_lookup (dir, filename, &inode);
  }
  dir_close(root);
  dir_close (dir);
  free(filename);

  // open the directory/file
  if (inode)
  {
    if (inode_isdir(inode))
    {
      return (struct file *) dir_open(inode);  
    }
    return file_open (inode);
  }
  
  return NULL;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  // cannot remove root directory
  if (strcmp(name, SLASH_STR) == 0) return false;
  if (name[0] == SLASH)
    if (dir_check_parent(name))
      return false;
  struct dir *dir = dir_get_wd(name);
  char *filename = filesys_get_target(name);
  const char* pwd = thread_get_pwd();
  struct dir *cwd = thread_get_cwd();
  // cannot remove cwd
  if (cwd != NULL) 
    if (inode_get_inumber(dir_get_inode(dir)) == inode_get_inumber(dir_get_inode(cwd)))
      return false;

  struct inode* inode = NULL;
  dir_lookup (dir, filename, &inode);

  // cannot remove a subdirectory that has been opened under root
  if (pwd != NULL)
  {
    if (pwd[0] == SLASH && inode_isdir(inode) && strcmp(pwd, SLASH_STR) != 0)
    {
      if (inode_isopened(inode))
      {
        dir_close(dir);
        return false;
      }
    }
  }

  // remove the directory/file
  bool success = dir != NULL && dir_remove (dir, filename);
  dir_close (dir); 
  free(filename);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

bool
filesys_create_dir(const char *name, off_t initial_size) 
{ 
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_get_wd(name);
  char *filename = filesys_get_target(name);
  
  // cannot create a directory with invalid name
  if (filename != NULL)
  {
    if (filename[0] == DOT || strcmp(filename, SLASH_STR) == 0)
      return false;
  }

  // create the directory
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create_by_type (inode_sector, initial_size, 1)
                  && dir_add(dir, filename, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  return success;
}

// Get copy of name of last one (a directory or a file) in the path 
char* 
filesys_get_target (const char* path)
{
  char* path_copy = get_copy(path);

  char *token, *save_ptr, *last_token = "";
  for (token = strtok_r(path_copy, SLASH_STR, &save_ptr); token != NULL; token = strtok_r (NULL, SLASH_STR, &save_ptr))
  {
    last_token = token;
  }
  char *target = get_copy(last_token);
  free(path_copy);
  return target;
}