#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

struct syscall_hist p_hist[SYS_CALL_NUM] = {0};

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;

  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  memset(&p->sched_info, 0, sizeof(p->sched_info));
  p->sched_info.queue = UNSET;
  p->sched_info.bjf.priority = BJF_PRIORITY_DEF;
  p->sched_info.bjf.priority_ratio = 1;
  p->sched_info.bjf.arrival_time_ratio = 1;
  p->sched_info.bjf.executed_cycle_ratio = 1;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
  change_queue(p->pid, UNSET);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  acquire(&tickslock);
  np->sched_info.last_run = ticks;
  np->sched_info.bjf.arrival_time = ticks;
  release(&tickslock);

  release(&ptable.lock);
  change_queue(np->pid, UNSET);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

void
ageprocs(int osTicks)
{
  struct proc *p;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE && p->sched_info.queue != ROUND_ROBIN)
    {
      if (osTicks - p->sched_info.last_run > AGING_THRESHOLD)
      {
        release(&ptable.lock);
        change_queue(p->pid, ROUND_ROBIN);
        acquire(&ptable.lock);
      }
    }
  }

  release(&ptable.lock);
}

struct proc*
roundrobin(struct proc *lastScheduled)
{
  struct proc *p = lastScheduled;
  for (;;)
  {
    p++;
    if (p >= &ptable.proc[NPROC])
      p = ptable.proc;

    if (p->state == RUNNABLE && p->sched_info.queue == ROUND_ROBIN)
      return p;

    if (p == lastScheduled)
      return 0;
  }
}

static float
bjfrank(struct proc* p)
{
  return p->sched_info.bjf.priority * p->sched_info.bjf.priority_ratio +
         p->sched_info.bjf.arrival_time * p->sched_info.bjf.arrival_time_ratio +
         p->sched_info.bjf.executed_cycle * p->sched_info.bjf.executed_cycle_ratio;
}

struct proc*
bestjobfirst(void)
{
  struct proc* result = 0;
  float minrank;

  struct proc* p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != RUNNABLE || p->sched_info.queue != BJF)
      continue;
    float rank = bjfrank(p);
    if(result == 0 || rank < minrank){
      result = p;
      minrank = rank;
    }
  }

  return result;
}

struct proc*
lottery()
{
  struct proc* result = 0;

  uint tickets_sum = 0;
  for (int i = 0; i < NPROC; ++i) {
    if ((ptable.proc[i].state == RUNNABLE) && (ptable.proc[i].sched_info.queue == LOTTERY)) {
      tickets_sum += ptable.proc[i].sched_info.tickets_count;
    }
  }
  if (tickets_sum == 0)
    return result;

  uint ticket = rand() % tickets_sum;

  uint prev_interval_begin = 0;
  for (int i = 0; i < NPROC; ++i) {
    if (ptable.proc[i].state != RUNNABLE)
      continue;
    if (ptable.proc[i].sched_info.queue != LOTTERY)
      continue;
    if (
      (ticket >= prev_interval_begin) &&
      (ticket <= prev_interval_begin + ptable.proc[i].sched_info.tickets_count)
    ) {
      result = &ptable.proc[i];
      break;
    }
    prev_interval_begin += ptable.proc[i].sched_info.tickets_count;
  }

  return result;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct proc *lastScheduledRR = &ptable.proc[NPROC - 1];
  struct cpu *c = mycpu();
  c->proc = 0;
  srand(ticks);

  for(;;){
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);

    p = roundrobin(lastScheduledRR);
    if(p){
      lastScheduledRR = p;
    }
    else{
      p = lottery();
      if(!p){
        p = bestjobfirst();
        if(!p){
          release(&ptable.lock);
          continue;
        }
      }
    }

    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;

    p->sched_info.last_run = ticks;
    p->sched_info.bjf.executed_cycle += 0.1f;

    swtch(&(c->scheduler), p->context);
    switchkvm();
    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

void
wakeupproc(struct proc* p)
{
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

void
push_p_hist(int pid, int syscall_number) {
  int cur_size = p_hist[syscall_number].size % PROC_HIST_SIZE;
  p_hist[syscall_number].pids[cur_size] = pid;
  ++(p_hist[syscall_number].size);
}

void
get_callers(int syscall_number) {
  int limit = p_hist[syscall_number].size;

  if(limit == 0){
    cprintf("No process has called system call number %d.\n", syscall_number);
    return;
  }

  int i = (limit > PROC_HIST_SIZE) ? limit % PROC_HIST_SIZE : 0;
  limit %= PROC_HIST_SIZE;
  while(1){
    cprintf("%d", p_hist[syscall_number].pids[i]);
    i = (i + 1) % PROC_HIST_SIZE;
    if(i == limit) break;
    cprintf(", ");
  }
  cprintf("\n");
}

int
change_queue(int pid, int new_queue) {
  struct proc *p;
  int old_queue = -1;

  if(new_queue == UNSET){
    if (pid == 1)
      new_queue = ROUND_ROBIN;
    else if (pid > 1)
      new_queue = LOTTERY;
    else
      return -1;
  }

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      old_queue = p->sched_info.queue;
      p->sched_info.queue = new_queue;
      if (new_queue == LOTTERY && p->sched_info.tickets_count <= 0) {
        p->sched_info.tickets_count = (rand() % MAX_RANDOM_TICKETS) + 1;
      }
      break;
    }
  }
  release(&ptable.lock);
  return old_queue;
}

