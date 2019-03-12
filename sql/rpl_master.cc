/* Copyright (c) 2010, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */


#include "sql_priv.h"
#include "unireg.h"
#include "sql_parse.h"                          // check_access
#include "global_threads.h"
#ifdef HAVE_REPLICATION

#include "sql_acl.h"                            // SUPER_ACL
#include "log_event.h"
#include "rpl_filter.h"
#include <my_dir.h>
#include "rpl_handler.h"
#include "rpl_master.h"
#include "debug_sync.h"
#include "rpl_binlog_sender.h"

int max_binlog_dump_events = 0; // unlimited
my_bool opt_sporadic_binlog_dump_fail = 0;

#define SLAVE_LIST_CHUNK 128
#define SLAVE_ERRMSG_SIZE (FN_REFLEN+64)
HASH slave_list;
extern TYPELIB binlog_checksum_typelib;


#define get_object(p, obj, msg) \
{\
  uint len; \
  if (p >= p_end) \
  { \
    my_error(ER_MALFORMED_PACKET, MYF(0)); \
    my_free(si); \
    return 1; \
  } \
  len= (uint)*p++;  \
  if (p + len > p_end || len >= sizeof(obj)) \
  {\
    errmsg= msg;\
    goto err; \
  }\
  strmake(obj,(char*) p,len); \
  p+= len; \
}\

extern "C" uint32
*slave_list_key(SLAVE_INFO* si, size_t *len,
		my_bool not_used MY_ATTRIBUTE((unused)))
{
  *len = 4;
  return &si->server_id;
}

extern "C" void slave_info_free(void *s)
{
  my_free(s);
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_slave_list;

static PSI_mutex_info all_slave_list_mutexes[]=
{
  { &key_LOCK_slave_list, "LOCK_slave_list", PSI_FLAG_GLOBAL}
};

static void init_all_slave_list_mutexes(void)
{
  int count;

  count= array_elements(all_slave_list_mutexes);
  mysql_mutex_register("sql", all_slave_list_mutexes, count);
}
#endif /* HAVE_PSI_INTERFACE */

void init_slave_list()
{
#ifdef HAVE_PSI_INTERFACE
  init_all_slave_list_mutexes();
#endif

  my_hash_init(&slave_list, system_charset_info, SLAVE_LIST_CHUNK, 0, 0,
               (my_hash_get_key) slave_list_key,
               (my_hash_free_key) slave_info_free, 0);
  mysql_mutex_init(key_LOCK_slave_list, &LOCK_slave_list, MY_MUTEX_INIT_FAST);
}

void end_slave_list()
{
  /* No protection by a mutex needed as we are only called at shutdown */
  if (my_hash_inited(&slave_list))
  {
    my_hash_free(&slave_list);
    mysql_mutex_destroy(&LOCK_slave_list);
  }
}

/**
  Register slave in 'slave_list' hash table.

  @return
    0	ok
  @return
    1	Error.   Error message sent to client
*/

int register_slave(THD* thd, uchar* packet, uint packet_length)
{
  int res;
  SLAVE_INFO *si;
  uchar *p= packet, *p_end= packet + packet_length;
  const char *errmsg= "Wrong parameters to function register_slave";

  if (check_access(thd, REPL_SLAVE_ACL, any_db, NULL, NULL, 0, 0))
    return 1;
  if (!(si = (SLAVE_INFO*)my_malloc(sizeof(SLAVE_INFO), MYF(MY_WME))))
    goto err2;

  /* 4 bytes for the server id */
  if (p + 4 > p_end)
  {
    my_error(ER_MALFORMED_PACKET, MYF(0));
    my_free(si);
    return 1;
  }

  thd->server_id= si->server_id= uint4korr(p);
  p+= 4;
  get_object(p,si->host, "Failed to register slave: too long 'report-host'");
  get_object(p,si->user, "Failed to register slave: too long 'report-user'");
  get_object(p,si->password, "Failed to register slave; too long 'report-password'");
  if (p+10 > p_end)
    goto err;
  si->port= uint2korr(p);
  p += 2;
  /* 
     We need to by pass the bytes used in the fake rpl_recovery_rank
     variable. It was removed in patch for BUG#13963. But this would 
     make a server with that patch unable to connect to an old master.
     See: BUG#49259
  */
  p += 4;
  if (!(si->master_id= uint4korr(p)))
    si->master_id= server_id;
  si->thd= thd;

  mysql_mutex_lock(&LOCK_slave_list);
  unregister_slave(thd, false, false/*need_lock_slave_list=false*/);
  res= my_hash_insert(&slave_list, (uchar*) si);
  mysql_mutex_unlock(&LOCK_slave_list);
  return res;

err:
  my_free(si);
  my_message(ER_UNKNOWN_ERROR, errmsg, MYF(0)); /* purecov: inspected */
err2:
  return 1;
}

void unregister_slave(THD* thd, bool only_mine, bool need_lock_slave_list)
{
  if (thd->server_id)
  {
    if (need_lock_slave_list)
      mysql_mutex_lock(&LOCK_slave_list);
    else
      mysql_mutex_assert_owner(&LOCK_slave_list);

    SLAVE_INFO* old_si;
    if ((old_si = (SLAVE_INFO*)my_hash_search(&slave_list,
                                              (uchar*)&thd->server_id, 4)) &&
	(!only_mine || old_si->thd == thd))
    my_hash_delete(&slave_list, (uchar*)old_si);

    if (need_lock_slave_list)
      mysql_mutex_unlock(&LOCK_slave_list);
  }
}


/**
  Execute a SHOW SLAVE HOSTS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool show_slave_hosts(THD* thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("show_slave_hosts");

  field_list.push_back(new Item_return_int("Server_id", 10,
					   MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Host", 20));
  if (opt_show_slave_auth_info)
  {
    field_list.push_back(new Item_empty_string("User",20));
    field_list.push_back(new Item_empty_string("Password",20));
  }
  field_list.push_back(new Item_return_int("Port", 7, MYSQL_TYPE_LONG));
  field_list.push_back(new Item_return_int("Master_id", 10,
					   MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Slave_UUID", UUID_LENGTH));

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  mysql_mutex_lock(&LOCK_slave_list);

  for (uint i = 0; i < slave_list.records; ++i)
  {
    SLAVE_INFO* si = (SLAVE_INFO*) my_hash_element(&slave_list, i);
    protocol->prepare_for_resend();
    protocol->store((uint32) si->server_id);
    protocol->store(si->host, &my_charset_bin);
    if (opt_show_slave_auth_info)
    {
      protocol->store(si->user, &my_charset_bin);
      protocol->store(si->password, &my_charset_bin);
    }
    protocol->store((uint32) si->port);
    protocol->store((uint32) si->master_id);

    /* get slave's UUID */
    String slave_uuid;
    if (get_slave_uuid(si->thd, &slave_uuid))
      protocol->store(slave_uuid.c_ptr_safe(), &my_charset_bin);
    if (protocol->write())
    {
      mysql_mutex_unlock(&LOCK_slave_list);
      DBUG_RETURN(TRUE);
    }
  }
  mysql_mutex_unlock(&LOCK_slave_list);
  my_eof(thd);
  DBUG_RETURN(FALSE);
}

