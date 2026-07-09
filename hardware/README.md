# hardware — 基板データ

ファミコン(FC 60ピン)カセット吸い出し基板の設計・製造データ。EasyEDA Pro で設計。

## ファイル

| ファイル | 内容 |
|---|---|
| `nes-rom-reader.eprj2` | EasyEDA Pro プロジェクト本体(SQLite形式。回路図+PCB) |
| `gerber/nes-rom-reader-gerber.zip` | 製造用ガーバー一式(全レイヤー+ドリル、2層) |
| `nes-rom-reader-BOM.csv` | 部品表 |
| `nes-rom-reader-PickAndPlace.csv` | 実装座標(ピック&プレース) |

## 基板仕様

- **層数**: 2層、両面GNDベタ
- **外形**: 約84×77mm、下辺中央にUSB-Cノッチ、四隅にM3取付穴
- **主要部品**:
  - M5Stamp S3 … **1.27mmピッチのソケット実装**(メスヘッダで着脱可)
  - 74HCT595 ×4(U2-U5、アドレス生成)
  - 74LVC245 ×2(U6/U7、データバス 5V↔3.3V)
  - 74HCT541 ×1(U8、制御線バッファ)
  - FC 60ピンエッジコネクタ(J1、2.54mmピッチ、パッド直接実装)
  - ポリヒューズ F1(500mA)、CIRAM A10分圧 R1/R2(10k/20k)、パスコン C1-C9

詳細な設計方針・ピンマップは [../docs/hardware-design.md](../docs/hardware-design.md) を参照。

## 製造

`gerber/nes-rom-reader-gerber.zip` をそのままJLCPCB等にアップロードして発注可能。

## 注意

- **1.27mmピッチのメスヘッダ**が必要(M5Stamp S3のネイティブピッチ)
- 電源OFFでカセットを挿抜すること(活線挿抜非対応)
- EasyEDA上でPCBは回路図から直接編集した箇所があり、ネットリストは意図的に不一致。
  **「Import Changes」を実行するとPCBの編集が巻き戻る**ため、改版時は注意。
