#include <event.h>
#include <evhttp.h>
#include <evutil.h>

#include "event2/event.h"
#include "event2/http.h"
#include "event2/buffer.h"
#include "event2/http_struct.h"
#include "event2/dns.h"

#include "config.h"
#include "network.h"
#include "util.h"
#include "conn.h"
#include "log.h"
#include "sig.h"
#include "prefixmatch.h"

#define IOV_MAX 1024

/** exported globals **/
time_t process_started;     /* when the process was started */

/** file scope variables **/
struct event_base *main_base;

/*
 * forward declarations
 */
void drive_machine(conn *c);
int try_read_command(conn *c);

/* event handling, network IO */
static void complete_nread(conn *c);
static void process_command(conn *c, char *command);
static void write_and_free(conn *c, char *buf, int bytes);
static int ensure_iov_space(conn *c);
static int add_iov(conn *c, const void *buf, int len);
static int add_msghdr(conn *c);

static void usage(void)
{
    printf("-f <file>      the path of configuration file\n");
    return;
}

enum transmit_result
{
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};

static enum transmit_result transmit(conn *c);

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int add_msghdr(conn *c)
{
    struct msghdr *msg;

    assert(c != NULL);

    if (c->msgsize == c->msgused)
    {
        msg = (msghdr *)realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if (! msg)
        {
            return -1;
        }
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    msg->msg_iov = &c->iov[c->iovused];

    if (c->request_addr_size > 0)
    {
        msg->msg_name = &c->request_addr;
        msg->msg_namelen = c->request_addr_size;
    }

    c->msgbytes = 0;
    c->msgused++;

    return 0;
}

/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c)
{
    assert(c != NULL);

    if (c->iovused >= c->iovsize)
    {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                                        (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov)
        {
            return -1;
        }
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++)
        {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}

/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */

static int add_iov(conn *c, const void *buf, int len)
{
    struct msghdr *m;
    int leftover;
    bool limit_to_mtu;

    assert(c != NULL);

    do
    {
        m = &c->msglist[c->msgused - 1];

        /*
         * Limit UDP packets, and the first payloads of TCP replies, to
         * UDP_MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = (1 == c->msgused);

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && c->msgbytes >= UDP_MAX_PAYLOAD_SIZE))
        {
            add_msghdr(c);
            m = &c->msglist[c->msgused - 1];
        }

        if (ensure_iov_space(c) != 0)
        {
            return -1;
        }

        /* If the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE)
        {
            leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        }
        else
        {
            leftover = 0;
        }

        m = &c->msglist[c->msgused - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        c->msgbytes += len;
        c->iovused++;
        m->msg_iovlen++;

        buf = ((char *)buf) + len;
        len = leftover;
    }
    while (leftover > 0);

    return 0;
}

void out_string(conn *c, const char *str)
{
    size_t len;

    assert(c != NULL);

    if (g_settings.verbose > 1)
    {
        log_debug(LOG_ERR, ">%d %s\n", c->sfd, str);
    }

    /* Nuke a partial output... */
    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    add_msghdr(c);

    len = strlen(str);
    if ((len + 2) > c->wsize)
    {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_new_cmd;
    return;
}

static void add_bin_header(conn *c, uint16_t err, uint32_t body_len)
{
    response_header *header;

    assert(c);

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0)
    {
        /* XXX:  out_string is inappropriate here */
        out_string(c, "SERVER_ERROR out of memory");
        return;
    }

    header = (response_header *)c->wbuf;
    header->response.magic = (uint8_t)RES;
    header->response.status = (uint16_t)htons(err);
    header->response.bodylen = htonl(body_len);

    if (g_settings.verbose > 1)
    {
        int ii;
        log_debug(LOG_ERR, ">%d Writing bin response:", c->sfd);
        for (ii = 0; ii < sizeof(header->bytes); ++ii)
        {
            if (ii % 4 == 0)
            {
                log_debug(LOG_ERR, "\n>%d  ", c->sfd);
            }
            log_debug(LOG_ERR, " 0x%02x", header->bytes[ii]);
        }
        log_debug(LOG_ERR, "\n");
    }

    add_iov(c, c->wbuf, sizeof(header->response));
}

static void write_bin_error(conn *c, response_status err, int swallow)
{
    const char *errstr = "Unknown error";
    size_t len;

    switch (err)
    {
        case RESPONSE_ENOMEM:
            errstr = "Out of memory";
            break;
        default:
            assert(false);
            errstr = "UNHANDLED ERROR";
            log_debug(LOG_ERR, ">%d UNHANDLED ERROR: %d\n", c->sfd, err);
    }

    if (g_settings.verbose > 1)
    {
        log_debug(LOG_ERR, ">%d Writing an error: %s\n", c->sfd, errstr);
    }

    len = strlen(errstr);
    add_bin_header(c, err, len);
    if (len > 0)
    {
        add_iov(c, errstr, len);
    }
    conn_set_state(c, conn_mwrite);
    if (swallow > 0)
    {
        c->sbytes = swallow;
        c->write_and_go = conn_swallow;
    }
    else
    {
        c->write_and_go = conn_new_cmd;
    }
}

/* Form and send a response to a command over the binary protocol */
static void write_bin_response(conn *c, void *d, int dlen)
{
    add_bin_header(c, 0, dlen);
    if (dlen > 0)
    {
        add_iov(c, d, dlen);
    }
    conn_set_state(c, conn_mwrite);
    c->write_and_go = conn_new_cmd;
}

static void reset_cmd_handler(conn *c)
{
    c->cmd = -1;

    conn_shrink(c);
    if (c->rbytes > 0)
    {
        conn_set_state(c, conn_parse_cmd);
    }
    else
    {
        conn_set_state(c, conn_waiting);
    }
}

/**
 * get a pointer to the start of the request struct for the current command
 */
static void *get_request(conn *c)
{
    char *ret = c->rcurr;
    ret -= sizeof(c->binary_header);

    assert(ret >= c->rbuf);
    return ret;
}

#define DEFAULT_GET_NUMBER 10

static void complete_nread(conn *c)
{
    char *data;
    uint32_t vlen;

    assert(c != NULL);

    vlen = c->binary_header.request.bodylen;
    data = (char *)c->ritem - vlen;

    if (g_settings.verbose > 1)
    {
        log_debug(LOG_ERR, "Value len is %d\n", vlen);
    }

    uint32_t number = *(uint32_t *)data;
    data += sizeof(uint32_t);
    string key(data, 0, vlen - sizeof(uint32_t));
    vector<string> vRes;
    int ret = Get(key, vRes);
    if (ret != 0)
    {
        log_debug(LOG_ERR, "Fail to get result for key:%s\n", key.c_str());
        write_bin_error(c, RESPONSE_ENOMEM, 0);
        return;
    }

    if (number == 0)
    {
        number = DEFAULT_GET_NUMBER;
    }
    if (number > vRes.size())
    {
        number = vRes.size();
    }

    int rlen = sizeof(uint32_t);
    for (int i = 0; i < vRes.size(); i++)
    {
        rlen += sizeof(uint32_t);
        rlen += vRes[i].length();
    }

    char *resp_buf = (char *)calloc(1, rlen + 1);
    if (resp_buf == NULL)
    {
        log_debug(LOG_ERR, "fail to malloc memory\n");
        write_bin_error(c, RESPONSE_ENOMEM, 0);
        return;
    }
    char *tbuf = resp_buf;

#define set_value(ptr, valueptr, size) do {\
        memcpy(tbuf, valueptr, size); \
        tbuf += size;\
    }while(0)

    set_value(tbuf, &number, sizeof(number));
    for (int i = 0; i < vRes.size(); i++)
    {
        uint32_t length = vRes[i].length();
        set_value(tbuf, &length, sizeof(length));
        set_value(tbuf, vRes[i].c_str(), length);
    }

    c->write_and_free = resp_buf;
    write_bin_response(c, resp_buf, rlen);
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn *c, char *buf, int bytes)
{
    if (buf)
    {
        c->write_and_free = buf;
        c->wcurr = buf;
        c->wbytes = bytes;
        conn_set_state(c, conn_write);
        c->write_and_go = conn_new_cmd;
    }
    else
    {
        out_string(c, "SERVER_ERROR out of memory writing stats");
    }
}

static void bin_read_key(conn *c)
{
    assert(c);
    c->rlbytes = c->binary_header.request.bodylen;

    /* Ok... do we have room for the extras and the key in the input buffer? */
    ptrdiff_t offset = c->rcurr + sizeof(request_header) - c->rbuf;
    if (c->rlbytes > c->rsize - offset)
    {
        size_t nsize = c->rsize;
        size_t size = c->rlbytes + sizeof(request_header);

        while (size > nsize)
        {
            nsize *= 2;
        }

        if (nsize != c->rsize)
        {
            if (g_settings.verbose > 1)
            {
                log_debug(LOG_ERR, "%d: Need to grow buffer from %lu to %lu\n",
                          c->sfd, (unsigned long)c->rsize, (unsigned long)nsize);
            }
            char *newm = (char *)realloc(c->rbuf, nsize);
            if (newm == NULL)
            {
                if (g_settings.verbose)
                {
                    log_debug(LOG_ERR, "%d: Failed to grow buffer.. closing connection\n",
                              c->sfd);
                }
                conn_set_state(c, conn_closing);
                return;
            }

            c->rbuf = newm;
            /* rcurr should point to the same offset in the packet */
            c->rcurr = c->rbuf + offset - sizeof(request_header);
            c->rsize = nsize;
        }
        if (c->rbuf != c->rcurr)
        {
            memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
            if (g_settings.verbose > 1)
            {
                log_debug(LOG_ERR, "%d: Repack input buffer\n", c->sfd);
            }
        }
    }

    /* preserve the header in the buffer.. */
    c->ritem = c->rcurr + sizeof(request_header);
    conn_set_state(c, conn_nread);
}

static void dispatch_bin_command(conn *c)
{
    uint32_t bodylen = c->binary_header.request.bodylen;
    bin_read_key(c);
}

/*
 * if we have a complete line in the buffer, process it.
 */
int try_read_command(conn *c)
{
    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));
    assert(c->rbytes > 0);

    /* Do we have the complete packet header? */
    if (c->rbytes < sizeof(c->binary_header))
    {
        /* need more data! */
        return 0;
    }
    else
    {
        request_header *req;
        req = (request_header *)c->rcurr;

        //check the magic
        if (req->request.magic != REQ)
        {
            if (g_settings.verbose)
            {
                log_debug(LOG_ERR, "Invalid magic:  %x\n", req->request.magic);
            }
            conn_set_state(c, conn_closing);
            return -1;
        }

        if (g_settings.verbose > 1)
        {
            /* Dump the packet before we convert it to host order */
            int ii;
            log_debug(LOG_ERR, "<%d Read binary protocol data:", c->sfd);
            for (ii = 0; ii < sizeof(req->bytes); ++ii)
            {
                if (ii % 4 == 0)
                {
                    log_debug(LOG_ERR, "\n<%d   ", c->sfd);
                }
                log_debug(LOG_ERR, " 0x%02x", req->bytes[ii]);
            }
            log_debug(LOG_ERR, "\n");
        }

        c->binary_header = *req;
        c->binary_header.request.bodylen = ntohl(req->request.bodylen);

        c->msgcurr = 0;
        c->msgused = 0;
        c->iovused = 0;
        if (add_msghdr(c) != 0)
        {
            out_string(c, "SERVER_ERROR out of memory");
            return 0;
        }

        c->cmd = c->binary_header.request.opcode;

        dispatch_bin_command(c);

        c->rbytes -= sizeof(c->binary_header);
        c->rcurr += sizeof(c->binary_header);
    }

    return 1;
}

