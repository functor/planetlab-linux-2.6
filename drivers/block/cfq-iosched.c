/*
 *  linux/drivers/block/cfq-iosched.c
 *
 *  CFQ, or complete fairness queueing, disk scheduler.
 *
 *  Based on ideas from a previously unfinished io
 *  scheduler (round robin per-process disk scheduling) and Andrea Arcangeli.
 *
 *  IO priorities are supported, from 0% to 100% in 5% increments. Both of
 *  those values have special meaning - 0% class is allowed to do io if
 *  noone else wants to use the disk. 100% is considered real-time io, and
 *  always get priority. Default process io rate is 95%. In absence of other
 *  io, a class may consume 100% disk bandwidth regardless. Withing a class,
 *  bandwidth is distributed equally among the citizens.
 *
 * TODO:
 *	- cfq_select_requests() needs some work for 5-95% io
 *	- barriers not supported
 *	- export grace periods in ms, not jiffies
 *
 *  Copyright (C) 2003 Jens Axboe <axboe@suse.de>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/hash.h>
#include <linux/rbtree.h>
#include <linux/mempool.h>

#if IOPRIO_NR > BITS_PER_LONG
#error Cannot support this many io priority levels
#endif

/*
 * tunables
 */
static int cfq_quantum = 6;
static int cfq_quantum_io = 256;
static int cfq_idle_quantum = 1;
static int cfq_idle_quantum_io = 64;
static int cfq_queued = 4;
static int cfq_grace_rt = HZ / 100 ?: 1;
static int cfq_grace_idle = HZ / 10;

#define CFQ_QHASH_SHIFT		6
#define CFQ_QHASH_ENTRIES	(1 << CFQ_QHASH_SHIFT)
#define list_entry_qhash(entry)	hlist_entry((entry), struct cfq_queue, cfq_hash)

#define CFQ_MHASH_SHIFT		8
#define CFQ_MHASH_BLOCK(sec)	((sec) >> 3)
#define CFQ_MHASH_ENTRIES	(1 << CFQ_MHASH_SHIFT)
#define CFQ_MHASH_FN(sec)	(hash_long(CFQ_MHASH_BLOCK((sec)),CFQ_MHASH_SHIFT))
#define rq_hash_key(rq)		((rq)->sector + (rq)->nr_sectors)
#define list_entry_hash(ptr)	hlist_entry((ptr), struct cfq_rq, hash)

#define list_entry_cfqq(ptr)	list_entry((ptr), struct cfq_queue, cfq_list)
#define list_entry_prio(ptr)	list_entry((ptr), struct cfq_rq, prio_list)

#define cfq_account_io(crq)	\
	((crq)->ioprio != IOPRIO_IDLE && (crq)->ioprio != IOPRIO_RT)

/*
 * defines how we distribute bandwidth (can be tgid, uid, etc)
 */

/* FIXME: change hash_key to be sizeof(void *) rather than sizeof(int) 
 * otherwise the cast of cki_tsk_icls will not work reliably on 64-bit arches.
 * OR, change cki_tsk_icls to return ints (will need another id space to be 
 * managed)
 */

#if defined(CONFIG_CKRM_RES_BLKIO) || defined(CONFIG_CKRM_RES_BLKIO_MODULE)
extern inline void *cki_hash_key(struct task_struct *tsk);
extern inline int cki_ioprio(struct task_struct *tsk);
#define cfq_hash_key(current)   ((int)cki_hash_key((current)))
#define cfq_ioprio(current)	(cki_ioprio((current)))

#else
#define cfq_hash_key(current)   ((current)->tgid)
/*
 * move to io_context
 */
#define cfq_ioprio(current)	((current)->ioprio)
#endif

#define CFQ_WAIT_RT	0
#define CFQ_WAIT_NORM	1

static kmem_cache_t *crq_pool;
static kmem_cache_t *cfq_pool;
static mempool_t *cfq_mpool;

/*
 * defines an io priority level
 */
struct io_prio_data {
	struct list_head rr_list;
	int busy_queues;
	int busy_rq;
	unsigned long busy_sectors;
	
	/* Statistics on requests, sectors and queues 
         * added to (in) and dispatched from (out) 
	 * this priority level. Reinsertion of previously
	 * dispatched crq's into cfq's results in double counting
	 * which is ignored for now as in-out should 
	 * still be accurate.
	 */
	atomic_t cum_rq_in,cum_rq_out;              
	atomic_t cum_sectors_in,cum_sectors_out;    
	atomic_t cum_queues_in,cum_queues_out;
      
	struct list_head prio_list;
	int last_rq;
	int last_sectors;
};

/*
 * per-request queue structure
 */
struct cfq_data {
	struct list_head *dispatch;
	struct hlist_head *cfq_hash;
	struct hlist_head *crq_hash;
	mempool_t *crq_pool;

	struct io_prio_data cid[IOPRIO_NR];

	/*
	 * total number of busy queues and requests
	 */
	int busy_rq;
	int busy_queues;
	unsigned long busy_sectors;

	unsigned long rq_starved_mask;

	/*
	 * grace period handling
	 */
	struct timer_list timer;
	unsigned long wait_end;
	unsigned long flags;
	struct work_struct work;

	/*
	 * tunables
	 */
	unsigned int cfq_quantum;
	unsigned int cfq_quantum_io;
	unsigned int cfq_idle_quantum;
	unsigned int cfq_idle_quantum_io;
	unsigned int cfq_queued;
	unsigned int cfq_grace_rt;
	unsigned int cfq_grace_idle;
};

/*
 * per-class structure
 */
struct cfq_queue {
	struct list_head cfq_list;
	struct hlist_node cfq_hash;
	int hash_key;
	struct rb_root sort_list;
	int queued[2];
	int ioprio;
};

/*
 * per-request structure
 */
struct cfq_rq {
	struct cfq_queue *cfq_queue;
	struct rb_node rb_node;
	struct hlist_node hash;
	sector_t rb_key;

	struct request *request;

	struct list_head prio_list;
	unsigned long nr_sectors;
	int ioprio;
};

static void cfq_put_queue(struct cfq_data *cfqd, struct cfq_queue *cfqq);
static struct cfq_queue *cfq_find_cfq_hash(struct cfq_data *cfqd, int pid);
static void cfq_dispatch_sort(struct list_head *head, struct cfq_rq *crq);

