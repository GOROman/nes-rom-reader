# nes-rom-reader

ファミコン(FC 60ピン)カセット吸い出しツール。M5Stamp S3 + 専用基板(EasyEDA Pro設計)+ Python CLI。

![配線済みPCB](docs/images/pcb-routed.png)

## 構成

| ディレクトリ | 内容 |
|---|---|
| `firmware/` | M5Stamp S3 (ESP32-S3) ファームウェア(PlatformIO / Arduino) |
| `host/` | PC側 Python CLI(USB CDCシリアル経由で吸い出し、.nes保存) |
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

## ドキュメント

- 設計方針・ピンマップ・60ピン配列: [docs/hardware-design.md](docs/hardware-design.md)
- 基板データ詳細: [hardware/README.md](hardware/README.md)

---

Powered by Claude Code (Fable 5)
