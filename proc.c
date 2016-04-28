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

void test_handler(int pid, int value);
int nextpid = 1;
extern void forkret(void);
extern void trapret(void);
void changeState(struct proc *p, int newState);
void changeStateFromTo(struct proc *p, int from, int to);
static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

int 
allocpid(void) 
{
  int oldPid = nextpid;
  while(!cas(&nextpid,oldPid,oldPid+1)){
    oldPid = nextpid;
  }
  return nextpid;
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

  p = ptable.proc;
  while  (!cas(&(p->state),UNUSED,EMBRYO))   {
   p = p+1;
   if (p == &ptable.proc[NPROC])
    return 0;
  
}

p->pid = allocpid();
p->signal = (void*)-1;  //new signal handler

//initialize cstack
struct cstackframe *csf;
for(csf = p->pending_signals.frames; csf < &p->pending_signals.frames[10]; csf++) {
  csf->used = 0;
}
p->pending_signals.head = 0;

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

  p->signal = (void*) -1; // init signal handler

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  changeState(p,RUNNABLE);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
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

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    cprintf("fork\n");
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;
  np->signal = np->parent->signal; //copy parent's signals

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
    np->cwd = idup(proc->cwd);

    safestrcpy(np->name, proc->name, sizeof(proc->name));

    pid = np->pid;  

  // lock to force the compiler to emit the np->state write last.
    pushcli();
    changeState(np,RUNNABLE);
    popcli();
    return pid;
  }

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
  void
  exit(void)
  {
    struct proc *p;
    int fd;

    if(proc == initproc)
      panic("init exiting");

  // Close all open files.
    for(fd = 0; fd < NOFILE; fd++){
      if(proc->ofile[fd]){
        fileclose(proc->ofile[fd]);
        proc->ofile[fd] = 0;
      }
    }

    begin_op();
    iput(proc->cwd);
    end_op();
    proc->cwd = 0;
    pushcli();
    changeState(proc,NEG_ZOMBIE);

  // Parent might be sleeping in wait().
    wakeup1(proc->parent);

  // Pass abandoned children to init.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent == proc){
        p->parent = initproc;
        if(p->state == ZOMBIE || p->state == NEG_ZOMBIE)
          wakeup1(initproc);
      }
    }

  // Jump into the scheduler, never to return.

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
    pushcli();
    for(;;){
      proc->chan = (int)proc;
      changeState(proc,NEG_SLEEPING);
    // Scan through table looking for zombie children.
      havekids = 0;
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->parent != proc)
          continue;
        havekids = 1;
        if(p->state == ZOMBIE){
        // Found one.
          pid = p->pid;
          p->state = UNUSED;
          p->pid = 0;
          p->parent = 0;
          p->name[0] = 0;

          proc->chan = 0;
          changeState(proc,RUNNING);
          popcli();
          return pid;
        }
      }
    // No point waiting if we don't have any children.
      if(!havekids || proc->killed){
        proc->chan = 0;
        popcli();
        changeState(proc,RUNNING);
        return -1;
      }
    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
      sched();
    }
  }

  void 
  freeproc(struct proc *p)
  {
    if (!p || p->state != NEG_ZOMBIE)
      panic("freeproc not zombie");
    kfree(p->kstack);
    p->kstack = 0;
    freevm(p->pgdir);
    p->killed = 0;
    p->chan = 0;
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
    for(;;){
    // Enable interrupts on this processor.
      sti();

    // Loop over process table looking for process to run.
      pushcli();
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(!cas(&(p->state),RUNNABLE,RUNNING))
          continue;
        proc = p;
        switchuvm(p);
        swtch(&cpu->scheduler, proc->context);
        // check if the process is at NEG_RUNNABLE or NEG_SLEEPING and change it's
        // status accordingly
        switchkvm();
        

      // Process is done running for now.
      // It should have changed its p->state before coming back.
        //cprintf("pid:%d state:%d\n",proc->pid,proc->state);
        if (p->state == NEG_ZOMBIE){
          //dont wakeup parent before finishing freeproc!!
          struct proc* parent = proc->parent;
          freeproc(proc);
          cas(&(p->state),NEG_ZOMBIE,ZOMBIE);
          wakeup1(parent);
        }
        cas(&(proc->state), NEG_RUNNABLE, RUNNABLE);
        cas(&(proc->state), NEG_SLEEPING, SLEEPING);
        proc = 0;
      }
      popcli();
    }
  }

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
  void
  sched(void)
  {
    int intena;
    if(cpu->ncli != 1)
      panic("sched locks");
    if(proc->state == RUNNING)
      panic("sched running");
    if(readeflags()&FL_IF)
      panic("sched interruptible");
    intena = cpu->intena;
    swtch(&proc->context, cpu->scheduler);
    cpu->intena = intena;
  }

