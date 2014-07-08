#include <sys/types.h>
#include <sys/wait.h>
#include "comm.h"
#include "sig.h"
#include "prefixmatch.h"
#include "log.h"

pthread_t sig_handler_thread;

int sig_handler_user1(int signo);
int sig_handler_user2(int signo);
int sig_handler_ttin(int signo);
int sig_handler_ttou(int signo);
int sig_handler_hup(int signo);
int sig_handler_int(int signo);
int sig_handler_child(int signo);

static struct signal signals[] =
{
    {
        SIGUSR1,
        (char *)"SIGUSR1",
        (char *)"user1",
        0,
        sig_handler_user1
    },

    {
        SIGUSR2,
        (char *)"SIGUSR2",
        (char *)"user2",
        0,
        sig_handler_user2
    },

    {
        SIGTTIN,
        (char *)"SIGTTIN",
        (char *)"up logging level",
        0,
        sig_handler_ttin
    },

    {
        SIGTTOU,
        (char *)"SIGTTOU",
        (char *)"down logging level",
        0,
        sig_handler_ttou
    },

    {
        SIGHUP,
        (char *)"SIGHUP",
        (char *)"reopening log file",
        0,
        sig_handler_hup
    },

    {
        SIGINT,
        (char *)"SIGINT",
        (char *)"exiting",
        0,
        sig_handler_int
    },

    null_signal
};

void signal_handler(int signo);

void *sigmgr_thread(void *arg)
{
    sigset_t waitset, oset;
    siginfo_t info;
    int rc;

    pthread_detach(sig_handler_thread);
    sigemptyset(&waitset);

    struct signal *sig;
    for (sig = signals; sig->signo != 0; sig++)
    {
        sigaddset(&waitset, sig->signo);
    }

    while (1)
    {
        rc = sigwaitinfo(&waitset, &info);
        if (rc != -1)
        {
            log_debug(LOG_ERR, "sigwaitinfo() fetch the signal - %d\n", rc);
            signal_handler(info.si_signo);
        }
        else
        {
            log_debug(LOG_ERR, "sigwaitinfo() returned err: %d; %s\n", errno, strerror(errno));
        }
    }
}

int signal_init(void)
{
    struct signal *sig;

    if (sigignore(SIGPIPE) == -1)
    {
        log_debug(LOG_ERR, "failed to ignore SIGPIPE; error: %s\n", strerror(errno));
        exit(-1);
    }

    //block signal in current thread, unblock signal in thread: sig_handler_thread
    sigset_t bset, oset;
    sigemptyset(&bset);
    for (sig = signals; sig->signo != 0; sig++)
    {
        sigaddset(&bset, sig->signo);
    }

    if (pthread_sigmask(SIG_BLOCK, &bset, &oset) == -1)
    {
        log_debug(LOG_ERR, "error");
        return -1;
    }

    //create thread
    pthread_create(&sig_handler_thread, NULL, sigmgr_thread, NULL);

    return 0;
}

void signal_deinit(void)
{
}

void signal_handler(int signo)
{
    struct signal *sig;
    for (sig = signals; sig->signo != 0; sig++)
    {
        if (sig->signo == signo)
        {
            log_debug(LOG_ERR, "recevie signal : %s, description: %s\n", sig->signame, sig->actionstr);
            sig->handler(signo);
            return;
        }
    }
}

int sig_handler_user1(int signo)
{
    Reload_index(NULL);
    return 0;
}

int sig_handler_user2(int signo)
{

    return 0;
}

int sig_handler_ttin(int signo)
{
    return 0;
}

int sig_handler_ttou(int signo)
{

    return 0;
}

int sig_handler_hup(int signo)
{

    return 0;
}

int sig_handler_int(int signo)
{
    exiting();
    usleep(100);
    exit(-1);
    return 0;
}

