#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "kalloc.h"
const uint64 weight[40] = {
  88761, 71755, 56483, 46273, 36291,
  29154, 23254, 18705, 14949, 11916,
  9548, 7620, 6100, 4904, 3906,
  3121, 2501, 1991, 1586, 1277,
  1024, 820, 655, 526, 423,
  335, 272, 215, 172, 137,
  110, 87, 70, 56, 45,
  36, 29, 23, 18, 15
};

struct spinlock rq_lock;
uint64 rq_weighted_diff = 0;  // sigma((vi - v0) * wi)
uint64 rq_min_vrun = 0;  // minimum vruntime of the runqueue
uint64 rq_weight_sum = 0; // sigma(wi)

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

//  run queue에 프로세스 p를 추가
static void
rq_add(struct proc *p)
{
  acquire(&rq_lock);

  if (rq_weight_sum == 0 || p->vruntime < rq_min_vrun) {  // 새로운 min_vrun
    rq_min_vrun = p->vruntime;
    rq_weighted_diff = 0;

    struct proc *q;
    for (q = proc; q < &proc[NPROC]; q++) {
      if (q->state == RUNNABLE || q->state == RUNNING) {
        rq_weighted_diff += (q->vruntime - rq_min_vrun) * q->weight;
      }
    }
  } else {  // 그냥 큐에 추가만 하는 경우
    rq_weighted_diff += (p->vruntime - rq_min_vrun) * p->weight;
  }

  rq_weight_sum += p->weight;
  release(&rq_lock);
}

//  run queue에서 프로세스 p를 제거
static void
rq_remove(struct proc *p)
{
  acquire(&rq_lock);

  rq_weight_sum -= p->weight;
  rq_weighted_diff -= (p->vruntime - rq_min_vrun) * p->weight;

  if (rq_weight_sum == 0) {
    rq_min_vrun = 0;
    rq_weighted_diff = 0;
  }
  else if (p->vruntime == rq_min_vrun) {  //  min_vrun이 제거된 경우
    struct proc *q;
    rq_min_vrun = (uint64)-1; //UINT64_MAX
    for (q = proc; q < &proc[NPROC]; q++) {
      if ((q->state == RUNNABLE || q->state == RUNNING) && q != p) {
        if (q->vruntime < rq_min_vrun) {
          rq_min_vrun = q->vruntime;
        }
      }
    }

    rq_weighted_diff = 0;
    for (q = proc; q < &proc[NPROC]; q++) {
      if (q->state == RUNNABLE || q->state == RUNNING) {
        rq_weighted_diff += (q->vruntime - rq_min_vrun) * q->weight;
      }
    }
  }

  release(&rq_lock);
}

//  부팅 완료 후 1회만 연산
static void
rq_init(void)
{
  rq_weight_sum = 0;
  rq_min_vrun = (uint64)-1; //UINT64_MAX
  rq_weighted_diff = 0;
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == RUNNABLE || p->state == RUNNING) {
      if (p->vruntime < rq_min_vrun) {
        rq_min_vrun = p->vruntime;
      }
    }
  }

  for (p = proc; p < &proc[NPROC]; p++) {
    if ((p->state == RUNNABLE || p->state == RUNNING) && p->weight != 0) {
      rq_weight_sum += p->weight;
      rq_weighted_diff += (p->vruntime - rq_min_vrun) * p->weight;
    }
  }
}

