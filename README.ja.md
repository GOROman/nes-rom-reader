# nes-rom-reader

[English](README.md) | **日本語** | [简体中文](README.zh.md)

**ファミコン(FC 60ピン)カセット吸い出しツール** — オープンハードウェアの専用基板 + M5Stamp S3 ファームウェア + ブラウザ用 Web UI。
カセットを `.nes` に吸い出し、CHRのグラフィックを閲覧・編集するまで、すべてChromeから行えます。

![実機で吸い出し中](docs/images/hero-dumping.jpeg)

> ✅ **実機で動作確認済み** — 実カセット(元祖西遊記 スーパーモンキー大冒険 / VAP 1986)を、
> 安定・再現性のあるCRCで吸い出し成功。

### ▶ インストール不要、今すぐ試す: **https://goroman.github.io/nes-rom-reader/web/**
Chrome / Edge(デスクトップ or Android)で開き、M5Stamp S3 を接続するだけ。

---

## 特徴

- **PRG / CHR を吸い出し**、標準の `.nes` ファイルとして保存(iNESヘッダ・ミラーリング自動判定)
- **全転送でCRC32照合** — 化けたデータが黙って通ることがない
- **Web UI**(WebSerial)— Python不要・ドライバ不要。ファミコン配色、日英切替、Android対応
- **🐵 Super Monkey Viewer** — CHRを8×8タイルに展開、パレット変更、**ドット絵編集**、
  **カセット内蔵フォントでのテキスト書き込み**
- **出荷テスト** — 基準カセットのCRC照合による全数検査
- **ステータスLED** — 緑=待機 / 青=動作中 / 赤点滅=エラー
- **オープンハードウェア** — EasyEDAプロジェクト・ガーバー・BOM・実装座標を同梱

## リポジトリ構成

| ディレクトリ | 内容 |
|---|---|
| `firmware/` | M5Stamp S3 (ESP32-S3) ファームウェア — PlatformIO / Arduino |
| `host/` | Python CLI: `famidump.py`(吸い出し)、`factory_test.py`(出荷テスト) |
| `web/` | ブラウザ用 Web UI(WebSerial)。上記URLで公開中 |
| `hardware/` | EasyEDA Pro プロジェクト・ガーバー・BOM・実装座標 |
| `docs/` | 設計ドキュメント — ピンマップ、バス設計、60ピン配列 |

## ハードウェア

![配線済みPCB](docs/images/pcb-routed.png)

- **MCU**: M5Stamp S3 を **1.27mmピッチのソケット**で実装(着脱可)
- **カートリッジI/F**: FC 60ピン(2.54mm)エッジコネクタ — パッドを基板に直接配置
- **アドレス**: 74HCT595 ×4 シフトレジスタチェーン(5V)
- **データ**: 74LVC245 ×2(PRG/CHR、5V→3.3Vレベル変換)
- **制御**: 74HCT541 バッファ(3.3V→5V)
- 2層、両面GNDベタ、約84×77mm、USB-Cノッチ、M3取付穴×4

自分で製造する場合は [`hardware/gerber/nes-rom-reader-gerber.zip`](hardware/gerber/) をJLCPCB等にアップロード。
詳細は [hardware/README.md](hardware/README.md) と [docs/hardware-design.md](docs/hardware-design.md)。

## 対応カセットと制限

- ✅ **mapper 0 (NROM)** — 完全対応(PRG 32KB + CHR 8KB)、実機確認済み
- ⚠️ **バンク切替式マッパー(CNROM / UNROM / MMC1 …)は未対応**

**理由**: バンク切替はマッパーレジスタへの*書き込み*が必要ですが、データバッファ(74LVC245)は
**方向固定の読み出し専用**(カートリッジ→ESP32)です。書き込みができないためバンクを選べません。

> 例 — **トランスフォーマー コンボイの謎**(CNROM / mapper 3): PRG 32KBは正常に吸えますが、
> CHRは電源投入時の1バンク(8KB)しか読めません。

対応には **双方向(書き込み対応)バッファを備えた v0.2 基板** とバンク切替ファームウェアが必要です。

