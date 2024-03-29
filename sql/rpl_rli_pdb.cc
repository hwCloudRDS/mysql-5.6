/* Copyright (c) 2011, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_securec.h"  /* Should be the first include */
#include "my_global.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "unireg.h"
#include "debug_sync.h"
#include "rpl_rli_pdb.h"
#include "rpl_slave.h"
#include "sql_string.h"
#include <hash.h>
#include "rpl_mts_submode.h"
#include "rpl_slave_commit_order_manager.h"

#ifndef DBUG_OFF
  ulong w_rr= 0;
  uint mts_debug_concurrent_access= 0;
#endif

#define HASH_DYNAMIC_INIT 4
#define HASH_DYNAMIC_INCR 1

using std::min;
using std::max;

/**
   This function is called by both coordinator and workers.

   Upon receiving the STOP command, the workers will identify a
   maximum group index already executed (or under execution).

   All groups whose index are below or equal to the maximum
   group index will be applied by the workers before stopping.

   The workers with groups above the maximum group index will
   exit without applying these groups by setting their running
   status to "STOP_ACCEPTED".

   @param worker    a pointer to the waiting Worker struct
   @param job_item  a pointer to struct carrying a reference to an event

   @return true if STOP command gets accepted otherwise false is returned.
*/
bool handle_slave_worker_stop(Slave_worker *worker,
                              Slave_job_item *job_item)
{
  ulonglong group_index= 0;
  Relay_log_info *rli= worker->c_rli;
  mysql_mutex_lock(&rli->exit_count_lock);
  /*
    First, W calculates a group-"at-hands" index which is
    either the currently read ev group index, or the last executed
    group's one when the  queue is empty.
  */
  group_index= (job_item->data)?
    rli->gaq->get_job_group(((Log_event*)
                             (job_item->data))->mts_group_idx)->total_seqno:
    worker->last_groups_assigned_index;

  /*
    The max updated index is being updated as long as
    exit_counter permits. That's stopped with the final W's
    increment of it.
  */
  if (!worker->exit_incremented)
  {
    if (rli->exit_counter < rli->slave_parallel_workers)
      rli->max_updated_index = max(rli->max_updated_index, group_index);

    ++rli->exit_counter;
    worker->exit_incremented= true;
    DBUG_ASSERT(!is_mts_worker(current_thd));
  }
#ifndef DBUG_OFF
  else
    DBUG_ASSERT(is_mts_worker(current_thd));
#endif

  /*
    Now let's decide about the deferred exit to consider
    the empty queue and the counter value reached
    slave_parallel_workers.
  */
  if (!job_item->data)
  {
    worker->running_status= Slave_worker::STOP_ACCEPTED;
    mysql_cond_signal(&worker->jobs_cond);
    mysql_mutex_unlock(&rli->exit_count_lock);
    return(true);
  }
  else if (rli->exit_counter == rli->slave_parallel_workers)
  {
    //over steppers should exit with accepting STOP
    if (group_index > rli->max_updated_index)
    {
      worker->running_status= Slave_worker::STOP_ACCEPTED;
      mysql_cond_signal(&worker->jobs_cond);
      mysql_mutex_unlock(&rli->exit_count_lock);
      return(true);
    }
  }
  mysql_mutex_unlock(&rli->exit_count_lock);
  return(false);
}

/**
   This function is called by both coordinator and workers.
   Both coordinator and workers contribute to max_updated_index.

   @param worker    a pointer to the waiting Worker struct
   @param job_item  a pointer to struct carrying a reference to an event

   @return true if STOP command gets accepted otherwise false is returned.
*/
bool set_max_updated_index_on_stop(Slave_worker *worker,
                                   Slave_job_item *job_item)
{
  head_queue(&worker->jobs, job_item);
  if (worker->running_status == Slave_worker::STOP)
  {
    if (handle_slave_worker_stop(worker, job_item))
      return true;
  }
  return false;
}

/*
  Please every time you add a new field to the worker slave info, update
  what follows. For now, this is just used to get the number of fields.
*/
const char *info_slave_worker_fields []=
{
  "id",
  /*
    These positions identify what has been executed. Notice that they are
    redudant and only the group_master_log_name and group_master_log_pos
    are really necessary. However, the additional information is kept to
    ease debugging.
  */
  "group_relay_log_name",
  "group_relay_log_pos",
  "group_master_log_name",
  "group_master_log_pos",

  /*
    These positions identify what a worker knew about the coordinator at
    the time a job was assigned. Notice that they are redudant and are
    kept to ease debugging.
  */
  "checkpoint_relay_log_name",
  "checkpoint_relay_log_pos",
  "checkpoint_master_log_name",
  "checkpoint_master_log_pos",

  /*
    Identify the greatest job, i.e. group, processed by a worker.
  */
  "checkpoint_seqno",
  /*
    Maximum number of jobs that can be assigned to a worker. This
    information is necessary to read the next entry.
  */
  "checkpoint_group_size",
  /*
    Bitmap used to identify what jobs were processed by a worker.
  */
  "checkpoint_group_bitmap"
};

/*
  Number of records in the mts partition hash below which
  entries with zero usage are tolerated so could be quickly
  recycled.
*/
ulong mts_partition_hash_soft_max= 16;

Slave_worker::Slave_worker(Relay_log_info *rli
#ifdef HAVE_PSI_INTERFACE
                           ,PSI_mutex_key *param_key_info_run_lock,
                           PSI_mutex_key *param_key_info_data_lock,
                           PSI_mutex_key *param_key_info_sleep_lock,
                           PSI_mutex_key *param_key_info_data_cond,
                           PSI_mutex_key *param_key_info_start_cond,
                           PSI_mutex_key *param_key_info_stop_cond,
                           PSI_mutex_key *param_key_info_sleep_cond
#endif
                           , uint param_id
                          )
  : Relay_log_info(FALSE
#ifdef HAVE_PSI_INTERFACE
                   ,param_key_info_run_lock, param_key_info_data_lock,
                   param_key_info_sleep_lock,
                   param_key_info_data_cond, param_key_info_start_cond,
                   param_key_info_stop_cond, param_key_info_sleep_cond
#endif
                   , param_id + 1, true /* Bug#14678248 Fix */
                  ), c_rli(rli), id(param_id),
    checkpoint_relay_log_pos(0), checkpoint_master_log_pos(0),
    checkpoint_seqno(0), running_status(NOT_RUNNING), exit_incremented(false)
{
  /*
    In the future, it would be great if we use only one identifier.
    So when factoring out this code, please, consider this.
  */
  DBUG_ASSERT(internal_id == id + 1);
  checkpoint_relay_log_name[0]= 0;
  checkpoint_master_log_name[0]= 0;
  my_init_dynamic_array(&curr_group_exec_parts, sizeof(db_worker_hash_entry*),
                        SLAVE_INIT_DBS_IN_GROUP, 1);
  mysql_mutex_init(key_mutex_slave_parallel_worker, &jobs_lock,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_cond_slave_parallel_worker, &jobs_cond, NULL);
}

Slave_worker::~Slave_worker()
{
  end_info();
  if (jobs.inited_queue)
  {
    DBUG_ASSERT(jobs.Q.elements == jobs.size);
    delete_dynamic(&jobs.Q);
  }
  delete_dynamic(&curr_group_exec_parts);
  mysql_mutex_destroy(&jobs_lock);
  mysql_cond_destroy(&jobs_cond);
  //mysql_mutex_lock(&info_thd_lock);
  info_thd= NULL;
  //mysql_mutex_unlock(&info_thd_lock);
  set_rli_description_event(NULL);
}

/**
   Method is executed by Coordinator at Worker startup time to initialize
   members parly with values supplied by Coordinator through rli.

   @param  rli  Coordinator's Relay_log_info pointer
   @param  i    identifier of the Worker

   @return 0          success
           non-zero   failure
*/
int Slave_worker::init_worker(Relay_log_info * rli, ulong i)
{
  DBUG_ENTER("Slave_worker::init_worker");
  DBUG_ASSERT(!rli->info_thd->is_error());
  uint k;
  Slave_job_item empty= {NULL, 0, 0};

  c_rli= rli;
  set_commit_order_manager(c_rli->get_commit_order_manager());

  if (rli_init_info(false) ||
      DBUG_EVALUATE_IF("inject_init_worker_init_info_fault", true, false))
    DBUG_RETURN(1);

  id= i;
  curr_group_exec_parts.elements= 0;
  relay_log_change_notified= FALSE; // the 1st group to contain relaylog name
  checkpoint_notified= FALSE;       // the same as above
  master_log_change_notified= false;// W learns master log during 1st group exec
  bitmap_shifted= 0;
  workers= c_rli->workers; // shallow copying is sufficient
  wq_empty_waits= wq_size_waits_cnt= groups_done= events_done= curr_jobs= 0;
  usage_partition= 0;
  end_group_sets_max_dbs= false;
  gaq_index= last_group_done_index= c_rli->gaq->size; // out of range
  last_groups_assigned_index=0;
  DBUG_ASSERT(!jobs.inited_queue);
  jobs.avail= 0;
  jobs.len= 0;
  jobs.overfill= FALSE;    //  todo: move into Slave_jobs_queue constructor
  jobs.waited_overfill= 0;
  jobs.entry= jobs.size= c_rli->mts_slave_worker_queue_len_max;
  jobs.inited_queue= true;
  curr_group_seen_begin= curr_group_seen_gtid= false;
#ifndef DBUG_OFF
  curr_group_seen_sequence_number= false;
#endif
  my_init_dynamic_array(&jobs.Q, sizeof(Slave_job_item), jobs.size, 0);
  for (k= 0; k < jobs.size; k++)
    insert_dynamic(&jobs.Q, (uchar*) &empty);

  DBUG_ASSERT(jobs.Q.elements == jobs.size);

  wq_overrun_cnt= excess_cnt= 0;
  underrun_level= (ulong) ((rli->mts_worker_underrun_level * jobs.size) / 100.0);
  // overrun level is symmetric to underrun (as underrun to the full queue)
  overrun_level= jobs.size - underrun_level;

  /* create mts submode for each of the the workers. */
  current_mts_submode=
    (mts_parallel_option == MTS_PARALLEL_TYPE_DB_NAME)?
       (Mts_submode*) new Mts_submode_database():
       (Mts_submode*) new Mts_submode_logical_clock();

  m_order_commit_deadlock= false;
  DBUG_RETURN(0);
}