/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit(conn *c)
{
    assert(c != NULL);

    if (c->msgcurr < c->msgused &&
        c->msglist[c->msgcurr].msg_iovlen == 0)
    {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused)
    {
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];

        res = sendmsg(c->sfd, m, 0);
        if (res > 0)
        {
            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len)
            {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0)
            {
                m->msg_iov->iov_base = (caddr_t)m->msg_iov->iov_base + res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }
        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!update_event(c, EV_WRITE | EV_PERSIST))
            {
                if (g_settings.verbose > 0)
                {
                    log_debug(LOG_ERR, "Couldn't update event\n");
                }
                conn_set_state(c, conn_closing);
                return TRANSMIT_HARD_ERROR;
            }
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (g_settings.verbose > 0)
        {
            log_debug(LOG_ERR, "Failed to write, and not due to blocking, error:%s\n", strerror(errno));
        }
        conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    }
    else
    {
        return TRANSMIT_COMPLETE;
    }
}

void drive_machine(conn *c)
{
    bool stop = false;
    int sfd, flags = 1;
    socklen_t addrlen;
    struct sockaddr_storage addr;
    int nreqs = g_settings.reqs_per_event;
    int res;
    const char *str;

    assert(c != NULL);

    while (!stop)
    {
        switch (c->state)
        {
            case conn_listening:
                addrlen = sizeof(addr);
                if ((sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen)) == -1)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        /* these are transient, so don't log anything */
                        stop = true;
                    }
                    else if (errno == EMFILE)
                    {
                        if (g_settings.verbose > 0)
                        {
                            log_debug(LOG_ERR, "Too many open connections\n");
                        }
                        accept_new_conns(false);
                        stop = true;
                    }
                    else
                    {
                        log_debug(LOG_ERR, "accept()");
                        stop = true;
                    }
                    break;
                }
                if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
                    fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0)
                {
                    log_debug(LOG_ERR, "setting O_NONBLOCK");
                    close(sfd);
                    break;
                }

                log_debug(LOG_NOTICE, "get a new connection\n");
                dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
                                  DATA_BUFFER_SIZE, tcp_transport);
                stop = true;
                break;

            case conn_waiting:
                if (!update_event(c, EV_READ | EV_PERSIST))
                {
                    if (g_settings.verbose > 0)
                    {
                        log_debug(LOG_ERR, "Couldn't update event\n");
                    }
                    conn_set_state(c, conn_closing);
                    break;
                }

                conn_set_state(c, conn_read);
                stop = true;
                break;

            case conn_read:
                res = try_read_network(c);

                switch (res)
                {
                    case READ_NO_DATA_RECEIVED:
                        conn_set_state(c, conn_waiting);
                        break;
                    case READ_DATA_RECEIVED:
                        conn_set_state(c, conn_parse_cmd);
                        break;
                    case READ_ERROR:
                        conn_set_state(c, conn_closing);
                        break;
                    case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                        /* State already set by try_read_network */
                        break;
                }
                break;

            case conn_parse_cmd :
                if (try_read_command(c) == 0)
                {
                    /* wee need more data! */
                    conn_set_state(c, conn_waiting);
                }

                break;

            case conn_new_cmd:
                /* Only process nreqs at a time to avoid starving other
                   connections */
                --nreqs;
                if (nreqs >= 0)
                {
                    reset_cmd_handler(c);
                }
                else
                {
                    if (c->rbytes > 0)
                    {
                        /* We have already read in data into the input buffer,
                           so libevent will most likely not signal read events
                           on the socket (unless more data is available. As a
                           hack we should just put in a request to write data,
                           because that should be possible ;-)
                        */
                        if (!update_event(c, EV_WRITE | EV_PERSIST))
                        {
                            if (g_settings.verbose > 0)
                            {
                                log_debug(LOG_ERR, "Couldn't update event\n");
                            }
                            conn_set_state(c, conn_closing);
                        }
                    }
                    stop = true;
                }
                break;

            case conn_nread:
                if (c->rlbytes == 0)
                {
                    complete_nread(c);
                    break;
                }
                /* first check if we have leftovers in the conn_read buffer */
                if (c->rbytes > 0)
                {
                    int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
                    if (c->ritem != c->rcurr)
                    {
                        memmove(c->ritem, c->rcurr, tocopy);
                    }
                    c->ritem += tocopy;
                    c->rlbytes -= tocopy;
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    if (c->rlbytes == 0)
                    {
                        break;
                    }
                }

                /*  now try reading from the socket */
                res = read(c->sfd, c->ritem, c->rlbytes);
                if (res > 0)
                {
                    if (c->rcurr == c->ritem)
                    {
                        c->rcurr += res;
                    }
                    c->ritem += res;
                    c->rlbytes -= res;
                    break;
                }
                if (res == 0)   /* end of stream */
                {
                    conn_set_state(c, conn_closing);
                    break;
                }
                if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    if (!update_event(c, EV_READ | EV_PERSIST))
                    {
                        if (g_settings.verbose > 0)
                        {
                            log_debug(LOG_ERR, "Couldn't update event\n");
                        }
                        conn_set_state(c, conn_closing);
                        break;
                    }
                    stop = true;
                    break;
                }
                /* otherwise we have a real error, on which we close the connection */
                if (g_settings.verbose > 0)
                {
                    log_debug(LOG_ERR, "Failed to read, and not due to blocking:\n"
                              "errno: %d %s \n"
                              "rcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d rsize=%d\n",
                              errno, strerror(errno),
                              (long)c->rcurr, (long)c->ritem, (long)c->rbuf,
                              (int)c->rlbytes, (int)c->rsize);
                }
                conn_set_state(c, conn_closing);
                break;

            case conn_swallow:
                /* we are reading sbytes and throwing them away */
                if (c->sbytes == 0)
                {
                    conn_set_state(c, conn_new_cmd);
                    break;
                }

                /* first check if we have leftovers in the conn_read buffer */
                if (c->rbytes > 0)
                {
                    int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                    c->sbytes -= tocopy;
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    break;
                }

                /*  now try reading from the socket */
                res = read(c->sfd, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
                if (res > 0)
                {
                    c->sbytes -= res;
                    break;
                }
                if (res == 0)   /* end of stream */
                {
                    conn_set_state(c, conn_closing);
                    break;
                }
                if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    if (!update_event(c, EV_READ | EV_PERSIST))
                    {
                        if (g_settings.verbose > 0)
                        {
                            log_debug(LOG_ERR, "Couldn't update event\n");
                        }
                        conn_set_state(c, conn_closing);
                        break;
                    }
                    stop = true;
                    break;
                }
                /* otherwise we have a real error, on which we close the connection */
                if (g_settings.verbose > 0)
                {
                    log_debug(LOG_ERR, "Failed to read, and not due to blocking\n");
                }
                conn_set_state(c, conn_closing);
                break;

            case conn_write:
                /*
                 * We want to write out a simple response. If we haven't already,
                 * assemble it into a msgbuf list (this will be a single-entry
                 * list for TCP or a two-entry list for UDP).
                 */
                if (c->iovused == 0)
                {
                    if (add_iov(c, c->wcurr, c->wbytes) != 0)
                    {
                        if (g_settings.verbose > 0)
                        {
                            log_debug(LOG_ERR, "Couldn't build response\n");
                        }
                        conn_set_state(c, conn_closing);
                        break;
                    }
                }

                /* fall through... */

            case conn_mwrite:
                switch (transmit(c))
                {
                    case TRANSMIT_COMPLETE:
                        if (c->state == conn_mwrite)
                        {
                            if (c->write_and_free)
                            {
                                free(c->write_and_free);
                                c->write_and_free = 0;
                            }
                            /*
                                        if (c->protocol == binary_prot)
                                        {
                                                conn_set_state(c, c->write_and_go);
                                        }*/
                            else
                            {
                                conn_set_state(c, conn_new_cmd);
                            }
                        }
                        else if (c->state == conn_write)
                        {
                            if (c->write_and_free)
                            {
                                free(c->write_and_free);
                                c->write_and_free = 0;
                            }
                            conn_set_state(c, c->write_and_go);
                        }
                        else
                        {
                            if (g_settings.verbose > 0)
                            {
                                log_debug(LOG_ERR, "Unexpected state %d\n", c->state);
                            }
                            conn_set_state(c, conn_closing);
                        }
                        break;

                    case TRANSMIT_INCOMPLETE:
                    case TRANSMIT_HARD_ERROR:
                        break;                   /* Continue in state machine. */

                    case TRANSMIT_SOFT_ERROR:
                        stop = true;
                        break;
                }
                break;

            case conn_closing:
                conn_close(c);
                stop = true;
                break;

            case conn_max_state:
                assert(false);
                break;
        }
    }

    return;
}

