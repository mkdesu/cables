/*
  Returned status: OK, BADREQ, BADFMT, BADCFG, <VERSION>
 */

/* Alternative: _POSIX_C_SOURCE 200809L */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>


#define VERSION             "LIBERTE CABLE 3.0"
#define REQVAR              "PATH_INFO"
#define MAX_REQ_LENGTH      512

/* caller shouldn't be able to differentiate OK/ERROR */
#define OK                  VERSION
#define BADREQ              "BADREQ"
#define BADFMT              "BADFMT"
#define BADCFG              "BADCFG"
#ifndef TESTING
#define ERROR               OK
#else
#define ERROR               "ERROR"
#endif

#define MSGID_LENGTH         40
#define TOR_HOSTNAME_LENGTH  16
#define I2P_HOSTNAME_LENGTH  52
#define USERNAME_LENGTH      32
#define MAC_LENGTH          128

#define DCREAT_MODE         (S_IRWXU | S_IRWXG | S_IRWXO)
#define FCREAT_MODE         (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)

#define CABLE_QUEUES        "CABLE_QUEUES"
#define QUEUE_SUBDIR        "queue"
#define RQUEUE_SUBDIR       "rqueue"


static void retstatus(const char *status) {
#ifdef TESTING
    if (!strcmp(status, ERROR) && errno)
        perror("Error");
#endif
    puts(status);
    exit(0);
}


/* lowercase hexadecimal */
static int vfyhex(int sz, const char *s) {
    if (strlen(s) != sz)
        return 0;

    for (; *s; ++s)
        if (!(isxdigit(*s) && !isupper(*s)))
            return 0;

    return 1;
}


/* lowercase Base-32 encoding (a-z, 2-7) */
static int vfybase32(int sz, const char *s) {
    if (strlen(s) != sz)
        return 0;

    for (; *s; ++s)
        if (!(islower(*s) || (*s >= '2' && *s <= '7')))
            return 0;

    return 1;
}


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


static void write_line(int dir, const char *path, const char *s) {
    int  fd;
    FILE *file;

    if ((fd = openat(dir, path, O_CREAT | O_WRONLY | O_TRUNC, FCREAT_MODE)) == -1)
        retstatus(ERROR);

    if (!(file = fdopen(fd, "w"))) {
        close(fd);
        retstatus(ERROR);
    }

    if (s) {
        if(fputs(s, file) < 0 || fputc('\n', file) != '\n') {
            fclose(file);
            retstatus(ERROR);
        }
    }

    if (fclose(file))
        retstatus(ERROR);
}


static void create_file(int dir, const char *path) {
    if (faccessat(dir, path, F_OK, 0))
        write_line(dir, path, NULL);
}


static void read_line(int dir, const char *path, char *s, int sz) {
    int  fd;
    FILE *file;

    if ((fd = openat(dir, path, O_RDONLY)) == -1)
        retstatus(ERROR);

    if (!(file = fdopen(fd, "r")))
        retstatus(ERROR);

    if (s) {
        if(!fgets(s, sz, file) || fgetc(file) != EOF)
            retstatus(ERROR);

        sz = strlen(s);
        if (s[sz-1] == '\n')
            s[sz-1] = '\0';
    }

    if (fclose(file))
        retstatus(ERROR);
}


static void check_file(int dir, const char *path) {
    if (faccessat(dir, path, F_OK, 0))
        retstatus(ERROR);
}


static void handle_msg(const char *msgid, const char *hostname,
                       const char *username, int cqdir) {
    char msgidnew[MSGID_LENGTH+4+1];
    int  msgdir;

    /* checkno /cables/rqueue/<msgid> (ok and skip if exists) */
    if (!faccessat(cqdir, msgid, F_OK, 0))
        return;

    /* temp base: .../cables/rqueue/<msgid>.new */
    strncpy(msgidnew, msgid, MSGID_LENGTH);
    strcpy(msgidnew + MSGID_LENGTH, ".new");

    /* create directory (ok if exists) */
    if (mkdirat(cqdir, msgidnew, DCREAT_MODE) && errno != EEXIST)
        retstatus(ERROR);

    if ((msgdir = openat(cqdir, msgidnew, O_RDONLY)) == -1)
        retstatus(ERROR);

    /* write hostname */
    write_line(msgdir, "hostname", hostname);

    /* write username */
    write_line(msgdir, "username", username);

    /* create peer.req */
    create_file(msgdir, "peer.req");

    /* rename .../cables/rqueue/<msgid>.new -> <msgid> */
    if (close(msgdir))
        retstatus(ERROR);

    if (renameat(cqdir, msgidnew, cqdir, msgid))
        retstatus(ERROR);
}


static void handle_snd(const char *msgid, const char *mac, int cqdir) {
    int  msgdir;

    /* base: .../cables/rqueue/<msgid> */
    if ((msgdir = openat(cqdir, msgid, O_RDONLY)) == -1)
        retstatus(ERROR);

    /* check peer.ok */
    check_file(msgdir, "peer.ok");

    /* write send.mac (skip if exists) */
    if (faccessat(msgdir, "send.mac", F_OK, 0))
        write_line(msgdir, "send.mac", mac);

    /* create recv.req (atomic, ok if exists) */
    if (! linkat(msgdir, "peer.ok", msgdir, "recv.req", 0)) {
        /* touch /cables/rqueue/<msgid>/ (if recv.req didn't exist) */
        /* euid owns msgdir, so O_RDWR is not needed */
        if (futimens(msgdir, NULL))
            retstatus(ERROR);
    }
    else if (errno != EEXIST)
        retstatus(ERROR);

    if (close(msgdir))
        retstatus(ERROR);
}