/**
   A part of Slave worker iitializer that provides a
   minimum context for MTS recovery.

   @param is_gaps_collecting_phase

          clarifies what state the caller
          executes this method from. When it's @c true
          that is @c mts_recovery_groups() and Worker should
          restore the last session time info which is processed
          to collect gaps that is not executed transactions (groups).
          Such recovery Slave_worker intance is destroyed at the end of
          @c mts_recovery_groups().
          Whet it's @c false Slave_worker is initialized for the run time
          nad should not read the last session time stale info.
          Its info will be ultimately reset once all gaps are executed
          to finish off recovery.

   @return 0 on success, non-zero for a failure
*/
int Slave_worker::rli_init_info(bool is_gaps_collecting_phase)
{
  enum_return_check return_check= ERROR_CHECKING_REPOSITORY;

  DBUG_ENTER("Slave_worker::rli_init_info");

  if (inited)
    DBUG_RETURN(0);

  /*
    Worker bitmap size depends on recovery mode.
    If it is gaps collecting the bitmaps must be capable to accept
    up to MTS_MAX_BITS_IN_GROUP of bits.
  */
  size_t num_bits= is_gaps_collecting_phase ?
    MTS_MAX_BITS_IN_GROUP : c_rli->checkpoint_group;
  /*
    This checks if the repository was created before and thus there
    will be values to be read. Please, do not move this call after
    the handler->init_info().
  */
  return_check= check_info();
  if (return_check == ERROR_CHECKING_REPOSITORY ||
      (return_check == REPOSITORY_DOES_NOT_EXIST && is_gaps_collecting_phase))
    goto err;

  if (handler->init_info())
    goto err;

  bitmap_init(&group_executed, NULL, num_bits, FALSE);
  bitmap_init(&group_shifted, NULL, num_bits, FALSE);

  if (is_gaps_collecting_phase &&
      (DBUG_EVALUATE_IF("mts_slave_worker_init_at_gaps_fails", true, false) ||
       read_info(handler)))
  {
    bitmap_free(&group_executed);
    bitmap_free(&group_shifted);
    goto err;
  }
  inited= 1;

  DBUG_RETURN(0);

err:
  // todo: handler->end_info(uidx, nidx);
  inited= 0;
  sql_print_error("Error reading slave worker configuration");
  DBUG_RETURN(1);
}

void Slave_worker::end_info()
{
  DBUG_ENTER("Slave_worker::end_info");

  if (!inited)
    DBUG_VOID_RETURN;

  if (handler)
    handler->end_info();

  if (inited)
  {
    bitmap_free(&group_executed);
    bitmap_free(&group_shifted);
  }
  inited = 0;

  DBUG_VOID_RETURN;
}

int Slave_worker::flush_info(const bool force)
{
  DBUG_ENTER("Slave_worker::flush_info");

  if (!inited)
    DBUG_RETURN(0);

  /*
    We update the sync_period at this point because only here we
    now that we are handling a Slave_worker. This needs to be
    update every time we call flush because the option may be
    dinamically set.
  */
  handler->set_sync_period(sync_relayloginfo_period);

  if (write_info(handler))
    goto err;

  if (handler->flush_info(force))
    goto err;

  DBUG_RETURN(0);

err:
  sql_print_error("Error writing slave worker configuration");
  DBUG_RETURN(1);
}

bool Slave_worker::read_info(Rpl_info_handler *from)
{
  DBUG_ENTER("Slave_worker::read_info");

  ulong temp_group_relay_log_pos= 0;
  ulong temp_group_master_log_pos= 0;
  ulong temp_checkpoint_relay_log_pos= 0;
  ulong temp_checkpoint_master_log_pos= 0;
  ulong temp_checkpoint_seqno= 0;
  ulong nbytes= 0;
  uchar *buffer= (uchar *) group_executed.bitmap;
  int temp_internal_id= 0;

  if (from->prepare_info_for_read())
    DBUG_RETURN(TRUE);

  if (from->get_info((int *) &temp_internal_id, (int) 0) ||
      from->get_info(group_relay_log_name,
                     (size_t) sizeof(group_relay_log_name),
                     (char *) "") ||
      from->get_info((ulong *) &temp_group_relay_log_pos,
                     (ulong) 0) ||
      from->get_info(group_master_log_name,
                     (size_t) sizeof(group_master_log_name),
                     (char *) "") ||
      from->get_info((ulong *) &temp_group_master_log_pos,
                     (ulong) 0) ||
      from->get_info(checkpoint_relay_log_name,
                     (size_t) sizeof(checkpoint_relay_log_name),
                     (char *) "") ||
      from->get_info((ulong *) &temp_checkpoint_relay_log_pos,
                     (ulong) 0) ||
      from->get_info(checkpoint_master_log_name,
                     (size_t) sizeof(checkpoint_master_log_name),
                     (char *) "") ||
      from->get_info((ulong *) &temp_checkpoint_master_log_pos,
                     (ulong) 0) ||
      from->get_info((ulong *) &temp_checkpoint_seqno,
                     (ulong) 0) ||
      from->get_info(&nbytes, (ulong) 0) ||
      from->get_info(buffer, (size_t) nbytes,
                     (uchar *) 0))
    DBUG_RETURN(TRUE);

  DBUG_ASSERT(nbytes <= no_bytes_in_map(&group_executed));

  internal_id=(uint) temp_internal_id;
  group_relay_log_pos=  temp_group_relay_log_pos;
  group_master_log_pos= temp_group_master_log_pos;
  checkpoint_relay_log_pos=  temp_checkpoint_relay_log_pos;
  checkpoint_master_log_pos= temp_checkpoint_master_log_pos;
  checkpoint_seqno= temp_checkpoint_seqno;

  DBUG_RETURN(FALSE);
}

bool Slave_worker::write_info(Rpl_info_handler *to)
{
  DBUG_ENTER("Master_info::write_info");

  ulong nbytes= (ulong) no_bytes_in_map(&group_executed);
  uchar *buffer= (uchar*) group_executed.bitmap;
  DBUG_ASSERT(nbytes <= (c_rli->checkpoint_group + 7) / 8);

  if (to->prepare_info_for_write() ||
      to->set_info((int) internal_id) ||
      to->set_info(group_relay_log_name) ||
      to->set_info((ulong) group_relay_log_pos) ||
      to->set_info(group_master_log_name) ||
      to->set_info((ulong) group_master_log_pos) ||
      to->set_info(checkpoint_relay_log_name) ||
      to->set_info((ulong) checkpoint_relay_log_pos) ||
      to->set_info(checkpoint_master_log_name) ||
      to->set_info((ulong) checkpoint_master_log_pos) ||
      to->set_info((ulong) checkpoint_seqno) ||
      to->set_info(nbytes) ||
      to->set_info(buffer, (size_t) nbytes))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}

/**
   Clean up a part of Worker info table that is regarded in
   in gaps collecting at recovery.
   This worker won't contribute to recovery bitmap at future
   slave restart (see @c mts_recovery_groups).

   @retrun FALSE as success TRUE as failure
*/
bool Slave_worker::reset_recovery_info()
{
  DBUG_ENTER("Slave_worker::reset_recovery_info");

  set_group_master_log_name("");
  set_group_master_log_pos(0);

  DBUG_RETURN(flush_info(true));
}

size_t Slave_worker::get_number_worker_fields()
{
  return sizeof(info_slave_worker_fields)/sizeof(info_slave_worker_fields[0]);
}

const char* Slave_worker::get_master_log_name()
{
  Slave_job_group* ptr_g= c_rli->gaq->get_job_group(gaq_index);

  return (ptr_g->checkpoint_log_name != NULL) ?
    ptr_g->checkpoint_log_name : checkpoint_master_log_name;
}

bool Slave_worker::commit_positions(Log_event *ev, Slave_job_group* ptr_g, bool force)
{
  DBUG_ENTER("Slave_worker::checkpoint_positions");

  /*
    Initial value of checkpoint_master_log_name is learned from
    group_master_log_name. The latter can be passed to Worker
    at rare event of master binlog rotation.
    This initialization is needed to provide to Worker info
    on physical coordiates during execution of the very first group
    after a rotation.
  */
  if (ptr_g->group_master_log_name != NULL)
  {
    strmake(group_master_log_name, ptr_g->group_master_log_name,
            sizeof(group_master_log_name) - 1);
    my_free(ptr_g->group_master_log_name);
    ptr_g->group_master_log_name= NULL;
    strmake(checkpoint_master_log_name, group_master_log_name,
            sizeof(checkpoint_master_log_name) - 1);
  }
  if (ptr_g->checkpoint_log_name != NULL)
  {
    strmake(checkpoint_relay_log_name, ptr_g->checkpoint_relay_log_name,
            sizeof(checkpoint_relay_log_name) - 1);
    checkpoint_relay_log_pos= ptr_g->checkpoint_relay_log_pos;
    strmake(checkpoint_master_log_name, ptr_g->checkpoint_log_name,
            sizeof(checkpoint_master_log_name) - 1);
    checkpoint_master_log_pos= ptr_g->checkpoint_log_pos;

    my_free(ptr_g->checkpoint_log_name);
    ptr_g->checkpoint_log_name= NULL;
    my_free(ptr_g->checkpoint_relay_log_name);
    ptr_g->checkpoint_relay_log_name= NULL;

    bitmap_copy(&group_shifted, &group_executed);
    bitmap_clear_all(&group_executed);
    for (uint pos= ptr_g->shifted; pos < c_rli->checkpoint_group; pos++)
    {
      if (bitmap_is_set(&group_shifted, pos))
        bitmap_set_bit(&group_executed, pos - ptr_g->shifted);
    }
  }
  /*
    Extracts an updated relay-log name to store in Worker's rli.
  */
  if (ptr_g->group_relay_log_name)
  {
    DBUG_ASSERT(strlen(ptr_g->group_relay_log_name) + 1
                <= sizeof(group_relay_log_name));
    strmake(group_relay_log_name, ptr_g->group_relay_log_name,
            sizeof(group_relay_log_name) - 1);
  }

  DBUG_ASSERT(ptr_g->checkpoint_seqno <= (c_rli->checkpoint_group - 1));

  bitmap_set_bit(&group_executed, ptr_g->checkpoint_seqno);
  checkpoint_seqno= ptr_g->checkpoint_seqno;
  group_relay_log_pos= ev->future_event_relay_log_pos;
  group_master_log_pos= ev->log_pos;

  /*
    Directly accessing c_rli->get_group_master_log_name() does not
    represent a concurrency issue because the current code places
    a synchronization point when master rotates.
  */
  strmake(group_master_log_name, c_rli->get_group_master_log_name(),
          sizeof(group_master_log_name)-1);

  DBUG_PRINT("mts", ("Committing worker-id %lu group master log pos %llu "
             "group master log name %s checkpoint sequence number %lu.",
             id, group_master_log_pos, group_master_log_name, checkpoint_seqno));

  DBUG_EXECUTE_IF("mts_debug_concurrent_access",
    {
      mts_debug_concurrent_access++;
    };
  );

  DBUG_RETURN(flush_info(force));
}

void Slave_worker::rollback_positions(Slave_job_group* ptr_g)
{
  if (!is_transactional())
  {
    bitmap_clear_bit(&group_executed, ptr_g->checkpoint_seqno);
    flush_info(false);
  }
}

