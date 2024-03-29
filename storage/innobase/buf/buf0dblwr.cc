/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0dblwr.cc
Doublwrite buffer module

Created 2011/12/19
*******************************************************/

#include "my_securec.h"  /* Should be the first include */
#include "buf0dblwr.h"

#ifdef UNIV_NONINL
#include "buf0buf.ic"
#include "buf0dblrw.ic"
#endif

#include "buf0buf.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "trx0sys.h"

#include "log.h"

#ifndef UNIV_HOTBACKUP

#ifdef UNIV_PFS_MUTEX
/* Key to register the mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	buf_dblwr_mutex_key;
#endif /* UNIV_PFS_RWLOCK */

/** The doublewrite buffer */
UNIV_INTERN buf_dblwr_t*	buf_dblwr = NULL;

/** Set to TRUE when the doublewrite buffer is being created */
UNIV_INTERN ibool	buf_dblwr_being_created = FALSE;

/****************************************************************//**
Determines if a page number is located inside the doublewrite buffer.
@return TRUE if the location is inside the two blocks of the
doublewrite buffer */
UNIV_INTERN
ibool
buf_dblwr_page_inside(
/*==================*/
	ulint	page_no)	/*!< in: page number */
{
	if (buf_dblwr == NULL) {

		return(FALSE);
	}

	if (page_no >= buf_dblwr->block1
	    && page_no < buf_dblwr->block1
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	if (page_no >= buf_dblwr->block2
	    && page_no < buf_dblwr->block2
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	return(FALSE);
}

/****************************************************************//**
Calls buf_page_get() on the TRX_SYS_PAGE and returns a pointer to the
doublewrite buffer within it.
@return	pointer to the doublewrite buffer within the filespace header
page. */
UNIV_INLINE
byte*
buf_dblwr_get(
/*==========*/
	mtr_t*	mtr)	/*!< in/out: MTR to hold the page latch */
{
	buf_block_t*	block;

	block = buf_page_get(TRX_SYS_SPACE, 0, TRX_SYS_PAGE_NO,
			     RW_X_LATCH, mtr);
	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	return(buf_block_get_frame(block) + TRX_SYS_DOUBLEWRITE);
}

/********************************************************************//**
Flush a batch of writes to the datafiles that have already been
written to the dblwr buffer on disk. */
UNIV_INLINE
void
buf_dblwr_sync_datafiles()
/*======================*/
{
	/* Wake possible simulated aio thread to actually post the
	writes to the operating system */
	os_aio_simulated_wake_handler_threads();

	/* Wait that all async writes to tablespaces have been posted to
	the OS */
	os_aio_wait_until_no_pending_writes();

	/* Now we flush the data to disk (for example, with fsync) */
	fil_flush_file_spaces(FIL_TABLESPACE);
}

/****************************************************************//**
Creates or initialializes the doublewrite buffer at a database start. */
static
void
buf_dblwr_init(
/*===========*/
	byte*	doublewrite)	/*!< in: pointer to the doublewrite buf
				header on trx sys page */
{
	ulint	buf_size;

	buf_dblwr = static_cast<buf_dblwr_t*>(
		mem_zalloc(sizeof(buf_dblwr_t)));

	/* There are two blocks of same size in the doublewrite
	buffer. */
	buf_size = 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;

	/* There must be atleast one buffer for single page writes
	and one buffer for batch writes. */
	ut_a(srv_doublewrite_batch_size > 0
	     && srv_doublewrite_batch_size < buf_size);

	mutex_create(buf_dblwr_mutex_key,
		     &buf_dblwr->mutex, SYNC_DOUBLEWRITE);

	buf_dblwr->s_event = os_event_create();
	buf_dblwr->s_reserved = 0;

	buf_dblwr->block1 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK1);
	buf_dblwr->block2 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK2);

	buf_dblwr->in_use = static_cast<bool*>(
		mem_zalloc(buf_size * sizeof(bool)));

	buf_dblwr->write_buf_unaligned = static_cast<byte*>(
		ut_malloc((1 + buf_size) * UNIV_PAGE_SIZE));

	buf_dblwr->write_buf = static_cast<byte*>(
		ut_align(buf_dblwr->write_buf_unaligned,
			 UNIV_PAGE_SIZE));

	buf_dblwr->buf_block_arr = static_cast<buf_page_t**>(
		mem_zalloc(buf_size * sizeof(void*)));
}

