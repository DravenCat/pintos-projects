#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "./process.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

static void syscall_handler (struct intr_frame *);
bool check_vaddr_validity(const void *vaddr);
static int get_user (const uint8_t *uaddr);
struct thread_file * get_file_by_fd (int file_id);
static void thread_exit_with_status(int status);
static void (*system_calls[20])(struct intr_frame *);
void sys_halt(struct intr_frame* f);
void sys_exit(struct intr_frame* f);
void sys_exec(struct intr_frame* f);
void sys_create(struct intr_frame* f);
void sys_remove(struct intr_frame* f);
void sys_open(struct intr_frame* f);
void sys_wait(struct intr_frame* f);
void sys_filesize(struct intr_frame* f);
void sys_read(struct intr_frame* f);
void sys_write(struct intr_frame* f);
void sys_seek(struct intr_frame* f);
void sys_tell(struct intr_frame* f);
void sys_close(struct intr_frame* f);
void sys_chdir(struct intr_frame* f);
void sys_mkdir(struct intr_frame* f);
void sys_readdir(struct intr_frame* f);
void sys_isdir(struct intr_frame* f);
void sys_inumber(struct intr_frame* f);

static char * get_full_path(const char *);
static bool path_go_back(char *path);

static void thread_exit_with_status(int status)
{
  struct thread *cur = thread_current();
  cur->return_value = status;
  thread_exit();
}

void sys_halt(struct intr_frame* f UNUSED)
{
  //call the function in devices/shutdown.h to shut down the pintos system
  shutdown_power_off();
}

void sys_exit(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  //check if the address is valid
  if(!check_vaddr_validity (p + 1)) {
    thread_exit_with_status(-1);
  }

  struct thread * cur = thread_current();
  struct list_elem *e =list_begin (&cur->files);

  while (e != list_end (&cur->files))
  {
    struct thread_file *thread_file = list_entry (e, struct thread_file, file_elem);
    struct inode* inode = file_get_inode(thread_file->file);
    if(inode == NULL)
        continue;
    if(inode_isdir(inode))
      dir_close((struct dir *)thread_file->file);
    else
      file_close (thread_file->file);
    e = list_next (e);
    free(thread_file);
  }

  // thread returns with the value
  thread_exit_with_status(*(p + 1));
}

void sys_exec(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  //check if the p+1 address and the address of p+1 points to are valid
  if(!check_vaddr_validity (p + 1) || !check_vaddr_validity ((const char *)*(p + 1))) {
    thread_exit_with_status(-1);
  }

  // call process_execute and store its return value
  f->eax = process_execute((char*)* (p + 1));
}

void sys_create(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  if(!check_vaddr_validity (p + 1) || !check_vaddr_validity (p + 2) || !check_vaddr_validity ((const char *)*(p + 1))) {
    thread_exit_with_status(-1);
  }

  // check if the name is NULL
  if((const char *)*(p + 1) == NULL) {
    thread_exit_with_status(-1);
  }

  acquire_filesys_lock();
  // create the file and store the return value
  f->eax = filesys_create ((const char *)*(p + 1), *(p + 2));
  release_filesys_lock();
}

void sys_remove(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  if(!check_vaddr_validity (p + 1) || !check_vaddr_validity ((const char *)*(p + 1))) {
    thread_exit_with_status(-1);
  }

  char *name = (char *)*(p + 1);

  // check if the name is NULL
  if(name == NULL) {
    thread_exit_with_status(-1);
  }
  char *fullname = get_full_path(name);
  acquire_filesys_lock();
  // create the file and store the return value
  f->eax = filesys_remove (fullname);
  release_filesys_lock();
}

void sys_open(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  //check if the pointers that contain the file is valid
  if(!check_vaddr_validity (p + 1) || !check_vaddr_validity ((const char *)*(p + 1))) {
    thread_exit_with_status(-1);
  }

  char *name  = (char *)*(p + 1);
  // check if the name is NULL
  if(name == NULL) {
    thread_exit_with_status(-1);
  }
  acquire_filesys_lock();
  //open the file using filesys_open
  struct file * file_opened = filesys_open(name);
  release_filesys_lock();
  struct thread * t = thread_current();

  //the file could be opened
  if (file_opened)
  {
    //allocate space for a temporary file
    struct thread_file *temp_file = malloc(sizeof(struct thread_file));

    //increment the file descriptor to ensure it is unique
    temp_file->fd = t->fd_accum++;
    temp_file->file = file_opened;

    //insert the file into the thread's list files
    list_push_front (&t->files, &temp_file->file_elem);

    //store the return value as the file fd
    f->eax = temp_file->fd;
  } 
  // the file could not be opened
  else
  {
    //return -1
    f->eax = -1;
  }
}