HASH mapping_db_to_worker;
static bool inited_hash_workers= FALSE;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_mutex_slave_worker_hash;
PSI_cond_key key_cond_slave_worker_hash;
#endif

mysql_mutex_t slave_worker_hash_lock;
mysql_cond_t slave_worker_hash_cond;


extern "C" uchar *get_key(const uchar *record, size_t *length,
                          my_bool not_used MY_ATTRIBUTE((unused)))
{
  DBUG_ENTER("get_key");

  db_worker_hash_entry *entry=(db_worker_hash_entry *) record;
  *length= strlen(entry->db);

  DBUG_PRINT("info", ("get_key  %s, %d", entry->db, (int) *length));

  DBUG_RETURN((uchar*) entry->db);
}


static void free_entry(db_worker_hash_entry *entry)
{
  THD *c_thd= current_thd;

  DBUG_ENTER("free_entry");

  DBUG_PRINT("info", ("free_entry %s, %d", entry->db, (int) strlen(entry->db)));

  DBUG_ASSERT(c_thd->system_thread == SYSTEM_THREAD_SLAVE_SQL);

  /*
    Although assert is correct valgrind senses entry->worker can be freed.

    DBUG_ASSERT(entry->usage == 0 ||
                !entry->worker    ||  // last entry owner could have errored out
                entry->worker->running_status != Slave_worker::RUNNING);
  */

  mts_move_temp_tables_to_thd(c_thd, entry->temporary_tables);
  entry->temporary_tables= NULL;

  my_free((void *) entry->db);
  my_free(entry);

  DBUG_VOID_RETURN;
}

bool init_hash_workers(ulong slave_parallel_workers)
{
  DBUG_ENTER("init_hash_workers");

  inited_hash_workers=
    (my_hash_init(&mapping_db_to_worker, &my_charset_bin,
                 0, 0, 0, get_key,
                 (my_hash_free_key) free_entry, 0) == 0);
  if (inited_hash_workers)
  {
#ifdef HAVE_PSI_INTERFACE
    mysql_mutex_init(key_mutex_slave_worker_hash, &slave_worker_hash_lock,
                     MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_cond_slave_worker_hash, &slave_worker_hash_cond, NULL);
#else
    mysql_mutex_init(NULL, &slave_worker_hash_lock,
                     MY_MUTEX_INIT_FAST);
    mysql_cond_init(NULL, &slave_worker_hash_cond, NULL);
#endif
  }

  DBUG_RETURN (!inited_hash_workers);
}

void destroy_hash_workers(Relay_log_info *rli)
{
  DBUG_ENTER("destroy_hash_workers");
  if (inited_hash_workers)
  {
    my_hash_free(&mapping_db_to_worker);
    mysql_mutex_destroy(&slave_worker_hash_lock);
    mysql_cond_destroy(&slave_worker_hash_cond);
    inited_hash_workers= false;
  }

  DBUG_VOID_RETURN;
}

/**
   Relocating temporary table reference into @c entry's table list head.
   Sources can be the coordinator's and the Worker's thd->temporary_tables.

   @param table   TABLE instance pointer
   @param thd     THD instance pointer of the source of relocation
   @param entry   db_worker_hash_entry instance pointer

   @note  thd->temporary_tables can become NULL

   @return the pointer to a table following the unlinked
*/
TABLE* mts_move_temp_table_to_entry(TABLE *table, THD *thd,
                                    db_worker_hash_entry *entry)
{
  TABLE *ret= table->next;

  if (table->prev)
  {
    table->prev->next= table->next;
    if (table->prev->next)
      table->next->prev= table->prev;
  }
  else
  {
    /* removing the first item from the list */
    DBUG_ASSERT(table == thd->temporary_tables);

    thd->temporary_tables= table->next;
    if (thd->temporary_tables)
      table->next->prev= 0;
  }
  table->next= entry->temporary_tables;
  table->prev= 0;
  if (table->next)
    table->next->prev= table;
  entry->temporary_tables= table;

  return ret;
}


/**
   Relocation of the list of temporary tables to thd->temporary_tables.

   @param thd     THD instance pointer of the destination
   @param temporary_tables
                  the source temporary_tables list

   @note     destroying references to the source list, if necessary,
             is left to the caller.

   @return   the post-merge value of thd->temporary_tables.
*/
TABLE* mts_move_temp_tables_to_thd(THD *thd, TABLE *temporary_tables)
{
  DBUG_ENTER ("mts_move_temp_tables_to_thd");
  TABLE *table= temporary_tables;
  if (!table)
    DBUG_RETURN(NULL);

  // accept only if this is the start of the list.
  DBUG_ASSERT(!table->prev);

  // walk along the source list and associate the tables with thd
  do
  {
    table->in_use= thd;
  } while(table->next && (table= table->next));

  // link the former list against the tail of the source list
  if (thd->temporary_tables)
    thd->temporary_tables->prev= table;
  table->next= thd->temporary_tables;
  thd->temporary_tables= temporary_tables;

  DBUG_RETURN(thd->temporary_tables);
}

/**
   Relocating references of temporary tables of a database
   of the entry argument from THD into the entry.

   @param thd    THD pointer of the source temporary_tables list
   @param entry  a pointer to db_worker_hash_entry record
                 containing database descriptor and temporary_tables list.

*/
static void move_temp_tables_to_entry(THD* thd, db_worker_hash_entry* entry)
{
  for (TABLE *table= thd->temporary_tables; table;)
  {
    if (strcmp(table->s->db.str, entry->db) == 0)
    {
      // table pointer is shifted inside the function
      table= mts_move_temp_table_to_entry(table, thd, entry);
    }
    else
    {
      table= table->next;
    }
  }
}


/**
   The function produces a reference to the struct of a Worker
   that has been or will be engaged to process the @c dbname -keyed  partition (D).
   It checks a local to Coordinator CGAP list first and returns
   @c last_assigned_worker when found (todo: assert).

   Otherwise, the partition is appended to the current group list:

        CGAP .= D

   here .= is concatenate operation,
   and a possible D's Worker id is searched in Assigned Partition Hash
   (APH) that collects tuples (P, W_id, U, mutex, cond).
   In case not found,

        W_d := W_c unless W_c is NULL.

   When W_c is NULL it is assigned to a least occupied as defined by
   @c get_least_occupied_worker().

        W_d := W_c := W_{least_occupied}

        APH .=  a new (D, W_d, 1)

   In a case APH contains W_d == W_c, (assert U >= 1)

        update APH set  U++ where  APH.P = D

   The case APH contains a W_d != W_c != NULL assigned to D-partition represents
   the hashing conflict and is handled as the following:

     a. marks the record of APH with a flag requesting to signal in the
        cond var when `U' the usage counter drops to zero by the other Worker;
     b. waits for the other Worker to finish tasks on that partition and
        gets the signal;
     c. updates the APH record to point to the first Worker (naturally, U := 1),
        scheduled the event, and goes back into the parallel mode

   @param  dbname      pointer to c-string containing database name
                       It can be empty string to indicate specific locking
                       to faciliate sequential applying.
   @param  rli         pointer to Coordinators relay-log-info instance
   @param  ptr_entry   reference to a pointer to the resulted entry in
                       the Assigne Partition Hash where
                       the entry's pointer is stored at return.
   @param  need_temp_tables
                       if FALSE migration of temporary tables not needed
   @param  last_worker caller opts for this Worker, it must be
                       rli->last_assigned_worker if one is determined.

   @note modifies  CGAP, APH and unlinks @c dbname -keyd temporary tables
         from C's thd->temporary_tables to move them into the entry record.

   @return the pointer to a Worker struct
*/
Slave_worker *map_db_to_worker(const char *dbname, Relay_log_info *rli,
                               db_worker_hash_entry **ptr_entry,
                               bool need_temp_tables, Slave_worker *last_worker)
{
  DYNAMIC_ARRAY *workers= &rli->workers;

  /*
    A dynamic array to store the mapping_db_to_worker hash elements
    that needs to be deleted, since deleting the hash entires while
    iterating over it is wrong.
  */
  DYNAMIC_ARRAY hash_element;
  THD *thd= rli->info_thd;

  DBUG_ENTER("get_slave_worker");

  DBUG_ASSERT(!rli->last_assigned_worker ||
              rli->last_assigned_worker == last_worker);
  DBUG_ASSERT(is_mts_db_partitioned(rli));

  if (!inited_hash_workers)
    DBUG_RETURN(NULL);

  db_worker_hash_entry *entry= NULL;
  my_hash_value_type hash_value;
  uchar dblength= (uint) strlen(dbname);


  // Search in CGAP
  for (db_worker_hash_entry **it= rli->curr_group_assigned_parts.begin();
       it != rli->curr_group_assigned_parts.end(); ++it)
  {
    entry= *it;

    if ((uchar) entry->db_len != dblength)
      continue;
    else
      if (strncmp(entry->db, const_cast<char*>(dbname), dblength) == 0)
      {
        *ptr_entry= entry;
        DBUG_RETURN(last_worker);
      }
  }

  DBUG_PRINT("info", ("Searching for %s, %d", dbname, dblength));

  hash_value= my_calc_hash(&mapping_db_to_worker, (uchar*) dbname,
                           dblength);

  mysql_mutex_lock(&slave_worker_hash_lock);

  entry= (db_worker_hash_entry *)
    my_hash_search_using_hash_value(&mapping_db_to_worker, hash_value,
                                    (uchar*) dbname, dblength);
  if (!entry)
  {
    /*
      The database name was not found which means that a worker never
      processed events from that database. In such case, we need to
      map the database to a worker my inserting an entry into the
      hash map.
    */
    my_bool ret;
    char *db= NULL;

    mysql_mutex_unlock(&slave_worker_hash_lock);

    DBUG_PRINT("info", ("Inserting %s, %d", dbname, dblength));
    /*
      Allocate an entry to be inserted and if the operation fails
      an error is returned.
    */
    if (!(db= (char *) my_malloc((size_t) dblength + 1, MYF(0))))
      goto err;
    if (!(entry= (db_worker_hash_entry *)
          my_malloc(sizeof(db_worker_hash_entry), MYF(0))))
    {
      my_free(db);
      goto err;
    }
    strmov(db, dbname);
    entry->db= db;
    entry->db_len= strlen(db);
    entry->usage= 1;
    entry->temporary_tables= NULL;
    /*
      Unless \exists the last assigned Worker, get a free worker based
      on a policy described in the function get_least_occupied_worker().
    */
    mysql_mutex_lock(&slave_worker_hash_lock);

    entry->worker= (!last_worker) ?
      get_least_occupied_worker(rli, workers, NULL) : last_worker;
    entry->worker->usage_partition++;
    if (mapping_db_to_worker.records > mts_partition_hash_soft_max)
    {
      /*
        remove zero-usage (todo: rare or long ago scheduled) records.
        Store the element of the hash in a dynamic array after checking whether
        the usage of the hash entry is 0 or not. We later free it from the HASH.
      */
      my_init_dynamic_array(&hash_element, sizeof(db_worker_hash_entry *),
                            HASH_DYNAMIC_INIT, HASH_DYNAMIC_INCR);
      for (uint i= 0; i < mapping_db_to_worker.records; i++)
      {
        DBUG_ASSERT(!entry->temporary_tables || !entry->temporary_tables->prev);
        DBUG_ASSERT(!thd->temporary_tables || !thd->temporary_tables->prev);

        db_worker_hash_entry *entry=
          (db_worker_hash_entry*) my_hash_element(&mapping_db_to_worker, i);

        if (entry->usage == 0)
        {
          mts_move_temp_tables_to_thd(thd, entry->temporary_tables);
          entry->temporary_tables= NULL;

          /* Push the element in the dynamic array*/
          push_dynamic(&hash_element, (uchar*) &entry);
        }
      }

      /* Delete the hash element based on the usage */
      for (uint i=0; i < hash_element.elements; i++)
      {
        db_worker_hash_entry *temp_entry= *(db_worker_hash_entry **) dynamic_array_ptr(&hash_element, i);
        my_hash_delete(&mapping_db_to_worker, (uchar*) temp_entry);
      }
        /* Deleting the dynamic array */
      delete_dynamic(&hash_element);
    }

    ret= my_hash_insert(&mapping_db_to_worker, (uchar*) entry);

    if (ret)
    {
      my_free(db);
      my_free(entry);
      entry= NULL;
      goto err;
    }
    DBUG_PRINT("info", ("Inserted %s, %d", entry->db, (int) strlen(entry->db)));
  }
  else
  {
    /* There is a record. Either  */
    if (entry->usage == 0)
    {
      entry->worker= (!last_worker) ?
        get_least_occupied_worker(rli, workers, NULL) : last_worker;
      entry->worker->usage_partition++;
      entry->usage++;
    }
    else if (entry->worker == last_worker || !last_worker)
    {

      DBUG_ASSERT(entry->worker);

      entry->usage++;
    }
    else
    {
      // The case APH contains a W_d != W_c != NULL assigned to
      // D-partition represents
      // the hashing conflict and is handled as the following:
      PSI_stage_info old_stage;

      DBUG_ASSERT(last_worker != NULL &&
                  rli->curr_group_assigned_parts.size() > 0);

      // future assignenment and marking at the same time
      entry->worker= last_worker;
      // loop while a user thread is stopping Coordinator gracefully
      do
      {
        thd->ENTER_COND(&slave_worker_hash_cond,
                                   &slave_worker_hash_lock,
                                   &stage_slave_waiting_worker_to_release_partition,
                                   &old_stage);
        mysql_cond_wait(&slave_worker_hash_cond, &slave_worker_hash_lock);
      } while (entry->usage != 0 && !thd->killed);

      thd->EXIT_COND(&old_stage);
      if (thd->killed)
      {
        entry= NULL;
        goto err;
      }
      mysql_mutex_lock(&slave_worker_hash_lock);
      entry->usage= 1;
      entry->worker->usage_partition++;
    }
  }

  /*
     relocation belonging to db temporary tables from C to W via entry
  */
  if (entry->usage == 1 && need_temp_tables)
  {
    if (!entry->temporary_tables)
    {
      if (entry->db_len != 0)
      {
        move_temp_tables_to_entry(thd, entry);
      }
      else
      {
        entry->temporary_tables= thd->temporary_tables;
        thd->temporary_tables= NULL;
      }
    }
#ifndef DBUG_OFF
    else
    {
      // all entries must have been emptied from temps by the caller

      for (TABLE *table= thd->temporary_tables; table; table= table->next)
      {
        DBUG_ASSERT(0 != strcmp(table->s->db.str, entry->db));
      }
    }
#endif
  }
  mysql_mutex_unlock(&slave_worker_hash_lock);

  DBUG_ASSERT(entry);

err:
  if (entry)
  {
    DBUG_PRINT("info",
               ("Updating %s with worker %lu", entry->db, entry->worker->id));
    rli->curr_group_assigned_parts.push_back(entry);
    *ptr_entry= entry;
  }
  DBUG_RETURN(entry ? entry->worker : NULL);
}

