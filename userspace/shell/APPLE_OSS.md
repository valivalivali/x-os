# Apple Open Source shell stack

Vendored / referenced from macOS 26.5 Apple OSS (local Desktop copies):

| Package | Local path |
|---------|------------|
| zsh-118 | `/Users/vali/Desktop/zsh-zsh-118` |
| file_cmds-479 | `/Users/vali/Desktop/file_cmds-file_cmds-479` |
| text_cmds-199 | `/Users/vali/Desktop/text_cmds-text_cmds-199` |
| basic_cmds-70 | `/Users/vali/Desktop/basic_cmds-basic_cmds-70` |
| libedit-65 | `/Users/vali/Desktop/libedit-libedit-65` |
| sudo-114.100.11 | `/Users/vali/Desktop/sudo-sudo-114.100.11` |

`userspace/shell/zsh-src` is the built zsh 5.9 tree (Apple zsh-118 lineage).

**Current boot path:** `zsh_entry.c` → IPC bridge → mini-shell (no allowlist) → `exec /bin/<cmd>`.

**Apple `zsh_main`:** linked; enable with `-DXOS_USE_ZSH_MAIN=1` once job-control wait/preempt is solid. Config was built without `HAVE_WAITPID`; zsh’s wait path plus cooperative scheduling blocked external commands.

External utilities: `userspace/cmds` multicall (shell_cmds / file_cmds / text_cmds). Init seeds `/bin/<name>` copies (XFS has no symlinks).

Apple `zprofile` / `zshrc`: `userspace/shell/apple-etc/`.
