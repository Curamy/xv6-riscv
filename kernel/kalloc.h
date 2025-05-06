#ifndef _KALLOC_H_
#define _KALLOC_H_
#include "types.h"

struct spinlock;

struct run {
  struct run *next;
};

// kmem 구조체 선언
extern struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void kinit(void);
void* kalloc(void);
void kfree(void*);
uint64 getfreemem(void);  // 사용 가능한 메모리 크기 반환
int getfreepagescount(void); // 사용 가능한 페이지 수 반환

#endif // _KALLOC_H_ 