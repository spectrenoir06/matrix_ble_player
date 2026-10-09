# Matrix_ble_player

ESP32 firmware for HUB75 LED matrices: plays GIF / PNG / Lua files from the
SD card (or flash), driven by the Spectre Matrix app over Bluetooth, USB or
WiFi (Spectre Protocol, `../../spectre_protocol`).

One firmware for every board: the panel layout (`src/Layout.cpp`) is a
setting, chosen in the app (Settings → Display → Panels).

## Build

The two repos side by side, as `platformio.ini` links the protocol:

```
dev/elec/spectre_protocol
dev/elec/platformIO/Matrix_ble_player
```

```bash
pio run -e matrix              # build
pio run -e matrix -t upload    # flash over USB (or update from the app)
```

## Versions and releases

The firmware's version is its git tag (`git describe --tags`): `v2.1.0` on
the tag, `v2.1.0-3-gabc1234` after it, `-dirty` with uncommitted changes.

CI (`.github/workflows/main.yml`) builds every push (the files are in the
run's artifacts). A `v*` tag also makes a GitHub Release; spectre-server picks
up the latest one for the app's install screen (`v2.1.0-rc1`: a prerelease,
not picked up).

```bash
scripts/release.sh v2.1.0
```

checks that both repos are committed and pushed, pins the spectre_protocol
commit in `spectre_protocol.ref` (what CI builds with; local builds warn when
it is out of date), tags and pushes.

spectre_protocol is private: CI reads it with a deploy key (public key in
spectre_protocol's Deploy keys, private key in this repo's
`SPECTRE_PROTOCOL_KEY` secret).
