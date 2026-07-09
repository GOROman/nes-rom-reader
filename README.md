# nes-rom-reader

ファミコン(FC 60ピン)カセット吸い出しツール。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `firmware/` | M5Stamp S3 (ESP32-S3) ファームウェア(PlatformIO / Arduino) |
| `host/` | PC側 Python CLI(USB CDCシリアル経由で吸い出し、.nes保存) |
| `hardware/` | EasyEDA Pro 基板設計メモ・BOM |
| `docs/` | 設計ドキュメント(ピンマップ、バス設計) |

## アーキテクチャ

- **MCU**: M5Stamp S3 を吸い出し基板に直接実装
- **カートリッジI/F**: 60ピン(2.54mmピッチ)エッジコネクタ
- **バス駆動**: アドレス線は 74HCT595 シフトレジスタチェーン(5V駆動)、
  データ読み出しは 74LVC245 ×2(PRG/CHR、5V→3.3Vレベル変換)
- **対応マッパー**: まず Mapper 0 (NROM)。以後 CNROM / UNROM / MMC1 と段階拡張予定
- **ホスト**: Python CLI (`host/famidump.py`)。iNESヘッダ付き .nes を出力

## 使い方(予定)

```sh
# ファームウェア書き込み
cd firmware && pio run -t upload

# 吸い出し(NROM: PRG 32KB + CHR 8KB)
python host/famidump.py --port /dev/tty.usbmodem* --prg 32 --chr 8 -o game.nes
```

詳細は [docs/hardware-design.md](docs/hardware-design.md) を参照。
