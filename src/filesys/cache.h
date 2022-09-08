#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <string.h>
#include "filesys/filesys.h"
#include "devices/block.h"
#include "devices/timer.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define CACHE_SIZE 64

struct buffer_cache_entry {

    char data[BLOCK_SECTOR_SIZE];            // the data in 512 bytes

    block_sector_t sec_num;                  // the corresponding sector number of the inode

    int lru;                                 // the number to evict blocks using LRU
    bool used;                               // if the block is in use
    bool dirty;                              // if the block is dirty

};

struct read_ahead_entry {

    block_sector_t sec_num;                  // the sector index of sector being read ahead
    bool read;                               // if the sector is already read to buffer cache

};

struct lock buffer_cache_lock;                             // the lock for the sycchronization of buffer cache
struct buffer_cache_entry cache_block_arr[CACHE_SIZE];     // the array for buffer cache
struct read_ahead_entry read_ahead_arr[CACHE_SIZE];        // the array for read ahead

void init_cache(void);
void read_cache(block_sector_t sector, void *buffer, off_t br, off_t so, int cs);
void write_cache(block_sector_t sector, void *buffer, off_t bw, off_t so, int cs);
int in_cache(block_sector_t sector);
int fetch_from_disk(block_sector_t sector);
void write_back_cache(void);
void periodic_read_ahead(void* aux UNUSED);
void periodic_write_behind(void* aux UNUSED);


#endif
