CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic
TARGET = mini_redis
SOURCES = hashtable.c resp.c server.c main.c
OBJECTS = $(SOURCES:.c=.o)
TEST_TARGET = test_resp
HASHTABLE_TEST_TARGET = test_hashtable

.PHONY: all test run valgrind clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

hashtable.o: hashtable.c hashtable.h
	$(CC) $(CFLAGS) -c hashtable.c -o $@

resp.o: resp.c resp.h
	$(CC) $(CFLAGS) -c resp.c -o $@

server.o: server.c server.h resp.h hashtable.h
	$(CC) $(CFLAGS) -c server.c -o $@

main.o: main.c server.h hashtable.h
	$(CC) $(CFLAGS) -c main.c -o $@

$(TEST_TARGET): resp.c resp.h test_resp.c
	$(CC) $(CFLAGS) resp.c test_resp.c -o $@

$(HASHTABLE_TEST_TARGET): hashtable.c hashtable.h test_hashtable.c
	$(CC) $(CFLAGS) hashtable.c test_hashtable.c -o $@

test: $(TEST_TARGET) $(HASHTABLE_TEST_TARGET)
	./$(TEST_TARGET)
	./$(HASHTABLE_TEST_TARGET)

run: $(TARGET)
	./$(TARGET)

valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

clean:
	rm -f $(OBJECTS) $(TARGET) $(TEST_TARGET) $(HASHTABLE_TEST_TARGET) test_hashtable.snapshot test_hashtable.snapshot.tmp
