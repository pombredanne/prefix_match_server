#include "config.h"
#include "thread.h"
#include "network.h"
#include "util.h"
#include "head.h"
#include "log.h"

extern struct event_base *main_base;
extern void drive_machine(conn *c);
extern void out_string(conn *c, const char *str);

static conn *listen_conn = NULL;

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return enum try_read_result
 */
enum try_read_result try_read_network(conn *c)
{
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    int res;
    int num_allocs = 0;
    assert(c != NULL);

    if (c->rcurr != c->rbuf)
    {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
        {
            memmove(c->rbuf, c->rcurr, c->rbytes);
        }
        c->rcurr = c->rbuf;
    }

    while (1)
    {
        if (c->rbytes >= c->rsize)
        {
            if (num_allocs == 4)
            {
                return gotdata;
            }
            ++num_allocs;
            char *new_rbuf = (char *)realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf)
            {
                if (g_settings.verbose > 0)
                {
                    log_debug(LOG_ERR, "Couldn't realloc input buffer\n");
                }
                c->rbytes = 0; /* ignore what we read */
                out_string(c, "SERVER_ERROR out of memory reading request");
                c->write_and_go = conn_closing;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        int avail = c->rsize - c->rbytes;
        res = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0)
        {
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail)
            {
                continue;
            }
            else
            {
                break;
            }
        }
        if (res == 0)
        {
            return READ_ERROR;
        }
        if (res == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return READ_ERROR;
        }
    }
    return gotdata;
}

/*
 * read a UDP request.
 */
enum try_read_result try_read_udp(conn *c)
{
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, &c->request_addr, &c->request_addr_size);
    if (res > 8)
    {
        unsigned char *buf = (unsigned char *)c->rbuf;

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1)
        {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return READ_NO_DATA_RECEIVED;
        }

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;
        return READ_DATA_RECEIVED;
    }
    return READ_NO_DATA_RECEIVED;
}

bool update_event(conn *c, const int new_flags)
{
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
    {
        return true;
    }
    if (event_del(&c->event) == -1)
    {
        return false;
    }
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1)
    {
        return false;
    }
    return true;
}

/*
 * Sets whether we are listening for new connections or not.
 */
void do_accept_new_conns(const bool do_accept)
{
    conn *next;

    for (next = listen_conn; next; next = next->next)
    {
        if (do_accept)
        {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, g_settings.backlog) != 0)
            {
                log_debug(LOG_ERR, "listen, error:%s\n", strerror(errno));
            }
        }
        else
        {
            update_event(next, 0);
            if (listen(next->sfd, 0) != 0)
            {
                log_debug(LOG_ERR, "listen, error:%s\n", strerror(errno));
            }
        }
    }
}

void event_handler(const int fd, const short which, void *arg)
{
    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd)
    {
        if (g_settings.verbose > 0)
        {
            log_debug(LOG_ERR, "Catastrophic: event fd doesn't match conn fd!\n");
        }
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}

int new_socket(struct addrinfo *ai)
{
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1)
    {
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        log_debug(LOG_ERR, "setting O_NONBLOCK, error:%s\n", strerror(errno));
        close(sfd);
        return -1;
    }
    return sfd;
}


/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
void maximize_sndbuf(const int sfd)
{
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0)
    {
        if (g_settings.verbose > 0)
        {
            log_debug(LOG_ERR, "getsockopt(SO_SNDBUF), error:%s\n", strerror(errno));
        }
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max)
    {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0)
        {
            last_good = avg;
            min = avg + 1;
        }
        else
        {
            max = avg - 1;
        }
    }

    if (g_settings.verbose > 1)
    {
        log_debug(LOG_ERR, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
    }
}

/**
 * Create a socket and bind it to a specific port number
 * @param interface the interface to bind to
 * @param port the port number to bind to
 * @param transport the transport protocol (TCP / UDP)
 * @param portnumber_file A filepointer to write the port numbers to
 *        when they are successfully added to the list of ports we
 *        listen on.
 */
