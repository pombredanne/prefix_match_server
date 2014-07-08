#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

struct settings g_settings;
pthread_rwlock_t g_rwsetlock = PTHREAD_RWLOCK_INITIALIZER;

void settings_init(void)
{
    g_settings.cfgpath = NULL;
    g_settings.username = NULL;
    g_settings.pidfile = NULL;
    g_settings.log_path = NULL;

    g_settings.port = LISTEN_PORT;
    g_settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    g_settings.verbose = 0;
    g_settings.socketpath = NULL;       /* by default, not using a unix socket */
    g_settings.num_threads = 4;         /* N workers */
    g_settings.reqs_per_event = 20;
    g_settings.backlog = 1024;
    g_settings.index_path = NULL;
    g_settings.chinese_map_file = NULL;
    g_settings.max_depth = 1024;
    g_settings.monitor_timeout = 10;
}

void parse_config()
{
#define bufsize 1024
    char buf[bufsize] = {0};
    if (strlen(g_settings.cfgpath) == 0)
    {
        return;
    }
    FILE *fp = fopen(g_settings.cfgpath, "r+");
    if (!fp)
    {
        return;
    }

    while (fgets(buf, bufsize, fp) != NULL)
    {
        buf[strlen(buf) - 1] = '\0';
        if (buf[strlen(buf) - 1] == '\r')
        {
            buf[strlen(buf) - 1] = '\0';
        }

        char *split = (char *)" \t";
        delstr(buf, split);
        if (strlen(buf) == 0 || *buf == '#')
        {
            continue;
        }

        char equal = '=';
        char *result[MAX_KEYWORD_LENGTH];
        int size;
        parse_string_fast(buf, &equal, result, &size);
        if (size != 2)
        {
            continue;
        }

#define set_config_int(str, param) do{ \
        if(!strncmp(str, result[0], strlen(str))) \
            g_settings.param = (int)atoi(result[1]);\
    } while(0)

#define set_config_short(str, param) do{ \
        if(!strncmp(str, result[0], strlen(str))) \
            g_settings.param = (short)atoi(result[1]);\
    } while(0)

#define set_config_str(str, param) do{ \
        if(!strncmp(str, result[0], strlen(str))) \
        {    \
            g_settings.param = strdup(result[1]); \
        } \
    } while(0)

        set_config_str("username", username);
        set_config_str("pidfile", pidfile);

        set_config_str("unixpath", socketpath);
        set_config_int("port", port);
        set_config_int("verbose", verbose);
        set_config_int("maxconn", maxconns);
        set_config_int("verbose", verbose);
        set_config_int("threads", num_threads);
        set_config_int("backlog", backlog);
        set_config_int("max_requests", reqs_per_event);
        set_config_str("chinese_map_file", chinese_map_file);
        set_config_str("index_file", index_path);
        set_config_int("max_depth", max_depth);
        set_config_str("log_path", log_path);
        set_config_str("log_level", log_level);
        set_config_short("monitor_port", monitor_port);
        set_config_int("monitor_timeout", monitor_timeout);
    }

    fclose(fp);
    return;
#undef set_config_int
#undef set_config_short
#undef set_config_str
}

void output_settings(settings *pset)
{
    printf("username: %s\n", g_settings.username);
    printf("pidfile: %s\n", g_settings.pidfile);

    printf("maxconns: %d\n", g_settings.maxconns);
    printf("tcpport: %d\n", g_settings.port);
    printf("verbosity: %d\n", g_settings.verbose);
    printf("domain_socket: %s\n", g_settings.socketpath ? g_settings.socketpath : "NULL");
    printf("umask: %o\n", g_settings.access);
    printf("num_threads: %d\n", g_settings.num_threads);
    printf("tcp_backlog: %d\n", g_settings.backlog);
    printf("py_file: %s\n", g_settings.chinese_map_file);
    printf("index_file: %s\n", g_settings.index_path);
    printf("max_depth: %d\n", g_settings.max_depth);
    printf("log_path: %s\n", g_settings.log_path);
    printf("log_level: %s\n", g_settings.log_level);
    printf("monitor port: %d\n", g_settings.monitor_port);
    printf("monitor_timeout: %d\n", g_settings.monitor_timeout);
}

int check_settings()
{
#define CHECK(param, str) do{\
        if(strlen(g_settings.param)==0) \
        {\
            printf("param %s error\n", str);\
            return -1;\
        }\
    }while(0)

    CHECK(cfgpath, "cfgpath");
    CHECK(index_path, "index_path");
    CHECK(chinese_map_file, "chinese_map_file");
    return 0;
#undef CHECK
}