/*
 * lots of deadline iosched dupes, can be abstracted later...
 */
static inline void cfq_del_crq_hash(struct cfq_rq *crq)
{
	hlist_del_init(&crq->hash);
}

static inline void
cfq_remove_merge_hints(request_queue_t *q, struct cfq_rq *crq)
{
	cfq_del_crq_hash(crq);

	if (q->last_merge == crq->request)
		q->last_merge = NULL;
}

static inline void cfq_add_crq_hash(struct cfq_data *cfqd, struct cfq_rq *crq)
{
	struct request *rq = crq->request;
	const int hash_idx = CFQ_MHASH_FN(rq_hash_key(rq));

	BUG_ON(!hlist_unhashed(&crq->hash));

	hlist_add_head(&crq->hash, &cfqd->crq_hash[hash_idx]);
}

static struct request *cfq_find_rq_hash(struct cfq_data *cfqd, sector_t offset)
{
	struct hlist_head *hash_list = &cfqd->crq_hash[CFQ_MHASH_FN(offset)];
	struct hlist_node *entry, *next;

	hlist_for_each_safe(entry, next, hash_list) {
		struct cfq_rq *crq = list_entry_hash(entry);
		struct request *__rq = crq->request;

		BUG_ON(hlist_unhashed(&crq->hash));

		if (!rq_mergeable(__rq)) {
			cfq_del_crq_hash(crq);
			continue;
		}

		if (rq_hash_key(__rq) == offset)
			return __rq;
	}

	return NULL;
}

/*
 * rb tree support functions
 */
#define RB_EMPTY(node)		((node)->rb_node == NULL)
#define rb_entry_crq(node)	rb_entry((node), struct cfq_rq, rb_node)
#define rq_rb_key(rq)		(rq)->sector

static void
cfq_del_crq_rb(struct cfq_data *cfqd, struct cfq_queue *cfqq,struct cfq_rq *crq)
{
	if (crq->cfq_queue) {
		crq->cfq_queue = NULL;

		if (cfq_account_io(crq)) {
			cfqd->busy_rq--;
			cfqd->busy_sectors -= crq->nr_sectors;
			cfqd->cid[crq->ioprio].busy_rq--;
			atomic_inc(&(cfqd->cid[crq->ioprio].cum_rq_out));
			cfqd->cid[crq->ioprio].busy_sectors -= crq->nr_sectors;
			atomic_add(crq->nr_sectors,&(cfqd->cid[crq->ioprio].cum_sectors_out));
		}

		cfqq->queued[rq_data_dir(crq->request)]--;
		rb_erase(&crq->rb_node, &cfqq->sort_list);
	}
}

static struct cfq_rq *
__cfq_add_crq_rb(struct cfq_queue *cfqq, struct cfq_rq *crq)
{
	struct rb_node **p = &cfqq->sort_list.rb_node;
	struct rb_node *parent = NULL;
	struct cfq_rq *__crq;

	while (*p) {
		parent = *p;
		__crq = rb_entry_crq(parent);

		if (crq->rb_key < __crq->rb_key)
			p = &(*p)->rb_left;
		else if (crq->rb_key > __crq->rb_key)
			p = &(*p)->rb_right;
		else
			return __crq;
	}

	rb_link_node(&crq->rb_node, parent, p);
	return 0;
}

static void
cfq_add_crq_rb(struct cfq_data *cfqd, struct cfq_queue *cfqq,struct cfq_rq *crq)
{
	struct request *rq = crq->request;
	struct cfq_rq *__alias;

	cfqq->queued[rq_data_dir(rq)]++;
	if (cfq_account_io(crq)) {
		cfqd->busy_rq++;
		cfqd->busy_sectors += crq->nr_sectors;
		cfqd->cid[crq->ioprio].busy_rq++;
		atomic_inc(&(cfqd->cid[crq->ioprio].cum_rq_in));		
		cfqd->cid[crq->ioprio].busy_sectors += crq->nr_sectors;
		atomic_add(crq->nr_sectors,&(cfqd->cid[crq->ioprio].cum_sectors_in));
	}
retry:
	__alias = __cfq_add_crq_rb(cfqq, crq);
	if (!__alias) {
		rb_insert_color(&crq->rb_node, &cfqq->sort_list);
		crq->rb_key = rq_rb_key(rq);
		crq->cfq_queue = cfqq;
		return;
	}

	cfq_del_crq_rb(cfqd, cfqq, __alias);
	cfq_dispatch_sort(cfqd->dispatch, __alias);
	goto retry;
}

static struct request *
cfq_find_rq_rb(struct cfq_data *cfqd, sector_t sector)
{
	struct cfq_queue *cfqq = cfq_find_cfq_hash(cfqd, cfq_hash_key(current));
	struct rb_node *n;

	if (!cfqq)
		goto out;

	n = cfqq->sort_list.rb_node;
	while (n) {
		struct cfq_rq *crq = rb_entry_crq(n);

		if (sector < crq->rb_key)
			n = n->rb_left;
		else if (sector > crq->rb_key)
			n = n->rb_right;
		else
			return crq->request;
	}

out:
	return NULL;
}

static void cfq_remove_request(request_queue_t *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct cfq_rq *crq = RQ_ELV_DATA(rq);

	if (crq) {
		cfq_remove_merge_hints(q, crq);
		list_del_init(&crq->prio_list);
		list_del_init(&rq->queuelist);

		/*
		 * set a grace period timer to allow realtime io to make real
		 * progress, if we release an rt request. for normal request,
		 * set timer so idle io doesn't interfere with other io
		 */
		if (crq->ioprio == IOPRIO_RT) {
			set_bit(CFQ_WAIT_RT, &cfqd->flags);
			cfqd->wait_end = jiffies + cfqd->cfq_grace_rt;
		} else if (crq->ioprio != IOPRIO_IDLE) {
			set_bit(CFQ_WAIT_NORM, &cfqd->flags);
			cfqd->wait_end = jiffies + cfqd->cfq_grace_idle;
		}

		if (crq->cfq_queue) {
			struct cfq_queue *cfqq = crq->cfq_queue;

			cfq_del_crq_rb(cfqd, cfqq, crq);

			if (RB_EMPTY(&cfqq->sort_list))
				cfq_put_queue(cfqd, cfqq);
		}
	}
}

