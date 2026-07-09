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

## BOM(部品表)

SMDのIC・受動部品は [JLCPCB](https://jlcpcb.com/) の実装サービス(下記LCSC品番)でまとめて発注するのが簡単です。
M5Stamp S3・コネクタ・ヘッダ類は国内の[スイッチサイエンス](https://www.switch-science.com/)・[秋月電子](https://akizukidenshi.com/)でも入手できます。

| 部品 | リファレンス | 個数 | 型番 / 仕様 | 入手先 |
|---|---|---|---|---|
| M5Stamp S3 | U1 | 1 | ESP32-S3 モジュール | [スイッチサイエンス](https://www.switch-science.com/search?q=M5Stamp+S3) / [秋月](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=M5Stamp) |
| FC 60ピンスロットコネクタ | J1 | 1 | 2.54mmピッチ・ファミコン用 | [スイッチサイエンス検索](https://www.switch-science.com/search?q=%E3%82%AB%E3%83%BC%E3%83%89%E3%82%A8%E3%83%83%E3%82%B8) ※互換品(Amazon/AliExpress等)も可 |
| 74HCT595(SOIC-16) | U2–U5 | 4 | シフトレジスタ / LCSC C70468 | [秋月(74HC595)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=74HC595) |
| 74LVC245(SOIC-20) | U6, U7 | 2 | バスバッファ / LCSC C571201 | [秋月(74LVC245)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=74LVC245) |
| 74HCT541(SOIC-20) | U8 | 1 | バスバッファ / LCSC C1548616 | [秋月(74HC541)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=74HC541) |
| 積層セラコン 100nF 0603 | C1–C8 | 8 | パスコン / LCSC C1591 | [秋月(チップコンデンサ)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=0.1uF+1608) |
| 積層セラコン 10µF 0603 | C9 | 1 | バルク / LCSC C19702 | [秋月(チップコンデンサ)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=10uF+1608) |
| チップ抵抗 10kΩ 0603 | R1 | 1 | LCSC C25804 | [秋月(チップ抵抗)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=10k+1608) |
| チップ抵抗 20kΩ 0603 | R2 | 1 | LCSC C4184 | [秋月(チップ抵抗)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=20k+1608) |
| ポリヒューズ 500mA | F1 | 1 | mSMD050 相当 | [秋月(ポリスイッチ)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=%E3%83%9D%E3%83%AA%E3%82%B9%E3%82%A4%E3%83%83%E3%83%81) |
| 1.27mmメスピンヘッダ | — | 2 | 左17ピン+右11ピン(Stampソケット用) | [スイッチサイエンス](https://www.switch-science.com/search?q=1.27mm+%E3%83%98%E3%83%83%E3%83%80) / [秋月](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=1.27mm) |
| M3ネジ・スペーサ | MH1–4 | 各4 | 3Dプリンタ筐体固定用 | [秋月(M3)](https://akizukidenshi.com/catalog/goods/search.aspx?keyword=M3+%E3%82%B9%E3%83%9A%E3%83%BC%E3%82%B5) |

> リンクは検索結果ページです(在庫・品番は変動するため)。完全なSMD部品表は [`hardware/nes-rom-reader-BOM.csv`](hardware/nes-rom-reader-BOM.csv) を参照。

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

## クレジット

このプロジェクトは **[Claude Code](https://claude.com/claude-code)(Fable 5)** によって、
ファームウェア開発と **EasyEDA Pro** での基板設計(回路図・PCB・配線・製造データ生成)が行われました。

Claude Code と EasyEDA Pro の連携には、WebSocketブリッジ拡張機能
**[eext-run-api-gateway](https://github.com/easyeda/eext-run-api-gateway)** を使用しています。
これにより、AIが EasyEDA Pro を直接操作して基板を設計できます。

---

Powered by Claude Code (Fable 5) × EasyEDA Pro
