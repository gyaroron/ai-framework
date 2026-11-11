#ifndef MINI_DBMS_MEM_BPTREE_H
#define MINI_DBMS_MEM_BPTREE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 메모리 기반 B+ tree 인덱스(index).
 * key는 레코드의 id, value는 그 레코드가 테이블 파일에서 시작하는 위치(byte offset)다.
 * 디스크 page를 쓰는 bplus_tree.c와 달리 node를 malloc으로 메모리에만 둔다.
 * 프로그램이 다시 시작하면 테이블 파일을 한 번 읽어 다시 만든다.
 */

/* node 하나가 가질 수 있는 key의 최대 개수. 이 수를 넘으면 node를 둘로 나눈다(split). */
#define MEM_BPTREE_MAX_KEYS 64

typedef struct MemBPTree MemBPTree;

/* 빈 tree를 만든다. 메모리가 모자라면 NULL. */
MemBPTree *mem_bptree_create(void);

/* tree와 모든 node를 해제한다. NULL을 넘겨도 된다. */
void mem_bptree_destroy(MemBPTree *tree);

/* key → value를 넣는다. 이미 있는 key면 value를 바꾼다. 성공 0, 메모리 부족 -1. */
int mem_bptree_insert(MemBPTree *tree, uint64_t key, uint64_t value);

/* key를 찾아 *out_value에 넣는다. 찾으면 1, 없으면 0. */
int mem_bptree_find(const MemBPTree *tree, uint64_t key, uint64_t *out_value);

/* 들어 있는 key의 개수. */
size_t mem_bptree_count(const MemBPTree *tree);

/* root에서 leaf까지의 node 층 수. 빈 tree도 leaf 하나가 있으므로 1이다. */
unsigned int mem_bptree_height(const MemBPTree *tree);

/*
 * leaf를 왼쪽부터 차례로 돌며 key가 오름차순인지, 개수가 맞는지 확인한다(테스트용).
 * 구조가 올바르면 1, 아니면 0.
 */
int mem_bptree_check(const MemBPTree *tree);

#endif