void sys_wait(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  // check if the address is valid
  if(!check_vaddr_validity (p + 1)) {
    thread_exit_with_status(-1);
  }

  // call process_wait and store its return value
  f->eax = process_wait(*(p + 1));
}

void sys_filesize(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  if(!check_vaddr_validity (p + 1)) {
    thread_exit_with_status(-1);
  }

  // get the file
  struct thread_file * file = get_file_by_fd (*(p + 1));

  //got the file
  if (file != NULL) {
    acquire_filesys_lock();
    // get the file size and store the return value
    f->eax = file_length (file->file);
    release_filesys_lock();
  } 
  //got no file
  else {
    //return -1
    f->eax = -1;
  }
}

void sys_read(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  // initialize variables to store file descriptor, buffer and size
  int fd = *(p + 1);
  char * buffer;
  off_t size = *(p + 3);

  // check if the pointers that contain file descriptor, buffer and size are all valid
  if (!check_vaddr_validity (p + 2) || !check_vaddr_validity (p + 3) || !check_vaddr_validity ((const char *)*(p + 2))) {
    thread_exit_with_status(-1);
  }

  // check if the buffer is NULL
  if((const char *)*(p + 2) == NULL) {
    thread_exit_with_status(-1);
  }
  
  buffer = (char *)*(p + 2);
    struct thread_file * temp_file = get_file_by_fd (fd);
    //got no file
    if (temp_file == NULL)
    {
      //return -1
      f->eax = -1;
    } 
    //got the file
    else
    {
      acquire_filesys_lock();
      // read the file into the buffer and store the bytes read
      f->eax = file_read(temp_file->file, buffer, size);
      release_filesys_lock();
    }
}

void sys_write(struct intr_frame* f)
{
  // initialize variables to store file descriptor, buffer and size
  int fd;
  const char * buffer;
  off_t size;

  uint32_t *p = f->esp;

  // check if the pointers that contain file descriptor, buffer and size are all valid
  if (!check_vaddr_validity (p + 1) || !check_vaddr_validity (p + 2) || !check_vaddr_validity (p + 3) || !check_vaddr_validity ((const char *)*(p + 2))) {
    thread_exit_with_status(-1);
  }

  // check if the buffer is NULL
  if((const char *)*(p + 2) == NULL) {
    thread_exit_with_status(-1);
  }

  // get file descriptor, buffer and size
  fd = *(p + 1);
  buffer = (const char *)*(p + 2);
  size = *(p + 3);
  // if the file descripter is equal to STDOUT_FILENO (1), then output to the console
  if (fd == STDOUT_FILENO) {
    f->eax = size;
    putbuf(buffer, size);
  } else {
    // find the file to write in the file list of the thread
    struct thread_file * thread_file= get_file_by_fd (fd);
    if (thread_file == NULL) {
      // cannot find the file corresponding to the id, return 0
      f->eax = 0;
      return;
    }
    else if (inode_isdir(file_get_inode(thread_file->file)))
    {
      f->eax = -1;
      return;
    }
    else {
      // only one thread can write to the file at the same time
      acquire_filesys_lock ();

      // write to the file and store the return value
      f->eax = file_write (thread_file->file, buffer, size);

      //release the lock
      release_filesys_lock ();
    }
  }
}

/* find the file by id that is stored in the current running thread */
struct thread_file * 
get_file_by_fd (int id)
{
  struct list *files = &thread_current ()->files;
  struct thread_file * thread_file = NULL;
  struct list_elem *elem = list_begin (files);

  // travese through the file list
  while(elem != list_end (files)) {
    // retrieve thread_file struct from the file list
    thread_file = list_entry (elem, struct thread_file, file_elem);

    // check if the file's fd is equal to id
    if (thread_file->fd == id)
      return thread_file;

    // move elem to the next element
    elem = list_next (elem);
  }

  // return NULL if no file matches
  return NULL;
}

void sys_seek(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  if(!check_vaddr_validity (p + 1)) {
    thread_exit_with_status(-1);
  }

  // get the file
  struct thread_file * file = get_file_by_fd (*(p + 1));

  if (file != NULL) {
    acquire_filesys_lock();
    // create the file and store the return value
    file_seek (file->file, *(p + 2));
    release_filesys_lock();
  }
}