static void handle_rcp(const char *msgid, const char *mac, int cqdir) {
    char exmac[MAC_LENGTH+2];
    int  msgdir;

    /* base: .../cables/queue/<msgid> */
    if ((msgdir = openat(cqdir, msgid, O_RDONLY)) == -1)
        retstatus(ERROR);

    /* check send.ok */
    check_file(msgdir, "send.ok");

    /* read recv.mac */
    read_line(msgdir, "recv.mac", exmac, sizeof(exmac));

    /* compare <recvmac> <-> recv.mac */
    if (strcmp(mac, exmac))
        retstatus(ERROR);

    /* create ack.req (atomic, ok if exists) */
    if (! linkat(msgdir, "send.ok", msgdir, "ack.req", 0)) {
        /* touch /cables/queue/<msgid>/ (if ack.req didn't exist) */
        /* euid owns msgdir, so O_RDWR is not needed */
        if (futimens(msgdir, NULL))
            retstatus(ERROR);
    }
    else if (errno != EEXIST)
        retstatus(ERROR);

    if (close(msgdir))
        retstatus(ERROR);
}


static void handle_ack(const char *msgid, const char *mac, int cqdir) {
    char msgiddel[MSGID_LENGTH+4+1], exmac[MAC_LENGTH+2];
    int  msgdir;

    /* base: .../cables/rqueue/<msgid> */
    if ((msgdir = openat(cqdir, msgid, O_RDONLY)) == -1)
        retstatus(ERROR);

    /* check recv.ok */
    check_file(msgdir, "recv.ok");

    /* read ack.mac */
    read_line(msgdir, "ack.mac", exmac, sizeof(exmac));

    /* compare <ackmac> <-> ack.mac */
    if (strcmp(mac, exmac))
        retstatus(ERROR);

    /* rename .../cables/rqueue/<msgid> -> <msgid>.del */
    strncpy(msgiddel, msgid, MSGID_LENGTH);
    strcpy(msgiddel + MSGID_LENGTH, ".del");

    if (close(msgdir))
        retstatus(ERROR);

    if (renameat(cqdir, msgid, cqdir, msgiddel))
        retstatus(ERROR);
}


int open_cqdir(const char *subdir) {
    const char *cqenv;
    int        cqdir, subcqdir;

    /* Get queues prefix from environment */
    if (!(cqenv = getenv(CABLE_QUEUES)))
        retstatus(BADCFG);

    if ((cqdir = open(cqenv, O_RDONLY)) == -1)
        retstatus(ERROR);
    cqenv = NULL;

    if ((subcqdir = openat(cqdir, subdir, O_RDONLY)) == -1)
        retstatus(ERROR);

    if (close(cqdir))
        retstatus(ERROR);

    return subcqdir;
}


int main() {
    char       buf[MAX_REQ_LENGTH+1];
    const char *pathinfo, *delim = "/";
    char       *cmd, *msgid, *arg1, *arg2;
    int        cqdir = -1;

    umask(0077);
    setlocale(LC_ALL, "C");

    
    /* HTTP headers */
    printf("Content-Type: text/plain\n"
           "Cache-Control: no-cache\n\n");


    /* Check request availability and length */
    pathinfo = getenv(REQVAR);
    if (!pathinfo || strlen(pathinfo) >= sizeof(buf))
        retstatus(BADREQ);

    /* Copy request to writeable buffer */
    strcpy(buf, pathinfo);
    pathinfo = NULL;


    /* Tokenize the request */
    cmd   = strtok(buf,  delim);
    msgid = strtok(NULL, delim);
    arg1  = strtok(NULL, delim);
    arg2  = strtok(NULL, delim);

    if (strtok(NULL, delim) || !cmd)
        retstatus(BADFMT);


    /* Handle commands

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
        if (msgid)
            retstatus(BADFMT);

        retstatus(VERSION);
    }
    else if (!strcmp("msg", cmd)) {
        if (!arg2)
            retstatus(BADFMT);

        if (   !vfyhex(MSGID_LENGTH, msgid)
            || !vfyhost(arg1)
            || !vfybase32(USERNAME_LENGTH, arg2))
            retstatus(BADFMT);

        cqdir = open_cqdir(RQUEUE_SUBDIR);
        handle_msg(msgid, arg1, arg2, cqdir);
    }
    else if (!strcmp("snd", cmd)) {
        if (!arg1 || arg2)
            retstatus(BADFMT);

        if (   !vfyhex(MSGID_LENGTH, msgid)
            || !vfyhex(MAC_LENGTH, arg1))
            retstatus(BADFMT);

        cqdir = open_cqdir(RQUEUE_SUBDIR);
        handle_snd(msgid, arg1, cqdir);
    }
    else if (!strcmp("rcp", cmd)) {
        if (!arg1 || arg2)
            retstatus(BADFMT);

        if (   !vfyhex(MSGID_LENGTH, msgid)
            || !vfyhex(MAC_LENGTH, arg1))
            retstatus(BADFMT);

        cqdir = open_cqdir(QUEUE_SUBDIR);
        handle_rcp(msgid, arg1, cqdir);
    }
    else if (!strcmp("ack", cmd)) {
        if (!arg1 || arg2)
            retstatus(BADFMT);

        if (   !vfyhex(MSGID_LENGTH, msgid)
            || !vfyhex(MAC_LENGTH, arg1))
            retstatus(BADFMT);

        cqdir = open_cqdir(RQUEUE_SUBDIR);
        handle_ack(msgid, arg1, cqdir);
    }
    else
        retstatus(BADFMT);


    if (close(cqdir))
        retstatus(ERROR);

    retstatus(OK);
    return 0;
}
