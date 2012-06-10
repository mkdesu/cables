#ifndef PROCESS_H
#define PROCESS_H

int init_process_acc();
int run_process(long maxproc, double waitsec, const char *const argv[]);

int stop_requested();

#endif
