#ifndef UTIL_H
#define UTIL_H

/* logging priorities */
#include <syslog.h>

/* warning() and error() assume that errno is set */
void syslog_init();
void flog(int priority, const char *format, ...);
void warning(const char *prefix);
void error(const char *prefix);

int vfyhex(int sz, const char *s);
int vfybase32(int sz, const char *s);

char* alloc_env(const char *var, const char *suffix);
void dealloc_env(char *env);

/* requires -lrt */
int rand_init();
double rand_shift();
double getmontime();
void sleepsec(double sec);

#endif
