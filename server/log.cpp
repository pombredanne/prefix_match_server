#include "log.h"
#include "util.h"

#define MAX_LOG_LEVEL 10

struct logger logger;

const int log_cfg_num = 8;
log_conf log_cfg[MAX_LOG_LEVEL] =
{
    {"LOG_EMERG",       LOG_EMERG},
    {"LOG_ALERT",       LOG_ALERT},
    {"LOG_CRIT",        LOG_CRIT},
    {"LOG_ERR",         LOG_ERR},
    {"LOG_WARN",        LOG_WARN},
    {"LOG_NOTICE",      LOG_NOTICE},
    {"LOG_INFO",        LOG_INFO},
    {"LOG_DEBUG",       LOG_DEBUG},
};

static int log_open_file(char *name)
{
    int fd = 0;
    if (access(name, W_OK) == 0) //file exist
    {
        fd = open(name, O_WRONLY | O_APPEND, 0);
    }
    else
    {
        fd = open(name, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd > 0)
        {
            return fd;
        }

        char tmp;
        char *dir = strrchr(name, '/');
        if (NULL == dir)
        {
            printf("the file must be a dir\n");
            return -1;
        }
        tmp = *dir;
        *dir = '\0';
        if (false == mkdir_recursive(name, (mode_t)0644))
        {
            printf("the mkdir recursive failed, the dir is %s\n", name);
            return -1;
        }
        *dir = tmp;
        fd = open(name, O_WRONLY | O_APPEND | O_CREAT, 0644);
    }
    if (fd < 0)
    {
        printf("opening log file '%s' failed: %s", name, strerror(errno));
        return -1;
    }

    return fd;
}

static int my_write(int fd, char *buffer, int size)
{
    while (1)
    {
        if (write(fd, buffer, size) < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }
            else
            {
                return -1;
            }
        }
        else
        {
            return 0;
        }
    }
    return 0;
}

static void *async_write(void *argv)
{
    struct logger *l = &logger;

    pthread_detach(pthread_self());
    while (1)
    {
        mysleep_millisec(l->write_interval);
        if (l->nerror == 1)
        {
            close(l->fd);
            l->fd = -1;
            if (log_open_file(l->name) < 0)
            {
                printf("open log file failed\n");
                continue;
            }
        }

        int wpos = 0;
        int index = l->buffer_index;
        l->buffer_index = (l->buffer_index + 1) % 2;

        pthread_mutex_lock(&l->buffer_locks[index]);
        wpos = l->buffer_wpos[index];

        if (wpos > 0 && my_write(l->fd, l->buffer[index], wpos) < 0)
        {
            l->nerror = 1;
        }
        l->buffer_wpos[index] = 0;
        pthread_mutex_unlock(&l->buffer_locks[index]);
    }
}

//file init
int log_init_file(char *name)
{
    struct logger *l = &logger;

    l->max_buffer_size = 1024 * 1024 * 10;
    l->write_interval = 100;
    l->buffer_index = 0;
    l->name = strdup(name);
    if (name == NULL || !strlen(name))
    {
        l->fd = STDERR_FILENO;
        return 0;
    }
    else
    {
        l->fd = log_open_file(name);
        if (l->fd < 0)
        {
            l->fd = STDERR_FILENO;
            return 0;
        }
    }

    //init buffer
    int i = 0;
    while (i < 2)
    {
        pthread_mutex_init(&l->buffer_locks[i], NULL);
        char *buffer = (char *)calloc(1, l->max_buffer_size);
        if (NULL == buffer)
        {
            goto failed;
        }
        l->buffer[i] = buffer;
        l->buffer_wpos[i] = 0;
        i++;
    }

    //init buffer write thread
    if (pthread_create(&l->file_async_writer, NULL, async_write, NULL) < 0)
    {
        goto failed;
    }

    l->inited = 1;
    return 0;

failed:
    i = 0;
    while (i < 2)
    {
        if (l->buffer[i] != NULL)
        {
            free(l->buffer[i]);
        }
        i++;
    }
    return -1;
}