static int
cfq_merge(request_queue_t *q, struct request **req, struct bio *bio)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct request *__rq;
	int ret;

	ret = elv_try_last_merge(q, bio);
	if (ret != ELEVATOR_NO_MERGE) {
		__rq = q->last_merge;
		goto out_insert;
	}

	__rq = cfq_find_rq_hash(cfqd, bio->bi_sector);
	if (__rq) {
		BUG_ON(__rq->sector + __rq->nr_sectors != bio->bi_sector);

		if (elv_rq_merge_ok(__rq, bio)) {
			ret = ELEVATOR_BACK_MERGE;
			goto out;
		}
	}

	__rq = cfq_find_rq_rb(cfqd, bio->bi_sector + bio_sectors(bio));
	if (__rq) {
		if (elv_rq_merge_ok(__rq, bio)) {
			ret = ELEVATOR_FRONT_MERGE;
			goto out;
		}
	}

	return ELEVATOR_NO_MERGE;
out:
	q->last_merge = __rq;
out_insert:
	*req = __rq;
	return ret;
}

static void cfq_merged_request(request_queue_t *q, struct request *req)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct cfq_rq *crq = RQ_ELV_DATA(req);

	cfq_del_crq_hash(crq);
	cfq_add_crq_hash(cfqd, crq);

	if (crq->cfq_queue && (rq_rb_key(req) != crq->rb_key)) {
		struct cfq_queue *cfqq = crq->cfq_queue;

		cfq_del_crq_rb(cfqd, cfqq, crq);
		cfq_add_crq_rb(cfqd, cfqq, crq);
	}

	cfqd->busy_sectors += req->hard_nr_sectors - crq->nr_sectors;
	cfqd->cid[crq->ioprio].busy_sectors += req->hard_nr_sectors - crq->nr_sectors;
	crq->nr_sectors = req->hard_nr_sectors;

	q->last_merge = req;
}

static void
cfq_merged_requests(request_queue_t *q, struct request *req,
		    struct request *next)
{
	cfq_merged_request(q, req);
	cfq_remove_request(q, next);
}

/*
 * sort into dispatch list, in optimal ascending order
 */
static void cfq_dispatch_sort(struct list_head *head, struct cfq_rq *crq)
{
	struct list_head *entry = head;
	struct request *__rq;

	if (!list_empty(head)) {
		__rq = list_entry_rq(head->next);

		if (crq->request->sector < __rq->sector) {
			entry = head->prev;
			goto link;
		}
	}

	while ((entry = entry->prev) != head) {
		__rq = list_entry_rq(entry);

		if (crq->request->sector <= __rq->sector)
			break;
	}

link:
	list_add_tail(&crq->request->queuelist, entry);
}

/*
 * remove from io scheduler core and put on dispatch list for service
 */
static inline int
__cfq_dispatch_requests(request_queue_t *q, struct cfq_data *cfqd,
			struct cfq_queue *cfqq)
{
	struct cfq_rq *crq;

	crq = rb_entry_crq(rb_first(&cfqq->sort_list));

	cfq_del_crq_rb(cfqd, cfqq, crq);
	cfq_remove_merge_hints(q, crq);
	cfq_dispatch_sort(cfqd->dispatch, crq);

	/*
	 * technically, for IOPRIO_RT we don't need to add it to the list.
	 */
	list_add_tail(&crq->prio_list, &cfqd->cid[cfqq->ioprio].prio_list);
	return crq->nr_sectors;
}

static int
cfq_dispatch_requests(request_queue_t *q, int prio, int max_rq, int max_sectors)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct list_head *plist = &cfqd->cid[prio].rr_list;
	struct list_head *entry, *nxt;
	int q_rq, q_io;

	/*
	 * for each queue at this prio level, dispatch a request
	 */
	q_rq = q_io = 0;
	list_for_each_safe(entry, nxt, plist) {
		struct cfq_queue *cfqq = list_entry_cfqq(entry);

		BUG_ON(RB_EMPTY(&cfqq->sort_list));

		q_io += __cfq_dispatch_requests(q, cfqd, cfqq);
		q_rq++;

		if (RB_EMPTY(&cfqq->sort_list))
			cfq_put_queue(cfqd, cfqq);

		/*
		 * if we hit the queue limit, put the string of serviced
		 * queues at the back of the pending list
		 */
		if (q_io >= max_sectors || q_rq >= max_rq) {
			struct list_head *prv = nxt->prev;

			if (prv != plist) {
				list_del(plist);
				list_add(plist, prv);
			}
			break;
		}
	}

	cfqd->cid[prio].last_rq = q_rq;
	cfqd->cid[prio].last_sectors = q_io;
	return q_rq;
}

/*
 * try to move some requests to the dispatch list. return 0 on success
 */
