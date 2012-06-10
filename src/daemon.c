/*
  The following environment variables are used (from /etc/cable/profile):
  CABLE_HOME, CABLE_QUEUES, CABLE_CERTS, CABLE_HOST, CABLE_PORT

  Testing environment:
  CABLE_NOLOOP, CABLE_NOWATCH
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/select.h>

#include "daemon.h"
#include "server.h"
#include "process.h"
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
#define RETRY_TMOUT 5
#define MAX_PROC    5
#define WAIT_PROC   5
#endif

/* inotify mask for for (r)queue directories */
#define INOTIFY_MASK (IN_ATTRIB | IN_MOVED_TO | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_ONLYDIR)


/* inotify file descriptor and (r)queue directories watch descriptors */
static int inotfd = -1, inotqwd = -1, inotrqwd = -1;


/* unregister inotify watches and dispose of allocated file descriptor */
static void unreg_watches() {
    if (inotfd != -1) {
        /* ignore errors due to automatically removed watches (IN_IGNORED) */
        if (inotqwd != -1) {
            if (inotify_rm_watch(inotfd, inotqwd)  &&  errno != EINVAL)
                warning("failed to remove inotify watch");
            inotqwd = -1;
        }

        /* ignore errors due to automatically removed watches (IN_IGNORED) */
        if (inotrqwd != -1) {
            if (inotify_rm_watch(inotfd, inotrqwd)  &&  errno != EINVAL)
                warning("failed to remove inotify watch");
            inotrqwd = -1;
        }

        /*
          closing/reopening an inotify fd is an expensive operation, but must be done
          because otherwise fd provides infinite stream of IN_IGNORED events
        */
        if (close(inotfd))
            warning("could not close inotify fd");
        inotfd = -1;
    }
}


/* register (r)queue-specific inotify watches, returning 1 if successful */
static int reg_watches(const char *qpath, const char *rqpath) {
    /* don't block on read(), since select() is used for polling */
    if (inotfd == -1  &&  (inotfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) == -1) {
        warning("failed to initialize inotify instance");
        return 0;
    }

#ifdef TESTING
    if (getenv("CABLE_NOWATCH"))
        return 1;
#endif

    /* existing watch is ok */
    if ((inotqwd  = inotify_add_watch(inotfd, qpath,  INOTIFY_MASK)) == -1) {
        warning("could not add inotify watch");
        return 0;
    }

    /* existing watch is ok */
    if ((inotrqwd = inotify_add_watch(inotfd, rqpath, INOTIFY_MASK)) == -1) {
        warning("could not add inotify watch");
        return 0;
    }

    return 1;
}


