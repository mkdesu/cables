/*
  NOT thread-safe!
*/

#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "process.h"
#include "util.h"


/* fast shutdown indicator */
static volatile int stop;


/*
  process counters (not used if initok is 0)
  pstarted:  incremented in run_process()
  pfinished: incremented in chld_handler()
*/
static volatile long pstarted = 0, pfinished = 0;
static          int  initok;


int stop_requested() {
    return stop;
}


static void stop_handler(int signum) {
    assert(signum == SIGINT  ||  signum == SIGTERM);
    if (signum == SIGINT  ||  signum == SIGTERM) {
        if (!stop) {
            stop = 1;

#ifndef TESTING
            /* kill pgroup; also sends signal to self, but stop=1 prevents recursion */
            kill(0, SIGTERM);
#endif
        }
    }
}


static void chld_handler(int signum) {
    pid_t pid;
    int   status;

    assert(signum == SIGCHLD);
    if (signum == SIGCHLD) {
        /*
          multiple instances of pending signals are compressed, so
          handle all completed processes as soon as possible
        */
        while ((pid = waitpid(0, &status, WNOHANG)) > 0)
            ++pfinished;

        /* -1/ECHILD is returned for no pending completed processes */
        assert(pid != -1  ||  errno == ECHILD);
    }
}


int init_process_acc() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    stop = 0;

    /* restart system calls interrupted by SIGCHLD */
    sa.sa_handler  = chld_handler;
    sa.sa_flags    = SA_RESTART;

    /* SIGCHLD is automatically added to the mask */
    initok =    !sigemptyset(&sa.sa_mask)
             && !sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler  = stop_handler;
    sa.sa_flags    = 0;

    initok =    initok
             && !sigaction(SIGINT,  &sa, NULL)
             && !sigaction(SIGTERM, &sa, NULL);

    return initok;
}


int run_process(long maxproc, double waitsec, const char *const argv[]) {
    int   res = 0;
    long  pcount;
    pid_t pid;

    /*
      wait if too many processes have been launched
      SIGCHLD from terminated processes also interrupts sleep
    */
    while (initok  &&  !stop_requested()  &&  (pcount = pstarted - pfinished) >= maxproc) {
        flog(LOG_NOTICE, "too many processes (%ld), waiting...", pcount);
        sleepsec(waitsec);
    }

    if (!stop_requested()) {
        if ((pid = fork()) == -1)
            warning("fork failed");
        else if (pid == 0) {
            /* modifiable strings signature seems to be historic */
            execvp(argv[0], (char *const *) argv);

            /* exits just the fork */
            error("loop execution failed");
        }
        else {
            ++pstarted;
            res = 1;
        }
    }

    return res;
}
