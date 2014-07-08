#include "thread.h"
#include "conn.h"
#include "network.h"
#include "config.h"
#include "log.h"

#define ITEMS_PER_ALLOC 64

/* Lock for cache operations (item_*, assoc_*) */
pthread_mutex_t cache_lock;

/* Connection lock around accepting new connections */
pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t atomics_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;

static LIBEVENT_DISPATCHER_THREAD dispatcher_thread;

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static LIBEVENT_THREAD *threads;

/*
 * Number of worker threads that have finished setting themselves up.
 */
static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

static void thread_libevent_process(int fd, short which, void *arg);

static inline int mutex_lock(pthread_mutex_t *mutex)
{
    while (pthread_mutex_trylock(mutex));
    return 0;
}

#define mutex_unlock(x) pthread_mutex_unlock(x)


static unsigned short refcount_incr(unsigned short *refcount)
{
#ifdef HAVE_GCC_ATOMICS
    return __sync_add_and_fetch(refcount, 1);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)++;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

static unsigned short refcount_decr(unsigned short *refcount)
{
#ifdef HAVE_GCC_ATOMICS
    return __sync_sub_and_fetch(refcount, 1);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)--;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

static void wait_for_thread_registration(int nthreads)
{
    while (init_count < nthreads)
    {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

static void register_thread_initialized(void)
{
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
}

/*
 * Initializes a connection queue.
 */
static void cq_init(CQ *cq)
{
    pthread_mutex_init(&cq->lock, NULL);
    pthread_cond_init(&cq->cond, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

/*
 * Looks for an item on a connection queue, but doesn't block if there isn't
 * one.
 * Returns the item, or NULL if no item is available
 */
static CQ_ITEM *cq_pop(CQ *cq)
{
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item)
    {
        cq->head = item->next;
        if (NULL == cq->head)
        {
            cq->tail = NULL;
        }
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

/*
 * Adds an item to a connection queue.
 */
static void cq_push(CQ *cq, CQ_ITEM *item)
{
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
    {
        cq->head = item;
    }
    else
    {
        cq->tail->next = item;
    }
    cq->tail = item;
    pthread_cond_signal(&cq->cond);
    pthread_mutex_unlock(&cq->lock);
}

/*
 * Returns a fresh connection queue item.
 */
static CQ_ITEM *cqi_new(void)
{
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist)
    {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

    if (NULL == item)
    {
        int i;

        /* Allocate a bunch of items at once to reduce fragmentation */
        item = (CQ_ITEM *)malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item)
        {
            return NULL;
        }

        /*
         * Link together all the new items except the first one
         * (which we'll return to the caller) for placement on
         * the freelist.
         */
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
        {
            item[i - 1].next = &item[i];
        }

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}

/*
 * Frees a connection queue item (adds it to the freelist.)
 */
static void cqi_free(CQ_ITEM *item)
{
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item;
    pthread_mutex_unlock(&cqi_freelist_lock);
}

/*
 * Creates a worker thread.
 */
static void create_worker(void * (*func)(void *), void *arg)
{
    pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, func, arg)) != 0)
    {
        log_debug(LOG_ERR, "Can't create thread: %s\n",
                  strerror(ret));
        exit(1);
    }
}

/*
 * Sets whether or not we accept new connections.
 */
void accept_new_conns(const bool do_accept)
{
    pthread_mutex_lock(&conn_lock);
    do_accept_new_conns(do_accept);
    pthread_mutex_unlock(&conn_lock);
}
/****************************** LIBEVENT THREADS *****************************/

/*
 * Set up a thread's information.
 */
static void setup_thread(LIBEVENT_THREAD *me)
{
    me->base = event_init();
    if (! me->base)
    {
        log_debug(LOG_ERR, "Can't allocate event base\n");
        exit(1);
    }

    /* Listen for notifications from other threads */
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1)
    {
        log_debug(LOG_ERR, "Can't monitor libevent notify pipe\n");
        exit(1);
    }

    me->new_conn_queue = (conn_queue *)malloc(sizeof(struct conn_queue));
    if (me->new_conn_queue == NULL)
    {
        log_debug(LOG_ERR, "Failed to allocate memory for connection queue, error:%s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);
}

/*
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg)
{
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD *)arg;

    /* Any per-thread setup can happen here; thread_init() will block until
     * all threads have finished initializing.
     */

    register_thread_initialized();

    event_base_loop(me->base, 0);
    return NULL;
}

/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(int fd, short which, void *arg)
{
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD *)arg;
    CQ_ITEM *item;
    char buf[1];

    if (read(fd, buf, 1) != 1)
        if (g_settings.verbose > 0)
        {
            log_debug(LOG_ERR, "Can't read from libevent pipe\n");
        }

    if (buf[0] != 'c')
    {
        return;
    }

    item = cq_pop(me->new_conn_queue);

    if (NULL != item)
    {
        conn *c = conn_new(item->sfd, item->init_state, item->event_flags,
                           item->read_buffer_size, item->transport, me->base);
        if (c == NULL)
        {
            if (g_settings.verbose > 0)
            {
                log_debug(LOG_ERR, "Can't listen for events on fd %d\n",
                          item->sfd);
            }
            close(item->sfd);
        }
        else
        {
            c->thread = me;
        }
        cqi_free(item);
    }

}

/* Which thread we assigned a connection to most recently. */
static int last_thread = -1;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                       int read_buffer_size, enum network_transport transport)
{
    CQ_ITEM *item = cqi_new();
    char buf[1];
    int tid = (last_thread + 1) % g_settings.num_threads;

    LIBEVENT_THREAD *thread = threads + tid;

    last_thread = tid;

    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;
    item->transport = transport;

    cq_push(thread->new_conn_queue, item);

    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1)
    {
        log_debug(LOG_ERR, "Writing to thread notify pipe, error:%s\n", strerror(errno));
    }
}

/*
 * Returns true if this is the thread that listens for new TCP connections.
 */
int is_listen_thread()
{
    return pthread_self() == dispatcher_thread.thread_id;
}

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 * main_base Event base for main thread
 */
void thread_init(int nthreads, struct event_base *main_base)
{
    int         i;
    int         power;

    pthread_mutex_init(&cache_lock, NULL);
    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;

    threads = (LIBEVENT_THREAD *)calloc(nthreads, sizeof(LIBEVENT_THREAD));
    if (! threads)
    {
        log_debug(LOG_ERR, "Can't allocate thread descriptors, error:%s\n", strerror(errno));
        exit(1);
    }

    dispatcher_thread.base = main_base;
    dispatcher_thread.thread_id = pthread_self();

    for (i = 0; i < nthreads; i++)
    {
        int fds[2];
        if (pipe(fds))
        {
            log_debug(LOG_ERR, "Can't create notify pipe, error:%s\n", strerror(errno));
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        setup_thread(&threads[i]);
        /* Reserve three fds for the libevent base, and two for the pipe */
    }

    /* Create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++)
    {
        create_worker(worker_libevent, &threads[i]);
    }

    /* Wait for all the threads to set themselves up before returning. */
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);
}

