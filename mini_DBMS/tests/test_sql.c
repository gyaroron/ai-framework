#define _POSIX_C_SOURCE 200809L

/* SQL 처리기 · 메모리 B+ tree · thread pool 테스트. */

#include "mem_bptree.h"
#include "sql.h"
#include "thread_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TREE_KEYS 200000u
#define WRITER_THREADS 4
#define ROWS_PER_WRITER 500
#define READER_THREADS 4
#define POOL_TASKS 1000

static char data_dir[64];

/* 테스트용 폴더를 만들고 users 테이블의 schema 파일을 쓴다. */
static void make_data_dir(void)
{
    char path[128];
    FILE *file;

    strcpy(data_dir, "/tmp/mini_dbms_sql_XXXXXX");
    assert(mkdtemp(data_dir) != NULL);
    snprintf(path, sizeof(path), "%s/users.schema", data_dir);
    file = fopen(path, "w");
    assert(file != NULL);
    fputs("id,name,email,age\n", file);
    fclose(file);
}

static void remove_data_dir(void)
{
    char path[128];

    snprintf(path, sizeof(path), "%s/users.schema", data_dir);
    (void)remove(path);
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    (void)remove(path);
    (void)rmdir(data_dir);
}

/* 문장을 실행하고 결과 글자를 돌려준다(다음 호출 전까지만 유효). */
static const char *run(SqlEngine *engine, const char *sql, int expected_result)
{
    static SqlBuffer out;

    sql_buffer_clear(&out);
    if (sql_engine_execute(engine, sql, &out) != expected_result) {
        fprintf(stderr, "unexpected result for: %s\n%s", sql, out.data ? out.data : "");
        abort();
    }
    return out.data;
}

static void test_mem_bptree(void)
{
    MemBPTree *tree = mem_bptree_create();
    uint64_t value = 0;
    uint64_t i;

    assert(tree != NULL);
    assert(mem_bptree_height(tree) == 1 && mem_bptree_count(tree) == 0);
    assert(mem_bptree_find(tree, 1, &value) == 0);

    /* 오름차순이 아닌 순서로 넣어 leaf 가운데 삽입과 split을 모두 거치게 한다. */
    for (i = 0; i < TREE_KEYS; ++i) {
        uint64_t key = (i * 7919u) % TREE_KEYS + 1;
        assert(mem_bptree_insert(tree, key, key * 10) == 0);
    }
    assert(mem_bptree_count(tree) == TREE_KEYS);
    assert(mem_bptree_height(tree) >= 3);
    assert(mem_bptree_check(tree) == 1);
    for (i = 1; i <= TREE_KEYS; ++i) {
        assert(mem_bptree_find(tree, i, &value) == 1 && value == i * 10);
    }
    assert(mem_bptree_find(tree, 0, &value) == 0);
    assert(mem_bptree_find(tree, TREE_KEYS + 1, &value) == 0);

    /* 같은 key를 다시 넣으면 개수는 그대로이고 값만 바뀐다. */
    assert(mem_bptree_insert(tree, 42, 4242) == 0);
    assert(mem_bptree_count(tree) == TREE_KEYS);
    assert(mem_bptree_find(tree, 42, &value) == 1 && value == 4242);
    mem_bptree_destroy(tree);
    mem_bptree_destroy(NULL);
    puts("PASS 1: memory B+ tree keeps 200000 keys sorted and findable");
}