int server_socket(const char *interface,
                  int port,
                  enum network_transport transport,
                  FILE *portnumber_file)
{
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints =
    {
ai_flags :
        AI_PASSIVE,
ai_family :
        AF_UNSPEC
    };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags = 1;

    hints.ai_socktype = SOCK_STREAM;

    if (port == -1)
    {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error = getaddrinfo(interface, port_buf, &hints, &ai);
    if (error != 0)
    {
        if (error != EAI_SYSTEM)
        {
            log_debug(LOG_ERR, "getaddrinfo(): %s\n", gai_strerror(error));
        }
        else
        {
            log_debug(LOG_ERR, "getaddrinfo(), error:%s\n", strerror(errno));
        }
        return 1;
    }

    for (next = ai; next; next = next->ai_next)
    {
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == -1)
        {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            if (errno == EMFILE)
            {
                /* ...unless we're out of fds */
                log_debug(LOG_ERR, "server_socket, error:%s\n", strerror(errno));
                exit(EX_OSERR);
            }
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6)
        {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &flags, sizeof(flags));
            if (error != 0)
            {
                log_debug(LOG_ERR, "setsockopt, error:%s\n", strerror(errno));
                close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));

        error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
        if (error != 0)
        {
            log_debug(LOG_ERR, "setsockopt, error:%s\n", strerror(errno));
        }

        error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
        if (error != 0)
        {
            log_debug(LOG_ERR, "setsockopt, error:%s\n", strerror(errno));
        }

        error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
        if (error != 0)
        {
            log_debug(LOG_ERR, "setsockopt, error:%s\n", strerror(errno));
        }

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1)
        {
            if (errno != EADDRINUSE)
            {
                log_debug(LOG_ERR, "bind(), error:%s\n", strerror(errno));
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        }
        else
        {
            success++;
            if (listen(sfd, g_settings.backlog) == -1)
            {
                log_debug(LOG_ERR, "listen(), error:%s\n", strerror(errno));
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if (portnumber_file != NULL &&
                (next->ai_addr->sa_family == AF_INET ||
                 next->ai_addr->sa_family == AF_INET6))
            {
                union
                {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr *)&my_sockaddr, &len) == 0)
                {
                    if (next->ai_addr->sa_family == AF_INET)
                    {
                        fprintf(portnumber_file, "%s INET: %u\n",
                                "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    }
                    else
                    {
                        fprintf(portnumber_file, "%s INET6: %u\n",
                                "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }


        if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                         EV_READ | EV_PERSIST, 1,
                                         transport, main_base)))
        {
            log_debug(LOG_ERR, "failed to create listening connection\n");
            exit(EXIT_FAILURE);
        }
        listen_conn_add->next = listen_conn;
        listen_conn = listen_conn_add;
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

int server_sockets(int port, enum network_transport transport,
                   FILE *portnumber_file)
{
    return server_socket(NULL, port, transport, portnumber_file);
}

int new_socket_unix(void)
{
    int sfd;
    int flags;

    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
        log_debug(LOG_ERR, "socket(), error:%s\n", strerror(errno));
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        log_debug(LOG_ERR, "setting O_NONBLOCK, error:%s\n", strerror(errno));
        close(sfd);
        return -1;
    }
    return sfd;
}

int server_socket_unix(const char *path, int access_mask)
{
    int sfd;
    struct linger ling = {0, 0};
    struct sockaddr_un addr;
    struct stat tstat;
    int flags = 1;
    int old_umask;

    if (!path)
    {
        return 1;
    }

    if ((sfd = new_socket_unix()) == -1)
    {
        return 1;
    }

    /*
     * Clean up a previous socket file if we left it around
     */
    if (lstat(path, &tstat) == 0)
    {
        if (S_ISSOCK(tstat.st_mode))
        {
            unlink(path);
        }
    }

    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    /*
     * the memset call clears nonstandard fields in some impementations
     * that otherwise mess things up.
     */
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    assert(strcmp(addr.sun_path, path) == 0);
    old_umask = umask(~(access_mask & 0777));
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        log_debug(LOG_ERR, "bind(), error:%s\n", strerror(errno));
        close(sfd);
        umask(old_umask);
        return 1;
    }
    umask(old_umask);
    if (listen(sfd, g_settings.backlog) == -1)
    {
        log_debug(LOG_ERR, "listen(), error:%s\n", strerror(errno));
        close(sfd);
        return 1;
    }
    if (!(listen_conn = conn_new(sfd, conn_listening,
                                 EV_READ | EV_PERSIST, 1,
                                 local_transport, main_base)))
    {
        log_debug(LOG_ERR, "failed to create listening connection\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}



