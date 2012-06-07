#ifndef UTIL_H
#define UTIL_H

/* logging priorities */
#include <syslog.h>

void syslog_init();
void flog(int priority, const char *format, ...);

int vfyhex(int sz, const char *s);
int vfybase32(int sz, const char *s);

char* alloc_env(const char *var, const char *suffix);

/* requires -lrt */
void rand_init();
double rand_shift();
double getmontime();
void sleepsec(double sec);

#endif