/****************************************************************//**
Creates the doublewrite buffer to a new InnoDB installation. The header of the
doublewrite buffer is placed on the trx system header page. */
UNIV_INTERN
void
buf_dblwr_create(void)
/*==================*/
{
	buf_block_t*	block2;
	buf_block_t*	new_block;
	byte*	doublewrite;
	byte*	fseg_header;
	ulint	page_no;
	ulint	prev_page_no;
	ulint	i;
	mtr_t	mtr;

	if (buf_dblwr) {
		/* Already inited */

		return;
	}

start_again:
	mtr_start(&mtr);
	buf_dblwr_being_created = TRUE;

	doublewrite = buf_dblwr_get(&mtr);

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has already been created:
		just read in some numbers */

		buf_dblwr_init(doublewrite);

		mtr_commit(&mtr);
		buf_dblwr_being_created = FALSE;
		return;
	}

	ib_logf(IB_LOG_LEVEL_INFO,
		"Doublewrite buffer not found: creating new");

	if (buf_pool_get_curr_size()
	    < ((2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE
		+ FSP_EXTENT_SIZE / 2 + 100)
	       * UNIV_PAGE_SIZE)) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create doublewrite buffer: you must "
			"increase your buffer pool size. Cannot continue "
			"operation.");

		exit(EXIT_FAILURE);
	}

	block2 = fseg_create(TRX_SYS_SPACE, TRX_SYS_PAGE_NO,
			     TRX_SYS_DOUBLEWRITE
			     + TRX_SYS_DOUBLEWRITE_FSEG, &mtr);

	/* fseg_create acquires a second latch on the page,
	therefore we must declare it: */

	buf_block_dbg_add_level(block2, SYNC_NO_ORDER_CHECK);

	if (block2 == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create doublewrite buffer: you must "
			"increase your tablespace size. "
			"Cannot continue operation.");

		/* We exit without committing the mtr to prevent
		its modifications to the database getting to disk */

		exit(EXIT_FAILURE);
	}

	fseg_header = doublewrite + TRX_SYS_DOUBLEWRITE_FSEG;
	prev_page_no = 0;

	for (i = 0; i < 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE
		     + FSP_EXTENT_SIZE / 2; i++) {
		new_block = fseg_alloc_free_page(
			fseg_header, prev_page_no + 1, FSP_UP, &mtr);
		if (new_block == NULL) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Cannot create doublewrite buffer: you must "
				"increase your tablespace size. "
				"Cannot continue operation.");

			exit(EXIT_FAILURE);
		}

		/* We read the allocated pages to the buffer pool;
		when they are written to disk in a flush, the space
		id and page number fields are also written to the
		pages. When we at database startup read pages
		from the doublewrite buffer, we know that if the
		space id and page number in them are the same as
		the page position in the tablespace, then the page
		has not been written to in doublewrite. */

		ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
		page_no = buf_block_get_page_no(new_block);

		if (i == FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == FSP_EXTENT_SIZE);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_BLOCK1,
					 page_no, MLOG_4BYTES, &mtr);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_REPEAT
					 + TRX_SYS_DOUBLEWRITE_BLOCK1,
					 page_no, MLOG_4BYTES, &mtr);

		} else if (i == FSP_EXTENT_SIZE / 2
			   + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
			ut_a(page_no == 2 * FSP_EXTENT_SIZE);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_BLOCK2,
					 page_no, MLOG_4BYTES, &mtr);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_REPEAT
					 + TRX_SYS_DOUBLEWRITE_BLOCK2,
					 page_no, MLOG_4BYTES, &mtr);

		} else if (i > FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == prev_page_no + 1);
		}

		if (((i + 1) & 15) == 0) {
			/* rw_locks can only be recursively x-locked
			2048 times. (on 32 bit platforms,
			(lint) 0 - (X_LOCK_DECR * 2049)
			is no longer a negative number, and thus
			lock_word becomes like a shared lock).
			For 4k page size this loop will
			lock the fseg header too many times. Since
			this code is not done while any other threads
			are active, restart the MTR occasionally. */
			mtr_commit(&mtr);
			mtr_start(&mtr);
			doublewrite = buf_dblwr_get(&mtr);
			fseg_header = doublewrite
				      + TRX_SYS_DOUBLEWRITE_FSEG;
		}

		prev_page_no = page_no;
	}

	mlog_write_ulint(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC,
			 TRX_SYS_DOUBLEWRITE_MAGIC_N,
			 MLOG_4BYTES, &mtr);
	mlog_write_ulint(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC
			 + TRX_SYS_DOUBLEWRITE_REPEAT,
			 TRX_SYS_DOUBLEWRITE_MAGIC_N,
			 MLOG_4BYTES, &mtr);

	mlog_write_ulint(doublewrite
			 + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED,
			 TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N,
			 MLOG_4BYTES, &mtr);
	mtr_commit(&mtr);

	/* Flush the modified pages to disk and make a checkpoint */
	log_make_checkpoint_at(LSN_MAX, TRUE);

	/* Remove doublewrite pages from LRU */
	buf_pool_invalidate();

	ib_logf(IB_LOG_LEVEL_INFO, "Doublewrite buffer created");

	goto start_again;
}

/** Compute the path to the parallel doublewrite buffer, if not already done */
MY_ATTRIBUTE((warn_unused_result))
static
dberr_t
buf_parallel_dblwr_make_path(void)
{
       static const char * dblwr_file = "ib_doublewrite";
       if (parallel_dblwr_buf.path)
               return(DB_SUCCESS);

       char path[FN_REFLEN];
       char dir_full[FN_REFLEN];
       const char *dir;
       int rc = 0;

       /* A path to the parallel doublewrite file is based
       either on srv_data_home, either mysql data directory if the
       former is empty. */
       dir = srv_data_home[0] ? srv_data_home
               : fil_path_to_mysql_datadir;

       if (my_realpath(dir_full, dir, MY_WME) != 0) {

               return(DB_ERROR);
       }

       if (dir_full[strlen(dir_full) - 1] == OS_PATH_SEPARATOR) {

               rc = my_sec_snprintf(path,
                                    sizeof(path),   // destMax
                                    sizeof(path)-1, // count
                                    "%s%s",
                                    dir_full,
                                    dblwr_file);
               if (rc <= 0) { // Also treat rc = 0 as error
                              // since we can't have an empty path.
		           ib_logf(IB_LOG_LEVEL_ERROR,
                           "buf_parallel_dblwr_make_path:1 - failed to create "
                           "path for the parallel double-write buffer "
                           "file. Error code = %d",
                           rc);

                   return(DB_ERROR);
               }
       } else {

               rc = my_sec_snprintf(path,
                                    sizeof(path),   // destMax
                                    sizeof(path)-1, // count
                                    "%s%c%s",
                                    dir_full,
                                    OS_PATH_SEPARATOR,
                                    dblwr_file);
               if (rc <= 0) { // Also treat rc = 0 as error
                              // since we can't have an empty path.
		           ib_logf(IB_LOG_LEVEL_ERROR,
                           "buf_parallel_dblwr_make_path:2 - failed to create "
                           "path for the parallel double-write buffer "
                           "file. Error code = %d",
                           rc);

                   return(DB_ERROR);
               }
       }

       os_file_type_t  type;
       ibool           exists = FALSE;
       ibool           ret;

       ret = os_file_status(path, &exists, &type);

       if (ret && exists) {
               if (type != OS_FILE_TYPE_FILE) {
		               ib_logf(IB_LOG_LEVEL_ERROR,
                               "Parallel doublewrite path %s "
                               " must point to a regular file",
                               path);
                       return(DB_IO_ERROR);
               }
       }

       parallel_dblwr_buf.path = mem_strdup(path);

       return(parallel_dblwr_buf.path ? DB_SUCCESS : DB_OUT_OF_MEMORY);
}

/** Close the parallel doublewrite buffer file */
static
void
buf_parallel_dblwr_close(void)
{
       if (OS_FILE_CLOSED != parallel_dblwr_buf.file.m_file) {
               os_file_close(parallel_dblwr_buf.file);
               parallel_dblwr_buf.file.m_file = OS_FILE_CLOSED;
       }
}

/** Maximum possible parallel doublewrite buffer file size in bytes */
#define MAX_DOUBLEWRITE_FILE_SIZE \
       ((MAX_DOUBLEWRITE_BATCH_SIZE) * (MAX_DBLWR_SHARDS) * (UNIV_PAGE_SIZE))