/*
  try to register inotify watches, unregistering them if not compeltely successful
  hold an open fd during the attempt, to prevent unmount during the process
*/
static int try_reg_watches(const char *qpath, const char *rqpath) {
    int    mpfd, ret = 0;
    struct stat st;

    /* unregister existing inotify watches */
    unreg_watches();

    /* try to quickly open a fd (expect read access on qpath) */
    if ((mpfd = open(qpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) != -1) {
        if      (lstat(qpath,  &st) == -1  ||  !S_ISDIR(st.st_mode))
            flog(LOG_NOTICE, "%s is not a directory, waiting...", qpath);
        else if (lstat(rqpath, &st) == -1  ||  !S_ISDIR(st.st_mode))
            flog(LOG_NOTICE, "%s is not a directory, waiting...", rqpath);

        /* if registering inotify watches is unsuccessful, immediately unregister */
        else if (reg_watches(qpath, rqpath))
            ret = 1;
        else
            unreg_watches();


        /* free the pin fd */
        if (close(mpfd))
            warning("could not close pin directory");
    }
    else
        flog(LOG_NOTICE, "failed to pin %s, waiting...", qpath);

    return ret;
}


/* retry registering inotify watches, using the retry strategy parameters */
static void wait_reg_watches(const char *qpath, const char *rqpath) {
    double slp = WAIT_INIT;

    while (!stop_requested()) {
        if (try_reg_watches(qpath, rqpath)) {
            flog(LOG_DEBUG, "registered watches");
            break;
        }
        else {
            sleepsec(slp);

            slp = (slp * WAIT_MULT);
            if (slp > WAIT_MAX)
                slp = WAIT_MAX;
        }
    }
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


/* run loop for given queue type and msgid[.del]; msgid is a volatile string */
static void run_loop(const char *qtype, const char *msgid, const char *looppath) {
    const char *args[] = { looppath, qtype, msgid, NULL };

    if (run_process(MAX_PROC, WAIT_PROC, args))
        flog(LOG_INFO, "processing: %s %s", qtype, msgid);
    else
        flog(LOG_WARNING, "failed to launch: %s %s", qtype, msgid);
}


/* return whether an event is ready on the inotify fd, with timeout */
static int wait_read(int fd, double sec) {
    struct timeval tv;
    fd_set rfds;
    int    ret;

    /* support negative arguments */
    if (sec < 0)
        sec = 0;

    tv.tv_sec  = (time_t)      sec;
    tv.tv_usec = (suseconds_t) ((sec - tv.tv_sec) * 1e6);

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    ret = select(fd+1, &rfds, NULL, NULL, &tv);

    if (ret == -1  &&  errno != EINTR)
        warning("waiting on inotify queue failed");
    else if (ret > 0  &&  !(ret == 1  &&  FD_ISSET(fd, &rfds))) {
        flog(LOG_WARNING, "unexpected fd while waiting on inotify queue");
        ret = 0;
    }

    return ret > 0;
}


/*
  exec run_loop for all correct entries in (r)queue directory
  NOT thread-safe, since readdir_r is unreliable with filenames > NAME_MAX
  (e.g., NTFS + 255 unicode chars get truncated to 256 chars w/o terminating NUL)
*/
static void retry_dir(const char *qtype, const char *qpath, const char *looppath) {
    /* [offsetof(struct dirent, d_name) + fpathconf(fd, _PC_NAME_MAX) + 1] */
    struct dirent *de;
    struct stat   st;
    DIR    *qdir;
    int    fd, run;

    flog(LOG_DEBUG, "retrying %s directories", qtype);

    /* open directory (O_CLOEXEC is implied) */
    if ((qdir = opendir(qpath))) {
        /* get corresponding file descriptor for stat */
        if ((fd = dirfd(qdir)) != -1) {
            for (errno = 0;  !stop_requested()  &&  ((de = readdir(qdir))); ) {
                run = 0;

                /* some filesystems don't support d_type, need to stat entry */
                if (de->d_type == DT_UNKNOWN  &&  is_msgdir(de->d_name)) {
                    if (!fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW))
                        run = S_ISDIR(st.st_mode);
                    else
                        warning("fstat failed");
                }
                else
                    run = (de->d_type == DT_DIR  &&  is_msgdir(de->d_name));

                if (run)
                    run_loop(qtype, de->d_name, looppath);
            }

            if (errno  &&  errno != EINTR)
                warning("reading directory failed");
        }
        else
            warning("dirfd failed");

        /* close directory */
        if (closedir(qdir))
            warning("could not close directory");
    }
    else
        warning("could not open directory");
}


int main() {
    /* using NAME_MAX prevents EINVAL on read() (twice for UTF-16 on NTFS) */
    char   buf[sizeof(struct inotify_event) + NAME_MAX*2 + 1];
    char   *crtpath, *qpath, *rqpath, *looppath, *lsthost, *lstport;
    int    sz, offset, rereg, evqok, retryid;
    struct inotify_event *iev;
    double retrytmout, lastclock;


    /* init logging */
    syslog_init();


    /* extract environment */
    crtpath  = alloc_env(CABLE_CERTS,  "/" CERTS_NAME);
    qpath    = alloc_env(CABLE_QUEUES, "/" QUEUE_NAME);
    rqpath   = alloc_env(CABLE_QUEUES, "/" RQUEUE_NAME);
    looppath = alloc_env(CABLE_HOME,   "/" LOOP_NAME);
    lsthost  = alloc_env(CABLE_HOST,   "");
    lstport  = alloc_env(CABLE_PORT,   "");


    /* initialize rng */
    if (!rand_init())
        warning("failed to initialize RNG");


    /* initialize process accounting */
    if (!init_process_acc())
        warning("failed to initialize process accounting");


    /* initialize webserver */
    if (!init_server(crtpath, qpath, rqpath, lsthost, lstport)) {
        flog(LOG_ERR, "failed to initialize webserver");
        return EXIT_FAILURE;
    }


    /* try to reregister watches as long as no signal caught */
    for (lastclock = getmontime(), retryid = 0;  !stop_requested(); ) {
        /* support empty CABLE_NOLOOP when testing, to act as pure server */
#ifdef TESTING
        if (getenv("CABLE_NOLOOP")) {
            sleepsec(RETRY_TMOUT);
            continue;
        }
#endif

        wait_reg_watches(qpath, rqpath);

        /* read events as long as no signal caught and no unmount / move_self / etc. events read */
        for (rereg = evqok = 0;  !stop_requested()  &&  !rereg; ) {
            /* wait for an event, or timeout (later blocking read() results in error) */
            retrytmout = RETRY_TMOUT + RETRY_TMOUT * (rand_shift() / 2);

            if (wait_read(inotfd, retrytmout - (getmontime() - lastclock))) {
                /* read events (non-blocking), taking care to handle interrupts due to signals */
                if ((sz = read(inotfd, buf, sizeof(buf))) == -1  &&  errno != EINTR) {
                    /* happens buffer is too small (e.g., NTFS + 255 unicode chars) */
                    warning("error while reading from inotify queue");
                    rereg = 1;
                }

                /* process all events in buffer, sz = -1 and 0 are automatically ignored */
                for (offset = 0;  offset < sz  &&  !stop_requested()  &&  !rereg;  evqok = 1) {
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
                        assert(iev->wd == inotqwd  ||  iev->wd == inotrqwd);
                        if (iev->wd == inotqwd  ||  iev->wd == inotrqwd) {
                            /* stop can be indicated here (while waiting for less processes) */
                            const char *qtype = (iev->wd == inotqwd) ? QUEUE_NAME : RQUEUE_NAME;
                            run_loop(qtype, iev->name, looppath);
                        }
                        else
                            flog(LOG_WARNING, "unknown watch descriptor");
                    }
                }
            }

            /*
              if sufficient time passed since last retries, retry again
            */
            if (!stop_requested()  &&  getmontime() - lastclock >= retrytmout) {
                /* alternate between queue dirs to prevent lock starvation on self-send */
                if ((retryid ^= 1))
                    retry_dir(QUEUE_NAME,  qpath,  looppath);
                else
                    retry_dir(RQUEUE_NAME, rqpath, looppath);

                lastclock = getmontime();

                /* inotify is apparently unreliable on fuse, so reregister when no events */
                if (!evqok)
                    rereg = 1;
                evqok = 0;
            }
        }
    }


    unreg_watches();

    if (!shutdown_server())
        flog(LOG_WARNING, "failed to shutdown webserver");

    dealloc_env(lstport);
    dealloc_env(lsthost);
    dealloc_env(looppath);
    dealloc_env(rqpath);
    dealloc_env(qpath);
    dealloc_env(crtpath);

    flog(LOG_INFO, "exiting");
    closelog();

    return EXIT_SUCCESS;
}
