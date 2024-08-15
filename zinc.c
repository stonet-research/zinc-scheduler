// SPDX-License-Identifier: GPL-2.0
/*
 *  ZINC I/O scheduler - adaptation of the mq-deadline scheduler (from Jens Axboe)
 *  Copyright (C) 2024 @Large Research 
 * 
 *  An extension of the mq-deadline scheduler (mq-deadline.c)
 *  Copyright (C) 2016 Jens Axboe <axboe@kernel.dk>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>

#include <trace/events/block.h>

#include "elevator.h"
#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-sched.h"

 /*
 * WARNING (ZINC)
 * 1. Only a single queue for all reset requests and a single queue for all finish requests
 * 2. This scheduler currently does not support APPEND requests. 
 */

/*
 * Default ZINC parameters
 */
static const int RESET_EPOCH_INTERVAL = 64;	// In ms
static const int RESET_COMMAND_TOKENS = 2000; // 
static const int RESET_MINIMUM_CONCURRENCY_THRESHOLD = 3; // In number of resets 
static const int RESET_MAXIMUM_EPOCH_HOLDS = 3; // In number of retries

static const int FINISH_EPOCH_INTERVAL = 64;	// In ms
static const int FINISH_COMMAND_TOKENS = 2000; // 
static const int FINISH_MINIMUM_CONCURRENCY_THRESHOLD = 3; // In number of resets 
static const int FINISH_MAXIMUM_EPOCH_HOLDS = 3; // In number of retries

/*
 *  I/O Unit conversions
 */
static const int ZINC_IO_SIZE_BIT_SHIFT = 13;	// We keep track of I/O sizes in 8KiB units 
static const int ZINC_IO_SIZE_SECTOR_SHIFT = 4;	// We keep track of I/O sizes in 8KiB

/*
 * See Documentation/block/deadline-iosched.rst
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
/*
 * Time after which to dispatch lower priority requests even if higher
 * priority requests are pending.
 */
static const int prio_aging_expire = 10 * HZ;
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

enum dd_data_dir {
    ZINC_READ,
	ZINC_WRITE,
    ZINC_APPEND,
	ZINC_FINISH,
	ZINC_RESET,
	ZINC_OTHER,
	ZINC_NUM_DIR,
};

#define DD_READ ZINC_READ
#define DD_WRITE ZINC_WRITE

static inline enum dd_data_dir zinc_data_dir(struct request *rq)
{
	enum req_op op = req_op(rq);
	switch (op & REQ_OP_MASK) {
		case REQ_OP_READ:
			return ZINC_READ;
		case REQ_OP_WRITE:
			return ZINC_WRITE;
		case REQ_OP_ZONE_RESET:
			// pr_alert("Reset\n");
			return ZINC_RESET;
		case REQ_OP_ZONE_FINISH:
			// pr_alert("Finish\n");
			return ZINC_FINISH;
		default:
			// pr_alert("Other\n");
			return ZINC_OTHER;
	}
}

enum { DD_DIR_COUNT = 2 };

enum dd_prio {
	DD_RT_PRIO	= 0,
	DD_BE_PRIO	= 1,
	DD_IDLE_PRIO	= 2,
	DD_PRIO_MAX	= 2,
};

enum { DD_PRIO_COUNT = 3 };

/*
 * I/O statistics per I/O priority. It is fine if these counters overflow.
 * What matters is that these counters are at least as wide as
 * log2(max_outstanding_requests).
 */
struct io_stats_per_prio {
	uint32_t inserted;
	uint32_t merged;
	uint32_t dispatched;
	atomic_t completed;
};

/*
 * Deadline scheduler data per I/O priority (enum dd_prio). Requests are
 * present on both sort_list[] and fifo_list[].
 */
struct dd_per_prio {
	struct list_head dispatch;
	struct rb_root sort_list[DD_DIR_COUNT];
	struct list_head fifo_list[DD_DIR_COUNT];
	/* Next request in FIFO order. Read, write or both are NULL. */
	struct request *next_rq[DD_DIR_COUNT];
	struct io_stats_per_prio stats;
};

struct deadline_data {
	// ZINC deadline data
	struct list_head reset_queue; 	// queue for reset requests
	struct list_head finish_queue; 	// queue for finish requests

	atomic_t reset_pending_requests;    	// number of in-flight pending write request in 8KiB units (larger requests are divided into this unit)
	atomic_t finish_pending_requests;    	// number of in-flight pending write request in 8KiB units (larger requests are divided into this unit)
	atomic_t reset_dispatched_write;      // number of dispatched write requests in 8KiB units
	atomic_t finish_dispatched_write;      // number of dispatched write requests in 8KiB units

	// ZINC Parameters
	int reset_command_tokens;
	int reset_maximum_epoch_holds;
	int reset_epoch_interval;     	// in jiffies
	int reset_minimum_concurrency_treshold; // threshold of the maximum number of pending requests in 8KiB units

	int finish_command_tokens;
	int finish_maximum_epoch_holds;
	int finish_epoch_interval;     	// in jiffies
	int finish_minimum_concurrency_treshold; // threshold of the maximum number of pending requests in 8KiB units

	// ZINC timers
	atomic_t reset_timer_fired;
	struct timer_list reset_timer;

	atomic_t finish_timer_fired;
	struct timer_list finish_timer;

	/*
	 * MQ run time data
	 */

	struct dd_per_prio per_prio[DD_PRIO_COUNT];

	/* MQ Data direction of latest dispatched request. */
	enum dd_data_dir last_dir;
	unsigned int batching;		/* number of sequential requests made */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * MQ settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[DD_DIR_COUNT];
	int fifo_batch;
	int writes_starved;
	int front_merges;
	u32 async_depth;
	int prio_aging_expire;

	spinlock_t lock;
	spinlock_t zone_lock;
};

/* Maps an I/O priority class to a deadline scheduler priority. */
static const enum dd_prio ioprio_class_to_prio[] = {
	[IOPRIO_CLASS_NONE]	= DD_BE_PRIO,
	[IOPRIO_CLASS_RT]	= DD_RT_PRIO,
	[IOPRIO_CLASS_BE]	= DD_BE_PRIO,
	[IOPRIO_CLASS_IDLE]	= DD_IDLE_PRIO,
};