static void test_parser(void)
{
    SqlStatement statement;
    char error[160];
    char *piece = NULL;
    const char *cursor;

    assert(sql_parse("INSERT INTO users VALUES ('Kim', 'kim@example.com', 30);",
                     &statement, error, sizeof(error)) == 0);
    assert(statement.type == SQL_STATEMENT_INSERT);
    assert(strcmp(statement.table, "users") == 0 && statement.value_count == 3);
    assert(strcmp(statement.values[0], "Kim") == 0 && strcmp(statement.values[2], "30") == 0);
    sql_statement_free(&statement);

    assert(sql_parse("insert into users (name, age) values ('O''Neil; x', 41)",
                     &statement, error, sizeof(error)) == 0);
    assert(statement.column_count == 2 && strcmp(statement.columns[1], "age") == 0);
    assert(strcmp(statement.values[0], "O'Neil; x") == 0);
    sql_statement_free(&statement);

    assert(sql_parse("SELECT name, email FROM users WHERE id = 7;",
                     &statement, error, sizeof(error)) == 0);
    assert(statement.type == SQL_STATEMENT_SELECT && statement.column_count == 2);
    assert(statement.has_where && strcmp(statement.where_column, "id") == 0);
    assert(strcmp(statement.where_value, "7") == 0);
    sql_statement_free(&statement);

    assert(sql_parse("SELECT COUNT(*) FROM users", &statement, error, sizeof(error)) == 0);
    assert(statement.is_count && !statement.has_where);
    sql_statement_free(&statement);

    /* 잘못된 문장은 거절하고 이유를 남긴다. */
    assert(sql_parse("", &statement, error, sizeof(error)) == -1);
    assert(sql_parse("DELETE FROM users", &statement, error, sizeof(error)) == -1);
    assert(strstr(error, "unsupported") != NULL);
    assert(sql_parse("SELECT * users", &statement, error, sizeof(error)) == -1);
    assert(sql_parse("SELECT * FROM users WHERE", &statement, error, sizeof(error)) == -1);
    assert(sql_parse("INSERT INTO users VALUES ('open", &statement, error, sizeof(error)) == -1);
    assert(strstr(error, "unterminated") != NULL);
    assert(sql_parse("INSERT INTO users VALUES (1, 2", &statement, error, sizeof(error)) == -1);
    assert(sql_parse("SELECT * FROM users; extra", &statement, error, sizeof(error)) == -1);
    assert(sql_parse("SELECT * FROM us$ers", &statement, error, sizeof(error)) == -1);

    /* script 나누기: 주석과 따옴표 안의 ';'을 구분한다. */
    cursor = "-- comment\n SELECT * FROM a ;\nINSERT INTO b VALUES ('x;y');  ";
    cursor = sql_next_statement(cursor, &piece);
    assert(cursor != NULL && strcmp(piece, "SELECT * FROM a ;") == 0);
    free(piece);
    cursor = sql_next_statement(cursor, &piece);
    assert(cursor != NULL && strcmp(piece, "INSERT INTO b VALUES ('x;y');") == 0);
    free(piece);
    assert(sql_next_statement(cursor, &piece) == NULL && piece == NULL);
    puts("PASS 2: parser accepts INSERT and SELECT and rejects malformed statements");
}

