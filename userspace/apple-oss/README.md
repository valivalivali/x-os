# Vendored Apple Open Source pieces for X OS

Source trees live on the Desktop (macOS 26.5 OSS). We **do not** drop full
XNU/Mach into the kernel. Instead we copy small, adaptable pieces here and
reimplement the rest against X OS syscalls.

## Desktop packages (upstream)

| Package | Desktop path | How we use it |
|---------|--------------|---------------|
| xnu | `~/Desktop/xnu-xnu-12377.121.6` | Ideas only: boot-args, msgbuf, wait/signal |
| zsh | `~/Desktop/zsh-zsh-118` | `zsh/` → `userspace/shell/zsh-src`; outer package → `userspace/shell/apple-zsh-118/` |
| shell_cmds | `~/Desktop/shell_cmds-shell_cmds-329` | Adapted into `userspace/cmds/cmds_main.c` |
| file_cmds | `~/Desktop/file_cmds-file_cmds-479` | Same multicall binary |
| text_cmds | `~/Desktop/text_cmds-text_cmds-199` | Same |
| basic_cmds | `~/Desktop/basic_cmds-basic_cmds-70` | Later (`mesg`/`write`) |
| adv_cmds | `~/Desktop/adv_cmds-adv_cmds-237` | `ps`, `pkill` (adapted) |
| system_cmds | `~/Desktop/system_cmds-system_cmds-1042.120.1` | `dmesg`, `sysctl`, `hostinfo`, `sw_vers`, `pagesize`, `arch` |
| files | `~/Desktop/files-files-974.120.2` | `etc/` skeletons (copied below) |
| usertemplate | `~/Desktop/usertemplate-usertemplate-109` | Home layout + prefs plist |
| libedit / ncurses | Desktop | Needs port for real line editing |
| sudo | Desktop | Stub in cmds until PAM exists |

## In this directory

- `etc/` — from `files-974` (`hosts`, `shells`, `paths`, `networks`)
- `GlobalPreferences.plist` — from usertemplate (locale defaults)
- `SystemVersion.plist` — X OS identity (sw_vers / CoreServices)

Init currently embeds equivalent strings for the ramdisk; keep these files as
the source of truth when we grow a real `/etc` install path.