/**
   Get the least occupied worker.

   @param ws  dynarray of pointers to Slave_worker
   @return a pointer to chosen Slave_worker instance

*/
Slave_worker *get_least_occupied_worker(Relay_log_info *rli, DYNAMIC_ARRAY *ws,
                                        Log_event* ev)
{
  return rli->current_mts_submode->get_least_occupied_worker(rli, ws, ev);
}

/**
   Deallocation routine to cancel out few effects of
   @c map_db_to_worker().
   Involved into processing of the group APH tuples are updated.
   @c last_group_done_index member is set to the GAQ index of
   the current group.
   CGEP the Worker partition cache is cleaned up.

   @param ev     a pointer to Log_event
   @param error  error code after processing the event by caller.
*/
void Slave_worker::slave_worker_ends_group(Log_event* ev, int error)
{
  DBUG_ENTER("Slave_worker::slave_worker_ends_group");
  Slave_job_group *ptr_g= NULL;

  if (!error)
  {
    Slave_committed_queue *gaq= c_rli->gaq;
    ptr_g= gaq->get_job_group(gaq_index);

    DBUG_ASSERT(gaq_index == ev->mts_group_idx);
    /*
      It guarantees that the worker is removed from order commit queue when
      its transaction doesn't binlog anything. It will break innodb group commit,
      but it should rarely happen.
    */
    if (get_commit_order_manager())
      get_commit_order_manager()->report_commit(this);

    // first ever group must have relay log name
    DBUG_ASSERT(last_group_done_index != c_rli->gaq->size ||
                ptr_g->group_relay_log_name != NULL);
    DBUG_ASSERT(ptr_g->worker_id == id);

    if (ev->get_type_code() != XID_EVENT)
    {
      commit_positions(ev, ptr_g, false);
      DBUG_EXECUTE_IF("crash_after_commit_and_update_pos",
           sql_print_information("Crashing crash_after_commit_and_update_pos.");
           flush_info(TRUE);
           DBUG_SUICIDE();
      );
    }

    ptr_g->group_master_log_pos= group_master_log_pos;
    ptr_g->group_relay_log_pos= group_relay_log_pos;
    my_atomic_store32_tmp(&ptr_g->done, 1);
    last_group_done_index= gaq_index;
    last_groups_assigned_index= ptr_g->total_seqno;
    reset_gaq_index();
    groups_done++;
  }
  else
  {
    if (running_status != STOP_ACCEPTED)
    {
      // tagging as exiting so Coordinator won't be able synchronize with it
      mysql_mutex_lock(&jobs_lock);
      running_status= ERROR_LEAVING;
      mysql_mutex_unlock(&jobs_lock);

      /* Fatal error happens, it notifies the following transaction to rollback */
      if (get_commit_order_manager())
        get_commit_order_manager()->report_rollback(this);

      // Killing Coordinator to indicate eventual consistency error
      mysql_mutex_lock(&c_rli->info_thd->LOCK_thd_data);
      c_rli->info_thd->awake(THD::KILL_QUERY);
      mysql_mutex_unlock(&c_rli->info_thd->LOCK_thd_data);
    }
  }

  /*
    Cleanup relating to the last executed group regardless of error.
  */
  DYNAMIC_ARRAY *ep= &curr_group_exec_parts;

  if (current_mts_submode->get_type() == MTS_PARALLEL_TYPE_DB_NAME)
  {
    for (uint i= 0; i < ep->elements; i++)
    {
      db_worker_hash_entry *entry=
        *((db_worker_hash_entry **) dynamic_array_ptr(ep, i));

      mysql_mutex_lock(&slave_worker_hash_lock);

      DBUG_ASSERT(entry);

      entry->usage --;

      DBUG_ASSERT(entry->usage >= 0);

      if (entry->usage == 0)
      {
        usage_partition--;
        /*
          The detached entry's temp table list, possibly updated, remains
          with the entry at least until time Coordinator will deallocate it
          from the hash, that is either due to stop or extra size of the hash.
        */
        DBUG_ASSERT(usage_partition >= 0);
        DBUG_ASSERT(this->info_thd->temporary_tables == 0);
        DBUG_ASSERT(!entry->temporary_tables ||
                    !entry->temporary_tables->prev);

        if (entry->worker != this) // Coordinator is waiting
        {
#ifndef DBUG_OFF
          // TODO: open it! DBUG_ASSERT(usage_partition || !entry->worker->jobs.len);
#endif
          DBUG_PRINT("info",
                     ("Notifying entry %p release by worker %lu", entry, this->id));

          mysql_cond_signal(&slave_worker_hash_cond);
        }
      }
      else
        DBUG_ASSERT(usage_partition != 0);

      mysql_mutex_unlock(&slave_worker_hash_lock);
    }

    if (ep->elements > ep->max_element)
    {
      // reallocate to lessen mem
      ep->elements= ep->max_element;
      ep->max_element= 0;
      freeze_size(ep); // restores max_element
    }
    ep->elements= 0;

    if (error)
    {
      // Awakening Coordinator that could be waiting for entry release
      mysql_mutex_lock(&slave_worker_hash_lock);
      mysql_cond_signal(&slave_worker_hash_cond);
      mysql_mutex_unlock(&slave_worker_hash_lock);
    }
  }
  else // not DB-type scheduler
  {
    DBUG_ASSERT(current_mts_submode->get_type() ==
                MTS_PARALLEL_TYPE_LOGICAL_CLOCK);
    /*
      Check if there're any waiter. If there're try incrementing lwm and
      signal to those who've got sasfied with the waiting condition.

      In a "good" "likely" execution branch the waiter set is expected
      to be empty. LWM is advanced by Coordinator asynchronously.
      Also lwm is advanced by a dependent Worker when it inserts its waiting
      request into the waiting list.
    */
    Mts_submode_logical_clock* mts_submode=
      static_cast<Mts_submode_logical_clock*>(c_rli->current_mts_submode);
    longlong min_child_waited_logical_ts=
      my_atomic_load64_tmp(&mts_submode->min_waited_timestamp);

    DBUG_EXECUTE_IF("slave_worker_ends_group_before_signal_lwm",
                    {
                      const char act[]= "now WAIT_FOR worker_continue";
                      DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                         STRING_WITH_LEN(act)));
                    });

    if (unlikely(error))
    {
      mysql_mutex_lock(&c_rli->mts_gaq_LOCK);
      mts_submode->is_error= true;
      if (mts_submode->min_waited_timestamp != SEQ_UNINIT)
        mysql_cond_signal(&c_rli->logical_clock_cond);
      mysql_mutex_unlock(&c_rli->mts_gaq_LOCK);
    }
    else if (min_child_waited_logical_ts != SEQ_UNINIT)
    {
      mysql_mutex_lock(&c_rli->mts_gaq_LOCK);

      /*
        min_child_waited_logical_ts may include an old value, so we need to
        check it again after getting the lock.
      */
      if (mts_submode->min_waited_timestamp != SEQ_UNINIT) {
        longlong curr_lwm= mts_submode->get_lwm_timestamp(c_rli, true);

        if (mts_submode->clock_leq(mts_submode->min_waited_timestamp, curr_lwm))
        {
          /*
            There's a transaction that depends on the current.
          */
          mysql_cond_signal(&c_rli->logical_clock_cond);
        }
      }
      mysql_mutex_unlock(&c_rli->mts_gaq_LOCK);
    }