static struct evhttp *http;
static struct evhttp_bound_socket *handle;

/* http_cb
 * support 2 operations: get, reload
 * in get operation, need 2 parameters:
 *  key, number
 * in reload operation, need 1 parameter:
 *  indexpath
 * eg. http://ip:8000/?opt=get&key=zhang&number=10
 *     http://ip:8000/?opt=reload&indexpath=/var/index
 */
static void process_http_cb(struct evhttp_request *req, void *arg)
{
    struct evbuffer *evb = NULL;
    struct evhttp_uri *decoded = NULL;
    const char *path = NULL;
    char *decoded_path = NULL;
    struct evkeyvalq http_query;
    char *decode_uri = NULL;
    const char *uri = evhttp_request_get_uri(req);

    const char *http_input_opt = NULL;
    const char *http_input_key = NULL;
    const char *http_input_number = NULL;
    const char *http_input_indexpath = NULL;

    log_debug(LOG_NOTICE, "Got a GET request for <%s>\n",  uri);   /* Decode the URI */
    decoded = evhttp_uri_parse(uri);
    if (!decoded)
    {
        log_debug(LOG_ERR, "It's not a good URI. Sending BADREQUEST\n");
        evhttp_send_error(req, HTTP_BADREQUEST, 0);
        return;
    }
    /* Let's see what path the user asked for. */
    path = evhttp_uri_get_path(decoded);
    if (!path)
    {
        path = "/";
    }
    /* We need to decode it, to see what path the user really wanted. */
    decoded_path = evhttp_uridecode(path, 0, NULL);
    if (decoded_path == NULL)
    {
        goto done;
    }
    decode_uri = strdup((char *) evhttp_request_uri(req));
    evhttp_parse_query(decode_uri, &http_query);
    free(decode_uri);           /* This holds the content we're sending. */
    evb = evbuffer_new();   /*  URI Parameter  */
    http_input_opt = evhttp_find_header(&http_query, "opt"); /* Operation Type */
    http_input_key = evhttp_find_header(&http_query, "key"); /* key */
    http_input_number = evhttp_find_header(&http_query, "number"); /* max number of we want */
    http_input_indexpath = evhttp_find_header(&http_query, "indexpath"); /* index path */      /* 1. get*/

    if (http_input_opt == NULL || strlen(http_input_opt) == 0)
    {
        evhttp_send_error(req, HTTP_BADREQUEST, 0);
        goto done;
    }

    if (strcmp(http_input_opt, "get") == 0)
    {
        int number = 10;
        if (http_input_key == NULL || strlen(http_input_key) == 0)
        {
            evhttp_send_error(req, HTTP_BADREQUEST, 0);
            goto done;
        }
        else if (http_input_number != NULL && strlen(http_input_number) > 0)
        {
            number = atoi(http_input_number);
        }
        string key(http_input_key);
        vector<string> vRes;
        int ret = Get(key, vRes);
        if (ret != 0 || vRes.size() == 0)
        {
            evhttp_send_error(req, HTTP_NOCONTENT, 0);
            goto done;
        }
        number = number > vRes.size() ? vRes.size() : number;
        const char *trailing_slash = "";
        if (!strlen(path) || path[strlen(path) - 1] != '/')
        {
            trailing_slash = "/";
        }
        evbuffer_add_printf(evb, "<html>\n <head>\n"
                            "  <title>%s</title>\n"
                            "  <base href='%s%s'>\n"
                            " </head>\n"
                            " <body>\n"
                            "  <h1>%s</h1>\n"
                            "  <ul>\n",
                            decoded_path, /* XXX html-escape this. */
                            path, /* XXX html-escape this? */
                            trailing_slash,
                            decoded_path /* XXX html-escape this */);
        for (int i = 0; i < number; i++)
        {
            //evbuffer_add_printf(evb, "    <li><a href=\"%s\">%s</a>\n",
            evbuffer_add_printf(evb, "    <li>%s</a>\n", vRes[i].c_str());/* XXX escape this */
        }
        evbuffer_add_printf(evb, "</ul></body></html>\n");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
        evhttp_send_reply(req, HTTP_OK , "OK", evb);
    }
    else if (strcmp(http_input_opt, "reload") == 0)
    {
        int use_default = 0;
        if (http_input_indexpath == NULL || strlen(http_input_indexpath) == 0)
        {
            use_default = 1;
        }

        int ret = Reload_index((char *)http_input_indexpath);
        if (ret == 0)
        {
            evbuffer_add_printf(evb, "<html>\n <head>\n"
                                "  <title>%s</title>\n"
                                " </head>\n"
                                " <body>\n"
                                "  <ul>\n",
                                decoded_path /* XXX html-escape this */);
            evbuffer_add_printf(evb, "    <li>reload index %s successfully</a>\n", use_default ? g_settings.index_path : http_input_indexpath);
            evbuffer_add_printf(evb, "</ul></body></html>\n");
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
            evhttp_send_reply(req, HTTP_OK, "OK", evb);
        }
        else
        {
            evbuffer_add_printf(evb, "<html>\n <head>\n"
                                " <title>%s</title>\n"
                                " </head>\n"
                                " <body>\n"
                                "  <ul>\n",
                                decoded_path /* XXX html-escape this */);
            evbuffer_add_printf(evb, "    <li>reload index %s failed</a>\n", use_default ? g_settings.index_path : http_input_indexpath); /* XXX escape this */
            evbuffer_add_printf(evb, "</ul></body></html>\n");
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
            evhttp_send_reply(req, HTTP_OK, "Internal Error", evb);
        }
    }
    else
    {
        evhttp_send_error(req, HTTP_NOTFOUND, 0);
    }

done:
    if (decoded)
    {
        evhttp_uri_free(decoded);
    }
    if (decoded_path)
    {
        free(decoded_path);
    }
    evhttp_clear_headers(&http_query);
    evbuffer_free(evb);
}