/* ZINC timers
 * When the timer is fired, we set the timer-fired flag to true and start a new timer.
 * This is not protected by lock, even if there is a race condition, we only miss a reset dispatch, protected it by a timer
 * will serialized the timer with the insert/dispatch function. MIGHT CAUSE A DEADLOCK.
 */
static void zinc_fin_reset_timer_fn(struct timer_list *t)
{
	struct deadline_data *zd = from_timer(zd, t, reset_timer);
	atomic_set(&zd->reset_timer_fired, 1);
	timer_reduce(&zd->reset_timer, jiffies + zd->reset_epoch_interval);
}

static void zinc_fin_finish_timer_fn(struct timer_list *t)
{
	struct deadline_data *zd = from_timer(zd, t, finish_timer);
	atomic_set(&zd->finish_timer_fired, 1);
	timer_reduce(&zd->finish_timer, jiffies + zd->finish_epoch_interval);
}


static inline struct rb_root *
deadline_rb_root(struct dd_per_prio *per_prio, struct request *rq)
{
	return &per_prio->sort_list[zinc_data_dir(rq)];
}

/*
 * Returns the I/O priority class (IOPRIO_CLASS_*) that has been assigned to a
 * request.
 */
static u8 dd_rq_ioclass(struct request *rq)
{
	return IOPRIO_PRIO_CLASS(req_get_ioprio(rq));
}

/*
 * get the request before `rq' in sector-sorted order
 */
static inline struct request *
deadline_earlier_request(struct request *rq)
{
	struct rb_node *node = rb_prev(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

/*
 * get the request after `rq' in sector-sorted order
 */
static inline struct request *
deadline_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
deadline_add_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	struct rb_root *root = deadline_rb_root(per_prio, rq);

	elv_rb_add(root, rq);
}

static inline void
deadline_del_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	const enum dd_data_dir data_dir = zinc_data_dir(rq);

	if (per_prio->next_rq[data_dir] == rq)
		per_prio->next_rq[data_dir] = deadline_latter_request(rq);

	elv_rb_del(deadline_rb_root(per_prio, rq), rq);
}

/*
 * remove rq from rbtree and fifo.
 */
static void deadline_remove_request(struct request_queue *q,
				    struct dd_per_prio *per_prio,
				    struct request *rq)
{
	list_del_init(&rq->queuelist);

	/*
	 * We might not be on the rbtree, if we are doing an insert merge
	 */
	if (!RB_EMPTY_NODE(&rq->rb_node))
		deadline_del_rq_rb(per_prio, rq);

	elv_rqhash_del(q, rq);
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

static void dd_request_merged(struct request_queue *q, struct request *req,
			      enum elv_merge type)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const u8 ioprio_class = dd_rq_ioclass(req);
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class];
	struct dd_per_prio *per_prio = &dd->per_prio[prio];

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(deadline_rb_root(per_prio, req), req);
		deadline_add_rq_rb(per_prio, req);
	}
}

/*
 * Callback function that is invoked after @next has been merged into @req.
 */
static void dd_merged_requests(struct request_queue *q, struct request *req,
			       struct request *next)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const u8 ioprio_class = dd_rq_ioclass(next);
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class];

	lockdep_assert_held(&dd->lock);

	dd->per_prio[prio].stats.merged++;

	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)req->fifo_time)) {
			list_move(&req->queuelist, &next->queuelist);
			req->fifo_time = next->fifo_time;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	deadline_remove_request(q, &dd->per_prio[prio], next);
}

/*
 * move an entry to dispatch queue
 */
static void
deadline_move_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      struct request *rq)
{
	const enum dd_data_dir data_dir = zinc_data_dir(rq);

	per_prio->next_rq[data_dir] = deadline_latter_request(rq);

	/*
	 * take it off the sort and fifo list
	 */
	deadline_remove_request(rq->q, per_prio, rq);
}

/* Number of requests queued for a given priority level. */
static u32 dd_queued(struct deadline_data *dd, enum dd_prio prio)
{
	const struct io_stats_per_prio *stats = &dd->per_prio[prio].stats;

	lockdep_assert_held(&dd->lock);

	return stats->inserted - atomic_read(&stats->completed);
}

/*
 * deadline_check_fifo returns 0 if there are no expired requests on the fifo,
 * 1 otherwise. Requires !list_empty(&dd->fifo_list[data_dir])
 */
static inline int deadline_check_fifo(struct dd_per_prio *per_prio,
				      enum dd_data_dir data_dir)
{
	struct request *rq = rq_entry_fifo(per_prio->fifo_list[data_dir].next);

	/*
	 * rq is expired!
	 */
	if (time_after_eq(jiffies, (unsigned long)rq->fifo_time))
		return 1;

	return 0;
}

/*
 * Check if rq has a sequential request preceding it.
 */
static bool deadline_is_seq_write(struct deadline_data *dd, struct request *rq)
{
	struct request *prev = deadline_earlier_request(rq);

	if (!prev)
		return false;

	return blk_rq_pos(prev) + blk_rq_sectors(prev) == blk_rq_pos(rq);
}

/*
 * Skip all write requests that are sequential from @rq, even if we cross
 * a zone boundary.
 */
static struct request *deadline_skip_seq_writes(struct deadline_data *dd,
						struct request *rq)
{
	sector_t pos = blk_rq_pos(rq);
	sector_t skipped_sectors = 0;

	while (rq) {
		if (blk_rq_pos(rq) != pos + skipped_sectors)
			break;
		skipped_sectors += blk_rq_sectors(rq);
		rq = deadline_latter_request(rq);
	}

	return rq;
}

/*
 * For the specified data direction, return the next request to
 * dispatch using arrival ordered lists.
 */
static struct request *
deadline_fifo_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	struct request *rq;
	unsigned long flags;

	if (list_empty(&per_prio->fifo_list[data_dir]))
		return NULL;

	rq = rq_entry_fifo(per_prio->fifo_list[data_dir].next);
	if (data_dir == DD_READ || !blk_queue_is_zoned(rq->q))
		return rq;

	/*
	 * Look for a write request that can be dispatched, that is one with
	 * an unlocked target zone. For some HDDs, breaking a sequential
	 * write stream can lead to lower throughput, so make sure to preserve
	 * sequential write streams, even if that stream crosses into the next
	 * zones and these zones are unlocked.
	 */
	spin_lock_irqsave(&dd->zone_lock, flags);
	list_for_each_entry(rq, &per_prio->fifo_list[DD_WRITE], queuelist) {
		if (blk_req_can_dispatch_to_zone(rq) &&
		    (blk_queue_nonrot(rq->q) ||
		     !deadline_is_seq_write(dd, rq)))
			goto out;
	}
	rq = NULL;
