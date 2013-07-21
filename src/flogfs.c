/*
Copyright (c) 2013, Ben Nahill <bnahill@gmail.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the FLogFS Project.
*/

/*!
 * @file flogfs.c
 * @author Ben Nahill <bnahill@gmail.com>
 * @ingroup FLogFS
 * @brief Core file system implementation
 */

#include "flogfs_private.h"
#include "flogfs.h"

#include <string.h>

#ifndef IS_DOXYGEN
#if !FLOG_BUILD_CPP
#ifdef __cplusplus
extern "C" {
#endif
#endif
#endif

#include "flogfs_conf_implement.h"

//! @addtogroup FLogPrivate
//! @{


typedef struct {
	flog_block_idx_t block;
	flog_block_age_t age;
} flog_block_alloc_t;

typedef struct {
	//! Block indices and ages
	flog_block_alloc_t blocks[FS_PREALLOCATE_SIZE];
	//! The number of entries
	uint16_t n;
	//! The sum of all ages
	flog_block_age_t age_sum;
} flog_prealloc_list_t;

typedef struct {
	flog_block_idx_t block;
	flog_write_file_t * file;
} flog_dirty_block_t;

typedef struct {
	flog_file_id_t file_id;
	flog_block_idx_t first_block;
} flog_file_find_result_t;

/*!
 @brief The complete FLogFS state structure
 */
typedef struct {
	//! The head of the list of read files
	flog_read_file_t * read_head;
	//! The head of the list of write files
	flog_write_file_t * write_head;
	//! The maximum file ID active in the system
	uint32_t max_file_id;

	//! The state of the file system
	flog_state_t state;

	//! The average block age in the file system
	uint32_t mean_block_age;

	//! A list of preallocated blocks for quick access
	flog_prealloc_list_t prealloc;

	//! The most recent timestamp (sequence number)
	//! @note To put a stamp on a new operation, you should preincrement. This
	//! is the timestamp of the most recent operation
	flog_timestamp_t t;

	//! The location of the first inode block
	flog_block_idx_t inode0;
	//! The number of files in the system
	flog_file_id_t   num_files;
	//! The number of free blocks
	flog_block_idx_t num_free_blocks;

	//! @brief Flash cache status
	//! @note This must be protected under @ref flogfs_t::lock !
	struct {
	flog_block_idx_t current_open_block;
	uint16_t         current_open_page;
	uint_fast8_t     page_open;
	flog_result_t    page_open_result;
	} cache_status;

	//! A lock to serialize some FS operations
	fs_lock_t lock;
	//! A lock to block any allocation-related operations
	fs_lock_t allocate_lock;
	//! The one dirty_block
	//! @note This may only be accessed under @ref flogfs_t::allocate_lock
	flog_dirty_block_t dirty_block;
	//! The moving allocator head
	flog_block_idx_t allocate_head;
} flogfs_t;



//! A single static instance
static flogfs_t flogfs;

static inline void flog_lock_fs(){fs_lock(&flogfs.lock);}
static inline void flog_unlock_fs(){fs_unlock(&flogfs.lock);}

static inline void flog_lock_allocate(){fs_lock(&flogfs.allocate_lock);}
static inline void flog_unlock_allocate(){fs_unlock(&flogfs.allocate_lock);}


/*!
 @brief Go find a suitable free block to erase
 @return A block. The index will be FLOG_BLOCK_IDX_INVALID if invalid.

 This attempts to claim a block from the block preallocation list and searches
 for a new one of the list is empty

 @note This requires flogfs_t::allocate_lock
 */
static flog_block_alloc_t flog_allocate_block();

/*!
 @brief Iterate the block allocation routine and return the result
 */
static flog_block_alloc_t flog_allocate_block_iterate();

/*!
 @brief Get the next block entry from any valid block
 @param block The previous block
 @returns The next block

 This is fine for both inode and file blocks

 If block == FLOG_BLOCK_IDX_INVALID, it will be returned
 */
static inline flog_block_idx_t
flog_universal_get_next_block(flog_block_idx_t block);


/*!
 @brief Perform an iteration of the preallocator. Basically check one block as
 a candidate.

 @note This requires the allocation lock, \ref flogfs_t::allocate_lock
 */
static inline void flog_prealloc_iterate();

/*!
 @brief Find a file inode entry
 @param[in] filename The filename to check for
 @param[out] iter An inode iterator -- If nothing is found, this will point to
                  the next free inode iterator
 @retval Fileinfo with first_block == FLOG_BLOCK_IDX_INVALID if not found

 @note This requires the FS lock, \ref flogfs_t::lock
 */
static flog_file_find_result_t flog_find_file(char const * filename,
                                       flog_inode_iterator_t * iter);

/*!
 @brief Open a page (read to flash cache) only if necessary
 */
static inline flog_result_t flog_open_page(uint16_t block, uint16_t page);

/*!
 @brief Open the page corresponding to a sector
 @param block The block
 @param sector The sector you wish to access
 */
static inline flog_result_t flog_open_sector(uint16_t block, uint16_t sector);


/*!
 @brief Initialize an inode iterator
 @param[in,out] iter The iterator structure
 @param[in] inode0 The first inode block to use
 */
static void flog_inode_iterator_init(flog_inode_iterator_t * iter,
                                     flog_block_idx_t inode0);

/*!
 @brief Advance an inode iterator to the next entry
 @param[in,out] iter The iterator structure

 This doesn't deal with any allocation. That is done with
 flog_inode_prepare_new.

 @warning You MUST check on every iteration for the validity of the entry and
 not iterate past an unallocated entry.
 */
static void flog_inode_iterator_next(flog_inode_iterator_t * iter);

/*!
 @brief Claim a new inode entry as your own.

 @note This requires the @ref flogfs_t::allocate_lock. It might allocate
 something.

 @warning Please be sure that iter points to the first unallocated entry
 */
static flog_result_t flog_inode_prepare_new(flog_inode_iterator_t * iter);

/*!
 @brief Get the value of the next sector in sequence
 @param sector The previous sector
 @return Next sector

 Sectors are written and read out of order so this is used to get the correct
 sequence
 */
static inline uint16_t flog_increment_sector(uint16_t sector);

static flog_result_t flog_flush_write(flog_write_file_t * file);

/*!
 @brief Add a free block candidate to the preallocation list

 @note This requires the allocation lock
 */
static void flog_prealloc_push(flog_block_idx_t block,
                               flog_block_age_t age);

/*!
 @brief Take the youngest block from the preallocation list
 @retval Index The allocated block index
 @retval FLOG_BLOCK_IDX_INVALID if empty

 @note This requires the allocation lock
 */
static flog_block_alloc_t flog_prealloc_pop();

/*!
 @brief Invalidate a chain of blocks
 @param base The first block in the chain
 */
static inline void flog_invalidate_chain(flog_block_idx_t base);

/*!
 @brief Check for a dirty block and flush it to allow for a new allocation

 @note This requires the allocation lock
 */
static void flog_flush_dirty_block();

static flog_result_t flog_commit_file_sector(flog_write_file_t * file,
                                             uint8_t const * data,
                                             flog_sector_nbytes_t n);

//! @}


///////////////////////////////////////////////////////////////////////////////
// Public implementations
///////////////////////////////////////////////////////////////////////////////

flog_result_t flogfs_init(){
	// Initialize locks
	fs_lock_init(&flogfs.allocate_lock);
	fs_lock_init(&flogfs.lock);

	flogfs.state = FLOG_STATE_RESET;
	flogfs.cache_status.page_open = 0;
	flogfs.dirty_block.block = FLOG_BLOCK_IDX_INVALID;
	return flash_init();
}

flog_result_t flogfs_format(){
	flog_block_idx_t i;
	flog_block_idx_t first_valid = FLOG_BLOCK_IDX_INVALID;

	union {
		flog_inode_sector0_t main_buffer;
		flog_inode_sector0_spare_t spare_buffer;
	};
	flash_lock();
	flog_lock_fs();

	for(i = 0; i < FS_NUM_BLOCKS; i++){
		flog_open_page(i, 0);
		if(FLOG_SUCCESS == flash_block_is_bad()){
			continue;
		}
		// Otherwise go erase it
		if(FLOG_FAILURE == flash_erase_block(i)){
			flog_unlock_fs();
			flash_unlock();
			flash_debug_error("FLogFS:" LINESTR);
			return FLOG_FAILURE;
		}
		if(first_valid == FLOG_BLOCK_IDX_INVALID){
			first_valid = i;
		}
	}

	// Really just assuming that at least 1 valid block was found

	// Write the first file table
	flash_open_page(first_valid, 0);
	main_buffer.age = 0;
	main_buffer.timestamp = 0;
	flash_write_sector((const uint8_t *)&main_buffer,
	                   0, 0, sizeof(main_buffer));
	spare_buffer.inode_index = 0;
	spare_buffer.type_id = FLOG_BLOCK_TYPE_INODE;
	flash_write_spare((const uint8_t *)&spare_buffer, 0);
	flash_commit();

	flog_unlock_fs();
	flash_unlock();
	return FLOG_SUCCESS;
}


flog_result_t flogfs_mount(){
	uint32_t i, done_scanning;
	uint16_t block;

	////////////////////////////////////////////////////////////
	// Data structures
	////////////////////////////////////////////////////////////

	// Use in search for highest allocation timestamp
	struct {
		flog_block_idx_t block;
		flog_block_age_t age;
		flog_file_id_t file_id;
		flog_timestamp_t timestamp;
	} last_allocation;

	struct {
		flog_block_idx_t first_block, last_block;
		flog_file_id_t   file_id;
		flog_timestamp_t timestamp;
	} last_deletion;

	// Find the freshest block to allocate. Why not?
	struct {
		flog_block_idx_t block;
		flog_block_age_t age;
	} min_age_block;

	flog_block_idx_t inode0_idx;

	// Find the maximum block age
	flog_block_age_t max_block_age;

	flog_inode_iterator_t inode_iter;

	////////////////////////////////////////////////////////////
	// Flexible buffers for flash reads
	////////////////////////////////////////////////////////////

	flog_timestamp_t timestamp_buffer;

	union {
		uint8_t sector0_buffer;
		flog_file_sector0_header_t file_sector0_header;
		flog_inode_sector0_t inode_sector0;
		flog_inode_file_invalidation_t inode_file_invalidation_sector;
	};

	union {
		uint8_t sector_buffer;
		flog_file_tail_sector_header_t file_tail_sector_header;
		flog_inode_tail_sector_t inode_tail_sector;
		flog_inode_file_allocation_header_t inode_file_allocation_sector;
		flog_universal_invalidation_header_t universal_invalidation_header;
	};

	union {
		uint8_t spare_buffer;
		flog_inode_sector0_spare_t inode_spare0;
		flog_file_sector_spare_t file_spare0;
	};



	////////////////////////////////////////////////////////////
	// Initialize data structures
	////////////////////////////////////////////////////////////

	last_allocation.block = FLOG_BLOCK_IDX_INVALID;
	last_allocation.timestamp = 0;

	last_deletion.timestamp = 0;
	last_deletion.file_id = FLOG_FILE_ID_INVALID;

	flogfs.num_free_blocks = 0;

	min_age_block.age = 0xFFFFFFFF;
	min_age_block.block = FLOG_BLOCK_IDX_INVALID;

	inode0_idx = FLOG_BLOCK_IDX_INVALID;

	max_block_age = 0;

	////////////////////////////////////////////////////////////
	// Claim the disk and get this show started
	////////////////////////////////////////////////////////////

	flog_lock_fs();

	if(flogfs.state == FLOG_STATE_MOUNTED){
		flog_unlock_fs();
		return FLOG_SUCCESS;
	}

	flash_lock();


	////////////////////////////////////////////////////////////
	// First, iterate through all blocks to find:
	// - Most recent allocation time in a file block
	// - Number of free blocks
	// - Some free blocks that are fair to use
	// - Oldest block age
	// - Inode table 0
	////////////////////////////////////////////////////////////
	for(i = 0; i < FS_NUM_BLOCKS; i++){
		// Everything can be determined from page 0
		if(FLOG_FAILURE == flash_open_page(i, 0)){
			continue;
		}
		if(FLOG_SUCCESS == flash_block_is_bad()){
			flash_debug_warn("FLogFS:" LINESTR);
			continue;
		}
		// Read the sector 0 spare to identify valid blocks
		flash_read_spare((uint8_t *)&spare_buffer, 0);

		switch(inode_spare0.type_id) {
		case FLOG_BLOCK_TYPE_INODE:
			// Check for an invalidation timestamp
			flash_read_sector((uint8_t *)&timestamp_buffer,
			                  FLOG_INODE_INVALIDATION_SECTOR, 0,
			                  sizeof(flog_timestamp_t));
			flash_read_sector(&sector0_buffer, 0, 0,
			                  sizeof(flog_inode_sector0_t));
			if(timestamp_buffer == FLOG_TIMESTAMP_INVALID){
				// This thing is still valid
				if(inode_spare0.inode_index == 0){
					// Found the original gangster!
					inode0_idx = i;
				} else {
					// Not the first, but valid!
				}
			} else {
				// YOU FOUND AN INVALIDATED INODE
				// Deal with it...
				// Count it as free?
			}
			// Check if this is a really old block
			if(inode_sector0.age > max_block_age){
				max_block_age = inode_sector0.age;
			}
			break;
		case FLOG_BLOCK_TYPE_FILE:
			flash_read_sector(&sector_buffer,
			                  FLOG_FILE_TAIL_SECTOR, 0,
			                  sizeof(flog_file_tail_sector_header_t));
			flash_read_sector(&sector0_buffer, 0, 0, sizeof(flog_file_sector0_header_t));
			if(file_tail_sector_header.timestamp == FLOG_TIMESTAMP_INVALID){
				// This is the last allocated block for whatever that file is
				// That is pointless
			} else if(file_tail_sector_header.timestamp >
			          last_allocation.timestamp){
				// This is now the most recent allocation timestamp!
				last_allocation.timestamp = file_tail_sector_header.timestamp;
				last_allocation.block = file_tail_sector_header.next_block;
				last_allocation.age = file_tail_sector_header.next_age;
				last_allocation.file_id = file_sector0_header.file_id;
			}
			// Check if this block is really old
			if(file_sector0_header.age > max_block_age){
				max_block_age = file_sector0_header.age;
			}
			break;
		case FLOG_BLOCK_TYPE_UNALLOCATED:
			flogfs.num_free_blocks += 1;
			break;
		default:
			flash_debug_error("FLogFS:" LINESTR);
			goto failure;
		}

		// Check for invalidated blocks
		if((inode_spare0.type_id == FLOG_BLOCK_TYPE_FILE) ||
		   (inode_spare0.type_id == FLOG_BLOCK_TYPE_INODE)){
			flash_read_sector(&sector_buffer,
			                  FLOG_FILE_INVALIDATION_SECTOR, 0,
			                  sizeof(flog_universal_invalidation_header_t));
			if(universal_invalidation_header.timestamp !=
			   FLOG_TIMESTAMP_INVALID){
				flogfs.num_free_blocks += 1;
			}
		}
	}



	if(inode0_idx == FLOG_BLOCK_IDX_INVALID){
		flash_debug_error("FLogFS:" LINESTR);
		goto failure;
	}

	////////////////////////////////////////////////////////////
	// Now iterate through the inode chain, finding:
	// - Most recent file deletion
	// - Most recent file allocation
	// - Max file ID
	////////////////////////////////////////////////////////////

	done_scanning = 0;
	block = inode0_idx; // Inode block
	for(flog_inode_iterator_init(&inode_iter, inode0_idx);;
		flog_inode_iterator_next(&inode_iter)){
		flog_open_sector(inode_iter.block, inode_iter.sector);
		flash_read_sector(&sector_buffer, inode_iter.sector, 0,
		                  sizeof(flog_inode_file_allocation_header_t));
		if(inode_file_allocation_sector.file_id == FLOG_FILE_ID_INVALID){
			// Passed the last file
			break;
		}
		flog_open_sector(inode_iter.block, inode_iter.sector + 1);
		flash_read_sector(&sector0_buffer, inode_iter.sector + 1, 0,
		                  sizeof(flog_inode_file_invalidation_t));

		// Keep track of the maximum file ID
		// Since these are allocated sequentially, this has to be the latest
		// ... so far ...
		flogfs.max_file_id = inode_file_allocation_sector.file_id;

		// Was it deleted?
		if(inode_file_invalidation_sector.timestamp ==
			FLOG_TIMESTAMP_INVALID){
			// This is still valid

			// Check if this is now the most recent allocation
			if(inode_file_allocation_sector.timestamp >
			   last_allocation.timestamp){
				// This isn't really always true becase we also consider
				// allocations in the file chain itself, which are not
				// reflected
				last_allocation.block =
				  inode_file_allocation_sector.first_block;
				last_allocation.file_id =
				  inode_file_allocation_sector.file_id;
				last_allocation.age =
				  inode_file_allocation_sector.first_block_age;
				last_allocation.timestamp =
				  inode_file_allocation_sector.timestamp;
			}
		} else {
			// Check if this was the most recent deletion
			if(inode_file_invalidation_sector.timestamp >
			   last_deletion.timestamp){
				last_deletion.first_block =
				  inode_file_allocation_sector.first_block;
				last_deletion.last_block =
				  inode_file_invalidation_sector.last_block;
				last_deletion.file_id =
				  inode_file_allocation_sector.file_id;
				last_deletion.timestamp =
				  inode_file_invalidation_sector.timestamp;
			}
		}
	}

	// Go check and (maybe) clean the last allocation
	if(last_allocation.timestamp > 0){
		flog_open_sector(last_allocation.block, 0);
		flash_read_sector(&sector0_buffer, 0, 0,
		                  sizeof(flog_file_sector0_header_t));
		if(file_sector0_header.file_id != last_allocation.file_id){
			// This block never got allocated
			// Erase and initialize it!
			flash_erase_block(last_allocation.block);
			flog_open_page(last_allocation.block, 0);
			file_sector0_header.age = last_allocation.age;
			file_sector0_header.file_id = last_allocation.file_id;
			flash_write_sector(&sector0_buffer, 0, 0,
			                   sizeof(flog_file_sector0_header_t));
			file_spare0.nbytes = 0;
			file_spare0.nothing = 0;
			file_spare0.type_id = FLOG_BLOCK_TYPE_FILE;
			flash_write_spare(&spare_buffer, 0);
			flash_commit();

			flogfs.t = last_allocation.timestamp + 1;
		}
	}

	// Verify the completion of the most recent deletion operation
	if(last_deletion.timestamp > 0){
		flog_open_sector(last_deletion.last_block, 0);
		flash_read_sector(&sector0_buffer, 0, 0,
		                  sizeof(flog_file_sector0_header_t));
		if(file_sector0_header.file_id == last_deletion.file_id){
			// This is the same file still, see if it's been invalidated
			flog_open_sector(last_deletion.last_block,
			                 FLOG_FILE_INVALIDATION_SECTOR);
			flash_read_sector(&sector_buffer, 0, 0,
			                  sizeof(flog_universal_invalidation_header_t));
			if(universal_invalidation_header.timestamp != FLOG_TIMESTAMP_INVALID){
				// Crap, this never got invalidated correctly
				flog_invalidate_chain(last_deletion.first_block);
				flash_debug_warn("FLogFS:" LINESTR);
			}
		}
	}

	flogfs.state = FLOG_STATE_MOUNTED;

	flash_unlock();
	flog_unlock_fs();
	return FLOG_SUCCESS;

failure:
	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}



flog_result_t flogfs_open_read(flog_read_file_t * file, char const * filename){
	flog_inode_iterator_t inode_iter;
	flog_read_file_t * file_iter;
	flog_file_find_result_t find_result;

	union {
		uint8_t sector_buffer;;
		flog_inode_file_allocation_t inode_file_allocation_sector;
		flog_inode_file_invalidation_t inode_file_invalidation_sector;
		flog_file_sector0_header_t file_sector0_header;
	};
	union {
		uint8_t spare_buffer;
		flog_file_sector_spare_t file_sector_spare;
	};

	if(strlen(filename) >= FLOG_MAX_FNAME_LEN){
		return FLOG_FAILURE;
	}

	flog_lock_fs();
	flash_lock();

	find_result = flog_find_file(filename, &inode_iter);
	if(find_result.first_block == FLOG_BLOCK_IDX_INVALID){
		// File doesn't exist
		goto failure;
	}

	file->block = find_result.first_block;
	file->id = find_result.file_id;
	/////////////
	// Actual file search
	/////////////


	// Now go find the start of file data (either first or second sector)
	// and adjust some settings
	flog_open_sector(file->block, 0);
	flash_read_spare(&spare_buffer, 0);

	if(file_sector_spare.nbytes != 0){
		// The first sector has some stuff in it!
		file->sector = 0;
		file->offset = sizeof(flog_file_sector0_header_t);
	} else {
		flog_open_sector(file->block, 1);
		flash_read_spare(&spare_buffer, 1);
		file->sector = 1;
		file->offset = 0;
	}

	file->sector_remaining_bytes = file_sector_spare.nbytes;

	// If we got this far...

	// Add to list of read files
	file->next = 0;
	if(flogfs.read_head){
		// Iterate to end of list
		for(file_iter = flogfs.read_head; file_iter->next;
			file_iter = file_iter->next);
		file_iter->next = file;
	} else {
		flogfs.read_head = file;
	}

	flash_unlock();
	flog_unlock_fs();
	return FLOG_SUCCESS;


failure:
	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}

flog_result_t flogfs_close_read(flog_read_file_t * file){
	flog_read_file_t * iter;
	flog_lock_fs();
	if(flogfs.read_head == file){
		flogfs.read_head = 0;
	} else {
		iter = flogfs.read_head;
		while(iter->next){
			if(iter->next == file){
				iter->next = file->next;
				break;
			}
			iter = iter->next;
		}
		goto failure;
	}
	flog_unlock_fs();
	return FLOG_SUCCESS;

failure:
	flog_unlock_fs();
	return FLOG_FAILURE;
}

uint32_t flogfs_read(flog_read_file_t * file, uint8_t * dst, uint32_t nbytes){
	uint32_t count = 0;
	uint16_t to_read;

	flog_block_idx_t block;
	uint16_t sector;

	union {
		uint8_t sector_header;
		flog_file_tail_sector_header_t file_tail_sector_header;
		flog_file_sector0_header_t file_sector0_header;
	};

	union {
		uint8_t sector_spare;
		flog_file_sector_spare_t file_sector_spare;
	};

	flog_lock_fs();
	flash_lock();

	while(nbytes){
		if(file->sector_remaining_bytes == 0){
			// We are/were at the end of file, look into the existence of new data
			// This block is responsible for setting:
			// -- file->sector_remaining_bytes
			// -- file->offset
			// -- file->sector
			// -- file->block
			// and bailing on the loop if EOF is encountered
			if(file->sector == FLOG_FILE_TAIL_SECTOR){
				// This was the last sector in the block, check the next
				flog_open_sector(file->block, FLOG_FILE_TAIL_SECTOR);
				flash_read_sector(&sector_header, FLOG_FILE_TAIL_SECTOR, 0,
								sizeof(flog_file_tail_sector_header_t));
				block = file_tail_sector_header.next_block;
				// Now check out that new block and make sure it's legit
				flog_open_sector(block, 0);
				flash_read_sector(&sector_header, 0, 0,
								sizeof(flog_file_sector0_header_t));
				if(file_sector0_header.file_id != file->id){
					// This next block hasn't been written. EOF for now
					goto done;
				}

				file->block = block;

				flash_read_spare(&sector_spare, 0);
				if(file_sector_spare.nbytes == 0){
					// It's possible for the first sector to have 0 bytes
					// Data is in next sector
					file->sector = 1;
				} else {
					file->sector = 0;
				}
			} else {
				// Increment to next sector but don't necessarily update file
				// state
				sector = flog_increment_sector(file->sector);

				flog_open_sector(file->block, sector);
				flash_read_spare(&sector_spare, sector);

				if(file_sector_spare.nbytes == FLOG_SECTOR_NBYTES_INVALID){
					// We're looking at an empty sector, GTFO
					goto done;
				} else {
					file->sector = sector;
				}
			}

			file->sector_remaining_bytes = file_sector_spare.nbytes;
			switch(file->sector){
			case FLOG_FILE_TAIL_SECTOR:
				file->offset = sizeof(flog_file_tail_sector_header_t);
				break;
			case 0:
				file->offset = sizeof(flog_file_sector0_header_t);
				break;
			default:
				file->offset = 0;
			}
		}

		// Figure out how many to read
		to_read = MIN(nbytes, file->sector_remaining_bytes);

		// Read this sector now
		flog_open_sector(file->block, file->sector);
		flash_read_sector(dst, file->sector, file->offset, to_read);
		count += to_read;
		nbytes -= to_read;
		dst += to_read;
		// Update file stats
		file->offset += to_read;
		file->sector_remaining_bytes -= to_read;
		file->read_head += to_read;
	}


done:
	flash_unlock();
	flog_unlock_fs();

	return count;
}

uint32_t flogfs_write(flog_write_file_t * file, uint8_t const * src,
                      uint32_t nbytes){
	uint32_t count = 0;

	flog_lock_fs();
	flash_lock();

	while(nbytes){
		if(nbytes >= file->sector_remaining_bytes){
			if(flog_commit_file_sector(file, src,
				file->sector_remaining_bytes) == FLOG_FAILURE){
				// Couldn't allocate or something
				return count;
			}

			// Now that sector is completely written
			src += file->sector_remaining_bytes;
			nbytes -= file->sector_remaining_bytes;
			count += file->sector_remaining_bytes;
		} else {
			// This is smaller than a sector; cache it
			memcpy(file->sector_buffer + file->offset, src, nbytes);
			count += nbytes;
			file->sector_remaining_bytes -= nbytes;
			file->offset += nbytes;
			file->bytes_in_block += nbytes;
			file->write_head += nbytes;
			nbytes = 0;
		}
	}


	flash_unlock();
	flog_unlock_fs();

	return count;
}

flog_result_t flogfs_seek(flog_read_file_t * file, uint32_t index){
	return FLOG_FAILURE;
}

flog_result_t flogfs_open_write(flog_write_file_t * file, char const * filename){
	flog_inode_iterator_t inode_iter;
	flog_block_alloc_t alloc_block;
	flog_file_find_result_t find_result;

	union {
		uint8_t sector_buffer;;
		flog_inode_file_allocation_t inode_file_allocation_sector;
		flog_file_sector0_header_t file_sector0_header;
		flog_file_tail_sector_header_t file_tail_sector_header;
	};
	union {
		uint8_t spare_buffer;
		flog_file_sector_spare_t file_sector_spare;
	};

	flog_lock_fs();
	flash_lock();

	find_result = flog_find_file(filename, &inode_iter);

	if(find_result.first_block != FLOG_BLOCK_IDX_INVALID){
		// TODO: Make sure file isn't already open for writing

		// File already exists
		file->block = find_result.first_block;
		file->id = find_result.file_id;
		file->sector = 0;
		// Count bytes from 0
		file->write_head = 0;
		// Iterate to the end of the file
		// First check each terminated block
		while(1){
			flog_open_sector(file->block, FLOG_FILE_TAIL_SECTOR);
			flash_read_sector(&sector_buffer, FLOG_FILE_TAIL_SECTOR, 0,
			                  sizeof(flog_file_tail_sector_header_t));
			if(file_tail_sector_header.timestamp == FLOG_TIMESTAMP_INVALID){
				// This block is incomplete
				break;
			}
			file->block = file_tail_sector_header.next_block;
			file->write_head += file_tail_sector_header.bytes_in_block;
		}
		// Now file->block is the first incomplete block
		// Scan it sector-by-sector

		// Check out sector 0 no matter what and move on. It might have no data
		flog_open_sector(file->block, 0);
		flash_read_spare(&spare_buffer, 0);
		file->write_head += file_sector_spare.nbytes;
		file->sector = flog_increment_sector(file->sector);
		while(1){
			// For each block in the file
			flog_open_sector(file->block, file->sector);
			flash_read_spare(&spare_buffer, file->sector);
			if(file_sector_spare.nbytes == FLOG_SECTOR_NBYTES_INVALID){
				// No data
				// We will write here!
				if(file->sector == FLOG_FILE_TAIL_SECTOR){
					file->offset = sizeof(flog_file_tail_sector_header_t);
				} else {
					file->offset = 0;
				}
				file->sector_remaining_bytes = FS_SECTOR_SIZE - file->offset;
				break;
			}
			file->write_head += file_sector_spare.nbytes;
			file->sector = flog_increment_sector(file->sector);
		}
	} else {
		// File doesn't exist

		// Get a new inode entry
		if(flog_inode_prepare_new(&inode_iter) != FLOG_SUCCESS){
			// Somehow couldn't allocate an inode entry
			goto failure;
		}

		// Configure inode to write
		strcpy(inode_file_allocation_sector.filename, filename);
		inode_file_allocation_sector.filename[FLOG_MAX_FNAME_LEN-1] = '\0';

		flog_lock_allocate();

		flog_flush_dirty_block();

		alloc_block = flog_allocate_block();
		if(alloc_block.block == FLOG_BLOCK_IDX_INVALID){
			flog_unlock_allocate();
			// Couldn't allocate a block
			goto failure;
		}

		flogfs.dirty_block.block = alloc_block.block;
		flogfs.dirty_block.file = file;

		flog_unlock_allocate();

		inode_file_allocation_sector.header.file_id = ++flogfs.max_file_id;
		inode_file_allocation_sector.header.first_block = alloc_block.block;
		inode_file_allocation_sector.header.first_block_age = ++alloc_block.age;
		inode_file_allocation_sector.header.timestamp = ++flogfs.t;

		// Write the new inode entry
		flog_open_sector(inode_iter.block,inode_iter.sector);
		flash_write_sector(&sector_buffer, inode_iter.sector, 0,
		                   sizeof(flog_inode_file_allocation_t));
		flash_commit();

		// Now safe to erase block after writing header
		flash_erase_block(alloc_block.block);

		file->block = alloc_block.block;
		file->block_age = alloc_block.age;
		file->id = flogfs.max_file_id;
		file->bytes_in_block = 0;
		file->write_head = 0;
		file->sector = 0;
		file->offset = sizeof(flog_file_sector0_header_t);
		file->sector_remaining_bytes = FS_SECTOR_SIZE -
		                               sizeof(flog_file_sector0_header_t);
	}

	// Add it to that list
	file->next = 0;
	if(flogfs.write_head == 0){
		flogfs.write_head = file;
	} else {
		flog_write_file_t * file_iter;
		for(file_iter = flogfs.write_head;
		    file_iter->next;
		    file_iter = file_iter->next);
		file_iter->next = file;
	}

	flash_unlock();
	flog_unlock_fs();

	return FLOG_SUCCESS;

failure:
	flash_unlock();
	flog_unlock_fs();

	return FLOG_FAILURE;
}

/*!
 @details
 ### Internals
 To close a write, all outstanding data must simply be flushed to flash. If any
 blocks are newly-allocated, they must be committed.

 TODO: Deal with files that can't be flushed due to no space for allocation
 */
flog_result_t flogfs_close_write(flog_write_file_t * file){
	flog_write_file_t * iter;
	flog_result_t result;


	flog_lock_fs();
	flash_lock();

	if(flogfs.write_head == file){
		flogfs.write_head = 0;
	} else {
		iter = flogfs.write_head;
		while(iter->next){
			if(iter->next == file){
				iter->next = file->next;
				break;
			}
			iter = iter->next;
		}
		goto failure;
	}

	result = flog_flush_write(file);

	flash_unlock();
	flog_unlock_fs();

	return result;

failure:

	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}

flog_result_t flogfs_rm(char const * filename){
	flog_file_find_result_t find_result;
	flog_inode_iterator_t inode_iter;
	flog_block_idx_t block, next_block;

	union {
		uint8_t sector_buffer;
		flog_inode_file_invalidation_t invalidation_buffer;
	};

	flog_lock_fs();
	flash_lock();

	find_result = flog_find_file(filename, &inode_iter);

	if(find_result.first_block == FLOG_BLOCK_IDX_INVALID){
		// Cool! The file already doesn't exist.
		// No work to be done here.
		goto failure;
	}

	// Navigate to the end to find the last block
	block = find_result.first_block;
	while(1){
		flog_open_sector(block, FLOG_FILE_TAIL_SECTOR);
		flash_read_sector((uint8_t*)&next_block, FLOG_FILE_TAIL_SECTOR, 0,
		                  sizeof(flog_block_idx_t));
		if(next_block == FLOG_BLOCK_IDX_INVALID){
			// THIS IS THE LAST BLOCK
			break;
		}
		block = next_block;
	}

	// Invalidate the inode entry
	invalidation_buffer.last_block = block;
	invalidation_buffer.timestamp = ++flogfs.t;
	flog_open_sector(inode_iter.block, inode_iter.sector + 1);
	flash_write_sector(&sector_buffer, inode_iter.sector + 1, 0,
	                   sizeof(flog_inode_file_invalidation_t));

	// A disk failure here can be recovered in mounting

	// Invalidate the file block chain
	flog_invalidate_chain(find_result.first_block);

	flash_unlock();
	flog_unlock_fs();
	return FLOG_SUCCESS;

failure:
	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}





void flogfs_start_ls(flogfs_ls_iterator_t * iter){
	// TODO: Lock something?

	flog_inode_iterator_init(iter, flogfs.inode0);
}

uint_fast8_t flogfs_ls_iterate(flogfs_ls_iterator_t * iter, char * fname_dst){
	union {
		uint8_t sector_buffer;
		flog_file_id_t file_id;
		flog_timestamp_t timestamp;
	};
	while(1){
		flog_open_sector(iter->block, iter->sector);
		flash_read_sector(&sector_buffer, iter->sector,
		                  0, sizeof(flog_file_id_t));
		if(file_id == FLOG_FILE_ID_INVALID){
			// Nothing here. Done.
			return 0;
		}
		// Now check to see if it's valid
		flog_open_sector(iter->block, iter->sector + 1);
		flash_read_sector(&sector_buffer, iter->sector+1,
		                  0, sizeof(flog_timestamp_t));
		if(timestamp == FLOG_TIMESTAMP_INVALID){
			// This file's good
			// Now check to see if it's valid
			// Go read the filename
			flog_open_sector(iter->block, iter->sector);
			flash_read_sector((uint8_t *)fname_dst, iter->sector,
			                  sizeof(flog_inode_file_allocation_header_t),
							  FLOG_MAX_FNAME_LEN);
			fname_dst[FLOG_MAX_FNAME_LEN-1] = '\0';
			flog_inode_iterator_next(iter);
			return 1;
		} else {
			flog_inode_iterator_next(iter);
		}
	}
}

void flogfs_stop_ls(flogfs_ls_iterator_t * iter){
	// TODO: Unlock something?
}



///////////////////////////////////////////////////////////////////////////////
// Static implementations
///////////////////////////////////////////////////////////////////////////////

flog_result_t flog_commit_file_sector(flog_write_file_t * file,
                                      uint8_t const * data,
                                      flog_sector_nbytes_t n){
	flog_file_sector_spare_t file_sector_spare;
	if(file->sector == FLOG_FILE_TAIL_SECTOR){
		flog_block_alloc_t next_block;
		flog_file_tail_sector_header_t * const file_tail_sector_header =
		   (flog_file_tail_sector_header_t *) file->sector_buffer;

		flog_lock_allocate();

		flog_flush_dirty_block();

		next_block = flog_allocate_block();
		if(next_block.block == FLOG_BLOCK_IDX_INVALID){
			// Can't write the last sector without sealing the file.
			// Bailing
			flog_unlock_allocate();
			return FLOG_FAILURE;
		}

		flogfs.dirty_block.block = next_block.block;
		flogfs.dirty_block.file = file;

		flog_unlock_allocate();

		// Prepare the header
		file_tail_sector_header->next_age = next_block.age + 1;
		file_tail_sector_header->next_block = next_block.block;
		file_tail_sector_header->timestamp = ++flogfs.t;
		file->bytes_in_block +=
			FS_SECTOR_SIZE - sizeof(flog_file_tail_sector_header_t);
		file_sector_spare.nbytes =
			FS_SECTOR_SIZE - sizeof(flog_file_tail_sector_header_t);
		file_tail_sector_header->bytes_in_block = file->bytes_in_block;

		flog_open_sector(file->block, FLOG_FILE_TAIL_SECTOR);
		// First write what was already buffered (and the header)
		flash_write_sector((uint8_t const *)file_tail_sector_header,
						FLOG_FILE_TAIL_SECTOR, 0, file->offset);
		// Now write the rest of the data
		flash_write_sector(data, FLOG_FILE_TAIL_SECTOR, file->offset,
							FS_SECTOR_SIZE - file->offset);
		flash_write_spare((uint8_t const *)&file_sector_spare, FLOG_FILE_TAIL_SECTOR);
		flash_commit();

		// Ready the file structure for the next block/sector
		file->block = next_block.block;
		file->block_age = next_block.age;
		file->sector = 0;
		file->sector_remaining_bytes =
		FS_SECTOR_SIZE - sizeof(flog_file_sector0_header_t);
		file->bytes_in_block = 0;
		file->offset = sizeof(flog_file_sector0_header_t);
		file->write_head += n;
		return FLOG_SUCCESS;
	} else {

		flog_file_sector0_header_t * const file_sector0_header =
			(flog_file_sector0_header_t *) file->sector_buffer;

		flog_lock_allocate();
		// So if this block is the dirty block...
		if(flogfs.dirty_block.file == file){
			flogfs.dirty_block.block = FLOG_BLOCK_IDX_INVALID;
		}
		flog_unlock_allocate();
		file_sector_spare.type_id = FLOG_BLOCK_TYPE_FILE;
		file_sector_spare.nbytes = file->offset + n;

		// We need to just write the data and advance
		if(file->sector == 0){
			// Need to prepare sector 0 header
			file_sector0_header->file_id = file->id;
			file_sector0_header->age = file->block_age;
		}

		flog_open_sector(file->block, file->sector);
		if(file->offset){
			// This is either sector 0 or there was data already
			// First write prior data/header
			flash_write_sector(file->sector_buffer, file->sector, 0, file->offset);
		}
		flash_write_sector(data, file->sector, file->offset, n);
		flash_write_spare((uint8_t const *)&file_sector_spare, file->sector);
		flash_commit();

		// Now update stuff for the new sector
		file->sector = flog_increment_sector(file->sector);
		if(file->sector == FLOG_FILE_TAIL_SECTOR){
			file->offset = sizeof(flog_file_tail_sector_header_t);
		} else {
			file->offset = 0;
		}
		file->bytes_in_block += n;
		file->sector_remaining_bytes = FS_SECTOR_SIZE - file->offset;
		file->write_head += n;
		return FLOG_SUCCESS;
	}
}

flog_result_t flog_flush_write (flog_write_file_t * file ){
	return flog_commit_file_sector(file, 0, 0);
}


void flog_prealloc_iterate() {
	flog_block_alloc_t block;
	block = flog_allocate_block_iterate();
	flog_prealloc_push(block.block, block.age);
}


flog_block_alloc_t flog_allocate_block_iterate(){
	flog_block_alloc_t block;
	union {
		uint8_t sector0_buffer;
		flog_universal_sector0_header_t universal_sector0_header;
	};

	union {
		uint8_t invalidation_buffer;
		flog_universal_invalidation_header_t universal_invalidation_header;
	};

	block.block = FLOG_BLOCK_IDX_INVALID;

	// TODO: First check if this block is active in the FS
	// No biggie since dirty writes are flushed first anyways

	// First check if there is data
	flog_open_sector(flogfs.allocate_head, 0);
	flash_read_sector(&sector0_buffer, 0, 0,
	                  sizeof(flog_universal_sector0_header_t));
	if(universal_sector0_header.age == FLOG_BLOCK_AGE_INVALID){
		// Never been allocated!
		block.block = flogfs.allocate_head;
		block.age = 0;
	} else {
		// Check if the block is free
		flog_open_sector(flogfs.allocate_head, FLOG_FILE_INVALIDATION_SECTOR);
		flash_read_sector(&invalidation_buffer, FLOG_FILE_INVALIDATION_SECTOR,
		                  0, sizeof(flog_universal_invalidation_header_t));
		if(universal_invalidation_header.timestamp != FLOG_TIMESTAMP_INVALID){
			// This has been invalidated
			block.age = universal_sector0_header.age;
			block.block = flogfs.allocate_head;
		}
	}

	// Move the head
	flogfs.allocate_head = (flogfs.allocate_head + 1) % FS_NUM_BLOCKS;

	return block;
}

static void flog_prealloc_push(flog_block_idx_t block,
                               flog_block_age_t age){
	if(flogfs.prealloc.n == 0){
		flogfs.prealloc.blocks[0].block = block;
		flogfs.prealloc.blocks[0].age = age;
		flogfs.prealloc.n = 1;
		flogfs.prealloc.age_sum += age;
		return;
	}

	if((flogfs.prealloc.n == FS_PREALLOCATE_SIZE) &&
	   (flogfs.prealloc.blocks[flogfs.prealloc.n - 1].age < age)){
		// This block sucks
		return;
	}
	for(flog_block_idx_t i = flogfs.prealloc.n - 1; i; i--){
		// Search for the right place (start at end)
		if(age <= flogfs.prealloc.blocks[i].age){
			if(flogfs.prealloc.n == FS_PREALLOCATE_SIZE){
				// Last block will be discarded
				flogfs.prealloc.age_sum -=
				  flogfs.prealloc.blocks[FS_PREALLOCATE_SIZE - 1].age;
			}
			for(flog_block_idx_t j = MIN(flogfs.prealloc.n,
			                             FS_PREALLOCATE_SIZE - 1);
			    j > i; j--){
				// Shift all older blocks
				flogfs.prealloc.blocks[j].age =
				   flogfs.prealloc.blocks[j - 1].age;
				flogfs.prealloc.blocks[j].block =
				   flogfs.prealloc.blocks[j - 1].block;
			}
			if(flogfs.prealloc.n < FS_PREALLOCATE_SIZE){
				flogfs.prealloc.n += 1;
			}
			flogfs.prealloc.blocks[i].age = age;
			flogfs.prealloc.blocks[i].block = block;

			flogfs.prealloc.age_sum += age;
			return;
		}
	}
}

static flog_block_alloc_t flog_prealloc_pop() {
	flog_block_alloc_t block;
	if(flogfs.prealloc.n == 0){
		block.block = FLOG_BLOCK_IDX_INVALID;
		return block;
	}
	block.block = flogfs.prealloc.blocks[0].block;
	block.age = flogfs.prealloc.blocks[0].age;

	// Shift all of the other entries forward
	flogfs.prealloc.n -= 1;
	for(flog_block_idx_t i = 0; i < flogfs.prealloc.n; i++){
		flogfs.prealloc.blocks[i].age = flogfs.prealloc.blocks[i + 1].age;
		flogfs.prealloc.blocks[i].block = flogfs.prealloc.blocks[i + 1].block;
	}
	return block;
}

static flog_result_t flog_open_page(uint16_t block, uint16_t page){
	if(flogfs.cache_status.page_open &&
	   (flogfs.cache_status.current_open_block == block) &&
	   (flogfs.cache_status.current_open_page == page)){
		return flogfs.cache_status.page_open_result;
	}
	flogfs.cache_status.page_open_result = flash_open_page(block, page);
	flogfs.cache_status.page_open = 1;
	flogfs.cache_status.current_open_block = block;
	flogfs.cache_status.current_open_page = page;

	return flogfs.cache_status.page_open_result;
}

flog_result_t flog_open_sector(uint16_t block, uint16_t sector){
	return flog_open_page(block, sector / FS_SECTORS_PER_PAGE);
}

void flog_close_sector(){
	flogfs.cache_status.page_open = 0;
}

flog_block_idx_t
flog_universal_get_next_block(flog_block_idx_t block){
	if(block == FLOG_BLOCK_IDX_INVALID)
		return block;
	flog_open_sector(block, FLOG_FILE_TAIL_SECTOR);
	flash_read_sector((uint8_t*)&block, FLOG_FILE_TAIL_SECTOR,
	                  0, sizeof(block));
	return block;
}


void flog_inode_iterator_init(flog_inode_iterator_t * iter,
                                     flog_block_idx_t inode0){
	union{
		uint8_t spare_buffer;
		flog_inode_sector0_spare_t inode_sector0_spare;
	};
	iter->block = inode0;
	flog_open_sector(inode0, FLOG_INODE_TAIL_SECTOR);
	flash_read_sector((uint8_t *)&iter->next_block, FLOG_INODE_TAIL_SECTOR, 0,
	                  sizeof(flog_block_idx_t));
	// Get the current inode block index
	flog_open_sector(inode0, 0);
	flash_read_spare(&spare_buffer, 0);
	iter->inode_block_idx = inode_sector0_spare.inode_index;

	// This is zero anyways
	iter->inode_idx = 0;
	iter->sector = FS_SECTORS_PER_PAGE;
}

/*!
 @details
 ### Internals
 Inode entries are organized sequentially in pairs of sectors following the
 first page. The first page contains simple header information. To iterate to
 the next entry, we simply advance by two sectors. If this goes past the end of
 the block, the next block is checked. If the next block hasn't yet been
 allocated, the sector is set to invalid to indicate that the next block has to
 be allocated using flog_inode_prepare_new().
 */
void flog_inode_iterator_next(flog_inode_iterator_t * iter){
	iter->sector += 2;
	iter->inode_idx += 1;
	if(iter->sector >= FS_SECTORS_PER_BLOCK){
		// The next sector is in ANOTHER BLOCK!!!
		if(iter->next_block != FLOG_BLOCK_IDX_INVALID){
			// The next block actually already exists
			iter->block = iter->next_block;
			// Check the next block
			iter->next_block = flog_universal_get_next_block(iter->block);

			// Point to the first inode sector of the next block
			iter->sector = FS_SECTORS_PER_PAGE;
		} else {
			// The next doesn't exist
			// WTF?
			flash_debug_warn("FLogFS:" LINESTR);
			// Don't do anything; this is dumb
			iter->sector -= 2;
			iter->sector -= 1;
		}
	}
}

flog_result_t flog_inode_prepare_new (flog_inode_iterator_t * iter) {
	flog_block_alloc_t block_alloc;
	union{
		uint8_t sector_buffer;
		flog_inode_tail_sector_t inode_tail_sector;
		flog_inode_sector0_t inode_sector0;
		flog_inode_sector0_spare_t inode_sector0_spare;
	};
	if(iter->sector == FS_SECTORS_PER_PAGE - 2){
		if(iter->next_block != FLOG_BLOCK_IDX_INVALID){
			flash_debug_warn("FLogFS:" LINESTR);
		}

		// We are at the last entry of the inode block
		// This entry is valid and will be used but now is the time to allocate
		// the next block

		flog_lock_allocate();

		flog_flush_dirty_block();

		block_alloc = flog_allocate_block();
		if(block_alloc.block == FLOG_BLOCK_IDX_INVALID){
			// Couldn't allocate a new block!
			flog_unlock_allocate();
			return FLOG_FAILURE;
		}

		flog_unlock_allocate();

		// Go write the tail sector
		flog_open_sector(iter->block, FLOG_INODE_TAIL_SECTOR);
		inode_tail_sector.next_age = block_alloc.age + 1;
		inode_tail_sector.next_block = block_alloc.block;
		inode_tail_sector.timestamp = ++flogfs.t;
		flash_write_sector(&sector_buffer, FLOG_INODE_TAIL_SECTOR, 0,
		                   sizeof(flog_inode_tail_sector_t));
		flash_commit();

		// Now erase the new block
		flash_erase_block(block_alloc.block);

		// And prepare the header
		flog_open_sector(block_alloc.block, 0);
		inode_sector0.age = block_alloc.age + 1;
		inode_sector0.timestamp = flogfs.t;
		flash_write_sector(&sector_buffer, 0, 0, sizeof(flog_inode_sector0_t));
		inode_sector0_spare.type_id = FLOG_BLOCK_TYPE_INODE;
		inode_sector0_spare.inode_index = ++iter->inode_block_idx;
		flash_write_spare(&sector_buffer, 0);
		flash_commit();

		iter->next_block = block_alloc.block;
	}
	// Otherwise this is a completely okay sector to use

	return FLOG_SUCCESS;
}

void flog_invalidate_chain (flog_block_idx_t base) {
	union {
		uint8_t invalidation_sector_buffer;
		flog_file_invalidation_sector_t file_invalidation_sector;
	};
	union {
		uint8_t tail_sector_buffer;
		flog_file_tail_sector_header_t file_tail_sector_header;
	};

	while(1){
		// Stop loop after encoutering a block with no next block

		// Read the tail to see if there even is a next block
		flog_open_sector(base, FLOG_FILE_TAIL_SECTOR);
		flash_read_sector(&tail_sector_buffer, FLOG_FILE_TAIL_SECTOR, 0,
		                  sizeof(flog_file_tail_sector_header_t));

		// Also check if it's already been invalidated
		flog_open_sector(base, FLOG_FILE_INVALIDATION_SECTOR);
		flash_read_sector(&invalidation_sector_buffer,
		                  FLOG_FILE_INVALIDATION_SECTOR, 0,
		                  sizeof(flog_file_invalidation_sector_t));

		if(file_invalidation_sector.timestamp != FLOG_TIMESTAMP_INVALID){
			// This one has already been invalidated
			if(file_invalidation_sector.next_age == FLOG_BLOCK_AGE_INVALID){
				// This is the last block!
				return;
			}

			if(file_tail_sector_header.next_block == FLOG_BLOCK_IDX_INVALID){
				// This is a double check really
				return;
			}

			// Otherwise just skip this one
			base = file_tail_sector_header.next_block;
			continue;
		}

		// This block now needs invalidation; prepare that

		// This will be FLOG_BLOCK_AGE_INVALID invalid if last block
		file_invalidation_sector.next_age = file_tail_sector_header.next_age;
		file_invalidation_sector.timestamp = ++flogfs.t;

		// Check if this is already invalidated

		flog_open_sector(base, FLOG_FILE_INVALIDATION_SECTOR);
		flash_write_sector(&invalidation_sector_buffer,
		                   FLOG_FILE_INVALIDATION_SECTOR, 0,
		                   sizeof(flog_file_invalidation_sector_t));
		flash_commit();

		flogfs.num_free_blocks += 1;

		if(file_tail_sector_header.next_block == FLOG_BLOCK_IDX_INVALID){
			// No blocks left
			return;
		} else {
			// Prepare to invalidate the next one!
			base = file_tail_sector_header.next_block;
		}
	}
}


flog_block_alloc_t flog_allocate_block(){
	flog_block_alloc_t block;

	// Don't lock because that should be done at higher level

	//flog_lock_allocate();
	if(flogfs.num_free_blocks == 0){
		// No free blocks in the system. GTFO.
		block.block = FLOG_BLOCK_IDX_INVALID;
		//flog_unlock_allocate();
		return block;
	}

	block = flog_prealloc_pop();
	if(block.block != FLOG_BLOCK_IDX_INVALID){
		// Got a block! Yahtzee!
		//flog_unlock_allocate();
		return block;
	}

	// Preallocate is empty
	// Go search for another
	for(flog_block_idx_t i = FS_NUM_BLOCKS; i; i--){
		block = flog_allocate_block_iterate();
		if(block.block != FLOG_BLOCK_IDX_INVALID){
			// Found a block
			break;
		}
	}
	//flog_unlock_allocate();

	return block;
}

uint16_t flog_increment_sector(uint16_t sector){
	switch(sector){
	case FLOG_FILE_TAIL_SECTOR - 1:
		return FS_SECTORS_PER_PAGE;
	case FS_PAGES_PER_BLOCK * FS_SECTORS_PER_PAGE - 1:
		return FLOG_FILE_TAIL_SECTOR;
	default:
		return sector + 1;
	}
}

flog_file_find_result_t flog_find_file(char const * filename,
                                              flog_inode_iterator_t * iter){
	union {
		uint8_t sector_buffer;;
		flog_inode_file_allocation_t inode_file_allocation_sector;
		flog_inode_file_invalidation_t inode_file_invalidation_sector;
	};
	union {
		uint8_t spare_buffer;
		flog_file_sector_spare_t file_sector_spare;
	};

	flog_file_find_result_t result;

	for(flog_inode_iterator_init(iter, flogfs.inode0);;
	    flog_inode_iterator_next(iter)){

		/////////////
		// Inode search
		/////////////

		// Check if the entry is valid
		flog_open_sector(iter->block, iter->sector);
		flash_read_sector(&sector_buffer, iter->sector, 0,
		                  sizeof(flog_inode_file_allocation_t));

		if(inode_file_allocation_sector.header.file_id ==
		   FLOG_FILE_ID_INVALID){
			// This file is the end.
			// Do a quick check to make sure there are no foolish errors
			if(iter->next_block != FLOG_BLOCK_IDX_INVALID){
				flash_debug_warn("FLogFS:" LINESTR);
			}
			goto failure;
		}

		// Check if the name matches
		if(strncmp(filename, inode_file_allocation_sector.filename,
		   FLOG_MAX_FNAME_LEN) != 0){
			continue;
		}

		result.first_block = inode_file_allocation_sector.header.first_block;
		result.file_id = inode_file_allocation_sector.header.file_id;

		// Now check if it's been deleted
		flog_open_sector(iter->block, iter->sector+1);
		flash_read_sector(&sector_buffer, iter->sector+1, 0,
		                  sizeof(flog_timestamp_t));

		if(inode_file_invalidation_sector.timestamp != FLOG_TIMESTAMP_INVALID){
			// This one is invalid
			continue;
		}

		// This seems to be fine
		return result;
	}

failure:
	result.first_block = FLOG_BLOCK_IDX_INVALID;
	return result;
}

void flog_flush_dirty_block(){
	if(flogfs.dirty_block.block != FLOG_BLOCK_IDX_INVALID){
		flog_flush_write(flogfs.dirty_block.file);
		flogfs.dirty_block.block = FLOG_BLOCK_IDX_INVALID;
	}
}

#ifndef IS_DOXYGEN
#if !FLOG_BUILD_CPP
#ifdef __cplusplus
};
#endif
#endif
#endif
