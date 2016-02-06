#include <linux/qnx.h>
#include <linux/linux.h>

enum DELAYED_WORK_STATUS {
	DW_NOT_IN_QUEUE = 0,
	DW_IN_QUEUE,
	DW_WAITING_TIMEOUT,
	DW_RUNNING,
	DW_FINISHED,
	DW_CANCELING,
	DW_RUN_NOW,
};

/*
 * System-wide workqueues which are always present.
 *
 * system_wq is the one used by schedule[_delayed]_work[_on]().
 * Multi-CPU multi-threaded.  There are users which expect relatively
 * short queue flush time.  Don't queue works which can run for too
 * long.
 *
 * system_long_wq is similar to system_wq but may host long running
 * works.  Queue flushing might take relatively long.
 *
 * system_nrt_wq is non-reentrant and guarantees that any given work
 * item is never executed in parallel by multiple CPUs.  Queue
 * flushing might take relatively long.
 *
 * system_unbound_wq is unbound workqueue.  Workers are not bound to
 * any specific CPU, not concurrency managed, and all queued works are
 * executed immediately as long as max_active limit is not reached and
 * resources are available.
 *
 * system_freezable_wq is equivalent to system_wq except that it's
 * freezable.
 *
 * system_nrt_freezable_wq is equivalent to system_nrt_wq except that
 * it's freezable.
 */

struct workqueue_struct *system_wq = NULL;
//struct workqueue_struct *system_long_wq = NULL;
//struct workqueue_struct *system_nrt_wq = NULL;
//struct workqueue_struct *system_unbound_wq;
//struct workqueue_struct *system_freezable_wq;
//struct workqueue_struct *system_nrt_freezable_wq;

static void *workqueue_thread (void *arg);

void init_wq_system(void) {
	system_wq = create_singlethread_workqueue("system wq");
	if(system_wq == NULL)
		slogf( _SLOG_SETCODE(_SLOGC_3RDPARTY_OEM00001_START, 911),
			   _SLOG_ERROR,
			   "Creating System WQ failed");
}

void destroy_wq_system (void) {
	if(system_wq)
		destroy_workqueue(system_wq);
}

workqueue_struct_t *init_workqueue (const char *name, int threads)
{
	int status;
	pthread_condattr_t cond_attr;
	workqueue_struct_t *wq = NULL;

	wq = (workqueue_struct_t *)kzalloc ( sizeof (workqueue_struct_t),
										 GFP_KERNEL);
	if (wq == NULL){
		qnx_error("kzalloc failed.");
		return NULL;
	}
	status = pthread_attr_init (&wq->attr);
	if (status != 0)
		goto free_wq;
	status = pthread_attr_setdetachstate (&wq->attr, PTHREAD_CREATE_DETACHED);
	if (status != 0) {
		goto free_attr;
	}
	status = pthread_attr_setstacksize(&wq->attr, THREAD_STACK_SIZE);
	if (status != 0) {
		goto free_attr;
	}
	status = pthread_attr_setguardsize(&wq->attr, THREAD_STACK_GUARD_SIZE);
	if (status != 0) {
		goto free_attr;
	}
	status = pthread_mutex_init (&wq->mutex, NULL);
	if (status != 0) {
		goto free_attr;
	}
	status = pthread_condattr_init(&cond_attr);
	if (status != 0) {
		goto free_mutex;
	}
	status = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
	if(status != 0){
		goto free_cv_attr;
	}
	status = pthread_cond_init (&wq->cv, &cond_attr);
	if (status != 0) {
		goto free_cv_attr;
	}
	status = pthread_cond_init (&wq->delayed_cv, &cond_attr);
	if (status != 0) {
		goto free_cv;
	}

	wq->quit = 0;
	wq->first = NULL;
	wq->last = NULL;
	strcpy(wq->name, name);
	wq->valid = WORKQ_VALID;
	INIT_LIST_HEAD (&wq->flush_list);

	if ( 0 != pthread_create (&wq->id, &wq->attr, workqueue_thread,
							  (void*)wq)) {
		goto free_delayed_cv;
	}
	pthread_condattr_destroy(&cond_attr);
	return wq;
 free_delayed_cv:
	pthread_cond_destroy(&wq->delayed_cv);
 free_cv:
	pthread_cond_destroy(&wq->cv);
 free_cv_attr:
	pthread_condattr_destroy(&cond_attr);
 free_mutex:
	pthread_mutex_destroy (&wq->mutex);
 free_attr:
	pthread_attr_destroy(&wq->attr);
 free_wq:
	kfree(wq);
	return NULL;
}

