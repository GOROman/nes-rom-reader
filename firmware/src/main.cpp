// nes-rom-reader firmware (M5Stamp S3)
//
// ホストとはUSB CDCで通信。プロトコル(1行コマンド、応答はバイナリ):
//   V                      -> "famidump v0.6 rev<基板リビジョン>\n"
//   R <addr_hex> <len_hex> -> PRG読み出し。"OK <len>\n" + 生データ + "CRC xxxxxxxx\n"
//   C <addr_hex> <len_hex> -> CHR読み出し。同上
//   M                      -> ミラーリング判定 "H\n" or "V\n" or "?\n"
//   T                      -> セルフテスト。複数行レポートの最後に
//                             "SELFTEST PASS\n" または "SELFTEST FAIL\n"
//   W <addr_hex> <len_hex> -> バッテリバックアップRAM($6000-$7FFF)読み出し。
//                             応答形式は R/C と同じ。
//   P <addr_hex> <data_hex> -> CPU空間へ1バイト書き込み(マッパーレジスタ/WRAM)。
//                             "WROK <addr> <data>\n" を返す。基板v0.2以降のみ。
//                             v0.1基板では "ERR NEEDS_REV2\n"。
//   B <addr_hex>           -> バンク選択(CNROM等)。指定アドレスへダミーライト
//                             サイクルを発生させ "BANK xxxx\n" を返す。
//                             バスコンフリクトを利用するため、addr には
//                             「選びたいバンク番号と同じ値が入っているPRGアドレス」
//                             を指定する(ホスト側でPRGダンプから検索)。
//   F                      -> YM2151 初期化(φM 3.579545MHz 供給開始 + /IC リセット)。
//                             "YMRDY\n" を返す。基板改造不要(全リビジョンで有効)。
//   Y <reg_hex> <val_hex>  -> YM2151 レジスタ書き込み。"YMOK rr vv\n"
//   Q                      -> YM2151 デモ(ドレミファソラシドをループ再生)。
//                             次のコマンド受信で停止し "YMDEMO DONE\n"
//   A                      -> G46 に 880Hz テストトーンを1.5秒出力(配線前チェック用)。
//                             "TONE DONE\n"
//
// スタンドアロン自動演奏: 電源ONから2秒以内にシリアル入力がなければ
// 自動で F+Q 相当を実行し演奏を続ける。シリアル入力で演奏と φM を止めて
// 通常のダンパー動作へ復帰する(その入力は普通のコマンドとして処理される)。
//
// YM2151 は docs/ym2151.md の通り「アドレスバス経由」でカートリッジへ接続する:
//   D0-7=CPU A0-A7  A0=CPU A8  /WR=CPU A9  /IC=CPU A10  /CS=GND  /RD=+5V
//   φM=M2ピン(LEDC PWM 3.579545MHz に切替)
// データもストローブも 74HCT595 が常時駆動するアドレス線に乗せるため
// U6/BUS_DIR の改造が不要で、カート側のハンダ付けは PRG ROM の足+
// エッジフィンガー32(M2)の1点だけで済む。BUSY は固定ウェイトで代替。
// F 実行後はカートリッジ用コマンド(R/C/W/P/B)と併用しないこと。
//
// データブロック直後の "CRC xxxxxxxx\n" は生データの CRC32
// (IEEE 802.3 / zlib.crc32 互換、8桁大文字hex)。ホスト側で照合する。

#include <Arduino.h>
#include <esp_task_wdt.h>

// --- ピンアサイン (docs/hardware-design.md と一致させること) ---
// 制御線は全て 74HCT541 バッファ経由で 5V 化してカートリッジへ。
static const int PIN_D[8]    = {4, 5, 6, 7, 8, 9, 10, 11};  // MD0-MD7 (LVC245 A側共通バス)
static const int PIN_SR_DATA  = 12;  // SR_DATA  -> 541 -> U2.SER
static const int PIN_SR_CLK   = 13;  // SR_CLK   -> 541 -> 595 SRCLK (4個共通)
static const int PIN_SR_LATCH = 14;  // SR_LATCH -> 541 -> 595 RCLK (4個共通)
static const int PIN_OE_PRG   = 15;  // OE_PRG_N: U6 (LVC245, CPUデータ) /OE 負論理
static const int PIN_OE_CHR   = 39;  // OE_CHR_N: U7 (LVC245, PPUデータ) /OE 負論理
static const int PIN_ROMSEL   = 41;  // /ROMSEL 負論理
static const int PIN_M2       = 42;
static const int PIN_RW       = 1;   // CPU R/W High=Read
static const int PIN_PPU_RD   = 2;   // PPU /RD 負論理
static const int PIN_PPU_WR   = 3;   // PPU /WR 負論理
static const int PIN_CIRAM_A10 = 46; // 入力 (10k/20k分圧で5V→3.3V)
static const int PIN_LED       = 21; // M5Stamp S3 内蔵 WS2812 RGB LED
static const int PIN_BUS_DIR   = 40; // 基板v0.2: 74LVC8T245 DIR (H=A→B 書き込み)