static int cfq_select_requests(request_queue_t *q, struct cfq_data *cfqd)
{
	int queued, busy_rq, busy_sectors, i;

	/*
	 * if there's any realtime io, only schedule that
	 */
	if (cfq_dispatch_requests(q, IOPRIO_RT, cfqd->cfq_quantum, cfqd->cfq_quantum_io))
		return 1;

	/*
	 * if RT io was last serviced and grace time hasn't expired,
	 * arm the timer to restart queueing if no other RT io has been
	 * submitted in the mean time
	 */
	if (test_bit(CFQ_WAIT_RT, &cfqd->flags)) {
		if (time_before(jiffies, cfqd->wait_end)) {
			mod_timer(&cfqd->timer, cfqd->wait_end);
			return 0;
		}
		clear_bit(CFQ_WAIT_RT, &cfqd->flags);
	}

	/*
	 * for each priority level, calculate number of requests we
	 * are allowed to put into service.
	 */
	queued = 0;
	busy_rq = cfqd->busy_rq;
	busy_sectors = cfqd->busy_sectors;
	for (i = IOPRIO_RT - 1; i > IOPRIO_IDLE; i--) {
		const int o_rq = busy_rq - cfqd->cid[i].busy_rq;
		const int o_sectors = busy_sectors - cfqd->cid[i].busy_sectors;
		int q_rq = cfqd->cfq_quantum * (i + 1) / IOPRIO_NR;
		int q_io = cfqd->cfq_quantum_io * (i + 1) / IOPRIO_NR;

		/*
		 * no need to keep iterating the list, if there are no
		 * requests pending anymore
		 */
		if (!cfqd->busy_rq)
			break;

		/*
		 * find out how many requests and sectors we are allowed to
		 * service
		 */
		if (o_rq)
			q_rq = o_sectors * (i + 1) / IOPRIO_NR;
		if (q_rq > cfqd->cfq_quantum)
			q_rq = cfqd->cfq_quantum;

		if (o_sectors)
			q_io = o_sectors * (i + 1) / IOPRIO_NR;
		if (q_io > cfqd->cfq_quantum_io)
			q_io = cfqd->cfq_quantum_io;

		/*
		 * average with last dispatched for fairness
		 */
		if (cfqd->cid[i].last_rq != -1)
			q_rq = (cfqd->cid[i].last_rq + q_rq) / 2;
		if (cfqd->cid[i].last_sectors != -1)
			q_io = (cfqd->cid[i].last_sectors + q_io) / 2;

		queued += cfq_dispatch_requests(q, i, q_rq, q_io);
	}

	if (queued)
		return 1;

	/*
	 * only allow dispatch of idle io, if the queue has been idle from
	 * servicing RT or normal io for the grace period
	 */
	if (test_bit(CFQ_WAIT_NORM, &cfqd->flags)) {
		if (time_before(jiffies, cfqd->wait_end)) {
			mod_timer(&cfqd->timer, cfqd->wait_end);
			return 0;
		}
		clear_bit(CFQ_WAIT_NORM, &cfqd->flags);
	}

	/*
	 * if we found nothing to do, allow idle io to be serviced
	 */
	if (cfq_dispatch_requests(q, IOPRIO_IDLE, cfqd->cfq_idle_quantum, cfqd->cfq_idle_quantum_io))
		return 1;

	return 0;
}

static struct request *cfq_next_request(request_queue_t *q)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct request *rq;

	if (!list_empty(cfqd->dispatch)) {
		struct cfq_rq *crq;
dispatch:
		/*
		 * end grace period, we are servicing a request
		 */
		del_timer(&cfqd->timer);
		clear_bit(CFQ_WAIT_RT, &cfqd->flags);
		clear_bit(CFQ_WAIT_NORM, &cfqd->flags);

		BUG_ON(list_empty(cfqd->dispatch));
		rq = list_entry_rq(cfqd->dispatch->next);

		BUG_ON(q->last_merge == rq);
		crq = RQ_ELV_DATA(rq);
		if (crq) {
			BUG_ON(!hlist_unhashed(&crq->hash));
			list_del_init(&crq->prio_list);
		}

		return rq;
	}

	/*
	 * we moved requests to dispatch list, go back end serve one
	 */
	if (cfq_select_requests(q, cfqd))
		goto dispatch;

	return NULL;
}

static inline struct cfq_queue *
__cfq_find_cfq_hash(struct cfq_data *cfqd, int hashkey, const int hashval)
{
	struct hlist_head *hash_list = &cfqd->cfq_hash[hashval];
	struct hlist_node *entry;

	hlist_for_each(entry, hash_list) {
		struct cfq_queue *__cfqq = list_entry_qhash(entry);

		if (__cfqq->hash_key == hashkey)
			return __cfqq;
	}

	return NULL;
}

static struct cfq_queue *cfq_find_cfq_hash(struct cfq_data *cfqd, int hashkey)
{
	const int hashval = hash_long(hashkey, CFQ_QHASH_SHIFT);

	return __cfq_find_cfq_hash(cfqd, hashkey, hashval);
}

static void cfq_put_queue(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	cfqd->busy_queues--;
	WARN_ON(cfqd->busy_queues < 0);

	cfqd->cid[cfqq->ioprio].busy_queues--;
	WARN_ON(cfqd->cid[cfqq->ioprio].busy_queues < 0);
	atomic_inc(&(cfqd->cid[cfqq->ioprio].cum_queues_out));

	list_del(&cfqq->cfq_list);
	hlist_del(&cfqq->cfq_hash);
	mempool_free(cfqq, cfq_mpool);
}

static struct cfq_queue *cfq_get_queue(struct cfq_data *cfqd, int hashkey)
{
	const int hashval = hash_long(hashkey, CFQ_QHASH_SHIFT);
	struct cfq_queue *cfqq, *new_cfqq = NULL;

 retry:
	cfqq = __cfq_find_cfq_hash(cfqd, hashkey, hashval);

	if (!cfqq) {
	        if (new_cfqq) {
		       cfqq = new_cfqq;
		       new_cfqq = NULL;
		} else {
		  new_cfqq = mempool_alloc(cfq_mpool, GFP_ATOMIC);
		  /* MEF: I think cfq-iosched.c needs further fixing
		     to avoid the bugon. Shailabh will be sending
		     a new patch for this soon.
		  */
		  BUG_ON(new_cfqq == NULL);
		  goto retry;
		}
		
		memset(cfqq, 0, sizeof(*cfqq));
		INIT_HLIST_NODE(&cfqq->cfq_hash);
		INIT_LIST_HEAD(&cfqq->cfq_list);

		cfqq->hash_key = cfq_hash_key(current);
		cfqq->ioprio = cfq_ioprio(current);
		hlist_add_head(&cfqq->cfq_hash, &cfqd->cfq_hash[hashval]);
	}

	if (new_cfqq) {
	        mempool_free(new_cfqq, cfq_mpool);
	}

	return cfqq;
}

