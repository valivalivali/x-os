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
/* Bump end-refs on pipes created by parent so the child keeps working copies. */
void pipe_fork_inherit(uint32_t parent_pid);
/* Poll helpers for select/poll support. */
int pipe_readable(int fd);   /* returns 1 if pipe has data to read */
int pipe_writable(int fd);   /* returns 1 if pipe has space to write */