// Give up the CPU for one scheduling round.
  void
  yield(void)
  {
    pushcli();
    changeState(proc,NEG_RUNNABLE);  
    sched();
    popcli();
  }

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
  void
  forkret(void)
  {
    static int first = 1;
  // Still holding ptable.lock from scheduler.
    popcli();
    if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot 
    // be run from main().
      first = 0;
      initlog();
    }

  // Return to "caller", actually trapret (see allocproc).
  }

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
  void
  sleep(void *chan, struct spinlock *lk)
  {
    if(proc == 0)
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
    pushcli();
    proc->chan = (int)chan;
    changeState(proc,NEG_SLEEPING);
    release(lk);
  }
  // Go to sleep.
  sched();

  // Reacquire original lock.
  if(lk != &ptable.lock){  
    popcli();
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
    if((p->state == SLEEPING || p->state == NEG_SLEEPING) && p->chan == (int)chan){
      // Tidy up.
      p->chan = 0;
      changeStateFromTo(p,SLEEPING,RUNNABLE);
    }

  }

// Wake up all processes sleeping on chan.
  void
  wakeup(void *chan)
  {
    pushcli();
    wakeup1(chan);
    popcli();
  }

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
  int
  kill(int pid)
  {
    struct proc *p;
    pushcli();
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->pid == pid){
        p->killed = 1;
      // Wake process from sleep if necessary.
        if(p->state == SLEEPING || p->state == NEG_SLEEPING)
          changeStateFromTo(p,SLEEPING,RUNNABLE);
        popcli();
        return 0;
      }
    }
    popcli();
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
      if(p->state == SLEEPING || p->state == NEG_SLEEPING){//NEG_SLEEPING?
        getcallerpcs((uint*)p->context->ebp+2, pc);
        for(i=0; i<10 && pc[i] != 0; i++)
          cprintf(" %p", pc[i]);
      }
      cprintf("\n");
    }
  }
  // update signal in proc and return old one
  sig_handler sigset(sig_handler sig){
    sig_handler oldsig = proc->signal;
    proc->signal = sig;
    return oldsig;
  }
//add a record to the recipient pending signals stack.
// return 0 on success and -1 on failure (if pending signals stack is full).
  int sigsend(int dest_pid, int value){
    struct proc *p;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->pid == dest_pid){  //found process
        if (push(&p->pending_signals, proc->pid, dest_pid, value)){ //succeeded push signal
          wakeup((void*)&(p->pending_signals)); 
          return 0;
        }
        else
          return -1;
      }
    }
    return -1;
  }
// restore the CPU registers values for the user space execution by restore old trapfram
  void sigret(void){
    proc->tf->eax = proc->oldtf.eax;
    proc->tf->edi = proc->oldtf.edi;
    proc->tf->esi = proc->oldtf.esi;
    proc->tf->ebp = proc->oldtf.ebp;
    proc->tf->oesp = proc->oldtf.oesp;
    proc->tf->ebx = proc->oldtf.ebx ;
    proc->tf->ecx = proc->oldtf.ecx ;
    proc->tf->gs = proc->oldtf.gs;
    proc->tf->padding1 =proc->oldtf.padding1 ;
    proc->tf->fs = proc->oldtf.fs ;
    proc->tf->padding2 = proc->oldtf.padding2;
    proc->tf->es =proc->oldtf.es ;
    proc->tf->padding3=proc->oldtf.padding3 ;
    proc->tf->ds=proc->oldtf.ds ;
    proc->tf->padding4 = proc->oldtf.padding4 ;
    proc->tf->trapno =proc->oldtf.trapno ;
    proc->tf->err =proc->oldtf.err ;
    proc->tf->eip = proc->oldtf.eip ;
    proc->tf->cs = proc->oldtf.cs ;
    proc->tf->padding5 = proc->oldtf.padding5 ;
    proc->tf->eflags = proc->oldtf.eflags;
    proc->tf->esp=proc->oldtf.esp ;
    proc->tf->ss=proc->oldtf.ss ;
    proc->tf->padding6=proc->oldtf.padding6 ;
  }
  //suspend the process until a new signal is received
  int sigpause(void){
    pushcli();
    while(isEmpty(&proc->pending_signals) && proc->killed == 0){ //no signals to handle go to sleep
      proc->chan = (int)(&(proc->pending_signals));  //sleep on my pending signals
      if(cas(&(proc->state), RUNNING, NEG_SLEEPING)){
        sched();
      }
    }
    popcli(); 
    return 0;
  }