static void
__cfq_enqueue(request_queue_t *q, struct cfq_data *cfqd, struct cfq_rq *crq)
{
	const int prio = crq->ioprio;
	struct cfq_queue *cfqq;

	cfqq = cfq_get_queue(cfqd, cfq_hash_key(current));

	/*
	 * not too good...
	 */
	if (prio > cfqq->ioprio) {
		printk("prio hash collision %d %d\n", prio, cfqq->ioprio);
		if (!list_empty(&cfqq->cfq_list)) {
			cfqd->cid[cfqq->ioprio].busy_queues--;
			WARN_ON(cfqd->cid[cfqq->ioprio].busy_queues < 0);
			atomic_inc(&(cfqd->cid[cfqq->ioprio].cum_queues_out));
			cfqd->cid[prio].busy_queues++;
			atomic_inc(&(cfqd->cid[prio].cum_queues_in));
			list_move_tail(&cfqq->cfq_list, &cfqd->cid[prio].rr_list);
		}
		cfqq->ioprio = prio;
	}

	cfq_add_crq_rb(cfqd, cfqq, crq);

	if (list_empty(&cfqq->cfq_list)) {
		list_add_tail(&cfqq->cfq_list, &cfqd->cid[prio].rr_list);
		cfqd->cid[prio].busy_queues++;
		atomic_inc(&(cfqd->cid[prio].cum_queues_in));
		cfqd->busy_queues++;
	}

	if (rq_mergeable(crq->request)) {
		cfq_add_crq_hash(cfqd, crq);

		if (!q->last_merge)
			q->last_merge = crq->request;
	}

}

static void cfq_reenqueue(request_queue_t *q, struct cfq_data *cfqd, int prio)
{
	struct list_head *prio_list = &cfqd->cid[prio].prio_list;
	struct list_head *entry, *tmp;

	list_for_each_safe(entry, tmp, prio_list) {
		struct cfq_rq *crq = list_entry_prio(entry);

		list_del_init(entry);
		list_del_init(&crq->request->queuelist);
		__cfq_enqueue(q, cfqd, crq);
	}
}

static void
cfq_enqueue(request_queue_t *q, struct cfq_data *cfqd, struct cfq_rq *crq)
{
	const int prio = cfq_ioprio(current);

	crq->ioprio = prio;
	crq->nr_sectors = crq->request->hard_nr_sectors;
	__cfq_enqueue(q, cfqd, crq);

	if (prio == IOPRIO_RT) {
		int i;

		/*
		 * realtime io gets priority, move all other io back
		 */
		for (i = IOPRIO_IDLE; i < IOPRIO_RT; i++)
			cfq_reenqueue(q, cfqd, i);
	} else if (prio != IOPRIO_IDLE) {
		/*
		 * check if we need to move idle io back into queue
		 */
		cfq_reenqueue(q, cfqd, IOPRIO_IDLE);
	}
}

static void
cfq_insert_request(request_queue_t *q, struct request *rq, int where)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct cfq_rq *crq = RQ_ELV_DATA(rq);

	switch (where) {
		case ELEVATOR_INSERT_BACK:
#if 0
			while (cfq_dispatch_requests(q, cfqd))
				;
#endif
			list_add_tail(&rq->queuelist, cfqd->dispatch);
			break;
		case ELEVATOR_INSERT_FRONT:
			list_add(&rq->queuelist, cfqd->dispatch);
			break;
		case ELEVATOR_INSERT_SORT:
			BUG_ON(!blk_fs_request(rq));
			cfq_enqueue(q, cfqd, crq);
			break;
		default:
			printk("%s: bad insert point %d\n", __FUNCTION__,where);
			return;
	}
}

static int cfq_queue_empty(request_queue_t *q)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;

	if (list_empty(cfqd->dispatch) && !cfqd->busy_queues)
		return 1;

	return 0;
}

static struct request *
cfq_former_request(request_queue_t *q, struct request *rq)
{
	struct cfq_rq *crq = RQ_ELV_DATA(rq);
	struct rb_node *rbprev = rb_prev(&crq->rb_node);

	if (rbprev)
		return rb_entry_crq(rbprev)->request;

	return NULL;
}

static struct request *
cfq_latter_request(request_queue_t *q, struct request *rq)
{
	struct cfq_rq *crq = RQ_ELV_DATA(rq);
	struct rb_node *rbnext = rb_next(&crq->rb_node);

	if (rbnext)
		return rb_entry_crq(rbnext)->request;

	return NULL;
}

static void cfq_queue_congested(request_queue_t *q)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;

	set_bit(cfq_ioprio(current), &cfqd->rq_starved_mask);
}

static int cfq_may_queue(request_queue_t *q, int rw)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct cfq_queue *cfqq;
	const int prio = cfq_ioprio(current);
	int limit, ret = 1;

	if (!cfqd->busy_queues)
		goto out;

	cfqq = cfq_find_cfq_hash(cfqd, cfq_hash_key(current));
	if (!cfqq)
		goto out;

	cfqq = cfq_find_cfq_hash(cfqd, cfq_hash_key(current));
	if (!cfqq)
		goto out;

	/*
	 * if higher or equal prio io is sleeping waiting for a request, don't
	 * allow this one to allocate one. as long as ll_rw_blk does fifo
	 * waitqueue wakeups this should work...
	 */
	if (cfqd->rq_starved_mask & ~((1 << prio) - 1))
		goto out;

	if (cfqq->queued[rw] < cfqd->cfq_queued || !cfqd->cid[prio].busy_queues)
		goto out;

	limit = q->nr_requests * (prio + 1) / IOPRIO_NR;
	limit /= cfqd->cid[prio].busy_queues;
	if (cfqq->queued[rw] > limit)
		ret = 0;

out:
	return ret;
}

static void cfq_put_request(request_queue_t *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct cfq_rq *crq = RQ_ELV_DATA(rq);

	if (crq) {
		BUG_ON(q->last_merge == rq);
		BUG_ON(!hlist_unhashed(&crq->hash));

		mempool_free(crq, cfqd->crq_pool);
		rq->elevator_private = NULL;
	}
}

static int cfq_set_request(request_queue_t *q, struct request *rq, int gfp_mask)
{
	struct cfq_data *cfqd = q->elevator.elevator_data;
	struct cfq_rq *crq = mempool_alloc(cfqd->crq_pool, gfp_mask);

	if (crq) {
		/*
		 * process now has one request
		 */
		clear_bit(cfq_ioprio(current), &cfqd->rq_starved_mask);

		memset(crq, 0, sizeof(*crq));
		crq->request = rq;
		INIT_HLIST_NODE(&crq->hash);
		INIT_LIST_HEAD(&crq->prio_list);
		rq->elevator_private = crq;
		return 0;
	}

	return 1;
}

static void cfq_exit(request_queue_t *q, elevator_t *e)
{
	struct cfq_data *cfqd = e->elevator_data;

	e->elevator_data = NULL;
	mempool_destroy(cfqd->crq_pool);
	kfree(cfqd->crq_hash);
	kfree(cfqd->cfq_hash);
	kfree(cfqd);
}

