#ifndef CONN_H
#define CONN_H

#include <event.h>
#include "head.h"
#include "thread.h"

/**
 * The structure representing a connection into memcached.
 */
typedef struct conn conn;
struct conn
{
    int    sfd;
    enum conn_states  state;
    struct event event;
    short  ev_flags;
    short  which;   /** which events were just triggered */

    char   *rbuf;   /** buffer to read commands into */
    char   *rcurr;  /** but if we parsed some already, this is where we stopped */
    int    rsize;   /** total allocated size of rbuf */
    int    rbytes;  /** how much data, starting from rcur, do we have unparsed */

    char   *wbuf;
    char   *wcurr;
    int    wsize;
    int    wbytes;

    /** which state to go into after finishing current write */
    enum conn_states  write_and_go;
    void   *write_and_free; /** free this memory after finishing writing */

    char   *ritem;  /** when we read in an item's value, it goes here */
    int    rlbytes;

    /* data for the swallow state */
    int    sbytes;    /* how many bytes to swallow */

    /* data for the mwrite state */
    struct iovec *iov;
    int    iovsize;   /* number of elements allocated in iov[] */
    int    iovused;   /* number of elements used in iov[] */

    struct msghdr *msglist;
    int    msgsize;   /* number of elements allocated in msglist[] */
    int    msgused;   /* number of elements used in msglist[] */
    int    msgcurr;   /* element in msglist[] being transmitted now */
    int    msgbytes;  /* number of bytes in current msg */

    enum network_transport transport; /* what transport is used by this connection */

    /* data for UDP clients */
    int    request_id; /* Incoming UDP request ID, if this is a UDP "connection" */
    struct sockaddr request_addr; /* Who sent the most recent request */
    socklen_t request_addr_size;
    unsigned char *hdrbuf; /* udp packet headers */
    int    hdrsize;   /* number of headers' worth of space is allocated */

    bool   noreply;   /* True if the reply should not be sent. */

    /* Binary protocol stuff */
    /* This is where the binary header goes */
    request_header binary_header;
    short cmd; /* current command being processed */
    conn   *next;     /* Used for generating a list of conn structures */
    LIBEVENT_THREAD *thread; /* Pointer to the thread object serving this connection */
};


/* Lock wrappers for cache functions that are called from main loop. */

conn *conn_from_freelist(void);
bool  conn_add_to_freelist(conn *c);

conn *conn_new(const int sfd, const enum conn_states init_state,
               const int event_flags, const int read_buffer_size,
               enum network_transport transport, struct event_base *base);

void conn_init(void);
void conn_free(conn *c);
void conn_close(conn *c);

void conn_shrink(conn *c);
void conn_cleanup(conn *c);

void conn_set_state(conn *c, enum conn_states state);

#endif
