/*
  + /<username>               common URL prefix: CABLE_CERTS/certs/username
  +   /certs/{ca,verify}.pem  serve  CABLE_CERTS/certs/{ca,verify}.pem
  +   /queue/<msgid>          serve  CABLE_QUEUES/queue/<msgid>/message.enc
  +   /queue/<msgid>.key      serve  CABLE_QUEUES/queue/<msgid>/speer.sig
  +   /rqueue/<msgid>.key     serve  CABLE_QUEUES/rqueue/<msgid>/rpeer.sig
  +   /request/...            invoke service(...), and return answer
 */

#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#define MHD_PLATFORM_H
#include <microhttpd.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <syslog.h>
#include <netdb.h>
#include <sys/stat.h>

#include "daemon.h"


/* retry and limit strategies */
#ifndef TESTING
#define WAIT_CONN  100
#define MAX_THREAD 4
#else
#define WAIT_CONN  10
#define MAX_THREAD 2
#endif

/* path and url suffixes */
#define USERNAME_SFX "username"
#define CA_SFX       "ca.pem"
#define VERIFY_SFX   "verify.pem"
#define MESSAGE_SFX  "message.enc"
#define SPEER_SFX    "speer.sig"
#define RPEER_SFX    "rpeer.sig"
#define KEY_SFX      ".key"

/* url prefixes */
#define CERTS_PFX    "/certs/"
#define QUEUE_PFX    "/queue/"
#define RQUEUE_PFX   "/rqueue/"
#define REQUEST_PFX  "/request/"

/* service responses */
#define SVC_RESP_OK  VERSION "\n"
#define SVC_RESP_ERR VERSION ": ERROR\n"


/* read-only values after server startup */
static struct MHD_Daemon   *mhd_daemon;
static struct MHD_Response *mhd_empty, *mhd_svc_ok, *mhd_svc_err;
static const  char         *crt_path, *cq_path, *crq_path;
static        char         username[USERNAME_LENGTH+2];


static int advance_pfx(const char **url, const char *pfx) {
    size_t len = strlen(pfx);
    int    ret = 0;

    if (!strncmp(*url, pfx, len)) {
        *url += len;
        ret   = 1;
    }

    return ret;
}


/*
  dir + [ / subdir ] + sfx
*/
static int queue_fd(struct MHD_Connection *connection,
                    const char *dir, const char *subdir, const char *sfx) {
    char   path[strlen(dir) + (subdir ? strlen(subdir) + 1 : 0) + strlen(sfx) + 1];
    struct MHD_Response *resp;
    struct stat         st;
    int    ret, fd;

    /* construct full path */
    strcpy(path, dir);
    if (subdir) {
        strcat(path, "/");
        strcat(path, subdir);
    }
    strcat(path, sfx);

    if ((fd = open(path, O_RDONLY)) != -1) {
        if (!fstat(fd, &st)  &&  ((resp = MHD_create_response_from_fd(st.st_size, fd)))) {
            ret = MHD_queue_response(connection, MHD_HTTP_OK, resp);
            MHD_destroy_response(resp);
        }
        else {
            close(fd);
            ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, mhd_empty);
        }
    }
    else
        ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, mhd_empty);

    return ret;
}


static int handle_connection(void *cls, struct MHD_Connection *connection,
                             const char *url, const char *method, const char *version,
                             const char *upload_data, size_t *upload_data_size,
                             void **con_cls) {
    enum   SVC_Status svc_status;
    char   msgid[MSGID_LENGTH + sizeof(KEY_SFX)];
    int    ret;

    /* support GET only, close connection otherwise */
    if ((strcmp(method, MHD_HTTP_METHOD_GET)  &&  strcmp(method, MHD_HTTP_METHOD_HEAD))
        ||  *upload_data_size)
        ret = MHD_queue_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, mhd_empty);

    /* do not queue response on first call (enabled pipelining) */
    else if (!*con_cls) {
        *con_cls = "";
        ret = MHD_YES;
    }

    /* check /<username> prefix, close connection if no match */
    else if (!(*(url++) == '/'  &&  advance_pfx(&url, username)))
        ret  = MHD_queue_response(connection, MHD_HTTP_FORBIDDEN, mhd_empty);

    /* handle username-authenticated queries */
    else {
        /* serve /certs/ files */
        if (     !strcmp(url, CERTS_PFX CA_SFX))
            ret = queue_fd(connection, crt_path, NULL, "/" CA_SFX);
        else if (!strcmp(url, CERTS_PFX VERIFY_SFX))
            ret = queue_fd(connection, crt_path, NULL, "/" VERIFY_SFX);

        /* serve /queue/<msgid>{,.key} and /rqueue/<msgid>.key */
        else if (advance_pfx(&url, QUEUE_PFX)) {
            strncpy(msgid, url, sizeof(msgid));

            if (!msgid[MSGID_LENGTH]  &&  vfyhex(MSGID_LENGTH, msgid))
                ret = queue_fd(connection, cq_path, msgid, "/" MESSAGE_SFX);
            else if (!strncmp(msgid + MSGID_LENGTH, KEY_SFX, sizeof(KEY_SFX))
                     &&  (msgid[MSGID_LENGTH] = '\0', vfyhex(MSGID_LENGTH, msgid)))
                ret = queue_fd(connection, cq_path, msgid, "/" SPEER_SFX);
            else
                ret = MHD_queue_response(connection, MHD_HTTP_FORBIDDEN, mhd_empty);
        }
        else if (advance_pfx(&url, RQUEUE_PFX)) {
            strncpy(msgid, url, sizeof(msgid));

            if (!strncmp(msgid + MSGID_LENGTH, KEY_SFX, sizeof(KEY_SFX))
                &&  (msgid[MSGID_LENGTH] = '\0', vfyhex(MSGID_LENGTH, msgid)))
                ret = queue_fd(connection, crq_path, msgid, "/" RPEER_SFX);
            else
                ret = MHD_queue_response(connection, MHD_HTTP_FORBIDDEN, mhd_empty);
        }

        /* handle /request/ interface */
        else if (advance_pfx(&url, REQUEST_PFX)) {
            svc_status = handle_request(url, cq_path, crq_path);

            switch (svc_status) {
            case SVC_OK:
                ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_svc_ok);
                break;
            case SVC_ERR:
                ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, mhd_svc_err);
                break;
            case SVC_BADFMT:
                ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, mhd_svc_err);
                break;
            default:
                ret = MHD_NO;
            }
        }
        else
            ret = MHD_queue_response(connection, MHD_HTTP_FORBIDDEN, mhd_empty);
    }

    return ret;
}


