#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct
{
	struct spinlock lock;
	struct proc proc[NPROC];
} ptable;

struct proc *queue[5][NPROC];
int queue_end[5] = {-1, -1, -1, -1, -1};
int timeslice[5] = {1, 2, 4, 8, 16};
char a[] = "\t";
int push(struct proc *p, int queue_num);
int rem(struct proc *p, int queue_num);

void change_queue_flag(struct proc *p)
{
	p->change_queue = 1;
	p->w_time = 0;
}

void func()
{

	acquire(&ptable.lock);

	struct proc *p;

	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		if (p->state == RUNNING)
		{
			p->rtime++;

#ifdef MLFQ

			calc_ticks(p);

#endif
		}
	}
	release(&ptable.lock);
}

void calc_ticks(struct proc *p)
{
	p->curr_ticks++;
	p->ticks[p->queue]++;
}

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
	initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int cpuid()
{
	return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
	int apicid, i;

	if (readeflags() & FL_IF)
		panic("mycpu called with interrupts enabled\n");

	apicid = lapicid();
	// APIC IDs are not guaranteed to be contiguous. Maybe we should have
	// a reverse map, or reserve a register to store &cpus[i].
	for (i = 0; i < ncpu; ++i)
	{
		if (cpus[i].apicid == apicid)
			return &cpus[i];
	}
	panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
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
static struct proc *
allocproc(void)
{
	struct proc *p;
	char *sp;
	//cprintf("ayyeee");

	acquire(&ptable.lock);

	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		if (p->state == UNUSED)
			goto found;

	release(&ptable.lock);
	return 0;

found:
	// cprintf("ayyeee");
	p->state = EMBRYO;
	p->pid = nextpid++;

	release(&ptable.lock);

	// Allocate kernel stack.
	if ((p->kstack = kalloc()) == 0)
	{
		p->state = UNUSED;
		return 0;
	}
	sp = p->kstack + KSTACKSIZE;

	// Leave room for trap frame.
	sp -= sizeof *p->tf;
	p->tf = (struct trapframe *)sp;

	// Set up new context to start executing at forkret,
	// which returns to trapret.
	sp -= 4;
	*(uint *)sp = (uint)trapret;

	sp -= sizeof *p->context;
	p->context = (struct context *)sp;
	memset(p->context, 0, sizeof *p->context);
	p->context->eip = (uint)forkret;

	// Initializes variables for waitx
	// acquire(&tickslock);
	p->ctime = ticks; // lock so that value of ticks doesnt change
	// release(&tickslock);
	p->etime = 0;
	p->rtime = 0;
	p->w_time = 0;
	p->wtime = 0;
	p->num_run = 0;
	p->priority = 60; // default
#ifdef MLFQ
	p->curr_ticks = 0;
	p->queue = 0;
	p->ticks[0] = 0;
	p->ticks[1] = 0;
	p->ticks[2] = 0;
	p->ticks[3] = 0;
	p->ticks[4] = 0;

#endif
	return p;
}

//PAGEBREAK: 32
// Set up first user process.
void userinit(void)
{
	struct proc *p;
	extern char _binary_initcode_start[], _binary_initcode_size[];

	p = allocproc();

	initproc = p;
	if ((p->pgdir = setupkvm()) == 0)
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
	p->tf->eip = 0; // beginning of initcode.S

	safestrcpy(p->name, "initcode", sizeof(p->name));
	p->cwd = namei("/");

	// this assignment to p->state lets other cores
	// run this process. the acquire forces the above
	// writes to be visible, and the lock is also needed
	// because the assignment might not be atomic.
	acquire(&ptable.lock);

	p->state = RUNNABLE;
#ifdef MLFQ
	push(p, 0);

#endif

	release(&ptable.lock);
	// cprintf("yeger");
}

void calc_wait()
{
	for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		acquire(&ptable.lock);
		if (p->pid != 0)
		{
			if (p->state == RUNNABLE)
			{
				p->wtime++;
				p->w_time++;
			}
#ifdef MLFQ
			if (p->w_time > AGE)
			{
				int k = p->queue;
				rem(p, k);
				push(p, k - 1);
				p->w_time = 0;
			}
#endif
		}
		release(&ptable.lock);
	}
}
// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
	uint sz;
	struct proc *curproc = myproc();

	sz = curproc->sz;
	if (n > 0)
	{
		if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
			return -1;
	}
	else if (n < 0)
	{
		if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
			return -1;
	}
	curproc->sz = sz;
	switchuvm(curproc);
	return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
	int i, pid;
	struct proc *np;
	struct proc *curproc = myproc();
	//cprintf("uyetwuer");

	// Allocate process.
	if ((np = allocproc()) == 0)
	{
		return -1;
	}

	// Copy process state from proc.
	if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
	{
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

	for (i = 0; i < NOFILE; i++)
		if (curproc->ofile[i])
			np->ofile[i] = filedup(curproc->ofile[i]);
	np->cwd = idup(curproc->cwd);

	safestrcpy(np->name, curproc->name, sizeof(curproc->name));

	pid = np->pid;

	acquire(&ptable.lock);

	np->state = RUNNABLE;

#ifdef MLFQ

	push(np, 0);
#endif

	release(&ptable.lock);

	return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
	struct proc *curproc = myproc();
	struct proc *p;
	int fd;

	if (curproc == initproc)
		panic("init exiting");

	// Close all open files.
	for (fd = 0; fd < NOFILE; fd++)
	{
		if (curproc->ofile[fd])
		{
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
	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		if (p->parent == curproc)
		{
			p->parent = initproc;
			if (p->state == ZOMBIE)
				wakeup1(initproc);
		}
	}

	// Jump into the scheduler, never to return.
	curproc->state = ZOMBIE;

	// acquire(&tickslock);
	curproc->etime = ticks; // protect with a lock so val of ticks doesn't change
	// release(&tickslock);
	cprintf("Total Time: %d\n", curproc->etime - curproc->ctime);
	sched();
	panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
	struct proc *p;
	int havekids, pid;
	struct proc *curproc = myproc();

	acquire(&ptable.lock);
	for (;;)
	{
		// Scan through table looking for exited children.
		havekids = 0;
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if (p->parent != curproc)
				continue;
			havekids = 1;
			if (p->state == ZOMBIE)
			{
				// Found one.
				pid = p->pid;
				kfree(p->kstack);
				p->kstack = 0;
				freevm(p->pgdir);
#ifdef MLFQ
				rem(p, p->queue);
#endif
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
		if (!havekids || curproc->killed)
		{
			release(&ptable.lock);
			return -1;
		}

		// Wait for children to exit.  (See wakeup1 call in proc_exit.)
		sleep(curproc, &ptable.lock); //DOC: wait-sleep
	}
}

int waitx(int *wtime, int *rtime)
{
	struct proc *p;
	int havekids, pid;
	struct proc *curproc = myproc();

	acquire(&ptable.lock);
	for (;;)
	{
		// Scan through table looking for exited children.
		havekids = 0;
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if (p->parent != curproc)
				continue;
			havekids = 1;
			if (p->state == ZOMBIE)
			{
				// Found one.
				*rtime = p->rtime;
				*wtime = p->wtime;
				pid = p->pid;
				kfree(p->kstack);
				p->kstack = 0;
				freevm(p->pgdir);
#ifdef MLFQ
				rem(p, p->queue);
#endif
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
		if (!havekids || curproc->killed)
		{
			release(&ptable.lock);
			return -1;
		}

		// Wait for children to exit.  (See wakeup1 call in proc_exit.)
		sleep(curproc, &ptable.lock); //DOC: wait-sleep
	}
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void)
{
	struct cpu *c = mycpu();
	c->proc = 0;

#if defined(DEFAULT) || defined(RR)
	for (;;)
	{
		// Enable interrupts on this processor.
		sti();
		// Loop over process table looking for process to run.
		acquire(&ptable.lock);

		for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if (p->state != RUNNABLE)
				continue;
			// Switch to chosen process.  It is the process's job
			// to release ptable.lock and then reacquire it
			// before jumping back to us.
			else if (p->state == RUNNABLE)
			{
				c->proc = p;
				switchuvm(p);
				p->num_run++;
				p->state = RUNNING;
				p->w_time = 0;

				swtch(&(c->scheduler), p->context);
				switchkvm();

				// Process is done running for now.
				// It should have changed its p->state before coming back.
				c->proc = 0;
			}
		}
		release(&ptable.lock);
	}
#endif

#ifdef FCFS
	for (;;)
	{
		struct proc *alottedP = 0;

		// Enable interrupts on this processor.
		sti();
		// Loop over process table looking for process to run.
		acquire(&ptable.lock);
		struct proc *minctimeProc = 0;
		for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if (p->state == RUNNABLE)
			{
				if (minctimeProc)
				{
					if (p->ctime < minctimeProc->ctime)
						minctimeProc = p;
				}
				else
				{
					minctimeProc = p;
				}
			}
			else
			{
				continue;
			}
		}
		if (minctimeProc != 0 && minctimeProc->state == RUNNABLE)
		{
			alottedP = minctimeProc;
			c->proc = alottedP;
			switchuvm(alottedP);
			alottedP->num_run++;
			alottedP->state = RUNNING;
			alottedP->w_time = 0;

			swtch(&(c->scheduler), alottedP->context);
			switchkvm();

			// Process is done running for now.
			// It should have changed its p->state before coming back.
			c->proc = 0;
		}
		release(&ptable.lock);
	}
#endif

#ifdef PBS
	for (;;)
	{
		struct proc *alottedP = 0;

		// Enable interrupts on this processor.
		sti();
		// Loop over process table looking for process to run.
		acquire(&ptable.lock);
		int minPrio = 101;
		for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if (p->state == RUNNABLE && p->priority <= (minPrio - 1))
			{
				minPrio = p->priority;
			}
			else
			{
				continue;
			}
		}
		struct proc *p, *p2;
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if (minPrio == 101)
			{
				break;
			}
			if (p->state != RUNNABLE)
			{
				continue;
			}
			else if (p->state == RUNNABLE)
			{
				if (p->priority == minPrio)
				{
					alottedP = p;
					// Switch to chosen process.  It is the process's job
					// to release ptable.lock and then reacquire it
					// before jumping back to us.
					c->proc = alottedP;

					switchuvm(alottedP);
					alottedP->num_run++;
					alottedP->state = RUNNING;
					p->w_time = 0;

					// cprintf("[PBSCHEDULER] pid %d on cpu %d (prio %d)\n",
					//         alottedP->pid, c->apicid, alottedP->priority);
					swtch(&(c->scheduler), alottedP->context);

					switchkvm();
					// Processis done running for now.
					// It should have changed its p->state before coming back.
					// else it has been interrupted and so we will exit this for loop as scheduler has called
					// it again and the next process will be scheduled accordingly
					c->proc = 0;

					int minPrio2 = 101;
					for (p2 = ptable.proc; p2 < &ptable.proc[NPROC]; p2++)
					{
						if (p2->state == RUNNABLE && p2->priority <= (minPrio2 - 1))
						{
							minPrio2 = p2->priority;
						}
						else
						{
							continue;
						}
					}

					if (minPrio2 <= (minPrio - 1))
					{
						break;
					}
				}
			}
		}
		release(&ptable.lock);
	}
