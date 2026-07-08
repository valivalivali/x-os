/* Entry point for zsh shell on x-os.
 * Calls zsh's zsh_main() with empty argv.
 * The kernel sets rsp = USER_STACK_TOP - 16 which is 16-byte aligned,
 * but x86_64 ABI expects rsp%16==8 at function entry (as if a call pushed
 * a return address). We adjust rsp to match the ABI convention.
 */
#include <stdlib.h>
#include <unistd.h>

extern int zsh_main(int argc, char **argv);

void _start(void) {
    /* Align stack to 16 bytes + 8 (simulating a return address push) */
    __asm__ volatile("andq $-16, %%rsp\n\tsubq $8, %%rsp\n" ::: "rsp");
    /* Test output to verify zsh process is running */
    write(1, "zsh: starting...\n", 17);
    char *argv[] = { "zsh", (char *)0 };
    int ret = zsh_main(1, argv);
    write(1, "zsh: exited\n", 12);
    _exit(ret);
}