void destroy_workqueue (workqueue_struct_t *wq)
{
	assert (wq->valid == WORKQ_VALID);
	/* notify thread to quit */
	pthread_mutex_lock (&wq->mutex);
	wq->quit = 1;
	pthread_cond_broadcast (&wq->cv);
	pthread_mutex_unlock (&wq->mutex);

	pthread_join(wq->id, NULL);

	/* destroy wq */
	pthread_mutex_destroy (&wq->mutex);
	pthread_cond_destroy (&wq->cv);
	pthread_attr_destroy (&wq->attr);
	pthread_cond_destroy(&wq->delayed_cv);
	pthread_cond_destroy(&wq->cv);
	pthread_mutex_destroy (&wq->mutex);
	pthread_attr_destroy(&wq->attr);
	kfree(wq);

	return;
}

static int
push_work (workqueue_struct_t *wq,
			 work_struct_t *item)
{
	assert(wq->valid == WORKQ_VALID);
	assert(item->func);
	item->status = 0;
	work_struct_t * w = wq->first;
	/* skip add it if item is already in queue */
	while(w){
		if(w == item){
			return 0;
		}
		w = w->next;
	}
	/* not in queue. add it */
	item->next = 0;
	if (wq->first == NULL){
		assert(wq->last == NULL);
		wq->first = item;
		wq->last = item;
	}else {
		assert(wq->last && wq->last->next == 0);
		wq->last->next = item;
		wq->last = item;
	}
	assert(wq->first && wq->last && wq->last->next == 0);
	assert( (wq->first && wq->last) || (!wq->first && !wq->last));
	return 0;
}

static work_struct_t *
pop_work(workqueue_struct_t *wq)
{
	work_struct_t * w = wq->first;
	if(w){
		if(wq->last == w){
			wq->last = NULL;
			assert(w->next == 0);
		}
		wq->first = w->next;
	}else {
		assert(wq->first == 0 && wq->last == 0);
	}
	assert( (wq->first && wq->last) || (!wq->first && !wq->last));
	return w;
}

static int
remove_work(workqueue_struct_t *wq, work_struct_t * wk)
{
	work_struct_t * w = wq->first;
	if(w == 0){
		/* wq already poped the wk.
		   but handler function is blocking in locking mutex */
		assert(wq->first == 0 && wq->last == 0);
		return 0;
	}
	if(w == wk){
		pop_work(wq);
		return 0;
	}
	assert(wq->first && wq->first != wk);

	/* find the one before wk in queue */
	while(w && w->next != wk){
		w = w->next;
	}
	if(!w){	/* wk is not in queue */
		assert(0);
		return -1;
	}
	/* remove wk from queue */
	w->next = wk->next;
	if(wq->last == wk){
		assert(wk->next == 0);
		wq->last = w;
	}
	assert( (wq->first && wq->last) || (!wq->first && !wq->last));
	return 0;
}

void queue_work (workqueue_struct_t *wq,
				 work_struct_t * w)
{
	pthread_mutex_lock (&wq->mutex);
	assert(w->func);
	push_work(wq, w);
	w->wq = wq;
	pthread_cond_broadcast(&wq->cv);
	pthread_mutex_unlock (&wq->mutex);

	return ;
}

/**
 * cancel_work_sync - cancel a work and wait for it to finish
 * @work: the work to cancel
 *
 * Cancel @work and wait for its execution to finish.  This function
 * can be used even if the work re-queues itself or migrates to
 * another workqueue.  On return from this function, @work is
 * guaranteed to be not pending or executing on any CPU.
 *
 * cancel_work_sync(&delayed_work->work) must not be used for
 * delayed_work's.  Use cancel_delayed_work_sync() instead.
 *
 * The caller must ensure that the workqueue on which @work was last
 * queued can't be destroyed before this function returns.
 *
 * Return:
 * %true if @work was pending, %false otherwise.
 */
bool cancel_work_sync(struct work_struct *work)
{
	int rc;
	workqueue_struct_t * wq = work->wq;
	if(!wq){
		return false;
	}
	pthread_mutex_lock (&wq->mutex);
	rc = remove_work(wq, work);
	pthread_mutex_unlock(&wq->mutex);
	if(work && rc == 0){
		/* valid work in workqueu */
		work->func (work->data);
		return true;
	}
	return false;
}