// 基板リビジョン。v0.1 はデータバッファが読み出し方向固定なので、
// 書き込み系(P コマンド)は BOARD_REV>=2 でのみ有効。platformio.ini で上書きする。
#ifndef BOARD_REV
#define BOARD_REV 1
#endif

// --- ステータスLED (内蔵WS2812) ---
// 緑=待機/正常、青=読み出し中、赤点滅=エラー。明るさは控えめ。
static void led(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(PIN_LED, r, g, b); }
static void ledReady() { led(0, 24, 0); }   // 緑: 電源ON/待機
static void ledBusy()  { led(0, 0, 30); }   // 青: 吸い出し中
static void ledError() {                     // 赤点滅 → 緑へ復帰
  for (int i = 0; i < 4; i++) { led(48, 0, 0); delay(120); led(0, 0, 0); delay(120); }
  ledReady();
}

// --- シフトレジスタ 32bit ビット割り当て (U2→U3→U4→U5 直列) ---
// srWrite32() は bit31 から MSBファーストで送出するため、32クロック後に
// bit0 が U2.QA、bit31 が U5.QH に格納される。
//
//   bit0-7   : U2 QA-QH = CPU A0-A7  (CA0-CA7)
//   bit8-14  : U3 QA-QG = CPU A8-A14 (CA8-CA14)
//   bit15    : U3 QH    = 予備
//   bit16-23 : U4 QA-QH = PPU A0-A7  (PA0-PA7)
//   bit24-29 : U5 QA-QF = PPU A8-A13 (PA8-PA13)
//   bit30    : U5 QG    = PPU /A13   (カートピン49) ※PPU A13 の反転を必ず駆動
//   bit31    : U5 QH    = 予備
static const uint32_t SR_PPU_A13_N = 1UL << 30;

// 通常の PRG/CHR 読み出しでは PPU A13=0 なので bit30 (PPU /A13) は常に 1。
static inline uint32_t srCpuAddr(uint16_t addr) {
  return ((uint32_t)(addr & 0x7FFF)) | SR_PPU_A13_N;
}
static inline uint32_t srPpuAddr(uint16_t addr) {
  uint32_t v = ((uint32_t)(addr & 0x3FFF)) << 16;
  if (!(addr & 0x2000)) v |= SR_PPU_A13_N;  // bit30 = NOT(PPU A13)
  return v;
}

static void srWrite32(uint32_t v) {
  for (int i = 31; i >= 0; i--) {  // MSBファースト (bit31が最初)
    digitalWrite(PIN_SR_DATA, (v >> i) & 1);
    digitalWrite(PIN_SR_CLK, HIGH);
    digitalWrite(PIN_SR_CLK, LOW);
  }
  digitalWrite(PIN_SR_LATCH, HIGH);
  digitalWrite(PIN_SR_LATCH, LOW);
}

static uint8_t readDataBus() {
  uint32_t in = REG_READ(GPIO_IN_REG);  // MD0-MD7は全てGPIO<32
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((in >> PIN_D[i]) & 1) << i;
  return v;
}

// CRC32 (IEEE 802.3, zlib互換)。テーブルレスのニブル実装。
static uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n) {
  static const uint32_t tbl[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
  };
  while (n--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ tbl[crc & 0x0F];
    crc = (crc >> 4) ^ tbl[crc & 0x0F];
  }
  return crc;
}

// PRG空間読み出し。addrはCPUアドレス($8000-$FFFF想定、A0-A14のみ使用)
static uint8_t readPrg(uint16_t addr) {
  srWrite32(srCpuAddr(addr));
  digitalWrite(PIN_ROMSEL, LOW);
  digitalWrite(PIN_OE_PRG, LOW);
  delayMicroseconds(1);  // ROMアクセスタイム待ち(要調整)
  uint8_t v = readDataBus();
  digitalWrite(PIN_OE_PRG, HIGH);
  digitalWrite(PIN_ROMSEL, HIGH);
  return v;
}

static void busIdle();  // 前方宣言

