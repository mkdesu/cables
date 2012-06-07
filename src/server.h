#ifndef SERVER_H
#define SERVER_H

int init_server(const char *certs, const char *qpath, const char *rqpath, const char *host,  const char *port);
int shutdown_server();

#endif