static int http_handler(unsigned short port, unsigned int timeout)
{
    struct sockaddr_storage ss;
    evutil_socket_t fd;
    ev_socklen_t socklen = sizeof(ss);
    char addrbuf[128];
    void *inaddr;
    const char *addr;
    int got_port = -1;
    char uri_root[512];

    log_debug(LOG_NOTICE, "begin to init http handler\n");

    /* Create a new evhttp object to handle requests. */
    http = evhttp_new(main_base);
    if (!http)
    {
        log_debug(LOG_ERR, "couldn't create evhttp. Exiting.\n");
        return -1;
    }
    /* set the http timeout */
    evhttp_set_timeout(http , timeout);
    /* only care about get request */
    evhttp_set_allowed_methods(http , EVHTTP_REQ_GET);
    /* We want to accept arbitrary requests, so we need to set a "generic" cb. We can also add callbacks for specific paths. */
    evhttp_set_gencb(http, process_http_cb, NULL);
    /* Now we tell the evhttp what port to listen on */
    handle = evhttp_bind_socket_with_handle(http, "0.0.0.0", port);
    if (!handle)
    {
        log_debug(LOG_ERR, "couldn't bind to port %d. Exiting.\n", (int)port);
        return -1;
    }
    /* Extract and display the address we're listening on. */
    fd = evhttp_bound_socket_get_fd(handle);
    memset(&ss, 0, sizeof(ss));
    if (getsockname(fd, (struct sockaddr *)&ss, &socklen))
    {
        log_debug(LOG_ERR, "getsockname() failed\n");
        return -1;
    }
    if (ss.ss_family == AF_INET)
    {
        got_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
        inaddr = &((struct sockaddr_in *)&ss)->sin_addr;
    }
    else if (ss.ss_family == AF_INET6)
    {
        got_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
        inaddr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
    }
    else
    {
        log_debug(LOG_ERR, "Weird address family %d\n", ss.ss_family);
        return -1;
    }
    addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf, sizeof(addrbuf));
    if (addr)
    {
        log_debug(LOG_NOTICE, "Listening on %s:%d\n", addr, got_port);
        evutil_snprintf(uri_root, sizeof(uri_root), "http://%s:%d", addr, got_port);
    }
    else
    {
        log_debug(LOG_ERR, "evutil_inet_ntop failed\n");
        return -1;
    }
    log_debug(LOG_NOTICE, "finish init http handle, uri root %s\n", uri_root);
    return 0;
}