/****************************************************************//**
At a database startup initializes the doublewrite buffer memory structure if
we already have a doublewrite buffer created in the data files. If we are
upgrading to an InnoDB version which supports multiple tablespaces, then this
function performs the necessary update operations. If we are in a crash
recovery, this function loads the pages from double write buffer into memory. */
dberr_t
buf_dblwr_init_or_load_pages(
/*=========================*/
	pfs_os_file_t	file,
	char*		path,
	bool		load_corrupt_pages)
{
	byte*	buf;
	byte*	read_buf;
	byte*	unaligned_read_buf = NULL;
	ulint	block1;
	ulint	block2;
	byte*	page;
	ibool	reset_space_ids = FALSE;
	byte*	doublewrite;
	ulint	space_id;
	ulint	i;
    ulint	block_bytes = 0;
	recv_dblwr_t& recv_dblwr = recv_sys->dblwr;
	off_t  trx_sys_page = 0;
    dberr_t err = DB_SUCCESS;
    ibool io_success = FALSE;

    if (srv_read_only_mode) {

		ib_logf(IB_LOG_LEVEL_INFO,
                "Skipping doublewrite buffer processing due to "
                "InnoDB running in read only mode");

        goto leave_func;
    }

	/* We do the file i/o past the buffer pool */

	unaligned_read_buf = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));

	read_buf = static_cast<byte*>(
		ut_align(unaligned_read_buf, UNIV_PAGE_SIZE));

	/* Read the trx sys header to check if we are using the doublewrite
	buffer */

	trx_sys_page = TRX_SYS_PAGE_NO * UNIV_PAGE_SIZE;
	io_success = os_file_read(file, read_buf, trx_sys_page, UNIV_PAGE_SIZE);

    if (!io_success) {
		ib_logf(IB_LOG_LEVEL_ERROR,
                 "Failed to read the system header page");
        err = DB_IO_ERROR;
        goto leave_func;
    }

	doublewrite = read_buf + TRX_SYS_DOUBLEWRITE;

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has been created */

		buf_dblwr_init(doublewrite);

		block1 = buf_dblwr->block1;
		block2 = buf_dblwr->block2;

		buf = buf_dblwr->write_buf;
	} else {
		goto leave_func;
	}

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED)
	    != TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N) {

		/* We are upgrading from a version < 4.1.x to a version where
		multiple tablespaces are supported. We must reset the space id
		field in the pages in the doublewrite buffer because starting
		from this version the space id is stored to
		FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID. */

		reset_space_ids = TRUE;

		ib_logf(IB_LOG_LEVEL_INFO,
			"Resetting space id's in the doublewrite buffer");
	}

	/* Read the pages from the doublewrite buffer to memory */

    block_bytes = TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;

	io_success = os_file_read(file, buf, block1 * UNIV_PAGE_SIZE, block_bytes);

    if (!io_success) {
   		ib_logf(IB_LOG_LEVEL_ERROR,
                "Failed to read the first double write buffer extent");
        err = DB_IO_ERROR;
        goto leave_func;
    }

	io_success = os_file_read(file, buf + block_bytes, block2 * UNIV_PAGE_SIZE,
		                           block_bytes);
    if (!io_success) {
   		ib_logf(IB_LOG_LEVEL_ERROR,
                "Failed to read the second double write buffer extent");
        err = DB_IO_ERROR;
        goto leave_func;
    }

	/* Check if any of these pages is half-written in data files, in the
	intended position */

	page = buf;

	for (i = 0; i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * 2; i++) {

		ulint source_page_no;

		if (reset_space_ids) {

			space_id = 0;
			mach_write_to_4(page
					+ FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);
			/* We do not need to calculate new checksums for the
			pages because the field .._SPACE_ID does not affect
			them. Write the page back to where we read it from. */

			if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
				source_page_no = block1 + i;
			} else {
				source_page_no = block2
					+ i - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			}

			io_success = os_file_write(path, file, page,
				                            source_page_no * UNIV_PAGE_SIZE,
                      				        UNIV_PAGE_SIZE);
            if (!io_success) {
                ib_logf(IB_LOG_LEVEL_ERROR,
                        "Failed to write to double write buffer");
                err = DB_IO_ERROR;
                goto leave_func;
            }
		} else if (load_corrupt_pages) {

			recv_dblwr.add(page);
		}

		page += UNIV_PAGE_SIZE;
	}

        err = buf_parallel_dblwr_make_path();
        if (err != DB_SUCCESS) {
            goto leave_func;
        }

        ut_ad(OS_FILE_CLOSED == parallel_dblwr_buf.file.m_file);
        parallel_dblwr_buf.file
                = os_file_create_simple_no_error_handling(
                        innodb_file_parallel_dblwrite_key,
                        parallel_dblwr_buf.path,
                        OS_FILE_OPEN, OS_FILE_READ_ONLY, &io_success);
        if (!io_success) {
                /* We are not supposed to check errno != ENOENT directly, but
                os_file_get_last_error will spam error log if it's handled
                there. */
                if (errno != ENOENT) {
                        os_file_get_last_error(true);

		                ib_logf(IB_LOG_LEVEL_ERROR,
                                "Failed to open the parallel doublewrite "
                                "buffer at %s",
                                parallel_dblwr_buf.path);

                        err = DB_IO_ERROR;
                        goto leave_func;
                }
                /* Failed to open because the file did not exist: OK */
		        ib_logf(IB_LOG_LEVEL_INFO,
                        "Crash recovery did not find the parallel "
                        "doublewrite buffer at %s",
                        parallel_dblwr_buf.path);
        } else {
                /* Cannot possibly be upgrading from 4.1 */
                ut_ad(!reset_space_ids);

                os_file_set_nocache(parallel_dblwr_buf.file.m_file,
                                    parallel_dblwr_buf.path,
                                    "open");

                os_offset_t size = os_file_get_size(parallel_dblwr_buf.file);

                if (size > MAX_DOUBLEWRITE_FILE_SIZE) {
		                ib_logf(IB_LOG_LEVEL_ERROR,
                                "Parallel doublewrite buffer size %lu "
                                "bytes is larger than the maximum size %lu "
                                " bytes supported by this server version",
                                size,
                                MAX_DOUBLEWRITE_FILE_SIZE);

                        buf_parallel_dblwr_close();
                        err = DB_CORRUPTION;
                        goto leave_func;
                }

                if (size % UNIV_PAGE_SIZE) {
		                ib_logf(IB_LOG_LEVEL_ERROR,
                                "Parallel doublewrite buffer size %lu "
                                "bytes is not a multiple of a page size %lu bytes",
                                 size, UNIV_PAGE_SIZE);
                        buf_parallel_dblwr_close();
                        err = DB_CORRUPTION;
                        goto leave_func;
                }

                if (size == 0) {
		                ib_logf(IB_LOG_LEVEL_INFO,
                                "Parallel doublewrite buffer is zero-sized");

                        buf_parallel_dblwr_close();
                        err = DB_SUCCESS;
                        goto leave_func;
                }

		        ib_logf(IB_LOG_LEVEL_INFO,
                        "Recovering partial pages from the parallel "
                        "doublewrite buffer at %s",
                        parallel_dblwr_buf.path);

                parallel_dblwr_buf.recovery_buf_unaligned
                        = static_cast<byte *>(
                                ut_malloc(size + UNIV_PAGE_SIZE));
                if (!parallel_dblwr_buf.recovery_buf_unaligned) {
                        buf_parallel_dblwr_close();
                        err = DB_OUT_OF_MEMORY;
                        goto leave_func;
                }
                byte* recovery_buf = static_cast<byte *>
                        (ut_align(parallel_dblwr_buf.recovery_buf_unaligned,
                                  UNIV_PAGE_SIZE));

                io_success = os_file_read( parallel_dblwr_buf.file,
                                   recovery_buf, 0, size);
                if (!io_success) {
		                ib_logf(IB_LOG_LEVEL_ERROR,
                                "Failed to read the parallel "
                                "doublewrite buffer");
                        buf_parallel_dblwr_close();
                        ut_free(parallel_dblwr_buf.recovery_buf_unaligned);
                        parallel_dblwr_buf.recovery_buf_unaligned = NULL;
                        err = DB_ERROR;
                        goto leave_func;
                }

                for (page = recovery_buf;
                     load_corrupt_pages && (page < recovery_buf + size);
                     page += UNIV_PAGE_SIZE) {

                        recv_dblwr.add(page);
                }
                buf_parallel_dblwr_close();
        }

	if (reset_space_ids) {
		os_file_flush(file);
	}

