#include "filesys/cache.h"

/* Initialize the buffer cache. */
void init_cache(void) {
  
  int i;
	for(i = 0; i < CACHE_SIZE; i++) {
		cache_block_arr[i].sec_num = -1;
		cache_block_arr[i].dirty = false;
    cache_block_arr[i].used = false;
    cache_block_arr[i].lru = -1;
	}

	lock_init(&buffer_cache_lock);

  // create a background asynchronous thread that periodically write back all dirty, cached blocks in the buffer cache to disk
  thread_create("buffer_cache_periodic_write_back", PRI_DEFAULT, periodic_write_behind, NULL);
  
  // initialize the read ahead blocks
  int j;
	for(j = 0; j < CACHE_SIZE; j++) {
		read_ahead_arr[j].sec_num = -1;
    read_ahead_arr[j].read = false;
	}

  // create a background asynchronous thread that read ahead
  thread_create("buffer_cache_periodic_read_ahead", PRI_DEFAULT, periodic_read_ahead, NULL);
}

/* Read from buffer cache. */
void read_cache(block_sector_t sector, void* buffer, off_t br, off_t so, int cs) {
    
    lock_acquire(&buffer_cache_lock);
    
    // check if the sector has a corresponding buffer cache block
    int n = in_cache(sector);

    // no corresponding block
    if (n == -1) {
        // fetch the block from disk into cache
        n = fetch_from_disk(sector);
    } 
    // has corresponding block
    else 
    {
        // use the cached data without going into disk
        memcpy(buffer + br, cache_block_arr[n].data + so, cs);
    }

    // increment the lru for eviction
    int i;
    for (i = 0; i < CACHE_SIZE; i++) {
        if (cache_block_arr[i].used) {
            cache_block_arr[i].lru++;
        }
    }

    cache_block_arr[n].lru = 0;

    // read ahead
    int j;
    for(j = 0; j < CACHE_SIZE; j++) {
      if (read_ahead_arr[j].sec_num == -1) {
        read_ahead_arr[j].sec_num = sector + 1;
        break;
      }
    }
    
    lock_release(&buffer_cache_lock);
}

/* Write to buffer cache. */
void write_cache(block_sector_t sector, void* buffer, off_t bw, off_t so, int cs) {

    lock_acquire(&buffer_cache_lock);

    // check if the sector has a corresponding buffer cache block
    int n = in_cache(sector);

    // no corresponding block
    if (n == -1) {
        // fetch the block from disk into cache
        n = fetch_from_disk(sector);
    } 
    // has corresponding block
    else
    {
        // write to the buffer cache
        memcpy(cache_block_arr[n].data + so, buffer + bw, cs);
        cache_block_arr[n].dirty = true;
    }

    //increment the lru for eviction
    int i;
    for (i = 0; i < CACHE_SIZE; i++) {
        if (cache_block_arr[i].used) {
            cache_block_arr[i].lru++;
        }
    }
    cache_block_arr[n].lru = 0;

    lock_release(&buffer_cache_lock);
}

/* Returns the block index of the buffer cache with corresponding SECTOR number,
-1 otherwise. */
int in_cache(block_sector_t sector) {
    int i;
    for (i = 0; i < CACHE_SIZE; i++) {
        if (cache_block_arr[i].sec_num == sector) {
            return i;
        }
    }
    return -1;
}

/* Allocate a buffer cache entry. Return the entry index.*/
int fetch_from_disk(block_sector_t sector) {
    int i;
    int n = -1;

    // find the first unused entry
    for (i = 0; i < CACHE_SIZE; i++) {
        if (cache_block_arr[i].used == false) {
            n = i;
            break;
        }
    }
    
    // buffer cache is full
    if (n == -1) {
        
        int j, k, max;
        max = 0;

        // get the block to evict
        for (j = 0; j < CACHE_SIZE; j++) {
            if (cache_block_arr[j].used == true && cache_block_arr[j].lru > max) {
                max = cache_block_arr[j].lru;
                k = j;
            }
        }

        // write back to disk
        block_write(fs_device, cache_block_arr[k].sec_num, &cache_block_arr[k].data);
        cache_block_arr[k].dirty = false;
        cache_block_arr[k].used = false;
        n = k;
    }

    cache_block_arr[n].sec_num = sector;
    cache_block_arr[n].dirty = false;
    cache_block_arr[n].used = true;
    cache_block_arr[n].lru = 0;
    block_read(fs_device, cache_block_arr[n].sec_num, &cache_block_arr[n].data);

    return n;
}

/* Write back from the buffer cache to disk. */
void write_back_cache(void) {

  lock_acquire(&buffer_cache_lock);

  // write back all the dirty blocks of the buffer cache
  int i;
  for (i = 0; i < CACHE_SIZE; i++) {
        if (cache_block_arr[i].dirty == true) {
            block_write(fs_device, cache_block_arr[i].sec_num, cache_block_arr[i].data);
            cache_block_arr[i].dirty = false;
        }
    }

  lock_release(&buffer_cache_lock);

}

/* Periodically read ahead the sectors. */
void periodic_read_ahead(void* aux UNUSED) {
  
  while (1) {

    // read ahead every 3 seconds
    timer_sleep (3 * TIMER_FREQ);

    lock_acquire(&buffer_cache_lock);
    
    int i;
    for(i = 0; i < CACHE_SIZE; i++) {
      if (read_ahead_arr[i].sec_num != -1 && !read_ahead_arr[i].read) {
          fetch_from_disk(read_ahead_arr[i].sec_num);
          read_ahead_arr[i].read = true;
      }

    }

    lock_release(&buffer_cache_lock);

  }

}

/* Periodically write behind the sectors. */
void periodic_write_behind(void* aux UNUSED) {

    while (1) {
      // write behind every 3 seconds
      timer_sleep (3 * TIMER_FREQ);
      write_back_cache();
    }

}