struct flush_data {
	workqueue_struct_t *wq;
	int done;
};

static void
flusher_func(struct work_struct *work)
{
	struct flush_data * data = (struct flush_data *)work;
	workqueue_struct_t *wq = data->wq;

	pthread_mutex_lock (&wq->mutex);
	data->done = 1;
	pthread_cond_broadcast (&wq->delayed_cv);
	pthread_mutex_unlock (&wq->mutex);
}

void flush_workqueue (workqueue_struct_t *wq)
{
	 struct flush_data data = {
		 .wq = wq,
		 .done = 0,
	 };
	 work_struct_t wk = {
		 .func = flusher_func,
		 .data = &data,
		 .next = 0,
	 };

	 queue_work(wq, &wk);

	 pthread_mutex_lock (&wq->mutex);
	 wq->flush = 1;
	 pthread_cond_broadcast (&wq->delayed_cv);
	 while (data.done == 0) {
		if (pthread_cond_wait (&wq->delayed_cv, &wq->mutex) != 0){
			//TODO.
		}
	 }
	 wq->flush = 0;
	 pthread_mutex_unlock (&wq->mutex);
}

static void
delayed_func(struct work_struct *work)
{
	struct timespec timeout;
	struct delayed_work * dw;
	dw = container_of(work,struct delayed_work, work);
	workqueue_struct_t *wq = work->wq;

	assert(dw && wq && dw->func);
	pthread_mutex_lock(&wq->mutex);

	if(dw->status == DW_CANCELING){
		/* someone is canceling this work */
		dw->status = DW_NOT_IN_QUEUE;
		pthread_cond_broadcast(&wq->delayed_cv);
		pthread_mutex_unlock(&wq->mutex);
		return ;
	}

	dw->status = DW_WAITING_TIMEOUT;

	while(wq->flush == 0){
		unsigned long cur = jiffies;
		unsigned long exp  = dw->delay;
		_uint64 exp_ns;
		if(dw->status == DW_CANCELING ||
		   dw->status == DW_RUN_NOW){
			/* someone is canceling the work */
			break;
		}
		if(exp < cur){ /* expired already */
			break;
		}
		/* wait a while */
		exp_ns = jiffies_to_nsec(exp);
		nsec2timespec(&timeout, exp_ns);
		pthread_cond_timedwait (&wq->delayed_cv, &wq->mutex, &timeout);
	}
	/* run it */
	if(dw->status == DW_WAITING_TIMEOUT ||
	   dw->status == DW_RUN_NOW){
		dw->status = DW_RUNNING; /* begin to run now */
		pthread_mutex_unlock(&wq->mutex);
		/*when work item is running, both wq's mutex && mutex are unlocked!*/
		dw->func(dw->data);
		/* become idle now */
		pthread_mutex_lock(&wq->mutex);
	}else {
		/* the work has been canceled by someone */
	}
	dw->status = DW_NOT_IN_QUEUE;
	pthread_cond_broadcast(&wq->delayed_cv);
	pthread_mutex_unlock(&wq->mutex);
}

/*TODO. FIXME. we should sort delayed works in workqueue */
static bool
queue_delayed_work_locked(workqueue_struct_t *wq,
						struct delayed_work *dw,
						unsigned long delay)
{
	if(dw->status == DW_NOT_IN_QUEUE){
		dw->status = DW_IN_QUEUE;
	} else {
		/* already in queue */
		return true;
	}
	assert(dw->data == dw);
	dw->delay = jiffies + delay;

	dw->work.wq = wq;
	dw->work.data = &dw->work;
	dw->work.func = delayed_func;
	push_work(wq, &dw->work);

	return true;
}

