# rpi-platform

C++ MQTT/HTTPS server that runs on a Raspberry Pi and acts as the bridge between an ESP32-based smart incubator and a web dashboard: it subscribes to the ESP32's sensor/actuator MQTT topics, persists everything in SQLite, and serves a control panel for monitoring and manual overrides.

Part of a larger system - see [`incubator-node`](https://github.com/stefancosin/incubator-node) for the ESP32 firmware this talks to.

## Architecture

```
ESP32 (incubator-node)  <--MQTT-->  Mosquitto broker  <--MQTT-->  rpi-platform  <--HTTPS-->  Browser
                                                                        |
                                                                     SQLite
```

- **MqttClient** - subscribes to `incubator/sensors/+` and `incubator/peripherals/+/state`, writes every message to the database. Also publishes commands to `incubator/peripherals/<name>/set` on behalf of the HTTP layer.
- **Database** - SQLite (via SQLiteCpp), WAL mode so the MQTT thread and the HTTP thread pool don't block each other. Two tables: `sensors` (append-only log of readings) and `peripherals` (latest state per name, upserted).
- **HttpServer** - HTTPS (via cpp-httplib + OpenSSL), serves the dashboard pages and a small JSON API the pages poll.

### MQTT topic scheme

| Topic | Direction | Payload | Meaning |
|---|---|---|---|
| `incubator/sensors/<name>` | ESP32 → RPi | plain number | A sensor reading (`temperature`, `humidity`) |
| `incubator/peripherals/<name>/state` | ESP32 → RPi | string | Current state of a peripheral, reported by the ESP32 |
| `incubator/peripherals/<name>/set` | RPi → ESP32 | string | Command to a peripheral, published when `/set-peripheral` is called |

`<name>` covers both real actuators (`heater`, `cooler`, `alarm`, `motor`) and two pseudo-peripherals that ride the same scheme: `mode` (`auto`/`manual`) and the AUTO thresholds `temp_low`/`temp_high` (plain numbers). Using the same topic shape for all of these means the server needed zero special-casing to support the AUTO/MANUAL switch or runtime-configurable thresholds - they're just another named peripheral as far as this code is concerned.

### HTTP API

| Route | Method | Purpose |
|---|---|---|
| `/` | GET | Home page |
| `/sensors` | GET | Sensor log dashboard |
| `/control` | GET | Manual control panel (AUTO/MANUAL switch + actuator toggles) |
| `/auto-config` | GET | AUTO-mode threshold configuration page |
| `/health` | GET | `{"status":"ok"}` liveness check |
| `/get-sensors` | GET | All stored sensor readings, as JSON |
| `/peripheral-state?name=<name>` | GET | Latest known state of one peripheral |
| `/set-peripheral?name=<name>&state=<state>` | POST | Records the intended state and publishes the corresponding MQTT command |
| `/add-sensor?name=<name>&value=<value>` | POST | Alternate HTTP path for a reading (the real ESP32 firmware uses MQTT instead) |

## Building

### Dependencies (Debian/Raspberry Pi OS package names)

- `libsqlitecpp-dev`
- `libcpp-httplib-dev` (built with OpenSSL support)
- `libpaho-mqttpp-dev` (pulls in `libpaho-mqtt-dev`)
- `libssl-dev`
- `pkg-config`, `cmake`, a C++17 compiler

### Native (on the RPi itself)

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

### Cross-compiling (from an x86_64 dev machine targeting an aarch64 RPi)

Only works when the host and target run the *same* Debian release, so the target's `:arm64` packages are ABI-compatible drop-ins (see `cmake/toolchain-aarch64.cmake`):

```bash
sudo dpkg --add-architecture arm64 && sudo apt update
sudo apt install -y crossbuild-essential-arm64 pkg-config \
    libsqlitecpp-dev:arm64 libcpp-httplib-dev:arm64 \
    libpaho-mqttpp-dev:arm64 libpaho-mqtt-dev:arm64 libssl-dev:arm64

cmake -S . -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
cmake --build build-aarch64 -j"$(nproc)"
```

## Deploying

`scripts/build-and-deploy.sh` does the full cycle: cross-compile locally, ship the binary to the RPi over `scp`, and restart the systemd service. Edit `RPI_HOST`/`RPI_BUILD_DIR` at the top of the script to match your setup, then:

```bash
./scripts/build-and-deploy.sh
```

The binary is uploaded to a temp name and `mv`'d into place, since overwriting a running executable's file in-place fails with `ETXTBSY`.

On the RPi, install the service once:

```bash
sudo cp systemd/rpi-platform.service /etc/systemd/system/
sudo systemctl enable --now rpi-platform.service
```

## Runtime layout

The binary expects to run from a `build/` (or `build-aarch64/`) directory with these as siblings of the project root:

```
rpi-platform/
├── build/rpi-platform       <- the binary, run from here (relative paths below are from here)
├── config/server.crt        <- TLS certificate (not in git - generate your own)
├── config/server.key        <- TLS private key (not in git)
├── data/rpi-platform.db     <- SQLite database (not in git, created on first run)
└── web/*.html               <- served as-is from disk on every request
```
