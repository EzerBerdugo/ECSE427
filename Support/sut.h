#ifndef __SUT_H__
#define __SUT_H__
#include <stdbool.h>

typedef void (*sut_task_f)();

void sut_init();
bool sut_create(sut_task_f fn);

void sut_yield();
void sut_exit();

int sut_open(char *file_name);
void sut_close(int fd);

void sut_write(int fd, char *buf, int size);
char* sut_read(int fd, char *buf, int size);

void sut_shutdown();

#endif
