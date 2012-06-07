/*
  The following environment variables are used (from /etc/cable/profile):
  CABLE_HOME, CABLE_QUEUES, CABLE_CERTS, CABLE_HOST, CABLE_PORT

  Testing environment:
  CABLE_NOLOOP, CABLE_NOWATCH
 */

#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <sys/select.h>

#include "daemon.h"
#include "server.h"
#include "util.h"


/* environment variables */
#define CABLE_HOME   "CABLE_HOME"
#define CABLE_QUEUES "CABLE_QUEUES"
#define CABLE_CERTS  "CABLE_CERTS"
#define CABLE_HOST   "CABLE_HOST"
#define CABLE_PORT   "CABLE_PORT"

/* executables and subdirectories */
#define LOOP_NAME    "loop"
#define QUEUE_NAME   "queue"
#define RQUEUE_NAME  "rqueue"
#define CERTS_NAME   "certs"


/* waiting strategy for inotify setup retries (e.g., after fs unmount) */
#define WAIT_INIT     2
#define WAIT_MULT   1.5
#define WAIT_MAX     60

/*
  retry and limits strategies
  wait time for too many processes can be long, since SIGCHLD interrupts sleep
*/
#ifndef TESTING
#define RETRY_TMOUT 150
#define MAX_PROC    100
#define WAIT_PROC   300
#else
#define RETRY_TMOUT 10
#define MAX_PROC    5
#define WAIT_PROC   5
#endif


/* inotify file descriptor and (r)queue directories watch descriptors */
static int inotfd = -1, inotqwd = -1, inotrqwd = -1;

/* fast shutdown indicator */
static volatile int stop = 0;

/* process counters */
static volatile long pstarted = 0, pfinished = 0;


/* non-fatal error */
static void warning() {
    flog(LOG_WARNING, "%m");
}

/* fatal errors which shouldn't happen in a correct program */
static void error() {
    flog(LOG_ERR, "%m");

    if (inotfd != -1  &&  close(inotfd) == -1)
        warning();

    exit(EXIT_FAILURE);
}


/* INT/TERM signals handler */
static void sig_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        if (!stop) {
            /* flog(LOG_NOTICE, "signal caught: %d", signum); */
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

    if (signum == SIGCHLD) {
        /*
          multiple instances of pending signals are compressed, so
          handle all completed processes as soon as possible
        */
        while ((pid = waitpid(0, &status, WNOHANG)) > 0)
            ++pfinished;

        /* -1/ECHILD is returned for no pending completed processes */
        if (pid == -1  &&  errno != ECHILD)
            _exit(EXIT_FAILURE);
    }
}


/* unregister inotify watches and dispose of allocated file descriptor */
static void unreg_watches() {
    if (inotfd != -1) {
        /* ignore errors due to automatically removed watches (IN_IGNORED) */
        if (inotqwd != -1   &&  inotify_rm_watch(inotfd, inotqwd)  == -1  &&  errno != EINVAL)
            error();
        else
            inotqwd = -1;

        /* ignore errors due to automatically removed watches (IN_IGNORED) */
        if (inotrqwd != -1  &&  inotify_rm_watch(inotfd, inotrqwd) == -1  &&  errno != EINVAL)
            error();
        else
            inotrqwd = -1;

        /*
          closing/reopening an inotify fd is an expensive operation, but must be done
          because otherwise fd provides infinite stream of IN_IGNORED events
        */
        if (close(inotfd) == -1) {
            inotfd = -1;
            error();
        }
        else
            inotfd = -1;
    }
}

