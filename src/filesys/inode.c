#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t direct[12]; /* Direct block pointer. */
  block_sector_t indirect; /* Indirect block pointer. */
  block_sector_t indirect_double; /* Double indirect block pointer. */

  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  uint32_t unused[112]; /* Not used. */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long, not inlcude internal or root. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* Return the number of all sectors for an inode SIZE bytes, including root and internal nodes */
static size_t bytes_to_blocks(off_t size) {
  size_t data_num = bytes_to_sectors(size);
  if (data_num <= 12) {
    return data_num + 1;
  } else if (data_num <= 128 + 12) {
    return data_num + 2;
  } else {
    return data_num + (data_num - 140) / 128 + 3;
  }
}

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct lock inode_lock; /* Lock for each inode struct. */  
};

struct cache_block {
  char content[BLOCK_SECTOR_SIZE];
  bool is_dirty;
  bool is_valid;
  block_sector_t bst;
  struct rw_lock lock;
  struct list_elem elem;
  // for testing purpose
  int hit_cnt;
  int miss_cnt;
};

struct list cache;
struct lock cache_lock;

bool inode_resize(struct inode_disk*, off_t);
struct cache_block* new_cache_block(void);
struct cache_block* find_block_and_acq_lock(block_sector_t bst, bool reader);

/* make a new cache block */
struct cache_block* new_cache_block() {
  struct cache_block* b = (struct cache_block*) calloc(sizeof(struct cache_block), 1);
  b->is_dirty = false;
  b->is_valid = false;
  rw_lock_init(&b->lock);
  b->hit_cnt = 0;
  b->miss_cnt = 0;
  return b;
}

/* initialize 64 blocks in cache and the cache lock */
void cache_init() {
  list_init(&cache);
  lock_init(&cache_lock);
  for (int i = 0; i < 64; i++) {
    struct cache_block* b = new_cache_block();
    list_push_front(&cache, &b->elem);
  }
}

void cache_read(void* dest, block_sector_t bst) {
  struct cache_block* b = find_block_and_acq_lock(bst, true);
  memcpy(dest, b->content, BLOCK_SECTOR_SIZE);
  rw_lock_release(&b->lock, true);
}

void cache_write(void* src, block_sector_t bst) {
  struct cache_block* b = find_block_and_acq_lock(bst, false);
  memcpy(b->content, src, BLOCK_SECTOR_SIZE);
  b->is_dirty = true;
  rw_lock_release(&b->lock, false);
}

/* find the cache block corespond to the given sector, 
   if none match, evict the oldest unused one and cache the new block. 
   This function will acquire the lock in the cache block but won't release it upon return
   So caller should release the lock when its work is done
*/
struct cache_block* find_block_and_acq_lock(block_sector_t bst, bool reader) {
  bool not_found = true;
  // acquire lock for the cache so only 1 thread can access the cache at a time
  lock_acquire(&cache_lock);
  struct list_elem* e = list_begin(&cache);
  struct cache_block* b;
  // find the cache block corespond to the given sector
  while (e != list_end(&cache) && not_found) {
    b = list_entry(e, struct cache_block, elem);
    if (b->is_valid && b->bst == bst) {
      not_found = false;
    }
    e = list_next(e);
  }
  // if sector is in cache, b is the block of that sector in cache
  // if sector is not in cache, b is the last block in cache
  e = &b->elem;
  // move the block to the front of the cache
  list_remove(e);
  list_push_front(&cache, e);
  if (not_found) {
    b->miss_cnt++;
    rw_lock_acquire(&b->lock, false);
    // write cache block to disk if it is valid and dirty
    if (b->is_valid && b->is_dirty) {
      block_write(fs_device, b->bst, b->content);
    }
    // update cache block content and sector number, mark the cache block valid and not dirty
    b->bst = bst;
    block_read(fs_device, bst, b->content);
    b->is_valid = true;
    b->is_dirty = false;
    rw_lock_release(&b->lock, false);
  }
  else {
    b->hit_cnt++;
  }
  rw_lock_acquire(&b->lock, reader);
  lock_release(&cache_lock);
  return b;
}

