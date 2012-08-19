#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "service.h"
#include "daemon.h"
#include "util.h"


#define MAX_REQUEST_LENGTH  255

#define TOR_HOSTNAME_LENGTH  16
#define I2P_HOSTNAME_LENGTH  52
#define MAC_LENGTH          128

#define DCREAT_MODE         (S_IRWXU | S_IRWXG | S_IRWXO)
#define FCREAT_MODE         (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)


/* lowercase hostnames: recognizes .onion and .b32.i2p addresses */
static int vfyhost(char *s) {
    int  result = 0;
    char *dot   = strchr(s, '.');

    if (dot) {
        *dot = '\0';

        /* Tor .onion hostnames */
        if (!strcmp("onion", dot+1))
            result = vfybase32(TOR_HOSTNAME_LENGTH, s);

        /* I2P .b32.i2p hostnames */
        else if (!strcmp("b32.i2p", dot+1))
            result = vfybase32(I2P_HOSTNAME_LENGTH, s);

        *dot = '.';
    }

    return result;
}


static int write_line(int dir, const char *path, const char *s) {
    int  res = 0, fd;
    FILE *file;

    if ((fd = openat(dir, path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, FCREAT_MODE)) != -1) {
        if ((file = fdopen(fd, "w"))) {
            if (s) {
                if (fputs(s, file) >= 0  &&  fputc('\n', file) == '\n')
                    res = 1;
            }
            else
                res = 1;

            if (fclose(file))
                res = 0;
        }
        else
            close(fd);
    }

    return res;
}


static int read_line(int dir, const char *path, char *s, int sz) {
    int  res = 0, fd;
    FILE *file;

    if ((fd = openat(dir, path, O_RDONLY | O_CLOEXEC)) != -1) {
        if ((file = fdopen(fd, "r"))) {
            if(fgets(s, sz, file)  &&  fgetc(file) == EOF) {
                sz = strlen(s);
                if (s[sz-1] == '\n')
                    s[sz-1] = '\0';

                res = 1;
            }

            if (fclose(file))
                res = 0;
        }
        else
            close(fd);
    }

    return res;
}


static int check_file(int dir, const char *path) {
    return !faccessat(dir, path, F_OK, 0);
}


static int create_file(int dir, const char *path) {
    return check_file(dir, path)  ||  write_line(dir, path, NULL);
}


/* attemts non-blocking lock (note: errno == EWOULDBLOCK) */
static int try_lock(int fd) {
    return !flock(fd, LOCK_EX | LOCK_NB);
}


static int handle_msg(const char *msgid, const char *hostname,
                       const char *username, int cqdir) {
    int  res = 0, msgdir;
    char msgidnew[MSGID_LENGTH+4+1];

    /* checkno /cables/rqueue/<msgid> (ok and skip if exists) */
    if (check_file(cqdir, msgid))
        res = 1;

    else if (errno == ENOENT) {
        /* temp base: .../cables/rqueue/<msgid>.new */
        strncpy(msgidnew, msgid, MSGID_LENGTH);
        strcpy(msgidnew + MSGID_LENGTH, ".new");

        /* create directory (ok if exists) */
        if (!mkdirat(cqdir, msgidnew, DCREAT_MODE)  ||  errno == EEXIST) {
            if ((msgdir = openat(cqdir, msgidnew, O_RDONLY | O_CLOEXEC)) != -1) {
                res =
                    /* lock temp base */
                       try_lock(msgdir)
                    /* write hostname */
                    && write_line(msgdir, "hostname", hostname)
                    /* write username */
                    && write_line(msgdir, "username", username)
                    /* create peer.req */
                    && create_file(msgdir, "peer.req")
                    /* rename .../cables/rqueue/<msgid>.new -> <msgid> */
                    && !renameat(cqdir, msgidnew, cqdir, msgid);

                /* close base (and unlock if locked) */
                if (close(msgdir))
                    res = 0;
            }
        }
    }

    return res;
}