//  eligibility 체크
static inline int
is_eligible(struct proc *p)
{
  acquire(&rq_lock);
  int eligible = (rq_weighted_diff >= (p->vruntime - rq_min_vrun) * rq_weight_sum);
  release(&rq_lock);
  return eligible;
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&rq_lock, "rq_lock");

  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  p->nice = DEFAULT_NICE; // 초기 nice 값을 기본값 20으로 설정
  p->weight = weight[DEFAULT_NICE]; // 초기 weight 값을 기본값으로 설정
  p->vruntime = 0;
  p->vdeadline = (weight[DEFAULT_NICE] * DEFAULT_TIME_SLICE) / p->weight;
  p->runtime = 0;
  p->timeslice = DEFAULT_TIME_SLICE; // 초기 time slice 값을 기본값으로 설정

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  rq_add(p); // run queue에 추가

  release(&p->lock);

  rq_init(); // run queue initialization
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->nice = p->nice;
  np->weight = weight[p->nice];
  np->vruntime = p->vruntime;
  np->runtime = 0;
  np->timeslice = DEFAULT_TIME_SLICE;
  np->vdeadline = np->vruntime + (weight[DEFAULT_NICE] * DEFAULT_TIME_SLICE) / np->weight;

  np->state = RUNNABLE;
  rq_add(np); // run queue에 추가
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  rq_remove(p); // run queue에서 제거
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    rq_init();

    uint64 min_vdeadline = (uint64)-1; //UINT64_MAX
    struct proc *selected_p = 0;

    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE && is_eligible(p)) {
        if (selected_p == 0 || p->vdeadline < min_vdeadline) {
          if (selected_p) { // 기존 min_vdeadline보다 현재 프로세스의 vdeadline이 더 작을 경우
            release(&selected_p->lock); // 이전 후보 프로세스의 lock 해제
          }
          selected_p = p;
          min_vdeadline = p->vdeadline;
          continue;
        }
      }
      release(&p->lock);
    }
    
    if (selected_p) {
      rq_remove(selected_p); // run queue에서 제거
      selected_p->state = RUNNING;
      selected_p->timeslice = DEFAULT_TIME_SLICE;
      c->proc = selected_p;

      swtch(&c->context, &selected_p->context);
      c->proc = 0;
      release(&selected_p->lock);
    }
    else {
      intr_on();
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  p->vdeadline = p->vruntime + (weight[DEFAULT_NICE] * DEFAULT_TIME_SLICE) / p->weight;
  rq_add(p); // run queue에 다시 추가
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  rq_remove(p); // run queue에서 제거
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        p->timeslice = DEFAULT_TIME_SLICE;
        p->vdeadline = p->vruntime + (weight[DEFAULT_NICE] * DEFAULT_TIME_SLICE) / p->weight;
        rq_add(p); // run queue에 추가
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int
getnice(int pid)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED) {
      int nice = p->nice;
      release(&p->lock);
      return nice;
    }
    release(&p->lock);
  }
  
  return -1; // pid와 일치하는 프로세스가 없으면 -1 반환
}

int
setnice(int pid, int value)
{
  if (value < 0 || value > 39) {
    return -1;
  }

  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED) {
      p->nice = value;
      p->weight = weight[value]; // nice 값에 따라 weight 업데이트
      p->vdeadline = p->vruntime + (weight[DEFAULT_NICE] * DEFAULT_TIME_SLICE) / p->weight; // vdeadline 업데이트
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }

  return -1; // pid와 일치하는 프로세스가 없으면 -1 반환
}

static void
pad_str(const char *str, int width)
{
  int len = strlen(str);
  printf("%s", str);
  for (int i = len; i < width; i++) {
    printf(" ");
  }
}

static void
pad_int(uint64 num, int width)
{
  printf("%lu", num);
  int len = 0;
  uint64 temp = num;
  do {
    len++;
    temp /= 10;
  } while(temp > 0);
  for (int i = len; i < width; i++) {
    printf(" ");
  }
}

