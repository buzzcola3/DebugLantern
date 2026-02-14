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
- `STOP <id>`
- `KILL <id>`
- `DEBUG <id>`
- `LIST`
- `STATUS <id>`
- `DELETE <id>`
- `DEPS`
  - Returns a JSON object listing required system dependencies and whether each is available on the host.
  - No arguments.

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

## Notes

- Commands are parsed per-line; extra bytes after a line are interpreted as the next command or upload payload.
- The server closes the connection on protocol violations or upload write failures.