out:
	spin_unlock_irqrestore(&dd->zone_lock, flags);

	return rq;
}

/*
 * For the specified data direction, return the next request to
 * dispatch using sector position sorted lists.
 */
static struct request *
deadline_next_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	struct request *rq;
	unsigned long flags;

	rq = per_prio->next_rq[data_dir];
	if (!rq)
		return NULL;

	if (data_dir == DD_READ || !blk_queue_is_zoned(rq->q))
		return rq;

	/*
	 * Look for a write request that can be dispatched, that is one with
	 * an unlocked target zone. For some HDDs, breaking a sequential
	 * write stream can lead to lower throughput, so make sure to preserve
	 * sequential write streams, even if that stream crosses into the next
	 * zones and these zones are unlocked.
	 */
	spin_lock_irqsave(&dd->zone_lock, flags);
	while (rq) {
		if (blk_req_can_dispatch_to_zone(rq))
			break;
		if (blk_queue_nonrot(rq->q))
			rq = deadline_latter_request(rq);
		else
			rq = deadline_skip_seq_writes(dd, rq);
	}
	spin_unlock_irqrestore(&dd->zone_lock, flags);

	return rq;
}

/*
 * Returns true if and only if @rq started after @latest_start where
 * @latest_start is in jiffies.
 */
static bool started_after(struct deadline_data *dd, struct request *rq,
			  unsigned long latest_start)
{
	unsigned long start_time = (unsigned long)rq->fifo_time;

	start_time -= dd->fifo_expire[zinc_data_dir(rq)];

	return time_after(start_time, latest_start);
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc and with a start time <= @latest_start.
 */
static struct request *__dd_dispatch_request(struct deadline_data *dd,
					     struct dd_per_prio *per_prio,
					     unsigned long latest_start)
{
	struct request *rq, *next_rq;
	enum dd_data_dir data_dir;
	enum dd_prio prio;
	u8 ioprio_class;
	int pending_requests = 0;

	lockdep_assert_held(&dd->lock);

	/* ZINC
	 * If the timer is fired, we first check if we can issue a reset
	 */
	if (atomic_cmpxchg(&(dd->reset_timer_fired), 1, 0)) {
		// dispatch reset
		pending_requests = atomic_read(&dd->reset_pending_requests);

		// case 0: The number of pending requests is less to the threshold, dispatch
		if ((!list_empty(&dd->reset_queue)) && 
		    pending_requests < dd->reset_minimum_concurrency_treshold) {
			// pr_alert("Pending < Reset \n");
			rq = list_first_entry(&dd->reset_queue, struct request,
				      queuelist);
			list_del_init(&rq->queuelist);
			atomic_set(&dd->reset_dispatched_write, 0); // Reset the write counter for the next time window
			goto done;
		}
		// case 1: We have dispatched enough write, then dispatch a reset
		else if ((!list_empty(&dd->reset_queue)) && 
		   atomic_read(&dd->reset_dispatched_write) > dd->reset_command_tokens) {
				// pr_alert("Tokens < Reset \n");
			rq = list_first_entry(&dd->reset_queue, struct request,
				      queuelist);
			list_del_init(&rq->queuelist);
			atomic_set(&dd->reset_dispatched_write, 0); // Reset the write counter for the next time window
			goto done;
		}
		// case 2: We haven't dispatched enough write, but we have a high priority reset
		else if ((!list_empty(&dd->reset_queue)) && 
		list_first_entry(&dd->reset_queue, struct request, queuelist)->deadline >= dd->reset_maximum_epoch_holds) {
				// pr_alert("Reset dispatched \n");
			rq = list_first_entry(&dd->reset_queue, struct request,
				      queuelist);
			list_del_init(&rq->queuelist);
			atomic_set(&dd->reset_dispatched_write, 0); // Reset the write counter for the next time window
			goto done;
		}
		// case 3: We can not dispatch a reset, then we increment the priority for each pending reset.
		//         Then continue to dispatch a normal request.
		list_for_each_entry(rq, &dd->reset_queue, queuelist) {
					// pr_alert("Reset prio\n");
			rq->deadline++;
		}

		//atomic_set(&dd->reset_dispatched_write, 0); // Reset the write counter for the next time window
	}


	/* ZINC
	 * If the timer is fired, we first check if we can issue a finish
	 */
	if (atomic_cmpxchg(&(dd->finish_timer_fired), 1, 0)) {
		// dispatch reset
		pending_requests = atomic_read(&dd->finish_pending_requests);

		// case 0: The number of pending requests is less to the threshold, dispatch
		if ((!list_empty(&dd->finish_queue)) && 
		    pending_requests < dd->finish_minimum_concurrency_treshold) {
			// pr_alert("Pending < finish min\n");
			rq = list_first_entry(&dd->finish_queue, struct request,
				      queuelist);
			list_del_init(&rq->queuelist);
			atomic_set(&dd->finish_dispatched_write, 0); // Reset the write counter for the next time window
			goto done;
		}
		// case 1: We have dispatched enough write, then dispatch a finish
		else if ((!list_empty(&dd->finish_queue)) && 
		   atomic_read(&dd->finish_dispatched_write) > dd->finish_command_tokens) {
			// pr_alert("Write < finish tokens\n");
			rq = list_first_entry(&dd->finish_queue, struct request,
				      queuelist);
			list_del_init(&rq->queuelist);
			atomic_set(&dd->finish_dispatched_write, 0); // Reset the write counter for the next time window
			goto done;
		}
		// case 2: We haven't dispatched enough write, but we have a high priority reset
		else if ((!list_empty(&dd->finish_queue)) && 
		list_first_entry(&dd->finish_queue, struct request, queuelist)->deadline >= dd->finish_maximum_epoch_holds) {
			// pr_alert("Max epoch holds\n");
			rq = list_first_entry(&dd->finish_queue, struct request,
				      queuelist);
			list_del_init(&rq->queuelist);
			atomic_set(&dd->finish_dispatched_write, 0); // Reset the write counter for the next time window
			goto done;
		}
		// case 3: We can not dispatch a reset, then we increment the priority for each pending reset.
		//         Then continue to dispatch a normal request.
		list_for_each_entry(rq, &dd->finish_queue, queuelist) {
			// pr_alert("Postponing %d\n", rq->deadline);
			rq->deadline++;
		}

		//atomic_set(&dd->finish_dispatched_write, 0); // Reset the write counter for the next time window
	}


	if (!list_empty(&per_prio->dispatch)) {
		rq = list_first_entry(&per_prio->dispatch, struct request,
				      queuelist);
		if (started_after(dd, rq, latest_start))
			return NULL;
		list_del_init(&rq->queuelist);
		goto done;
	}

	/*
	 * batches are currently reads XOR writes
	 */
	rq = deadline_next_request(dd, per_prio, dd->last_dir);
	if (rq && dd->batching < dd->fifo_batch)
		/* we have a next request are still entitled to batch */
		goto dispatch_request;

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (!list_empty(&per_prio->fifo_list[DD_READ])) {
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_READ]));

		if (deadline_fifo_request(dd, per_prio, DD_WRITE) &&
		    (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;

		data_dir = DD_READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (!list_empty(&per_prio->fifo_list[DD_WRITE])) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_WRITE]));

		dd->starved = 0;

		data_dir = DD_WRITE;

		goto dispatch_find_request;
	}

	return NULL;

dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	next_rq = deadline_next_request(dd, per_prio, data_dir);
	if (deadline_check_fifo(per_prio, data_dir) || !next_rq) {
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		rq = deadline_fifo_request(dd, per_prio, data_dir);
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		rq = next_rq;
	}

	/*
	 * For a zoned block device, if we only have writes queued and none of
	 * them can be dispatched, rq will be NULL.
	 */
	if (!rq)
		return NULL;

	dd->last_dir = data_dir;
	dd->batching = 0;

dispatch_request:
	if (started_after(dd, rq, latest_start))
		return NULL;

	/*
	 * rq is the selected appropriate request.
	 */
	dd->batching++;
	deadline_move_request(dd, per_prio, rq);
done:
	ioprio_class = dd_rq_ioclass(rq);
	prio = ioprio_class_to_prio[ioprio_class];
	dd->per_prio[prio].stats.dispatched++;
	/*
	 * If the request needs its target zone locked, do it.
	 */
	blk_req_zone_write_lock(rq);
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

/*
 * Check whether there are any requests with priority other than DD_RT_PRIO
 * that were inserted more than prio_aging_expire jiffies ago.
 */
static struct request *dd_dispatch_prio_aged_requests(struct deadline_data *dd,
						      unsigned long now)
{
	struct request *rq;
	enum dd_prio prio;
	int prio_cnt;

	lockdep_assert_held(&dd->lock);

	prio_cnt = !!dd_queued(dd, DD_RT_PRIO) + !!dd_queued(dd, DD_BE_PRIO) +
		   !!dd_queued(dd, DD_IDLE_PRIO);
	if (prio_cnt < 2)
		return NULL;

	for (prio = DD_BE_PRIO; prio <= DD_PRIO_MAX; prio++) {
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio],
					   now - dd->prio_aging_expire);
		if (rq)
			return rq;
	}

	return NULL;
}

/*
 * Called from blk_mq_run_hw_queue() -> __blk_mq_sched_dispatch_requests().
 *
 * One confusing aspect here is that we get called for a specific
 * hardware queue, but we may return a request that is for a
 * different hardware queue. This is because mq-deadline has shared
 * state for all hardware queues, in terms of sorting, FIFOs, etc.
 */
static struct request *dd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	const unsigned long now = jiffies;
	struct request *rq;
	enum dd_prio prio;

	spin_lock(&dd->lock);
	rq = dd_dispatch_prio_aged_requests(dd, now);
	if (rq)
		goto unlock;

	/*
	 * Next, dispatch requests in priority order. Ignore lower priority
	 * requests if any higher priority requests are pending.
	 */
	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio], now);
		if (rq || dd_queued(dd, prio))
			break;
	}

unlock:
    if (rq && zinc_data_dir(rq) == ZINC_WRITE) {
        // Figure out the I/O size from the request
        unsigned int io_units = (rq->__data_len >> ZINC_IO_SIZE_BIT_SHIFT);	

        // If I/O was smaller than 8KiB, we still count it as 1, since flash page size for us is 8KiB anyways
        if (io_units < 1u)
            io_units = 1;

        atomic_add(io_units, &dd->reset_dispatched_write);
        atomic_add(io_units, &dd->finish_dispatched_write);
        atomic_add(io_units, &dd->reset_pending_requests);
        atomic_add(io_units, &dd->finish_pending_requests);
    }

	spin_unlock(&dd->lock);

	return rq;
}

/*
 * Called by __blk_mq_alloc_request(). The shallow_depth value set by this
 * function is used by __blk_mq_get_tag().
 */
static void dd_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	struct deadline_data *dd = data->q->elevator->elevator_data;

	/* Do not throttle synchronous reads. */
	if (op_is_sync(opf) && !op_is_write(opf))
		return;

	/*
	 * Throttle asynchronous requests and writes such that these requests
	 * do not block the allocation of synchronous requests.
	 */
	data->shallow_depth = dd->async_depth;
}

/* Called by blk_mq_update_nr_requests(). */
static void dd_depth_updated(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	struct blk_mq_tags *tags = hctx->sched_tags;

	dd->async_depth = max(1UL, 3 * q->nr_requests / 4);

	sbitmap_queue_min_shallow_depth(&tags->bitmap_tags, dd->async_depth);
}

/* Called by blk_mq_init_hctx() and blk_mq_init_sched(). */
static int dd_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	dd_depth_updated(hctx);
	return 0;
}

static void dd_exit_sched(struct elevator_queue *e)
{
	struct deadline_data *dd = e->elevator_data;
	enum dd_prio prio;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		struct dd_per_prio *per_prio = &dd->per_prio[prio];
		const struct io_stats_per_prio *stats = &per_prio->stats;
		uint32_t queued;

		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_READ]));
		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_WRITE]));

		spin_lock(&dd->lock);
		queued = dd_queued(dd, prio);
		spin_unlock(&dd->lock);

		WARN_ONCE(queued != 0,
			  "statistics for priority %d: i %u m %u d %u c %u\n",
			  prio, stats->inserted, stats->merged,
			  stats->dispatched, atomic_read(&stats->completed));
	}

	// ZINC
	WARN_ON_ONCE(!list_empty(&dd->reset_queue));
	WARN_ON_ONCE(!list_empty(&dd->finish_queue));
	timer_shutdown_sync(&dd->reset_timer);
	timer_shutdown_sync(&dd->finish_timer);

	kfree(dd);
}

