# debuglantern Protocol

## Transport

- TCP connection to the control port (default 4444).
- Commands are line framed, ASCII text, terminated by `\n`.
- Responses are single-line JSON, terminated by `\n`.
- Binary payloads only follow `UPLOAD` and are not line framed.

## Command Grammar

```
COMMAND        := WORD *(SP WORD) LF
WORD           := 1*(ALNUM / "-" / "_" / "." / "/" / ":")
LF             := "\n"
SP             := " "
```

## Commands

- `UPLOAD <size>`
  - Client sends a line with the byte length, then exactly `<size>` raw bytes.
  - Server validates ELF magic and stores a memfd session.
- `UPLOAD <size> <exec_path>`
  - Client sends a line with the byte length and relative path to the executable, then exactly `<size>` raw bytes of a tar.gz archive.
  - Server extracts the archive to a temporary directory, validates the binary at `<exec_path>` is a valid ELF, and creates a bundle session.
  - `<exec_path>` is relative to the archive root (e.g., `my_app/my_app` or `bin/server`).
- `START <id> [--debug]`
  - Starts the session using any previously saved arguments.
  - When combined with `--debug`, the binary is launched under gdbserver.
- `ARGS <id> <arg1 arg2 ...>`
  - Sets (or updates) the saved arguments for a session. Arguments are persisted and used on every subsequent `START`.
  - The argument string is stored as-is and split on whitespace at start time.
  - Example: `ARGS a3f2c9d1 --port 8080 --verbose`
- `ENV <id> <KEY=VALUE>`
  - Sets (or updates) an environment variable for a session. Env vars are merged with the daemon's environment at start time.
  - Example: `ENV a3f2c9d1 LD_LIBRARY_PATH=/opt/libs:/usr/lib`
- `ENVDEL <id> <KEY>`
  - Removes a previously set environment variable from a session.
  - Example: `ENVDEL a3f2c9d1 LD_LIBRARY_PATH`
- `ENVLIST <id>`
  - Returns the session's custom environment variables as a JSON object.
- `STOP <id>`
- `KILL <id>`
- `DEBUG <id>`
- `LIST`
- `STATUS <id>`
- `DELETE <id>`
- `OUTPUT <id> [<offset>]`
  - Returns captured stdout/stderr output of the session's process.
  - Optional `<offset>` (byte offset) returns only new output since that position.
  - Output is buffered up to 256 KB per session (oldest data is trimmed).
- `DEPS`
  - Returns a JSON object listing required system dependencies and whether each is available on the host.
  - No arguments.
- `SYSROOT`
  - Downloads a tar.gz archive of the device's shared libraries for use as a GDB sysroot.
  - No arguments.
  - Collects `/lib`, `/lib64`, `/usr/lib`, and `/usr/lib/debug` (with symlinks dereferenced).
  - Response: `SYSROOT <size>\n` header followed by `<size>` raw bytes of tar.gz data.
  - On error, returns a standard JSON error response instead.

## Responses

Success responses are JSON objects or arrays, single line, newline terminated.

Example:

```json
{ "id": "a3f2c9d1", "state": "RUNNING", "pid": 2134, "debug_port": null }
```

Error responses are JSON objects with the following fields:

```json
{
  "ok": false,
  "error_code": "not_found",
  "message": "session not found",
  "time": "2026-02-13T12:34:56Z"
}
```

## Upload Framing Example

Single binary:

Client:

```
UPLOAD 1048576\n
<1048576 raw bytes>
```

Server:

```
{ "id": "...", "state": "LOADED", "size": 1048576 }\n
```

Bundle:

Client:

```
UPLOAD 52428800 my_app/my_app\n
<52428800 raw bytes of tar.gz>
```

Server:

```
{ "id": "...", "state": "LOADED", "size": 52428800, "bundle": true, "exec_path": "my_app/my_app" }\n
```

## Sysroot Response Example

```
SYSROOT
```

Success:

```
SYSROOT 104857600
<104857600 raw bytes of tar.gz>
```

Error:

```json
{ "ok": false, "error_code": "sysroot_no_libs", "message": "no system library directories found", "time": "2026-02-18T12:00:00Z" }
```

## Notes

- Commands are parsed per-line; extra bytes after a line are interpreted as the next command or upload payload.
- The server closes the connection on protocol violations or upload write failures.

## Output Response Example

```
OUTPUT a3f2c9d1
```

```json
{ "id": "a3f2c9d1", "output": "Hello world\nListening on port 8080\n", "offset": 0, "total": 35 }
```

With offset (streaming):

```
OUTPUT a3f2c9d1 35
```

```json
{ "id": "a3f2c9d1", "output": "Client connected\n", "offset": 35, "total": 53 }
```
