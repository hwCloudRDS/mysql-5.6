/* Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.

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
#ifndef MTS_SUBMODE_H
#define MTS_SUBMODE_H

#include "log.h"
#include "log_event.h"
#include "rpl_rli.h"

/**
 * Temporary functions for using gcc atomic build-ins for memory read-write
 * - MySQL 5.7 used the gcc atomic build-in and removed the related atomic
 *   locks, see WL#7655, etc. (Should use GCC 4.7 or up)
 * - Here, in order to keep the performance during back-porting the MTS
 *   related features, we have the following temporary functions, which
 *   would be removed when we back-port the 5.7's using gcc atomic build-ins
 */
static inline void my_atomic_store64_tmp(int64 volatile *a, int64 v)
{
  __atomic_store_n(a, v, __ATOMIC_SEQ_CST);
}

static inline int64 my_atomic_add64_tmp(int64 volatile *a, int64 v)
{
  return __atomic_fetch_add(a, v, __ATOMIC_SEQ_CST);
}

static inline int64 my_atomic_load64_tmp(int64 volatile *a)
{
  return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}

static inline int my_atomic_cas64_tmp(int64 volatile *a, int64 *cmp, int64 set)
{
  return __atomic_compare_exchange_n(a, cmp, set, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline void my_atomic_store32_tmp(int32 volatile *a, int32 v)
{
  __atomic_store_n(a, v, __ATOMIC_SEQ_CST);
}

static inline int32 my_atomic_load32_tmp(int32 volatile *a)
{
  return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}


class Mts_submode_database;
class Mts_submode_logical_clock;

// Extend the following class as per requirement for each sub mode
class Mts_submode
{
private:

protected:

  Mts_submode(enum_mts_parallel_type type_) : type(type_) {}

  /* Parallel type */
  enum_mts_parallel_type  type;
public:
  inline enum_mts_parallel_type get_type(){return type;}
  // pure virtual methods. Should be extended in the derieved class

  /* Logic to schedule the next event. called at the B event for each
     transaction */
  virtual int schedule_next_event(Relay_log_info* rli,
                                   Log_event *ev)= 0;

  /* logic to attach temp tables Should be extended in the derieved class */
  virtual void attach_temp_tables(THD *thd, const Relay_log_info* rli,
                                  Query_log_event *ev)= 0;

  /* logic to detach temp tables. Should be extended in the derieved class  */
  virtual void detach_temp_tables(THD *thd, const Relay_log_info* rli,
                                  Query_log_event *ev)= 0;

  /* returns the least occupied worker. Should be extended in the derieved class  */
  virtual Slave_worker* get_least_occupied_worker(Relay_log_info* rli,
                                                  DYNAMIC_ARRAY *ws,
                                                  Log_event *ev)= 0;
  /* wait for slave workers to finish */
  virtual int wait_for_workers_to_finish(Relay_log_info *rli,
                                         Slave_worker *ignore= NULL)=0;

  virtual ~Mts_submode(){}
};

/**
  DB partitioned submode
  For significance of each method check definition of Mts_submode
*/
class Mts_submode_database: public Mts_submode
{
public:
  Mts_submode_database() : Mts_submode(MTS_PARALLEL_TYPE_DB_NAME) {}
  int schedule_next_event(Relay_log_info* rli, Log_event *ev);
  void attach_temp_tables(THD *thd, const Relay_log_info* rli,
                                                      Query_log_event *ev);
  void detach_temp_tables(THD *thd, const Relay_log_info* rli,
                                                      Query_log_event *ev);
  Slave_worker* get_least_occupied_worker(Relay_log_info* rli,
                                          DYNAMIC_ARRAY *ws, Log_event *ev);
  int wait_for_workers_to_finish(Relay_log_info  *rli,
                                 Slave_worker *ignore= NULL);
  ~Mts_submode_database(){}
};

/**
  Parallelization using Master parallelization information
  For significance of each method check definition of Mts_submode
 */
class Mts_submode_logical_clock: public Mts_submode
{
private:
  bool first_event, force_new_group;
  bool is_new_group;
  uint delegated_jobs;

  /* "instant" value of committed transactions low-water-mark */
  longlong last_lwm_timestamp;

  /* GAQ index corresponding to the min commit point */
  ulong last_lwm_index;
  longlong last_committed;
  longlong sequence_number;

public:
  uint jobs_done;
  bool is_error;

  /*
    the logical timestamp of the olderst transaction that is being waited by
    before to resume scheduling.
  */
  longlong min_waited_timestamp;

  /*
    Committed transactions and those that are waiting for their commit parents
    comprise sequences whose items are identified as GAQ index.
    An empty sequence is described by the following magic value which can't
    be in the GAQ legitimate range.
    todo: an alternative could be to pass a magic value to the constructor.
    E.g GAQ.size as a good candidate being outside of the valid range.
    That requires further wl6314 refactoring in activation/deactivation
    of the scheduler.
  */
  static const ulong INDEX_UNDEF= (ulong) -1;

protected:
  std::pair<uint, my_thread_id> get_server_and_thread_id(TABLE* table);
  Slave_worker* get_free_worker(Relay_log_info *rli);

public:
  Mts_submode_logical_clock();
  int schedule_next_event(Relay_log_info* rli, Log_event *ev);
  void attach_temp_tables(THD *thd, const Relay_log_info* rli,
                                                      Query_log_event *ev);
  void detach_temp_tables(THD *thd, const Relay_log_info* rli,
                                                      Query_log_event *ev);
  Slave_worker* get_least_occupied_worker(Relay_log_info* rli,
                                          DYNAMIC_ARRAY *ws, Log_event *ev);

  /* Sets the force new group variable */
  inline void start_new_group()
  {
    force_new_group= true;
    first_event= true;
  }

  int wait_for_workers_to_finish(Relay_log_info  *rli,
                                 Slave_worker *ignore= NULL);

  bool wait_for_last_committed_trx(Relay_log_info* rli,
                                   longlong last_committed_arg,
                                   longlong lwm_estimate_arg);

  /*
    LEQ comparison of two logical timestamps follows regular rules for
    integers. SEQ_UNINIT is regarded as the least value in the clock domain.

    @param a  the lhs logical timestamp value
    @param b  the rhs logical timestamp value

    @return   true  when a "<=" b,
              false otherwise
  */
  static bool clock_leq(longlong a, longlong b)
  {
    if (a == SEQ_UNINIT)
      return true;
    else if (b == SEQ_UNINIT)
      return false;
    else
      return a <= b;
  }

  longlong get_lwm_timestamp(Relay_log_info *rli, bool need_lock);

  longlong estimate_lwm_timestamp()
  {
    return my_atomic_load64_tmp(&last_lwm_timestamp);
  };

  ~Mts_submode_logical_clock() {}
};

#endif /*MTS_SUBMODE_H*/
