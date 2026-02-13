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
- `START <id> [--debug]`
- `STOP <id>`
- `KILL <id>`
- `DEBUG <id>`
- `LIST`
- `STATUS <id>`
- `DELETE <id>`

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

Client:

```
UPLOAD 1048576\n
<1048576 raw bytes>
```

Server:

```
{ "id": "...", "state": "LOADED", "size": 1048576 }\n
```

## Notes

- Commands are parsed per-line; extra bytes after a line are interpreted as the next command or upload payload.
- The server closes the connection on protocol violations or upload write failures.