leave_func:
    if (unaligned_read_buf != NULL) {
	    ut_free(unaligned_read_buf);
        unaligned_read_buf = NULL;
    }

    return(err);
}

/** Delete the parallel doublewrite file, if its path already has been
computed. It is up to the caller to ensure that this called at safe point */
void
buf_parallel_dblwr_delete(void)
{
       if (parallel_dblwr_buf.path) {

               os_file_delete_if_exists(innodb_file_parallel_dblwrite_key,
                                        parallel_dblwr_buf.path);
       }
}

/** Release any unused parallel doublewrite pages and free their underlying
buffer at the end of crash recovery */
void
buf_parallel_dblwr_finish_recovery(void)
{
       recv_sys->dblwr.pages.clear();
       ut_free(parallel_dblwr_buf.recovery_buf_unaligned);
       parallel_dblwr_buf.recovery_buf_unaligned = NULL;
}

/****************************************************************//**
Process the double write buffer pages. */
void
buf_dblwr_process()
/*===============*/
{
	ulint	space_id;
	ulint	page_no;
	ulint	page_no_dblwr = 0;
	byte*	page;
	byte*	read_buf;
	byte*	unaligned_read_buf;
	recv_dblwr_t& recv_dblwr = recv_sys->dblwr;

    ut_ad(!srv_read_only_mode);

	unaligned_read_buf = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));

	read_buf = static_cast<byte*>(
		ut_align(unaligned_read_buf, UNIV_PAGE_SIZE));

	for (std::list<byte*>::iterator i = recv_dblwr.pages.begin();
	     i != recv_dblwr.pages.end(); ++i, ++page_no_dblwr ) {

		page = *i;
		page_no  = mach_read_from_4(page + FIL_PAGE_OFFSET);
		space_id = mach_read_from_4(page + FIL_PAGE_SPACE_ID);

		if (!fil_tablespace_exists_in_mem(space_id)) {
			/* Maybe we have dropped the single-table tablespace
			and this page once belonged to it: do nothing */

		} else if (!fil_check_adress_in_tablespace(space_id,
							   page_no)) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"A page in the doublewrite buffer is not "
				"within space bounds; space id %lu "
				"page number %lu, page %lu in "
				"doublewrite buf.",
				(ulong) space_id, (ulong) page_no,
				page_no_dblwr);
		} else {
			ulint	zip_size = fil_space_get_zip_size(space_id);

			/* Read in the actual page from the file */
			fil_io(OS_FILE_READ, true, space_id, zip_size,
			       page_no, 0,
			       zip_size ? zip_size : UNIV_PAGE_SIZE,
			       read_buf, NULL);

			/* Check if the page is corrupt */

			if (buf_page_is_corrupted(true, read_buf, zip_size)) {

			    ib_logf(IB_LOG_LEVEL_WARN,
					    "database page corruption or a failed "
					    "file read of space %lu page %lu. "
					    "Trying to recover it from the doublewrite buffer.",
					    space_id, page_no);

				/*fprintf(stderr,
					"InnoDB: Warning: database page"
					" corruption or a failed\n"
					"InnoDB: file read of"
					" space %lu page %lu.\n"
					"InnoDB: Trying to recover it from"
					" the doublewrite buffer.\n",
					(ulong) space_id, (ulong) page_no);*/

				if (buf_page_is_corrupted(true,
							  page, zip_size)) {
					fprintf(stderr,
						"InnoDB: Dump of the page:\n");
					buf_page_print(
						read_buf, zip_size,
						BUF_PAGE_PRINT_NO_CRASH);
					fprintf(stderr,
						"InnoDB: Dump of"
						" corresponding page"
						" in doublewrite buffer:\n");
					buf_page_print(
						page, zip_size,
						BUF_PAGE_PRINT_NO_CRASH);

					fprintf(stderr,
						"InnoDB: Also the page in the"
						" doublewrite buffer"
						" is corrupt.\n"
						"InnoDB: Cannot continue"
						" operation.\n"
						"InnoDB: You can try to"
						" recover the database"
						" with the my.cnf\n"
						"InnoDB: option:\n"
						"InnoDB:"
						" innodb_force_recovery=6\n");
					ut_error;
				}

				/* Write the good page from the
				doublewrite buffer to the intended
				position */

				fil_io(OS_FILE_WRITE, true, space_id,
				       zip_size, page_no, 0,
				       zip_size ? zip_size : UNIV_PAGE_SIZE,
				       page, NULL);

				ib_logf(IB_LOG_LEVEL_INFO,
					"Recovered the page from"
					" the doublewrite buffer.");

			} else if (buf_page_is_zeroes(read_buf, zip_size)) {

				if (!buf_page_is_zeroes(page, zip_size)
				    && !buf_page_is_corrupted(true, page,
							      zip_size)) {

					/* Database page contained only
					zeroes, while a valid copy is
					available in dblwr buffer. */

					fil_io(OS_FILE_WRITE, true, space_id,
					       zip_size, page_no, 0,
					       zip_size ? zip_size
							: UNIV_PAGE_SIZE,
					       page, NULL);
				}
			}
		}
	}

	fil_flush_file_spaces(FIL_TABLESPACE);
	ut_free(unaligned_read_buf);

    buf_parallel_dblwr_finish_recovery();

    /* If parallel doublewrite buffer was used, now it's safe to
    delete and re-create it. */
    buf_parallel_dblwr_delete();
    if (srv_use_doublewrite_buf && buf_parallel_dblwr_create() != DB_SUCCESS) { 
	    ib_logf(IB_LOG_LEVEL_FATAL,
                "Creating the parallel doublewrite buffer failed");
    }
}