void cache_destroy() {
  while (!list_empty(&cache)) {
    struct list_elem* e = list_pop_front(&cache);
    struct cache_block* b = list_entry(e, struct cache_block, elem);
    // acquire write lock because we want to destroy the cache block, 
    // so wait for any access from other threads to finish
    rw_lock_acquire(&b->lock, false);
    // write any dirty block to disk
    if (b->is_valid && b->is_dirty) {
      block_write(fs_device, b->bst, b->content);
    }
    // can release the lock before destroying the block because thread holds the cache lock,
    // so no other thread can access the destroying block
    rw_lock_release(&b->lock, false);
    free(b);
  }
}

void cache_reset() {
  lock_acquire(&cache_lock);
  cache_destroy();
  cache_init();
}

int get_cache_hit_cnt() {
  int total_hit_cnt = 0;
  lock_acquire(&cache_lock);
  struct list_elem* e = list_begin(&cache);
  while (e != list_end(&cache)) {
    struct cache_block* b = list_entry(e, struct cache_block, elem);
    total_hit_cnt += b->hit_cnt;
    e = list_next(e);
  }
  lock_release(&cache_lock);
  return total_hit_cnt;
}

int get_cache_miss_cnt() {
  int total_miss_cnt = 0;
  lock_acquire(&cache_lock);
  struct list_elem* e = list_begin(&cache);
  while (e != list_end(&cache)) {
    struct cache_block* b = list_entry(e, struct cache_block, elem);
    total_miss_cnt += b->miss_cnt;
    e = list_next(e);
  }
  lock_release(&cache_lock);
  return total_miss_cnt;
}