int log_deinit_file()
{
    struct logger *l = &logger;

    int i = 0;
    while (i < 2)
    {
        if (l->buffer[i] != NULL)
        {
            free(l->buffer[i]);
        }
        i++;
    }

    free(l->name);
    l->name = NULL;

    if (l->fd < 0 || l->fd == STDERR_FILENO)
    {
        return 0;
    }

    close(l->fd);
    return 0;
}

int log_init_db(char *server, short port)
{
    struct logger *l = &logger;

    return 0;
}

int log_deinit_db()
{
    return 0;
}

int log_init_leveldb(const char *name)
{
    struct logger *l = &logger;

    return 0;
}

int log_deinit_leveldb()
{
    struct logger *l = &logger;

    return 0;
}

int parse_loglevel(char *loglevel)
{
    int i;
    int level_log = -1;
    for (i = 0; i < log_cfg_num; i++)
    {
        if (strcasestr(loglevel, log_cfg[i].cfg_key) != NULL)
        {
            level_log = log_cfg[i].cfg_shift;
            break;
        }
    }

    printf("level_log = 0X%x\n", level_log);
    return level_log;
}

int log_init(int type, char *level, char *name, char *server, short port)
{
    struct logger *l = &logger;

    int levellog = parse_loglevel(level);
    l->level = MAX(LOG_EMERG, MIN(levellog, LOG_DEBUG));
    l->type = type;
    l->nerror = 0;

    if (type & GENERIC_FILE)
    {
        return log_init_file(name);
    }

    if (type & DATABASE)
    {
        return log_init_db(server, port);
    }

    if (type & LEVELDB)
    {
        return log_init_leveldb(name);
    }

    return 0;
}

void log_deinit(void)
{
    struct logger *l = &logger;

    int ret = 0;
    int type = l->type;
    if (type & GENERIC_FILE)
    {
        ret = log_init_file(l->name);
    }

    if (type & DATABASE)
    {
        ret = log_init_db(l->ip, l->port);
    }

    if (type & LEVELDB)
    {
        ret = log_init_leveldb(l->name);
    }

    return;
}

void log_reopen(void)
{
    struct logger *l = &logger;

    int ret = 0;
    int type = l->type;
    if (type & GENERIC_FILE)
    {
        close(l->fd);
        l->fd = log_open_file(l->name);
        if (l->fd < 0)
        {
            log_debug(LOG_DEBUG, "reopening log file '%s' failed, ignored: %s", l->name, strerror(errno));
        }
    }

    if (type & DATABASE)
    {
        ret = log_init_db(l->ip, l->port);
    }

    if (type & LEVELDB)
    {
        ret = log_init_leveldb(l->name);
    }
}

void log_level_up(void)
{
    struct logger *l = &logger;

    if (l->level < LOG_DEBUG)
    {
        l->level++;
        loga("up log level to %d", l->level);
    }
}

void log_level_down(void)
{
    struct logger *l = &logger;

    if (l->level > LOG_EMERG)
    {
        l->level--;
        loga("down log level to %d", l->level);
    }
}

void log_level_set(int level)
{
    struct logger *l = &logger;

    l->level = MAX(LOG_EMERG, MIN(level, LOG_DEBUG));
    loga("set log level to %d", l->level);
}

int log_loggable(int level)
{
    struct logger *l = &logger;

    if (level > l->level)
    {
        return 0;
    }

    return 1;
}

static int _log_internal(char *str_buf, int len)
{
    struct logger *l = &logger;
    int index = 0;
    int wpos = 0;

    if (NULL == str_buf || len == 0)
    {
        return 0;
    }

    index = l->buffer_index;
    pthread_mutex_lock(&l->buffer_locks[index]);

    wpos = l->buffer_wpos[index];
    if (wpos + len < l->max_buffer_size)
    {
        strncpy(l->buffer[index] + wpos, str_buf, len);
        l->buffer_wpos[index] += len;
    }
    else
    {
        if (wpos > 0 && my_write(l->fd, l->buffer[index], wpos) < 0)
        {
            l->nerror = 1;
        }
        l->buffer_wpos[index] = 0;
        strncpy(l->buffer[index], str_buf, len);
        l->buffer_wpos[index] += len;
    }

    pthread_mutex_unlock(&l->buffer_locks[index]);
    return 0;
}

