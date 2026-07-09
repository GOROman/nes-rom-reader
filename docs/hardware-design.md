# ハードウェア設計(EasyEDA Pro)

## 全体方針(確定)

ファミコンカートリッジのバスは約50信号あり、ESP32-S3のGPIOでは直結できない。
そこで:

- **アドレス出力**(CPU A0–A14、PPU A0–A13、PPU /A13): 74HCT595 シフトレジスタチェーン
  (U2→U3→U4→U5 の4個 = 32bit、5V電源)
  - ESP32からの SER/SRCLK/RCLK は **74HCT541 バッファ経由**(HCT入力は3.3Vを受けられる)
  - チェーン接続: U2.QH' → U3.SER、U3.QH' → U4.SER、U4.QH' → U5.SER
- **データ入力**(CPU D0–D7、PPU D0–D7): 74LVC245 ×2(U6/U7)
  - 3.3V電源・5Vトレラント入力。DIR固定で B側(5V, カートリッジ)→A側(3.3V, ESP32)方向
  - U6: B側 = CPU D0–D7 (CD0–7)、U7: B側 = PPU D0–D7 (PD0–7)
  - A側は共通8bitバス **MD0–MD7** としてESP32 G4–G11 に接続(G4=MD0 … G11=MD7)
  - /OE を個別制御(U6=G15 OE_PRG_N、U7=G39 OE_CHR_N)。**同時イネーブル禁止**
- **制御線**(/ROMSEL, R/W, M2, PPU /RD, PPU /WR)と SR_DATA/SR_CLK/SR_LATCH:
  ESP32 から **74HCT541 バッファ**を通して5Vロジックでカートリッジ/595へ供給
- **CIRAM A10**(カート→ESP32): 10kΩ/20kΩ 抵抗分圧で 5V→3.3V に落とし G46(入力専用)へ。
  ミラーリング判定用
- **未接続ピン**: ピン45/46(拡張音声)、ピン15(/IRQ)、ピン48(CIRAM /CE)は結線しない

## カートリッジ電源

- カートリッジには **5V** を供給(USB VBUSから。ポリヒューズ+パワースイッチIC推奨)
- 活線挿抜は非対応。**電源OFFで挿抜**する運用(基板に注意書きシルク)

## M5Stamp S3 ピンアサイン(確定)

| 信号 | GPIO | 方向 | 備考 |
|---|---|---|---|
| MD0–MD7(共通データバス) | G4–G11 | 入力 | LVC245 A側。全て GPIO<32 なので1レジスタ読みで8bit取得可 |
| SR_DATA | G12 | 出力 | 541経由 → U2.SER |
| SR_CLK | G13 | 出力 | 541経由 → 595 SRCLK(4個共通) |
| SR_LATCH | G14 | 出力 | 541経由 → 595 RCLK(4個共通) |
| OE_PRG_N | G15 | 出力 | U6 (LVC245, CPUデータ) /OE 負論理 |
| OE_CHR_N | G39 | 出力 | U7 (LVC245, PPUデータ) /OE 負論理 |
| /ROMSEL | G41 | 出力 | 541経由。PRG-ROM /CE 相当 |
| M2 | G42 | 出力 | 541経由。常時Highで可(NROM)。マッパー拡張時にストローブ |
| CPU R/W | G1 | 出力 | 541経由。読み出し時 High |
| PPU /RD | G2 | 出力 | 541経由。CHR読み出しストローブ |
| PPU /WR | G3 | 出力 | 541経由。CHR-RAM書き込み用(将来) |
| CIRAM A10 | G46 | 入力 | 10k/20k分圧(5V→3.3V)。ミラーリング判定用 |

G0(BOOTストラップ)、G43/G44(UART)は空けておく。

## シフトレジスタチェーンのビット割り当て(確定)

