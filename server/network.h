#ifndef _NETWORK_H
#define _NETWORK_H

#include "conn.h"

enum try_read_result
{
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occured (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};

enum try_read_result try_read_network(conn *c);
bool update_event(conn *c, const int new_flags);

void do_accept_new_conns(const bool do_accept);
void event_handler(const int fd, const short which, void *arg);

int new_socket(struct addrinfo *ai);
void maximize_sndbuf(const int sfd);
int server_socket(const char *interface,
                  int port,
                  enum network_transport transport,
                  FILE *portnumber_file);

int server_sockets(int port, enum network_transport transport,
                   FILE *portnumber_file);

int new_socket_unix(void);

int server_socket_unix(const char *path, int access_mask);

#endif
