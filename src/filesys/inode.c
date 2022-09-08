#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "devices/block.h"
#include "threads/synch.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INDIRECT_PTRS 128
#define LEVEL_NUM 4

#define BLOCK_NUM 15
#define DIRECT_BLOCK_NUM 12
#define INDIRECT_BLOCK_NUM 1
#define DOUBLE_INDIRECT_BLOCK_NUM 1
#define TRIPLE_INDIRECT_BLOCK_NUM 1

#define LEVEL0_INDEX (DIRECT_BLOCK_NUM * BLOCK_SECTOR_SIZE)
#define LEVEL1_INDEX (LEVEL0_INDEX + INDIRECT_PTRS * BLOCK_SECTOR_SIZE)
#define LEVEL2_INDEX (LEVEL1_INDEX + INDIRECT_PTRS * INDIRECT_PTRS * BLOCK_SECTOR_SIZE)
#define LEVEL3_INDEX (LEVEL2_INDEX + INDIRECT_PTRS * INDIRECT_PTRS * INDIRECT_PTRS * BLOCK_SECTOR_SIZE)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  off_t length;                 /* File size in bytes. */
  uint32_t blocks[BLOCK_NUM];   /* Blocks to store data */
  unsigned isDir;               /* Is directory or not */
  unsigned magic;               /* Magic number. */
  uint32_t unused[110];          /* Not used. */
};

/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
  struct lock lock;       /* For synchronization */
};

struct data_index
{
  uint32_t secondary_index[LEVEL_NUM];  /* An array of secondary Index for locating data.  */
  uint32_t primary_index[LEVEL_NUM];    /* An array of primary Index for locating data.  */
  uint16_t lev;                         /* The block level where the offset is*/
  uint32_t off;                         /* Data Offset */
};

bool get_data_index(struct data_index *data_index, off_t off);
bool get_data_index_for_lev3(struct data_index *data_index, off_t off);
bool get_data_index_for_lev2(struct data_index *data_index, off_t off);
bool get_data_index_for_lev1(struct data_index *data_index, off_t off);
bool get_data_index_for_lev0(struct data_index *data_index, off_t off);
bool inode_allocate(struct inode_disk *, off_t);
void inode_free(struct inode *inode);

/* Returns data_index struct which contains the information about where the OFF should
   be located. If the offset OFF is out of bound, return false */
bool 
get_data_index(struct data_index *data_index, off_t off)
{
  bool success = false;

  // initialize the secondary index array, so that they can be used later
  for (int i = 0; i < LEVEL_NUM; i++)
    data_index->secondary_index[i] = 0;

  // return false if the offset is out of bound
  if (off < 0 || off >= LEVEL3_INDEX)
    return success;

  // the data is in level 3 block
  if (off >= LEVEL2_INDEX)
    success = get_data_index_for_lev3(data_index, off);

  // the data is in level 2 block
  else if (off >= LEVEL1_INDEX)
    success = get_data_index_for_lev2(data_index, off);

  // the data is in level 1 block
  else if (off >= LEVEL0_INDEX)
    success = get_data_index_for_lev1(data_index, off);

  // the data is in level 0 block
  else
    success = get_data_index_for_lev0(data_index, off);
  return success;
}

/* Returns data_index struct which contains the information about where the OFF should
   be located in level 3 block*/
bool get_data_index_for_lev3(struct data_index *data_index, off_t off)
{
  // data is is level 3 block
  data_index->lev = 3;

  // the offset in level 3 block
  off -= LEVEL2_INDEX;

  // set the block index among 15 blocks in level 0
  data_index->primary_index[0] = DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM + DOUBLE_INDIRECT_BLOCK_NUM;

  // set the block index in triple indirect block in level 1 
  data_index->primary_index[1] = off / (INDIRECT_PTRS * INDIRECT_PTRS * BLOCK_SECTOR_SIZE);
  off -= data_index->primary_index[1] * (INDIRECT_PTRS * INDIRECT_PTRS * BLOCK_SECTOR_SIZE);

  // set the block index in triple indirect block in level 2 
  data_index->primary_index[2] = off / (INDIRECT_PTRS * BLOCK_SECTOR_SIZE);
  off -= data_index->primary_index[2] * (INDIRECT_PTRS * BLOCK_SECTOR_SIZE);

  // set the block index in triple indirect block in level 3 
  data_index->primary_index[3] = off / BLOCK_SECTOR_SIZE;
  data_index->off = off % BLOCK_SECTOR_SIZE;

  return true;
}