static void save_pid(const char *pid_file)
{
    FILE *fp;
    if (access(pid_file, F_OK) == 0)
    {
        if ((fp = fopen(pid_file, "r")) != NULL)
        {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), fp) != NULL)
            {
                unsigned int pid;
                if (safe_strtoul(buffer, &pid) && kill((pid_t)pid, 0) == 0)
                {
                    fprintf(stderr, "WARNING: The pid file contained the following (running) pid: %u\n", pid);
                }
            }
            fclose(fp);
        }
    }

    if ((fp = fopen(pid_file, "w")) == NULL)
    {
        vperror("Could not open the pid file %s for writing", pid_file);
        return;
    }

    fprintf(fp, "%ld\n", (long)getpid());
    if (fclose(fp) == -1)
    {
        vperror("Could not close the pid file %s", pid_file);
        return;
    }
    log_debug(LOG_NOTICE, "save pid %d in to file %s successully\n", (int)getpid(), pid_file);
}

static void remove_pidfile(const char *pid_file)
{
    if (pid_file == NULL)
    {
        return;
    }

    if (unlink(pid_file) != 0)
    {
        vperror("Could not remove the pid file %s", pid_file);
    }

}

int daemonize(int nochdir, int noclose)
{
    int fd;

    switch (fork())
    {
        case -1:
            return (-1);
        case 0:
            break;
        default:
            _exit(EXIT_SUCCESS);
    }

    if (setsid() == -1)
    {
        return (-1);
    }

    if (nochdir == 0)
    {
        if (chdir("/") != 0)
        {
            perror("chdir");
            return (-1);
        }
    }

    if (noclose == 0 && (fd = open("/dev/null", O_RDWR, 0)) != -1)
    {
        if (dup2(fd, STDIN_FILENO) < 0)
        {
            perror("dup2 stdin");
            return (-1);
        }
        if (dup2(fd, STDOUT_FILENO) < 0)
        {
            perror("dup2 stdout");
            return (-1);
        }
        if (dup2(fd, STDERR_FILENO) < 0)
        {
            perror("dup2 stderr");
            return (-1);
        }

        if (fd > STDERR_FILENO)
        {
            if (close(fd) < 0)
            {
                perror("close");
                return (-1);
            }
        }
    }
    return (0);
}

