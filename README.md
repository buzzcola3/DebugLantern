# debuglantern

RAM-based remote process manager with dynamic GDB attach, mDNS discovery, and a real-time web dashboard.

Upload ELF binaries over the network, execute them from RAM (memfd), attach gdbserver on demand — no SSH, no filesystem, everything ephemeral.

## Build

Requires: `libavahi-client-dev`, `libavahi-common-dev`, `uuid-dev`, `gdbserver`

```sh
sudo apt install libavahi-client-dev libavahi-common-dev uuid-dev gdbserver
bazel build //:debuglanternd //:debuglanternctl
```

## Run

```sh
# Daemon only
./bazel-bin/debuglanternd --port 4444

# Daemon + web dashboard
./bazel-bin/debuglanternd --port 4444 --web-port 8080
```

## CLI Usage

```sh
# Discover devices
avahi-browse -rt _mydebug._tcp

# Upload binary
debuglanternctl upload ./my_app --target 192.168.1.50 --port 4444

# Start
debuglanternctl start <id> --target 192.168.1.50 --port 4444

# Attach debugger
debuglanternctl debug <id> --target 192.168.1.50 --port 4444

# Then from GDB:
gdb ./my_app
(gdb) target remote 192.168.1.50:<debug_port>

# Download device sysroot for GDB
debuglanternctl sysroot ~/pi-sysroot --target 192.168.1.50 --port 4444

# Stop / Kill / Delete
debuglanternctl stop <id> --target 192.168.1.50 --port 4444
debuglanternctl kill <id> --target 192.168.1.50 --port 4444
debuglanternctl delete <id> --target 192.168.1.50 --port 4444
```

## Web Dashboard

Pass `--web-port 8080` and open `http://<device>:8080` in a browser.

- Drag-and-drop ELF upload
- Real-time session list via SSE
- Start / Stop / Kill / Debug / Delete buttons
- Connection status indicator

## Daemon Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 4444 | Control protocol TCP port |
| `--web-port` | off | Web UI HTTP port (0 = disabled) |
| `--service-name` | debuglantern | mDNS service name |
| `--max-sessions` | 32 | Max concurrent sessions |
| `--max-total-bytes` | 512MB | Max total RAM for binaries |
| `--uid` / `--gid` | none | Drop privileges after bind |

## systemd

```sh
sudo cp debuglantern.service /etc/systemd/system/
sudo systemctl enable --now debuglantern
```

## Protocol

See [PROTOCOL.md](PROTOCOL.md) for the TCP framing spec.

## License

MIT