void sys_tell(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  if(!check_vaddr_validity (p + 1)) {
    thread_exit_with_status(-1);
  }

  // get the file
  struct thread_file * file = get_file_by_fd (*(p + 1));

  if (file != NULL) {
    // call file tell and store the return value
    acquire_filesys_lock();
    f->eax = file_tell (file->file);
    release_filesys_lock();
  }
  else
  {
    f->eax = -1;
  }

}

void sys_close(struct intr_frame* f)
{
  uint32_t *p = f->esp;

  if(!check_vaddr_validity (p + 1)) {
    thread_exit_with_status(-1);
  }

  // get the file
  struct thread_file * file = get_file_by_fd (*(p + 1));

  if (file != NULL) {
    if (inode_isdir(file_get_inode(file->file))==1)
    {
      acquire_filesys_lock();
      // call file_close to close the file
      dir_close ((struct dir *)file->file);
      release_filesys_lock();
    }else {

      acquire_filesys_lock();
      // call file_close to close the file
      file_close (file->file);
      release_filesys_lock();
    }
    // remove the file from file list of the thread
    list_remove (&file->file_elem);
  }
}

void sys_chdir(struct intr_frame* f)
{
  if (!check_vaddr_validity(((int *) f->esp) + 2))
    thread_exit_with_status(-1);

  //get the full path
  char *cd_target = (char *)*((int *)f->esp + 1);
	char *full_path = get_full_path(cd_target);

  // open the directory
	struct dir *dir = dir_open_with_fullpath(cd_target);
	if(dir == NULL)
	{
		f->eax = 0;
		free(full_path);
	}
  else
  {
    // Set pwd and cwd to the new working directory
    thread_set_cwd(dir);
	  thread_set_pwd(full_path);
  	f->eax = 1;
  }
}

void sys_mkdir(struct intr_frame* f)
{
  if (!check_vaddr_validity(((int *) f->esp) + 2))
    thread_exit_with_status(-1);
  char *new_dir_name = (char *)*((int *) f->esp + 1);
  
  if (strcmp(new_dir_name, SLASH_STR) == 0)
    PANIC("Cannot make dir with name /\n");

	if(strlen(new_dir_name) <= 0)
  {
	  f->eax = 0;
  }
	else 
  {
    // create the dircectory
    acquire_filesys_lock();
    f->eax = filesys_create_dir(new_dir_name, 0);
    release_filesys_lock();
  }
}

void sys_readdir(struct intr_frame* f)
{
  if (!check_vaddr_validity(((int *) f->esp) + 2))
    thread_exit_with_status(-1);
  
	char *name = (char *)*((int *) f->esp + 2);
  if(name == NULL)
  {
    f->eax = -1;
    thread_exit_with_status(-1);
  }
  //check whether the directory is removed
  int fd = *((int *) f->esp + 1);
  struct thread_file *fp= get_file_by_fd(fd);
  struct inode *fp_inode = file_get_inode(fp->file);
	if(inode_isremoved(fp_inode))
	{
		f->eax=0;
		return ;
	}
  
  // open the directory
  acquire_filesys_lock();
  struct dir *target_dir = dir_open_with_pos(fp_inode, file_tell(fp->file));
  release_filesys_lock();

	if(dir_readdir(target_dir, name))
		f->eax = 1;
	else
		f->eax = 0;
	file_seek(fp->file, dir_get_pos(target_dir));
  dir_close(target_dir);
}

void sys_isdir(struct intr_frame* f)
{
  if (!check_vaddr_validity(((int *) f->esp) + 2))
    thread_exit_with_status(-1);
  int fd = *((int *) f->esp + 1);
	struct thread_file *fn= get_file_by_fd(fd);

  struct inode *fn_inode = file_get_inode(fn->file);
  if (fn_inode == NULL)
    thread_exit_with_status(-1);
  // check if it is a directory
	f->eax = inode_isdir(fn_inode);

}