/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);
  size_t sector_num = bytes_to_sectors(pos);
  struct inode_disk* inode_content = NULL;
  cache_read((void*)inode_content, inode->sector);

  if (sector_num <= 12) {
    return inode_content->direct[sector_num - 1];
  } else if (sector_num <= 140) {
    sector_num -= 12;
    block_sector_t indir_content[128];
    cache_read((void*)indir_content, inode_content->indirect);
    return indir_content[sector_num - 1];
  } else {
    sector_num -= 140;
    // Read first level indirect pointer
    block_sector_t indir2_content[128];
    cache_read((void*)indir2_content, inode_content->indirect_double);

    // Read second level indirect pointer
    block_sector_t indir_content[128];
    cache_read((void*)indir_content, indir2_content[sector_num / 128]); // Start from 0, no need to + 1

    return indir_content[sector_num % 128 - 1];
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Protect open inode list */
struct lock inode_list_lock;

/* Initializes the inode module. */
void inode_init(void) {
  list_init(&open_inodes);
  lock_init(&inode_list_lock);
}

/* Call resize in inode_write and inode_create */
bool inode_resize(struct inode_disk* ind_d, off_t size) {
  // Get block number including all internal and root block
  size_t num_block_old = bytes_to_blocks(ind_d->length);
  size_t num_block_new = bytes_to_blocks(size);
  bool success = true;

  size_t new_alloc_num = (num_block_new - num_block_old) > 0 ? (num_block_new - num_block_old) : 0;
  block_sector_t new_block_list[new_alloc_num];
  success = free_map_allocate_non_consecutive(new_alloc_num, new_block_list);
  if (!success) {
    return false;
  }

  int new_list_i = 0;
  // Handle direct pointer
  for (int i = 0; i < 12; i++) {
    if (size <= BLOCK_SECTOR_SIZE * i && ind_d->direct[i] != 0) {
      // Shrink
      free_map_release(ind_d->direct[i], 1);
      ind_d->direct[i] = 0;
    } else if (size > BLOCK_SECTOR_SIZE * i && ind_d->direct[i] == 0) {
      // Grow
      ind_d->direct[i] = new_block_list[new_list_i++];
      static char zeros[BLOCK_SECTOR_SIZE];
      cache_write(zeros, new_block_list[i]);
    }
  }
  if (ind_d->indirect == 0 && size <= 12 * BLOCK_SECTOR_SIZE) {
    ind_d->length = size;
    return true;
  }

  // Handle indirect pointer, hit only if indir ptr is needed
  block_sector_t buffer[128];
  memset(buffer, 0, 512);
  // Create indirect pointer if not exist, read from disk otherwise
  if (ind_d->indirect == 0) {
    ind_d->indirect = new_block_list[new_list_i++];
  } else {
    cache_read((void*)buffer, ind_d->indirect);
  }
  for (int i = 0; i < 128; i++) {
    if (size <= (12 + i) * BLOCK_SECTOR_SIZE && buffer[i] != 0) {
      // Shrink
      free_map_release(buffer[i], 1);
      buffer[i] = 0;
    } else if (size > (12 + i) * BLOCK_SECTOR_SIZE && buffer[i] == 0) {
      // Grow
      buffer[i] = new_block_list[new_list_i++];
    }
  }
  cache_write((void*)buffer, ind_d->indirect);
  if (ind_d->indirect_double == 0 && size <= 140 * BLOCK_SECTOR_SIZE) {
    ind_d->length = size;
    return true;
  }

  // Handle doubly indirect pointer, hit only if db indir ptr is needed
  block_sector_t buffer1[128];
  memset(buffer1, 0, 512);
  // Load doubly indirect pointer into buffer
  if (ind_d->indirect_double == 0) {
    ind_d->indirect_double = new_block_list[new_list_i++];
  } else {
    cache_read((void*)buffer1, ind_d->indirect_double);
  }
  for (int i = 0; i < 128; i++) {
    if (size <= (140 + i) * BLOCK_SECTOR_SIZE && buffer1[i] != 0) {
      // Shrink first pointer
      free_map_release(buffer1[i], 1);
      buffer1[i] = 0;
    } else if (size > (140 + i) * BLOCK_SECTOR_SIZE) {
      // Grow first pointer
      block_sector_t buffer2[128];
      memset(buffer2, 0, 512);
      if (buffer1[i] == 0) {
        buffer1[i] = new_block_list[new_list_i++];
      } else {
        cache_read((void*)buffer2, buffer1[i]);
      }
      for (int j = 0; j < 128; j++) {
        if (size <= (140 + i * 128 + j) * BLOCK_SECTOR_SIZE && buffer2[j] != 0) {
          // Shrink second pointer
          free_map_release(buffer2[j], 1);
          buffer2[j] = 0;
        } else if (size > (140 + i * 128 + j) * BLOCK_SECTOR_SIZE && buffer2[j] == 0) {
          // Grow second pointer
          buffer2[j] = new_block_list[new_list_i++];
        }
      }
      cache_write((void*)buffer2, buffer1[i]);
    }
  }
  cache_write((void*)buffer1, ind_d->indirect_double);
  ind_d->length = size;
  return true;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length) {
  struct inode_disk* disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    disk_inode->length = 0;
    disk_inode->magic = INODE_MAGIC;

    if (inode_resize(disk_inode, length)) {
      cache_write(disk_inode, sector);
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */

  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->inode_lock);

  lock_acquire(&inode_list_lock);
  list_push_front(&open_inodes, &inode->elem);
  lock_release(&inode_list_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  lock_acquire(&inode->inode_lock);
  if (inode != NULL)
    inode->open_cnt++;
  lock_release(&inode->inode_lock);
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    lock_acquire(&inode->inode_lock);
    list_remove(&inode->elem);
    lock_release(&inode->inode_lock);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      struct inode_disk* ind_d = NULL;
      cache_read(ind_d, inode->sector);
      // Resize the inode to be 0 size so that all blocks are deallocated
      inode_resize(ind_d, 0);
      free_map_release(inode->sector, 1);
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t* bounce = NULL;

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      cache_read(buffer + bytes_read, sector_idx);
    } else {
      /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      cache_read(bounce, sector_idx);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t* bounce = NULL;

  lock_acquire(&inode->inode_lock);
  if (inode->deny_write_cnt)
    return 0;

  // Resize inode if necessary
  off_t new_lenght = size + offset;
  struct inode_disk* ind_d = NULL;
  cache_read(ind_d, inode->sector);
  if (new_lenght > ind_d->length) {
    inode_resize(ind_d, new_lenght);
  }

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Write full sector directly to disk. */
      cache_write((void *) (buffer + bytes_written), sector_idx);
    } else {
      /* We need a bounce buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }

      /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left)
        cache_read(bounce, sector_idx);
      else
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      cache_write(bounce, sector_idx);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);
  lock_release(&inode->inode_lock);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  lock_acquire(&inode->inode_lock);
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  lock_acquire(&inode->inode_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  lock_acquire(&inode->inode_lock);
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release(&inode->inode_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) {
  struct inode_disk* ind_d = NULL;
  cache_read(ind_d, inode->sector);
  return ind_d->length;
}
