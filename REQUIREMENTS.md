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
    int     memfd;           // -1 for bundles
    char    bundle_dir[256]; // empty for single binaries
    char    exec_path[256];  // relative path inside bundle
    bool    is_bundle;
    pid_t   pid;             // -1 if not running
    int     debug_port;      // -1 if not debugging
    int     output_pipe_fd;  // pipe read end for stdout/stderr capture
    char    output[256KB];   // ring buffer of captured output
    char    saved_args[];    // saved arguments string
    map     env_vars;        // custom environment variable overrides
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
| **upload** (single binary) | `memfd_create` + write + ELF validate → state=LOADED |
| **upload** (bundle) | write tar.gz to tmpfile, extract to tmpdir, validate exec_path is ELF → state=LOADED |
| **start** (single binary) | `fork` + `fexecve(memfd)` → state=RUNNING |
| **start** (bundle) | `fork` + `chdir(bundle_dir)` + `execve(exec_path)` → state=RUNNING |
| **start with args** | Uses saved args (set via `ARGS` command) as argv for the binary |
| **start with env** | Merges session env vars with daemon environ; session overrides take precedence |
| **stop** | `kill(pid, SIGTERM)` + `waitpid` → state=STOPPED |
| **kill** | `kill(pid, SIGKILL)` + `waitpid` → state=STOPPED |
| **debug** | `gdbserver :PORT --attach <pid>` → state=DEBUGGING |
| **start --debug** (single) | `gdbserver :PORT /proc/self/fd/X` → state=DEBUGGING |
| **start --debug** (bundle) | `gdbserver :PORT bundle_dir/exec_path` → state=DEBUGGING |
| **delete** (single) | ensure stopped, `close(memfd)` → free RAM |
| **delete** (bundle) | ensure stopped, `rm -rf bundle_dir` → free disk |

## Process Monitoring

Use `pidfd_open(pid, 0)` + `epoll`. When process exits, epoll triggers and session state is cleaned. Avoids `SIGCHLD` complexity.

## Output Capture

Child process stdout and stderr are redirected to a pipe. The pipe read-end is added to the epoll loop. Output is drained into a per-session ring buffer (max 256 KB). Oldest output is trimmed when the buffer is full. Output is preserved across process stop/restart (cleared on re-start).

Clients retrieve output via the `OUTPUT <id> [offset]` command. The `offset` parameter enables streaming: clients track their read position and request only new data.

## Control Protocol

TCP, line-framed commands. JSON responses.

Commands: `UPLOAD <size> [exec_path]`, `START <id> [--debug]`, `ARGS <id> <args...>`, `ENV <id> KEY=VALUE`, `ENVDEL <id> KEY`, `ENVLIST <id>`, `STOP <id>`, `KILL <id>`, `DEBUG <id>`, `LIST`, `STATUS <id>`, `DELETE <id>`, `OUTPUT <id> [offset]`, `DEPS`

When `exec_path` is provided, the upload is treated as a tar.gz bundle. The server extracts the archive and uses the binary at `exec_path` (relative to bundle root) for execution and debugging.

## System Dependencies

The daemon checks for the following system packages at runtime:

| Dependency | Purpose | Required |
|------------|---------|----------|
| `gdbserver` | Debug attach and `start --debug` | Yes |
| `tar` | Bundle (tar.gz) extraction | Yes |
| `gzip` | Bundle (tar.gz) decompression | Yes |

Use the `DEPS` command (or `debuglanternctl deps`) to check availability. The web UI also displays dependency status.

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