static void cfq_timer(unsigned long data)
{
	struct cfq_data *cfqd = (struct cfq_data *) data;

	clear_bit(CFQ_WAIT_RT, &cfqd->flags);
	clear_bit(CFQ_WAIT_NORM, &cfqd->flags);
	kblockd_schedule_work(&cfqd->work);
}

static void cfq_work(void *data)
{
	request_queue_t *q = data;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	if (cfq_next_request(q))
		q->request_fn(q);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

static int cfq_init(request_queue_t *q, elevator_t *e)
{
	struct cfq_data *cfqd;
	int i;

	cfqd = kmalloc(sizeof(*cfqd), GFP_KERNEL);
	if (!cfqd)
		return -ENOMEM;

	memset(cfqd, 0, sizeof(*cfqd));

	init_timer(&cfqd->timer);
	cfqd->timer.function = cfq_timer;
	cfqd->timer.data = (unsigned long) cfqd;

	INIT_WORK(&cfqd->work, cfq_work, q);

	for (i = 0; i < IOPRIO_NR; i++) {
		struct io_prio_data *cid = &cfqd->cid[i];

		INIT_LIST_HEAD(&cid->rr_list);
		INIT_LIST_HEAD(&cid->prio_list);
		cid->last_rq = -1;
		cid->last_sectors = -1;

		atomic_set(&cid->cum_rq_in,0);		
		atomic_set(&cid->cum_rq_out,0);
		atomic_set(&cid->cum_sectors_in,0);
		atomic_set(&cid->cum_sectors_out,0);		
		atomic_set(&cid->cum_queues_in,0);
		atomic_set(&cid->cum_queues_out,0);
	}

	cfqd->crq_hash = kmalloc(sizeof(struct hlist_head) * CFQ_MHASH_ENTRIES, GFP_KERNEL);
	if (!cfqd->crq_hash)
		goto out_crqhash;

	cfqd->cfq_hash = kmalloc(sizeof(struct hlist_head) * CFQ_QHASH_ENTRIES, GFP_KERNEL);
	if (!cfqd->cfq_hash)
		goto out_cfqhash;

	cfqd->crq_pool = mempool_create(BLKDEV_MIN_RQ, mempool_alloc_slab, mempool_free_slab, crq_pool);
	if (!cfqd->crq_pool)
		goto out_crqpool;

	for (i = 0; i < CFQ_MHASH_ENTRIES; i++)
		INIT_HLIST_HEAD(&cfqd->crq_hash[i]);
	for (i = 0; i < CFQ_QHASH_ENTRIES; i++)
		INIT_HLIST_HEAD(&cfqd->cfq_hash[i]);

	cfqd->cfq_queued = cfq_queued;
	cfqd->cfq_quantum = cfq_quantum;
	cfqd->cfq_quantum_io = cfq_quantum_io;
	cfqd->cfq_idle_quantum = cfq_idle_quantum;
	cfqd->cfq_idle_quantum_io = cfq_idle_quantum_io;
	cfqd->cfq_grace_rt = cfq_grace_rt;
	cfqd->cfq_grace_idle = cfq_grace_idle;

	q->nr_requests <<= 2;

	cfqd->dispatch = &q->queue_head;
	e->elevator_data = cfqd;

	return 0;
out_crqpool:
	kfree(cfqd->cfq_hash);
out_cfqhash:
	kfree(cfqd->crq_hash);
out_crqhash:
	kfree(cfqd);
	return -ENOMEM;
}

static int __init cfq_slab_setup(void)
{
	crq_pool = kmem_cache_create("crq_pool", sizeof(struct cfq_rq), 0, 0,
					NULL, NULL);

	if (!crq_pool)
		panic("cfq_iosched: can't init crq pool\n");

	cfq_pool = kmem_cache_create("cfq_pool", sizeof(struct cfq_queue), 0, 0,
					NULL, NULL);

	if (!cfq_pool)
		panic("cfq_iosched: can't init cfq pool\n");

	cfq_mpool = mempool_create(64, mempool_alloc_slab, mempool_free_slab, cfq_pool);

	if (!cfq_mpool)
		panic("cfq_iosched: can't init cfq mpool\n");

	return 0;
}

subsys_initcall(cfq_slab_setup);

/*
 * sysfs parts below -->
 */
struct cfq_fs_entry {
	struct attribute attr;
	ssize_t (*show)(struct cfq_data *, char *);
	ssize_t (*store)(struct cfq_data *, const char *, size_t);
};

static ssize_t
cfq_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
cfq_var_store(unsigned int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtoul(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR)					\
static ssize_t __FUNC(struct cfq_data *cfqd, char *page)		\
{									\
	return cfq_var_show(__VAR, (page));				\
}
SHOW_FUNCTION(cfq_quantum_show, cfqd->cfq_quantum);
SHOW_FUNCTION(cfq_quantum_io_show, cfqd->cfq_quantum_io);
SHOW_FUNCTION(cfq_idle_quantum_show, cfqd->cfq_idle_quantum);
SHOW_FUNCTION(cfq_idle_quantum_io_show, cfqd->cfq_idle_quantum_io);
SHOW_FUNCTION(cfq_queued_show, cfqd->cfq_queued);
SHOW_FUNCTION(cfq_grace_rt_show, cfqd->cfq_grace_rt);
SHOW_FUNCTION(cfq_grace_idle_show, cfqd->cfq_grace_idle);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX)				\
static ssize_t __FUNC(struct cfq_data *cfqd, const char *page, size_t count)	\
{									\
	int ret = cfq_var_store(__PTR, (page), count);			\
	if (*(__PTR) < (MIN))						\
		*(__PTR) = (MIN);					\
	else if (*(__PTR) > (MAX))					\
		*(__PTR) = (MAX);					\
	return ret;							\
}
STORE_FUNCTION(cfq_quantum_store, &cfqd->cfq_quantum, 1, INT_MAX);
STORE_FUNCTION(cfq_quantum_io_store, &cfqd->cfq_quantum_io, 4, INT_MAX);
STORE_FUNCTION(cfq_idle_quantum_store, &cfqd->cfq_idle_quantum, 1, INT_MAX);
STORE_FUNCTION(cfq_idle_quantum_io_store, &cfqd->cfq_idle_quantum_io, 4, INT_MAX);
STORE_FUNCTION(cfq_queued_store, &cfqd->cfq_queued, 1, INT_MAX);
STORE_FUNCTION(cfq_grace_rt_store, &cfqd->cfq_grace_rt, 0, INT_MAX);
STORE_FUNCTION(cfq_grace_idle_store, &cfqd->cfq_grace_idle, 0, INT_MAX);
#undef STORE_FUNCTION


/* Additional entries to get priority level data */
static ssize_t
cfq_prio_show(struct cfq_data *cfqd, char *page, unsigned int priolvl)
{
	int r1,r2,s1,s2,q1,q2;

	if (!(priolvl >= IOPRIO_IDLE && priolvl <= IOPRIO_RT)) 
		return 0;
	
	r1 = (int)atomic_read(&(cfqd->cid[priolvl].cum_rq_in));
	r2 = (int)atomic_read(&(cfqd->cid[priolvl].cum_rq_out));
	s1 = (int)atomic_read(&(cfqd->cid[priolvl].cum_sectors_in));
	s2 = (int)atomic_read(&(cfqd->cid[priolvl].cum_sectors_out));
	q1 = (int)atomic_read(&(cfqd->cid[priolvl].cum_queues_in)); 
	q2 = (int)atomic_read(&(cfqd->cid[priolvl].cum_queues_out));
	

	/*
	  return sprintf(page,"rq %d (%d,%d) sec %d (%d,%d) q %d (%d,%d)\n",
		      r1-r2,r1,r2,
		      s1-s2,s1,s2,
		      q1-q2,q1,q2);
	*/

	return sprintf(page,"rq (%d,%d) sec (%d,%d) q (%d,%d)\n",
		      r1,r2,
		      s1,s2,
		      q1,q2);

}

#define SHOW_PRIO_DATA(__PRIOLVL)                                               \
static ssize_t cfq_prio_##__PRIOLVL##_show(struct cfq_data *cfqd, char *page)	\
{									        \
	return cfq_prio_show(cfqd,page,__PRIOLVL);				\
}
SHOW_PRIO_DATA(0);
SHOW_PRIO_DATA(1);
SHOW_PRIO_DATA(2);
SHOW_PRIO_DATA(3);
SHOW_PRIO_DATA(4);
SHOW_PRIO_DATA(5);
SHOW_PRIO_DATA(6);
SHOW_PRIO_DATA(7);
SHOW_PRIO_DATA(8);
SHOW_PRIO_DATA(9);
SHOW_PRIO_DATA(10);
SHOW_PRIO_DATA(11);
SHOW_PRIO_DATA(12);
SHOW_PRIO_DATA(13);
SHOW_PRIO_DATA(14);
SHOW_PRIO_DATA(15);
SHOW_PRIO_DATA(16);
SHOW_PRIO_DATA(17);
SHOW_PRIO_DATA(18);
SHOW_PRIO_DATA(19);
SHOW_PRIO_DATA(20);
#undef SHOW_PRIO_DATA


static ssize_t cfq_prio_store(struct cfq_data *cfqd, const char *page, size_t count, int priolvl)
{	
	atomic_set(&(cfqd->cid[priolvl].cum_rq_in),0);
	atomic_set(&(cfqd->cid[priolvl].cum_rq_out),0);
	atomic_set(&(cfqd->cid[priolvl].cum_sectors_in),0);
	atomic_set(&(cfqd->cid[priolvl].cum_sectors_out),0);
	atomic_set(&(cfqd->cid[priolvl].cum_queues_in),0);
	atomic_set(&(cfqd->cid[priolvl].cum_queues_out),0);

	return count;
}


#define STORE_PRIO_DATA(__PRIOLVL)				                                   \
static ssize_t cfq_prio_##__PRIOLVL##_store(struct cfq_data *cfqd, const char *page, size_t count) \
{									                           \
        return cfq_prio_store(cfqd,page,count,__PRIOLVL);                                          \
}                  
STORE_PRIO_DATA(0);     
STORE_PRIO_DATA(1);
STORE_PRIO_DATA(2);
STORE_PRIO_DATA(3);
STORE_PRIO_DATA(4);
STORE_PRIO_DATA(5);
STORE_PRIO_DATA(6);
STORE_PRIO_DATA(7);
STORE_PRIO_DATA(8);
STORE_PRIO_DATA(9);
STORE_PRIO_DATA(10);
STORE_PRIO_DATA(11);
STORE_PRIO_DATA(12);
STORE_PRIO_DATA(13);
STORE_PRIO_DATA(14);
STORE_PRIO_DATA(15);
STORE_PRIO_DATA(16);
STORE_PRIO_DATA(17);
STORE_PRIO_DATA(18);
STORE_PRIO_DATA(19);
STORE_PRIO_DATA(20);
#undef STORE_PRIO_DATA



static struct cfq_fs_entry cfq_quantum_entry = {
	.attr = {.name = "quantum", .mode = S_IRUGO | S_IWUSR },
	.show = cfq_quantum_show,
	.store = cfq_quantum_store,
};
static struct cfq_fs_entry cfq_quantum_io_entry = {
	.attr = {.name = "quantum_io", .mode = S_IRUGO | S_IWUSR },
	.show = cfq_quantum_io_show,
	.store = cfq_quantum_io_store,
};
static struct cfq_fs_entry cfq_idle_quantum_entry = {
	.attr = {.name = "idle_quantum", .mode = S_IRUGO | S_IWUSR },
	.show = cfq_idle_quantum_show,
	.store = cfq_idle_quantum_store,
};
static struct cfq_fs_entry cfq_idle_quantum_io_entry = {
	.attr = {.name = "idle_quantum_io", .mode = S_IRUGO | S_IWUSR },
	.show = cfq_idle_quantum_io_show,
	.store = cfq_idle_quantum_io_store,
};
static struct cfq_fs_entry cfq_queued_entry = {
	.attr = {.name = "queued", .mode = S_IRUGO | S_IWUSR },
	.show = cfq_queued_show,
	.store = cfq_queued_store,
};
static struct cfq_fs_entry cfq_grace_rt_entry = {
	.attr = {.name = "grace_rt", .mode = S_IRUGO | S_IWUSR },
	.show = cfq_grace_rt_show,
	.store = cfq_grace_rt_store,
};
static struct cfq_fs_entry cfq_grace_idle_entry = {
	.attr = {.name = "grace_idle", .mode = S_IRUGO | S_IWUSR },
	.show = cfq_grace_idle_show,
	.store = cfq_grace_idle_store,
};

#define P_0_STR   "p0"
#define P_1_STR   "p1"
#define P_2_STR   "p2"
#define P_3_STR   "p3"
#define P_4_STR   "p4"
#define P_5_STR   "p5"
#define P_6_STR   "p6"
#define P_7_STR   "p7"
#define P_8_STR   "p8"
#define P_9_STR   "p9"
#define P_10_STR  "p10"
#define P_11_STR  "p11"
#define P_12_STR  "p12"
#define P_13_STR  "p13"
#define P_14_STR  "p14"
#define P_15_STR  "p15"
#define P_16_STR  "p16"
#define P_17_STR  "p17"
#define P_18_STR  "p18"
#define P_19_STR  "p19"
#define P_20_STR  "p20"


#define CFQ_PRIO_SYSFS_ENTRY(__PRIOLVL)				           \
static struct cfq_fs_entry cfq_prio_##__PRIOLVL##_entry = {                \
	.attr = {.name = P_##__PRIOLVL##_STR, .mode = S_IRUGO | S_IWUSR }, \
	.show = cfq_prio_##__PRIOLVL##_show,                               \
	.store = cfq_prio_##__PRIOLVL##_store,                             \
};
CFQ_PRIO_SYSFS_ENTRY(0);
CFQ_PRIO_SYSFS_ENTRY(1);
CFQ_PRIO_SYSFS_ENTRY(2);
CFQ_PRIO_SYSFS_ENTRY(3);
CFQ_PRIO_SYSFS_ENTRY(4);
CFQ_PRIO_SYSFS_ENTRY(5);
CFQ_PRIO_SYSFS_ENTRY(6);
CFQ_PRIO_SYSFS_ENTRY(7);
CFQ_PRIO_SYSFS_ENTRY(8);
CFQ_PRIO_SYSFS_ENTRY(9);
CFQ_PRIO_SYSFS_ENTRY(10);
CFQ_PRIO_SYSFS_ENTRY(11);
CFQ_PRIO_SYSFS_ENTRY(12);
CFQ_PRIO_SYSFS_ENTRY(13);
CFQ_PRIO_SYSFS_ENTRY(14);
CFQ_PRIO_SYSFS_ENTRY(15);
CFQ_PRIO_SYSFS_ENTRY(16);
CFQ_PRIO_SYSFS_ENTRY(17);
CFQ_PRIO_SYSFS_ENTRY(18);
CFQ_PRIO_SYSFS_ENTRY(19);
CFQ_PRIO_SYSFS_ENTRY(20);
#undef CFQ_PRIO_SYSFS_ENTRY


static struct attribute *default_attrs[] = {
	&cfq_quantum_entry.attr,
	&cfq_quantum_io_entry.attr,
	&cfq_idle_quantum_entry.attr,
	&cfq_idle_quantum_io_entry.attr,
	&cfq_queued_entry.attr,
	&cfq_grace_rt_entry.attr,
	&cfq_grace_idle_entry.attr,
	&cfq_prio_0_entry.attr,
	&cfq_prio_1_entry.attr,
	&cfq_prio_2_entry.attr,
	&cfq_prio_3_entry.attr,
	&cfq_prio_4_entry.attr,
	&cfq_prio_5_entry.attr,
	&cfq_prio_6_entry.attr,
	&cfq_prio_7_entry.attr,
	&cfq_prio_8_entry.attr,
	&cfq_prio_9_entry.attr,
	&cfq_prio_10_entry.attr,
	&cfq_prio_11_entry.attr,
	&cfq_prio_12_entry.attr,
	&cfq_prio_13_entry.attr,
	&cfq_prio_14_entry.attr,
	&cfq_prio_15_entry.attr,
	&cfq_prio_16_entry.attr,
	&cfq_prio_17_entry.attr,
	&cfq_prio_18_entry.attr,
	&cfq_prio_19_entry.attr,
	&cfq_prio_20_entry.attr,
	NULL,
};

#define to_cfq(atr) container_of((atr), struct cfq_fs_entry, attr)

static ssize_t
cfq_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct cfq_fs_entry *entry = to_cfq(attr);

	if (!entry->show)
		return 0;

	return entry->show(e->elevator_data, page);
}

