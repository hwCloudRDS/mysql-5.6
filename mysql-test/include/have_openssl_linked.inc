--require r/have_openssl_linked.require
disable_query_log;
SHOW STATUS LIKE '%rsa%';
enable_query_log;
