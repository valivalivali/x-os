/* zsh_entry.c — C entry point for zsh on x-os.
 * Sets up IPC bridge with the terminal, then runs an interactive
 * echo shell loop. Falls back to zsh_main if it doesn't hang. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"

extern void set_shell_bridge(port_handle_t input_port, port_handle_t output_port);

/* Shell bridge IPC message types */
#define SHELL_BRIDGE_HELLO   0x1000  /* terminal → zsh: here is your input port */

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void log(const char *s) {
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, my_strlen(s));
}

/* The terminal registers PORT_NS_SHELL_BRIDGE. zsh creates its own input
 * port, sends a HELLO message to the bridge port with its input port handle,
 * and the terminal starts forwarding keyboard chars to it.
 *
 * After handshake, _read(fd=0) receives chars from the terminal via IPC,
 * and _write(fd=1,2) sends output text to the terminal via IPC. */

void zsh_entry(void) {
    log("[zsh] entry\n");

    /* Create our input port (terminal will send keyboard chars here) */
    port_handle_t my_input = sys_port_create();
    if (!my_input) {
        log("[zsh] port create fail, idling\n");
        for (;;) syscall0(SYS_YIELD);
    }
    log("[zsh] input port created\n");

    /* Look up the terminal's bridge port */
    port_handle_t bridge = 0;
    for (int i = 0; i < 200 && !bridge; i++) {
        bridge = sys_ns_lookup(PORT_NS_SHELL_BRIDGE);
        if (!bridge) syscall0(SYS_YIELD);
    }
    if (!bridge) {
        log("[zsh] bridge port not found, idling\n");
        for (;;) syscall0(SYS_YIELD);
    }
    log("[zsh] bridge found\n");

    /* Send HELLO message with our input port handle.
     * The terminal will read our input port handle from the payload
     * and use it to send us keyboard chars. It will also know our
     * PID from sender_pid to send output back to us — but actually,
     * the terminal needs an output port from us too. Let's think...
     *
     * Actually, the terminal needs to know:
     * 1. Our input port (to send keyboard chars to us)
     * 2. A port to receive our output from
     *
     * We'll create an output port too and send both in the HELLO.
     * But IPC messages only go to one port. So the terminal creates
     * its own port for receiving zsh output, and we need to know it.
     *
     * Simpler approach: the terminal creates the bridge port and
     * registers it. zsh sends HELLO with its input port handle.
     * The terminal then sends its output-receive port handle back
     * as a response. zsh uses that as g_bridge_output_port.
     *
     * Actually even simpler: zsh creates two ports:
     * - input_port: terminal sends keyboard chars here
     * - The bridge port itself IS the terminal's port. zsh sends
     *   output TO the bridge port. The terminal receives from it.
     *   And the terminal sends keyboard chars to zsh's input port.
     *
     * So: g_bridge_output_port = bridge (terminal's port)
     *     g_bridge_input_port = my_input (zsh's port)
     *
     * The HELLO message tells the terminal zsh's input port handle. */

    /* Send HELLO to terminal: payload = our input port handle */
    ipc_msg_t hello;
    /* Manual zeroing — avoid SSE instructions */
    for (size_t i = 0; i < sizeof(hello); i++) ((uint8_t *)&hello)[i] = 0;
    hello.type = IPC_MSG_REQUEST;
    hello.sender_pid = syscall0(SYS_PROC_PID);
    hello.cap_count = 0;
    hello.payload_len = sizeof(uint32_t) + sizeof(uint64_t);
    uint32_t hello_type = SHELL_BRIDGE_HELLO;
    /* Manual copy — avoid SSE instructions */
    for (size_t i = 0; i < sizeof(uint32_t); i++) hello.payload[i] = ((uint8_t *)&hello_type)[i];
    for (size_t i = 0; i < sizeof(uint64_t); i++) hello.payload[sizeof(uint32_t) + i] = ((uint8_t *)&my_input)[i];

    if (!sys_port_send(bridge, &hello)) {
        log("[zsh] hello send fail\n");
        return;
    }
    log("[zsh] hello sent\n");

    /* Set up the bridge: input from my_input, output to bridge (terminal's port) */
    set_shell_bridge(my_input, bridge);

    log("[zsh] bridge active, starting shell\n");

    /* Write welcome banner */
    const char welcome[] = "x-os zsh shell ready\n";
    /* write(1, welcome, sizeof) — goes through _write which uses bridge */
    /* We need to call write() which is the newlib wrapper that calls _write.
     * But we don't have newlib headers here. Just call _write directly. */
    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    _write(1, welcome, sizeof(welcome) - 1);

    /* Interactive echo shell loop */
    const char prompt[] = "x> ";
    extern _ssize_t _read(int fd, void *buf, size_t cnt);

    for (;;) {
        _write(1, prompt, sizeof(prompt) - 1);

        char buf[256];
        _ssize_t n = _read(0, buf, sizeof(buf));
        if (n <= 0) continue;

        /* Echo back what was typed */
        _write(1, buf, (size_t)n);

        /* If newline, the character is already echoed. */
        /* Check for 'exit' command */
        if (n >= 4) {
            if (buf[0] == 'e' && buf[1] == 'x' && buf[2] == 'i' && buf[3] == 't') {
                const char bye[] = "goodbye\n";
                _write(1, bye, sizeof(bye) - 1);
                sys_exit(0);
            }
        }
    }
}
