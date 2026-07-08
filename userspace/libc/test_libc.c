/* Minimal test program to verify newlib + syscall stubs work.
 * This links against newlib's libc.a and our syscall stubs.
 * It should be loadable via exec() from the filesystem.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void _start(void) {
    /* Simple test: write to stdout (fd 1) */
    const char *msg = "Hello from newlib!\n";
    write(1, msg, strlen(msg));

    /* Test malloc */
    char *buf = malloc(64);
    if (buf) {
        strcpy(buf, "malloc works!\n");
        write(1, buf, strlen(buf));
        free(buf);
    }

    /* Test printf (writes to stdout via _write) */
    printf("printf: pid=%d\n", getpid());

    /* Exit */
    _exit(0);
}