/* Returns data_index struct which contains the information about where the OFF should
   be located in level 2 block*/
bool get_data_index_for_lev2(struct data_index *data_index, off_t off)
{
  // data is is level 2 block
  data_index->lev = 2;
  
  // the offset in level 2 block
  off -= LEVEL1_INDEX;

  // set the block index among 15 blocks in level 0
  data_index->primary_index[0] = DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM;

  // set the block index in double indirect block in level 1 
  data_index->primary_index[1] = off / (BLOCK_SECTOR_SIZE * INDIRECT_PTRS);
  off -= data_index->primary_index[1] * (BLOCK_SECTOR_SIZE * INDIRECT_PTRS);

  // set the block index in double indirect block in level 2 
  data_index->primary_index[2] = off / BLOCK_SECTOR_SIZE;
  data_index->off = off % BLOCK_SECTOR_SIZE;

  return true;
}

/* Returns data_index struct which contains the information about where the OFF should
   be located in level 1 block*/
bool get_data_index_for_lev1(struct data_index *data_index, off_t off)
{
  // the offset in level 1 block
  off -= LEVEL0_INDEX;

  // data is is level 1 block
  data_index->lev = 1;

  // set the block index among 15 blocks in level 0
  data_index->primary_index[0] = DIRECT_BLOCK_NUM;

  // set the block index in indirect block in level 1 
  data_index->primary_index[1] = off / BLOCK_SECTOR_SIZE;
  data_index->off = off % BLOCK_SECTOR_SIZE;

  return true;
}

/* Returns data_index struct which contains the information about where the OFF should
   be located in level 0 block*/
bool get_data_index_for_lev0(struct data_index *data_index, off_t off)
{
  // the offset in level 0 block
  data_index->lev = 0;

  // set the block index among 15 blocks in level 0
  data_index->primary_index[0] = off / BLOCK_SECTOR_SIZE;
  data_index->off = off % BLOCK_SECTOR_SIZE;

  return true;
}

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors(off_t size)
{
  return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
// static block_sector_t
// byte_to_sector (const struct inode *inode, off_t pos)
// {
//   ASSERT (inode != NULL);
//   if (pos < inode->data.length)
//     return inode->data.start + pos / BLOCK_SECTOR_SIZE;
//   else
//     return -1;
// }

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void)
{
  list_init(&open_inodes);
}

