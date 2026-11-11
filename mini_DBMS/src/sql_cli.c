#define _POSIX_C_SOURCE 200809L

/*
 * SQL 처리기의 CLI(Command Line Interface, 명령줄 실행 창구).
 *
 *   mini_dbms_sql <data_dir> <파일.sql>            파일에 적힌 SQL 문장들을 차례로 실행
 *   mini_dbms_sql <data_dir> -e "SQL; SQL"         명령줄에 적은 SQL 문장들을 실행
 *   mini_dbms_sql <data_dir>                       표준 입력으로 받은 SQL 문장들을 실행
 *   mini_dbms_sql <data_dir> --bench <테이블> <건수>
 *       레코드를 <건수>만큼 INSERT한 뒤, id 조회(인덱스)와 다른 컬럼 조회(선형 탐색)의 시간을 비교
 */

#include "sql.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 비교 측정에서 실행할 조회 횟수. 선형 탐색은 한 번이 오래 걸리므로 적게 돈다. */
#define BENCH_INDEX_QUERIES 10000
#define BENCH_SCAN_QUERIES 10

static double now_seconds(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* stream의 내용을 끝까지 읽어 문자열 하나로 돌려준다. 호출자가 free한다. */
static char *read_all(FILE *stream)
{
    SqlBuffer text;
    char chunk[4096];
    size_t received;

    sql_buffer_init(&text);
    while ((received = fread(chunk, 1, sizeof(chunk), stream)) > 0) {
        if (sql_buffer_append(&text, chunk, received) != 0) {
            sql_buffer_free(&text);
            return NULL;
        }
    }
    if (text.data == NULL && sql_buffer_append(&text, "", 0) != 0) {
        return NULL;
    }
    return text.data;
}

/* script 안의 문장을 하나씩 실행하고 결과를 출력한다. 실패한 문장 수를 돌려준다. */
static int run_script(SqlEngine *engine, const char *script)
{
    const char *cursor = script;
    char *statement = NULL;
    SqlBuffer out;
    int failures = 0;

    sql_buffer_init(&out);
    while ((cursor = sql_next_statement(cursor, &statement)) != NULL) {
        int has_failed;

        sql_buffer_clear(&out);
        has_failed = sql_engine_execute(engine, statement, &out) != 0;
        failures += has_failed;
        /* 결과는 표준 출력으로, 오류 한 줄은 표준 오류로 보낸다. */
        if (out.data != NULL) {
            fputs(out.data, has_failed ? stderr : stdout);
        }
        free(statement);
    }
    sql_buffer_free(&out);
    return failures;
}

/* 다음 난수(선형 합동 생성기). 측정마다 같은 순서가 나오도록 직접 만든다. */
static uint64_t next_random(uint64_t *state)
{
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state >> 33;
}

/*
 * 대량 삽입 + 조회 속도 비교.
 * INSERT도 SELECT도 SQL 문장을 만들어 sql_engine_execute에 넘긴다.
 * 즉 SQL 처리기(파싱 → 실행)를 그대로 거친다.
 */
static int run_bench(SqlEngine *engine, const char *table, unsigned long rows)
{
    SqlBuffer out;
    char sql[256];
    uint64_t seed = 20261111ULL;
    unsigned long first_id = 0;
    unsigned long i;
    double started;
    double insert_seconds;
    double index_seconds;
    double scan_seconds;

    sql_buffer_init(&out);
    started = now_seconds();
    for (i = 1; i <= rows; ++i) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO %s VALUES ('user_%lu', 'user_%lu@example.com', %lu);",
                 table, i, i, 20 + i % 50);
        sql_buffer_clear(&out);
        if (sql_engine_execute(engine, sql, &out) != 0) {
            fputs(out.data, stderr);
            sql_buffer_free(&out);
            return 1;
        }
        if (i == 1 && sscanf(out.data, "INSERT 1 (id=%lu)", &first_id) != 1) {
            fputs("could not read the first assigned id\n", stderr);
            sql_buffer_free(&out);
            return 1;
        }
    }
    insert_seconds = now_seconds() - started;

    /* id 기준 조회: B+ tree 인덱스를 쓴다. */
    started = now_seconds();
    for (i = 0; i < BENCH_INDEX_QUERIES; ++i) {
        unsigned long id = first_id + (unsigned long)(next_random(&seed) % rows);

        snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE id = %lu;", table, id);
        sql_buffer_clear(&out);
        if (sql_engine_execute(engine, sql, &out) != 0 ||
            strstr(out.data, "(1 row, index lookup)") == NULL) {
            fprintf(stderr, "index query failed: %s\n%s", sql, out.data);
            sql_buffer_free(&out);
            return 1;
        }
    }
    index_seconds = now_seconds() - started;

    /* 다른 컬럼 기준 조회: 파일 전체를 읽는 선형 탐색이다. */
    started = now_seconds();
    for (i = 0; i < BENCH_SCAN_QUERIES; ++i) {
        unsigned long number = 1 + (unsigned long)(next_random(&seed) % rows);

        snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE name = 'user_%lu';", table, number);
        sql_buffer_clear(&out);
        if (sql_engine_execute(engine, sql, &out) != 0 ||
            strstr(out.data, "full scan of") == NULL) {
            fprintf(stderr, "scan query failed: %s\n%s", sql, out.data);
            sql_buffer_free(&out);
            return 1;
        }
    }
    scan_seconds = now_seconds() - started;
    sql_buffer_free(&out);

    {
        double index_ms = index_seconds * 1000.0 / BENCH_INDEX_QUERIES;
        double scan_ms = scan_seconds * 1000.0 / BENCH_SCAN_QUERIES;

        printf("inserted rows            : %lu (%.2f s, %.0f rows/s)\n", rows,
               insert_seconds, (double)rows / insert_seconds);
        printf("SELECT WHERE id = ?      : %.4f ms per query (%d queries, B+ tree index)\n",
               index_ms, BENCH_INDEX_QUERIES);
        printf("SELECT WHERE name = ?    : %.4f ms per query (%d queries, full scan)\n",
               scan_ms, BENCH_SCAN_QUERIES);
        printf("full scan / index lookup : %.0f times slower\n", scan_ms / index_ms);
    }
    return 0;
}