static int read_username(const char *certs) {
    int  res = 0, len;
    FILE *file;
    char path[strlen(certs) + sizeof("/" USERNAME_SFX)];

    strcpy(path, certs);
    strcat(path, "/" USERNAME_SFX);

    if ((file = fopen(path, "r"))) {
        if(fgets(username, sizeof(username), file)  &&  fgetc(file) == EOF) {
            len = strlen(username);
            if (username[len-1] == '\n')
                username[len-1] = '\0';

            if (vfybase32(USERNAME_LENGTH, username))
                res = 1;
        }

        if (fclose(file))
            res = 0;
    }

    return res;
}


int init_server(const char *certs, const char *qpath, const char *rqpath,
                const char *host,  const char *port) {
#ifdef TESTING
    const enum MHD_FLAG extra_flags = MHD_USE_DEBUG;
#else
    const enum MHD_FLAG extra_flags = 0;
#endif
    struct addrinfo    addr_hints, *address;
    int                addr_res;


    /* save paths */
    crt_path = certs;
    cq_path  = qpath;
    crq_path = rqpath;

    if (!read_username(certs)) {
        flog(LOG_ERR, "could not read %s/username", certs);
        return 0;
    }


    /* create immutable responses */
    if (   !(mhd_empty   = MHD_create_response_from_buffer(0,                      NULL,         MHD_RESPMEM_PERSISTENT))
        || !(mhd_svc_ok  = MHD_create_response_from_buffer(sizeof(SVC_RESP_OK)-1,  SVC_RESP_OK,  MHD_RESPMEM_PERSISTENT))
        || !(mhd_svc_err = MHD_create_response_from_buffer(sizeof(SVC_RESP_ERR)-1, SVC_RESP_ERR, MHD_RESPMEM_PERSISTENT))
        || MHD_NO == MHD_add_response_header(mhd_svc_ok,  MHD_HTTP_HEADER_CONTENT_TYPE,  "text/plain")
        || MHD_NO == MHD_add_response_header(mhd_svc_ok,  MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache")
        || MHD_NO == MHD_add_response_header(mhd_svc_err, MHD_HTTP_HEADER_CONTENT_TYPE,  "text/plain"))
        return 0;


    /* translate host address */
    memset(&addr_hints, 0, sizeof(addr_hints));
    addr_hints.ai_family   = AF_INET;
    addr_hints.ai_socktype = SOCK_STREAM;
    addr_hints.ai_protocol = IPPROTO_TCP;
    addr_hints.ai_flags    = AI_V4MAPPED | AI_ADDRCONFIG | AI_PASSIVE;

    if ((addr_res = getaddrinfo((*host ? host : NULL), port, &addr_hints, &address))) {
        flog(LOG_ERR, "%s", gai_strerror(addr_res));
        return 0;
    }


    if (!(mhd_daemon = MHD_start_daemon(
              MHD_USE_SELECT_INTERNALLY | MHD_USE_PEDANTIC_CHECKS | MHD_SUPPRESS_DATE_NO_CLOCK | extra_flags,
              /* port, ignored if sock_addr is specified, but must be non-0 */
              -1,
              /* MHD_AcceptPolicyCallback */
              NULL, NULL,
              /* MHD_AccessHandlerCallback */
              handle_connection, NULL,
              /* options section */
              MHD_OPTION_CONNECTION_LIMIT, (unsigned) WAIT_CONN,
              MHD_OPTION_THREAD_POOL_SIZE, (unsigned) MAX_THREAD,
              MHD_OPTION_SOCK_ADDR, address->ai_addr,
              MHD_OPTION_END)))
        return 0;


    freeaddrinfo(address);

    return 1;
}

int shutdown_server() {
    MHD_stop_daemon(mhd_daemon);

    MHD_destroy_response(mhd_svc_err);
    MHD_destroy_response(mhd_svc_ok);
    MHD_destroy_response(mhd_empty);

    return 1;
}