void do_privilege(char *username)
{
    struct passwd *pw;
    struct rlimit rlim;
    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */
    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0)
    {
        log_debug(LOG_ERR, "failed to getrlimit number of files\n");
        sleep(1);
        exit(EX_OSERR);
    }
    else
    {
        rlim.rlim_cur = g_settings.maxconns;
        rlim.rlim_max = g_settings.maxconns;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0)
        {
            log_debug(LOG_ERR, "failed to set rlimit for open files. Try starting as root or requesting smaller maxconns value.\n");
            sleep(1);
            exit(EX_OSERR);
        }
    }

    /* lose root privileges if we have them */
    if (getuid() == 0 || geteuid() == 0)
    {
        if (username == 0 || *username == '\0')
        {
            log_debug(LOG_ERR, "can't run as root without the -u switch\n");
            sleep(1);
            exit(EX_USAGE);
        }
        if ((pw = getpwnam(username)) == 0)
        {
            log_debug(LOG_ERR, "can't find the user %s to switch to\n", username);
            sleep(1);
            exit(EX_NOUSER);
        }
        if (pw->pw_uid != 0)
        {
            if ((pw->pw_gid == 0 && setgid(pw->pw_gid) < 0)
                || (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0))
            {
                log_debug(LOG_ERR, "failed to assume identity of user %s\n", username);
                sleep(1);
                exit(EX_OSERR);
            }
        }
    }

}

