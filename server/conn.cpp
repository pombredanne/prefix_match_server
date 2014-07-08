#include "conn.h"
#include "config.h"
#include "network.h"
#include "config.h"
#include "log.h"

/*
 * Free list management for connections.
 */
static conn **freeconns;
static int freetotal;
static int freecurr;
/* Lock for connection freelist */
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

extern struct settings g_settings;

void conn_init(void)
{
    freetotal = 200;
    freecurr = 0;
    if ((freeconns = (conn **)calloc(freetotal, sizeof(conn *))) == NULL)
    {
        log_debug(LOG_ERR, "Failed to allocate connection structures\n");
    }
    return;
}

/*
 * Returns a connection from the freelist, if any.
 */
conn *conn_from_freelist()
{
    conn *c;

    pthread_mutex_lock(&conn_lock);
    if (freecurr > 0)
    {
        c = freeconns[--freecurr];
    }
    else
    {
        c = NULL;
    }
    pthread_mutex_unlock(&conn_lock);

    return c;
}

/*
 * Adds a connection to the freelist. 0 = success.
 */
bool conn_add_to_freelist(conn *c)
{
    bool ret = true;
    pthread_mutex_lock(&conn_lock);
    if (freecurr < freetotal)
    {
        freeconns[freecurr++] = c;
        ret = false;
    }
    else
    {
        /* try to enlarge free connections array */
        size_t newsize = freetotal * 2;
        conn **new_freeconns = (conn **)realloc(freeconns, sizeof(conn *) * newsize);
        if (new_freeconns)
        {
            freetotal = newsize;
            freeconns = new_freeconns;
            freeconns[freecurr++] = c;
            ret = false;
        }
    }
    pthread_mutex_unlock(&conn_lock);
    return ret;
}

conn *conn_new(const int sfd, enum conn_states init_state,
               const int event_flags,
               const int read_buffer_size, enum network_transport transport,
               struct event_base *base)
{
    conn *c = conn_from_freelist();

    if (NULL == c)
    {
        if (!(c = (conn *)calloc(1, sizeof(conn))))
        {
            log_debug(LOG_ERR, "calloc()\n");
            return NULL;
        }

        c->rbuf = c->wbuf = 0;
        c->iov = 0;
        c->msglist = 0;
        c->hdrbuf = 0;

        c->rsize = read_buffer_size;
        c->wsize = DATA_BUFFER_SIZE;
        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;
        c->hdrsize = 0;

        c->rbuf = (char *)malloc((size_t)c->rsize);
        c->wbuf = (char *)malloc((size_t)c->wsize);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgsize);

        if (c->rbuf == 0 || c->wbuf == 0 || c->iov == 0 || c->msglist == 0)
        {
            conn_free(c);
            log_debug(LOG_ERR, "malloc()\n");
            return NULL;
        }
    }

    c->transport = transport;

    /* unix socket mode doesn't need this, so zeroed out.  but why
     * is this done for every command?  presumably for UDP
     * mode.  */
    if (!g_settings.socketpath)
    {
        c->request_addr_size = sizeof(c->request_addr);
    }
    else
    {
        c->request_addr_size = 0;
    }

    if (g_settings.verbose > 1)
    {
        if (init_state == conn_listening)
        {
            log_debug(LOG_ERR, "<%d server listening\n", sfd);
        }
    }

    c->sfd = sfd;
    c->state = init_state;
    c->rlbytes = 0;
    c->cmd = -1;
    c->rbytes = c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;

    c->write_and_go = init_state;
    c->write_and_free = 0;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1)
    {
        if (conn_add_to_freelist(c))
        {
            conn_free(c);
        }
        log_debug(LOG_ERR, "event_add, error:%s\n", strerror(errno));
        return NULL;
    }

    return c;
}

void conn_cleanup(conn *c)
{
    assert(c != NULL);

    if (c->write_and_free)
    {
        free(c->write_and_free);
        c->write_and_free = 0;
    }
}

/*
 * Frees a connection.
 */
void conn_free(conn *c)
{
    if (c)
    {
        if (c->hdrbuf)
        {
            free(c->hdrbuf);
        }
        if (c->msglist)
        {
            free(c->msglist);
        }
        if (c->rbuf)
        {
            free(c->rbuf);
        }
        if (c->wbuf)
        {
            free(c->wbuf);
        }
        if (c->iov)
        {
            free(c->iov);
        }
        free(c);
    }
}

void conn_close(conn *c)
{
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);

    if (g_settings.verbose > 1)
    {
        log_debug(LOG_ERR, "<%d connection closed.\n", c->sfd);
    }

    close(c->sfd);
    pthread_mutex_lock(&conn_lock);
    do_accept_new_conns(true);
    pthread_mutex_unlock(&conn_lock);
    conn_cleanup(c);

    /* if the connection has big buffers, just free it */
    if (c->rsize > READ_BUFFER_HIGHWAT || conn_add_to_freelist(c))
    {
        conn_free(c);
    }

    return;
}

/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
void conn_shrink(conn *c)
{
    assert(c != NULL);
    if (c->rsize > READ_BUFFER_HIGHWAT && c->rbytes < DATA_BUFFER_SIZE)
    {
        char *newbuf;

        if (c->rcurr != c->rbuf)
        {
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);
        }

        newbuf = (char *)realloc((void *)c->rbuf, DATA_BUFFER_SIZE);

        if (newbuf)
        {
            c->rbuf = newbuf;
            c->rsize = DATA_BUFFER_SIZE;
        }
        /* TODO check other branch... */
        c->rcurr = c->rbuf;
    }

    if (c->msgsize > MSG_LIST_HIGHWAT)
    {
        struct msghdr *newbuf = (struct msghdr *) realloc((void *)c->msglist, MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf)
        {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
        /* TODO check error condition? */
    }

    if (c->iovsize > IOV_LIST_HIGHWAT)
    {
        struct iovec *newbuf = (struct iovec *) realloc((void *)c->iov, IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf)
        {
            c->iov = newbuf;
            c->iovsize = IOV_LIST_INITIAL;
        }
        /* TODO check return value */
    }
}

/**
 * Convert a state name to a human readable form.
 */
static const char *state_text(enum conn_states state)
{
    const char *const statenames[] = { "conn_listening",
                                       "conn_new_cmd",
                                       "conn_waiting",
                                       "conn_read",
                                       "conn_parse_cmd",
                                       "conn_write",
                                       "conn_nread",
                                       "conn_swallow",
                                       "conn_closing",
                                       "conn_mwrite"
                                     };
    return statenames[state];
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
void conn_set_state(conn *c, enum conn_states state)
{
    assert(c != NULL);
    assert(state >= conn_listening && state < conn_max_state);

    if (state != c->state)
    {
        if (g_settings.verbose > 2)
        {
            log_debug(LOG_ERR, "%d: going from %s to %s\n",
                      c->sfd, state_text(c->state),
                      state_text(state));
        }

        c->state = state;
    }
}