static void get_timestamp(char *txt, int len)
{
    time_t gmt;
    struct tm *timeptr;
    struct tm timestruct;

    time(&gmt);
    timeptr = localtime_r(&gmt, &timestruct);
    snprintf(txt, len,
             "%04d.%02d.%02d %02d:%02d:%02d",
             timeptr->tm_year + 1900, timeptr->tm_mon + 1, timeptr->tm_mday,
             timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec);
}

void _log(const char *file, const char *func, int line, const char *fmt, ...)
{
#define STRLEN 256

    struct logger *l = &logger;
    int len, size, errno_save;
    char buf[LOG_MAX_LEN] = {0}, stamp[STRLEN] = {0};
    va_list args;
    int n = 0;

    if (l->inited == 0)
    {
        _log_stderr(fmt);
        return;
    }

    if (l->fd < 0)
    {
        return;
    }

    errno_save = errno;
    len = 0;            /* length of output buffer */
    size = LOG_MAX_LEN; /* size of output buffer */

    get_timestamp(stamp, STRLEN);
    len += snprintf(buf + len, size - len, "[%s] %s:%s:%d ",
                    stamp, file, func, line);

    va_start(args, fmt);
    len += vsnprintf(buf + len, size - len, fmt, args);
    va_end(args);

    if (buf[len - 1] != '\n')
    {
        buf[len++] = '\n';
        buf[len] = '\0';
    }

    _log_internal(buf, len);

    errno = errno_save;
}

void _log_stderr(const char *fmt, ...)
{
    struct logger *l = &logger;
    int len, size, errno_save;
    char buf[4 * LOG_MAX_LEN];
    va_list args;
    ssize_t n;

    errno_save = errno;
    len = 0;                /* length of output buffer */
    size = 4 * LOG_MAX_LEN; /* size of output buffer */

    va_start(args, fmt);
    len += vsnprintf(buf, size, fmt, args);
    va_end(args);

    buf[len++] = '\n';

    n = write(STDERR_FILENO, buf, len);
    if (n < 0)
    {
        l->nerror++;
    }

    errno = errno_save;
}

/*
 * Hexadecimal dump in the canonical hex + ascii display
 * See -C option in man hexdump
 */
void _log_hexdump(const char *file, const char *func, int line, char *data, int datalen,
                  const char *fmt, ...)
{
    struct logger *l = &logger;
    char buf[8 * LOG_MAX_LEN];
    int i, off, len, size, errno_save;
    ssize_t n;

    if (l->fd < 0 || l->inited > 0)
    {
        return;
    }

    /* log hexdump */
    errno_save = errno;
    off = 0;                  /* data offset */
    len = 0;                  /* length of output buffer */
    size = 8 * LOG_MAX_LEN;   /* size of output buffer */

    while (datalen != 0 && (len < size - 1))
    {
        char *save, *str;
        unsigned char c;
        int savelen;

        len += snprintf(buf + len, size - len, "%08x  ", off);

        save = data;
        savelen = datalen;

        for (i = 0; datalen != 0 && i < 16; data++, datalen--, i++)
        {
            c = (unsigned char)(*data);
            str = (i == 7) ? (char *)"  " : (char *)" ";
            len += snprintf(buf + len, size - len, "%02x%s", c, str);
        }
        for (; i < 16; i++)
        {
            str = (i == 7) ? (char *)"  " : (char *)" ";
            len += snprintf(buf + len, size - len, "  %s", str);
        }

        data = save;
        datalen = savelen;

        len += snprintf(buf + len, size - len, "  |");

        for (i = 0; datalen != 0 && i < 16; data++, datalen--, i++)
        {
            c = (unsigned char)(isprint(*data) ? *data : '.');
            len += snprintf(buf + len, size - len, "%c", c);
        }
        len += snprintf(buf + len, size - len, "|\n");

        off += 16;
    }

    n = _log_internal(buf, len);
    if (n < 0)
    {
        l->nerror++;
    }

    errno = errno_save;
}


#ifdef MAIN_TEST

int main()
{
    char *filename = (char *)"/var/mylog";
    int ret = log_init(LOG_WARN, filename);
    printf("ret: %d\n", ret);

    loga("for test");
    loga("for test %d", 1);
    loga("for test %s", "test");

    return 0;
}

#endif