/****************************************************************//**
Frees doublewrite buffer. */
UNIV_INTERN
void
buf_dblwr_free(void)
/*================*/
{
	/* Free the double write data structures. */
	ut_a(buf_dblwr != NULL);
	ut_ad(buf_dblwr->s_reserved == 0);

	os_event_free(buf_dblwr->s_event);
	ut_free(buf_dblwr->write_buf_unaligned);
	buf_dblwr->write_buf_unaligned = NULL;

	mem_free(buf_dblwr->buf_block_arr);
	buf_dblwr->buf_block_arr = NULL;

	mem_free(buf_dblwr->in_use);
	buf_dblwr->in_use = NULL;

	mutex_free(&buf_dblwr->mutex);
	mem_free(buf_dblwr);
	buf_dblwr = NULL;
}

/********************************************************************//**
Updates the doublewrite buffer when an IO request is completed. */
UNIV_INTERN
void
buf_dblwr_update(
/*=============*/
	const buf_page_t*	bpage,	/*!< in: buffer block descriptor */
	buf_flush_t		flush_type)/*!< in: flush type */
{
	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		return;
	}

    // From WL#7682 (commit 810de5c)
    ut_ad(!srv_read_only_mode);

	switch (flush_type) {
	case BUF_FLUSH_LIST:
	case BUF_FLUSH_LRU:
             {
                ulint i = buf_parallel_dblwr_partition(bpage, flush_type);
                                                       
                struct parallel_dblwr_shard_t* dblwr_shard
                        = &parallel_dblwr_buf.shard[i];

                ut_ad(!os_event_is_set(dblwr_shard->batch_completed));

                if (os_atomic_decrement_ulint(&dblwr_shard->batch_size,
                                              1)
                    == 0) {

                        /* The last page from the doublewrite batch. */
                        os_event_set(dblwr_shard->batch_completed);
                }

                break;
             }

	case BUF_FLUSH_SINGLE_PAGE:
		{
			const ulint size = 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			ulint i;
			mutex_enter(&buf_dblwr->mutex);
            for (i = 0; i < size; ++i) {
				if (buf_dblwr->buf_block_arr[i] == bpage) {
					buf_dblwr->s_reserved--;
					buf_dblwr->buf_block_arr[i] = NULL;
					buf_dblwr->in_use[i] = false;
					break;
				}
			}

			/* The block we are looking for must exist as a
			reserved block. */
			ut_a(i < size);
		}
		os_event_set(buf_dblwr->s_event);
		mutex_exit(&buf_dblwr->mutex);
		break;
	case BUF_FLUSH_N_TYPES:
		ut_error;
	}
}

/********************************************************************//**
Check the LSN values on the page. */
static
void
buf_dblwr_check_page_lsn(
/*=====================*/
	const page_t*	page)		/*!< in: page to check */
{
	if (memcmp(page + (FIL_PAGE_LSN + 4),
		   page + (UNIV_PAGE_SIZE
			   - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
		   4)) {

		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: ERROR: The page to be written"
			" seems corrupt!\n"
			"InnoDB: The low 4 bytes of LSN fields do not match "
			"(" ULINTPF " != " ULINTPF ")!"
			" Noticed in the buffer pool.\n",
			mach_read_from_4(
				page + FIL_PAGE_LSN + 4),
			mach_read_from_4(
				page + UNIV_PAGE_SIZE
				- FIL_PAGE_END_LSN_OLD_CHKSUM + 4));
	}
}

/********************************************************************//**
Asserts when a corrupt block is find during writing out data to the
disk. */
static
void
buf_dblwr_assert_on_corrupt_block(
/*==============================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	buf_page_print(block->frame, 0, BUF_PAGE_PRINT_NO_CRASH);

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: Apparent corruption of an"
		" index page n:o %lu in space %lu\n"
		"InnoDB: to be written to data file."
		" We intentionally crash server\n"
		"InnoDB: to prevent corrupt data"
		" from ending up in data\n"
		"InnoDB: files.\n",
		(ulong) buf_block_get_page_no(block),
		(ulong) buf_block_get_space(block));

	ut_error;
}

/********************************************************************//**
Check the LSN values on the page with which this block is associated.
Also validate the page if the option is set. */
static
void
buf_dblwr_check_block(
/*==================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE
	    || block->page.zip.data) {
		/* No simple validate for compressed pages exists. */
		return;
	}

	buf_dblwr_check_page_lsn(block->frame);

	if (!block->check_index_page_at_flush) {
		return;
	}

	if (page_is_comp(block->frame)) {
		if (!page_simple_validate_new(block->frame)) {
			buf_dblwr_assert_on_corrupt_block(block);
		}
	} else if (!page_simple_validate_old(block->frame)) {

		buf_dblwr_assert_on_corrupt_block(block);
	}
}

/********************************************************************//**
Writes a page that has already been written to the doublewrite buffer
to the datafile. It is the job of the caller to sync the datafile. */
static
void
buf_dblwr_write_block_to_datafile(
/*==============================*/
	const buf_page_t*	bpage,	/*!< in: page to write */
	bool			sync)	/*!< in: true if sync IO
					is requested */
{
	ut_a(bpage);
	ut_a(buf_page_in_file(bpage));

	const ulint flags = sync
		? OS_FILE_WRITE
		: OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER;

	if (bpage->zip.data) {
		fil_io(flags, sync, buf_page_get_space(bpage),
		       buf_page_get_zip_size(bpage),
		       buf_page_get_page_no(bpage), 0,
		       buf_page_get_zip_size(bpage),
		       (void*) bpage->zip.data,
		       (void*) bpage);

		return;
	}


	const buf_block_t* block = (buf_block_t*) bpage;
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	buf_dblwr_check_page_lsn(block->frame);

	fil_io(flags, sync, buf_block_get_space(block), 0,
	       buf_block_get_page_no(block), 0, UNIV_PAGE_SIZE,
	       (void*) block->frame, (void*) block);

}