#ifndef DBUG_OFF
    curr_group_seen_sequence_number= false;
#endif
  }
  curr_group_seen_gtid= curr_group_seen_begin= false;

  DBUG_VOID_RETURN;
}


/**
   Class circular_buffer_queue.

   Content of the being dequeued item is copied to the arg-pointer
   location.

   @return the queue's array index that the de-queued item
           located at, or an error as an int outside the legacy
           [0, size) (value `size' is excluded) range.
*/

ulong circular_buffer_queue::de_queue(uchar *val)
{
  ulong ret;
  if (entry == size)
  {
    DBUG_ASSERT(len == 0);
    return (ulong) -1;
  }

  ret= entry;
  get_dynamic(&Q, val, entry);
  len--;

  // pre boundary cond
  if (avail == size)
    avail= entry;
  entry= (entry + 1) % size;

  // post boundary cond
  if (avail == entry)
    entry= size;

  DBUG_ASSERT(entry == size ||
              (len == (avail >= entry)? (avail - entry) :
               (size + avail - entry)));
  DBUG_ASSERT(avail != entry);

  return ret;
}

/**
   Similar to de_queue() but removing an item from the tail side.

   return  the queue's array index that the de-queued item
           located at, or an error.
*/
ulong circular_buffer_queue::de_tail(uchar *val)
{
  if (entry == size)
  {
    DBUG_ASSERT(len == 0);
    return (ulong) -1;
  }

  avail= (entry + len - 1) % size;
  get_dynamic(&Q, val, avail);
  len--;

  // post boundary cond
  if (avail == entry)
    entry= size;

  DBUG_ASSERT(entry == size ||
              (len == (avail >= entry)? (avail - entry) :
               (size + avail - entry)));
  DBUG_ASSERT(avail != entry);

  return avail;
}

/**
    @return  the index where the arg item has been located
             or an error.
*/
ulong circular_buffer_queue::en_queue(void *item)
{
  ulong ret;
  if (avail == size)
  {
    DBUG_ASSERT(avail == Q.elements);
    return (ulong) -1;
  }

  // store

  ret= avail;
  set_dynamic(&Q, (uchar*) item, avail);


  // pre-boundary cond
  if (entry == size)
    entry= avail;

  avail= (avail + 1) % size;
  len++;

  // post-boundary cond
  if (avail == entry)
    avail= size;

  DBUG_ASSERT(avail == entry ||
              len == (avail >= entry) ?
              (avail - entry) : (size + avail - entry));
  DBUG_ASSERT(avail != entry);

  return ret;
}

void* circular_buffer_queue::head_queue()
{
  uchar *ret= NULL;
  if (entry == size)
  {
    DBUG_ASSERT(len == 0);
  }
  else
  {
    get_dynamic(&Q, (uchar*) ret, entry);
  }
  return (void*) ret;
}

/**
   two index comparision to determine which of the two
   is ordered first.

   @note   The caller makes sure the args are within the valid
           range, incl cases the queue is empty or full.

   @return TRUE  if the first arg identifies a queue entity ordered
                 after one defined by the 2nd arg,
           FALSE otherwise.
*/
bool circular_buffer_queue::gt(ulong i, ulong k)
{
  DBUG_ASSERT(i < size && k < size);
  DBUG_ASSERT(avail != entry);

  if (i >= entry)
    if (k >= entry)
      return i > k;
    else
      return FALSE;
  else
    if (k >= entry)
      return TRUE;
    else
      return i > k;
}

#ifndef DBUG_OFF
bool Slave_committed_queue::count_done(Relay_log_info* rli)
{
  ulong i, k, cnt= 0;

  for (i= entry, k= 0; k < len; i= (i + 1) % size, k++)
  {
    Slave_job_group *ptr_g;

    ptr_g= (Slave_job_group *) dynamic_array_ptr(&Q, i);

    if (ptr_g->worker_id != (ulong) -1 && ptr_g->done)
      cnt++;
  }

  DBUG_ASSERT(cnt <= size);

  DBUG_PRINT("mts", ("Checking if it can simulate a crash:"
             " mts_checkpoint_group %u counter %lu parallel slaves %lu\n",
             opt_mts_checkpoint_group, cnt, rli->slave_parallel_workers));

  return (cnt == (rli->slave_parallel_workers * opt_mts_checkpoint_group));
}
#endif


/**
   The queue is processed from the head item by item
   to purge items representing committed groups.
   Progress in GAQ is assessed through comparision of GAQ index value
   with Worker's @c last_group_done_index.
   Purging breaks at a first discovered gap, that is an item
   that the assinged item->w_id'th Worker has not yet completed.

   The caller is supposed to be the checkpoint handler.

   A copy of the last discarded item containing
   the refreshed value of the committed low-water-mark is stored
   into @c lwm container member for further caller's processing.
   @c last_done is updated with the latest total_seqno for each Worker
   that was met during GAQ parse.

   @note dyn-allocated members of Slave_job_group such as
         group_relay_log_name as freed here.

   @return number of discarded items
*/
ulong Slave_committed_queue::move_queue_head(DYNAMIC_ARRAY *ws)
{
  DBUG_ENTER("Slave_committed_queue::move_queue_head");
  ulong i, cnt= 0;

  for (i= entry; i != avail && !empty(); cnt++, i= (i + 1) % size)
  {
    Slave_worker *w_i;
    Slave_job_group *ptr_g, g;
    char grl_name[FN_REFLEN];
    ulong ind MY_ATTRIBUTE((unused));

#ifndef DBUG_OFF
    if (DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0) &&
        cnt == opt_mts_checkpoint_period)
      DBUG_RETURN(cnt);
#endif

    grl_name[0]= 0;
    ptr_g= (Slave_job_group *) dynamic_array_ptr(&Q, i);

    /*
      The current job has not been processed or it was not
      even assigned, this means there is a gap.
    */
    if (ptr_g->worker_id == MTS_WORKER_UNDEF ||
        my_atomic_load32_tmp(&ptr_g->done) == 0)
      break; /* gap at i'th */

    /* Worker-id domain guard */
    compile_time_assert(MTS_WORKER_UNDEF > MTS_MAX_WORKERS);

    get_dynamic(ws, (uchar *) &w_i, ptr_g->worker_id);

    /*
      Memorizes the latest valid group_relay_log_name.
    */
    if (ptr_g->group_relay_log_name)
    {
      strcpy(grl_name, ptr_g->group_relay_log_name);
      my_free(ptr_g->group_relay_log_name);
      /*
        It is important to mark the field as freed.
      */
      ptr_g->group_relay_log_name= NULL;
    }

    /*
      Removes the job from the (G)lobal (A)ssigned (Q)ueue.
    */
    ind= de_queue((uchar*) &g);

    /*
      Stores the memorized name into the result struct. Note that we
      take care of the pointer first and then copy the other elements
      by assigning the structures.
    */
    if (grl_name[0] != 0)
    {
      strcpy(lwm.group_relay_log_name, grl_name);
    }
    g.group_relay_log_name= lwm.group_relay_log_name;
    lwm= g;

    DBUG_ASSERT(ind == i);
    DBUG_ASSERT(!ptr_g->group_relay_log_name);
    DBUG_ASSERT(ptr_g->total_seqno == lwm.total_seqno);
#ifndef DBUG_OFF
    {
      ulonglong l;
      get_dynamic(&last_done, (uchar *) &l, w_i->id);
      /*
        There must be some progress otherwise we should have
        exit the loop earlier.
      */
      DBUG_ASSERT(l < ptr_g->total_seqno);
    }
#endif
    /*
      This is used to calculate the last time each worker has
      processed events.
    */
    set_dynamic(&last_done, &ptr_g->total_seqno, w_i->id);
  }

  DBUG_ASSERT(cnt <= size);

  DBUG_RETURN(cnt);
}


/**
   Finds low-water mark of committed jobs in GAQ.
   That is an index below which all jobs are marked as done.

   Notice the first available index is returned when the queue
   does not have any incomplete jobs. That includes cases of
   the empty and the full of complete jobs queue.
   A mutex protecting from concurrent LWM change by
   move_queue_head() (by Coordinator) should be taken by the caller.

   @param arg_g [out]  a double pointer to Slave job descriptor item
                       last marked with done-as-true boolean.
   @param start_index  a GAQ index to start/resume searching.
                       Caller is to make sure the index points into
                       assigned (occupied) range of circular buffer of GAQ.
   @return             GAQ index of the last consecutive done job, or the GAQ
                       size when none is found.
*/
ulong Slave_committed_queue::find_lwm(Slave_job_group** arg_g,
                                      ulong start_index)
{
  Slave_job_group *ptr_g= NULL;
  ulong i, k, cnt;

  DBUG_ASSERT(start_index <= size);

  if (empty())
    return size;

  /*
    Loop continuation condition relies on
    (TODO: assert it)
    the start_index being in the running range:

       start_index \in [entry, avail - 1].

    It satisfies any queue size including 1.
    It does not satisfy the empty queue case which is bailed out earlier above.
  */
  for (i= start_index, cnt= 0; cnt < len - (start_index + size - entry) % size;
       i= (i + 1) % size, cnt++)
  {
    ptr_g= (Slave_job_group *) dynamic_array_ptr(&Q, i);

    if (my_atomic_load32_tmp(&ptr_g->done) == 0)
    {
      if (cnt == 0)
        return size;             // the first node of the queue is not done
      break;
    }
  }

  k= (i + size - 1) % size;
  ptr_g= (Slave_job_group *) dynamic_array_ptr(&Q, k);
  *arg_g= ptr_g;

  return k;
}


/**
   Method should be executed at slave system stop to
   cleanup dynamically allocated items that remained as unprocessed
   by Coordinator and Workers in their regular execution course.
*/
void Slave_committed_queue::free_dynamic_items()
{
  ulong i, k;
  for (i= entry, k= 0; k < len; i= (i + 1) % size, k++)
  {
    Slave_job_group *ptr_g= (Slave_job_group *) dynamic_array_ptr(&Q, i);
    if (ptr_g->group_relay_log_name)
    {
      my_free(ptr_g->group_relay_log_name);
    }
    if (ptr_g->checkpoint_log_name)
    {
      my_free(ptr_g->checkpoint_log_name);
    }
    if (ptr_g->checkpoint_relay_log_name)
    {
      my_free(ptr_g->checkpoint_relay_log_name);
    }
    if (ptr_g->group_master_log_name)
    {
      my_free(ptr_g->group_master_log_name);
    }
  }
  DBUG_ASSERT((avail == size /* full */ || entry == size /* empty */) ||
              i == avail /* all occupied are processed */);
}