## はじめかた

### 1. ファームウェア書き込み

```sh
cd firmware && pio run -t upload
```

### 2a. ブラウザから吸い出す(推奨)

**https://goroman.github.io/nes-rom-reader/web/** を開く → **接続** → PRG/CHRサイズ指定 → **吸い出して .nes 保存**

### 2b. CLIから吸い出す

```sh
pip install -r host/requirements.txt
python host/famidump.py --port /dev/tty.usbmodem* --prg 32 --chr 8 -o game.nes
```

## Web UI

![Web UI](docs/images/webui.png)

ローカルで動かす場合(WebSerialは `https` か `localhost` が必要):

```sh
cd web && python3 -m http.server 8000   # → http://localhost:8000
```

### 🐵 Super Monkey Viewer — CHRツール

![CHR Viewer](docs/images/chr-viewer.png)

- **タイルビューア** — CHR(2bpp平面)を8×8タイルに展開。拡大率・1行タイル数を調整可
- **パレット** — プリセット(Grayscale / Game Boy / Mario …)、またはNES 64色マスターパレットから4色を個別選択
- **ドット絵エディタ** — **タイルをダブルクリック**で8×8ドットエディタを起動(マウス/タッチ対応)→ 適用
- **タイル編集** — タイルを選んで消去、または別タイルを貼り付け
- **カセットのフォントでテキスト書き込み** — CHR内のメッセージは字形タイルを並べただけなので、
  フォントの開始タイルと字形の並び順を指定し、本文を入力すると該当の字形タイルを書き込みます
- **書き出し** — 編集した `.chr` または再生成した `.nes` をダウンロード
- **オフラインでも利用可** — ハード無しで任意の `.nes` / `.chr` を読み込んで閲覧・編集

## 出荷テスト(全数検査)

**基準カセット**の吸い出しCRCを照合して、組み立て済み基板の良否を判定します。

```sh
# 1) 既知良品の基板で基準CRCを学習
python host/factory_test.py --port /dev/cu.usbmodem* --learn --name "MyRefCart"

# 2) 各基板を検査 — 終了コード 0=PASS / 1=FAIL
python host/factory_test.py --port /dev/cu.usbmodem* --test --name "MyRefCart"

# セルフテストのみ(カセット不要)
python host/factory_test.py --port /dev/cu.usbmodem* --selftest
```

Web UIの**出荷テスト**パネルからも同じ流れを実行できます。各ステップを約1秒ずつ進めるので、
ステータスLEDを目で追えます。セルフテストは全出力ピンの読み戻しとシフトレジスタ動作を確認し、
`SELFTEST PASS/FAIL` を返します。

## シリアルプロトコル

USB CDC、115200 baud。1行コマンド:

| コマンド | 応答 |
|---|---|
| `V` | `famidump v0.2` |
| `R <addr_hex> <len_hex>` | `OK <len>` + 生データ + `CRC xxxxxxxx`(PRG) |
| `C <addr_hex> <len_hex>` | 同上(CHR) |
| `M` | `H` / `V` / `?` — ミラーリング |
| `T` | ペーシング付きセルフテスト、最後に `SELFTEST PASS/FAIL` |
| `S` | `STATUS famidump-v0.2 mirror=? pins=PASS md=0x20` |

CRCは CRC32(IEEE 802.3 / `zlib.crc32` 互換)、8桁の大文字16進です。

## リンク

- リポジトリ: https://github.com/GOROman/nes-rom-reader
- Web UI(公開版): https://goroman.github.io/nes-rom-reader/web/
- 作者 X: [@GOROman](https://x.com/GOROman)

## クレジット

ファームウェア開発と **EasyEDA Pro** での基板設計(回路図・PCB・配線・製造データ生成)は
**[Claude Code](https://claude.com/claude-code)(Fable 5)** が実施しました。WebSocketブリッジ拡張
**[eext-run-api-gateway](https://github.com/easyeda/eext-run-api-gateway)** 経由で EasyEDA Pro を直接操作しています。

---

Powered by Claude Code (Fable 5) × EasyEDA Pro
