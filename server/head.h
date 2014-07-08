#ifndef __HEAD_H__
#define __HEAD_H__

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <ctype.h>
#include <stdarg.h>
#include <pwd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <sysexits.h>
#include <stddef.h>

#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <event.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>

/**
 * Possible states of a connection.
 */
enum conn_states
{
    conn_listening,  /**< the socket which listens for connections */
    conn_new_cmd,    /**< Prepare connection for next command */
    conn_waiting,    /**< waiting for a readable socket */
    conn_read,       /**< reading in a command line */
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      /**< writing out a simple response */
    conn_nread,      /**< reading in a fixed number of bytes */
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    /**< closing this connection */
    conn_mwrite,     /**< writing out many items sequentially */
    conn_max_state   /**< Max state value (used for assertion) */
};

enum network_transport
{
    local_transport, /* Unix sockets*/
    tcp_transport,
    udp_transport
};

/** Maximum length of a key. */
#define KEY_MAX_LENGTH 250

/** Size of an incr buf. */
#define INCR_MAX_STORAGE_LEN 24

#define DATA_BUFFER_SIZE 2048
#define UDP_READ_BUFFER_SIZE 65536
#define UDP_MAX_PAYLOAD_SIZE 1400
#define UDP_HEADER_SIZE 8
#define MAX_SENDBUF_SIZE (256 * 1024 * 1024)

/** Initial size of the sendmsg() scatter/gather array. */
#define IOV_LIST_INITIAL 400

/** Initial number of sendmsg() argument structures to allocate. */
#define MSG_LIST_INITIAL 10

/** High water marks for buffer shrinking */
#define READ_BUFFER_HIGHWAT 8192
#define ITEM_LIST_HIGHWAT 400
#define IOV_LIST_HIGHWAT 600
#define MSG_LIST_HIGHWAT 100

/* Binary protocol stuff */
#define MIN_BIN_PKT_LENGTH 16
#define BIN_PKT_HDR_WORDS (MIN_BIN_PKT_LENGTH/sizeof(uint32_t))

#define MAX_VERBOSITY_LEVEL 2

/**
 * This file contains definitions of the constants and packet formats
 * defined in the binary specification. Please note that you _MUST_ remember
 * to convert each multibyte field to / from network byte order to / from
 * host order.
 */
#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Definition of the legal "magic" values used in a packet.
     * See section 3.1 Magic byte
     */
    typedef enum
    {
        REQ = 0x80,
        RES = 0x81
    }
          magic;

    /**
     * Definition of the valid response status numbers.
     * See section 3.2 Response Status
     */
    typedef enum
    {
        RESPONSE_SUCCESS = 0x00,
        RESPONSE_ENOMEM = 0x82
    } response_status;

    /**
     * Defintion of the different command opcodes.
     * See section 3.3 Command Opcodes
     */
    typedef enum
    {
        CMD_GET = 0x00,
    } binary_command;

    /**
     * Definition of the header structure for a request packet.
     * See section 2
     */
    typedef union
    {
        struct
        {
            uint8_t magic;
            uint8_t opcode;
            uint32_t bodylen;
            uint8_t opaque[0];
        } request;
        uint8_t bytes[8];
    } request_header;

    /**
     * Definition of the header structure for a response packet.
     * See section 2
     */
    typedef union
    {
        struct
        {
            uint8_t magic;
            uint8_t status;
            uint32_t bodylen;
            uint8_t  opaque[0];
        } response;
        uint8_t bytes[8];
    } response_header;

    typedef struct request
    {
        request_header header;
        uint8_t body[0];
    } request;

    typedef struct response
    {
        response_header header;
        uint8_t body[0];
    } response;

#ifdef __cplusplus
}
#endif

#endif /* H */