void sys_inumber(struct intr_frame* f)
{
  if (!check_vaddr_validity(((int *) f->esp) + 2))
  {
    thread_exit_with_status(-1);
  }

  int fd = *((int *) f->esp + 1);
	struct thread_file *fn= get_file_by_fd(fd);

  struct inode *fn_inode = file_get_inode(fn->file);
  if (fn_inode == NULL)
  {
    thread_exit_with_status(-1);
  }
  // get the inumber 
	f->eax = inode_get_inumber(fn_inode);
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");

  system_calls[SYS_EXEC] = &sys_exec;
  system_calls[SYS_HALT] = &sys_halt;
  system_calls[SYS_EXIT] = &sys_exit;
  system_calls[SYS_WAIT] = &sys_wait;
  system_calls[SYS_CREATE] = &sys_create;
  system_calls[SYS_REMOVE] = &sys_remove;
  system_calls[SYS_OPEN] = &sys_open;
  system_calls[SYS_WRITE] = &sys_write;
  system_calls[SYS_SEEK] = &sys_seek;
  system_calls[SYS_TELL] = &sys_tell;
  system_calls[SYS_CLOSE] = &sys_close;
  system_calls[SYS_READ] = &sys_read;
  system_calls[SYS_FILESIZE] = &sys_filesize;
  system_calls[SYS_CHDIR] = &sys_chdir;
  system_calls[SYS_MKDIR] = &sys_mkdir;
  system_calls[SYS_READDIR] = &sys_readdir;
  system_calls[SYS_ISDIR] = &sys_isdir;
  system_calls[SYS_INUMBER] = &sys_inumber;
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  //check if f->esp is a valid address
  uint32_t * p = f->esp;
  if (!check_vaddr_validity (p)) {
    thread_exit_with_status(-1);
  }

  //get the system call number
  int syscall_number = * (int *)f->esp;

  //check if the system call corresponding to the system call number exists
  if(syscall_number < 0 || syscall_number >= 20){
    thread_exit_with_status(-1);
  }
  
  //make the system call
  system_calls[syscall_number](f);
}

/* Check if the vaddr is a valid address
   If not, return false. 
   Otherwise, return true*/
bool
check_vaddr_validity(const void *vaddr)
{ 
  // check if vaddr is a user virtual address
  if (!is_user_vaddr(vaddr))
    return false;

  // get the kernal virtual address
  void *ptr = pagedir_get_page (thread_current()->pagedir, vaddr);

  // check if the vaddr is unmapped
  if (!ptr)
    return false;

  // check if the bytes at vaddr are valid
  uint8_t *validate_byte = (uint8_t *) vaddr;
  if (get_user(validate_byte + 0) == -1 || 
      get_user(validate_byte + 1) == -1 ||
      get_user(validate_byte + 2) == -1 ||
      get_user(validate_byte + 3) == -1) {
    return false;
  }

  //otherwise return true if the vaddr is valid
  return true;
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int 
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:" : "=&a" (result) : "m" (*uaddr));
  return result;
}

static bool 
path_go_back(char *path) {
    if (strcmp(path, "/") == 0)
        return false;
    else {

        size_t n = strlen(path);
        path[n - 1] = '\0';
        n = strlen(path);
        while (path[n - 1] != '/')
            n--;
        if (n == 1) {
            path[n] = '\0';
        } else {
            path[n - 1] = '\0';
        }
    }
    return true;
}

static char * 
get_full_path(const char *cd_target)
{
    const char *pwd = thread_get_pwd();
    char *to;

    char *cd_copy = malloc(strlen(cd_target) + 1);
    memcpy(cd_copy, cd_target, strlen(cd_target) + 1);

    // absolute path
    if (*(cd_copy) == '/') {
        to = malloc(strlen(cd_target) + 5);
        memcpy(to, "/", strlen("/") + 1);

        cd_copy += 1;
        char *token, *save_ptr;
        for (token = strtok_r (cd_copy, "/", &save_ptr); token != NULL; token = strtok_r (NULL, "/", &save_ptr))
        {
            if (strcmp(token, ".") == 0) {
                continue;
            } else if (strcmp(token, "..") == 0) {
                if (!path_go_back(to)) return 0;
                continue;
            } else {
                int n = strlen(to);
                if (n != 1) memcpy(to + n, "/", 2);
                n = strlen(to);
                memcpy(to + n, token, strlen(token) + 1);
            }
        }
    } else { // relative path
        if (pwd != NULL) {
            to = malloc(strlen(pwd) + strlen(cd_target) + 3);
            memcpy(to, pwd, strlen(pwd) + 1);
        } else {
            to = malloc(strlen(cd_target) + 3);
            memcpy(to, "/", strlen("/") + 1);
        }
        char *token, *save_ptr;
        for (token = strtok_r (cd_copy, "/", &save_ptr); token != NULL; token = strtok_r (NULL, "/", &save_ptr))
        {
            if (strcmp(token, ".") == 0) {
                continue;
            } else if (strcmp(token, "..") == 0) {
                if (!path_go_back(to)) return 0;
                continue;
            } else {
                int n = strlen(to);
                if (n != 1) memcpy(to + n, "/", 2);
                n = strlen(to);
                memcpy(to + n, token, strlen(token) + 1);
            }
        }
    }
    return to;
}