/* register (r)queue-specific inotify watches, returning 1 if successful */
static int reg_watches(const char *qpath, const char *rqpath) {
    /* don't block on read(), since select() is used for polling */
    if (inotfd == -1  &&  (inotfd = inotify_init1(IN_NONBLOCK)) == -1)
        error();

#ifdef TESTING
    if (getenv("CABLE_NOWATCH"))
        return 1;
#endif

    /* existing watch is ok */
    if ((inotqwd  = inotify_add_watch(inotfd, qpath,  IN_ATTRIB | IN_MOVED_TO | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_ONLYDIR)) == -1) {
        warning();
        return 0;
    }

    /* existing watch is ok */
    if ((inotrqwd = inotify_add_watch(inotfd, rqpath, IN_ATTRIB | IN_MOVED_TO | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_ONLYDIR)) == -1) {
        warning();
        return 0;
    }

    return 1;
}


/*
  try to register inotify watches, unregistering them if not compeltely successful
  hold an open fd during the attempt, to prevent unmount during the process
*/
static int try_reg_watches(const char *qpath, const char *rqpath) {
    int  mpfd = -1, ret = 0;
    struct stat st;

    /* unregister existing inotify watches */
    unreg_watches();

    /* try to quickly open a fd (expect read access on qpath) */
    if ((mpfd = open(qpath, O_RDONLY | O_NONBLOCK)) == -1)
        flog(LOG_NOTICE, "failed to pin %s, waiting...", qpath);

    else if (lstat(qpath, &st) == -1  ||  !S_ISDIR(st.st_mode))
        flog(LOG_NOTICE, "%s is not a directory, waiting...", qpath);
    else if (lstat(rqpath, &st) == -1  ||  !S_ISDIR(st.st_mode))
        flog(LOG_NOTICE, "%s is not a directory, waiting...", rqpath);

    /* if registering inotify watches is unsuccessful, immediately unregister */
    else if (reg_watches(qpath, rqpath))
        ret = 1;
    else
        unreg_watches();

    /* always free the fd if the mp path was successfully opened */
    if (mpfd != -1  &&  close(mpfd) == -1)
        error();

    return ret;
}


/* retry registering inotify watches, using the retry strategy parameters */
static void wait_reg_watches(const char *qpath, const char *rqpath) {
    double slp = WAIT_INIT;
    int    ok  = 0;

    while (!stop  &&  !(ok = try_reg_watches(qpath, rqpath))) {
        sleepsec(slp);

        slp = (slp * WAIT_MULT);
        if (slp > WAIT_MAX)
            slp = WAIT_MAX;
    }

    if (ok)
        flog(LOG_DEBUG, "registered watches");
}


/* set signal handlers */
static void set_signals() {
    struct sigaction sa;

    sa.sa_handler  = sig_handler;
    sa.sa_flags    = 0;
    sa.sa_restorer = NULL;

    /*
      block signals from killed processes during INT/TERM,
      also block simultaneous INT/TERM, and interference with CHLD
    */
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    sigaddset(&sa.sa_mask, SIGCHLD);

    if (sigaction(SIGINT,  &sa, NULL) == -1  ||  sigaction(SIGTERM, &sa, NULL) == -1)
        error();


    sa.sa_handler  = chld_handler;
    sa.sa_flags    = SA_RESTART;

    if (sigaction(SIGCHLD, &sa, NULL) == -1)
        error();


    /* ignore SIGPIPE, as recommended for libmicrohttpd */
    sigemptyset(&sa.sa_mask);
    sa.sa_handler  = SIG_IGN;

    if (sigaction(SIGPIPE, &sa, NULL) == -1)
        error();
}


/* lower-case hexadecimal of correct length, possibly ending with ".del" */
static int is_msgdir(char *s) {
    size_t len = strlen(s);
    int    res = 0;

    if (len == MSGID_LENGTH)
        res = vfyhex(MSGID_LENGTH, s);

    else if (len == MSGID_LENGTH+4  &&  strcmp(".del", s + MSGID_LENGTH) == 0) {
        s[MSGID_LENGTH] = '\0';
        res = vfyhex(MSGID_LENGTH, s);
        s[MSGID_LENGTH] = '.';
    }

    return res;
}