/*
 * initialize elevator private data (deadline_data).
 */
static int dd_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct deadline_data *dd;
	struct elevator_queue *eq;
	enum dd_prio prio;
	int ret = -ENOMEM;

	eq = elevator_alloc(q, e);
	if (!eq)
		return ret;

	dd = kzalloc_node(sizeof(*dd), GFP_KERNEL, q->node);
	if (!dd)
		goto put_eq;

	eq->elevator_data = dd;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) {
		struct dd_per_prio *per_prio = &dd->per_prio[prio];

		INIT_LIST_HEAD(&per_prio->dispatch);
		INIT_LIST_HEAD(&per_prio->fifo_list[DD_READ]);
		INIT_LIST_HEAD(&per_prio->fifo_list[DD_WRITE]);
		per_prio->sort_list[DD_READ] = RB_ROOT;
		per_prio->sort_list[DD_WRITE] = RB_ROOT;
	}
	dd->fifo_expire[DD_READ] = read_expire;
	dd->fifo_expire[DD_WRITE] = write_expire;
	dd->writes_starved = writes_starved;
	dd->front_merges = 1;
	dd->last_dir = DD_WRITE;
	dd->fifo_batch = fifo_batch;
	dd->prio_aging_expire = prio_aging_expire;

    // ZINC
	INIT_LIST_HEAD(&dd->reset_queue);
	INIT_LIST_HEAD(&dd->finish_queue);
	atomic_set(&dd->reset_pending_requests, 0);
	atomic_set(&dd->finish_pending_requests, 0);
	atomic_set(&dd->reset_dispatched_write, 0);
	atomic_set(&dd->finish_dispatched_write, 0);
	atomic_set(&dd->reset_timer_fired, 0);

	// reset
	dd->reset_command_tokens = RESET_COMMAND_TOKENS;
	dd->reset_maximum_epoch_holds = RESET_MAXIMUM_EPOCH_HOLDS;
	dd->reset_epoch_interval = msecs_to_jiffies(RESET_EPOCH_INTERVAL);
	if (dd->reset_epoch_interval < 1) {
		dd->reset_epoch_interval = 1;
	}
	dd->reset_minimum_concurrency_treshold = RESET_MINIMUM_CONCURRENCY_THRESHOLD;
	timer_setup(&dd->reset_timer, zinc_fin_reset_timer_fn, 0);
	timer_reduce(&dd->reset_timer, jiffies + dd->reset_epoch_interval);

	// finish
	dd->finish_command_tokens = FINISH_COMMAND_TOKENS;
	dd->finish_maximum_epoch_holds = FINISH_MAXIMUM_EPOCH_HOLDS;
	dd->finish_epoch_interval = msecs_to_jiffies(FINISH_EPOCH_INTERVAL);
	if (dd->finish_epoch_interval < 1) {
		dd->finish_epoch_interval = 1;
	}
	dd->finish_minimum_concurrency_treshold = FINISH_MINIMUM_CONCURRENCY_THRESHOLD;
	timer_setup(&dd->finish_timer, zinc_fin_finish_timer_fn, 0);
	timer_reduce(&dd->finish_timer, jiffies + dd->finish_epoch_interval);	

	spin_lock_init(&dd->lock);
	spin_lock_init(&dd->zone_lock);

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);

	q->elevator = eq;
	return 0;

put_eq:
	kobject_put(&eq->kobj);
	return ret;
}

/*
 * Try to merge @bio into an existing request. If @bio has been merged into
 * an existing request, store the pointer to that request into *@rq.
 */
static int dd_request_merge(struct request_queue *q, struct request **rq,
			    struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const u8 ioprio_class = IOPRIO_PRIO_CLASS(bio->bi_ioprio);
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class];
	struct dd_per_prio *per_prio = &dd->per_prio[prio];
	sector_t sector = bio_end_sector(bio);
	struct request *__rq;

	if (!dd->front_merges)
		return ELEVATOR_NO_MERGE;

	__rq = elv_rb_find(&per_prio->sort_list[bio_data_dir(bio)], sector);
	if (__rq) {
		BUG_ON(sector != blk_rq_pos(__rq));

		if (elv_bio_merge_ok(__rq, bio)) {
			*rq = __rq;
			if (blk_discard_mergable(__rq))
				return ELEVATOR_DISCARD_MERGE;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to merge a bio into an existing request. This function is called
 * before @bio is associated with a request.
 */
static bool dd_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct request *free = NULL;
	bool ret;

	spin_lock(&dd->lock);
	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free);
	spin_unlock(&dd->lock);

	if (free)
		blk_mq_free_request(free);

	return ret;
}

/*
 * add rq to rbtree and fifo
 */