/********************************************************************//**
Flushes possible buffered writes from the specified partition of the
doublewrite memory buffer to disk,
and also wakes up the aio thread if simulated aio is used. It is very
important to call this function after a batch of writes has been posted,
and also when we may have to wait for a page latch! Otherwise a deadlock
of threads can occur. */
UNIV_INTERN
void
buf_dblwr_flush_buffered_writes(
/*============================*/
    ulint dblwr_partition)  /*!< in: doublewrite partition */
{
	byte*		write_buf;
	ulint		len;
    ulint       first_free = 0;
    ibool       ret = 0;

    ut_ad(parallel_dblwr_buf.recovery_buf_unaligned == NULL);

	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		/* Sync the writes to the disk. */
		buf_dblwr_sync_datafiles();
		return;
	}

    // From WL#7682 (commit 810de5c)
    ut_ad(!srv_read_only_mode);

    struct parallel_dblwr_shard_t* dblwr_shard
               = &parallel_dblwr_buf.shard[dblwr_partition];

	/* Write first to doublewrite buffer blocks. We use synchronous
	aio and thus know that file write has been completed when the
	control returns. */

    if (dblwr_shard->first_free == 0) {
        // Parallel DWB - there's no system temporary tablespace in 5.6
        // So the code from 5.7 patch doesn't apply.       

        /* The code below is from WL#7682 (commit 810de5c), but it is
        disabled since there is no system temporary tablespace in 5.6. */
        /* Wake possible simulated aio thread as there could be
        system temporary tablespace pages active for flushing.
        Note: system temporary tablespace pages are not scheduled
        for doublewrite. */
        //os_aio_simulated_wake_handler_threads();

		return;
	}

    write_buf = dblwr_shard->write_buf;

	for (ulint len2 = 0, i = 0;
         i < dblwr_shard->first_free;
	     len2 += UNIV_PAGE_SIZE, i++) {

		const buf_block_t*	block;

        block = (buf_block_t*)dblwr_shard->buf_block_arr[i];

		if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE
		    || block->page.zip.data) {
			/* No simple validate for compressed
			pages exists. */
			continue;
		}

		/* Check that the actual page in the buffer pool is
		not corrupt and the LSN values are sane. */
		buf_dblwr_check_block(block);

		/* Check that the page as written to the doublewrite
		buffer has sane LSN values. */
		buf_dblwr_check_page_lsn(write_buf + len2);
	}

    len = dblwr_shard->first_free * UNIV_PAGE_SIZE;

    /* Find our part of the doublewrite buffer */
    os_offset_t file_pos = dblwr_partition
                * srv_doublewrite_batch_size * UNIV_PAGE_SIZE;

#ifdef UNIV_DEBUG
    /* The file size must not increase */
    os_offset_t desired_size = srv_doublewrite_batch_size * UNIV_PAGE_SIZE
                * buf_parallel_dblwr_shard_num();
    os_offset_t actual_size = os_file_get_size(parallel_dblwr_buf.file);
    ut_ad(desired_size == actual_size);
    ut_ad(file_pos + len <= actual_size);
    /* We must not touch neighboring buffers */
    ut_ad(file_pos + len <= (dblwr_partition + 1)
              * srv_doublewrite_batch_size * UNIV_PAGE_SIZE);
#endif

    ret = os_file_write(parallel_dblwr_buf.path, parallel_dblwr_buf.file,
                        write_buf, file_pos, len);

    if (!ret) {
        /* We failed to write to the parallel dwb file, and letting
        execution to continue is not safe and could lead to data
        corruption. Passing in IB_LOG_LEVEL_FATAL will cause ut_error
        be called to crash the system. This preserves the error handling
        behavior prior to the parallel dwb optimization patch. */

        ib_logf(IB_LOG_LEVEL_FATAL,
                "Failed to write to the parallel double-write buffer file "
                "due to an error. This is a fatal error and allowing "
                "execution to continue may lead to data corruption.");
    }

    ut_ad(dblwr_shard->first_free <= srv_doublewrite_batch_size);

	/* increment the doublewrite flushed pages counter */
    srv_stats.dblwr_pages_written.add(dblwr_shard->first_free);
	srv_stats.dblwr_writes.inc();

    /* Now flush the doublewrite buffer data to disk */
    if (parallel_dblwr_buf.needs_flush) {

        /* Note that os_file_flush will crash the server
        if the file couldn't be sync'ed. So we don't need
        to handle the error here. */
        os_file_flush(parallel_dblwr_buf.file);
    }

	/* We know that the writes have been flushed to disk now
	and in recovery we will find them in the doublewrite buffer
	blocks. Next do the writes to the intended positions. */

    /* To future-proof this code and prevent race conditions
    if we ever port back the "online bufferpool resize" patch
    from 5.7, we need to reset first_free *before* the memory
    write barrier (though only needed in weak consistency platform
    - ie, x86-64 is ok since it has a strong consistency memory
    model) and before buf_dblwr_write_block_to_datafile(), such
    that we don't accidentally wipe out first_free from a new 
    flush batch after bp->n_flush[flush_type] is possibly reset
    to 0 as part of buf_dblwr_write_block_to_datafile()'s
    processing (so another thread can pass the check in 
    buf_flush_start() and initiate a new flush batch). */
    dblwr_shard->batch_size = dblwr_shard->first_free;
    first_free = dblwr_shard->first_free;
    dblwr_shard->first_free = 0;
    os_wmb;

    for (ulint i = 0; i < first_free; i++) {
		buf_dblwr_write_block_to_datafile(
            dblwr_shard->buf_block_arr[i], false);
	}

	/* Wake possible simulated aio thread to actually post the
	writes to the operating system. We don't flush the files
	at this point. We leave it to the IO helper thread to flush
	datafiles when the whole batch has been processed. */
	os_aio_simulated_wake_handler_threads();

    os_event_wait(dblwr_shard->batch_completed);
    os_event_reset(dblwr_shard->batch_completed);

#ifdef UNIV_DEBUG
    os_rmb;
    ut_ad(dblwr_shard->batch_size == 0);
#endif

    /* This will finish the batch. Sync data files
    to the disk. */
    fil_flush_file_spaces(FIL_TABLESPACE);
}

/********************************************************************//**
Posts a buffer page for writing. If the doublewrite memory buffer is
full, calls buf_dblwr_flush_buffered_writes and waits for for free
space to appear. */
UNIV_INTERN
void
buf_dblwr_add_to_batch(
/*====================*/
    buf_page_t* bpage,  /*!< in: buffer block to write */
    buf_flush_t flush_type)/*!< in: BUF_FLUSH_LRU or BUF_FLUSH_LIST */
{
	ulint	zip_size;

    ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
	ut_a(buf_page_in_file(bpage));

    ulint dblwr_partition = buf_parallel_dblwr_partition(bpage, flush_type);
    
    struct parallel_dblwr_shard_t* dblwr_shard
        = &parallel_dblwr_buf.shard[dblwr_partition];

try_again:
        ut_a(dblwr_shard->first_free <= srv_doublewrite_batch_size);
        ut_ad(!os_event_is_set(dblwr_shard->batch_completed));

    if (dblwr_shard->first_free == srv_doublewrite_batch_size) {

        buf_dblwr_flush_buffered_writes(dblwr_partition);

        goto try_again;
    }

    // Begin - Parallel DWB patch
    byte* p = dblwr_shard->write_buf +
              (UNIV_PAGE_SIZE * dblwr_shard->first_free);

	zip_size = buf_page_get_zip_size(bpage);

    /* For a parallel dwb shard, each write buffer has a max _writable_ size
    in bytes (after alignment) of ((batch_size-first_free)*UNIV_PAGE_SIZE) */
    size_t buf_max_size = 
        (srv_doublewrite_batch_size - dblwr_shard->first_free) * UNIV_PAGE_SIZE;

    ut_ad(buf_max_size > 0);

	if (zip_size) {
		UNIV_MEM_ASSERT_RW(bpage->zip.data, zip_size);
		/* Copy the compressed page and clear the rest. */
		if (NULL == my_memcpy(p,
                              buf_max_size,
                              bpage->zip.data, zip_size,
                              sql_print_error)) {
		    ib_logf(IB_LOG_LEVEL_FATAL,
			        "Cannot copy to parallel dwb shard buffer due to "
                    "memory security error.");
        }

		if (NULL == my_memset(p + zip_size,
                              buf_max_size - zip_size,
                              0, UNIV_PAGE_SIZE - zip_size,
                              sql_print_error)) {
		    ib_logf(IB_LOG_LEVEL_FATAL,
			        "Cannot clear parallel dwb shard buffer due to "
                    "memory security error.");
        }
	} else {
		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
		UNIV_MEM_ASSERT_RW(((buf_block_t*) bpage)->frame,
				   UNIV_PAGE_SIZE);

		if (NULL == my_memcpy(p,
                              buf_max_size,
                              ((buf_block_t*) bpage)->frame, UNIV_PAGE_SIZE,
                              sql_print_error)) {
		    ib_logf(IB_LOG_LEVEL_FATAL,
			        "Cannot copy to parallel dwb shard buffer due to "
                    "memory security error.");
        }
	}
    // End - Parallel DWB patch

    dblwr_shard->buf_block_arr[dblwr_shard->first_free++] = bpage;

    ut_ad(!os_event_is_set(dblwr_shard->batch_completed));
    ut_ad(dblwr_shard->first_free <= srv_doublewrite_batch_size);
}