void Slave_worker::do_report(loglevel level, int err_code, const char *msg,
                             va_list args) const
{
  char buff_coord[MAX_SLAVE_ERRMSG];
  char buff_gtid[Gtid::MAX_TEXT_LENGTH + 1];
  const char* log_name= const_cast<Slave_worker*>(this)->get_master_log_name();
  ulonglong log_pos= const_cast<Slave_worker*>(this)->get_master_log_pos();
  const Gtid_specification *gtid_next= &info_thd->variables.gtid_next;

  if (gtid_next->type == GTID_GROUP)
  {
    global_sid_lock->rdlock();
    gtid_next->to_string(global_sid_map, buff_gtid);
    global_sid_lock->unlock();
  }
  else
  {
    buff_gtid[0]= 0;
  }

  my_snprintf(buff_coord, sizeof(buff_coord),
           "Worker %lu failed executing transaction '%s' at "
           "master log %s, end_log_pos %llu",
           id, buff_gtid, log_name, log_pos);
  c_rli->va_report(level, err_code, buff_coord, msg, args);
}


#ifndef DBUG_OFF
static bool may_have_timestamp(Log_event *ev)
{
  bool res= false;

  switch (ev->get_type_code())
  {
  case QUERY_EVENT:
    res= true;
    break;

  case GTID_LOG_EVENT:
    res= true;
    break;

  default:
    break;
  }

  return res;
}


static longlong get_last_committed(Log_event *ev)
{
  longlong res= SEQ_UNINIT;

  switch (ev->get_type_code())
  {
  case QUERY_EVENT:
    res= static_cast<Query_log_event*>(ev)->last_committed;
    break;

  case GTID_LOG_EVENT:
    res= static_cast<Gtid_log_event*>(ev)->last_committed;
    break;

  default:
    break;
  }

  return res;
}


static longlong get_sequence_number(Log_event *ev)
{
  longlong res= SEQ_UNINIT;

  switch (ev->get_type_code())
  {
  case QUERY_EVENT:
    res= static_cast<Query_log_event*>(ev)->sequence_number;
    break;

  case GTID_LOG_EVENT:
    res= static_cast<Gtid_log_event*>(ev)->sequence_number;
    break;

  default:
    break;
  }

  return res;
}
#endif


/**
  MTS worker main routine.
  The worker thread loops in waiting for an event, executing it and
  fixing statistics counters.

  @param worker    a pointer to the assigned Worker struct
  @param rli       a pointer to Relay_log_info of Coordinator
                   to update statistics.

  @return 0 success
         -1 got killed or an error happened during appying
*/
int Slave_worker::slave_worker_exec_event(Log_event *ev)
{
  Relay_log_info *rli= c_rli;
  THD *thd= info_thd;
  int ret= 0;

  DBUG_ENTER("slave_worker_exec_event");

  thd->server_id = ev->server_id;
  thd->set_time();
  //thd->lex->set_current_select(0);
  if (!ev->when.tv_sec)
    ev->when.tv_sec= my_time(0);
  ev->thd= thd; // todo: assert because up to this point, ev->thd == 0
  ev->worker= this;

#ifndef DBUG_OFF
  if (!is_mts_db_partitioned(rli) && may_have_timestamp(ev) &&
      !curr_group_seen_sequence_number)
  {
    curr_group_seen_sequence_number= true;

    longlong lwm_estimate= static_cast<Mts_submode_logical_clock*>
      (rli->current_mts_submode)->estimate_lwm_timestamp();
    longlong last_committed, sequence_number;

    last_committed= get_last_committed(ev);
    sequence_number= get_sequence_number(ev);
    /*
      The commit timestamp waiting condition:

        lwm_estimate < last_committed  <=>  last_committed  \not <= lwm_estimate

      must have been satisfied by Coordinator.
      The first scheduled transaction does not have to wait for anybody.
    */
    DBUG_ASSERT(rli->gaq->entry == ev->mts_group_idx ||
                Mts_submode_logical_clock::clock_leq(last_committed,
                                                     lwm_estimate));
    DBUG_ASSERT(lwm_estimate != SEQ_UNINIT || rli->gaq->entry == ev->mts_group_idx);
    /*
      The current transaction's timestamp can't be less that lwm.
    */
    DBUG_ASSERT(sequence_number == SEQ_UNINIT ||
                !Mts_submode_logical_clock::
                clock_leq(sequence_number,
                          static_cast<Mts_submode_logical_clock*>
                          (rli->current_mts_submode)->
                          estimate_lwm_timestamp()));
  }
#endif

  // Address partioning only in database mode
  if (!is_gtid_event(ev) && is_mts_db_partitioned(rli))
  {
    if (ev->contains_partition_info(end_group_sets_max_dbs))
    {
      uint num_dbs= ev->mts_number_dbs();
      DYNAMIC_ARRAY *ep= &curr_group_exec_parts;

      if (num_dbs == OVER_MAX_DBS_IN_EVENT_MTS)
        num_dbs= 1;

      DBUG_ASSERT(num_dbs > 0);

      for (uint k= 0; k < num_dbs; k++)
      {
        bool found= false;

        for (uint i= 0; i < ep->elements && !found; i++)
        {
          found=
            *((db_worker_hash_entry **) dynamic_array_ptr(ep, i)) ==
            ev->mts_assigned_partitions[k];
        }
        if (!found)
        {
          /*
            notice, can't assert
            DBUG_ASSERT(ev->mts_assigned_partitions[k]->worker == worker);
            since entry could be marked as wanted by other worker.
          */
          insert_dynamic(ep, (uchar*) &ev->mts_assigned_partitions[k]);
        }
      }
      end_group_sets_max_dbs= false;
    }
  }

  set_future_event_relay_log_pos(ev->future_event_relay_log_pos);
  set_master_log_pos(ev->log_pos);
  set_gaq_index(ev->mts_group_idx);

  ret= ev->do_apply_event_worker(this);
  DBUG_RETURN(ret);
}

/**
  Sleep for a given amount of seconds or until killed.

  @param seconds    The number of seconds to sleep.

  @retval True if the thread has been killed, false otherwise.
*/

bool Slave_worker::worker_sleep(ulong seconds)
{
  bool ret= false;
  struct timespec abstime;
  mysql_mutex_t *lock= &jobs_lock;
  mysql_cond_t *cond= &jobs_cond;

  /* Absolute system time at which the sleep time expires. */
  set_timespec(abstime, seconds);

  mysql_mutex_lock(lock);
  info_thd->ENTER_COND(cond, lock, NULL, NULL);

  while (!(ret= info_thd->killed || running_status != RUNNING))
  {
    int error= mysql_cond_timedwait(cond, lock, &abstime);
    if (error == ETIMEDOUT || error == ETIME)
      break;
  }

  info_thd->EXIT_COND(NULL); /* unlocked lock inside (even arg is NULL) */
  return ret;
}

/**
  It is called after an error happens. It checks if that is an temporary
  error and if the situation is allow to retry the transaction. Then it will
  retry the transaction if it is allowed. Retry policy and logic is similar to
  single-threaded slave.

  @param[in] start_relay_number The extension number of the relay log which
               includes the first event of the transaction.
  @param[in] start_relay_pos The offset of the transaction's first event.

  @param[in] end_relay_number The extension number of the relay log which
               includes the last event it should retry.
  @param[in] end_relay_pos The offset of the last event it should retry.

  @return false if succeeds, otherwise returns true.
*/
bool Slave_worker::retry_transaction(uint start_relay_number,
                                     my_off_t start_relay_pos,
                                     uint end_relay_number,
                                     my_off_t end_relay_pos)
{
  THD *thd= info_thd;
  bool silent= false;

  DBUG_ENTER("Slave_worker::retry_transaction");

  if (slave_trans_retries == 0)
    DBUG_RETURN(true);

  do
  {
    /* Simulate a lock deadlock error */
    uint error= 0;

    if (found_order_commit_deadlock())
      error= ER_LOCK_DEADLOCK;

    if (!has_temporary_error(thd, error, &silent) ||
        thd->transaction.all.cannot_safely_rollback())
      DBUG_RETURN(true);

    if (trans_retries >= slave_trans_retries)
    {
      thd->is_fatal_error= 1;
      c_rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(),
                    "worker thread retried transaction %lu time(s) "
                    "in vain, giving up. Consider raising the value of "
                    "the slave_transaction_retries variable.", trans_retries);
      DBUG_RETURN(true);
    }

    if (!silent)
      trans_retries++;

    mysql_mutex_lock(&c_rli->data_lock);
    c_rli->retried_trans++;
    mysql_mutex_unlock(&c_rli->data_lock);

    cleanup_context(thd, 1);
    reset_order_commit_deadlock();
    worker_sleep(min<ulong>(trans_retries, MAX_SLAVE_RETRY_PAUSE));

  } while (read_and_apply_events(start_relay_number, start_relay_pos,
                                 end_relay_number, end_relay_pos));
  DBUG_RETURN(false);
}