static void dd_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
			      blk_insert_t flags)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	const enum dd_data_dir data_dir = zinc_data_dir(rq);
	u16 ioprio = req_get_ioprio(rq);
	u8 ioprio_class = IOPRIO_PRIO_CLASS(ioprio);
	struct dd_per_prio *per_prio;
	enum dd_prio prio;
	int pending_requests = 0;
	LIST_HEAD(free);

	lockdep_assert_held(&dd->lock);

	/*
	 * This may be a requeue of a write request that has locked its
	 * target zone. If it is the case, this releases the zone lock.
	//  */
	// if (blk_req_zone_is_write_locked(rq)) {
	// 	printk("Requeue \n");
 	// 	io_units = (rq->__data_len) >> ZINC_IO_SIZE_BIT_SHIFT;		
	// 	if (io_units < 1u)
    //         io_units = 1;
	// 	atomic_sub(io_units, &dd->pending_requests); 
	// }
	blk_req_zone_write_unlock(rq);

	if (data_dir == ZINC_FINISH) {
		rq->deadline = 0;
		// pr_alert("Add finish\n");
		list_add(&rq->queuelist, &dd->finish_queue);
		// Force timer
		pending_requests = atomic_read(&dd->finish_pending_requests);
		if (pending_requests < dd->finish_minimum_concurrency_treshold) {
			atomic_set(&(dd->finish_timer_fired), 1);
		}
		return;
    }	
    else if (data_dir > ZINC_WRITE) {
        rq->deadline = 0;
		// pr_alert("Add Reset\n");
        list_add(&rq->queuelist, &dd->reset_queue);
        // Force timer
		pending_requests = atomic_read(&dd->reset_pending_requests);
		if (pending_requests < dd->reset_minimum_concurrency_treshold) {
			atomic_set(&(dd->reset_timer_fired), 1);
		}
		return;
    } 

	prio = ioprio_class_to_prio[ioprio_class];
	per_prio = &dd->per_prio[prio];
	if (!rq->elv.priv[0]) {
		per_prio->stats.inserted++;
		rq->elv.priv[0] = (void *)(uintptr_t)1;
	}

	if (blk_mq_sched_try_insert_merge(q, rq, &free)) {
		blk_mq_free_requests(&free);
		return;
	}

	trace_block_rq_insert(rq);

	if (flags & BLK_MQ_INSERT_AT_HEAD) {
		list_add(&rq->queuelist, &per_prio->dispatch);
		rq->fifo_time = jiffies;
	} else {
		deadline_add_rq_rb(per_prio, rq);

		if (rq_mergeable(rq)) {
			elv_rqhash_add(q, rq);
			if (!q->last_merge)
				q->last_merge = rq;
		}

		/*
		 * set expire time and add to fifo list
		 */
		rq->fifo_time = jiffies + dd->fifo_expire[data_dir];
		list_add_tail(&rq->queuelist, &per_prio->fifo_list[data_dir]);
	}
}

/*
 * Called from blk_mq_sched_insert_request() or blk_mq_sched_insert_requests().
 */
static void dd_insert_requests(struct blk_mq_hw_ctx *hctx,
			       struct list_head *list, blk_insert_t flags)
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;

	spin_lock(&dd->lock);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
		dd_insert_request(hctx, rq, flags);
	}
	spin_unlock(&dd->lock);
}

/* Callback from inside blk_mq_rq_ctx_init(). */
static void dd_prepare_request(struct request *rq)
{
	rq->elv.priv[0] = NULL;
}

static bool dd_has_write_work(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	enum dd_prio p;

	for (p = 0; p <= DD_PRIO_MAX; p++)
		if (!list_empty_careful(&dd->per_prio[p].fifo_list[DD_WRITE]))
			return true;

	return false;
}

/*
 * Callback from inside blk_mq_free_request().
 *
 * For zoned block devices, write unlock the target zone of
 * completed write requests. Do this while holding the zone lock
 * spinlock so that the zone is never unlocked while deadline_fifo_request()
 * or deadline_next_request() are executing. This function is called for
 * all requests, whether or not these requests complete successfully.
 *
 * For a zoned block device, __dd_dispatch_request() may have stopped
 * dispatching requests if all the queued requests are write requests directed
 * at zones that are already locked due to on-going write requests. To ensure
 * write request dispatch progress in this case, mark the queue as needing a
 * restart to ensure that the queue is run again after completion of the
 * request and zones being unlocked.
 */
static void dd_finish_request(struct request *rq)
{
	struct request_queue *q = rq->q;
	struct deadline_data *dd = q->elevator->elevator_data;
	const u8 ioprio_class = dd_rq_ioclass(rq);
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class];
	struct dd_per_prio *per_prio = &dd->per_prio[prio];
	int pending_requests = 0;
	unsigned int io_units = 0;

	/*
	 * The block layer core may call dd_finish_request() without having
	 * called dd_insert_requests(). Skip requests that bypassed I/O
	 * scheduling. See also blk_mq_request_bypass_insert().
	 */
	if (!rq->elv.priv[0])
		return;

	atomic_inc(&per_prio->stats.completed);

    // ZINC
	if (zinc_data_dir(rq) == ZINC_WRITE) {
		// Instead of the __data_len on finish request this is may no longer be set and the device sets the number of sectors written
		io_units = (rq->stats_sectors >> ZINC_IO_SIZE_SECTOR_SHIFT);	

		// If I/O was smaller than 8KiB, we still count it as 1, since flash page size for us is 8KiB anyways
		if (io_units < 1u)
			io_units = 1;

		atomic_sub(io_units, &dd->finish_pending_requests); 
		atomic_sub(io_units, &dd->reset_pending_requests); 
		// printk("DECREASE HERE %d %d TYPE %d\n", atomic_read(&zd->pending_requests), io_units, zinc_data_dir(rq));
	}  else if (zinc_data_dir(rq) == ZINC_FINISH) {
		pending_requests = atomic_read(&dd->finish_pending_requests);
		if (pending_requests < dd->finish_minimum_concurrency_treshold) {
			// printk("Reset fired instantly\n");
			atomic_set(&(dd->finish_timer_fired), 1);
		}
	}  else if (zinc_data_dir(rq) > ZINC_WRITE) {
		pending_requests = atomic_read(&dd->reset_pending_requests);
		if (pending_requests < dd->reset_minimum_concurrency_treshold) {
			// printk("Reset fired instantly\n");
			atomic_set(&(dd->reset_timer_fired), 1);
		}
	}

	if (blk_queue_is_zoned(q)) {
		unsigned long flags;

		spin_lock_irqsave(&dd->zone_lock, flags);
		blk_req_zone_write_unlock(rq);
		spin_unlock_irqrestore(&dd->zone_lock, flags);

		if (dd_has_write_work(rq->mq_hctx))
			blk_mq_sched_mark_restart_hctx(rq->mq_hctx);
	}
}

static bool dd_has_work_for_prio(struct dd_per_prio *per_prio)
{
	return !list_empty_careful(&per_prio->dispatch) ||
		!list_empty_careful(&per_prio->fifo_list[DD_READ]) ||
		!list_empty_careful(&per_prio->fifo_list[DD_WRITE]);
}

static bool dd_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	enum dd_prio prio;

	for (prio = 0; prio <= DD_PRIO_MAX; prio++)
		if (dd_has_work_for_prio(&dd->per_prio[prio]))
			return true;

    // ZINC
    if (!list_empty(&dd->reset_queue)) {
        return true;
    }
    if (!list_empty(&dd->finish_queue)) {
        return true;
    }

	return false;
}

