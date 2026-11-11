#define _POSIX_C_SOURCE 200809L

#include "sql.h"
#include "mem_bptree.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PATH_CAPACITY 512
/* 테이블 파일을 처음부터 끝까지 읽을 때(선형 탐색) 쓰는 stdio buffer 크기. */
#define SCAN_BUFFER_SIZE (1024 * 1024)
/* id로 한 줄을 읽을 때 한 번에 읽는 크기. 줄이 더 길면 이 크기만큼씩 더 읽는다. */
#define ROW_READ_CHUNK 512

/* 열려 있는 테이블 하나. */
typedef struct SqlTable {
    char name[SQL_MAX_NAME];
    int column_count;
    char columns[SQL_MAX_COLUMNS][SQL_MAX_NAME];
    char path[PATH_CAPACITY];
    FILE *append;          /* 레코드를 파일 끝에 붙이는 stream */
    int read_fd;           /* id 조회에서 pread로 한 줄만 읽는 file descriptor */
    uint64_t next_id;      /* 다음 INSERT가 받을 id (자동 부여) */
    uint64_t row_count;
    uint64_t file_size;    /* 지금까지 쓴 byte 수 = 다음 레코드의 시작 위치 */
    int has_unflushed;     /* append stream에 아직 파일로 내려가지 않은 레코드가 있으면 1 */
    MemBPTree *index;      /* id → 레코드 시작 위치 */
    struct SqlTable *next;
} SqlTable;

struct SqlEngine {
    char data_dir[PATH_CAPACITY];
    SqlTable *tables;
    /* 테이블 목록(tables)을 지키는 lock. 테이블을 처음 열 때만 잡는다. */
    pthread_mutex_t tables_mutex;
    /* 레코드와 인덱스를 지키는 lock. SELECT는 읽기 lock, INSERT는 쓰기 lock을 잡는다. */
    pthread_rwlock_t data_lock;
    /* 1이면 INSERT마다 바로 파일로 내려보낸다(서버용). 0이면 모아서 쓴다(대량 삽입용). */
    int flush_each_insert;
};

/* ───────────── 값의 저장 형식 ───────────── */

/*
 * 값 안의 tab · 줄바꿈 · 역슬래시를 두 글자(\t \n \r \\)로 바꿔 buffer에 붙인다.
 * 이렇게 해야 "한 줄 = 레코드 한 건, tab = 컬럼 경계" 규칙이 깨지지 않는다.
 */
static int append_escaped(SqlBuffer *buffer, const char *value)
{
    const char *p;

    for (p = value; *p != '\0'; ++p) {
        int result;

        switch (*p) {
        case '\t': result = sql_buffer_append(buffer, "\\t", 2); break;
        case '\n': result = sql_buffer_append(buffer, "\\n", 2); break;
        case '\r': result = sql_buffer_append(buffer, "\\r", 2); break;
        case '\\': result = sql_buffer_append(buffer, "\\\\", 2); break;
        default: result = sql_buffer_append(buffer, p, 1); break;
        }
        if (result != 0) {
            return -1;
        }
    }
    return 0;
}

/* 실행 실패: out을 비우고 "ERROR: 이유" 한 줄만 남긴 뒤 -1을 돌려준다. */
static int fail(SqlBuffer *out, const char *reason, const char *detail)
{
    sql_buffer_clear(out);
    if (detail != NULL) {
        (void)sql_buffer_printf(out, "ERROR: %s '%s'\n", reason, detail);
    } else {
        (void)sql_buffer_printf(out, "ERROR: %s\n", reason);
    }
    return -1;
}

/* ───────────── 테이블 열기 ───────────── */