static int handle_snd(const char *msgid, const char *mac, int cqdir) {
    int res = 0, msgdir;

    /* base: .../cables/rqueue/<msgid> */
    if ((msgdir = openat(cqdir, msgid, O_RDONLY | O_CLOEXEC)) != -1) {
        if (/* lock base */
               try_lock(msgdir)
            /* check peer.ok */
            && check_file(msgdir, "peer.ok")
            /* write send.mac (skip if exists) */
            && (check_file(msgdir, "send.mac")
                || (errno == ENOENT  &&  write_line(msgdir, "send.mac", mac)))) {

            /* create recv.req (atomic, ok if exists) */
            if (linkat(msgdir, "peer.ok", msgdir, "recv.req", 0))
                res = (errno == EEXIST);
            else
                res =
                    /* unlock base (touch triggers loop's lock) */
                       !flock(msgdir, LOCK_UN)
                    /* touch /cables/rqueue/<msgid>/ (if recv.req didn't exist) */
                    /* euid owns msgdir, so O_RDWR is not needed (NOTE: unless overlayfs) */
                    && !futimens(msgdir, NULL);
        }

        /* close base (and unlock if locked) */
        if (close(msgdir))
            res = 0;
    }

    return res;
}


static int handle_rcp(const char *msgid, const char *mac, int cqdir) {
    int  res = 0, msgdir;
    char exmac[MAC_LENGTH+2];

    /* base: .../cables/queue/<msgid> */
    if ((msgdir = openat(cqdir, msgid, O_RDONLY | O_CLOEXEC)) != -1) {
        if (/* lock base */
               try_lock(msgdir)
            /* check send.ok */
            && check_file(msgdir, "send.ok")
            /* read recv.mac */
            && read_line(msgdir, "recv.mac", exmac, sizeof(exmac))
            /* compare <recvmac> <-> recv.mac */
            && !strcmp(mac, exmac)) {

            /* create ack.req (atomic, ok if exists) */
            if (linkat(msgdir, "send.ok", msgdir, "ack.req", 0))
                res = (errno == EEXIST);
            else
                res =
                    /* unlock base (touch triggers loop's lock) */
                       !flock(msgdir, LOCK_UN)
                    /* touch /cables/queue/<msgid>/ (if ack.req didn't exist) */
                    /* euid owns msgdir, so O_RDWR is not needed (NOTE: unless overlayfs) */
                    && !futimens(msgdir, NULL);
        }

        /* close base (and unlock if locked) */
        if (close(msgdir))
            res = 0;
    }

    return res;
}


static int handle_ack(const char *msgid, const char *mac, int cqdir) {
    int  res = 0, msgdir;
    char msgiddel[MSGID_LENGTH+4+1], exmac[MAC_LENGTH+2];

    /* base: .../cables/rqueue/<msgid> */
    if ((msgdir = openat(cqdir, msgid, O_RDONLY | O_CLOEXEC)) != -1) {
        strncpy(msgiddel, msgid, MSGID_LENGTH);
        strcpy(msgiddel + MSGID_LENGTH, ".del");

        res =
            /* lock base */
               try_lock(msgdir)
            /* check recv.ok */
            && check_file(msgdir, "recv.ok")
            /* read ack.mac */
            && read_line(msgdir, "ack.mac", exmac, sizeof(exmac))
            /* compare <ackmac> <-> ack.mac */
            && !strcmp(mac, exmac)
            /* rename .../cables/rqueue/<msgid> -> <msgid>.del */
            && !renameat(cqdir, msgid, cqdir, msgiddel);

        /* close base (and unlock if locked) */
        if (close(msgdir))
            res = 0;
    }

    return res;
}


/*
  returns memory-persistent response (including trailing newline)
  thread-safe
  does not leak memory / file descriptors
 */
