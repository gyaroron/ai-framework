#ifndef MINI_DBMS_SQL_H
#define MINI_DBMS_SQL_H

#include <stddef.h>

/*
 * SQL 처리기(SQL processor): SQL 문장 → 파싱(parsing) → 실행(execution) → 파일 저장.
 *
 * 지원하는 문장
 *   INSERT INTO 테이블 VALUES (값, 값, ...);
 *   INSERT INTO 테이블 (컬럼, 컬럼) VALUES (값, 값);
 *   SELECT * FROM 테이블 [WHERE 컬럼 = 값];
 *   SELECT 컬럼, 컬럼 FROM 테이블 [WHERE 컬럼 = 값];
 *   SELECT COUNT(*) FROM 테이블 [WHERE 컬럼 = 값];
 *
 * 테이블과 schema는 이미 있다고 가정한다(CREATE TABLE 없음).
 *   <data_dir>/<테이블>.schema  컬럼 이름을 쉼표로 적은 한 줄. 첫 컬럼은 반드시 id.
 *   <data_dir>/<테이블>.tbl     레코드 한 건이 한 줄. 컬럼 값은 tab으로 나눈다.
 */

#define SQL_MAX_COLUMNS 16
#define SQL_MAX_NAME 32

typedef enum SqlStatementType {
    SQL_STATEMENT_INSERT,
    SQL_STATEMENT_SELECT
} SqlStatementType;

/* 파싱 결과: SQL 문장 하나를 구조체로 풀어 놓은 것. */
typedef struct SqlStatement {
    SqlStatementType type;
    char table[SQL_MAX_NAME];
    /* INSERT의 컬럼 목록 또는 SELECT의 컬럼 목록. 0개면 "전체 컬럼"이라는 뜻이다. */
    int column_count;
    char columns[SQL_MAX_COLUMNS][SQL_MAX_NAME];
    /* INSERT의 VALUES 안 값들. malloc으로 잡은 문자열이다. */
    int value_count;
    char *values[SQL_MAX_COLUMNS];
    /* SELECT COUNT(*)이면 1. */
    int is_count;
    /* WHERE 컬럼 = 값 이 있으면 1. where_value는 malloc으로 잡은 문자열이다. */
    int has_where;
    char where_column[SQL_MAX_NAME];
    char *where_value;
} SqlStatement;

/*
 * SQL 문장 하나를 파싱한다. 끝의 ';'은 있어도 되고 없어도 된다.
 * 성공하면 0. 실패하면 -1을 돌려주고 error에 이유를 적는다.
 * 성공한 결과는 sql_statement_free로 해제한다.
 */
int sql_parse(const char *text, SqlStatement *out, char *error, size_t error_size);

void sql_statement_free(SqlStatement *statement);

/*
 * 여러 문장이 든 script에서 다음 문장 하나를 떼어 낸다(';' 기준, 따옴표 안의 ';'은 무시).
 * *out_statement에 malloc으로 잡은 문장을 넣고, 그다음 읽을 위치를 돌려준다.
 * 더 이상 문장이 없으면 NULL을 돌려주고 *out_statement도 NULL이다.
 */
const char *sql_next_statement(const char *script, char **out_statement);

/* 실행 결과를 담는, 필요한 만큼 커지는 문자열. */
typedef struct SqlBuffer {
    char *data;
    size_t length;
    size_t capacity;
} SqlBuffer;

void sql_buffer_init(SqlBuffer *buffer);
void sql_buffer_free(SqlBuffer *buffer);
void sql_buffer_clear(SqlBuffer *buffer);
int sql_buffer_append(SqlBuffer *buffer, const char *text, size_t length);
int sql_buffer_printf(SqlBuffer *buffer, const char *format, ...);

typedef struct SqlEngine SqlEngine;

/* data_dir 폴더의 테이블 파일을 쓰는 엔진을 연다. 실패하면 NULL. */
SqlEngine *sql_engine_open(const char *data_dir);

/*
 * enabled가 1이면 INSERT 한 건마다 바로 파일로 내려보낸다. API 서버가 켠다.
 * 0(기본값)이면 메모리에 모았다가 SELECT 직전이나 닫을 때 한꺼번에 쓴다. 대량 삽입이 빨라진다.
 */
void sql_engine_set_flush_each_insert(SqlEngine *engine, int enabled);

/* 아직 파일에 내려가지 않은 레코드를 쓰고, 열린 테이블과 인덱스를 모두 닫는다. */
void sql_engine_close(SqlEngine *engine);

/*
 * SQL 문장 하나를 실행하고 결과 글자를 out 뒤에 붙인다.
 * 여러 thread가 같은 엔진으로 동시에 불러도 된다.
 *   SELECT끼리는 함께 실행되고(읽기 lock), INSERT는 혼자 실행된다(쓰기 lock).
 * 성공하면 0. 실패하면 -1이고 out에는 "ERROR: 이유" 한 줄이 들어간다.
 */
int sql_engine_execute(SqlEngine *engine, const char *sql, SqlBuffer *out);

#endif