bool
queue_delayed_work(workqueue_struct_t *wq,
						struct delayed_work *dw,
						unsigned long delay)
{
	bool rc;
	pthread_mutex_lock (&wq->mutex);
	rc = queue_delayed_work_locked(wq, dw, delay);
	pthread_mutex_unlock (&wq->mutex);
	return rc;
}
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
						 struct delayed_work *dw,
						 unsigned long delay)
{
	bool rc = true;
	(void)cpu;

	pthread_mutex_lock(&wq->mutex);
	if(!wq->flush){
		switch(dw->status){
		case DW_NOT_IN_QUEUE: /* it's not in queue. add it */
			rc = queue_delayed_work_locked(wq, dw, delay);
			break;
		case DW_IN_QUEUE:   /* in queue, but not runing yet */
		case DW_WAITING_TIMEOUT: {
			/* in queue, not running, but waiting for timeout & ready to run*/
			/* it's safe to modify time */
			/* TODO. FIXME. sort dwork according to the new dealy */
			unsigned long expired = jiffies + delay;
			dw->delay = expired;
			pthread_cond_broadcast(&wq->delayed_cv);
			}
			break;
		case DW_RUNNING: /* the working is running, there no way to mod time */
			rc = false;
			break;
		default: /* that's impossible */
			assert(0);
			break;
		}
	}
	pthread_mutex_unlock(&wq->mutex);
	return rc;
}
/**
 * cancel_delayed_work -
 * cancel a delayed work and wait until work is done if sync
 * is true.
 * return true if delayed work was pending.
 */
static bool
cancel_dwork(struct delayed_work *dw, bool sync)
{
	bool rc = true;
	workqueue_struct_t *wq = dw->work.wq;
	if(!wq){
		return false;
	}
	pthread_mutex_lock(&wq->mutex);
	switch(dw->status){
	case DW_NOT_IN_QUEUE: /* not in queue */
		break;
	case DW_WAITING_TIMEOUT: /* in queue, waiting for timeout */
		/* notify thread this dw is marked as canceled */
		dw->status = DW_CANCELING;
		pthread_cond_broadcast(&wq->delayed_cv);
		while(dw->status != DW_NOT_IN_QUEUE){
			pthread_cond_wait(&wq->delayed_cv, &wq->mutex);
		}
		break;
	case DW_IN_QUEUE: /* in queue, not running */
		/* remove work item */
		remove_work(wq, &dw->work);
		dw->status = DW_NOT_IN_QUEUE;
		break;
	case DW_RUNNING: /* in queue & running */
		if(sync){
			pthread_cond_broadcast(&wq->delayed_cv);
			while(dw->status != DW_NOT_IN_QUEUE){
				pthread_cond_wait(&wq->delayed_cv, &wq->mutex);
			}
		}else {
			rc = false;
		}
		break;
	default:
		assert(0);
		break;
	}
	pthread_mutex_unlock(&wq->mutex);
	return rc;
}

/**
 * cancel_delayed_work - cancel a delayed work
 * @dwork: delayed_work to cancel
 *
 * Kill off a pending delayed_work.
 *
 * Return: %true if @dwork was pending and canceled; %false if it wasn't
 * pending.
 *
 * Note:
 * The work callback function may still be running on return, unless
 * it returns %true and the work doesn't re-arm itself.  Explicitly flush or
 * use cancel_delayed_work_sync() to wait on it.
 *
 * This function is safe to call from any context including IRQ handler.
 */
bool cancel_delayed_work(struct delayed_work *dw)
{
	return cancel_dwork(dw, false);
}
/**
 * cancel_delayed_work_sync - cancel a delayed work and wait for it to finish
 * @dwork: the delayed work cancel
 *
 * This is cancel_work_sync() for delayed works.
 *
 * Return:
 * %true if @dwork was pending, %false otherwise.
 */
bool cancel_delayed_work_sync(struct delayed_work *dw)
{
	return cancel_dwork(dw, true);
}