/**
  Read events from relay logs and apply them.

  @param[in] start_relay_number The extension number of the relay log which
               includes the first event of the transaction.
  @param[in] start_relay_pos The offset of the transaction's first event.

  @param[in] end_relay_number The extension number of the relay log which
               includes the last event it should retry.
  @param[in] end_relay_pos The offset of the last event it should retry.

  @return false if succeeds, otherwise returns true.
*/
bool Slave_worker::read_and_apply_events(uint start_relay_number,
                                         my_off_t start_relay_pos,
                                         uint end_relay_number,
                                         my_off_t end_relay_pos)
{
  DBUG_ENTER("Slave_worker::read_and_apply_events");

  Relay_log_info *rli= c_rli;
  IO_CACHE relay_io;
  char file_name[FN_REFLEN+1];
  uint file_number= start_relay_number;
  bool error= true;
  bool arrive_end= false;

  if (relay_log_number_to_name(start_relay_number, file_name) != 0) {
    /* Nothing else to do: caller would handle the failure 
     * - Just as cannot open relay log file */
    sql_print_error("Failed to generate relay log name");
    DBUG_RETURN(error);
  }

  if (my_memset(&relay_io, sizeof(IO_CACHE), 0, sizeof(IO_CACHE), 
               sql_print_error) == NULL) {
    sql_print_error("Failed to initialize relay log IO cashe object");
    /* Nothing else to do: caller would handle the failure */
    DBUG_RETURN(error);
  }

  while (!arrive_end)
  {
    Log_event *ev= NULL;

    if (!my_b_inited(&relay_io))
    {
      const char *errmsg;

      DBUG_PRINT("info", ("Open relay log %s", file_name));

      if (open_binlog_file(&relay_io, file_name, &errmsg) == -1)
      {
        sql_print_error("Failed to open relay log %s, error: %s", file_name,
                        errmsg);
        goto end;
      }
      my_b_seek(&relay_io, start_relay_pos);
    }

    /* If it is the last event, then set arrive_end as true */
    arrive_end= (my_b_tell(&relay_io) == end_relay_pos &&
                 file_number == end_relay_number);

    ev= Log_event::read_log_event(&relay_io, NULL,
                                  rli->get_rli_description_event(),
                                  opt_slave_sql_verify_checksum);
    if (ev != NULL)
    {
      /* It is a event belongs to the transaction */
      if (!ev->is_mts_sequential_exec(rli->current_mts_submode->get_type() ==
                                      MTS_PARALLEL_TYPE_DB_NAME))
      {
        int ret = 0;

        ev->future_event_relay_log_pos= my_b_tell(&relay_io);
        ev->mts_group_idx= gaq_index;

        if (is_mts_db_partitioned(rli) && ev->contains_partition_info(true))
          assign_partition_db(ev);

        ret = slave_worker_exec_event(ev);
        if (ev->worker != NULL)
        {
          delete ev;
          /* comment to fix coverity error: stored value is not used ev = NULL; */
        }
        if (ret != 0) {
            goto end;
        }
      }
      else
      {
          /*
             It is a Rotate_log_event, Format_description_log_event event or other
             type event doesn't belong to the transaction.
           */
           delete ev;
          /* comment to fix coverity error: stored value is not used  ev= NULL; */
      }
    }
    else
    {
      /*
        IO error happens if relay_io.error != 0, otherwise it arrives the
        end of the relay log
      */
      if (relay_io.error != 0)
      {
        sql_print_error("Error when worker read relay log events,"
                        "relay log name %s, position %llu",
                        rli->get_event_relay_log_name(), my_b_tell(&relay_io));
        goto end;
      }

      if (rli->relay_log.find_next_relay_log(file_name))
      {
        sql_print_error("Failed to find next relay log when retrying the "
                        "transaction, current relay log is %s", file_name);
        goto end;
      }

      file_number= relay_log_name_to_number(file_name);

      end_io_cache(&relay_io);
      mysql_file_close(relay_io.file, MYF(0));
      start_relay_pos= BIN_LOG_HEADER_SIZE;
    }
  }

  error= false;
end:
  if (my_b_inited(&relay_io))
  {
    end_io_cache(&relay_io);
    mysql_file_close(relay_io.file, MYF(0));
  }
  DBUG_RETURN(error);
}

/*
  Find database entry from map_db_to_worker hash table.
 */
static db_worker_hash_entry *find_entry_from_db_map(const char *dbname)
{
  db_worker_hash_entry *entry= NULL;
  my_hash_value_type hash_value;
  uchar dblength= (uint) strlen(dbname);

  hash_value= my_calc_hash(&mapping_db_to_worker, (const uchar*) dbname,
                           dblength);

  mysql_mutex_lock(&slave_worker_hash_lock);

  entry= (db_worker_hash_entry *)
    my_hash_search_using_hash_value(&mapping_db_to_worker, hash_value,
                                    (uchar*) dbname, dblength);

  mysql_mutex_unlock(&slave_worker_hash_lock);
  return entry;
}

/*
  Initialize Log_event::mts_assigned_partitions array. It is for transaction
  retry and is only called when retrying a transaction by workers.
*/
void Slave_worker::assign_partition_db(Log_event *ev)
{
  Mts_db_names mts_dbs;
  int i;

  ev->get_mts_dbs(&mts_dbs);

  if (mts_dbs.num == OVER_MAX_DBS_IN_EVENT_MTS)
    ev->mts_assigned_partitions[0]= find_entry_from_db_map("");
  else
    for (i= 0; i < mts_dbs.num; i++)
      ev->mts_assigned_partitions[i]= find_entry_from_db_map(mts_dbs.name[i]);
}


// returns the next available! (TODO: incompatible to circurla_buff method!!!)
static int en_queue(Slave_jobs_queue *jobs, Slave_job_item *item)
{
  if (jobs->avail == jobs->size)
  {
    DBUG_ASSERT(jobs->avail == jobs->Q.elements);
    return -1;
  }

  // store

  set_dynamic(&jobs->Q, (uchar*) item, jobs->avail);

  // pre-boundary cond
  if (jobs->entry == jobs->size)
    jobs->entry= jobs->avail;

  jobs->avail= (jobs->avail + 1) % jobs->size;
  jobs->len++;

  // post-boundary cond
  if (jobs->avail == jobs->entry)
    jobs->avail= jobs->size;
  DBUG_ASSERT(jobs->avail == jobs->entry ||
              jobs->len == (jobs->avail >= jobs->entry) ?
              (jobs->avail - jobs->entry) : (jobs->size + jobs->avail - jobs->entry));
  return jobs->avail;
}

/**
   return the value of @c data member of the head of the queue.
*/
void * head_queue(Slave_jobs_queue *jobs, Slave_job_item *ret)
{
  if (jobs->entry == jobs->size)
  {
    DBUG_ASSERT(jobs->len == 0);
    ret->data= NULL;               // todo: move to caller
    return NULL;
  }
  get_dynamic(&jobs->Q, (uchar*) ret, jobs->entry);

  DBUG_ASSERT(ret->data);         // todo: move to caller

  return ret;
}


/**
   return a job item through a struct which point is supplied via argument.
*/
Slave_job_item * de_queue(Slave_jobs_queue *jobs, Slave_job_item *ret)
{
  if (jobs->entry == jobs->size)
  {
    DBUG_ASSERT(jobs->len == 0);
    return NULL;
  }
  get_dynamic(&jobs->Q, (uchar*) ret, jobs->entry);
  jobs->len--;

  // pre boundary cond
  if (jobs->avail == jobs->size)
    jobs->avail= jobs->entry;
  jobs->entry= (jobs->entry + 1) % jobs->size;

  // post boundary cond
  if (jobs->avail == jobs->entry)
    jobs->entry= jobs->size;

  DBUG_ASSERT(jobs->entry == jobs->size ||
              (jobs->len == (jobs->avail >= jobs->entry) ?
               (jobs->avail - jobs->entry) :
               (jobs->size + jobs->avail - jobs->entry)));

  return ret;
}

/**
   Coordinator enqueues a job item into a Worker private queue.

   @param job_item  a pointer to struct carrying a reference to an event
   @param worker    a pointer to the assigned Worker struct
   @param rli       a pointer to Relay_log_info of Coordinator

   @return false Success.
           true  Thread killed or worker stopped while waiting for
                 successful enqueue.
*/
bool append_item_to_jobs(slave_job_item *job_item,
                         Slave_worker *worker, Relay_log_info *rli)
{
  THD *thd= rli->info_thd;
  int ret= -1;
  ulong ev_size= ((Log_event*) (job_item->data))->data_written;
  ulonglong new_pend_size;
  PSI_stage_info old_stage;

  DBUG_ASSERT(thd == current_thd);

  mysql_mutex_lock(&rli->pending_jobs_lock);
  new_pend_size= rli->mts_pending_jobs_size + ev_size;
  bool big_event= (ev_size > rli->mts_pending_jobs_size_max);
  /*
    C waits basing on *data* sizes in the queues.
    If it is a big event (event size is greater than
    slave_pending_jobs_size_max but less than slave_max_allowed_packet),
    it will wait for all the jobs in the workers's queue to be
    completed. If it is normal event (event size is less than
    slave_pending_jobs_size_max), then it will wait for
    enough empty memory to keep the event in one of the workers's
    queue.
    NOTE: Receiver thread (I/O thread) is taking care of restricting
    the event size to slave_max_allowed_packet. If an event from
    the master is bigger than this value, IO thread will be stopped
    with error ER_NET_PACKET_TOO_LARGE.
  */
  while ( (!big_event && new_pend_size > rli->mts_pending_jobs_size_max)
          || (big_event && rli->mts_pending_jobs_size != 0 ))
  {
    rli->mts_wq_oversize= TRUE;
    rli->wq_size_waits_cnt++; // waiting due to the total size
    thd->ENTER_COND(&rli->pending_jobs_cond, &rli->pending_jobs_lock,
                    &stage_slave_waiting_worker_to_free_events, &old_stage);
    mysql_cond_wait(&rli->pending_jobs_cond, &rli->pending_jobs_lock);
    thd->EXIT_COND(&old_stage);
    if (thd->killed)
      return true;
    if (log_warnings > 1 && (rli->wq_size_waits_cnt % 10 == 1))
      sql_print_information("Multi-threaded slave: Coordinator has waited "
                            "%lu times hitting slave_pending_jobs_size_max; "
                            "current event size = %lu.",
                            rli->wq_size_waits_cnt, ev_size);
    mysql_mutex_lock(&rli->pending_jobs_lock);

    new_pend_size= rli->mts_pending_jobs_size + ev_size;
  }
  rli->pending_jobs++;
  rli->mts_pending_jobs_size= new_pend_size;
  rli->mts_events_assigned++;
  mysql_mutex_unlock(&rli->pending_jobs_lock);

  /*
    Sleep unless there is an underrunning Worker and the current Worker
    queue is empty or filled lightly (not more than underrun level).
  */
  if (rli->mts_wq_underrun_w_id == MTS_WORKER_UNDEF &&
      worker->jobs.len > worker->underrun_level)
  {
    /*
      todo: experiment with weight to get a good approximation formula.
      Max possible nap time is choosen 1 ms.
      The bigger the excessive overrun counter the longer the nap.
    */
    ulong nap_weight= rli->mts_wq_excess_cnt + 1;
    /*
       Nap time is a product of a weight factor and the basic nap unit.
       The weight factor is proportional to the worker queues overrun excess
       counter. For example when there were only one overruning Worker
       the max nap_weight as 0.1 * worker->jobs.size would be
       about 1600 so the max nap time is approx 0.008 secs.
       Such value is not reachable because of min().
       Notice, granularity of sleep depends on the resolution of the software
       clock, High-Resolution Timer (HRT) configuration. Without HRT
       the precision of wake-up through @c select() may be greater or
       equal 1 ms. So don't expect the nap last a prescribed fraction of 1 ms
       in such case.
    */
    my_sleep(min<ulong>(1000, nap_weight * rli->mts_coordinator_basic_nap));
    rli->mts_wq_no_underrun_cnt++;
  }

  mysql_mutex_lock(&worker->jobs_lock);

  // possible WQ overfill
  while (worker->running_status == Slave_worker::RUNNING && !thd->killed &&
         (ret= en_queue(&worker->jobs, job_item)) == -1)
  {
    thd->ENTER_COND(&worker->jobs_cond, &worker->jobs_lock,
                    &stage_slave_waiting_worker_queue, &old_stage);
    worker->jobs.overfill= TRUE;
    worker->jobs.waited_overfill++;
    rli->mts_wq_overfill_cnt++;
    mysql_cond_wait(&worker->jobs_cond, &worker->jobs_lock);
    thd->EXIT_COND(&old_stage);

    mysql_mutex_lock(&worker->jobs_lock);
  }
  if (ret != -1)
  {
    worker->curr_jobs++;
    if (worker->jobs.len == 1)
      mysql_cond_signal(&worker->jobs_cond);

    mysql_mutex_unlock(&worker->jobs_lock);
  }
  else
  {
    mysql_mutex_unlock(&worker->jobs_lock);

    mysql_mutex_lock(&rli->pending_jobs_lock);
    rli->pending_jobs--;                  // roll back of the prev incr
    rli->mts_pending_jobs_size -= ev_size;
    mysql_mutex_unlock(&rli->pending_jobs_lock);
  }

  return (-1 != ret ? false : true);
}