/**
  If there are less than BYTES bytes left to read in the packet,
  report error.
*/
#define CHECK_PACKET_SIZE(BYTES)                                        \
  do {                                                                  \
    if (packet_bytes_todo < BYTES)                                      \
      goto error_malformed_packet;                                      \
  } while (0)

/**
  Auxiliary macro used to define READ_INT and READ_STRING.

  Check that there are at least BYTES more bytes to read, then read
  the bytes using the given DECODER, then advance the reading
  position.
*/
#define READ(DECODE, BYTES)                                             \
  do {                                                                  \
    CHECK_PACKET_SIZE(BYTES);                                           \
    DECODE;                                                             \
    packet_position+= BYTES;                                            \
    packet_bytes_todo-= BYTES;                                          \
  } while (0)

#define SKIP(BYTES) READ((void)(0), BYTES)

/**
  Check that there are at least BYTES more bytes to read, then read
  the bytes and decode them into the given integer VAR, then advance
  the reading position.
*/
#define READ_INT(VAR, BYTES)                                            \
  READ(VAR= uint ## BYTES ## korr(packet_position), BYTES)

/**
  Check that there are at least BYTES more bytes to read and that
  BYTES+1 is not greater than BUFFER_SIZE, then read the bytes into
  the given variable VAR, then advance the reading position.
*/
#define READ_STRING(VAR, BYTES, BUFFER_SIZE)                            \
  do {                                                                  \
    if (BUFFER_SIZE <= BYTES)                                           \
      goto error_malformed_packet;                                      \
    READ(memcpy(VAR, packet_position, BYTES), BYTES);                   \
    VAR[BYTES]= '\0';                                                   \
  } while (0)


bool com_binlog_dump(THD *thd, char *packet, uint packet_length)
{
  DBUG_ENTER("com_binlog_dump");
  ulong pos;
  ushort flags= 0;
  const uchar* packet_position= (uchar *) packet;
  uint packet_bytes_todo= packet_length;

  status_var_increment(thd->status_var.com_other);
  thd->enable_slow_log= opt_log_slow_admin_statements;
  if (check_global_access(thd, REPL_SLAVE_ACL))
    DBUG_RETURN(false);

  /*
    4 bytes is too little, but changing the protocol would break
    compatibility.  This has been fixed in the new protocol. @see
    com_binlog_dump_gtid().
  */
  READ_INT(pos, 4);
  READ_INT(flags, 2);
  READ_INT(thd->server_id, 4);

  DBUG_PRINT("info", ("pos=%lu flags=%d server_id=%d", pos, flags, thd->server_id));

  kill_zombie_dump_threads(thd);

  general_log_print(thd, thd->get_command(), "Log: '%s'  Pos: %ld",
                    packet + 10, (long) pos);
  mysql_binlog_send(thd, thd->strdup(packet + 10), (my_off_t) pos, NULL, flags);

  unregister_slave(thd, true, true/*need_lock_slave_list=true*/);
  /*  fake COM_QUIT -- if we get here, the thread needs to terminate */
  DBUG_RETURN(true);

error_malformed_packet:
  my_error(ER_MALFORMED_PACKET, MYF(0));
  DBUG_RETURN(true);
}


bool com_binlog_dump_gtid(THD *thd, char *packet, uint packet_length)
{
  DBUG_ENTER("com_binlog_dump_gtid");
  /*
    Before going GA, we need to make this protocol extensible without
    breaking compatitibilty. /Alfranio.
  */
  ushort flags= 0;
  uint32 data_size= 0;
  uint64 pos= 0;
  char name[FN_REFLEN + 1];
  uint32 name_size= 0;
  char* gtid_string= NULL;
  const uchar* packet_position= (uchar *) packet;
  uint packet_bytes_todo= packet_length;
  Sid_map sid_map(NULL/*no sid_lock because this is a completely local object*/);
  Gtid_set slave_gtid_executed(&sid_map);

  status_var_increment(thd->status_var.com_other);
  thd->enable_slow_log= opt_log_slow_admin_statements;
  if (check_global_access(thd, REPL_SLAVE_ACL))
    DBUG_RETURN(false);

  READ_INT(flags, 2);
  READ_INT(thd->server_id, 4);
  READ_INT(name_size, 4);
  READ_STRING(name, name_size, sizeof(name));
  READ_INT(pos, 8);
  DBUG_PRINT("info", ("pos=%llu flags=%d server_id=%d", pos, flags, thd->server_id));
  READ_INT(data_size, 4);
  CHECK_PACKET_SIZE(data_size);
  if (slave_gtid_executed.add_gtid_encoding(packet_position, data_size) !=
      RETURN_STATUS_OK)
    DBUG_RETURN(true);
  gtid_string= slave_gtid_executed.to_string();
  DBUG_PRINT("info", ("Slave %d requested to read %s at position %llu gtid set "
                      "'%s'.", thd->server_id, name, pos, gtid_string));

  kill_zombie_dump_threads(thd);
  general_log_print(thd, thd->get_command(), "Log: '%s' Pos: %llu GTIDs: '%s'",
                    name, pos, gtid_string);
  my_free(gtid_string);
  mysql_binlog_send(thd, name, (my_off_t) pos, &slave_gtid_executed, flags);

  unregister_slave(thd, true, true/*need_lock_slave_list=true*/);
  /*  fake COM_QUIT -- if we get here, the thread needs to terminate */
  DBUG_RETURN(true);

error_malformed_packet:
  my_error(ER_MALFORMED_PACKET, MYF(0));
  DBUG_RETURN(true);
}


void mysql_binlog_send(THD* thd, char* log_ident, my_off_t pos,
		               const Gtid_set* slave_gtid_executed, int flags)
{
  // Notify threadpool that it has a binlog dumper thread. Binlog
  // dumper is special because it never returns to the threadpool
  // until slave stops. Threapool should not consider binlog dumper 
  // thread as a "busy" thread, i.e. binlog dumper is neither an active 
  // thread, nor a waiting thread.
  thd_wait_begin(thd, THD_WAIT_BL_DUMPER);
  Binlog_sender sender(thd, log_ident, pos, slave_gtid_executed, flags);

  sender.run();
  thd_wait_end(thd);
}


/**
  An auxiliary function extracts slave UUID.

  @param[in]    thd  THD to access a user variable
  @param[out]   value String to return UUID value.

  @return       if success value is returned else NULL is returned.
*/
String *get_slave_uuid(THD *thd, String *value)
{
  uchar name[]= "slave_uuid";

  if (value == NULL)
    return NULL;

  /* Protects thd->user_vars. */
  mysql_mutex_lock(&thd->LOCK_thd_data);

  user_var_entry *entry=
    (user_var_entry*) my_hash_search(&thd->user_vars, name, sizeof(name)-1);
  if (entry && entry->length() > 0)
  {
    value->copy(entry->ptr(), entry->length(), NULL);
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    return value;
  }

  mysql_mutex_unlock(&thd->LOCK_thd_data);
  return NULL;
}

/*

  Kill all Binlog_dump threads which previously talked to the same slave
  ("same" means with the same UUID(for slave versions >= 5.6) or same server id
  (for slave versions < 5.6). Indeed, if the slave stops, if the
  Binlog_dump thread is waiting (mysql_cond_wait) for binlog update, then it
  will keep existing until a query is written to the binlog. If the master is
  idle, then this could last long, and if the slave reconnects, we could have 2
  Binlog_dump threads in SHOW PROCESSLIST, until a query is written to the
  binlog. To avoid this, when the slave reconnects and sends COM_BINLOG_DUMP,
  the master kills any existing thread with the slave's UUID/server id (if this id is
  not zero; it will be true for real slaves, but false for mysqlbinlog when it
  sends COM_BINLOG_DUMP to get a remote binlog dump).

  SYNOPSIS
    kill_zombie_dump_threads()
    @param thd newly connected dump thread object

*/

void kill_zombie_dump_threads(THD *thd)
{
  String slave_uuid;
  get_slave_uuid(thd, &slave_uuid);
  if (slave_uuid.length() == 0 && thd->server_id == 0)
    return;

  mysql_mutex_lock(&LOCK_thread_count);
  THD *tmp= NULL;
  Thread_iterator it= global_thread_list_begin();
  Thread_iterator end= global_thread_list_end();
  bool is_zombie_thread= false;
  for (; it != end; ++it)
  {
    if ((*it) != thd && ((*it)->get_command() == COM_BINLOG_DUMP ||
                         (*it)->get_command() == COM_BINLOG_DUMP_GTID))
    {
      String tmp_uuid;
      get_slave_uuid((*it), &tmp_uuid);
      if (slave_uuid.length())
      {
        is_zombie_thread= (tmp_uuid.length() &&
                           !strncmp(slave_uuid.c_ptr(),
                                    tmp_uuid.c_ptr(), UUID_LENGTH));
      }
      else
      {
        /*
          Check if it is a 5.5 slave's dump thread i.e., server_id should be
          same && dump thread should not contain 'UUID'.
        */
        is_zombie_thread= (((*it)->server_id == thd->server_id) &&
                           !tmp_uuid.length());
      }
      if (is_zombie_thread)
      {
        tmp= *it;
        mysql_mutex_lock(&tmp->LOCK_thd_data);	// Lock from delete
        break;
      }
    }
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  if (tmp)
  {
    /*
      Here we do not call kill_one_thread() as
      it will be slow because it will iterate through the list
      again. We just to do kill the thread ourselves.
    */
    if (log_warnings > 1)
    {
      if (slave_uuid.length())
      {
        sql_print_information("While initializing dump thread for slave with "
                              "UUID <%s>, found a zombie dump thread with the "
                              "same UUID. Master is killing the zombie dump "
                              "thread(%lu).", slave_uuid.c_ptr(),
                              tmp->thread_id);
      }
      else
      {
        sql_print_information("While initializing dump thread for slave with "
                              "server_id <%u>, found a zombie dump thread with the "
                              "same server_id. Master is killing the zombie dump "
                              "thread(%lu).", thd->server_id,
                              tmp->thread_id);
      }
    }
    tmp->duplicate_slave_id= true;
    tmp->awake(THD::KILL_QUERY);
    mysql_mutex_unlock(&tmp->LOCK_thd_data);
  }
}

/**
  Execute a RESET MASTER statement.

  @param thd Pointer to THD object of the client thread executing the
  statement.

  @retval 0 success
  @retval 1 error
*/
int reset_master(THD* thd)
{
  if (!mysql_bin_log.is_open())
  {
    my_message(ER_FLUSH_MASTER_BINLOG_CLOSED,
               ER(ER_FLUSH_MASTER_BINLOG_CLOSED), MYF(ME_BELL+ME_WAITTANG));
    return 1;
  }

  if (mysql_bin_log.reset_logs(thd))
    return 1;
  (void) RUN_HOOK(binlog_transmit, after_reset_master, (thd, 0 /* flags */));
  return 0;
}


/**
  Execute a SHOW MASTER STATUS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool show_master_status(THD* thd)
{
  Protocol *protocol= thd->protocol;
  char* gtid_set_buffer= NULL;
  int gtid_set_size= 0;
  List<Item> field_list;

  DBUG_ENTER("show_binlog_info");

  global_sid_lock->wrlock();
  const Gtid_set* gtid_set= gtid_state->get_logged_gtids();
  if ((gtid_set_size= gtid_set->to_string(&gtid_set_buffer)) < 0)
  {
    global_sid_lock->unlock();
    my_eof(thd);
    my_free(gtid_set_buffer);
    DBUG_RETURN(true);
  }
  global_sid_lock->unlock();

  field_list.push_back(new Item_empty_string("File", FN_REFLEN));
  field_list.push_back(new Item_return_int("Position",20,
					   MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Binlog_Do_DB",255));
  field_list.push_back(new Item_empty_string("Binlog_Ignore_DB",255));
  field_list.push_back(new Item_empty_string("Executed_Gtid_Set",
                                             gtid_set_size));

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
  {
    my_free(gtid_set_buffer);
    DBUG_RETURN(true);
  }
  protocol->prepare_for_resend();

  if (mysql_bin_log.is_open())
  {
    LOG_INFO li;
    mysql_bin_log.get_current_log(&li);
    int dir_len = dirname_length(li.log_file_name);
    protocol->store(li.log_file_name + dir_len, &my_charset_bin);
    protocol->store((ulonglong) li.pos);
    protocol->store(binlog_filter->get_do_db());
    protocol->store(binlog_filter->get_ignore_db());
    protocol->store(gtid_set_buffer, &my_charset_bin);
    if (protocol->write())
    {
      my_free(gtid_set_buffer);
      DBUG_RETURN(true);
    }
  }
  my_eof(thd);
  my_free(gtid_set_buffer);
  DBUG_RETURN(false);
}


/**
  Execute a SHOW BINARY LOGS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool show_binlogs(THD* thd)
{
  IO_CACHE *index_file;
  LOG_INFO cur;
  File file;
  char fname[FN_REFLEN];
  List<Item> field_list;
  uint length;
  int cur_dir_len;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("show_binlogs");

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    DBUG_RETURN(TRUE);
  }

  field_list.push_back(new Item_empty_string("Log_name", 255));
  field_list.push_back(new Item_return_int("File_size", 20,
                                           MYSQL_TYPE_LONGLONG));
  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);
  
  mysql_mutex_lock(mysql_bin_log.get_log_lock());
  DEBUG_SYNC(thd, "show_binlogs_after_lock_log_before_lock_index");
  mysql_bin_log.lock_index();
  index_file=mysql_bin_log.get_index_file();
  
  mysql_bin_log.raw_get_current_log(&cur); // dont take mutex
  mysql_mutex_unlock(mysql_bin_log.get_log_lock()); // lockdep, OK
  
  cur_dir_len= dirname_length(cur.log_file_name);

  reinit_io_cache(index_file, READ_CACHE, (my_off_t) 0, 0, 0);

  /* The file ends with EOF or empty line */
  while ((length=my_b_gets(index_file, fname, sizeof(fname))) > 1)
  {
    int dir_len;
    ulonglong file_length= 0;                   // Length if open fails
    fname[--length] = '\0';                     // remove the newline

    protocol->prepare_for_resend();
    dir_len= dirname_length(fname);
    length-= dir_len;
    protocol->store(fname + dir_len, length, &my_charset_bin);

    if (!(strncmp(fname+dir_len, cur.log_file_name+cur_dir_len, length)))
      file_length= cur.pos;  /* The active log, use the active position */
    else
    {
      /* this is an old log, open it and find the size */
      if ((file= mysql_file_open(key_file_binlog,
                                 fname, O_RDONLY | O_SHARE | O_BINARY,
                                 MYF(0))) >= 0)
      {
        file_length= (ulonglong) mysql_file_seek(file, 0L, MY_SEEK_END, MYF(0));
        mysql_file_close(file, MYF(0));
      }
    }
    protocol->store(file_length);
    if (protocol->write())
    {
      DBUG_PRINT("info", ("stopping dump thread because protocol->write failed at line %d", __LINE__));
      goto err;
    }
  }
  if(index_file->error == -1)
    goto err;
  mysql_bin_log.unlock_index();
  my_eof(thd);
  DBUG_RETURN(FALSE);

err:
  mysql_bin_log.unlock_index();
  DBUG_RETURN(TRUE);
}

#endif /* HAVE_REPLICATION */
