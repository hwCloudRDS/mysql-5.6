--require r/have_pool_of_threads.require
disable_query_log;
select count(*) = 1 from information_schema.global_variables where variable_name = 'threadpool_size';
enable_query_log;