// --- バンク切替(CNROM等): バスコンフリクトを利用したダミーライトサイクル ---
//
// CNROM は書き込みサイクル中も PRG-ROM が出力を続ける(AND型バスコンフリクト)。
// そのため「選びたいバンク番号と同じ値が入っている PRG アドレス」へ書き込みサイクルを
// 発生させれば、ROM 自身がその値をデータバスへ出し、カートリッジ側のラッチが取り込む。
// → 本基板はデータバスを駆動できない(読み出し専用)が、バンク選択は可能。
//
// ホスト側は先に PRG を吸い出し、PRG[X] == バンク番号 となる X を探して
// addr = 0x8000 + X を渡す。
static void bankSelectWrite(uint16_t addr) {
  srWrite32(srCpuAddr(addr));   // アドレス確定
  digitalWrite(PIN_OE_PRG, HIGH);  // 自分はデータバスを見ない(ROMに任せる)
  delayMicroseconds(2);

  digitalWrite(PIN_RW, LOW);    // 書き込みサイクル
  delayMicroseconds(1);
  digitalWrite(PIN_ROMSEL, LOW);  // PRG-ROM がデータを出す(バスコンフリクト)
  delayMicroseconds(2);
  digitalWrite(PIN_M2, LOW);      // M2 をトグル(ラッチのクロック源対策)
  delayMicroseconds(2);
  digitalWrite(PIN_M2, HIGH);
  delayMicroseconds(2);
  digitalWrite(PIN_ROMSEL, HIGH); // 立ち上がりでラッチする実装が多い
  delayMicroseconds(1);
  digitalWrite(PIN_RW, HIGH);     // 読み出しに戻す
  delayMicroseconds(2);
  busIdle();
}

// --- 実バス書き込み (基板 v0.2 以降のみ) ---
//
// v0.1 のデータバッファ(74LVC245)は方向がB→A固定なので、MCU側から出力すると
// バッファの出力と衝突してショートする。よって BOARD_REV>=2 でのみ有効化する。
// v0.2 は 74LVC8T245 + BUS_DIR(G40) でカートリッジ側を駆動できる。
#if BOARD_REV >= 2
static void driveDataBus(uint8_t v) {
  for (int i = 0; i < 8; i++) {
    pinMode(PIN_D[i], OUTPUT);
    digitalWrite(PIN_D[i], (v >> i) & 1);
  }
}
static void releaseDataBus() {
  for (int i = 0; i < 8; i++) pinMode(PIN_D[i], INPUT);
}

// CPU空間へ1バイト書き込む。マッパーレジスタ($8000-$FFFF)とWRAM($6000-$7FFF)の両方に対応。
static void writeCpu(uint16_t addr, uint8_t data) {
  bool rom = addr >= 0x8000;   // $8000以上は /ROMSEL でデコードされる
  srWrite32(srCpuAddr(addr));
  digitalWrite(PIN_OE_PRG, HIGH);   // バッファを一旦切ってから方向を変える
  digitalWrite(PIN_BUS_DIR, HIGH);  // A→B: MCUがカートリッジを駆動
  driveDataBus(data);
  digitalWrite(PIN_OE_PRG, LOW);    // バッファON。ここでカート側にデータが出る
  delayMicroseconds(2);

  digitalWrite(PIN_RW, LOW);        // 書き込みサイクル
  delayMicroseconds(1);
  if (rom) digitalWrite(PIN_ROMSEL, LOW);
  digitalWrite(PIN_M2, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_M2, HIGH);       // M2 High がCPUアクセス位相
  delayMicroseconds(3);
  digitalWrite(PIN_M2, LOW);        // 立ち下がりでラッチする実装が多い
  delayMicroseconds(1);
  if (rom) digitalWrite(PIN_ROMSEL, HIGH);
  digitalWrite(PIN_RW, HIGH);
  delayMicroseconds(1);

  digitalWrite(PIN_OE_PRG, HIGH);   // バッファOFF → 方向を読み出しへ戻す
  releaseDataBus();
  digitalWrite(PIN_BUS_DIR, LOW);
  busIdle();
}
#endif  // BOARD_REV >= 2

// バッテリバックアップRAM(WRAM/SRAM)読み出し。addrは$6000-$7FFF。
//
// この領域は /ROMSEL が非アクティブ(High)のまま、カートリッジ側が
// M2 + CPU A13/A14 をデコードしてRAMを選択する。よってPRG読み出しとは
// シーケンスが異なり、M2 を Low→High とトグルしてCPUアクセス位相を作る。
static uint8_t readWram(uint16_t addr) {
  srWrite32(srCpuAddr(addr));      // A13=A14=1 ($6000-$7FFF)
  digitalWrite(PIN_ROMSEL, HIGH);  // PRG-ROMは選択しない
  digitalWrite(PIN_RW, HIGH);      // 読み出し
  digitalWrite(PIN_M2, LOW);       // アドレス確定フェーズ
  delayMicroseconds(1);
  digitalWrite(PIN_M2, HIGH);      // M2 High = CPUアクセス中。ここでRAMが出力
  delayMicroseconds(2);
  digitalWrite(PIN_OE_PRG, LOW);
  delayMicroseconds(1);
  uint8_t v = readDataBus();
  digitalWrite(PIN_OE_PRG, HIGH);
  digitalWrite(PIN_M2, LOW);
  return v;
}