int
set_lottery_ticket(int pid, int tickets)
{
  acquire(&ptable.lock);
  struct proc* p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid && p->sched_info.queue == LOTTERY){
      p->sched_info.tickets_count = tickets;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int
set_bjf_params_process(int pid, float priority_ratio, float arrival_time_ratio, float executed_cycles_ratio)
{
  acquire(&ptable.lock);
  struct proc* p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->sched_info.bjf.priority_ratio = priority_ratio;
      p->sched_info.bjf.arrival_time_ratio = arrival_time_ratio;
      p->sched_info.bjf.executed_cycle_ratio = executed_cycles_ratio;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

void
set_bjf_params_system(float priority_ratio, float arrival_time_ratio, float executed_cycles_ratio)
{
  acquire(&ptable.lock);
  struct proc* p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    p->sched_info.bjf.priority_ratio = priority_ratio;
    p->sched_info.bjf.arrival_time_ratio = arrival_time_ratio;
    p->sched_info.bjf.executed_cycle_ratio = executed_cycles_ratio;
  }
  release(&ptable.lock);
}

int
set_bjf_priority(int pid, int priority)
{
  acquire(&ptable.lock);
  struct proc* p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->sched_info.bjf.priority = priority;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

void
print_process_info()
{
 static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleeping",
  [RUNNABLE]  "runnable",
  [RUNNING]   "running",
  [ZOMBIE]    "zombie"
  };

  static int columns[] = {16, 8, 9, 8, 8, 8, 8, 9, 8, 8, 8, 8};
  cprintf("Process_Name    PID     State    Queue   Cycle   Arrival Ticket  Priority R_Prty  R_Arvl  R_Exec  Rank\n"
          "------------------------------------------------------------------------------------------------------\n");

  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;

    const char* state;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    cprintf("%s", p->name);
    printspaces(columns[0] - strlen(p->name));

    cprintf("%d", p->pid);
    printspaces(columns[1] - digitcount(p->pid));

    cprintf("%s", state);
    printspaces(columns[2] - strlen(state));

    cprintf("%d", p->sched_info.queue);
    printspaces(columns[3] - digitcount(p->sched_info.queue));

    cprintf("%d", (int)p->sched_info.bjf.executed_cycle);
    printspaces(columns[4] - digitcount((int)p->sched_info.bjf.executed_cycle));

    cprintf("%d", p->sched_info.bjf.arrival_time);
    printspaces(columns[5] - digitcount(p->sched_info.bjf.arrival_time));

    cprintf("%d", p->sched_info.tickets_count);
    printspaces(columns[6] - digitcount(p->sched_info.tickets_count));

    cprintf("%d", p->sched_info.bjf.priority);
    printspaces(columns[7] - digitcount(p->sched_info.bjf.priority));

    cprintf("%d", (int)p->sched_info.bjf.priority_ratio);
    printspaces(columns[8] - digitcount((int)p->sched_info.bjf.priority_ratio));

    cprintf("%d", (int)p->sched_info.bjf.arrival_time_ratio);
    printspaces(columns[9] - digitcount((int)p->sched_info.bjf.arrival_time_ratio));

    cprintf("%d", (int)p->sched_info.bjf.executed_cycle_ratio);
    printspaces(columns[10] - digitcount((int)p->sched_info.bjf.executed_cycle_ratio));

    cprintf("%d", (int)bjfrank(p));
    cprintf("\n");
  }
}