/*
 * sysfs parts below
 */
#define SHOW_INT(__FUNC, __VAR)						\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct deadline_data *dd = e->elevator_data;			\
									\
	return sysfs_emit(page, "%d\n", __VAR);				\
}
#define SHOW_JIFFIES(__FUNC, __VAR) SHOW_INT(__FUNC, jiffies_to_msecs(__VAR))
SHOW_JIFFIES(deadline_read_expire_show, dd->fifo_expire[DD_READ]);
SHOW_JIFFIES(deadline_write_expire_show, dd->fifo_expire[DD_WRITE]);
SHOW_JIFFIES(deadline_prio_aging_expire_show, dd->prio_aging_expire);
SHOW_INT(deadline_writes_starved_show, dd->writes_starved);
SHOW_INT(deadline_front_merges_show, dd->front_merges);
SHOW_INT(deadline_async_depth_show, dd->async_depth);
SHOW_INT(deadline_fifo_batch_show, dd->fifo_batch);

SHOW_INT(deadline_reset_maximum_epoch_holds_show, dd->reset_maximum_epoch_holds);
SHOW_INT(deadline_reset_command_tokens_show, dd->reset_command_tokens);
SHOW_JIFFIES(deadline_reset_epoch_interval_show, dd->reset_epoch_interval);
SHOW_INT(deadline_reset_minimum_concurrency_treshold_show, dd->reset_minimum_concurrency_treshold);

SHOW_INT(deadline_finish_maximum_epoch_holds_show, dd->finish_maximum_epoch_holds);
SHOW_INT(deadline_finish_command_tokens_show, dd->finish_command_tokens);
SHOW_JIFFIES(deadline_finish_epoch_interval_show, dd->finish_epoch_interval);
SHOW_INT(deadline_finish_minimum_concurrency_treshold_show, dd->finish_minimum_concurrency_treshold);

#undef SHOW_INT
#undef SHOW_JIFFIES

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data, __ret;						\
									\
	__ret = kstrtoint(page, 0, &__data);				\
	if (__ret < 0)							\
		return __ret;						\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	*(__PTR) = __CONV(__data);					\
	return count;							\
}
#define STORE_INT(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, )
#define STORE_JIFFIES(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, msecs_to_jiffies)
STORE_JIFFIES(deadline_read_expire_store, &dd->fifo_expire[DD_READ], 0, INT_MAX);
STORE_JIFFIES(deadline_write_expire_store, &dd->fifo_expire[DD_WRITE], 0, INT_MAX);
STORE_JIFFIES(deadline_prio_aging_expire_store, &dd->prio_aging_expire, 0, INT_MAX);
STORE_INT(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX);
STORE_INT(deadline_front_merges_store, &dd->front_merges, 0, 1);
STORE_INT(deadline_async_depth_store, &dd->async_depth, 1, INT_MAX);
STORE_INT(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX);

STORE_INT(deadline_reset_maximum_epoch_holds_store, &dd->reset_maximum_epoch_holds, 0, INT_MAX);
STORE_INT(deadline_reset_command_tokens_store, &dd->reset_command_tokens, 0, INT_MAX);
STORE_JIFFIES(deadline_reset_epoch_interval_store, &dd->reset_epoch_interval, 0, INT_MAX);
STORE_INT(deadline_reset_minimum_concurrency_treshold_store, &dd->reset_minimum_concurrency_treshold, 0, INT_MAX);

STORE_INT(deadline_finish_maximum_epoch_holds_store, &dd->finish_maximum_epoch_holds, 0, INT_MAX);
STORE_INT(deadline_finish_command_tokens_store, &dd->finish_command_tokens, 0, INT_MAX);
STORE_JIFFIES(deadline_finish_epoch_interval_store, &dd->finish_epoch_interval, 0, INT_MAX);
STORE_INT(deadline_finish_minimum_concurrency_treshold_store, &dd->finish_minimum_concurrency_treshold, 0, INT_MAX);

#undef STORE_FUNCTION
#undef STORE_INT
#undef STORE_JIFFIES

#define DD_ATTR(name) \
	__ATTR(name, 0644, deadline_##name##_show, deadline_##name##_store)

static struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(async_depth),
	DD_ATTR(fifo_batch),
	DD_ATTR(prio_aging_expire),
	DD_ATTR(reset_maximum_epoch_holds),
	DD_ATTR(reset_command_tokens),
	DD_ATTR(reset_epoch_interval),
	DD_ATTR(reset_minimum_concurrency_treshold),
	DD_ATTR(finish_maximum_epoch_holds),
	DD_ATTR(finish_command_tokens),
	DD_ATTR(finish_epoch_interval),
	DD_ATTR(finish_minimum_concurrency_treshold),
	__ATTR_NULL
};

#ifdef CONFIG_BLK_DEBUG_FS
#define DEADLINE_DEBUGFS_DDIR_ATTRS(prio, data_dir, name)		\
static void *deadline_##name##_fifo_start(struct seq_file *m,		\
					  loff_t *pos)			\
	__acquires(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	spin_lock(&dd->lock);						\
	return seq_list_start(&per_prio->fifo_list[data_dir], *pos);	\
}									\
									\
static void *deadline_##name##_fifo_next(struct seq_file *m, void *v,	\
					 loff_t *pos)			\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	return seq_list_next(v, &per_prio->fifo_list[data_dir], pos);	\
}									\
									\
static void deadline_##name##_fifo_stop(struct seq_file *m, void *v)	\
	__releases(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
									\
	spin_unlock(&dd->lock);						\
}									\
									\
static const struct seq_operations deadline_##name##_fifo_seq_ops = {	\
	.start	= deadline_##name##_fifo_start,				\
	.next	= deadline_##name##_fifo_next,				\
	.stop	= deadline_##name##_fifo_stop,				\
	.show	= blk_mq_debugfs_rq_show,				\
};									\
									\
static int deadline_##name##_next_rq_show(void *data,			\
					  struct seq_file *m)		\
{									\
	struct request_queue *q = data;					\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
	struct request *rq = per_prio->next_rq[data_dir];		\
									\
	if (rq)								\
		__blk_mq_debugfs_rq_show(m, rq);			\
	return 0;							\
}