static int find_column(const SqlTable *table, const char *name)
{
    int i;

    for (i = 0; i < table->column_count; ++i) {
        if (strcmp(table->columns[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/* <테이블>.schema 파일의 첫 줄(쉼표로 나눈 컬럼 이름)을 읽는다. */
static int load_schema(SqlTable *table, const char *schema_path)
{
    char line[SQL_MAX_COLUMNS * (SQL_MAX_NAME + 2)];
    FILE *file = fopen(schema_path, "r");
    char *name;
    char *save = NULL;

    if (file == NULL) {
        return -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);
    for (name = strtok_r(line, ", \t\r\n", &save); name != NULL;
         name = strtok_r(NULL, ", \t\r\n", &save)) {
        if (table->column_count == SQL_MAX_COLUMNS || strlen(name) >= SQL_MAX_NAME) {
            return -1;
        }
        strcpy(table->columns[table->column_count++], name);
    }
    /* 첫 컬럼은 자동으로 번호를 매기는 id여야 하고, 그 뒤에 컬럼이 하나 이상 있어야 한다. */
    return table->column_count >= 2 && strcmp(table->columns[0], "id") == 0 ? 0 : -1;
}

/*
 * 테이블 파일을 처음부터 읽어 인덱스를 다시 만든다(프로그램을 다시 켰을 때).
 * 줄마다 맨 앞의 id를 읽어 "id → 그 줄의 시작 위치"를 B+ tree에 넣는다.
 * 줄바꿈 없이 끝난 마지막 줄은 쓰다가 중단된 레코드이므로 파일에서 잘라 낸다.
 */
static int rebuild_index(SqlTable *table)
{
    FILE *file = fopen(table->path, "rb");
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    uint64_t offset = 0;

    if (file == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    (void)setvbuf(file, NULL, _IOFBF, SCAN_BUFFER_SIZE);
    while ((length = getline(&line, &capacity, file)) > 0) {
        char *end = NULL;
        uint64_t id;

        if (line[length - 1] != '\n') {
            break;
        }
        errno = 0;
        id = strtoull(line, &end, 10);
        if (errno != 0 || end == line || *end != '\t' || id == 0) {
            free(line);
            fclose(file);
            return -1;
        }
        if (mem_bptree_insert(table->index, id, offset) != 0) {
            free(line);
            fclose(file);
            return -1;
        }
        if (id >= table->next_id) {
            table->next_id = id + 1;
        }
        ++table->row_count;
        offset += (uint64_t)length;
    }
    free(line);
    fclose(file);
    table->file_size = offset;
    if (truncate(table->path, (off_t)offset) != 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

static void table_close(SqlTable *table)
{
    if (table == NULL) {
        return;
    }
    if (table->append != NULL) {
        (void)fclose(table->append);
    }
    if (table->read_fd >= 0) {
        (void)close(table->read_fd);
    }
    mem_bptree_destroy(table->index);
    free(table);
}

/* 이름으로 테이블을 찾고, 처음 쓰는 테이블이면 schema와 데이터 파일을 연다. */
static SqlTable *get_table(SqlEngine *engine, const char *name)
{
    char schema_path[PATH_CAPACITY];
    SqlTable *table;

    pthread_mutex_lock(&engine->tables_mutex);
    for (table = engine->tables; table != NULL; table = table->next) {
        if (strcmp(table->name, name) == 0) {
            pthread_mutex_unlock(&engine->tables_mutex);
            return table;
        }
    }

    table = (SqlTable *)calloc(1, sizeof(SqlTable));
    if (table == NULL) {
        pthread_mutex_unlock(&engine->tables_mutex);
        return NULL;
    }
    table->read_fd = -1;
    table->next_id = 1;
    strcpy(table->name, name);
    if (snprintf(schema_path, sizeof(schema_path), "%s/%s.schema", engine->data_dir, name)
            >= (int)sizeof(schema_path) ||
        snprintf(table->path, sizeof(table->path), "%s/%s.tbl", engine->data_dir, name)
            >= (int)sizeof(table->path) ||
        load_schema(table, schema_path) != 0 ||
        (table->index = mem_bptree_create()) == NULL ||
        rebuild_index(table) != 0 ||
        (table->append = fopen(table->path, "ab")) == NULL ||
        (table->read_fd = open(table->path, O_RDONLY)) < 0) {
        table_close(table);
        pthread_mutex_unlock(&engine->tables_mutex);
        return NULL;
    }
    (void)setvbuf(table->append, NULL, _IOFBF, SCAN_BUFFER_SIZE);
    table->next = engine->tables;
    engine->tables = table;
    pthread_mutex_unlock(&engine->tables_mutex);
    return table;
}

SqlEngine *sql_engine_open(const char *data_dir)
{
    SqlEngine *engine;
    struct stat info;

    if (data_dir == NULL || strlen(data_dir) >= PATH_CAPACITY ||
        stat(data_dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
        return NULL;
    }
    engine = (SqlEngine *)calloc(1, sizeof(SqlEngine));
    if (engine == NULL) {
        return NULL;
    }
    strcpy(engine->data_dir, data_dir);
    if (pthread_mutex_init(&engine->tables_mutex, NULL) != 0) {
        free(engine);
        return NULL;
    }
    if (pthread_rwlock_init(&engine->data_lock, NULL) != 0) {
        pthread_mutex_destroy(&engine->tables_mutex);
        free(engine);
        return NULL;
    }
    return engine;
}

void sql_engine_set_flush_each_insert(SqlEngine *engine, int enabled)
{
    if (engine != NULL) {
        engine->flush_each_insert = enabled != 0;
    }
}

void sql_engine_close(SqlEngine *engine)
{
    SqlTable *table;

    if (engine == NULL) {
        return;
    }
    table = engine->tables;
    while (table != NULL) {
        SqlTable *next = table->next;
        table_close(table);   /* fclose가 남은 레코드를 파일로 내려보낸다. */
        table = next;
    }
    pthread_rwlock_destroy(&engine->data_lock);
    pthread_mutex_destroy(&engine->tables_mutex);
    free(engine);
}

/* ───────────── INSERT ───────────── */

/*
 * INSERT 실행: id를 자동으로 붙이고, 한 줄을 파일 끝에 쓰고, id를 인덱스에 등록한다.
 * 호출자가 쓰기 lock을 잡고 있어야 한다.
 */
static int execute_insert(SqlTable *table, const SqlStatement *statement, SqlBuffer *out)
{
    const char *row_values[SQL_MAX_COLUMNS];
    SqlBuffer line;
    uint64_t id = table->next_id;
    int i;

    for (i = 0; i < table->column_count; ++i) {
        row_values[i] = "";
    }
    if (statement->column_count == 0) {
        /* 컬럼 목록이 없으면 id를 뺀 모든 컬럼의 값을 schema 순서대로 받아야 한다. */
        if (statement->value_count != table->column_count - 1) {
            sql_buffer_clear(out);
            (void)sql_buffer_printf(out,
                "ERROR: table '%s' needs %d values (id is assigned automatically), got %d\n",
                table->name, table->column_count - 1, statement->value_count);
            return -1;
        }
        for (i = 0; i < statement->value_count; ++i) {
            row_values[i + 1] = statement->values[i];
        }
    } else {
        if (statement->column_count != statement->value_count) {
            return fail(out, "column count and value count differ", NULL);
        }
        for (i = 0; i < statement->column_count; ++i) {
            int column = find_column(table, statement->columns[i]);

            if (column < 0) {
                return fail(out, "unknown column", statement->columns[i]);
            }
            if (column == 0) {
                return fail(out, "id is assigned automatically and cannot be given", NULL);
            }
            row_values[column] = statement->values[i];
        }
    }

    sql_buffer_init(&line);
    if (sql_buffer_printf(&line, "%llu", (unsigned long long)id) != 0) {
        sql_buffer_free(&line);
        return fail(out, "out of memory", NULL);
    }
    for (i = 1; i < table->column_count; ++i) {
        if (sql_buffer_append(&line, "\t", 1) != 0 ||
            append_escaped(&line, row_values[i]) != 0) {
            sql_buffer_free(&line);
            return fail(out, "out of memory", NULL);
        }
    }
    if (sql_buffer_append(&line, "\n", 1) != 0) {
        sql_buffer_free(&line);
        return fail(out, "out of memory", NULL);
    }

    /* 파일에 먼저 쓰고, 성공했을 때만 인덱스와 다음 id를 바꾼다. */
    if (fwrite(line.data, 1, line.length, table->append) != line.length) {
        sql_buffer_free(&line);
        return fail(out, "could not write the table file", table->name);
    }
    if (mem_bptree_insert(table->index, id, table->file_size) != 0) {
        sql_buffer_free(&line);
        return fail(out, "out of memory while updating the index", NULL);
    }
    table->file_size += line.length;
    table->has_unflushed = 1;
    ++table->next_id;
    ++table->row_count;
    sql_buffer_free(&line);
    return sql_buffer_printf(out, "INSERT 1 (id=%llu)\n", (unsigned long long)id);
}

/* ───────────── SELECT ───────────── */

/*
 * 한 줄(끝의 '\n'은 이미 '\0'으로 바뀐 상태)을 tab에서 잘라 fields에 넣는다.
 * line 안의 tab을 '\0'으로 바꾸므로 line이 바뀐다. 컬럼 수를 돌려준다.
 */
static int split_row(char *line, char **fields, int max_fields)
{
    int count = 0;
    char *p = line;

    fields[count++] = p;
    while ((p = strchr(p, '\t')) != NULL && count < max_fields) {
        *p++ = '\0';
        fields[count++] = p;
    }
    return count;
}

/* 고른 컬럼(selected)만 tab으로 이어 결과 한 줄을 만든다. */
static int append_row(SqlBuffer *out, char **fields, const int *selected, int selected_count)
{
    int i;

    for (i = 0; i < selected_count; ++i) {
        const char *value = fields[selected[i]];

        if ((i > 0 && sql_buffer_append(out, "\t", 1) != 0) ||
            sql_buffer_append(out, value, strlen(value)) != 0) {
            return -1;
        }
    }
    return sql_buffer_append(out, "\n", 1);
}

/* 인덱스가 알려 준 위치에서 레코드 한 줄을 읽는다. 돌려준 문자열은 호출자가 free한다. */
static char *read_row_at(const SqlTable *table, uint64_t offset)
{
    size_t capacity = ROW_READ_CHUNK;
    size_t length = 0;
    char *line = (char *)malloc(capacity + 1);

    if (line == NULL) {
        return NULL;
    }
    for (;;) {
        ssize_t received = pread(table->read_fd, line + length, capacity - length,
                                 (off_t)(offset + length));
        char *newline;

        if (received <= 0) {
            free(line);
            return NULL;
        }
        newline = (char *)memchr(line + length, '\n', (size_t)received);
        length += (size_t)received;
        if (newline != NULL) {
            *newline = '\0';
            return line;
        }
        if (length == capacity) {
            char *grown;

            capacity *= 2;
            grown = (char *)realloc(line, capacity + 1);
            if (grown == NULL) {
                free(line);
                return NULL;
            }
            line = grown;
        }
    }
}

/*
 * SELECT 실행. 호출자가 읽기 lock을 잡고 있어야 하고, 파일에 안 내려간 레코드가 없어야 한다.
 *   WHERE id = 숫자  → B+ tree 인덱스로 위치를 찾아 그 한 줄만 읽는다(index lookup).
 *   그 밖의 WHERE    → 파일의 모든 줄을 읽으며 비교한다(선형 탐색, full scan).
 */
static int execute_select(const SqlTable *table, const SqlStatement *statement, SqlBuffer *out)
{
    int selected[SQL_MAX_COLUMNS];
    int selected_count = 0;
    int where_column = -1;
    char *fields[SQL_MAX_COLUMNS];
    uint64_t matched = 0;
    int i;

    if (statement->column_count == 0) {
        for (i = 0; i < table->column_count; ++i) {
            selected[selected_count++] = i;
        }
    } else {
        for (i = 0; i < statement->column_count; ++i) {
            int column = find_column(table, statement->columns[i]);

            if (column < 0) {
                return fail(out, "unknown column", statement->columns[i]);
            }
            selected[selected_count++] = column;
        }
    }
    if (statement->has_where) {
        where_column = find_column(table, statement->where_column);
        if (where_column < 0) {
            return fail(out, "unknown column", statement->where_column);
        }
    }

    /* 결과 첫 줄: 컬럼 이름. */
    if (statement->is_count) {
        if (sql_buffer_append(out, "count\n", 6) != 0) {
            return fail(out, "out of memory", NULL);
        }
    } else {
        for (i = 0; i < selected_count; ++i) {
            const char *name = table->columns[selected[i]];

            if ((i > 0 && sql_buffer_append(out, "\t", 1) != 0) ||
                sql_buffer_append(out, name, strlen(name)) != 0) {
                return fail(out, "out of memory", NULL);
            }
        }
        if (sql_buffer_append(out, "\n", 1) != 0) {
            return fail(out, "out of memory", NULL);
        }
    }

    /* 경로 1: 조건 없는 COUNT(*)는 테이블이 기억하는 건수를 바로 답한다. */
    if (statement->is_count && !statement->has_where) {
        return sql_buffer_printf(out, "%llu\n(1 row, table metadata)\n",
                                 (unsigned long long)table->row_count);
    }

    /* 경로 2: WHERE id = 숫자 → 인덱스 사용. */
    if (where_column == 0) {
        char *end = NULL;
        uint64_t id;
        uint64_t offset = 0;

        errno = 0;
        id = strtoull(statement->where_value, &end, 10);
        if (errno == 0 && end != statement->where_value && *end == '\0' &&
            statement->where_value[0] != '-' &&
            mem_bptree_find(table->index, id, &offset)) {
            char *line = read_row_at(table, offset);

            if (line == NULL) {
                return fail(out, "could not read the table file", table->name);
            }
            if (split_row(line, fields, SQL_MAX_COLUMNS) != table->column_count) {
                free(line);
                return fail(out, "damaged row in table", table->name);
            }
            matched = 1;
            if (!statement->is_count &&
                append_row(out, fields, selected, selected_count) != 0) {
                free(line);
                return fail(out, "out of memory", NULL);
            }
            free(line);
        }
        if (statement->is_count) {
            (void)sql_buffer_printf(out, "%llu\n", (unsigned long long)matched);
            matched = 1;
        }
        return sql_buffer_printf(out, "(%llu row%s, index lookup)\n",
                                 (unsigned long long)matched, matched == 1 ? "" : "s");
    }

    /* 경로 3: 선형 탐색. 저장 형식과 같은 모양으로 바꾼 조건 값과 줄마다 비교한다. */
    {
        SqlBuffer wanted;
        FILE *file;
        char *line = NULL;
        size_t capacity = 0;
        ssize_t length;
        uint64_t scanned = 0;

        sql_buffer_init(&wanted);
        if (statement->has_where &&
            (append_escaped(&wanted, statement->where_value) != 0 ||
             sql_buffer_append(&wanted, "", 0) != 0)) {
            sql_buffer_free(&wanted);
            return fail(out, "out of memory", NULL);
        }
        file = fopen(table->path, "rb");
        if (file == NULL) {
            sql_buffer_free(&wanted);
            return fail(out, "could not read the table file", table->name);
        }
        (void)setvbuf(file, NULL, _IOFBF, SCAN_BUFFER_SIZE);
        while ((length = getline(&line, &capacity, file)) > 0) {
            if (line[length - 1] != '\n') {
                break;
            }
            line[length - 1] = '\0';
            ++scanned;
            if (split_row(line, fields, SQL_MAX_COLUMNS) != table->column_count) {
                free(line);
                fclose(file);
                sql_buffer_free(&wanted);
                return fail(out, "damaged row in table", table->name);
            }
            if (statement->has_where && strcmp(fields[where_column], wanted.data) != 0) {
                continue;
            }
            ++matched;
            if (!statement->is_count &&
                append_row(out, fields, selected, selected_count) != 0) {
                free(line);
                fclose(file);
                sql_buffer_free(&wanted);
                return fail(out, "out of memory", NULL);
            }
        }
        free(line);
        fclose(file);
        sql_buffer_free(&wanted);
        if (statement->is_count) {
            (void)sql_buffer_printf(out, "%llu\n", (unsigned long long)matched);
            matched = 1;
        }
        return sql_buffer_printf(out, "(%llu row%s, full scan of %llu rows)\n",
                                 (unsigned long long)matched, matched == 1 ? "" : "s",
                                 (unsigned long long)scanned);
    }
}

/* ───────────── 실행 진입점 ───────────── */

int sql_engine_execute(SqlEngine *engine, const char *sql, SqlBuffer *out)
{
    SqlStatement statement;
    char error[160];
    SqlTable *table;
    int result;

    if (engine == NULL || sql == NULL || out == NULL) {
        return -1;
    }
    /* 파싱은 공유 자료를 건드리지 않으므로 lock 없이 한다. */
    if (sql_parse(sql, &statement, error, sizeof(error)) != 0) {
        return fail(out, error, NULL);
    }
    table = get_table(engine, statement.table);
    if (table == NULL) {
        sql_statement_free(&statement);
        return fail(out, "unknown table (missing or invalid schema file)", statement.table);
    }

    if (statement.type == SQL_STATEMENT_INSERT) {
        pthread_rwlock_wrlock(&engine->data_lock);
        result = execute_insert(table, &statement, out);
        if (result == 0 && engine->flush_each_insert) {
            if (fflush(table->append) == 0) {
                table->has_unflushed = 0;
            } else {
                result = fail(out, "could not write the table file", table->name);
            }
        }
        pthread_rwlock_unlock(&engine->data_lock);
    } else {
        /*
         * SELECT는 파일을 직접 읽으므로, append stream에 남은 레코드를 먼저 파일로 내려야 한다.
         * 내리는 일은 쓰기 lock으로, 읽는 일은 읽기 lock으로 한다.
         * 두 lock 사이에 다른 thread가 INSERT를 하면 다시 내려야 하므로 반복해서 확인한다.
         */
        int flush_failed = 0;

        for (;;) {
            pthread_rwlock_rdlock(&engine->data_lock);
            if (!table->has_unflushed) {
                break;
            }
            pthread_rwlock_unlock(&engine->data_lock);
            pthread_rwlock_wrlock(&engine->data_lock);
            if (table->has_unflushed) {
                if (fflush(table->append) == 0) {
                    table->has_unflushed = 0;
                } else {
                    flush_failed = 1;
                }
            }
            pthread_rwlock_unlock(&engine->data_lock);
            if (flush_failed) {
                sql_statement_free(&statement);
                return fail(out, "could not write the table file", table->name);
            }
        }
        result = execute_select(table, &statement, out);
        pthread_rwlock_unlock(&engine->data_lock);
    }
    sql_statement_free(&statement);
    return result;
}
