#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "head.h"

#define SERV_PORT 10000

static int my_read(int fd, void *buf, int len)
{
    unsigned int ret = 0, nrecv = 0;

    for (nrecv = 0; nrecv < len; nrecv += ret)
    {
        ret = read(fd, (char *)buf + nrecv, len - nrecv);
        if (ret <= 0)
        {
            return -1;
        }
    }

    return nrecv;
}

int connect_server(int &connfd)
{
    struct sockaddr_in serveraddr;
    connfd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    char *local_addr = (char *)"127.0.0.1";
    inet_aton(local_addr, &(serveraddr.sin_addr));
    serveraddr.sin_port = htons(SERV_PORT);
    int status = connect(connfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
    if (status != 0)
    {
        perror("connect error\n");
        return -1;
    }
}

void *send_cmd_tcp(void *)
{
    pthread_detach(pthread_self());

    int connfd = 0;
    int sendfd = 0;

    connect_server(connfd);

    char buf[1024] = "liu";
    uint32_t number = 10;
    int bsize = strlen(buf) + sizeof(number);
    int tsize = sizeof(request_header) + bsize;

    char *msg = (char *)calloc(1, tsize + 1);
    if (msg == NULL)
    {
        return NULL;
    }

    request_header *head = (request_header *)msg;
    head->request.magic = REQ;
    head->request.bodylen = (uint32_t)htonl(bsize);

    char *data = msg + sizeof(request_header);
    memcpy(data, &number, sizeof(number));
    memcpy(data + sizeof(number), buf, strlen(buf));

    while (1)
    {
        int sz = write(connfd, msg, tsize);
        if (tsize == sz)
        {
            printf("send ok\n");
            response_header rep;
            int header_size = read(connfd, &rep, sizeof(rep));
            if (rep.response.magic != RES)
            {
                printf("magic error\n");
            }
            else
            {
                int len = ntohl(rep.response.bodylen);
                char *rep = (char *)calloc(1, len + 1);
                if (rep == NULL)
                {
                    printf("malloc failed \n");
                    usleep(1000);
                    continue;
                }
                int ret = 0;
                if ((ret = my_read(connfd, rep, len)) != len)
                {
                    printf("read body error, ret: %d\n", ret);
                }
                else
                {
                    char *trep = rep;
                    uint32_t number = *(uint32_t *)trep;
                    trep += sizeof(uint32_t);
                    for (int i = 0; i < number; i++)
                    {
                        char buffer[8096] = {0};
                        memset(buffer, 0, sizeof(buffer));

                        uint32_t len = *(uint32_t *)trep;
                        trep += sizeof(uint32_t);
                        memcpy(buffer, trep, len);
                        trep += len;

                        printf("%s\n", buffer);
                    }
                }
                free(rep);
            }
        }
        else
        {
            printf("send error\n");
            if (connfd > 0)
            {
                close(connfd);
            }
            connect_server(connfd);
        }
        usleep(100);
    }

    free(msg);
    return NULL;
}

#if 0
struct sockaddr_in udpserveraddr;
static uint16_t req_id = 0;
int send_cmd_udp()
{
    char buf[1024] = "songweisongwei";
    int size = sizeof(struct udp_datagram_header) + sizeof(request_header) + strlen(buf);
    char *msg = (char *)malloc(size);

    struct udp_datagram_header *udp_header = (struct udp_datagram_header *)msg;
    udp_header->request_id = (uint16_t)htons(req_id++);
    udp_header->num_datagrams = (uint16_t)htons(1);

    request_header *head = (request_header *)(msg + sizeof(struct udp_datagram_header));
    head->request.magic = REQ;
    head->request.bodylen = (uint32_t)htonl(strlen(buf));

    char *data = msg + sizeof(request_header) + sizeof(struct udp_datagram_header);
    memcpy(data, buf, strlen(buf));

    while (1)
    {
        int sz = sendto(sendfd, msg, size, 0, (struct sockaddr *)&udpserveraddr, sizeof(udpserveraddr));
        if (size == sz)
        {
            printf("send ok\n");
        }
        else
        {
            printf("send error\n");
        }
        usleep(100);
    }

    free(msg);
    return 0;
}
#endif

int main(int argc, char **argv)
{
#if 0
    int tcp = 0;
    if (strcmp(argv[1], "tcp") == 0)
    {
        tcp = 1;
    }
#endif

#if 0
    //udp
    sendfd = socket(AF_INET, SOCK_DGRAM, 0);
    bzero(&udpserveraddr, sizeof(udpserveraddr));
    udpserveraddr.sin_family = AF_INET;
    char *udp_addr = (char *)"127.0.0.1";
    inet_aton(udp_addr, &(udpserveraddr.sin_addr));
    udpserveraddr.sin_port = htons(SERV_PORT);
#endif
#if 0
    //send msg
    if (tcp == 1)
    {
#endif

        sigignore(SIGPIPE);

        int thread_num = 10;
        pthread_t threads[100];
        for (int i = 0; i < thread_num; i++)
        {
            pthread_create(&threads[i], NULL, send_cmd_tcp, NULL);
        }

        while (1)
        {
            pause();
        }

#if 0
    }
    else
    {
        send_cmd_udp();
    }
#endif
    return 0;
}
