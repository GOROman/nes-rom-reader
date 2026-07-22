# nes-rom-reader

ファミコン(FC 60ピン)カセット吸い出しツール。M5Stamp S3 + 専用基板(EasyEDA Pro設計)+ Python CLI / Web UI。

![実機で吸い出し中](docs/images/hero-dumping.jpeg)

> 実機で本物のカセット(西遊記スーパーモンキー大冒険 / VAP 1986)を吸い出し、
> Web UI の「Super Monkey Viewer」でCHR-ROMのタイルを表示している様子。

![配線済みPCB](docs/images/pcb-routed.png)

## 構成

| ディレクトリ | 内容 |
|---|---|
| `firmware/` | M5Stamp S3 (ESP32-S3) ファームウェア(PlatformIO / Arduino) |
| `host/` | PC側 Python CLI(吸い出し `famidump.py` / 出荷テスト `factory_test.py`) |
| `web/` | ブラウザ用 Web UI(WebSerial。吸い出し + 出荷テスト。インストール不要) |
| `hardware/` | EasyEDA Pro プロジェクト・ガーバー・BOM・実装座標 |
| `docs/` | 設計ドキュメント(ピンマップ、バス設計、60ピン配列) |

## アーキテクチャ

- **MCU**: M5Stamp S3 を **1.27mmピッチのソケット実装**(メスヘッダで着脱可)
- **カートリッジI/F**: FC 60ピン(2.54mmピッチ)エッジコネクタ、パッド直接実装
- **バス駆動**:
  - アドレス線 … 74HCT595 ×4 シフトレジスタチェーン(5V駆動)
  - データ読み出し … 74LVC245 ×2(PRG/CHR、5V→3.3Vレベル変換)
  - 制御線 … 74HCT541 バッファ(3.3V→5V)
- **対応マッパー**: まず Mapper 0 (NROM)。以後 CNROM / UNROM / MMC1 と段階拡張予定
- **ホスト**: Python CLI (`host/famidump.py`)。CRC32照合付きでiNESヘッダ付き .nes を出力

## 基板

- 2層、両面GNDベタ、約84×77mm
- 下辺中央にUSB-Cノッチ、四隅にM3取付穴(3Dプリンタ筐体対応)
- 製造データ: [`hardware/gerber/nes-rom-reader-gerber.zip`](hardware/gerber/) をそのままJLCPCB等へ

## 使い方

```sh
# ファームウェア書き込み
cd firmware && pio run -t upload

# 吸い出し(NROM: PRG 32KB + CHR 8KB)
python host/famidump.py --port /dev/tty.usbmodem* --prg 32 --chr 8 -o game.nes
```

## Web UI(ブラウザから吸い出し)

![Web UI](docs/images/webui.png)

`web/index.html` を **Chrome / Edge**(デスクトップ)で開くだけ。WebSerialでM5Stamp S3に接続し、
GUIで吸い出し・.nesダウンロード・出荷テストができます(Python不要)。ファミコン配色 & ドットフォント、
**日本語 / English 切替**対応。

### 🐵 Super Monkey Viewer(CHR-ROMビューア)

![CHR Viewer](docs/images/chr-viewer.png)

吸い出したCHR-ROM(2bpp平面フォーマット)を8x8タイルにデコード表示。NES 64色マスターパレットから
4色を自由に変更でき、プリセット(Grayscale/Game Boy/Mario など)も切替可能。`.nes` / `.chr` ファイルの
読み込みにも対応(ハード不要でオフライン閲覧可)。

```sh
# ローカルで開く(file:// でも動くが、WebSerialは https か localhost 推奨)
cd web && python3 -m http.server 8000   # → http://localhost:8000
```

## 出荷テスト(全数検査)

組み立て後の基板が正しく動くかを、**基準カセット(既知CRCのカセット)** の吸い出しCRC照合で判定します。

```sh
# 1) 既知良品の基板で基準CRCを学習(基準カセットを挿して)
python host/factory_test.py --port /dev/cu.usbmodem* --learn --name "MyRefCart"

# 2) 各基板を検査(同じ基準カセットを挿して)。終了コード 0=PASS / 1=FAIL
python host/factory_test.py --port /dev/cu.usbmodem* --test --name "MyRefCart"

# カセット不要のセルフテストのみ(GPIO/シフトレジスタの健全性)
python host/factory_test.py --port /dev/cu.usbmodem* --selftest
```

ファームの `T` コマンド(セルフテスト)は、出力ピンの読み戻し・シフトレジスタ送出・
データバスのフロート読みを行い、最後に `SELFTEST PASS/FAIL` を返します。
Web UIの「出荷テスト」タブからも同じ検査が実行できます。

## ドキュメント

- 設計方針・ピンマップ・60ピン配列: [docs/hardware-design.md](docs/hardware-design.md)
- 基板データ詳細: [hardware/README.md](hardware/README.md)

## クレジット

このプロジェクトは **[Claude Code](https://claude.com/claude-code)(Fable 5)** によって、
ファームウェア開発と **EasyEDA Pro** での基板設計(回路図・PCB・配線・製造データ生成)が行われました。

Claude Code と EasyEDA Pro の連携には、WebSocketブリッジ拡張機能
**[eext-run-api-gateway](https://github.com/easyeda/eext-run-api-gateway)** を使用しています。
これにより、AIが EasyEDA Pro を直接操作して基板を設計できます。

---

Powered by Claude Code (Fable 5) × EasyEDA Pro
