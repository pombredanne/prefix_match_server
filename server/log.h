#ifndef __LOG_H__
#define __LOG_H__

#include "comm.h"
#include "util.h"

struct logger
{
    //global
    int  level;     /* log level */
    int  nerror;    /* # log error */
    int  type;      /*log type*/
    int  inited;    /*log is inited*/

    //file
    char *name;  /* log file name */
    int  fd;     /* log file descriptor */
    int max_buffer_size; /*max buffer size*/
    int write_interval; /*write file interval*/
    pthread_mutex_t buffer_locks[2]; /*buffer locks*/
    volatile int buffer_index;   /*buffer index*/
    char *buffer[2];    /*buffers*/
    unsigned int buffer_wpos[2];    /*buffer write pos*/
    pthread_t file_async_writer; /*write buffer to file thread*/

    //database
    char *ip;   /*database server ip*/
    short port; /*database server port*/
    int batch;  /*support batch write*/
};

//logger type
#define GENERIC_FILE    0x01
#define DATABASE        0x02
#define LEVELDB         0x04

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

//log level
#define LOG_EMERG   0   /* system in unusable */
#define LOG_ALERT   1   /* action must be taken immediately */
#define LOG_CRIT    2   /* critical conditions */
#define LOG_ERR     3   /* error conditions */
#define LOG_WARN    4   /* warning conditions */
#define LOG_NOTICE  5   /* normal but significant condition (default) */
#define LOG_INFO    6   /* informational */
#define LOG_DEBUG   7   /* debug messages */

typedef struct log_conf
{
    const char *cfg_key;
    int cfg_shift;
} log_conf;
extern log_conf log_cfg[10];

#define LOG_MAX_LEN 1024 /* max length of log message */

/*
 * loga         - log always
 * loga_hexdump - log hexdump always
 * log_debug    - debug log messages based on a log level
 * log_hexdump  - hexadump -C of a log buffer
 */
#define log_debug(_level, ...) do {                                         \
        if (log_loggable(_level) != 0) {                                        \
            _log(__FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);                           \
        }                                                                       \
    } while (0)

#define log_hexdump(_level, _data, _datalen, ...) do {                      \
        if (log_loggable(_level) != 0) {                                        \
            _log(__FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);                           \
            _log_hexdump(__FILE__, __FUNCTION__, __LINE__, (char *)(_data), (int)(_datalen),  \
                         __VA_ARGS__);                                          \
        }                                                                       \
    } while (0)

#define loga(...) do {                                                      \
        _log(__FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);                               \
    } while (0)

int log_init(int type, char *level, char *filename, char *server, short port);
void log_deinit(void);
void log_level_up(void);
void log_level_down(void);
void log_level_set(int level);
void log_reopen(void);
int log_loggable(int level);
void _log(const char *file, const char *func, int line, const char *fmt, ...);
void _log_stderr(const char *fmt, ...);
void _log_hexdump(const char *file, char *func, int line, char *data, int datalen, const char *fmt, ...);

#endif