static void test_engine(void)
{
    SqlEngine *engine = sql_engine_open(data_dir);
    const char *out;

    assert(engine != NULL);
    assert(sql_engine_open("/no/such/dir") == NULL);

    out = run(engine, "SELECT * FROM users;", 0);
    assert(strcmp(out, "id\tname\temail\tage\n(0 rows, full scan of 0 rows)\n") == 0);

    /* id는 1부터 자동으로 붙는다. */
    assert(strcmp(run(engine, "INSERT INTO users VALUES ('Kim', 'kim@example.com', 30);", 0),
                  "INSERT 1 (id=1)\n") == 0);
    assert(strcmp(run(engine, "INSERT INTO users VALUES ('Lee', 'lee@example.com', 25);", 0),
                  "INSERT 1 (id=2)\n") == 0);
    assert(strcmp(run(engine, "INSERT INTO users (age, name) VALUES (30, 'Tab\tand\nline');", 0),
                  "INSERT 1 (id=3)\n") == 0);

    /* WHERE id = ? 는 인덱스를 쓰고, 다른 컬럼은 선형 탐색을 한다. */
    out = run(engine, "SELECT * FROM users WHERE id = 2;", 0);
    assert(strcmp(out, "id\tname\temail\tage\n2\tLee\tlee@example.com\t25\n"
                       "(1 row, index lookup)\n") == 0);
    out = run(engine, "SELECT name FROM users WHERE age = 30;", 0);
    assert(strcmp(out, "name\nKim\nTab\\tand\\nline\n(2 rows, full scan of 3 rows)\n") == 0);
    out = run(engine, "SELECT id, email FROM users WHERE name = 'Tab\tand\nline';", 0);
    assert(strcmp(out, "id\temail\n3\t\n(1 row, full scan of 3 rows)\n") == 0);
    out = run(engine, "SELECT * FROM users WHERE id = 99;", 0);
    assert(strstr(out, "(0 rows, index lookup)") != NULL);
    out = run(engine, "SELECT COUNT(*) FROM users;", 0);
    assert(strcmp(out, "count\n3\n(1 row, table metadata)\n") == 0);
    out = run(engine, "SELECT COUNT(*) FROM users WHERE age = 30;", 0);
    assert(strcmp(out, "count\n2\n(1 row, full scan of 3 rows)\n") == 0);

    /* 잘못된 요청은 파일을 바꾸지 않고 오류로 답한다. */
    assert(strstr(run(engine, "INSERT INTO users VALUES ('only one');", -1), "needs 3 values"));
    assert(strstr(run(engine, "INSERT INTO users (id, name) VALUES (9, 'x');", -1),
                  "assigned automatically"));
    assert(strstr(run(engine, "INSERT INTO users (nope) VALUES (1);", -1), "unknown column"));
    assert(strstr(run(engine, "SELECT nope FROM users;", -1), "unknown column"));
    assert(strstr(run(engine, "SELECT * FROM users WHERE nope = 1;", -1), "unknown column"));
    assert(strstr(run(engine, "SELECT * FROM missing;", -1), "unknown table"));
    assert(strstr(run(engine, "SELECT * FROM ../users;", -1), "ERROR"));
    assert(strstr(run(engine, "DROP TABLE users;", -1), "unsupported"));
    assert(strstr(run(engine, "SELECT COUNT(*) FROM users;", 0), "\n3\n"));
    sql_engine_close(engine);

    /* 다시 열면 파일을 읽어 인덱스와 다음 id를 되살린다. */
    engine = sql_engine_open(data_dir);
    assert(engine != NULL);
    out = run(engine, "SELECT name FROM users WHERE id = 1;", 0);
    assert(strcmp(out, "name\nKim\n(1 row, index lookup)\n") == 0);
    assert(strcmp(run(engine, "INSERT INTO users VALUES ('Park', 'p@example.com', 50);", 0),
                  "INSERT 1 (id=4)\n") == 0);
    sql_engine_close(engine);

    /* 쓰다가 중단된 마지막 줄(줄바꿈 없음)은 다시 열 때 버린다. */
    {
        char path[128];
        FILE *file;

        snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
        file = fopen(path, "ab");
        assert(file != NULL);
        fputs("5\thalf written", file);
        fclose(file);
    }
    engine = sql_engine_open(data_dir);
    assert(engine != NULL);
    assert(strstr(run(engine, "SELECT COUNT(*) FROM users;", 0), "\n4\n"));
    assert(strcmp(run(engine, "INSERT INTO users VALUES ('Choi', 'c@example.com', 33);", 0),
                  "INSERT 1 (id=5)\n") == 0);
    assert(strstr(run(engine, "SELECT name FROM users WHERE id = 5;", 0), "\nChoi\n"));
    sql_engine_close(engine);
    puts("PASS 3: engine stores rows in a file, assigns ids, and uses the index for id");
}

/* 동시성 테스트에서 thread들이 함께 쓰는 정보. */
typedef struct SharedWork {
    SqlEngine *engine;
    int writer_number;
    int failures;
} SharedWork;

static void *writer_main(void *argument)
{
    SharedWork *work = (SharedWork *)argument;
    SqlBuffer out;
    char sql[160];
    int i;

    sql_buffer_init(&out);
    for (i = 0; i < ROWS_PER_WRITER; ++i) {
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('w%d_%d', 'w@example.com', %d);",
                 work->writer_number, i, work->writer_number);
        sql_buffer_clear(&out);
        if (sql_engine_execute(work->engine, sql, &out) != 0) {
            ++work->failures;
        }
    }
    sql_buffer_free(&out);
    return NULL;
}

