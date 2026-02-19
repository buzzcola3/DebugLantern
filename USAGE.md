# debuglantern — Usage

## Discover Device

```sh
avahi-browse -rt _mydebug._tcp
```

```
= eth0 IPv4 target-board  _mydebug._tcp  local
   hostname = [target-board.local]
   address = [192.168.1.50]
   port = [4444]
```

## Upload Binary

```sh
debuglanternctl upload my_app --target target-board.local
```

```json
{ "id": "a3f2c9d1", "state": "LOADED", "size": 1048576 }
```

## Upload Bundle

Upload a tar.gz archive and specify which binary inside it to run:

```sh
debuglanternctl upload openautoflutter-arm64.tar.gz --exec-path openautoflutter/openautoflutter --target target-board.local
```

```json
{ "id": "b7e812aa", "state": "LOADED", "size": 52428800, "bundle": true, "exec_path": "openautoflutter/openautoflutter" }
```

The archive is extracted on the device. Shared libraries, assets, and other files in the bundle are preserved alongside the binary. The working directory is set to the bundle root when the process starts.

## Start

```sh
debuglanternctl start a3f2c9d1
```

```json
{ "id": "a3f2c9d1", "state": "RUNNING", "pid": 2134 }
```

## Start with Arguments

Set arguments for a session (saved and reused on every start):

```sh
debuglanternctl args a3f2c9d1 "--port 8080 --config /etc/app.conf"
```

```json
{ "id": "a3f2c9d1", "state": "LOADED", "pid": null, "debug_port": null, "args": "--port 8080 --config /etc/app.conf" }
```

Then start — saved args are used automatically:

```sh
debuglanternctl start a3f2c9d1
```

```json
{ "id": "a3f2c9d1", "state": "RUNNING", "pid": 2135 }
```

Arguments are forwarded as argv to the executable. Works with both single binaries and bundles. Update args at any time while the session is stopped.

## Environment Variables

Set environment variables for a session:

```sh
debuglanternctl env a3f2c9d1 LD_LIBRARY_PATH=/opt/libs:/usr/lib
debuglanternctl env a3f2c9d1 MY_CONFIG=/etc/app.conf
```

List environment variables:

```sh
debuglanternctl envlist a3f2c9d1
```

```json
{"LD_LIBRARY_PATH":"/opt/libs:/usr/lib","MY_CONFIG":"/etc/app.conf"}
```

Remove an environment variable:

```sh
debuglanternctl envdel a3f2c9d1 MY_CONFIG
```

Environment variables are merged with the daemon's environment at start time. Session overrides take precedence. Works with both single binaries and bundles.

## Start Under Debugger

```sh
debuglanternctl start a3f2c9d1 --debug
```

```json
{ "id": "a3f2c9d1", "state": "DEBUGGING", "debug_port": 5504 }
```

Debug start with arguments:

```sh
debuglanternctl args a3f2c9d1 "--port 8080"
debuglanternctl start a3f2c9d1 --debug
```

Then connect:

```sh
gdb my_app
(gdb) target remote 192.168.1.50:5504
```

## List Sessions

```sh
debuglanternctl list
```

```json
[
  { "id": "a3f2c9d1", "state": "RUNNING", "pid": 2134, "debug_port": null },
  { "id": "b7e812aa", "state": "STOPPED", "pid": null, "debug_port": null }
]
```

## Stop (Graceful)

```sh
debuglanternctl stop a3f2c9d1
```

```json
{ "id": "a3f2c9d1", "state": "STOPPED" }
```

## Kill (Force)

```sh
debuglanternctl kill a3f2c9d1
```

## Attach Debugger to Running Process

```sh
debuglanternctl debug a3f2c9d1
```

```json
{ "id": "a3f2c9d1", "state": "DEBUGGING", "debug_port": 5503 }
```

Then on your PC:

```sh
gdb my_app
(gdb) target remote 192.168.1.50:5503
```

Detach from inside GDB (`detach` / `quit`) — session returns to `RUNNING`.

## View Process Output

Get stdout/stderr output of a running (or stopped) process:

```sh
debuglanternctl output a3f2c9d1
```

```
Hello world
Listening on port 8080
Client connected from 192.168.1.10
```

## Stream Output (Follow Mode)

Continuously stream new output (like `tail -f`):

```sh
debuglanternctl output a3f2c9d1 --follow
```

Output streams in real time until interrupted with Ctrl+C. Useful for monitoring long-running services.

## Delete Session

```sh
debuglanternctl delete a3f2c9d1
```

```json
{ "id": "a3f2c9d1", "state": "DELETED" }
```

## Multiple Binaries

```sh
debuglanternctl upload sensor
debuglanternctl upload ui
debuglanternctl upload logger

debuglanternctl start sensor_id
debuglanternctl start ui_id
debuglanternctl start logger_id
```

Three independent processes; debug, stop, or kill each separately.

## CI / Automation

```sh
ID=$(debuglanternctl upload build/output/test_binary | jq -r .id)
debuglanternctl args "$ID" "--test-mode --verbose"
debuglanternctl start "$ID"
sleep 2
debuglanternctl output "$ID"
debuglanternctl debug "$ID"
PORT=$(debuglanternctl status "$ID" | jq -r .debug_port)
gdb -batch -ex "target remote device:$PORT" -ex "bt"
debuglanternctl kill "$ID"
debuglanternctl output "$ID"
debuglanternctl delete "$ID"
```

## CI / Automation (Bundle)

```sh
ID=$(debuglanternctl upload build/output/my_app.tar.gz --exec-path my_app/my_app | jq -r .id)
debuglanternctl start "$ID"
sleep 2
debuglanternctl debug "$ID"
PORT=$(debuglanternctl status "$ID" | jq -r .debug_port)
gdb -batch -ex "target remote device:$PORT" -ex "bt"
debuglanternctl kill "$ID"
debuglanternctl delete "$ID"
```

## Typical Dev Loop

```
build locally  →  upload  →  start  →  attach gdb  →  fix bug  →  re-upload
```

No SSH. No SCP. No filesystem pollution. Everything ephemeral.

## Check System Dependencies

Verify the target device has all required tools installed:

```sh
debuglanternctl deps --target target-board.local
```

```json
{
  "deps": [
    { "name": "gdbserver", "description": "Required for debug attach and start --debug", "available": true, "required": true },
    { "name": "tar", "description": "Required for bundle extraction", "available": true, "required": true },
    { "name": "gzip", "description": "Optional: only required if you upload gzip-compressed bundles", "available": true, "required": false }
  ],
  "all_satisfied": true
}
```

The web UI dashboard also shows dependency status — click the gear icon in the status bar.