/**
  Remove a job item from the given workers job queue. It also updates related
  status.

  param[in] job_item The job item will be removed
  param[in] worker   The worker which job_item belongs to.
  param[in] rli      slave's relay log info object.
 */
static void remove_item_from_jobs(slave_job_item *job_item,
                                  Slave_worker *worker, Relay_log_info *rli)
{
  Log_event *ev= static_cast<Log_event *>(job_item->data);

  mysql_mutex_lock(&worker->jobs_lock);
  de_queue(&worker->jobs, job_item);
  /* possible overfill */
  if (worker->jobs.len == worker->jobs.size - 1 &&
      worker->jobs.overfill == TRUE)
  {
    worker->jobs.overfill= false;
    // todo: worker->hungry_cnt++;
    mysql_cond_signal(&worker->jobs_cond);
  }
  mysql_mutex_unlock(&worker->jobs_lock);

  /* statistics */

  /* todo: convert to rwlock/atomic write */
  mysql_mutex_lock(&rli->pending_jobs_lock);

  rli->pending_jobs--;
  rli->mts_pending_jobs_size-= ev->data_written;
  DBUG_ASSERT(rli->mts_pending_jobs_size < rli->mts_pending_jobs_size_max);

  /*
    The positive branch is underrun: number of pending assignments
    is less than underrun level.
    Zero of jobs.len has to reset underrun w_id as the worker may get
    the next piece of assignement in a long time.
  */
  if (worker->underrun_level > worker->jobs.len && worker->jobs.len != 0)
  {
    rli->mts_wq_underrun_w_id= worker->id;
  } else if (rli->mts_wq_underrun_w_id == worker->id)
  {
    // reset only own marking
    rli->mts_wq_underrun_w_id= MTS_WORKER_UNDEF;
  }

  /*
    Overrun handling.
    Incrementing the Worker private and the total excess counter corresponding
    to number of events filled above the overrun_level.
    The increment amount to the total counter is a difference between
    the current and the previous private excess (worker->wq_overrun_cnt).
    When the current queue length drops below overrun_level the global
    counter is decremented, the local is reset.
  */
  if (worker->overrun_level < worker->jobs.len)
  {
    ulong last_overrun= worker->wq_overrun_cnt;
    ulong excess_delta;

    /* current overrun */
    worker->wq_overrun_cnt= worker->jobs.len - worker->overrun_level;
    excess_delta= worker->wq_overrun_cnt - last_overrun;
    worker->excess_cnt+= excess_delta;
    rli->mts_wq_excess_cnt+= excess_delta;
    rli->mts_wq_overrun_cnt++;  // statistics

    // guarding correctness of incrementing in case of the only one Worker
    DBUG_ASSERT(rli->workers.elements != 1 ||
                rli->mts_wq_excess_cnt == worker->wq_overrun_cnt);
  }
  else if (worker->excess_cnt > 0)
  {
    // When level drops below the total excess is decremented by the
    // value of the worker's contribution to the total excess.
    rli->mts_wq_excess_cnt-= worker->excess_cnt;
    worker->excess_cnt= 0;
    worker->wq_overrun_cnt= 0; // and the local is reset

    DBUG_ASSERT(rli->mts_wq_excess_cnt >= 0);
    DBUG_ASSERT(rli->mts_wq_excess_cnt == 0 || rli->workers.elements > 1);

  }

  /* coordinator can be waiting */
  if (rli->mts_pending_jobs_size < rli->mts_pending_jobs_size_max &&
      rli->mts_wq_oversize)  // TODO: unit/general test wq_oversize
  {
    rli->mts_wq_oversize= FALSE;
    mysql_cond_signal(&rli->pending_jobs_cond);
  }

  mysql_mutex_unlock(&rli->pending_jobs_lock);

  worker->events_done++;
}


/**
   Worker's routine to wait for a new assignement through
   @c append_item_to_jobs()

   @param worker    a pointer to the waiting Worker struct
   @param job_item  a pointer to struct carrying a reference to an event

   @return NULL failure or
           a-pointer to an item.
*/
struct slave_job_item* pop_jobs_item(Slave_worker *worker, Slave_job_item *job_item)
{
  THD *thd= worker->info_thd;

  mysql_mutex_lock(&worker->jobs_lock);

  job_item->data= NULL;
  while (!job_item->data && !thd->killed &&
         (worker->running_status == Slave_worker::RUNNING ||
          worker->running_status == Slave_worker::STOP))
  {
    PSI_stage_info old_stage;
    if (set_max_updated_index_on_stop(worker, job_item))
      break;
    if (job_item->data == NULL)
    {
      worker->wq_empty_waits++;
      thd->ENTER_COND(&worker->jobs_cond, &worker->jobs_lock,
                               &stage_slave_waiting_event_from_coordinator,
                               &old_stage);
      mysql_cond_wait(&worker->jobs_cond, &worker->jobs_lock);
      thd->EXIT_COND(&old_stage);
      mysql_mutex_lock(&worker->jobs_lock);
    }
  }
  if (job_item->data)
    worker->curr_jobs--;

  mysql_mutex_unlock(&worker->jobs_lock);

  thd_proc_info(worker->info_thd, "Executing event");
  return job_item;
}


/**
  apply one job group.

  @note the function maintains worker's CGEP and modifies APH, updates
        the current group item in GAQ via @c slave_worker_ends_group().

  param[in] worker the worker which calls it.
  param[in] rli    slave's relay log info object.

  return returns 0 if the group of jobs are applied successfully, otherwise
         returns an error code.
 */
int slave_worker_exec_job_group(Slave_worker *worker, Relay_log_info *rli)
{
  struct slave_job_item item= {NULL, 0, 0};
  struct slave_job_item *job_item= &item;
  THD *thd= worker->info_thd;
  bool seen_begin= false;
  int error= 0;
  Log_event *ev= NULL;
  uint start_relay_number;
  my_off_t start_relay_pos;

  DBUG_ENTER("slave_worker_exec_job_group");

  if (unlikely(worker->trans_retries > 0))
    worker->trans_retries= 0;

  job_item= pop_jobs_item(worker, job_item);
  start_relay_number= job_item->relay_number;
  start_relay_pos= job_item->relay_pos;

  while (1)
  {
    if (thd->killed || worker->running_status == Slave_worker::STOP_ACCEPTED)
    {
      DBUG_ASSERT(worker->running_status != Slave_worker::ERROR_LEAVING);
      // de-queueing and decrement counters is in the caller's exit branch
      error= -1;
      goto err;
    }

    ev= static_cast<Log_event*>(job_item->data);
    DBUG_ASSERT(ev != NULL);
    DBUG_PRINT("slave_worker_exec_job:", ("W_%lu <- job item: %p data: %p thd: %p",
               worker->id, job_item, ev, thd));

    if (!seen_begin && ev->starts_group())
    {
      seen_begin= true; // The current group is started with B-event
      worker->end_group_sets_max_dbs= true;
    }

    set_timespec_nsec(worker->ts_exec[0], 0); // pre-exec
    worker->stats_read_time += diff_timespec(worker->ts_exec[0],
                                             worker->ts_exec[1]);

    error= worker->slave_worker_exec_event(ev);

    set_timespec_nsec(worker->ts_exec[1], 0); // pre-exec
    worker->stats_exec_time += diff_timespec(worker->ts_exec[1],
                                             worker->ts_exec[0]);
    if (error || worker->found_order_commit_deadlock())
    {
      error= worker->retry_transaction(start_relay_number, start_relay_pos,
                                        job_item->relay_number,
                                        job_item->relay_pos);
      if (error)
          goto err;
    }
    /*
      p-event or any other event of B-free (malformed) group can
      "commit" with logical clock scheduler. In that case worker id
      points to the only active "exclusive" Worker that processes such
      malformed group events one by one.
    */
    DBUG_ASSERT(seen_begin || is_gtid_event(ev) ||
                ev->get_type_code() == QUERY_EVENT ||
                is_mts_db_partitioned(rli) || worker->id == 0);

    if (ev->ends_group() ||
        (!seen_begin && !is_gtid_event(ev) && (ev->get_type_code() == QUERY_EVENT ||
                                               !is_mts_db_partitioned(rli))))
      break;

    remove_item_from_jobs(job_item, worker, rli);
    /* The event will be used later if worker is NULL, so it is not freed */
    if (ev->worker != NULL) {
      delete ev;
      ev = NULL;
    }

    job_item= pop_jobs_item(worker, job_item);
  }

  DBUG_PRINT("info", (" commits GAQ index %lu, last committed  %lu",
                      ev->mts_group_idx, worker->last_group_done_index));
  /* The group is applied successfully, so error should be 0 */
  worker->slave_worker_ends_group(ev, 0);

#ifndef DBUG_OFF
  DBUG_PRINT("mts", ("Check_slave_debug_group worker %lu mts_checkpoint_group"
                     " %u processed %lu debug %d\n", worker->id, opt_mts_checkpoint_group,
                     worker->groups_done,
                     DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0)));

  if (DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0) &&
      opt_mts_checkpoint_group == worker->groups_done)
  {
    DBUG_PRINT("mts", ("Putting worker %lu in busy wait.", worker->id));
    while (true) my_sleep(6000000);
  }
#endif

  remove_item_from_jobs(job_item, worker, rli);
  delete ev;

  DBUG_RETURN(0);

err:
  if (error)
  {
    sql_print_information("Worker %lu is exiting: killed %i, error %i, "
                          "running_status %d",
                          worker->id, thd->killed, thd->is_error(),
                          worker->running_status);
    /* If error is non-zero, ev would not be used in the following function */ 
    worker->slave_worker_ends_group(ev, error); /* last done sets post exec */
    /* TODO: Reach here, ev could be NULL: May need special handling or handled by caller? */
  }
  DBUG_RETURN(error);
}