/////CSTACK IMPLEMENTATION
// adds a new frame to the cstack which is initialized with values
// sender_pid, recepient_pid and value, then returns 1 on success and 0
// if the stack is full
  int 
  push(struct cstack *cstack, int sender_pid, int recepient_pid, int value){
    struct cstackframe *csf;
    
    for(csf = cstack->frames; csf < &cstack->frames[10]; csf++) {
      if(cas(&csf->used, 0, 1)) 
        goto found;
    }
  //stack is full
    return 0;

  //found an unused signal
    found:
  // copy values
    csf->sender_pid = sender_pid;
    csf->recepient_pid = recepient_pid;
    csf->value = value;
    do {
      csf->next = cstack->head;
    } while (!cas((int*)&(cstack->head), (int)csf->next, (int)csf));

    return 1;
  }

// removes and returns an element from the head of given cstack // if the stack is empty, then return 0
  struct cstackframe*
  pop(struct cstack *cstack){
    struct cstackframe *csf;
    do {
      csf = cstack->head;
      if (!csf)
        return 0;
    } while (!cas((int*)&(cstack->head), (int)csf, (int)csf->next));
    return csf;
  }
    //return 1 if empty 0 otherwise 
  int
  isEmpty(struct cstack *cstack){
    return !(cstack->head);
  }
/////END OF CSTACK

  void
  changeState(struct proc *p, int newState){
    while(!cas(&(p->state),p->state,newState));
  }

  void
  changeStateFromTo(struct proc *p, int from, int to){
    while(!cas(&(p->state),from,to)){}
  }

void backuptf(void){
  proc->oldtf.eax = proc->tf->eax;
  proc->oldtf.edi = proc->tf->edi;
  proc->oldtf.esi = proc->tf->esi;
  proc->oldtf.ebp = proc->tf->ebp;
  proc->oldtf.oesp = proc->tf->oesp;
  proc->oldtf.ebx = proc->tf->ebx;
  proc->oldtf.ecx = proc->tf->ecx;
  proc->oldtf.gs = proc->tf->gs;
  proc->oldtf.padding1 = proc->tf->padding1;
  proc->oldtf.fs = proc->tf->fs;
  proc->oldtf.padding2 = proc->tf->padding2;
  proc->oldtf.es = proc->tf->es;
  proc->oldtf.padding3 = proc->tf->padding3;
  proc->oldtf.ds = proc->tf->ds;
  proc->oldtf.padding4 = proc->tf->padding4;
  proc->oldtf.trapno = proc->tf->trapno;
  proc->oldtf.err = proc->tf->err;
  proc->oldtf.eip = proc->tf->eip;
  proc->oldtf.cs = proc->tf->cs;
  proc->oldtf.padding5 = proc->tf->padding5;
  proc->oldtf.eflags = proc->tf->eflags;
  proc->oldtf.esp = proc->tf->esp;
  proc->oldtf.ss = proc->tf->ss;
  proc->oldtf.padding6 = proc->tf->padding6;
}

// push args to user stack in order to use them later, plus pop from pending signals
// in order to use the signal we first need to backup all environment
void
usesignal(struct trapframe *tf){
  // make sure there is a signal handler and a pending signal and has been called from user to kernel
  if ( ( (tf->cs&3) == 3) && (proc != 0) && ((int)proc->signal != -1) && (proc->pending_signals.head != 0) && (proc->pending_signals.head->used != 0) ){
    struct cstackframe *signalhead = pop(&proc->pending_signals);
    backuptf();
    proc->tf->eip = (uint)(proc->signal);
        // the size we need to use in the stack in order to copy the relevant code
    int diffbytes = ((int)(&endsigret) -(int)(&startsigret));
        //push the code into the stack
    memmove((void*)(proc->tf->esp-diffbytes),&startsigret,diffbytes);
        //update esp to point to the new place
    proc->tf->esp =proc->tf->esp - diffbytes;
    int value = signalhead->value;
    int senderpid = signalhead->sender_pid;
    signalhead->used = 0;
        //push all args to stack
    memmove((void*)(proc->tf->esp-4),&value,4);
    memmove((void*)(proc->tf->esp-8),&senderpid,4);
    proc->tf->esp =proc->tf->esp - 8;
    int return_add = proc->tf->esp +8;
        //push the return address to the stack
    memmove((void*)(proc->tf->esp-4),&return_add,4);
    proc->tf->esp =proc->tf->esp - 4;   
  }
}
