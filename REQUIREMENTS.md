# debuglantern — Requirements

## Overview

RAM-based remote process manager with dynamic GDB attach over mDNS-discoverable control server.

## Architecture

```
+----------------------+
|    debuglantern      |
|----------------------|
| 1. mDNS advertiser   |
| 2. Control server    |
| 3. Session registry  |
| 4. Process manager   |
+----------+-----------+
           |
  ----------------------------
  |         |                |
Session A  Session B    Session C
```

Single-threaded `epoll` loop. All sockets non-blocking. `pidfd` integrated into the same loop.

## Session

```c
struct session {
    uuid_t  id;
    int     memfd;
    pid_t   pid;          // -1 if not running
    int     debug_port;   // -1 if not debugging
    enum { LOADED, RUNNING, DEBUGGING, STOPPED } state;
};
```

Storage: hash table (UUID -> session) or indexed vector.

## State Machine

```
LOADED  --START-->  RUNNING  --STOP/KILL-->  STOPPED
                      |  ^                      |
                    DEBUG  detach             START
                      v  |                      |
                   DEBUGGING                RUNNING
```

## Lifecycle

| Operation | Action |
|-----------|--------|
| **upload** | `memfd_create` + write + ELF validate → state=LOADED |
| **start** | `fork` + `fexecve(memfd)` → state=RUNNING |
| **stop** | `kill(pid, SIGTERM)` + `waitpid` → state=STOPPED |
| **kill** | `kill(pid, SIGKILL)` + `waitpid` → state=STOPPED |
| **debug** | `gdbserver :PORT --attach <pid>` → state=DEBUGGING |
| **start --debug** | `gdbserver :PORT /proc/self/fd/X` → state=DEBUGGING |
| **delete** | ensure stopped, `close(memfd)` → free RAM |

## Process Monitoring

Use `pidfd_open(pid, 0)` + `epoll`. When process exits, epoll triggers and session state is cleaned. Avoids `SIGCHLD` complexity.

## Control Protocol

TCP, line-framed commands. JSON responses.

Commands: `UPLOAD <size>`, `START <id>`, `STOP <id>`, `KILL <id>`, `DEBUG <id>`, `LIST`, `STATUS <id>`, `DELETE <id>`

## Discovery

Advertise via mDNS: `_mydebug._tcp` on configured port (default 4444).

## Resource Limits

- `MAX_SESSIONS` — cap concurrent sessions
- `MAX_TOTAL_BINARY_SIZE` — cap total RAM usage
- Per-process: `RLIMIT_CPU`, `RLIMIT_AS`, `RLIMIT_NOFILE`

## Security

- Drop privileges, never run as root
- Optional namespace isolation: `unshare(CLONE_NEWUSER | CLONE_NEWPID)`
- Optional `chroot` into tmpfs

## Estimated Size

~2000–3500 LOC (C) or equivalent Rust with `tokio` + `nix`.
