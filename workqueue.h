#ifndef _QNX_LINUX_WORKQUEUE_H
#define _QNX_LINUX_WORKQUEUE_H

#include <linux/wait.h>
#include <assert.h>

#define WORKQ_VALID     0x20122012
#define WORKQUEUE_WORKER_EXIT_TIMEOUT  4

struct workqueue_struct;
extern struct workqueue_struct *system_wq;
#define system_long_wq	system_wq
#define system_nrt_wq	system_wq

struct work_struct;
typedef struct work_struct {
	struct work_struct *next;
	void *data;
	void (*func)(struct work_struct *work);
	struct workqueue_struct * wq;
	int status;
} work_struct_t;

typedef struct workqueue_struct {
	pthread_t id;
	pthread_mutex_t     mutex;
	pthread_cond_t      delayed_cv;             /* wait for work */
	pthread_cond_t      cv;             /* wait for work */
	pthread_attr_t      attr;           /* create detached threads */
	work_struct_t      *first, *last;  /* work queue */
	int                 valid;          /* set when valid */
	int                 quit;           /* set when wq should quit */
#if WQ_MULTI_THREAD /*TODO add thread pool for wq */
	int                 parallelism;    /* number of threads required */
	int                 counter;        /* current number of threads */
	int                 idle;           /* number of idle threads */
#endif
	int                 flush;          /* force delayed work to run */
	char			    name[80];         /* I: workqueue name */
} workqueue_struct_t;

struct delayed_work {
	struct work_struct work;
	//struct timer_list timer;
	unsigned long delay; /* jiffies expired */
	void *data;
	void (*func)(struct work_struct *work);
	unsigned status;
};

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 *///FIXME: remove duplicate here. defined in kernel.h
#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

static inline struct delayed_work *
to_delayed_work(struct work_struct *work)
{
	return container_of(work, struct delayed_work, work);
}

workqueue_struct_t *init_workqueue (const char *name, int threads);
void destroy_workqueue (workqueue_struct_t *wq);
void flush_workqueue (workqueue_struct_t *wq);
void drain_workqueue (workqueue_struct_t *wq);
void queue_work (workqueue_struct_t *wq, work_struct_t *data);
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
						 struct delayed_work *dwork, unsigned long delay);

#define WORK_STATUS_IN_QUEUE  1

#define INIT_WORK(_work, _func)			\
	do {								\
		(_work)->data = _work;			\
		(_work)->func = (_func);		\
		(_work)->next = NULL;			\
	} while (0)

#define INIT_DELAYED_WORK(_work, _func)				\
	do {											\
		(_work)->data = _work;						\
		(_work)->func = (_func);					\
		(_work)->status = 0;						\
		(_work)->work.next = NULL;					\
	} while (0)

#define create_singlethread_workqueue(name)			\
	init_workqueue((name), 1)
#define alloc_workqueue(name,flags,threads) init_workqueue((name), threads)
#define alloc_ordered_workqueue(name,flags) init_workqueue((name), 1)

void init_wq_system(void);
void destroy_wq_system(void);

bool cancel_delayed_work_sync(struct delayed_work *dwork);
bool cancel_delayed_work(struct delayed_work *dwork);
bool cancel_work_sync(struct work_struct *dwork);

/**
 * mod_delayed_work - modify delay of or queue a delayed work
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * mod_delayed_work_on() on local CPU.
 */
static inline bool
mod_delayed_work(struct workqueue_struct *wq,
				 struct delayed_work *dwork,
				 unsigned long delay)
{
	return mod_delayed_work_on(0 /*WORK_CPU_UNBOUND*/, wq, dwork, delay);
}

bool queue_delayed_work(workqueue_struct_t *wq, struct delayed_work *dwork,
						unsigned long delay);

static inline void
schedule_work(struct work_struct *dwork)
{
	assert(system_wq);
	queue_work(system_wq,dwork);
}

static inline bool
schedule_delayed_work(struct delayed_work *dwork,unsigned long delay)
{
	assert(system_wq);
	return queue_delayed_work(system_wq, dwork, delay);
}

static inline void flush_scheduled_work(void)
{
	/* FIXME. TODO */
	assert(system_wq);
	flush_workqueue (system_wq);
}

bool flush_work(struct work_struct *work);
bool flush_delayed_work(struct delayed_work *dwork);


/* These are specified by iBCS2 */
#define POLLIN          0x0001
/* The rest seem to be more-or-less nonstandard. Check them! */
#define POLLRDNORM      0x0040

struct poll_table_struct;
struct file;
/*
 * structures and helpers for f_op->poll implementations
 */
typedef void (*poll_queue_proc)(struct file *f, wait_queue_head_t *wq, struct poll_table_struct *pt);

/*
 * Do not touch the structure directly, use the access functions
 * poll_does_not_wait() and poll_requested_events() instead.
 */
typedef struct poll_table_struct {
	poll_queue_proc _qproc;
	unsigned long _key;
} poll_table;

static inline void
poll_wait(struct file *filp, wait_queue_head_t *wait_address, poll_table *p)
{
	if (p && p->_qproc && wait_address)
		p->_qproc(filp, wait_address, p);
}

#endif /* _WQ_H */