// CHR空間読み出し。addrはPPUアドレス($0000-$1FFF、PPU A13=0 → bit30=1)
static uint8_t readChr(uint16_t addr) {
  srWrite32(srPpuAddr(addr & 0x1FFF));
  digitalWrite(PIN_PPU_RD, LOW);
  digitalWrite(PIN_OE_CHR, LOW);
  delayMicroseconds(1);
  uint8_t v = readDataBus();
  digitalWrite(PIN_OE_CHR, HIGH);
  digitalWrite(PIN_PPU_RD, HIGH);
  return v;
}

// ミラーリング判定: PPU A11をトグルしてCIRAM A10が追従すればH、
// PPU A10追従ならV
static char detectMirroring() {
  srWrite32(srPpuAddr(0x0800));  // PPU A11=1 (PPU A13=0, bit30=1)
  delayMicroseconds(1);
  bool a11 = digitalRead(PIN_CIRAM_A10);
  srWrite32(srPpuAddr(0x0400));  // PPU A10=1
  delayMicroseconds(1);
  bool a10 = digitalRead(PIN_CIRAM_A10);
  if (a11 && !a10) return 'H';
  if (a10 && !a11) return 'V';
  return '?';  // 4画面 or 未接続
}

// YM モード中は M2 ピンが LEDC で φM を出力しているため、
// digitalWrite で GPIO に戻してしまわないようガードする。
static bool ymClockOn = false;

static void busIdle() {
  digitalWrite(PIN_OE_PRG, HIGH);
  digitalWrite(PIN_OE_CHR, HIGH);
  digitalWrite(PIN_ROMSEL, HIGH);
  if (!ymClockOn) digitalWrite(PIN_M2, HIGH);
  digitalWrite(PIN_RW, HIGH);
  digitalWrite(PIN_PPU_RD, HIGH);
  digitalWrite(PIN_PPU_WR, HIGH);
}

// --- YM2151 (フルアドレスバス方式、基板改造不要・全リビジョン対応) ---
//
// YM2151 をすべてアドレス線だけで駆動する。74HCT595 が常時5Vで駆動するので
// レベル変換も U6/BUS_DIR 改造も不要:
//   CA0-CA7 = D0-D7 (PRG ROM pin 10-3 の連続8本)
//   CA8     = A0    (PRG ROM pin 25)
//   CA9     = /WR   (PRG ROM pin 24)  srWrite32 でストローブを作る
//   CA10    = /IC   (PRG ROM pin 21)
// /CS=GND・/RD=+5V 固定。φM のみエッジフィンガー32(M2)から供給する。
// /WR パルス幅は srWrite32 1回分(数十µs)になるが、YM2151 はスタティック
// 入力で幅の上限はなく min 100ns を満たせばよい。
// 起動直後〜F実行前はシフトレジスタが全0 = /IC=Low なので YM はリセット状態
// に保たれる(好都合)。
static const uint32_t YM_CLOCK_HZ = 3579545;  // φM: NTSC カラーバースト
static const uint16_t YM_WR_N = 1 << 9;   // CPU A9  = /WR (負論理)
static const uint16_t YM_IC_N = 1 << 10;  // CPU A10 = /IC (負論理)

static void ymClockStart() {
  if (ymClockOn) return;
  ledcAttach(PIN_M2, YM_CLOCK_HZ, 3);  // 3bit分解能 (4bit だと div<2 で setup 失敗)
  ledcWrite(PIN_M2, 4);                // duty 4/8 = 50%
  ymClockOn = true;
}

// --- YM3012 シミュレーション ---
//
// YM2151 のシリアル音声出力を取り込み、デコードして PWM で再生する。
//   SO  → カートエッジ26 (PPU D0) → U7で3.3V化 → MD0 = G4
//   SH1 → カートエッジ27 (PPU D1) → U7        → MD1 = G5
//   φ1  → カートエッジ28 (PPU D2) → U7        → MD2 = G6
// 取り込み中は OE_CHR を Low にして U7 を有効化する。
// 再生は G46(CIRAM A10 分圧の中点)へ LEDC PWM 78.125kHz/10bit を出力し、
// 分圧中点から RC フィルタ+DCカット経由でアンプへ渡す。
//
// フォーマット(YM3012 互換): φ1 立ち上がりで SO を LSB ファーストにシフト、
// SH1 立ち下がりで直前の13bit(仮数 B0-B9=オフセットバイナリ、指数 S0-S2)を
// ラッチする。SH1 側 = RIGHT ch のモノラル。フレーム同期は SH1 エッジで
// 取り直すため、ビット化けしても次のワードで自己復帰する。
static const int PIN_YM_SO   = 4;   // MD0
static const int PIN_YM_SH1  = 5;   // MD1
static const int PIN_YM_PHI1 = 6;   // MD2
static const int PIN_PWM_OUT = 46;  // CIRAM A10 分圧の中点
// 78.125kHz / 9bit (80MHz / 512 / 2)。LEDC は分周比2未満を設定できないため
// 10bit では setup が失敗する(div_param=0)。
static const uint32_t PWM_FREQ = 78125;
static const int PWM_RES = 9;       // duty 0-511、中点 256