static void *reader_main(void *argument)
{
    SharedWork *work = (SharedWork *)argument;
    SqlBuffer out;
    int i;

    sql_buffer_init(&out);
    for (i = 0; i < 200; ++i) {
        sql_buffer_clear(&out);
        /* id 1 ~ 5는 앞 테스트가 넣은 레코드라 언제 읽어도 있어야 한다. */
        if (sql_engine_execute(work->engine, "SELECT * FROM users WHERE id = 4;", &out) != 0 ||
            strstr(out.data, "\tPark\t") == NULL) {
            ++work->failures;
        }
        sql_buffer_clear(&out);
        if (sql_engine_execute(work->engine, "SELECT COUNT(*) FROM users WHERE age = 50;",
                               &out) != 0 || strstr(out.data, "count\n1\n") == NULL) {
            ++work->failures;
        }
    }
    sql_buffer_free(&out);
    return NULL;
}

static void test_concurrency(void)
{
    SqlEngine *engine = sql_engine_open(data_dir);
    pthread_t threads[WRITER_THREADS + READER_THREADS];
    SharedWork work[WRITER_THREADS + READER_THREADS];
    char expected[64];
    char sql[96];
    int i;

    assert(engine != NULL);
    for (i = 0; i < WRITER_THREADS + READER_THREADS; ++i) {
        work[i].engine = engine;
        work[i].writer_number = 100 + i;
        work[i].failures = 0;
        assert(pthread_create(&threads[i], NULL,
                              i < WRITER_THREADS ? writer_main : reader_main, &work[i]) == 0);
    }
    for (i = 0; i < WRITER_THREADS + READER_THREADS; ++i) {
        pthread_join(threads[i], NULL);
        assert(work[i].failures == 0);
    }

    /* 레코드가 하나도 사라지거나 겹치지 않았는지: 전체 건수와 writer별 건수를 센다. */
    snprintf(expected, sizeof(expected), "count\n%d\n", 5 + WRITER_THREADS * ROWS_PER_WRITER);
    assert(strstr(run(engine, "SELECT COUNT(*) FROM users;", 0), expected) != NULL);
    snprintf(expected, sizeof(expected), "count\n%d\n", ROWS_PER_WRITER);
    for (i = 0; i < WRITER_THREADS; ++i) {
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM users WHERE age = %d;", 100 + i);
        assert(strstr(run(engine, sql, 0), expected) != NULL);
    }
    /* 마지막 id까지 인덱스로 찾을 수 있어야 한다(id가 겹쳤다면 찾지 못한다). */
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM users WHERE id = %d;",
             5 + WRITER_THREADS * ROWS_PER_WRITER);
    assert(strstr(run(engine, sql, 0), "count\n1\n") != NULL);
    sql_engine_close(engine);
    puts("PASS 4: 4 writer and 4 reader threads share one engine without losing rows");
}

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void count_task(void *argument)
{
    (void)argument;
    pthread_mutex_lock(&counter_mutex);
    ++counter;
    pthread_mutex_unlock(&counter_mutex);
}

static void test_thread_pool(void)
{
    ThreadPool *pool = thread_pool_create(4, 8);
    int i;

    assert(pool != NULL);
    assert(thread_pool_create(0, 8) == NULL);
    assert(thread_pool_create(4, 0) == NULL);
    assert(thread_pool_submit(NULL, count_task, NULL) == -1);
    assert(thread_pool_submit(pool, NULL, NULL) == -1);
    /* 대기열(8칸)보다 훨씬 많은 작업을 넣어 "꽉 차면 기다림"도 거친다. */
    for (i = 0; i < POOL_TASKS; ++i) {
        assert(thread_pool_submit(pool, count_task, NULL) == 0);
    }
    /* destroy는 대기열에 남은 작업을 모두 끝낸 뒤에 돌아온다. */
    thread_pool_destroy(pool);
    assert(counter == POOL_TASKS);
    thread_pool_destroy(NULL);
    puts("PASS 5: thread pool runs every submitted task exactly once");
}

int main(void)
{
    make_data_dir();
    test_mem_bptree();
    test_parser();
    test_engine();
    test_concurrency();
    test_thread_pool();
    remove_data_dir();
    puts("All SQL processor tests passed.");
    return 0;
}
