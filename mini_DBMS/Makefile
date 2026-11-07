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
TEST_FILES = test_db.bin test_buffer_pool.bin test_bplus_tree.bin \
	test_transaction.db test_transaction.wal

.PHONY: all test valgrind clean

all: $(LIBRARY) $(SERVER)

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

server_bridge.o: src/server_bridge.c include/server_bridge.h ../resp.h
	$(CC) $(CFLAGS) -I.. -c src/server_bridge.c -o $@

$(SERVER): $(SERVER_OBJECTS) $(LIBRARY) ../resp.c ../resp.h
	$(CC) $(CFLAGS) -I.. $(SERVER_OBJECTS) ../resp.c $(LIBRARY) -o $@

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

test: $(PAGER_TEST) $(BUFFER_POOL_TEST) $(BPLUS_TREE_TEST) $(TRANSACTION_TEST)
	./$(PAGER_TEST)
	./$(BUFFER_POOL_TEST)
	./$(BPLUS_TREE_TEST)
	./$(TRANSACTION_TEST)

valgrind: $(PAGER_TEST) $(BUFFER_POOL_TEST) $(BPLUS_TREE_TEST) $(TRANSACTION_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(PAGER_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(BUFFER_POOL_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(BPLUS_TREE_TEST)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TRANSACTION_TEST)

clean:
	rm -f $(OBJECTS) $(SERVER_OBJECTS) $(SERVER) $(LIBRARY) $(PAGER_TEST) $(BUFFER_POOL_TEST) \
	      $(BPLUS_TREE_TEST) $(TRANSACTION_TEST) $(TEST_FILES)