#endif

#ifdef MLFQ
	for (;;)
	{
		// struct proc *alottedP = 0;

		// Enable interrupts on this processor.
		sti();
		// Loop over process table looking for process to run.
		acquire(&ptable.lock);
		struct proc *p = 0;

		// int oof = 0;
		for (int i = 0; i <= 4; i++)
		{
			if (queue_end[i] > -1)
			{
				p = queue[i][0];
				rem(p, i);
				break;
			}
			else
			{
				continue;
			}
		}

		if (p != 0 && p->state == RUNNABLE)
		{
			p->num_run++;
			c->proc = p;

			switchuvm(p);
			p->state = RUNNING;
			p->w_time = 0;
			swtch(&c->scheduler, p->context);
			switchkvm();
			c->proc = 0;

			if (p != 0 && p->state == RUNNABLE)
			{
				if (p->change_queue != 0)
				{
					p->change_queue = 0;
					if (p->queue <= 3)
					{
						p->w_time = 0;
						p->queue++;
					}
					p->curr_ticks = 0;
				}
				else if (p->change_queue == 0)
				{
					p->curr_ticks = 0;
				}
				push(p, p->queue);
			}
		}
		release(&ptable.lock);
	}
#endif
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
	int intena;
	struct proc *p = myproc();

	if (!holding(&ptable.lock))
		panic("sched ptable.lock");
	if (mycpu()->ncli != 1)
		panic("sched locks");
	if (p->state == RUNNING)
		panic("sched running");
	if (readeflags() & FL_IF)
		panic("sched interruptible");
	intena = mycpu()->intena;
	swtch(&p->context, mycpu()->scheduler);
	mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
	acquire(&ptable.lock); //DOC: yieldlock
	myproc()->state = RUNNABLE;

	sched();
	release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
	static int first = 1;
	// Still holding ptable.lock from scheduler.
	release(&ptable.lock);

	if (first)
	{
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
void sleep(void *chan, struct spinlock *lk)
{
	struct proc *p = myproc();

	if (p == 0)
		panic("sleep");

	if (lk == 0)
		panic("sleep without lk");

	// Must acquire ptable.lock in order to
	// change p->state and then call sched.
	// Once we hold ptable.lock, we can be
	// guaranteed that we won't miss any wakeup
	// (wakeup runs with ptable.lock locked),
	// so it's okay to release lk.
	if (lk != &ptable.lock)
	{						   //DOC: sleeplock0
		acquire(&ptable.lock); //DOC: sleeplock1
		release(lk);
	}
	// Go to sleep.
	p->chan = chan;
	p->state = SLEEPING;

	sched();

	// Tidy up.
	p->chan = 0;

	// Reacquire original lock.
	if (lk != &ptable.lock)
	{ //DOC: sleeplock2
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

	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		if (p->state == SLEEPING && p->chan == chan)
		{
			p->state = RUNNABLE;
#ifdef MLFQ
			p->curr_ticks = 0;
			push(p, p->queue);
#endif
		}
	}
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
	acquire(&ptable.lock);
	wakeup1(chan);
	release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
	struct proc *p;

	acquire(&ptable.lock);
	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		if (p->pid == pid)
		{
			p->killed = 1;
			// Wake process from sleep if necessary.
			if (p->state == SLEEPING)
			{
				p->state = RUNNABLE;
#ifdef MLFQ
				push(p, p->queue);
#endif
			}
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
void procdump(void)
{
	static char *states[] = {
		[UNUSED] "unused",
		[EMBRYO] "embryo",
		[SLEEPING] "sleep ",
		[RUNNABLE] "runble",
		[RUNNING] "run   ",
		[ZOMBIE] "zombie"};
	int i;
	struct proc *p;
	char *state;
	uint pc[10];

	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		if (p->state == UNUSED)
			continue;
		if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
			state = states[p->state];
		else
			state = "???";
		cprintf("%d %s %s", p->pid, state, p->name);
		if (p->state == SLEEPING)
		{
			getcallerpcs((uint *)p->context->ebp + 2, pc);
			for (i = 0; i < 10 && pc[i] != 0; i++)
				cprintf(" %p", pc[i]);
		}
		cprintf("\n");
	}
}

int set_priority(int pid, int priority)
{
	struct proc *p;
	int intrpt = 0;
	int prevp = 0;
	if (priority > 100)
		return 1;
	else
	{
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			if (p->pid == pid)
			{
				intrpt = 0;
				acquire(&ptable.lock);
				prevp = p->priority;
				p->priority = priority;
				if (prevp > p->priority)
				{
					intrpt = 1;
				}
				else
				{
					intrpt = 0;
				}
				release(&ptable.lock);
				break;
			}
		}
		if (intrpt == 0)
		{
			return prevp;
		}
		else if (intrpt == 1)
		{
			yield();
			return prevp;
		}
	}
	return prevp;
}

int print_pinfo()
{
	struct proc *p;
	int ret = -1;
	char *states[] = {
		[UNUSED] "unused",
		[EMBRYO] "embryo",
		[SLEEPING] "sleeping",
		[RUNNABLE] "runnable",
		[RUNNING] "running\t",
		[ZOMBIE] "zombie"};
	cprintf("PID\tPriority\tState\t\tr_time\tw_time\tn_run\tcur_q\tq0\tq1\tq2\tq3\tq4\n\n");

	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
	{
		if (p->pid != 0)
		{
			cprintf("%d%s%d%s\t%s%s%d%s%d%s%d%s%d%s%d%s%d%s%d%s%d%s%d%s\n", p->pid, a, p->priority, a, states[p->state], a, p->rtime, a, p->w_time, a, p->num_run, a, p->queue, a, p->ticks[0], a, p->ticks[1], a, p->ticks[2], a, p->ticks[3], a, p->ticks[4]);
		}
	}
	ret = 1;
	return ret;
}

int push(struct proc *p, int queue_num)
{
	int f = 0;
	for (int i = 0; i < queue_end[queue_num]; i++)
	{
		if (p->pid == queue[queue_num][i]->pid)
		{
			f = 1;
		}
	}
	if (f == 1)
	{
		return -1;
	}
	else
	{
		p->queue = queue_num;
		queue_end[queue_num]++;
		queue[queue_num][queue_end[queue_num]] = p;
		p->w_time = 0;
		return 1;
	}
}

int rem(struct proc *p, int queue_num)
{
	int f = 0;
	int k = -1;
	for (int i = 0; i <= queue_end[queue_num]; i++)
	{
		if (queue[queue_num][i]->pid == p->pid)
		{
			k = i;
			break;
		}
	}
	if (k >= 0)
	{
		f = 1;
	}
	else
	{
		f = 0;
	}

	if (f == 0)
	{
		return -1;
	}
	else if (f == 1)
	{
		for (int i = k; i < queue_end[queue_num]; i++)
		{
			queue[queue_num][i] = queue[queue_num][i + 1];
		}
		queue_end[queue_num]--;
		return 1;
	}
	return 1;
}