void init_server()
{
    /* parse config file */
    if (strlen(g_settings.cfgpath) == 0)
    {
        printf("no config file privoided\n");
        exit(-1);
    }

    parse_config();
    if (check_settings() != 0)
    {
        printf("error in config file %s\n", g_settings.cfgpath);
        exit(-1);
    }
    output_settings(NULL);

    /* init log file */
    if (log_init(GENERIC_FILE, g_settings.log_level, g_settings.log_path, NULL, 0) != 0)
    {
        printf("error in parse log config\n");
        exit(-1);
    }

    //we can use log now
    if (Init_Index(g_settings.chinese_map_file, g_settings.index_path) != 0)
    {
        printf("error in load index file\n");
        exit(-1);
    }

    do_privilege(g_settings.username);
}

int main(int argc, char **argv)
{
    int c;
    int retval = EXIT_SUCCESS;

    /* init settings */
    settings_init();

    /* process arguments */
    while (-1 != (c = getopt(argc, argv, "f:")))
    {
        switch (c)
        {
            case 'f' :
                g_settings.cfgpath = strdup(optarg);
                break;
            default:
                fprintf(stderr, "Illegal argument \"%c\"\n", c);
                usage();
                exit(-1);
        }
    }

    if (!g_settings.cfgpath)
    {
        fprintf(stderr, "failed to find the config file\n");
        usage();
        exit(-1);
    }

    if (daemonize(1, g_settings.verbose) == -1)
    {
        fprintf(stderr, "failed to daemon() in order to daemonize\n");
        exit(EXIT_FAILURE);
    }

    if (signal_init() != 0)
    {
        printf("init signal handler failed\n");
        exit(-1);
    }

    init_server();

    /* initialize main thread libevent instance */
    main_base = event_init();

    /* initialize other stuff */
    conn_init();

    /* start up worker threads if MT mode */
    thread_init(g_settings.num_threads, main_base);

    /* create unix mode sockets after dropping privileges */
    if (g_settings.socketpath != NULL)
    {
        errno = 0;
        if (server_socket_unix(g_settings.socketpath, g_settings.access))
        {
            log_debug(LOG_ERR, "failed to listen on UNIX socket: %s, errno:%d\n", g_settings.socketpath, error);
            exit(EX_OSERR);
        }
        log_debug(LOG_NOTICE, "listen on UNIX socket: %s successfully\n", g_settings.socketpath);
    }

    /* create the listening socket, bind it, and init */
    errno = 0;
    if (g_settings.port && server_sockets(g_settings.port, tcp_transport, NULL))
    {
        log_debug(LOG_ERR, "failed to listen on TCP port %d, errno:%d\n", g_settings.port, error);
        exit(EX_OSERR);
    }

    log_debug(LOG_ERR, "listen on TCP port %d successfully\n", g_settings.port);
    http_handler(g_settings.monitor_port, g_settings.monitor_timeout);

    /* Give the sockets a moment to open. I know this is dumb, but the error
     * is only an advisory.
     */
    usleep(1000);

    if (g_settings.pidfile != NULL)
    {
        save_pid(g_settings.pidfile);
    }

    log_debug(LOG_ERR, "begin to process in main loop\n");

    /* enter the event loop */
    if (event_base_loop(main_base, 0) != 0)
    {
        retval = EXIT_FAILURE;
    }

    /* remove the PID file if we're a daemon */
    remove_pidfile(g_settings.pidfile);

    Deinit_Index();

    evhttp_free(http);
    event_base_free(main_base);

    return retval;
}
