# M5Unified Stack-chan VBAN Receiver

[English](README.md)

M5Unified と m5stack-avatar を使った Stack-chan 向けの VBAN audio receiver です。
VBAN の PCM 音声を UDP で受信し、スピーカー再生、Avatar の口パク、サーボ、GPIO9 接続の Necomimi LED 表示を行います。

## 機能

- VBAN UDP 音声受信
- 受信音声に合わせた Avatar の口パク
- GPIO9 接続の Necomimi LED レベルメーター、5段階表示
- Stack-chan 設定に基づくサーボ制御
- VBAN 拡張設定による固定IP指定
- CoreS3 をデフォルト環境として対応
- AtomS3 + Atomic Echo Base 環境に対応

## ハードウェア

デフォルト環境:

- M5Stack CoreS3
- Stack-chan サーボ構成
- GPIO9 に接続した Necomimi LED

Atomic Echo Base 環境:

- M5Stack AtomS3
- M5Atomic Echo Base

## ビルド

```sh
/Users/mongonta555/.platformio/penv/bin/pio run
```

PlatformIO のデフォルト環境は `m5stack-cores3` です。

利用可能な環境:

- `m5stack-core2`
- `m5stack-cores3`
- `m5stack-atoms3-echobase`

例:

```sh
/Users/mongonta555/.platformio/penv/bin/pio run -e m5stack-atoms3-echobase
```

## 設定ファイル

CoreS3 では SD カードから設定を読み込みます。リポジトリの `data/` ディレクトリは、配置先ファイルシステムの構成に対応しています。

必要なファイル:

- `/yaml/SC_BasicConfig.yaml`
- `/yaml/SC_SecConfig.yaml`
- `/SC_VBAN/SC_ExtConfig.yaml`

### Wi-Fi

`/yaml/SC_SecConfig.yaml` を編集します。

```yaml
wifi:
  ssid: "YOUR_WIFI_SSID"
  password: "YOUR_WIFI_PASSWORD"
```

### 固定IP

`/SC_VBAN/SC_ExtConfig.yaml` を編集します。

`STATIC_IP.ip` が空の場合は DHCP を使います。`ip` を指定した場合は、Wi-Fi 接続前に `WiFi.config()` で固定IPを適用します。

```yaml
STATIC_IP:
  ip: "192.168.1.50"
  gateway: "192.168.1.1"
  subnet: "255.255.255.0"
  dns1: "192.168.1.1"
  dns2: "8.8.8.8"
```

省略時のデフォルト:

- `gateway`: `ip` と同じサブネットで末尾を `.1` にした値
- `subnet`: `255.255.255.0`
- `dns1`: gateway
- `dns2`: `0.0.0.0`

拡張設定は `stackchan-arduino` の `StackchanExConfig` パターンに合わせ、`StackchanVbanConfig` 経由で読み込みます。

## VBAN 設定

デフォルトの受信設定:

- UDP ポート: `6980`
- ストリーム名: `Stream1`
- 音声フォーマット: PCM 16-bit

ストリーム名はビルド時に `VBAN_STREAM_NAME` で変更できます。

## 口パクとLED

口パクは受信音声の FFT から計算します。

- 更新間隔: `100 ms`
- 反応ゲイン: `2.0`
- Necomimi LED: GPIO9
- LED 種類/順序: SK6812, GRB
- LED 数: 18
- LED レベル: 5段階

LED は口パクと同じリップシンクレベルを使うため、口とLEDが連動して反応します。

## メモ

- `SC_SecConfig.yaml` は Wi-Fi 認証情報などの個人情報用です。
- VBAN 固有の設定は `/SC_VBAN/SC_ExtConfig.yaml` に記述します。
- `DynamicJsonDocument` に関するビルド警告は、現在利用している `stackchan-arduino` API 由来の想定内の警告です。