/********************************************************************//**
Writes a page to the doublewrite buffer on disk, sync it, then write
the page to the datafile and sync the datafile. This function is used
for single page flushes. If all the buffers allocated for single page
flushes in the doublewrite buffer are in use we wait here for one to
become free. We are guaranteed that a slot will become free because any
thread that is using a slot must also release the slot before leaving
this function. */
UNIV_INTERN
void
buf_dblwr_write_single_page(
/*========================*/
	buf_page_t*	bpage,	/*!< in: buffer block to write */
	bool		sync)	/*!< in: true if sync IO requested */
{
	ulint		size;
	ulint		zip_size;
	ulint		offset;
	ulint		i;

	ut_a(buf_page_in_file(bpage));
	ut_a(srv_use_doublewrite_buf);
	ut_a(buf_dblwr != NULL);

	size = 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;

	if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {

		/* Check that the actual page in the buffer pool is
		not corrupt and the LSN values are sane. */
		buf_dblwr_check_block((buf_block_t*) bpage);

		/* Check that the page as written to the doublewrite
		buffer has sane LSN values. */
		if (!bpage->zip.data) {
			buf_dblwr_check_page_lsn(
				((buf_block_t*) bpage)->frame);
		}
	}

retry:
	mutex_enter(&buf_dblwr->mutex);
	if (buf_dblwr->s_reserved == size) {

		/* All slots are reserved. */
		ib_int64_t	sig_count =
			os_event_reset(buf_dblwr->s_event);
		mutex_exit(&buf_dblwr->mutex);
		os_event_wait_low(buf_dblwr->s_event, sig_count);

		goto retry;
	}

	for (i = 0; i < size; ++i) {

		if (!buf_dblwr->in_use[i]) {
			break;
		}
	}

	/* We are guaranteed to find a slot. */
	ut_a(i < size);
	buf_dblwr->in_use[i] = true;
	buf_dblwr->s_reserved++;
	buf_dblwr->buf_block_arr[i] = bpage;

	/* increment the doublewrite flushed pages counter */
	srv_stats.dblwr_pages_written.inc();
	srv_stats.dblwr_writes.inc();

	mutex_exit(&buf_dblwr->mutex);

	/* Lets see if we are going to write in the first or second
	block of the doublewrite buffer. */
	if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		offset = buf_dblwr->block1 + i;
	} else {
		offset = buf_dblwr->block2 + i
			 - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
	}

	/* We deal with compressed and uncompressed pages a little
	differently here. In case of uncompressed pages we can
	directly write the block to the allocated slot in the
	doublewrite buffer in the system tablespace and then after
	syncing the system table space we can proceed to write the page
	in the datafile.
	In case of compressed page we first do a memcpy of the block
	to the in-memory buffer of doublewrite before proceeding to
	write it. This is so because we want to pad the remaining
	bytes in the doublewrite page with zeros. */

	zip_size = buf_page_get_zip_size(bpage);
	if (zip_size) {
		memcpy(buf_dblwr->write_buf + UNIV_PAGE_SIZE * i,
		       bpage->zip.data, zip_size);
		memset(buf_dblwr->write_buf + UNIV_PAGE_SIZE * i
		       + zip_size, 0, UNIV_PAGE_SIZE - zip_size);

		fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
		       offset, 0, UNIV_PAGE_SIZE,
		       (void*) (buf_dblwr->write_buf
				+ UNIV_PAGE_SIZE * i), NULL);
	} else {
		/* It is a regular page. Write it directly to the
		doublewrite buffer */
		fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
		       offset, 0, UNIV_PAGE_SIZE,
		       (void*) ((buf_block_t*) bpage)->frame,
		       NULL);
	}

	/* Now flush the doublewrite buffer data to disk */
	fil_flush(TRX_SYS_SPACE);

	/* We know that the write has been flushed to disk now
	and during recovery we will find it in the doublewrite buffer
	blocks. Next do the write to the intended position. */
	buf_dblwr_write_block_to_datafile(bpage, sync);
}