static volatile bool ymCaptureRun = false;

// Core 0 の専用タスク。φ1(約1.79MHz)をポーリングでエッジ検出する。
// 割り込みは許可したままなので tick 等で稀にビットを落とすが、
// SH1 エッジ同期のため次ワードで復帰する(軽微なクラックルのみ)。
static void ymCaptureLoop(void*) {
  const uint32_t SO = 1UL << PIN_YM_SO;
  const uint32_t SH = 1UL << PIN_YM_SH1;
  const uint32_t P1 = 1UL << PIN_YM_PHI1;
  for (;;) {
    if (!ymCaptureRun) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    uint32_t prev = REG_READ(GPIO_IN_REG);
    uint16_t sr = 0;
    while (ymCaptureRun) {
      uint32_t in = REG_READ(GPIO_IN_REG);
      uint32_t chg = in ^ prev;
      if ((chg & P1) && (in & P1))            // φ1 立ち上がり: SO を取り込み
        sr = (sr >> 1) | ((in & SO) ? 0x8000 : 0);
      if ((chg & SH) && !(in & SH)) {         // SH1 立ち下がり: ワード確定
        uint16_t m = (sr >> 3) & 0x3FF;       // 仮数 (B0 が LSB、B9=符号)
        uint16_t e = (sr >> 13) & 0x07;       // 指数 (S0 が LSB)
        int32_t v = (int32_t)m - 512;         // -512..+511
        // 指数が大きいほど大振幅と仮定(逆だったらここを (e) に変える)
        int32_t duty = 256 + (v >> (8 - (e ? e : 1)));  // 9bit: 0-511
        ledcWrite(PIN_PWM_OUT, (uint32_t)duty);
      }
      prev = in;
    }
  }
}

static void ymAudioStart() {
  digitalWrite(PIN_OE_CHR, LOW);        // U7 有効化 → SO/SH1/φ1 が G4-G6 に届く
  ledcAttach(PIN_PWM_OUT, PWM_FREQ, PWM_RES);
  ledcWrite(PIN_PWM_OUT, 256);          // 無音(中点)
  ymCaptureRun = true;
}

static void ymAudioStop() {
  ymCaptureRun = false;
  delay(2);
  ledcDetach(PIN_PWM_OUT);
  pinMode(PIN_PWM_OUT, INPUT);          // ミラーリング判定入力に戻す
  digitalWrite(PIN_OE_CHR, HIGH);
}

// G46 出力の配線前チェック用テストトーン(880Hz 矩形波を1.5秒)
static void toneTest() {
  bool wasRunning = ymCaptureRun;
  if (wasRunning) ymAudioStop();
  ledcAttach(PIN_PWM_OUT, 880, 10);
  ledcWrite(PIN_PWM_OUT, 512);
  delay(1500);
  ledcDetach(PIN_PWM_OUT);
  pinMode(PIN_PWM_OUT, INPUT);
  if (wasRunning) ymAudioStart();
}

// φM を止めて M2 を通常の GPIO(High) に戻す。カートリッジコマンドと共存するため。
static void ymClockStop() {
  if (!ymClockOn) return;
  ymAudioStop();
  ledcDetach(PIN_M2);
  ymClockOn = false;
  pinMode(PIN_M2, OUTPUT);
  digitalWrite(PIN_M2, HIGH);
}

// BUSY は読めないので最悪値で待つ。BUSY 期間は φM 68サイクル ≒ 19µs。
static void ymWaitBusy() { delayMicroseconds(30); }

// データと A0 をアドレス線に確定させ、A9 で /WR パルスを作る。
// セットアップ/ホールドは srWrite32 の所要時間(数十µs)で自然に満たされる。
static void ymWriteBus(bool a0, uint8_t v) {
  uint16_t base = (uint16_t)v | (a0 ? 0x100 : 0) | YM_IC_N;
  srWrite32(srCpuAddr(base | YM_WR_N));  // データ確定、/WR=H
  srWrite32(srCpuAddr(base));            // /WR=L
  srWrite32(srCpuAddr(base | YM_WR_N));  // /WR 立ち上がりで取り込み
}

