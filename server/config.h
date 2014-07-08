#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <pthread.h>

#define LISTEN_PORT 10000

/**
 * Globally accessible settings as derived from the commandline.
 */
typedef struct settings
{
    //config file
    char *cfgpath;
    char *username;
    char *pidfile;

    //network settings
    int maxconns;
    int port; /* listen port*/
    char *socketpath; /* path to unix socket if using local socket */
    int access;  /* access mask (a la chmod) for unix domain socket */
    int num_threads;        /* number of worker (without dispatcher) libevent threads to run */
    int num_threads_per_udp; /* number of worker threads serving each udp socket */
    char prefix_delimiter;  /* character that marks a key prefix (for stats) */
    int reqs_per_event;     /* Maximum number of io to process on each io-event. */
    int backlog;
    int verbose;

    //index file
    char *index_path;
    char *chinese_map_file;
    int max_depth;

    //logs
    char *log_path;
    char *log_level;

    //monitor
    unsigned short monitor_port;
    int monitor_timeout;
} settings;

extern pthread_rwlock_t g_rwsetlock;
extern struct settings g_settings;

/* defaults */
void settings_init(void);
void parse_config();
void output_settings(settings *pset);
int check_settings();

#endif