/* run loop for given queue type and msgid; msgid is a volatile string */
static void run_loop(const char *qtype, const char *msgid, const char *looppath) {
    pid_t pid;
    long  pcount;

    /* wait if too many processes have been launched */
    while (!stop  &&  (pcount = pstarted - pfinished) >= MAX_PROC) {
        flog(LOG_NOTICE, "too many processes (%ld), waiting...", pcount);
        sleepsec(WAIT_PROC);
    }

    if (!stop) {
        pid = fork();
        if (pid == -1)
            warning();
        else if (pid == 0) {
            execlp(looppath, LOOP_NAME, qtype, msgid, (char *) NULL);

            /* exits just the fork */
            error();
        }
        else {
            flog(LOG_INFO, "processing: %s %s", qtype, msgid);
            ++pstarted;
        }
    }
}


/* return whether an event is ready on the inotify fd, with timeout */
static int wait_read(double sec) {
    struct timeval tv;
    fd_set rfds;
    int    ret;

    /* support negative arguments */
    if (sec < 0)
        sec = 0;

    tv.tv_sec  = (time_t)      sec;
    tv.tv_usec = (suseconds_t) ((sec - tv.tv_sec) * 1e6);

    FD_ZERO(&rfds);
    FD_SET(inotfd, &rfds);

    ret = select(inotfd+1, &rfds, NULL, NULL, &tv);

    if (ret == -1  &&  errno != EINTR)
        error();
    else if (ret > 0  &&  !(ret == 1  &&  FD_ISSET(inotfd, &rfds))) {
        errno = EINVAL;
        error();
    }

    return ret > 0;
}


/* exec run_loop for all correct entries in (r)queue directory */
static void retry_dir(const char *qtype, const char *qpath, const char *looppath) {
    /* [offsetof(struct dirent, d_name) + fpathconf(fd, _PC_NAME_MAX) + 1] */
    struct dirent de, *deres;
    struct stat   st;
    DIR    *qdir;
    int    fd, run, rdres;

    flog(LOG_DEBUG, "retrying %s directories", qtype);

    /* open directory */
    if ((qdir = opendir(qpath)) == NULL)
        warning();
    /* get corresponding file descriptor for stat */
    else if ((fd = dirfd(qdir)) == -1)
        error();
    else {
        for (rdres = errno = 0;  !stop  &&  (rdres = readdir_r(qdir, &de, &deres)) == 0  &&  deres == &de; ) {
            run = 0;

            /* some filesystems don't support d_type, need to stat entry */
            if (de.d_type == DT_UNKNOWN  &&  is_msgdir(de.d_name)) {
                if (fstatat(fd, de.d_name, &st, AT_SYMLINK_NOFOLLOW) == -1)
                    warning();
                else if (S_ISDIR(st.st_mode))
                    run = 1;
            }
            else if (de.d_type == DT_DIR  &&  is_msgdir(de.d_name))
                run = 1;

            if (run)
                run_loop(qtype, de.d_name, looppath);
        }

        if (rdres != 0  &&  rdres != EINTR  &&  errno != EINTR)
            warning();
    }

    /* close directory if it was successfully opened */
    if (qdir  &&  closedir(qdir) == -1)
        error();
}