DEADLINE_DEBUGFS_DDIR_ATTRS(DD_RT_PRIO, DD_READ, read0);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_RT_PRIO, DD_WRITE, write0);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_BE_PRIO, DD_READ, read1);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_BE_PRIO, DD_WRITE, write1);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_IDLE_PRIO, DD_READ, read2);
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_IDLE_PRIO, DD_WRITE, write2);
#undef DEADLINE_DEBUGFS_DDIR_ATTRS

static int deadline_batching_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", dd->batching);
	return 0;
}

static int deadline_starved_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", dd->starved);
	return 0;
}

static int dd_async_depth_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", dd->async_depth);
	return 0;
}

static int dd_queued_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;
	u32 rt, be, idle;

	spin_lock(&dd->lock);
	rt = dd_queued(dd, DD_RT_PRIO);
	be = dd_queued(dd, DD_BE_PRIO);
	idle = dd_queued(dd, DD_IDLE_PRIO);
	spin_unlock(&dd->lock);

	seq_printf(m, "%u %u %u\n", rt, be, idle);

	return 0;
}

/* Number of requests owned by the block driver for a given priority. */
static u32 dd_owned_by_driver(struct deadline_data *dd, enum dd_prio prio)
{
	const struct io_stats_per_prio *stats = &dd->per_prio[prio].stats;

	lockdep_assert_held(&dd->lock);

	return stats->dispatched + stats->merged -
		atomic_read(&stats->completed);
}

static int dd_owned_by_driver_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;
	u32 rt, be, idle;

	spin_lock(&dd->lock);
	rt = dd_owned_by_driver(dd, DD_RT_PRIO);
	be = dd_owned_by_driver(dd, DD_BE_PRIO);
	idle = dd_owned_by_driver(dd, DD_IDLE_PRIO);
	spin_unlock(&dd->lock);

	seq_printf(m, "%u %u %u\n", rt, be, idle);

	return 0;
}

#define DEADLINE_DISPATCH_ATTR(prio)					\
static void *deadline_dispatch##prio##_start(struct seq_file *m,	\
					     loff_t *pos)		\
	__acquires(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	spin_lock(&dd->lock);						\
	return seq_list_start(&per_prio->dispatch, *pos);		\
}									\
									\
static void *deadline_dispatch##prio##_next(struct seq_file *m,		\
					    void *v, loff_t *pos)	\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	return seq_list_next(v, &per_prio->dispatch, pos);		\
}									\
									\
static void deadline_dispatch##prio##_stop(struct seq_file *m, void *v)	\
	__releases(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
									\
	spin_unlock(&dd->lock);						\
}									\
									\
static const struct seq_operations deadline_dispatch##prio##_seq_ops = { \
	.start	= deadline_dispatch##prio##_start,			\
	.next	= deadline_dispatch##prio##_next,			\
	.stop	= deadline_dispatch##prio##_stop,			\
	.show	= blk_mq_debugfs_rq_show,				\
}

DEADLINE_DISPATCH_ATTR(0);
DEADLINE_DISPATCH_ATTR(1);
DEADLINE_DISPATCH_ATTR(2);
#undef DEADLINE_DISPATCH_ATTR

#define DEADLINE_QUEUE_DDIR_ATTRS(name)					\
	{#name "_fifo_list", 0400,					\
			.seq_ops = &deadline_##name##_fifo_seq_ops}
#define DEADLINE_NEXT_RQ_ATTR(name)					\
	{#name "_next_rq", 0400, deadline_##name##_next_rq_show}
static const struct blk_mq_debugfs_attr deadline_queue_debugfs_attrs[] = {
	DEADLINE_QUEUE_DDIR_ATTRS(read0),
	DEADLINE_QUEUE_DDIR_ATTRS(write0),
	DEADLINE_QUEUE_DDIR_ATTRS(read1),
	DEADLINE_QUEUE_DDIR_ATTRS(write1),
	DEADLINE_QUEUE_DDIR_ATTRS(read2),
	DEADLINE_QUEUE_DDIR_ATTRS(write2),
	DEADLINE_NEXT_RQ_ATTR(read0),
	DEADLINE_NEXT_RQ_ATTR(write0),
	DEADLINE_NEXT_RQ_ATTR(read1),
	DEADLINE_NEXT_RQ_ATTR(write1),
	DEADLINE_NEXT_RQ_ATTR(read2),
	DEADLINE_NEXT_RQ_ATTR(write2),
	{"batching", 0400, deadline_batching_show},
	{"starved", 0400, deadline_starved_show},
	{"async_depth", 0400, dd_async_depth_show},
	{"dispatch0", 0400, .seq_ops = &deadline_dispatch0_seq_ops},
	{"dispatch1", 0400, .seq_ops = &deadline_dispatch1_seq_ops},
	{"dispatch2", 0400, .seq_ops = &deadline_dispatch2_seq_ops},
	{"owned_by_driver", 0400, dd_owned_by_driver_show},
	{"queued", 0400, dd_queued_show},
	{},
};
#undef DEADLINE_QUEUE_DDIR_ATTRS
#endif

static struct elevator_type zinc = {
	.ops = {
		.depth_updated		= dd_depth_updated,
		.limit_depth		= dd_limit_depth,
		.insert_requests	= dd_insert_requests,
		.dispatch_request	= dd_dispatch_request,
		.prepare_request	= dd_prepare_request,
		.finish_request		= dd_finish_request,
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
		.bio_merge		= dd_bio_merge,
		.request_merge		= dd_request_merge,
		.requests_merged	= dd_merged_requests,
		.request_merged		= dd_request_merged,
		.has_work		= dd_has_work,
		.init_sched		= dd_init_sched,
		.exit_sched		= dd_exit_sched,
		.init_hctx		= dd_init_hctx,
	},

#ifdef CONFIG_BLK_DEBUG_FS
	.queue_debugfs_attrs = deadline_queue_debugfs_attrs,
#endif
	.elevator_attrs = deadline_attrs,
	.elevator_name = "zinc",
	.elevator_alias = "zinc",
	.elevator_features = ELEVATOR_F_ZBD_SEQ_WRITE,
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("zinc-iosched");

static int __init deadline_init(void)
{
	return elv_register(&zinc);
}

static void __exit deadline_exit(void)
{
	elv_unregister(&zinc);
}

module_init(deadline_init);
module_exit(deadline_exit);

MODULE_AUTHOR("@Large Research");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZINC IO scheduler");
