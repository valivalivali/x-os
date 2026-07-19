# Apple Open Source stack for X OS

Vendored / referenced from **macOS 26.5** Apple OSS (local Desktop copies).
XNU is **reference only** — we do not replace the X OS kernel.

## On Desktop (local paths)

Full map + vendored copies: [`userspace/apple-oss/README.md`](../apple-oss/README.md).

| Package | Path | Role |
|---------|------|------|
| xnu-12377.121.6 | `/Users/vali/Desktop/xnu-xnu-12377.121.6` | Reference: boot-args, msgbuf, wait/signal |
| zsh-118 | `/Users/vali/Desktop/zsh-zsh-118` | `zsh/` → `userspace/shell/zsh-src` (Apple 5.9); package chrome → `apple-zsh-118/` |
| shell_cmds / file_cmds / text_cmds | Desktop | Adapted into `cmds_main.c` multicall |
| adv_cmds-237 | Desktop | `ps`, `pkill` |
| system_cmds-1042 | Desktop | `dmesg`, `sysctl`, `hostinfo`, `sw_vers`, `pagesize`, `arch`, `sync` |
| files-974 / usertemplate | Desktop | Copied under `userspace/apple-oss/` |
| libedit / ncurses / sudo | Desktop | Later (needs more libc/PAM) |

## Boot chrome

Inspired by XNU `pexpert` (reimplemented):

- `kernel/boot/bootargs.c` — `PE_parse_boot_argn`-style parser
- Limine `cmdline:` → boot-args (`-v` = verbose serial)
- Silent by default; `-v` enables `[boot]` lines (see `boot/uefi/limine.conf`)
- Grey Apple / progress bar: deferred (desktop first paint preferred)

## Shell path

**Current:** `zsh_entry.c` → IPC bridge → Apple `zsh_main` (`-i -f`).

**Fallback mini-shell:** set `XOS_USE_ZSH_MAIN` to `0` in `zsh_entry.c`.

Apple `zprofile` / `zshrc`: `userspace/shell/apple-etc/`.

## Done recently

- Relative `chdir` / path resolution (`cd System` works)
- libc `open(".")` uses kernel cwd (was hardcoding `/`)
- `ps` via `SYS_PROC_LIST` + process `comm` names (adv_cmds-inspired)
- Kernel **msgbuf** + `SYS_SYSCTL` / `SYS_MSGBUF_READ` → real `dmesg` / `sysctl`
- Terminal scrollbar drag + PS/2 mouse wheel → egui ScrollArea
- Seed `/System/Library/*`, `/Users/vali`, `/etc` + `/private/etc` from files-974 / usertemplate

## Next ports (priority)

1. ~~wait/signal basics~~ — `WNOHANG`, wait status, `getppid`, `sigaction`/`sigprocmask`  
2. ~~SIGCHLD + catchable delivery~~ — pending mask, stack trampoline, `SYS_SIGRETURN`  
3. Shake out zsh job-control / TTY gaps under `zsh_main`  
4. Full Apple libedit + ncurses (beyond mini line-edit)  
5. Download `network_cmds` for `ping` / `ifconfig` (not on Desktop yet)  
6. `login` / `getty` from system_cmds once multi-user exists
