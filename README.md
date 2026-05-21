# Stackchan_VBAN

VBANで送られてきた音声をWi-Fi経由で受信し、M5Stack系デバイスのスピーカーから再生するｽﾀｯｸﾁｬﾝです。

Avatar表示、リップシンク、サーボ制御には以下のライブラリを使用しています。

- [M5Unified](https://github.com/m5stack/M5Unified)
- [m5stack-avatar](https://github.com/meganetaaan/m5stack-avatar)
- [stackchan-arduino](https://github.com/stack-chan/stackchan-arduino)

## 対応環境

PlatformIOでビルドします。

- `m5stack-cores3`
- `m5stack-core2`
- `m5stack-atoms3-echobase`

デフォルト環境は `m5stack-cores3` です。Atomic Echo Baseを使う場合は `m5stack-atoms3-echobase` を指定してください。

## VBAN受信仕様

- UDP port: `6980`
- Stream name: `Stream1`
- PCM 16bit integer
- mono入力は左右同一に展開
- stereo以上の入力は先頭2chをmono mixして左右同一で再生
- 受信ジッタ対策、欠落補間、長時間再生時のドリフト補正あり

Stream名を変えたい場合は `platformio.ini` の `build_flags` に例のように追加してください。

```ini
build_flags =
    ${env.build_flags}
    -DVBAN_STREAM_NAME=\"MyStream\"
```

## Wi-Fi設定

`M5Unified_StackChan/data/yaml/SC_SecConfig.yaml` を参考に、SDカードまたはSPIFFS上の設定ファイルへWi-Fi情報を入れてください。

```yaml
wifi:
  ssid: "YOUR_WIFI_SSID"
  password: "YOUR_WIFI_PASSWORD"
```

起動するとシリアルログに受信先IPアドレスが表示されます。

```text
VBAN listening address: 10.x.x.x:6980 stream:Stream1
```

## 音量とサーボ設定

`M5Unified_StackChan/data/yaml/SC_BasicConfig.yaml` を参考に設定します。

- 起動時の音量: `bluetooth.start_volume`
- サーボ種別: `servo_type`
- サーボの初期角度、オフセット、可動範囲: `servo`

Avatarを使わない音声専用ビルドにしたい場合は、`build_flags` に以下を追加します。

```ini
-DVBAN_AUDIO_ONLY=1
```

## ビルド

リポジトリ直下から実行します。

```sh
cd M5Unified_StackChan
pio run
```

Atomic Echo Base向け:

```sh
pio run -e m5stack-atoms3-echobase
```

書き込み:

```sh
pio run -t upload
```

## 送信側の設定例

VBAN送信側、たとえば Voicemeeter などで以下のように設定します。

- IP address: 起動ログに表示されたM5StackのIP
- Port: `6980`
- Stream name: `Stream1`
- Format: PCM 16bit
- Sample rate: 48kHz推奨

ネットワークが不安定な場合は、1パケットあたりのサンプル数を増やす設定、またはVBAN送信側の品質/負荷設定を安定寄りにしてください。

## ログの見方

5秒ごとに受信状態を出力します。

- `ok`: 有効なVBAN音声パケット数
- `drop`: 破棄したパケット数
- `gap`: VBANフレーム番号の欠落
- `plc`: 欠落補間で挿入したパケット数
- `ooo`: 重複または古い順序のパケット数
- `resync`: 大きなフレームジャンプによる再同期回数
- `driftDrop`: 遅延補正のために破棄したパケット数
- `queued`: 再生待ちキューの現在値
- `bufMs`: 再生待ちキューの目安遅延
- `underrun`: 再生側で音声が足りなかった回数
- `pps`: 直近の受信パケット/秒
- `audioHz`: 直近の受信音声レート推定値

長時間再生でズレる場合は、`queued` と `bufMs` が増え続けていないか、または `underrun` が増え続けていないかを確認してください。

## ディレクトリ

```text
M5Unified_StackChan/
  src/
    main.cpp
    AudioOutputM5Speaker.h
  data/yaml/
    SC_BasicConfig.yaml
    SC_SecConfig.yaml
  platformio.ini
```