static int usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s <data_dir> [file.sql | -e \"SQL; SQL\" | --bench <table> <rows>]\n",
            program);
    return 2;
}

int main(int argc, char **argv)
{
    SqlEngine *engine;
    char *script = NULL;
    int result;

    if (argc < 2) {
        return usage(argv[0]);
    }
    engine = sql_engine_open(argv[1]);
    if (engine == NULL) {
        fprintf(stderr, "could not open data directory '%s'\n", argv[1]);
        return 1;
    }

    if (argc == 5 && strcmp(argv[2], "--bench") == 0) {
        char *end = NULL;
        unsigned long rows = strtoul(argv[4], &end, 10);

        if (end == argv[4] || *end != '\0' || rows == 0) {
            sql_engine_close(engine);
            return usage(argv[0]);
        }
        result = run_bench(engine, argv[3], rows);
    } else if (argc == 4 && strcmp(argv[2], "-e") == 0) {
        result = run_script(engine, argv[3]) == 0 ? 0 : 1;
    } else if (argc == 3) {
        FILE *file = fopen(argv[2], "rb");

        if (file == NULL) {
            perror(argv[2]);
            sql_engine_close(engine);
            return 1;
        }
        script = read_all(file);
        fclose(file);
        result = script != NULL && run_script(engine, script) == 0 ? 0 : 1;
    } else if (argc == 2) {
        script = read_all(stdin);
        result = script != NULL && run_script(engine, script) == 0 ? 0 : 1;
    } else {
        sql_engine_close(engine);
        return usage(argv[0]);
    }

    free(script);
    sql_engine_close(engine);
    return result;
}