static ssize_t
cfq_attr_store(struct kobject *kobj, struct attribute *attr,
	       const char *page, size_t length)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct cfq_fs_entry *entry = to_cfq(attr);

	if (!entry->store)
		return -EINVAL;

	return entry->store(e->elevator_data, page, length);
}

static struct sysfs_ops cfq_sysfs_ops = {
	.show	= cfq_attr_show,
	.store	= cfq_attr_store,
};

struct kobj_type cfq_ktype = {
	.sysfs_ops	= &cfq_sysfs_ops,
	.default_attrs	= default_attrs,
};

elevator_t iosched_cfq = {
	.elevator_name =		"cfq",
	.elevator_ktype =		&cfq_ktype,
	.elevator_merge_fn = 		cfq_merge,
	.elevator_merged_fn =		cfq_merged_request,
	.elevator_merge_req_fn =	cfq_merged_requests,
	.elevator_next_req_fn =		cfq_next_request,
	.elevator_add_req_fn =		cfq_insert_request,
	.elevator_remove_req_fn =	cfq_remove_request,
	.elevator_queue_empty_fn =	cfq_queue_empty,
	.elevator_former_req_fn =	cfq_former_request,
	.elevator_latter_req_fn =	cfq_latter_request,
	.elevator_set_req_fn =		cfq_set_request,
	.elevator_put_req_fn =		cfq_put_request,
	.elevator_may_queue_fn =	cfq_may_queue,
	.elevator_set_congested_fn =	cfq_queue_congested,
	.elevator_init_fn =		cfq_init,
	.elevator_exit_fn =		cfq_exit,
};

EXPORT_SYMBOL(iosched_cfq);