static void ymWriteReg(uint8_t reg, uint8_t val) {
  ymWriteBus(false, reg);   // A0=0: アドレス
  ymWaitBusy();
  ymWriteBus(true, val);    // A0=1: データ
  ymWaitBusy();
}

// φM 供給開始 + /IC リセット。YM2151 はリセット中もクロックが必要。
static void ymInit() {
  busIdle();
  ymClockStart();
  srWrite32(srCpuAddr(YM_WR_N));           // /IC=L (A10=0)、/WR=H
  delay(2);                                // 最低 100µs 以上
  srWrite32(srCpuAddr(YM_WR_N | YM_IC_N)); // /IC 解除
  delay(2);
  ymAudioStart();                          // YM3012 シミュレーション開始
}

// デモ: ch0 にシンプルな音色(CON=7, キャリア1個)を組んで
// ドレミファソラシド(C4-C5)をループ再生する。
// 次のシリアル入力(=次のコマンド)を受信したら抜ける。
static void ymDemo() {
  ymWriteReg(0x20, 0xC7);  // ch0: RL=両ch, FB=0, CON=7(全スロット並列)
  ymWriteReg(0x30, 0x00);  // KF=0
  for (int op = 0; op < 4; op++) {
    uint8_t s = 0 + op * 8;               // ch0 のスロットオフセット
    ymWriteReg(0x40 + s, 0x01);           // DT1=0, MUL=1
    ymWriteReg(0x60 + s, op == 3 ? 0x10 : 0x7F);  // C2 のみ発音、他は TL 最小
    ymWriteReg(0x80 + s, 0x1F);           // AR 最速
    ymWriteReg(0xA0 + s, 0x05);           // D1R
    ymWriteReg(0xC0 + s, 0x02);           // D2R
    ymWriteReg(0xE0 + s, 0x1A);           // D1L=1, RR=10
  }
  // KC: 上位ニブル=オクターブ、下位=音名 (C#=0,D=1,D#=2,E=4,F=5,F#=6,
  // G=8,G#=9,A=A,A#=C,B=D,C=E。3/7/B/F は欠番)
  static const uint8_t kc[8] = {0x3E, 0x41, 0x44, 0x45,   // ド レ ミ ファ (C4 D4 E4 F4)
                                0x48, 0x4A, 0x4D, 0x4E};  // ソ ラ シ ド   (G4 A4 B4 C5)
  while (!Serial.available()) {   // 次のコマンドが来るまでループ
    for (int i = 0; i < 8 && !Serial.available(); i++) {
      ymWriteReg(0x28, kc[i]);  // KC
      ymWriteReg(0x08, 0x78);   // ch0 全スロット KeyOn
      delay(220);
      ymWriteReg(0x08, 0x00);   // KeyOff
      delay(60);
    }
    delay(300);                 // 1周ごとに一息
  }
  ymWriteReg(0x08, 0x00);       // 抜けるときはキーオフ
}

void setup() {
  for (int i = 0; i < 8; i++) pinMode(PIN_D[i], INPUT);
  pinMode(PIN_SR_DATA, OUTPUT);
  pinMode(PIN_SR_CLK, OUTPUT);
  pinMode(PIN_SR_LATCH, OUTPUT);
  pinMode(PIN_OE_PRG, OUTPUT);
  pinMode(PIN_OE_CHR, OUTPUT);
  pinMode(PIN_ROMSEL, OUTPUT);
  pinMode(PIN_M2, OUTPUT);
  pinMode(PIN_RW, OUTPUT);
  pinMode(PIN_PPU_RD, OUTPUT);
  pinMode(PIN_PPU_WR, OUTPUT);
  pinMode(PIN_CIRAM_A10, INPUT);
#if BOARD_REV >= 2
  // U6 DIRジャンパ改造基板で外付けプルダウンを省く場合の保険として、
  // 内部プルダウン(約45kΩ)を有効化してから LOW 駆動に移る。
  // ※ 電源ON〜ここまでの間は G40 はフローティングなので、常用は外付け10k推奨。
  pinMode(PIN_BUS_DIR, INPUT_PULLDOWN);
  pinMode(PIN_BUS_DIR, OUTPUT);
  digitalWrite(PIN_BUS_DIR, LOW);  // 既定は B→A(読み出し)
#endif
  busIdle();
  srWrite32(SR_PPU_A13_N);  // アイドル時も PPU /A13=1 (PPU A13=0)
  // YM3012 シミュレーション用キャプチャタスク(Core 0)。
  // ymCaptureRun が立つまで待機する。優先度は USB スタックより低く、
  // loopTask(Core 1)とは別コアなので通常動作へ影響しない。
  // キャプチャ中は Core 0 の idle が回らないためタスクWDTを止める
  // (disableCore0WDT は core 3.x でログを吐き続けるので deinit を使う)。
  esp_task_wdt_deinit();
  xTaskCreatePinnedToCore(ymCaptureLoop, "ymcap", 4096, nullptr, 3, nullptr, 0);
  Serial.begin(115200);
  ledReady();               // 電源ON = 緑
}

