# M5Unified Stack-chan VBAN Receiver

[日本語](README_ja.md)

This is a VBAN audio receiver for Stack-chan using M5Unified and m5stack-avatar.
It receives VBAN PCM audio over UDP and drives speaker playback, Avatar lip sync, servo motion, and a Necomimi LED connected to GPIO9.

## Features

- VBAN UDP audio receive
- Avatar lip sync from received audio
- Necomimi LED level meter on GPIO9, 5 levels
- Servo gaze motion with Stack-chan configuration
- Optional static IP address from VBAN extension config
- CoreS3 default environment
- AtomS3 + Atomic Echo Base environment

## Hardware

Default environment:

- M5Stack CoreS3
- Stack-chan servo setup
- Necomimi LED connected to GPIO9

Atomic Echo Base environment:

- M5Stack AtomS3
- M5Atomic Echo Base

## Build

```sh
/Users/mongonta555/.platformio/penv/bin/pio run
```

Default PlatformIO environment is `m5stack-cores3`.

Other available environments:

- `m5stack-core2`
- `m5stack-cores3`
- `m5stack-atoms3-echobase`

Example:

```sh
/Users/mongonta555/.platformio/penv/bin/pio run -e m5stack-atoms3-echobase
```

## Configuration Files

For CoreS3, configuration is read from the SD card. The repository `data/` directory mirrors the expected filesystem layout.

Required files:

- `/yaml/SC_BasicConfig.yaml`
- `/yaml/SC_SecConfig.yaml`
- `/SC_VBAN/SC_ExtConfig.yaml`

### Wi-Fi

Edit `/yaml/SC_SecConfig.yaml`.

```yaml
wifi:
  ssid: "YOUR_WIFI_SSID"
  password: "YOUR_WIFI_PASSWORD"
```

### Static IP

Edit `/SC_VBAN/SC_ExtConfig.yaml`.

If `STATIC_IP.ip` is empty, DHCP is used. If `ip` is set, `WiFi.config()` is applied before connecting.

```yaml
STATIC_IP:
  ip: "192.168.1.50"
  gateway: "192.168.1.1"
  subnet: "255.255.255.0"
  dns1: "192.168.1.1"
  dns2: "8.8.8.8"
```

Defaults when omitted:

- `gateway`: same subnet as `ip`, with last octet `.1`
- `subnet`: `255.255.255.0`
- `dns1`: gateway
- `dns2`: `0.0.0.0`

The extension config is loaded through `StackchanVbanConfig`, following the `StackchanExConfig` pattern from `stackchan-arduino`.

## VBAN Settings

Default receiver settings:

- UDP port: `6980`
- Stream name: `Stream1`
- Audio format: PCM 16-bit

The stream name can be changed at build time with `VBAN_STREAM_NAME`.

## Lip Sync And LED

Lip sync is calculated from the received audio FFT.

- Update interval: `100 ms`
- Response gain: `2.0`
- Necomimi LED: GPIO9
- LED type/order: SK6812, GRB
- LED count: 18
- LED levels: 5

The LED uses the same lip sync level as the mouth animation, so the two react together.

## Notes

- `SC_SecConfig.yaml` is for personal information such as Wi-Fi credentials.
- VBAN-specific options should go in `/SC_VBAN/SC_ExtConfig.yaml`.
- Build warnings about `DynamicJsonDocument` come from the current `stackchan-arduino` API and are expected.
