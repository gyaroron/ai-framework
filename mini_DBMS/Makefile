CC = gcc
AR = ar
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Iinclude
LIBRARY = libpager.a
OBJECTS = pager.o buffer_pool.o bplus_tree.o wal.o transaction.o
PAGER_TEST = test_pager
BUFFER_POOL_TEST = test_buffer_pool
BPLUS_TREE_TEST = test_bplus_tree
TRANSACTION_TEST = test_transaction
SERVER = mini_dbms_server
SERVER_OBJECTS = main.o server_bridge.o
# SQL 처리기(Phase 1) + 메모리 B+ tree 인덱스(Phase 2) + thread pool API 서버(Phase 3)
SQL_SOURCES = src/mem_bptree.c src/sql_parser.c src/sql_engine.c src/thread_pool.c
SQL_HEADERS = include/mem_bptree.h include/sql.h include/thread_pool.h
SQL_CLI = mini_dbms_sql
SQL_SERVER = mini_dbms_sql_server
SQL_TEST = test_sql
PTHREAD = -pthread
TEST_FILES = test_db.bin test_buffer_pool.bin test_bplus_tree.bin \
	test_transaction.db test_transaction.wal

.PHONY: all test valgrind clean bench

all: $(LIBRARY) $(SERVER) $(SQL_CLI) $(SQL_SERVER)

$(SQL_CLI): $(SQL_SOURCES) $(SQL_HEADERS) src/sql_cli.c
	$(CC) $(CFLAGS) $(PTHREAD) $(SQL_SOURCES) src/sql_cli.c -o $@

$(SQL_SERVER): $(SQL_SOURCES) $(SQL_HEADERS) src/sql_server.c ../mini_Redis/resp.c ../mini_Redis/resp.h
	$(CC) $(CFLAGS) $(PTHREAD) -I../mini_Redis $(SQL_SOURCES) src/sql_server.c ../mini_Redis/resp.c -o $@

$(SQL_TEST): $(SQL_SOURCES) $(SQL_HEADERS) tests/test_sql.c
	$(CC) $(CFLAGS) $(PTHREAD) $(SQL_SOURCES) tests/test_sql.c -o $@

# 100만 건을 INSERT한 뒤 id 조회(인덱스)와 name 조회(선형 탐색)의 시간을 비교한다.
BENCH_ROWS = 1000000
BENCH_DIR = /tmp/mini_dbms_bench
bench: $(SQL_CLI)
	rm -rf $(BENCH_DIR) && mkdir -p $(BENCH_DIR) && cp data/users.schema $(BENCH_DIR)/
	./$(SQL_CLI) $(BENCH_DIR) --bench users $(BENCH_ROWS)
	rm -rf $(BENCH_DIR)

$(LIBRARY): $(OBJECTS)
	$(AR) rcs $@ $^

pager.o: src/pager.c include/pager.h include/page.h
	$(CC) $(CFLAGS) -c src/pager.c -o $@

buffer_pool.o: src/buffer_pool.c include/buffer_pool.h include/pager.h include/page.h
	$(CC) $(CFLAGS) -c src/buffer_pool.c -o $@

bplus_tree.o: src/bplus_tree.c include/bplus_tree.h include/buffer_pool.h include/pager.h include/page.h
	$(CC) $(CFLAGS) -c src/bplus_tree.c -o $@

wal.o: src/wal.c include/wal.h
	$(CC) $(CFLAGS) -c src/wal.c -o $@

transaction.o: src/transaction.c include/transaction.h include/wal.h \
               include/bplus_tree.h include/buffer_pool.h
	$(CC) $(CFLAGS) -c src/transaction.c -o $@

main.o: src/main.c include/server_bridge.h include/transaction.h
	$(CC) $(CFLAGS) -c src/main.c -o $@

server_bridge.o: src/server_bridge.c include/server_bridge.h ../mini_Redis/resp.h
	$(CC) $(CFLAGS) -I../mini_Redis -c src/server_bridge.c -o $@

$(SERVER): $(SERVER_OBJECTS) $(LIBRARY) ../mini_Redis/resp.c ../mini_Redis/resp.h
	$(CC) $(CFLAGS) -I../mini_Redis $(SERVER_OBJECTS) ../mini_Redis/resp.c $(LIBRARY) -o $@

$(PAGER_TEST): src/pager.c tests/test_pager.c include/pager.h include/page.h
	$(CC) $(CFLAGS) src/pager.c tests/test_pager.c -o $@

$(BUFFER_POOL_TEST): src/pager.c src/buffer_pool.c tests/test_buffer_pool.c \
                     include/buffer_pool.h include/pager.h include/page.h
	$(CC) $(CFLAGS) src/pager.c src/buffer_pool.c tests/test_buffer_pool.c -o $@

$(BPLUS_TREE_TEST): src/pager.c src/buffer_pool.c src/bplus_tree.c \
                    tests/test_bplus_tree.c include/bplus_tree.h \
                    include/buffer_pool.h include/pager.h include/page.h
	$(CC) $(CFLAGS) src/pager.c src/buffer_pool.c src/bplus_tree.c \
	       tests/test_bplus_tree.c -o $@

$(TRANSACTION_TEST): src/pager.c src/buffer_pool.c src/bplus_tree.c \
                     src/wal.c src/transaction.c tests/test_transaction.c \
                     include/transaction.h include/wal.h include/bplus_tree.h \
                     include/buffer_pool.h include/pager.h include/page.h
	$(CC) $(CFLAGS) src/pager.c src/buffer_pool.c src/bplus_tree.c \
	       src/wal.c src/transaction.c tests/test_transaction.c -o $@

test: $(PAGER_TEST) $(BUFFER_POOL_TEST) $(BPLUS_TREE_TEST) $(TRANSACTION_TEST) $(SQL_TEST)
	./$(PAGER_TEST)
	./$(BUFFER_POOL_TEST)
	./$(BPLUS_TREE_TEST)
	./$(TRANSACTION_TEST)
	./$(SQL_TEST)

valgrind: $(PAGER_TEST) $(BUFFER_POOL_TEST) $(BPLUS_TREE_TEST) $(TRANSACTION_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(PAGER_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(BUFFER_POOL_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(BPLUS_TREE_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TRANSACTION_TEST)

clean:
	rm -f $(OBJECTS) $(SERVER_OBJECTS) $(SERVER) $(LIBRARY) $(PAGER_TEST) $(BUFFER_POOL_TEST) \
	      $(BPLUS_TREE_TEST) $(TRANSACTION_TEST) $(TEST_FILES) \n	      $(SQL_CLI) $(SQL_SERVER) $(SQL_TEST)
