#ifndef DAEMON_H
#define DAEMON_H

/* common constants */
#define VERSION         "LIBERTE CABLE 3.0"

#define MSGID_LENGTH    40
#define USERNAME_LENGTH 32

enum SVC_Status {
    SVC_BADFMT = -1,
    SVC_ERR    = 0,
    SVC_OK     = 1
};


/* common functions: daemon */
void flog(int priority, const char *format, ...);


/* common functions: server */
int init_server(const char *certs, const char *qpath, const char *rqpath, const char *host,  const char *port);
int shutdown_server();


/* common functions: service */
enum SVC_Status handle_request(const char *request, const char *queues, const char *rqueues);
int vfyhex(int sz, const char *s);
int vfybase32(int sz, const char *s);

#endif