/**
 * flush_work - wait for a work to finish executing the last queueing instance
 * @work: the work to flush
 *
 * Wait until @work has finished execution.  @work is guaranteed to be idle
 * on return if it hasn't been requeued since flush started.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
bool flush_work(struct work_struct *work)
{
	struct workqueue_struct * wq = work->wq;
	pthread_mutex_lock(&wq->mutex);
	if(work->status != 1) {
		bool f = false;
		struct work_struct * w;
		list_for_each_entry(w, &wq->flush_list, list){
			if (work == w){
				f = true;
				break;
			}
		}
		if(!f){
			list_add(&wq->flush_list, &work->list);
		}
		while(work->status !=1){
			pthread_cond_wait(&wq->cv, &wq->mutex);
		}
		list_del(&work->list);
	}
	pthread_mutex_lock(&wq->mutex);
	return true;
}
/**
 * flush_delayed_work - wait for a dwork to finish executing the last queueing
 * @dwork: the delayed work to flush
 *
 * Delayed timer is cancelled and the pending work is queued for
 * immediate execution.  Like flush_work(), this function only
 * considers the last queueing instance of @dwork.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
bool flush_delayed_work(struct delayed_work *dw)
{
	bool rc = true, sync = true;
	workqueue_struct_t *wq = dw->work.wq;
	if(!wq){
		return false;
	}

	pthread_mutex_lock(&wq->mutex);
	switch(dw->status){
	case DW_NOT_IN_QUEUE: /* not in queue */
		break;
	case DW_WAITING_TIMEOUT: /* in queue, waiting for timeout */
		/* notify thread this dw is marked as canceled */
		dw->status = DW_RUN_NOW;
		pthread_cond_broadcast(&wq->delayed_cv);
		while(dw->status != DW_NOT_IN_QUEUE){
			pthread_cond_wait(&wq->delayed_cv, &wq->mutex);
		}
		break;
	case DW_IN_QUEUE: /* in queue, not running */
		/* remove work item */
		remove_work(wq, &dw->work);
		dw->status = DW_NOT_IN_QUEUE;
		pthread_mutex_unlock(&wq->mutex);
		/*when work item is running, both wq's mutex && mutex are unlocked!*/
		dw->func(dw->data);
		/* become idle now */
		pthread_mutex_lock(&wq->mutex);
		break;
	case DW_RUNNING: /* in queue & running */
		if(sync){
			pthread_cond_broadcast(&wq->delayed_cv);
			while(dw->status != DW_NOT_IN_QUEUE){
				pthread_cond_wait(&wq->delayed_cv, &wq->mutex);
			}
		}else {
			rc = false;
		}
		break;
	default:
		assert(0);
		break;
	}
	pthread_mutex_unlock(&wq->mutex);
	return rc;
	/* local_irq_disable(); */
	/* if (del_timer_sync(&dwork->timer)) */
	/* 	__queue_work(dwork->cpu, dwork->wq, &dwork->work); */
	/* local_irq_enable(); */
	/* return flush_work(&dwork->work); */
}


static void *workqueue_thread (void *arg)
{
	struct timespec timeout;
	workqueue_struct_t *wq = (workqueue_struct_t *)arg;
	work_struct_t *we = NULL;
	int status;

	if (wq->name[0]){
		pthread_setname_np(0,wq->name);
	}
	status = pthread_mutex_lock (&wq->mutex);
	if (status != 0)
		return NULL;

	while (1) {
		clock_gettime (CLOCK_MONOTONIC, &timeout);
		timeout.tv_sec += WORKQUEUE_WORKER_EXIT_TIMEOUT;

		while ((we = pop_work(wq)) == NULL && !wq->quit) {
			status = pthread_cond_timedwait (&wq->cv, &wq->mutex, &timeout);
			if (status == ETIMEDOUT) {
				break;
			} else if (status != 0) {
				assert(0);
				pthread_mutex_unlock (&wq->mutex);
				return NULL;
			}
		}
		if (wq->quit) {
			break;
		}
		if (we) {
			status = pthread_mutex_unlock (&wq->mutex);
			assert(EOK == status);
			/* NOTE: if func free 'we', then 'we' should not be on
			   flush_list which means there are no threads waiting on it,
			   else it is a BUG! */
			we->func (we->data);
			/* we is not valid, since it was freed in func() */
			status = pthread_mutex_lock (&wq->mutex);
			assert(EOK == status);

			struct work_struct * w;
			list_for_each_entry(w, &wq->flush_list, list){
				if (we == w){
					we->status = 1;
					pthread_cond_broadcast(&wq->cv);
					break;
				}
			}
			pthread_cond_broadcast(&wq->cv);
		}
	}
	pthread_mutex_unlock (&wq->mutex);
	return NULL;
}

/**
 * work_busy - test whether a work is currently pending or running
 * @work: the work to be tested
 *
 * Test whether @work is currently pending or running.  There is no
 * synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * Return:
 * OR'd bitmask of WORK_BUSY_* bits.
 */
unsigned int work_busy(struct work_struct *work)
{
	return 0;
}

#if defined(__QNXNTO__) && defined(__USESRCVERSION)
#include <sys/srcversion.h>
__SRCVERSION("$URL: http://svn.ott.qnx.com/product/branches/6.6.0/trunk/hardware/gpu/drm/server-3.12.2/qnx/workqueue.c $ $Rev: 779374 $")
#endif
