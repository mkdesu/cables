#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#ifdef TESTING
#include <stdio.h>
#endif

#include "util.h"


/* logging init */
void syslog_init() {
    openlog("cable", LOG_PID, LOG_MAIL);
}

/* logging */
void flog(int priority, const char *format, ...) {
    va_list ap;
    va_start(ap, format);

#ifndef TESTING
    vsyslog(priority, format, ap);
#else
    fprintf(stderr, "[%d] cable: ", priority);
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");
#endif

    va_end(ap);
}

/* non-fatal error */
void warning(const char *prefix) {
    flog(LOG_WARNING, "%s: %m", prefix);
}

/* fatal errors which shouldn't happen in a correct program */
void error(const char *prefix) {
    flog(LOG_CRIT, "%s: %m", prefix);
    exit(EXIT_FAILURE);
}


/* lowercase hexadecimal (0-9, a-f) */
int vfyhex(int sz, const char *s) {
    if (strlen(s) != sz)
        return 0;

    for (; *s; ++s)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
            return 0;

    return 1;
}


/* lowercase Base-32 encoding (a-z, 2-7) */
int vfybase32(int sz, const char *s) {
    if (strlen(s) != sz)
        return 0;

    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '2' && *s <= '7')))
            return 0;

    return 1;
}


/* allocate buffer for environment variable + suffix */
char* alloc_env(const char *var, const char *suffix) {
    const char *value;
    char       *buf;
    size_t     varlen;

    if (!((value = getenv(var)))) {
        flog(LOG_ERR, "environment variable %s is not set", var);
        exit(EXIT_FAILURE);
    }

    varlen = strlen(value);
    if (!((buf = (char*) malloc(varlen + strlen(suffix) + 1))))
        error("malloc failed");

    strncpy(buf, value, varlen);
    strcpy(buf + varlen, suffix);

    return buf;
}

void dealloc_env(char *env) {
    free(env);
}


/* initialize rng */
int rand_init() {
    struct timespec tp;

    return !clock_gettime(CLOCK_MONOTONIC, &tp)
        && (srandom(((unsigned) tp.tv_sec << 29) ^ (unsigned) tp.tv_nsec), 1);
}


/* uniformly distributed value in [-1, 1] */
double rand_shift() {
    return random() / (RAND_MAX / 2.0) - 1;
}


/* get strictly monotonic time (unaffected by ntp/htp) in seconds */
double getmontime() {
    struct timespec tp;

    if (clock_gettime(CLOCK_MONOTONIC, &tp))
        error("failed to read monotonic clock");

    return tp.tv_sec + tp.tv_nsec / 1e9;
}

/*
  sleep given number of seconds without interferences with SIGALRM
  do not complete interrupted sleeps, to facilitate fast process shutdown
 */
void sleepsec(double sec) {
    struct timespec req;

    /* support negative arguments */
    if (sec > 0) {
        req.tv_sec  = (time_t) sec;
        req.tv_nsec = (long) ((sec - req.tv_sec) * 1e9);

        if (nanosleep(&req, NULL)  &&  errno != EINTR)
            warning("sleep failed");
    }
}