int main() {
    /* using FILENAME_MAX prevents EINVAL on read() */
    char   buf[sizeof(struct inotify_event) + FILENAME_MAX+1];
    char   *crtpath, *qpath, *rqpath, *looppath, *lsthost, *lstport;
    int    sz, offset, rereg, evqok = 0, retryid = 0;
    struct inotify_event *iev;
    double retrytmout, lastclock;

    /* init logging */
    syslog_init();

    /* install INT/TERM/CHLD handlers and ignore PIPE */
    set_signals();

    /* become one's own process group (EPERM if session leader) */
    /*
    if (setpgid(0, 0) == -1)
        error();
    */

    /* extract environment */
    crtpath  = alloc_env(CABLE_CERTS,  "/" CERTS_NAME);
    qpath    = alloc_env(CABLE_QUEUES, "/" QUEUE_NAME);
    rqpath   = alloc_env(CABLE_QUEUES, "/" RQUEUE_NAME);
    looppath = alloc_env(CABLE_HOME,   "/" LOOP_NAME);
    lsthost  = alloc_env(CABLE_HOST,   "");
    lstport  = alloc_env(CABLE_PORT,   "");


    /* initialize rng */
    rand_init();


    /* initialize webserver */
    if (!init_server(crtpath, qpath, rqpath, lsthost, lstport)) {
        flog(LOG_ERR, "failed to initialize webserver");
        exit(EXIT_FAILURE);
    }


    /* try to reregister watches as long as no signal caught */
    lastclock = getmontime();
    while (!stop) {
        /* support empty CABLE_NOLOOP when testing, to act as pure server */
#ifdef TESTING
        if (getenv("CABLE_NOLOOP")) {
            sleepsec(RETRY_TMOUT);
            continue;
        }
#endif

        wait_reg_watches(qpath, rqpath);

        /* read events as long as no signal caught and no unmount / move_self / etc. events read */
        rereg = 0;
        while (!stop  &&  !rereg) {
            /* wait for an event, or timeout (later blocking read() results in error) */
            retrytmout = RETRY_TMOUT + RETRY_TMOUT * (rand_shift() / 2);

            if (wait_read(retrytmout - (getmontime() - lastclock))) {
                /* read events (non-blocking), taking care to handle interrupts due to signals */
                if ((sz = read(inotfd, buf, sizeof(buf))) == -1  &&  errno != EINTR)
                    error();

                /* process all events in buffer, sz = -1 and 0 are automatically ignored */
                for (offset = 0;  offset < sz  &&  !stop  &&  !rereg;  evqok = 1) {
                    /* get handler to next event in read buffer, and update offset */
                    iev     = (struct inotify_event*) (buf + offset);
                    offset += sizeof(struct inotify_event) + iev->len;

                    /*
                      IN_IGNORED is triggered by watched directory removal / fs unmount
                      IN_MOVE_SELF is only triggered by move of actual watched directory
                      (i.e., not its parent)
                    */
                    if ((iev->mask & (IN_IGNORED | IN_UNMOUNT | IN_Q_OVERFLOW | IN_MOVE_SELF)))
                        rereg = 1;

                    /* ignore non-subdirectory events, and events with incorrect name */
                    else if (iev->len > 0  &&  (iev->mask & IN_ISDIR)  &&  is_msgdir(iev->name)) {
                        if (iev->wd != inotqwd  &&  iev->wd != inotrqwd)
                            flog(LOG_WARNING, "unknown watch descriptor");
                        else {
                            /* stop can be indicated here (while waiting for less processes) */
                            const char *qtype = (iev->wd == inotqwd) ? QUEUE_NAME : RQUEUE_NAME;
                            run_loop(qtype, iev->name, looppath);
                        }
                    }
                }
            }

            /*
              if sufficient time passed since last retries, retry again
            */
            if (!stop  &&  getmontime() - lastclock >= retrytmout) {
                /* alternate between queue dirs to prevent lock starvation on self-send */
                if ((retryid ^= 1))
                    retry_dir(QUEUE_NAME,  qpath,  looppath);
                else
                    retry_dir(RQUEUE_NAME, rqpath, looppath);

                lastclock = getmontime();

                /* inotify is apparently unreliable on fuse, so reregister when no events */
                if (!evqok)
                    rereg  = 1;
                evqok = 0;
            }
        }
    }

    unreg_watches();


    /* initialize webserver */
    if (!shutdown_server())
        flog(LOG_WARNING, "failed to shutdown webserver");


    free(lstport);
    free(lsthost);
    free(looppath);
    free(rqpath);
    free(qpath);
    free(crtpath);

    flog(LOG_INFO, "exiting");
    closelog();

    return EXIT_SUCCESS;
}