enum SVC_Status handle_request(const char *request, const char *queues, const char *rqueues) {
    enum   SVC_Status status = SVC_BADFMT;
    char   buf[MAX_REQUEST_LENGTH+1], *saveptr, *cmd, *msgid, *arg1, *arg2;
    int    cqdir;
    size_t reqlen;


    /* Copy request to modifiable buffer, check for length and bad delimiters */
    reqlen = strlen(request);
    if (reqlen < sizeof(buf)  &&  reqlen > 0
        &&  !strstr(request, "//")
        &&  request[0] != '/'  &&  request[reqlen-1] != '/') {
        strcpy(buf, request);

        /* Tokenize the request */
        cmd   = strtok_r(buf,  "/", &saveptr);
        msgid = strtok_r(NULL, "/", &saveptr);
        arg1  = strtok_r(NULL, "/", &saveptr);
        arg2  = strtok_r(NULL, "/", &saveptr);

        if (cmd  &&  !strtok_r(NULL, "/", &saveptr)) {
            /*
               ver
               msg/<msgid>/<hostname>/<username>
               snd/<msgid>/<mac>
               rcp/<msgid>/<mac>
               ack/<msgid>/<mac>

               msgid:    MSGID_LENGTH        lowercase xdigits
               mac:      MAC_LENGTH          lowercase xdigits
               hostname: TOR_HOSTNAME_LENGTH lowercase base-32 chars + ".onion"
                         I2P_HOSTNAME_LENGTH lowercase base-32 chars + ".b32.i2p"
               username: USERNAME_LENGTH     lowercase base-32 chars
            */
            if (!strcmp("ver", cmd)) {
                if (!msgid)
                    status = SVC_OK;
            }
            else if (!strcmp("msg", cmd)) {
                if (arg2
                    && vfyhex(MSGID_LENGTH, msgid)
                    && vfyhost(arg1)
                    && vfybase32(USERNAME_LENGTH, arg2)) {

                    status = SVC_ERR;

                    if ((cqdir = open(rqueues, O_RDONLY | O_CLOEXEC)) != -1) {
                        if (handle_msg(msgid, arg1, arg2, cqdir))
                            status = SVC_OK;

                        if (close(cqdir))
                            status = SVC_ERR;
                    }
                }
            }
            else if (!strcmp("snd", cmd)) {
                if (arg1 && !arg2
                    && vfyhex(MSGID_LENGTH, msgid)
                    && vfyhex(MAC_LENGTH, arg1)) {

                    status = SVC_ERR;

                    if ((cqdir = open(rqueues, O_RDONLY | O_CLOEXEC)) != -1) {
                        if (handle_snd(msgid, arg1, cqdir))
                            status = SVC_OK;

                        if (close(cqdir))
                            status = SVC_ERR;
                    }
                }
            }
            else if (!strcmp("rcp", cmd)) {
                if (arg1 && !arg2
                    && vfyhex(MSGID_LENGTH, msgid)
                    && vfyhex(MAC_LENGTH, arg1)) {

                    status = SVC_ERR;

                    if ((cqdir = open(queues, O_RDONLY | O_CLOEXEC)) != -1) {
                        if (handle_rcp(msgid, arg1, cqdir))
                            status = SVC_OK;

                        if (close(cqdir))
                            status = SVC_ERR;
                    }
                }
            }
            else if (!strcmp("ack", cmd)) {
                if (arg1 && !arg2
                    && vfyhex(MSGID_LENGTH, msgid)
                    && vfyhex(MAC_LENGTH, arg1)) {

                    status = SVC_ERR;

                    if ((cqdir = open(rqueues, O_RDONLY | O_CLOEXEC)) != -1) {
                        if (handle_ack(msgid, arg1, cqdir))
                            status = SVC_OK;

                        if (close(cqdir))
                            status = SVC_ERR;
                    }
                }
            }
        }
    }

    return status;
}
