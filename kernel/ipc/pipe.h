#pragma once
#include <stdint.h>
#include <stddef.h>

/* Kernel pipe — ring buffer for shell pipelines.
 * pipefd[0] = read end, pipefd[1] = write end. */

int pipe_create(int pipefd[2]);
int pipe_read(int fd, void *buf, size_t count);
int pipe_write(int fd, const void *buf, size_t count);
void pipe_close(int fd);
int pipe_dup(int oldfd, int newfd);