void
ps(int pid)
{
  struct proc *p;
  char *state;

  if (pid == 0) { // 모든 프로세스의 정보 출력
    pad_str("name", 12);
    pad_str("pid", 6);
    pad_str("state", 12);
    pad_str("priority", 12);
    pad_str("runtime/weight", 16);
    pad_str("runtime", 12);
    pad_str("vruntime", 12);
    pad_str("vdeadline", 12);
    pad_str("is_eligible", 16);
    printf("tick ");
    uint64 total_tick = ticks * 1000ULL;
    printf("%lu\n", total_tick);
    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->state != UNUSED) {
        switch (p->state) { // case 0: UNUSED 일 경우는 없으니 제외
          case 1: state = "USED"; break;
          case 2: state = "SLEEPING"; break;
          case 3: state = "RUNNABLE"; break;
          case 4: state = "RUNNING"; break;
          case 5: state = "ZOMBIE"; break;
          default: state = "???"; break;
        }
        uint64 runtime = p->runtime * 1000ULL;
        uint64 runperwei = runtime / p->weight;
        uint64 vruntime = p->vruntime * 1000ULL;
        uint64 vdeadline = p->vdeadline * 1000ULL;
        char *eligibility = is_eligible(p) ? "true" : "false";

        pad_str(p->name, 12);
        pad_int(p->pid, 6);
        pad_str(state, 12);
        pad_int(p->nice, 12);
        pad_int(runperwei, 16);
        pad_int(runtime, 12);
        pad_int(vruntime, 12);
        pad_int(vdeadline, 12);
        pad_str(eligibility, 16);
        printf("\n");
      }
    }
  }

  else { // 해당하는 프로세스의 정보만 출력하거나 없으면 아무것도 출력하지 않음
    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->pid == pid && p->state != UNUSED) {
        pad_str("name", 12);
        pad_str("pid", 6);
        pad_str("state", 12);
        pad_str("priority", 12);
        pad_str("runtime/weight", 16);
        pad_str("runtime", 12);
        pad_str("vruntime", 12);
        pad_str("vdeadline", 12);
        pad_str("is_eligible", 16);
        printf("tick ");
        uint64 total_tick = ticks * 1000ULL;
        printf("%lu\n", total_tick);
        switch (p->state) {
          case 1: state = "USED"; break;
          case 2: state = "SLEEPING"; break;
          case 3: state = "RUNNABLE"; break;
          case 4: state = "RUNNING"; break;
          case 5: state = "ZOMBIE"; break;
          default: state = "???"; break;
        }
        uint64 runtime = p->runtime * 1000ULL;
        uint64 runperwei = runtime / p->weight;
        uint64 vruntime = p->vruntime * 1000ULL;
        uint64 vdeadline = p->vdeadline * 1000ULL;
        char *eligibility = is_eligible(p) ? "true" : "false";

        pad_str(p->name, 12);
        pad_int(p->pid, 6);
        pad_str(state, 12);
        pad_int(p->nice, 12);
        pad_int(runperwei, 16);
        pad_int(runtime, 12);
        pad_int(vruntime, 12);
        pad_int(vdeadline, 12);
        pad_str(eligibility, 16);
        printf("\n");
      }
    }
  }
}

uint64
meminfo(void)
{
  uint64 freemem = getfreemem();
  printf("available memory: %lu bytes\n", freemem);
  return freemem;
}

int
waitpid(int pid)
{
  struct proc *np; // 자식 프로세스
  struct proc *p = myproc(); // 부모 프로세스
  acquire(&wait_lock);
  
  for (;;) {
    int found = 0;
    for (np = proc; np < &proc[NPROC]; np++) {
      if (np->pid != pid) { // pid가 일치하지 않으면 다음 반복으로 넘어감
        continue;
      } 
      
      if (np->parent != p) {  // 부모가 아닌 경우
        release(&wait_lock);
        return -1;
      }

      found = 1; // 자식 프로세스 찾음
      if (np->state == ZOMBIE) {  // 자식이 종료된 경우
        freeproc(np);
        release(&wait_lock);
        return 0;
      }
    }

    if (!found) {  // 프로세스를 찾지 못한 경우
      release(&wait_lock);
      return -1;
    }

    sleep(p, &wait_lock); // 자식이 존재하고 아직 종료되지 않은 경우 대기
  }
}

void
getpname(int pid)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED) {
      printf("%s\n", p->name);  // 프로세스 이름 출력
      release(&p->lock);
      return;
    }
    release(&p->lock);
  }
}