#ifndef SERVICE_H
#define SERVICE_H

enum SVC_Status {
    SVC_BADFMT = -1,
    SVC_ERR    = 0,
    SVC_OK     = 1
};

enum SVC_Status handle_request(const char *request, const char *queues, const char *rqueues);

#endif