// セルフテスト: カセット無しで実行できる範囲の健全性チェック。
// 1) 出力GPIOがドライブできるか(High/Lowを実測して読み戻し)
// 2) シフトレジスタへ walking-bit を流して "詰まり" なく送出できるか
// 3) データバス(MD0-7)のフロート読み(参考値)
// ※ 完全な導通試験には基準カセット吸い出しのCRC照合(ホスト側 factory_test)を併用。
// 制御用出力ピンの自己読み戻しチェック。verbose時は各ピンの結果を出力。
static bool selfCheckPins(bool verbose) {
  bool pass = true;
  const int outPins[] = {PIN_SR_DATA, PIN_SR_CLK, PIN_SR_LATCH, PIN_OE_PRG,
                         PIN_OE_CHR, PIN_ROMSEL, PIN_M2, PIN_RW, PIN_PPU_RD, PIN_PPU_WR};
  const char* outNames[] = {"SR_DATA", "SR_CLK", "SR_LATCH", "OE_PRG_N",
                            "OE_CHR_N", "ROMSEL_N", "M2", "CPU_RW", "PPU_RD_N", "PPU_WR_N"};
  int nOut = sizeof(outPins) / sizeof(outPins[0]);
  for (int i = 0; i < nOut; i++) {
    digitalWrite(outPins[i], HIGH);
    bool hi = digitalRead(outPins[i]);
    digitalWrite(outPins[i], LOW);
    bool lo = digitalRead(outPins[i]);
    bool ok = hi && !lo;
    if (!ok) pass = false;
    if (verbose) Serial.printf("PIN %-9s %s\n", outNames[i], ok ? "OK" : "NG");
  }
  busIdle();
  return pass;
}

// データバス(MD0-7)のフロート読み(参考値)
static uint8_t readMdFloat() {
  digitalWrite(PIN_OE_PRG, LOW);
  delayMicroseconds(2);
  uint8_t v = readDataBus();
  digitalWrite(PIN_OE_PRG, HIGH);
  return v;
}

static const uint32_t STEP_DELAY_MS = 1000;  // セルフテスト 1ステップの待ち時間

// セルフテスト: 1ステップごとに約1秒待ちながら進める(LED: 青=検査中→緑/赤=結果)。
static void handleSelfTest() {
  bool pass = true;
  const int outPins[] = {PIN_SR_DATA, PIN_SR_CLK, PIN_SR_LATCH, PIN_OE_PRG,
                         PIN_OE_CHR, PIN_ROMSEL, PIN_M2, PIN_RW, PIN_PPU_RD, PIN_PPU_WR};
  const char* outNames[] = {"SR_DATA", "SR_CLK", "SR_LATCH", "OE_PRG_N",
                            "OE_CHR_N", "ROMSEL_N", "M2", "CPU_RW", "PPU_RD_N", "PPU_WR_N"};
  int nOut = sizeof(outPins) / sizeof(outPins[0]);
  for (int i = 0; i < nOut; i++) {
    ledBusy();  // 青: 検査中
    delay(150);
    digitalWrite(outPins[i], HIGH);
    bool hi = digitalRead(outPins[i]);
    digitalWrite(outPins[i], LOW);
    bool lo = digitalRead(outPins[i]);
    bool ok = hi && !lo;
    if (!ok) pass = false;
    led(ok ? 0 : 60, ok ? 30 : 0, 0);  // 緑=OK / 赤=NG
    Serial.printf("PIN %-9s %s\n", outNames[i], ok ? "OK" : "NG");
    delay(STEP_DELAY_MS);
  }
  busIdle();

  ledBusy(); delay(150);
  for (int b = 0; b < 32; b++) srWrite32(1UL << b);  // シフトレジスタ walking-bit 送出確認
  srWrite32(SR_PPU_A13_N);
  Serial.printf("SHIFT walking-bit x32 DONE\n");
  delay(STEP_DELAY_MS);

  ledBusy(); delay(150);
  Serial.printf("MD float read = 0x%02X\n", readMdFloat());
  delay(STEP_DELAY_MS);

  Serial.printf("SELFTEST %s\n", pass ? "PASS" : "FAIL");
  if (pass) ledReady(); else ledError();  // 合格=緑 / 不合格=赤点滅
}

