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

## Start Under Debugger

```sh
debuglanternctl start a3f2c9d1 --debug
```

```json
{ "id": "a3f2c9d1", "state": "DEBUGGING", "debug_port": 5504 }
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
debuglanternctl start "$ID"
sleep 2
debuglanternctl debug "$ID"
PORT=$(debuglanternctl status "$ID" | jq -r .debug_port)
gdb -batch -ex "target remote device:$PORT" -ex "bt"
debuglanternctl kill "$ID"
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