/** Compute the size and path of the parallel doublewrite buffer, create it,
and disable OS caching for it
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
buf_parallel_dblwr_file_create(void)
{
        ut_ad(!srv_read_only_mode);
        /* The buffer size is two doublewrite batches (one for LRU,
        one for flush list) per buffer pool instance. */
        os_offset_t size = srv_doublewrite_batch_size * UNIV_PAGE_SIZE
                * buf_parallel_dblwr_shard_num();
        ut_a(size <= MAX_DOUBLEWRITE_FILE_SIZE);
        ut_a(size > 0);
        ut_a(size % UNIV_PAGE_SIZE == 0);

        dberr_t err = buf_parallel_dblwr_make_path();
        if (err != DB_SUCCESS)
                return(err);

        ut_ad(OS_FILE_CLOSED == parallel_dblwr_buf.file.m_file);
        ut_ad(parallel_dblwr_buf.recovery_buf_unaligned == NULL);

        /* Set O_SYNC if innodb_flush_method == O_DSYNC. */
        ulint o_sync = (srv_unix_file_flush_method == SRV_UNIX_O_DSYNC)
                ? OS_FILE_O_SYNC : 0;

        ibool success;
        parallel_dblwr_buf.file
                = os_file_create_simple(innodb_file_parallel_dblwrite_key,
                                        parallel_dblwr_buf.path,
                                        OS_FILE_CREATE | o_sync,
                                        OS_FILE_READ_WRITE, &success);
        if (!success) {
                if (os_file_get_last_error(false) == OS_FILE_ALREADY_EXISTS) {
		                ib_logf(IB_LOG_LEVEL_ERROR,
                                 "A parallel doublewrite file %s "
                                 " found on startup.",
                                 parallel_dblwr_buf.path);
                }
                return(DB_ERROR);
        }


        os_file_set_nocache(parallel_dblwr_buf.file.m_file,
                                      parallel_dblwr_buf.path,
                                      "create");
        switch (srv_unix_file_flush_method) {
        case SRV_UNIX_NOSYNC:
        case SRV_UNIX_O_DIRECT_NO_FSYNC:
                parallel_dblwr_buf.needs_flush = false;
                break;
        case SRV_UNIX_O_DSYNC:
        case SRV_UNIX_FSYNC:
        case SRV_UNIX_LITTLESYNC:
        case SRV_UNIX_O_DIRECT:
                parallel_dblwr_buf.needs_flush = true;
                break;
        }

        success = os_file_set_size(parallel_dblwr_buf.path,
                                   parallel_dblwr_buf.file, size);
        if (!success) {
                buf_parallel_dblwr_free(true);
                return(DB_ERROR);
        }
        ut_ad(os_file_get_size(parallel_dblwr_buf.file) == size);

        if (parallel_dblwr_buf.needs_flush) {
           os_file_flush(parallel_dblwr_buf.file);
        }

		ib_logf(IB_LOG_LEVEL_INFO,
                "Created parallel doublewrite buffer at %s, size %lu bytes",
                parallel_dblwr_buf.path,
                os_file_get_size(parallel_dblwr_buf.file));

        /* Parallel DWB patch:
        Always dump out a message to inform the user that the DWB file is
        created successfully. This is for distinguishing the case where the
        file is corrupted during execution time after server startup, or for
        whatever reason the server crashed during DWB creation leaving it
        corrupted. For the latter case it is safe to simply remove the
        partial DWB file and restart the server. */ 
        fprintf(stderr,
                "InnoDB: double-write buffer file created successfully at %s\n",
                parallel_dblwr_buf.path);

        return(DB_SUCCESS);
}

/** Initialize parallel doublewrite subsystem: create its data structure and
the disk file.
@return DB_SUCCESS or error code */
dberr_t
buf_parallel_dblwr_create(void)
{
        if (OS_FILE_CLOSED != parallel_dblwr_buf.file.m_file || srv_read_only_mode) {

                ut_ad(parallel_dblwr_buf.recovery_buf_unaligned == NULL);
                return(DB_SUCCESS);
        }

        if (NULL == my_memset(parallel_dblwr_buf.shard,
                              sizeof(parallel_dblwr_buf.shard),
                              0, sizeof(parallel_dblwr_buf.shard),
                              sql_print_error)) {
		    ib_logf(IB_LOG_LEVEL_ERROR,
			        "Cannot clear parallel dwb shard due to "
                    "memory security error.");

            return (DB_ERROR);
        }

        dberr_t err = buf_parallel_dblwr_file_create();
        if (err != DB_SUCCESS) {
                return(err);
        }

        for (ulint i = 0; i < buf_parallel_dblwr_shard_num(); i++) {

                struct parallel_dblwr_shard_t* dblwr_shard
                        = &parallel_dblwr_buf.shard[i];

                size_t buf_size = 0;

                dblwr_shard->write_buf_unaligned
                        = static_cast<byte*>(ut_malloc((1
                                          + srv_doublewrite_batch_size)
                                                       * UNIV_PAGE_SIZE));
                if (!dblwr_shard->write_buf_unaligned) {
                        buf_parallel_dblwr_free(true);
                        return(DB_OUT_OF_MEMORY);
                }
                dblwr_shard->write_buf = static_cast<byte*>(
                        ut_align(dblwr_shard->write_buf_unaligned,
                                 UNIV_PAGE_SIZE));

                buf_size = srv_doublewrite_batch_size * sizeof(void*);
                dblwr_shard->buf_block_arr
                        = static_cast<buf_page_t**>(
                        mem_alloc(buf_size));

                if (!dblwr_shard->buf_block_arr) {
                        buf_parallel_dblwr_free(true);
                        return(DB_OUT_OF_MEMORY);
                }

                if (NULL == my_memset(dblwr_shard->buf_block_arr,
                                      buf_size, //max size
                                      0,
                                      buf_size, //max size
                                      sql_print_error)) {
                        buf_parallel_dblwr_free(true);
                        return(DB_ERROR);
                }

                dblwr_shard->batch_completed
                        = os_event_create();

                os_event_reset(dblwr_shard->batch_completed);
        }

        return(DB_SUCCESS);
}

/** Cleanup parallel doublewrite memory structures and optionally close and
delete the doublewrite buffer file too.
@param  delete_file     whether to close and delete the buffer file too  */
void
buf_parallel_dblwr_free(bool delete_file)
{
        for (ulint i = 0; i < buf_parallel_dblwr_shard_num(); i++) {

                struct parallel_dblwr_shard_t* dblwr_shard
                        = &parallel_dblwr_buf.shard[i];

                if (dblwr_shard->write_buf_unaligned
                    && dblwr_shard->buf_block_arr) {
                        os_event_free(dblwr_shard->batch_completed);
                }

                ut_free(dblwr_shard->write_buf_unaligned);
                dblwr_shard->write_buf_unaligned = NULL;

                // mem_free doesn't check for null ptr.
                if (dblwr_shard->buf_block_arr != NULL) {
                    mem_free(dblwr_shard->buf_block_arr);
                    dblwr_shard->buf_block_arr = NULL;
                }
        }

        if (delete_file) {
                buf_parallel_dblwr_close();
                buf_parallel_dblwr_delete();
        }

        // mem_free doesn't check for null ptr.
        if (parallel_dblwr_buf.path != NULL) {
            mem_free(parallel_dblwr_buf.path);
            parallel_dblwr_buf.path = NULL;
        }
}

/** The parallel doublewrite buffer */
parallel_dblwr_t parallel_dblwr_buf;

/** Default constructor for the parallel doublewrite instance */
parallel_dblwr_t::parallel_dblwr_t(void)
{
    file.m_file = OS_FILE_CLOSED;
#ifdef UNIV_PFS_IO
    file.m_psi = NULL;
#endif

    needs_flush = false;
    path = NULL;

    if (NULL == my_memset(shard, sizeof(shard),
                          0, sizeof(shard), sql_print_error)) {
        ib_logf(IB_LOG_LEVEL_ERROR,
                "Cannot clear parallel dwb shard buffer due to "
                "memory security error.");
    }

    recovery_buf_unaligned = NULL;
}

#endif /* !UNIV_HOTBACKUP */
