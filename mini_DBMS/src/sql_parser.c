#define _POSIX_C_SOURCE 200809L

#include "sql.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ───────────── 결과 buffer ───────────── */

void sql_buffer_init(SqlBuffer *buffer)
{
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

void sql_buffer_free(SqlBuffer *buffer)
{
    free(buffer->data);
    sql_buffer_init(buffer);
}

void sql_buffer_clear(SqlBuffer *buffer)
{
    buffer->length = 0;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

/* 글자를 뒤에 붙인다. 모자라면 용량을 두 배씩 늘린다. 항상 '\0'으로 끝난다. */
int sql_buffer_append(SqlBuffer *buffer, const char *text, size_t length)
{
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        char *grown;

        while (capacity < buffer->length + length + 1) {
            capacity *= 2;
        }
        grown = (char *)realloc(buffer->data, capacity);
        if (grown == NULL) {
            return -1;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

int sql_buffer_printf(SqlBuffer *buffer, const char *format, ...)
{
    char small[256];
    va_list arguments;
    int needed;

    va_start(arguments, format);
    needed = vsnprintf(small, sizeof(small), format, arguments);
    va_end(arguments);
    if (needed < 0) {
        return -1;
    }
    if ((size_t)needed < sizeof(small)) {
        return sql_buffer_append(buffer, small, (size_t)needed);
    }
    {
        /* 256 byte에 다 들어가지 않은 긴 글자는 필요한 크기만큼 잡아 다시 만든다. */
        char *large = (char *)malloc((size_t)needed + 1);
        int result;

        if (large == NULL) {
            return -1;
        }
        va_start(arguments, format);
        (void)vsnprintf(large, (size_t)needed + 1, format, arguments);
        va_end(arguments);
        result = sql_buffer_append(buffer, large, (size_t)needed);
        free(large);
        return result;
    }
}

/* ───────────── 토큰 나누기(tokenizer) ───────────── */

typedef enum TokenType {
    TOKEN_END,      /* 문장 끝 */
    TOKEN_WORD,     /* 키워드 · 이름 · 숫자 (따옴표 없는 글자 묶음) */
    TOKEN_STRING,   /* '작은따옴표 문자열' */
    TOKEN_SYMBOL,   /* ( ) , = ; * 가운데 한 글자 */
    TOKEN_INVALID   /* 닫히지 않은 따옴표 · 모르는 글자 */
} TokenType;

typedef struct Token {
    TokenType type;
    char *text;     /* malloc으로 잡은 글자. TOKEN_END면 NULL. */
} Token;

typedef struct Parser {
    const char *cursor;
    Token current;
    char *error;
    size_t error_size;
    int failed;
} Parser;

static void set_error(Parser *parser, const char *format, ...)
{
    va_list arguments;

    if (parser->failed) {
        return;   /* 첫 번째 오류만 남긴다. */
    }
    parser->failed = 1;
    if (parser->error == NULL || parser->error_size == 0) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(parser->error, parser->error_size, format, arguments);
    va_end(arguments);
}

static int is_word_char(int c)
{
    return isalnum(c) || c == '_' || c == '.' || c == '-' || c == '+' || c == '@' ||
           c == ':' || c == '/';
}

static char *copy_range(const char *start, size_t length)
{
    char *text = (char *)malloc(length + 1);

    if (text != NULL) {
        memcpy(text, start, length);
        text[length] = '\0';
    }
    return text;
}

/* cursor 위치에서 다음 토큰 하나를 읽어 parser->current에 넣는다. */
static void next_token(Parser *parser)
{
    const char *p = parser->cursor;

    free(parser->current.text);
    parser->current.text = NULL;
    while (isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p == '\0') {
        parser->current.type = TOKEN_END;
        parser->cursor = p;
        return;
    }
    if (*p == '\'') {
        /* 문자열: 닫는 따옴표까지 읽는다. 문자열 안의 따옴표는 '' 두 개로 쓴다. */
        SqlBuffer text;

        sql_buffer_init(&text);
        ++p;
        for (;;) {
            if (*p == '\0') {
                sql_buffer_free(&text);
                parser->current.type = TOKEN_INVALID;
                parser->cursor = p;
                set_error(parser, "unterminated string literal");
                return;
            }
            if (*p == '\'') {
                if (p[1] == '\'') {
                    (void)sql_buffer_append(&text, "'", 1);
                    p += 2;
                    continue;
                }
                ++p;
                break;
            }
            (void)sql_buffer_append(&text, p, 1);
            ++p;
        }
        parser->current.type = TOKEN_STRING;
        parser->current.text = text.data != NULL ? text.data : copy_range("", 0);
        parser->cursor = p;
        return;
    }
    if (strchr("(),=;*", *p) != NULL) {
        parser->current.type = TOKEN_SYMBOL;
        parser->current.text = copy_range(p, 1);
        parser->cursor = p + 1;
        return;
    }
    if (is_word_char((unsigned char)*p)) {
        const char *start = p;

        while (is_word_char((unsigned char)*p)) {
            ++p;
        }
        parser->current.type = TOKEN_WORD;
        parser->current.text = copy_range(start, (size_t)(p - start));
        parser->cursor = p;
        return;
    }
    parser->current.type = TOKEN_INVALID;
    parser->cursor = p + 1;
    set_error(parser, "unexpected character '%c'", *p);
}

static int is_symbol(const Parser *parser, char symbol)
{
    return parser->current.type == TOKEN_SYMBOL && parser->current.text[0] == symbol;
}

static int is_keyword(const Parser *parser, const char *keyword)
{
    return parser->current.type == TOKEN_WORD &&
           strcasecmp(parser->current.text, keyword) == 0;
}

/* 지금 토큰이 keyword면 넘어가고, 아니면 오류를 남긴다. */
static void expect_keyword(Parser *parser, const char *keyword)
{
    if (parser->failed) {
        return;
    }
    if (!is_keyword(parser, keyword)) {
        set_error(parser, "expected %s near '%s'", keyword,
                  parser->current.text != NULL ? parser->current.text : "end of statement");
        return;
    }
    next_token(parser);
}

static void expect_symbol(Parser *parser, char symbol)
{
    if (parser->failed) {
        return;
    }
    if (!is_symbol(parser, symbol)) {
        set_error(parser, "expected '%c' near '%s'", symbol,
                  parser->current.text != NULL ? parser->current.text : "end of statement");
        return;
    }
    next_token(parser);
}

/* 테이블 · 컬럼 이름 하나를 읽어 destination에 복사한다. 영문자 · 숫자 · _ 만 받는다. */
static void parse_name(Parser *parser, char *destination, const char *what)
{
    const char *p;

    if (parser->failed) {
        return;
    }
    if (parser->current.type != TOKEN_WORD) {
        set_error(parser, "expected %s name", what);
        return;
    }
    if (strlen(parser->current.text) >= SQL_MAX_NAME) {
        set_error(parser, "%s name is too long", what);
        return;
    }
    for (p = parser->current.text; *p != '\0'; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            set_error(parser, "invalid %s name '%s'", what, parser->current.text);
            return;
        }
    }
    strcpy(destination, parser->current.text);
    next_token(parser);
}

/* 값 하나(숫자 · 따옴표 없는 글자 · 문자열)를 읽어 malloc 문자열로 돌려준다. */
static char *parse_value(Parser *parser)
{
    char *value;

    if (parser->failed) {
        return NULL;
    }
    if (parser->current.type != TOKEN_WORD && parser->current.type != TOKEN_STRING) {
        set_error(parser, "expected a value");
        return NULL;
    }
    value = parser->current.text;
    parser->current.text = NULL;   /* 소유권을 호출자에게 넘긴다. */
    next_token(parser);
    return value;
}

/* INSERT INTO 테이블 [(컬럼, ...)] VALUES (값, ...) */
static void parse_insert(Parser *parser, SqlStatement *out)
{
    out->type = SQL_STATEMENT_INSERT;
    expect_keyword(parser, "INTO");
    parse_name(parser, out->table, "table");
    if (!parser->failed && is_symbol(parser, '(')) {
        next_token(parser);
        for (;;) {
            if (out->column_count == SQL_MAX_COLUMNS) {
                set_error(parser, "too many columns");
                return;
            }
            parse_name(parser, out->columns[out->column_count], "column");
            if (parser->failed) {
                return;
            }
            ++out->column_count;
            if (!is_symbol(parser, ',')) {
                break;
            }
            next_token(parser);
        }
        expect_symbol(parser, ')');
    }
    expect_keyword(parser, "VALUES");
    expect_symbol(parser, '(');
    while (!parser->failed) {
        if (out->value_count == SQL_MAX_COLUMNS) {
            set_error(parser, "too many values");
            return;
        }
        out->values[out->value_count] = parse_value(parser);
        if (parser->failed) {
            return;
        }
        ++out->value_count;
        if (!is_symbol(parser, ',')) {
            break;
        }
        next_token(parser);
    }
    expect_symbol(parser, ')');
}

/* SELECT * | COUNT(*) | 컬럼, ... FROM 테이블 [WHERE 컬럼 = 값] */
static void parse_select(Parser *parser, SqlStatement *out)
{
    out->type = SQL_STATEMENT_SELECT;
    if (is_symbol(parser, '*')) {
        next_token(parser);
    } else if (is_keyword(parser, "COUNT")) {
        next_token(parser);
        expect_symbol(parser, '(');
        expect_symbol(parser, '*');
        expect_symbol(parser, ')');
        out->is_count = 1;
    } else {
        for (;;) {
            if (out->column_count == SQL_MAX_COLUMNS) {
                set_error(parser, "too many columns");
                return;
            }
            parse_name(parser, out->columns[out->column_count], "column");
            if (parser->failed) {
                return;
            }
            ++out->column_count;
            if (!is_symbol(parser, ',')) {
                break;
            }
            next_token(parser);
        }
    }
    expect_keyword(parser, "FROM");
    parse_name(parser, out->table, "table");
    if (!parser->failed && is_keyword(parser, "WHERE")) {
        next_token(parser);
        parse_name(parser, out->where_column, "column");
        expect_symbol(parser, '=');
        out->where_value = parse_value(parser);
        out->has_where = !parser->failed;
    }
}

int sql_parse(const char *text, SqlStatement *out, char *error, size_t error_size)
{
    Parser parser;

    if (text == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    memset(&parser, 0, sizeof(parser));
    parser.cursor = text;
    parser.error = error;
    parser.error_size = error_size;
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }

    next_token(&parser);
    if (parser.failed) {
        /* 첫 토큰부터 잘못된 경우: 아래에서 정리한다. */
    } else if (is_keyword(&parser, "INSERT")) {
        next_token(&parser);
        parse_insert(&parser, out);
    } else if (is_keyword(&parser, "SELECT")) {
        next_token(&parser);
        parse_select(&parser, out);
    } else if (parser.current.type == TOKEN_END) {
        set_error(&parser, "empty statement");
    } else {
        set_error(&parser, "unsupported statement '%s' (only INSERT and SELECT)",
                  parser.current.text != NULL ? parser.current.text : "");
    }

    /* 문장 뒤에는 ';' 하나까지만 올 수 있다. */
    if (!parser.failed && is_symbol(&parser, ';')) {
        next_token(&parser);
    }
    if (!parser.failed && parser.current.type != TOKEN_END) {
        set_error(&parser, "unexpected '%s' after the statement",
                  parser.current.text != NULL ? parser.current.text : "");
    }
    free(parser.current.text);
    if (parser.failed) {
        sql_statement_free(out);
        return -1;
    }
    return 0;
}

void sql_statement_free(SqlStatement *statement)
{
    int i;

    if (statement == NULL) {
        return;
    }
    for (i = 0; i < statement->value_count; ++i) {
        free(statement->values[i]);
        statement->values[i] = NULL;
    }
    statement->value_count = 0;
    free(statement->where_value);
    statement->where_value = NULL;
    statement->has_where = 0;
}

const char *sql_next_statement(const char *script, char **out_statement)
{
    const char *start;
    const char *p;
    int in_string = 0;

    *out_statement = NULL;
    if (script == NULL) {
        return NULL;
    }
    for (;;) {
        /* 문장 앞의 공백과 "-- 주석" 줄을 건너뛴다. */
        while (isspace((unsigned char)*script)) {
            ++script;
        }
        if (script[0] == '-' && script[1] == '-') {
            while (*script != '\0' && *script != '\n') {
                ++script;
            }
            continue;
        }
        break;
    }
    if (*script == '\0') {
        return NULL;
    }
    start = script;
    for (p = script; *p != '\0'; ++p) {
        if (*p == '\'') {
            in_string = !in_string;
        } else if (*p == ';' && !in_string) {
            ++p;
            break;
        }
    }
    *out_statement = copy_range(start, (size_t)(p - start));
    return *out_statement == NULL ? NULL : p;
}