U2→U3→U4→U5 の直列(QH'→次段SER)。ESP32 は **MSBファースト(bit31を最初)** に
32bit送出するため、32クロック後に bit0 が U2.QA、bit31 が U5.QH に格納される。

| bit | 595出力 | 信号(ネット名) |
|---|---|---|
| 0–7 | U2 QA–QH | CPU A0–A7 (CA0–CA7) |
| 8–14 | U3 QA–QG | CPU A8–A14 (CA8–CA14) |
| 15 | U3 QH | 予備 |
| 16–23 | U4 QA–QH | PPU A0–A7 (PA0–PA7) |
| 24–29 | U5 QA–QF | PPU A8–A13 (PA8–PA13) |
| 30 | U5 QG | **PPU /A13** (PPU_A13_N、カートピン49) |
| 31 | U5 QH | 予備 |

**bit30 (PPU /A13) は必ず PPU A13 の反転を駆動する。** CHR-ROM読み出しでは
PPU A13=0 なので bit30=1。通常のPRG/CHR読み出しでは常に bit30=1 とする
(ファームウェアの `srCpuAddr()` / `srPpuAddr()` がこれを保証)。

## 読み出しシーケンス(NROM)

**PRG (CPU $8000–$FFFF, 32KB):**
1. R/W=High, M2=High, OE_CHR_N=High
2. アドレス(A0–A14、bit30=1)をシフト出力 → LATCH
3. /ROMSEL=Low → OE_PRG_N=Low → 数百ns待ち → MD0–MD7読み取り
4. /ROMSEL=High に戻す

**CHR (PPU $0000–$1FFF, 8KB):**
1. PPU A13=0(bit30=1)を含むアドレスをシフト出力 → LATCH
2. PPU /RD=Low → OE_CHR_N=Low → 読み取り → 戻す

## BOM(主要部品)

| 部品 | リファレンス | 数 | 用途 |
|---|---|---|---|
| M5Stamp S3 | U1 | 1 | メインMCU(1.27mmピッチのメスヘッダでソケット実装・着脱可) |
| 1.27mmメスピンヘッダ | - | 2 | M5Stamp S3ソケット用(左17ピン+右11ピン) |
| 74HCT595 (SOIC-16) | U2–U5 | 4 | アドレスシフトレジスタ(5V) |
| 74LVC245 (TSSOP-20) | U6, U7 | 2 | データバスレベル変換(3.3V電源・5Vトレラント、DIR固定B→A) |
| 74HCT541 (SOIC-20) | U8 | 1 | 制御線+SR信号の3.3V→5Vバッファ |
| 抵抗 10kΩ + 20kΩ | - | 各1 | CIRAM A10 分圧(5V→3.3V) |
| FC 60ピンエッジコネクタ (2.54mmピッチ) | J1 | 1 | カートリッジスロット(例: Hirose CR22A-60D-2.54DS、KEL 4630-060-038、FC補修用60Pスロット) |
| ポリヒューズ 500mA | F1 | 1 | 5V保護 |
| パスコン 0.1µF ×IC数 + 10µF | - | - | 電源 |

## 60ピンコネクタ ピンアサイン(NesDev Wiki「Cartridge connector」で確認済み)

| ピン | 信号 | ピン | 信号 |
|---|---|---|---|
| 1 | GND | 31 | +5V |
| 2 | CPU A11 | 32 | M2 |
| 3 | CPU A10 | 33 | CPU A12 |
| 4 | CPU A9 | 34 | CPU A13 |
| 5 | CPU A8 | 35 | CPU A14 |
| 6 | CPU A7 | 36 | CPU D7 |
| 7 | CPU A6 | 37 | CPU D6 |
| 8 | CPU A5 | 38 | CPU D5 |
| 9 | CPU A4 | 39 | CPU D4 |
| 10 | CPU A3 | 40 | CPU D3 |
| 11 | CPU A2 | 41 | CPU D2 |
| 12 | CPU A1 | 42 | CPU D1 |
| 13 | CPU A0 | 43 | CPU D0 |
| 14 | CPU R/W | 44 | /ROMSEL |
| 15 | /IRQ **(未接続)** | 45 | 2A03からの音声 **(未接続)** |
| 16 | GND | 46 | RFへの音声 **(未接続)** |
| 17 | PPU /RD | 47 | PPU /WR |
| 18 | CIRAM A10 | 48 | CIRAM /CE **(未接続)** |
| 19 | PPU A6 | 49 | PPU /A13 |
| 20 | PPU A5 | 50 | PPU A7 |
| 21 | PPU A4 | 51 | PPU A8 |
| 22 | PPU A3 | 52 | PPU A9 |
| 23 | PPU A2 | 53 | PPU A10 |
| 24 | PPU A1 | 54 | PPU A11 |
| 25 | PPU A0 | 55 | PPU A12 |
| 26 | PPU D0 | 56 | PPU A13 |
| 27 | PPU D1 | 57 | PPU D7 |
| 28 | PPU D2 | 58 | PPU D6 |
| 29 | PPU D3 | 59 | PPU D5 |
| 30 | +5V | 60 | PPU D4 |

ミラーリングはカセット側で CIRAM A10 ↔ PPU A10/A11 が結線されているため、
CIRAM A10 を読めばH/V判定できる。

## EasyEDA Pro 作業メモ

- プロジェクト名: `nes-rom-reader`
- M5Stamp S3 のフットプリントはLCSC/コミュニティライブラリにあり(C→検索: "M5Stamp S3")
- 60ピンスロットのフットプリントはPCB上に直接パッド生成済み(J1相当、ネット割り当て済み):
  - **ピッチ 2.54mm(100mil)、列間 5.08mm(200mil)**、2列×30ピン
  - パッド: 楕円 55.9×85mil、スルーホール Φ40.2mil(約1.02mm)、ピン1のみ角形
  - ピン番号はストレート配列(ピン31がピン1の真後ろ)
  - 出典: EasyEDAコミュニティ「60 PIN FAMICOM CARTRIDGE SLOT」(uuid 3b83de0e…)の実寸を移植。
    発注前に入手したコネクタ実物と照合すること