bool inode_create(block_sector_t sector, off_t length)
{
  return inode_create_by_type(sector, length, 0);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create_by_type(block_sector_t sector, off_t length, uint32_t isDir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->isDir = isDir;
    if (inode_allocate(disk_inode, length))
    {
      block_write(fs_device, sector, disk_inode);
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

bool inode_allocate(struct inode_disk *disk_inode, off_t length)
{
  int i, j;

  // initialize a inode to call inode_write_at function later
  struct inode inode;

  // an array of zeros 
  static char zeros[BLOCK_SECTOR_SIZE];

  // initialize the blocks to 0
  for (i = 0; i < BLOCK_NUM; i++)
  {
    disk_inode->blocks[i] = 0;
  }
  inode.deny_write_cnt = 0;
  inode.data = *disk_inode;

  // the number of sectors to write
  size_t sectors = bytes_to_sectors(length);
  for (j = 0; j < (int) sectors; j++)
  {
    size_t size = length < BLOCK_SECTOR_SIZE ? length : BLOCK_SECTOR_SIZE;

    // write zeros 
    inode_write_at(&inode, zeros, size, j * BLOCK_SECTOR_SIZE);
    length -= BLOCK_SECTOR_SIZE;
  }

  *disk_inode = inode.data;

  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open(block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
       e = list_next(e))
  {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector)
    {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);
  block_read(fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen(struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber(const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed)
    {
      free_map_release(inode->sector, 1);
      inode_free(inode);
    }
    free(inode);
  }
}

/* free the inode */
void inode_free(struct inode *inode)
{ 
  // free triple indirect blocks
  if (inode->data.blocks[DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM + DOUBLE_INDIRECT_BLOCK_NUM] != 0)
  {
    // level 1 blocks
    unsigned *buffer4 = (unsigned *)malloc(BLOCK_SECTOR_SIZE);
    block_read(fs_device, inode->data.blocks[DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM + DOUBLE_INDIRECT_BLOCK_NUM], buffer4);
    int j, k, l;
    for (j = 0; j < INDIRECT_PTRS && buffer4[j] != 0; j++)
    {
      // level 2 blocks
      unsigned *buffer5 = (unsigned *)malloc(BLOCK_SECTOR_SIZE);
      block_read(fs_device, buffer4[j], buffer5);
      for (k = 0; k < INDIRECT_PTRS && buffer5[k] != 0; k++)
      {
        // level 3 blocks
        unsigned *buffer6 = (unsigned *)malloc(BLOCK_SECTOR_SIZE);
        block_read(fs_device, buffer5[k], buffer6);
        for (l = 0; l < INDIRECT_PTRS && buffer6[l] != 0; l++)
        {
          free_map_release(buffer6[l], 1);
          free(buffer6);
        }
        free_map_release(buffer5[k], 1);
        free(buffer5);
      }

      free_map_release(buffer4[j], 1);
    }
    free_map_release(inode->data.blocks[DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM + DOUBLE_INDIRECT_BLOCK_NUM], 1);
    free(buffer4);
  }

  // free double indirect blocks
  if (inode->data.blocks[DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM] != 0)
  {
    // level 1 blocks
    unsigned *buffer2 = (unsigned *)malloc(BLOCK_SECTOR_SIZE);
    block_read(fs_device, inode->data.blocks[DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM], buffer2);
    int j, k;
    for (j = 0; j < INDIRECT_PTRS && buffer2[j] != 0; j++)
    {
      // level 2 blocks
      unsigned *buffer3 = (unsigned *)malloc(BLOCK_SECTOR_SIZE);
      block_read(fs_device, buffer2[j], buffer3);
      for (k = 0; k < INDIRECT_PTRS && buffer3[k] != 0; k++)
      {
        free_map_release(buffer3[k], 1);
        free(buffer3);
      }
      free_map_release(buffer2[j], 1);
    }
    free_map_release(inode->data.blocks[DIRECT_BLOCK_NUM + INDIRECT_BLOCK_NUM], 1);
    free(buffer2);
  }

  // free indirect blocks
  if (inode->data.blocks[DIRECT_BLOCK_NUM] != 0)
  {
    // level 1 blocks
    unsigned *buffer = (unsigned *)malloc(BLOCK_SECTOR_SIZE);
    block_read(fs_device, inode->data.blocks[DIRECT_BLOCK_NUM], buffer);
    for (int j = 0; j < INDIRECT_PTRS && buffer[j] != 0; j++)
      free_map_release(buffer[j], 1);
    free(buffer);
    free_map_release(inode->data.blocks[DIRECT_BLOCK_NUM], 1);
  }

  // free direct blocks
  for (int i = 0; i < DIRECT_BLOCK_NUM; i++)
    if (inode->data.blocks[i] != 0)
      free_map_release(inode->data.blocks[i], 1);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode)
{
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  off_t desired_byte = inode->data.length - offset;
  if (desired_byte <= 0)
    return 0;

  uint8_t *buffer = buffer_;
  off_t bytes_read = 0, cur_read_bytes = 0;
  struct data_index data_index;

  // temporary buffer
  uint32_t *tem = (uint32_t *)malloc(BLOCK_SECTOR_SIZE);

  while (size > 0)
  {
    // get the information of data location
    if (!get_data_index(&data_index, offset))
    {
      size -= cur_read_bytes;
      memset(buffer + bytes_read, 0, cur_read_bytes);
      bytes_read += cur_read_bytes;
      offset += cur_read_bytes;
      continue;
    }

    // determine the number of bytes to read for current step
    cur_read_bytes = size;
    if ((int) (BLOCK_SECTOR_SIZE - data_index.off) < (int) size)
      cur_read_bytes = BLOCK_SECTOR_SIZE - data_index.off;

    // determine the level of block
    uint16_t level = data_index.lev;

    // secondary index for multiple level blocks
    // the value is 0 if it does not contain bytes
    uint32_t secondary = inode->data.blocks[data_index.primary_index[0]];
    if (secondary == 0)
    {
      size -= cur_read_bytes;
      memset(buffer + bytes_read, 0, cur_read_bytes);
      bytes_read += cur_read_bytes;
      offset += cur_read_bytes;
      continue;
    }
    else
    {
      // update secondary index in data_index
      data_index.secondary_index[0] = secondary;

      // read the blocks into temporary buffer
      block_read(fs_device, secondary, (void *)tem);
    }

    if (level >= 1)
    {
      uint32_t secondary = tem[data_index.primary_index[1]];
      if (secondary == 0)
      {
        size -= cur_read_bytes;
        memset(buffer + bytes_read, 0, cur_read_bytes);
        bytes_read += cur_read_bytes;
        offset += cur_read_bytes;
        continue;
      }
      else
      {
        // read the blocks into temporary buffer
        block_read(fs_device, secondary, (void *)tem);

        // update secondary index in data_index
        data_index.secondary_index[1] = secondary;
      }
    }
    if (level >= 2)
    {
      uint32_t secondary = tem[data_index.primary_index[2]];
      if (secondary == 0)
      {
        size -= cur_read_bytes;
        memset(buffer + bytes_read, 0, cur_read_bytes);
        bytes_read += cur_read_bytes;
        offset += cur_read_bytes;
        continue;
      }
      else
      {
        // read the blocks into temporary buffer
        block_read(fs_device, secondary, (void *)tem);

        // update secondary index in data_index
        data_index.secondary_index[2] = secondary;
      }
    }
    if (level >= 3)
    {
      uint32_t secondary = tem[data_index.primary_index[3]];
      if (secondary == 0)
      {
        size -= cur_read_bytes;
        memset(buffer + bytes_read, 0, cur_read_bytes);
        bytes_read += cur_read_bytes;
        offset += cur_read_bytes;
        continue;
      }
      else
      {
        // read the blocks into temporary buffer
        block_read(fs_device, secondary, (void *)tem);

        // update secondary index in data_index
        data_index.secondary_index[3] = secondary;
      }
    }
    size -= cur_read_bytes;
    memcpy(buffer + bytes_read, (void *)tem + data_index.off, cur_read_bytes);
    offset += cur_read_bytes;
    bytes_read += cur_read_bytes;
  }
  free(tem);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
                     off_t offset)
{
  // the file cannot be written
  if (inode->deny_write_cnt)
    return 0;

  const uint8_t *buffer = buffer_;

  // an array of zeros
  static char zeros[BLOCK_SECTOR_SIZE];
  memset(zeros, 0, BLOCK_SECTOR_SIZE);

  // final return value.
  // it will increment at every step
  off_t bytes_written = 0;

  // initialize data_index struct to get data location information
  struct data_index data_index;

  // temporary buffer
  uint32_t *tem = (uint32_t *)malloc(BLOCK_SECTOR_SIZE), cur_write = 0;

  while (size > 0)
  {
    // exceeds file size limit
    if (!get_data_index(&data_index, offset))
      return 0;

    // determine the number of bytes to write for current step
    cur_write = size;
    if ((int) (BLOCK_SECTOR_SIZE - data_index.off) < size)
      cur_write = BLOCK_SECTOR_SIZE - data_index.off;

    // secondary index for multiple level blocks
    // the value is 0 if it does not contain bytes
    uint32_t secondary = inode->data.blocks[data_index.primary_index[0]];

    // determine the level of block
    uint16_t level = data_index.lev;
    
    // the level 1 block has not yet been written
    if (secondary == 0)
    {
      // try allocate free map
      if (!free_map_allocate(1, &secondary))
        break;

      // update the block sector to the allocated one
      inode->data.blocks[data_index.primary_index[0]] = secondary;

      // write zeros to temporary buffer
      memcpy(tem, zeros, BLOCK_SECTOR_SIZE);

      // update the secondary index
      data_index.secondary_index[0] = secondary;
    }
    else
    {
      // read the block into temporary buffer
      block_read(fs_device, secondary, tem);

      // update the secondary index
      data_index.secondary_index[0] = secondary;
    }

    if (level >= 1)
    {
      // update the secondary index to level 1 block
      secondary = tem[data_index.primary_index[1]];
      if (secondary == 0)
      {
        // try allocate
        if (!free_map_allocate(1, &secondary))
          break;
        tem[data_index.primary_index[1]] = secondary;
        block_write(fs_device, data_index.secondary_index[0], tem);
        memcpy(tem, zeros, BLOCK_SECTOR_SIZE);
        data_index.secondary_index[1] = secondary;
      }
      else
      {
        block_read(fs_device, secondary, tem);
        data_index.secondary_index[1] = secondary;
      }
    }

    if (level >= 2)
    {
      secondary = tem[data_index.primary_index[2]];
      if (secondary == 0)
      {
        if (!free_map_allocate(1, &secondary))
          break;

        // update the level 2 block after being allocated
        tem[data_index.primary_index[2]] = secondary;
        block_write(fs_device, data_index.secondary_index[1], tem);
        memcpy(tem, zeros, BLOCK_SECTOR_SIZE);
        data_index.secondary_index[2] = secondary;
      }
      else
      {
        block_read(fs_device, secondary, tem);
        data_index.secondary_index[2] = secondary;
      }
    }

    if (level >= 3)
    {
      secondary = tem[data_index.primary_index[3]];
      if (secondary == 0)
      {
        if (!free_map_allocate(1, &secondary))
          break;
        tem[data_index.primary_index[3]] = secondary;
        block_write(fs_device, data_index.secondary_index[2], tem);
        memcpy(tem, zeros, BLOCK_SECTOR_SIZE);
        data_index.secondary_index[3] = secondary;
      }
      else
      {
        block_read(fs_device, secondary, tem);
        data_index.secondary_index[3] = secondary;
      }
    }

    // copy the cur_write number of bytes in buffer into temporary buffer
    memcpy((void *)tem + data_index.off, buffer + bytes_written, cur_write);
    block_write(fs_device, data_index.secondary_index[level], tem);
    bytes_written = bytes_written + cur_write;
    offset = offset + cur_write;
    size -= cur_write;
  }

  // update the inode length if offset exceeds current inode length
  // for file growth
  if (offset > inode->data.length)
  {
    inode->data.length = offset;

    // update the inode in disk
    block_write(fs_device, inode->sector, &inode->data);
  }

  free(tem);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode)
{
  if (inode->deny_write_cnt > 0)
  {
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
    inode->deny_write_cnt--;
  }
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode)
{
  return inode->data.length;
}

unsigned inode_isdir(const struct inode *inode)
{
  return inode->data.isDir;
}
bool inode_isremoved(const struct inode *inode)
{
  return inode->removed;
}

bool inode_isopened(struct inode*inode)
{
  return inode->open_cnt > 1;
}

void inode_require_lock (const struct inode *inode)
{
  lock_acquire(&((struct inode *)inode)->lock);
}

void inode_release_lock (const struct inode *inode)
{
  lock_release(&((struct inode *) inode)->lock);
}
