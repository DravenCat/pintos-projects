#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/block.h"
#include "filesys/off_t.h"

/* Maximum length of a file name component.
   This is the traditional UNIX maximum length.
   After directories are implemented, this maximum length may be
   retained, but much longer full path names must be allowed. */
#define NAME_MAX 14
#define SLASH_STR "/"
#define CURRENT_DIR "."
#define PARENT_DIR ".."
#define SLASH '/'
#define DOT '.'

struct inode;

/* Opening and closing directories. */
bool dir_create (block_sector_t sector, size_t entry_cnt);
struct dir *dir_open (struct inode *);
struct dir *dir_open_root (void);
struct dir *dir_reopen (struct dir *);
void dir_close (struct dir *);
struct inode *dir_get_inode (struct dir *);
off_t dir_get_pos(struct dir *);
struct dir* dir_get_wd (const char*);
struct dir *dir_open_with_pos (struct inode *, off_t);
struct dir *dir_open_with_fullpath(const char *);
void dir_path_terminate(char *, int);
char *get_copy(const char* org_path);
bool dir_check_parent(const char *);
bool dir_isopened(struct dir*);

/* Reading and writing. */
bool dir_lookup (const struct dir *, const char *name, struct inode **);
bool dir_add (struct dir *, const char *name, block_sector_t);
bool dir_remove (struct dir *, const char *name);
bool dir_readdir (struct dir *, char name[NAME_MAX + 1]);

#endif /* filesys/directory.h */