// 機械可読の1行ステータス(Web UI/CLIがパースしやすい形):
//   STATUS <ver> mirror=<H|V|?> pins=<PASS|FAIL> md=0x<hex>
static void handleStatus() {
  bool pins = selfCheckPins(false);
  char m = detectMirroring();
  uint8_t md = readMdFloat();
  Serial.printf("STATUS famidump-v0.6 mirror=%c pins=%s md=0x%02X\n",
                m, pins ? "PASS" : "FAIL", md);
}

// region: 'R'=PRG-ROM, 'C'=CHR, 'W'=バッテリバックアップRAM($6000-$7FFF)
static void handleRead(char region, uint32_t addr, uint32_t len) {
  ledBusy();  // 青: 読み出し中
  Serial.printf("OK %lX\n", (unsigned long)len);
  uint8_t buf[256];
  uint32_t crc = 0xFFFFFFFF;
  while (len > 0) {
    uint32_t n = min(len, (uint32_t)sizeof(buf));
    for (uint32_t i = 0; i < n; i++)
      buf[i] = region == 'C' ? readChr(addr + i)
             : region == 'W' ? readWram(addr + i)
                             : readPrg(addr + i);
    crc = crc32Update(crc, buf, n);
    Serial.write(buf, n);
    addr += n;
    len -= n;
  }
  Serial.printf("CRC %08lX\n", (unsigned long)(crc ^ 0xFFFFFFFF));
  ledReady();  // 緑: 完了
}

void loop() {
  // スタンドアロン自動演奏: 電源ONから2秒間シリアル入力がなければ
  // YM2151 を初期化してデモをループ再生する。シリアル入力(=最初のコマンド)で
  // 演奏を止め、φM も停止して通常のダンパー動作へ完全復帰する。
  static bool autoPlayChecked = false;
  if (!autoPlayChecked) {
    if (Serial.available()) {
      autoPlayChecked = true;          // ホストが先に喋った → 通常モード
    } else if (millis() > 2000) {
      autoPlayChecked = true;
      ymInit();
      ledBusy();
      ymDemo();                        // シリアル入力が来るまで演奏
      ymClockStop();                   // M2 を GPIO に戻す
      busIdle();
      ledReady();
    } else {
      return;
    }
  }

  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') {
      if (c != '\r') line += c;
      continue;
    }
    char cmd = line.length() ? line[0] : 0;
    uint32_t addr = 0, len = 0;
    sscanf(line.c_str() + 1, "%lx %lx", (unsigned long*)&addr, (unsigned long*)&len);
    line = "";
    // カートリッジ系コマンドは YM モード(M2=クロック出力)と両立しないので、
    // 実行前に自動で YM モードを解除して通常のダンパー状態へ戻す。
    if (cmd && ymClockOn && strchr("RCWMTSBP", cmd)) ymClockStop();
    switch (cmd) {
      case 'V': Serial.printf("famidump v0.6 rev%d\n", BOARD_REV); break;
      case 'R': handleRead('R', addr, len); break;
      case 'C': handleRead('C', addr, len); break;
      case 'W': handleRead('W', addr, len); break;
      case 'M': Serial.printf("%c\n", detectMirroring()); break;
      case 'T': handleSelfTest(); break;
      case 'S': handleStatus(); break;
      case 'B': bankSelectWrite((uint16_t)addr); Serial.printf("BANK %04X\n", (unsigned)(addr & 0xFFFF)); break;
#if BOARD_REV >= 2
      case 'P':
        writeCpu((uint16_t)addr, (uint8_t)len);
        Serial.printf("WROK %04X %02X\n", (unsigned)(addr & 0xFFFF), (unsigned)(len & 0xFF));
        break;
#else
      case 'P': Serial.print("ERR NEEDS_REV2\n"); break;
#endif
      case 'F':
        ymInit();
        Serial.print("YMRDY\n");
        break;
      case 'Y':
        if (!ymClockOn) { Serial.print("ERR YM_NOT_INIT\n"); ledError(); break; }
        ymWriteReg((uint8_t)addr, (uint8_t)len);
        Serial.printf("YMOK %02X %02X\n", (unsigned)(addr & 0xFF), (unsigned)(len & 0xFF));
        break;
      case 'Q':
        if (!ymClockOn) { Serial.print("ERR YM_NOT_INIT\n"); ledError(); break; }
        ledBusy();
        ymDemo();
        ledReady();
        Serial.print("YMDEMO DONE\n");
        break;
      case 'A':
        toneTest();
        Serial.print("TONE DONE\n");
        break;
      default:  Serial.print("ERR\n"); ledError(); break;  // 不正コマンド=赤点滅
    }
  }
}
