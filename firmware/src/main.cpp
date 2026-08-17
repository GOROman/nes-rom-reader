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
//   F                      -> YM2151 初期化(φM 4MHz 供給開始 + /IC リセット)。
//                             "YMRDY\n" を返す。基板改造不要(全リビジョンで有効)。
//   Y <reg_hex> <val_hex>  -> YM2151 レジスタ書き込み。"YMOK rr vv\n"
//   Q                      -> YM2151 デモ(ドレミファソラシドをループ再生)。
//                             次のコマンド受信で停止し "YMDEMO DONE\n"
//   A                      -> R=880Hz/L=440Hz テストトーンを3秒出力(配線チェック用)。
//                             "TONE DONE\n"
//   K <kHz>                -> φM を変更(10進kHz、100-4500)。"CLK <hz>\n"。主にピッチが変わる
//                             行頭の +/- キー1打でも ±50kHz(改行不要)。
//                             +/- は E/Q の演奏を止めずに効く(X ストリーム中は不可)
//   即時キー(YMモード中、改行不要、演奏中も可):
//     1-8=chミュート A=全ch解除 L/R=左右出力トグル +/-=ピッチ±50kHz
//     P=内蔵曲を頭から S=曲停止(モード維持) Q=Quit(YMモード完全終了) H=ヘルプ
//
// WiFi: W コマンド(引数なし)でオンデマンド起動/停止。SoftAP "YM2151"
// (pass: ym2151jukebox) → http://192.168.4.1 に Web UI(再生/停止・
// chミュート・L/R・ピッチ)。操作は仮想キー注入なのでストリーム再生中も効く。
// ※常時ONにしないのは、送信スパイク電流でカート接続時にブートループするため
//
// スタンドアロン自動演奏: 電源ONから2秒以内にシリアル入力がなければ
// 自動で F+Q 相当を実行し演奏を続ける。シリアル入力で演奏と φM を止めて
// 通常のダンパー動作へ復帰する(その入力は普通のコマンドとして処理される)。
//
// YM2151 は docs/ym2151.md の通り「アドレスバス経由」でカートリッジへ接続する:
//   D0-7=CPU A0-A7  A0=CPU A8  /WR=CPU A9  /IC=CPU A10  /CS=GND  /RD=+5V
//   φM=M2ピン(LEDC PWM 4MHz に切替)
// データもストローブも 74HCT595 が常時駆動するアドレス線に乗せるため
// U6/BUS_DIR の改造が不要で、カート側のハンダ付けは PRG ROM の足+
// エッジフィンガー32(M2)の1点だけで済む。BUSY は固定ウェイトで代替。
// F 実行後はカートリッジ用コマンド(R/C/W/P/B)と併用しないこと。
//
// データブロック直後の "CRC xxxxxxxx\n" は生データの CRC32
// (IEEE 802.3 / zlib.crc32 互換、8桁大文字hex)。ホスト側で照合する。

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
#include "embedded_song.h"
#include <hal/gpio_ll.h>
#include <driver/ledc.h>
#include <hal/ledc_ll.h>
#include <driver/dedic_gpio.h>
#include <hal/dedic_gpio_cpu_ll.h>

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

// GPIO レジスタ直叩きで高速化(digitalWrite 比 約10倍)。YM2151 再生時の
// 書き込みバースト遅延(テンポのもたつき)対策。SRCLK パルス幅はダミー
// 書き込みで約80nsを確保(74HCT595 の最小パルス幅 20ns@4.5V に対し十分)。
static void srWrite32(uint32_t v) {
  const uint32_t B_DATA  = 1UL << PIN_SR_DATA;
  const uint32_t B_CLK   = 1UL << PIN_SR_CLK;
  const uint32_t B_LATCH = 1UL << PIN_SR_LATCH;
  for (int i = 31; i >= 0; i--) {  // MSBファースト (bit31が最初)
    if ((v >> i) & 1) REG_WRITE(GPIO_OUT_W1TS_REG, B_DATA);
    else              REG_WRITE(GPIO_OUT_W1TC_REG, B_DATA);
    REG_WRITE(GPIO_OUT_W1TS_REG, B_CLK);
    REG_WRITE(GPIO_OUT_W1TS_REG, B_CLK);   // パルス幅確保
    REG_WRITE(GPIO_OUT_W1TC_REG, B_CLK);
  }
  REG_WRITE(GPIO_OUT_W1TS_REG, B_LATCH);
  REG_WRITE(GPIO_OUT_W1TS_REG, B_LATCH);   // パルス幅確保
  REG_WRITE(GPIO_OUT_W1TC_REG, B_LATCH);
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
static const uint32_t YM_CLOCK_HZ = 4000000;  // φM 既定値: X68000 と同じ 4MHz
static volatile uint32_t ymClockHz = YM_CLOCK_HZ;  // 現在の φM (K コマンド/±キーで可変)
static volatile uint32_t ymBusyUs  = 22;           // BUSY待ち = 68/φM + 5µs (クロック追従)
static const uint16_t YM_WR_N = 1 << 9;   // CPU A9  = /WR (負論理)
static const uint16_t YM_IC_N = 1 << 10;  // CPU A10 = /IC (負論理)

// φM は Arduino の ledcAttach だと MHz 帯の設定が静かに失敗することがあるため、
// ESP-IDF の API で APB 80MHz ソースを明示して設定する。
// タイマー3/チャネル7 を専有(Arduino 側の自動割り当てと衝突させない)。
static bool ymClockStart() {
  if (ymClockOn) return true;
  // LEDC は全タイマーでクロック源を共有し、Arduino の ledcAttach(音声PWM側)は
  // XTAL(40MHz) を選ぶため、φM 側も明示的に XTAL に合わせる。
  // 2bit 分解能で分周比 40M/(4M×4)=2.5 → 実周波数 4.000MHz。
  ledc_timer_config_t tcfg = {};
  tcfg.speed_mode = LEDC_LOW_SPEED_MODE;
  tcfg.duty_resolution = LEDC_TIMER_2_BIT;
  tcfg.timer_num = LEDC_TIMER_3;
  tcfg.freq_hz = ymClockHz;
  tcfg.clk_cfg = LEDC_USE_XTAL_CLK;
  esp_err_t e1 = ledc_timer_config(&tcfg);
  ledc_channel_config_t ccfg = {};
  ccfg.gpio_num = PIN_M2;
  ccfg.speed_mode = LEDC_LOW_SPEED_MODE;
  ccfg.channel = LEDC_CHANNEL_7;
  ccfg.timer_sel = LEDC_TIMER_3;
  ccfg.duty = 2;                       // 2/4 = 50%
  ccfg.hpoint = 0;
  esp_err_t e2 = ledc_channel_config(&ccfg);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 2);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
  ledc_timer_resume(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3);
  uint32_t fr = ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3);
  Serial.printf("YMCLK freq=%lu (target %lu)\n", (unsigned long)fr, (unsigned long)ymClockHz);
  // 実周波数が目標の±2%以内であることまで検査する(過去に78kHzなど
  // 誤った非ゼロ周波数で成功扱いになった事故があるため)
  bool ok = (e1 == ESP_OK && e2 == ESP_OK &&
             fr > ymClockHz / 100 * 98 && fr < ymClockHz / 100 * 102);
  if (!ok)
    Serial.printf("ERR YM_CLOCK timer=%d ch=%d freq=%lu\n", (int)e1, (int)e2, (unsigned long)fr);
  ymClockOn = ok;   // 失敗時はYMモードに入らない(F は ERR を返す)
  return ok;
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
// YM シリアル信号の割り当て(配線固定・実測で確定済み):
//   φ1  → CHR ROM pin11 (D0) → MD0=G4
//   SO  → CHR ROM pin12 (D1) → MD1=G5
//   SH1 → CHR ROM pin13 (D2) → MD2=G6
//   SH2 → CHR ROM pin16 (D4) → MD4=G8  ※pin15(D3)はパッド不調のため移設
static const int PIN_YM_PHI1 = 4;   // MD0
static const int PIN_YM_SO   = 5;   // MD1
static const int PIN_YM_SH1  = 6;   // MD2
static const int PIN_YM_SH2  = 8;   // MD4 (SH2=LEFTch)
// PWM 音声出力はカートリッジへ向かう空き制御線2本を転用する(基板側の配線が不要):
//   R = PPU /RD (G2)  → 541 → エッジ17 → CHR ROM pin 22 の基板側パッド
//       ※要 CHR ROM pin 22(/OE)の足上げ。しないと PWM で ROM が
//         バスに出てきて YM の信号と衝突する(実測で確認済み)
//   L = /ROMSEL (G41) → 541 → エッジ44 → PRG ROM pin 20 のカット箇所エッジ側
//       (/CS=GND 化により /ROMSEL は完全に未使用。カットは施工済み)
static const int PIN_PWM_R = PIN_PPU_RD;
static const int PIN_PWM_L = PIN_ROMSEL;
// 78.125kHz / 9bit (80MHz / 512 / 2)。LEDC は分周比2未満を設定できないため
// 10bit では setup が失敗する(div_param=0)。
static const uint32_t PWM_FREQ = 78125;
static const int PWM_RES = 9;       // duty 0-511、中点 256
// キャプチャループからレジスタ直叩きで duty 更新するため、チャネル番号を固定する。
// ch0/ch1 は同じタイマー0を共有(同一周波数・独立デューティ)。φM は ch7/timer3。
static const ledc_channel_t YM_PWM_CH_R = LEDC_CHANNEL_0;
static const ledc_channel_t YM_PWM_CH_L = LEDC_CHANNEL_1;

static volatile bool ymCaptureRun = false;
static volatile uint32_t ymSampleCount = 0;   // 採用したワード数
static volatile uint32_t ymFrameCount = 0;    // SH1 立ち下がり総数
static volatile uint16_t ymLastRaw = 0;       // 最後にラッチした生ワード
static volatile uint16_t ymP1Min = 0xFFFF, ymP1Max = 0;  // フレームあたり φ1 エッジ数
static volatile int16_t ymDutyMin = 32767, ymDutyMax = -32768;
static volatile uint32_t ymSampleCountL = 0;  // LEFT ch(SH2)で採用したワード数
static volatile uint16_t ymLastRawL = 0;
static volatile int ymLOff = 0;   // L デコードのビットオフセット (-2..+3、O コマンドで調整)
static volatile int ymROff = 0;   // R 側も同様に調整可能
static volatile uint32_t ymRawBuf[64];  // R ラッチ時の生シフトレジスタ(G コマンドでダンプ)
static volatile uint8_t ymRawIdx = 0;
static volatile uint8_t ymRawArm = 0;   // G コマンドで64サンプル分だけ記録(通常再生は負荷ゼロ)
static volatile bool ymMedianEn = true; // メディアンフィルタ有効(N コマンドで切替)
static volatile uint32_t ymMedFixR = 0, ymMedFixL = 0;  // フィルタが値を差し替えた回数
static volatile uint32_t ymBadExp = 0;  // e==0(YM3012仕様で禁止値)で破棄した数
static volatile uint8_t ymMuteMask = 0;      // bit n = ch n ミュート
static volatile bool ymOutMuteL = false, ymOutMuteR = false;  // L/R出力の個別ミュート
static volatile bool ymRestartReq = false;   // P キー: 頭から再生
static volatile bool ymStopReq = false;      // S キー: 曲停止(YMモードは維持)
static volatile bool ymQuitReq = false;      // Q キー: YMモード完全終了

// --- Web UI からの操作は「仮想キー」として注入し、シリアルの即時キーと
//     同じ経路(再生ループ/メインループ)で消費する。バス書き込みの競合なし ---
static volatile uint8_t vkeyQ[16];
static volatile uint8_t vkeyHead = 0, vkeyTail = 0;
static void pushVKey(uint8_t c) {
  uint8_t next = (vkeyHead + 1) & 15;
  if (next != vkeyTail) { vkeyQ[vkeyHead] = c; vkeyHead = next; }
}
static int popVKey() {
  if (vkeyHead == vkeyTail) return -1;
  uint8_t c = vkeyQ[vkeyTail];
  vkeyTail = (vkeyTail + 1) & 15;
  return c;
}
static volatile int16_t ymDutyMinL = 32767, ymDutyMaxL = -32768;

// Core 0 の専用タスク。φ1(=φM/2、4MHz時2MHz)をポーリングでエッジ検出する。
// 割り込みは許可したままなので tick 等で稀にビットを落とすが、
// SH1 エッジ同期のため次ワードで復帰する(軽微なクラックルのみ)。
// 信号の役割は配線固定(自動判別は起動ごとに揺れて誤判定することが
// あったため廃止し、実測で確定した割り当てを PIN_YM_* に固定)。

static void ymCaptureLoop(void*) {
  // Dedicated GPIO: G4/G5/G6 を CPU 直結バンドル(bit0/1/2)にして1サイクルで読む。
  // 通常の GPIO_IN レジスタ読み(APB経由 ~150ns)では φ1=2MHz の
  // 全エッジを捕捉しきれない。バンドルは使用するコア(Core 0)で作ること。
  static dedic_gpio_bundle_handle_t bundle = NULL;
  if (bundle == NULL) {
    // バンドルは「配列順=ビット順」。1<<(pin-4) の変換が成り立つよう
    // 必ず GPIO 番号昇順・欠番なし(G4-G8)で並べること
    const int pins[5] = {4, 5, 6, 7, 8};
    dedic_gpio_bundle_config_t cfg = {};
    cfg.gpio_array = (int*)pins;
    cfg.array_size = 5;
    cfg.flags.in_en = 1;
    dedic_gpio_new_bundle(&cfg, &bundle);
  }
  // 13bitワード(sr)→ 9bit デューティ。フル精度デコード+1次ノイズシェーピング。
  // 量子化ノイズが高域に移り、後段の RC フィルタで削れるので実効 S/N が上がる。
  auto decodeDuty = [](uint16_t m, uint16_t e, int32_t &nsErr) -> int32_t {
    int32_t v = 2 * (int32_t)m - 1023;            // ±1023 (YM3012式の半LSB中心化)
    int32_t pcm = (v * 32) >> (7 - (e ? e : 1));  // ±32736 (16bit相当)。負数<<はUBなので乗算
    int32_t acc = (pcm + 32768) + nsErr;
    int32_t duty = acc >> 7;                      // 16bit -> 9bit
    if (duty < 0) duty = 0; else if (duty > 511) duty = 511;
    nsErr = acc - (duty << 7);
    if (nsErr > 127) nsErr = 127; else if (nsErr < -128) nsErr = -128;  // アンチワインドアップ
    return duty;
  };
  for (;;) {
    if (!ymCaptureRun) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    // バンドルビット(1<<(pin-4))。割り当ては配線固定
    const uint32_t SO  = 1UL << (PIN_YM_SO - 4);
    const uint32_t SH  = 1UL << (PIN_YM_SH1 - 4);
    const uint32_t P1  = 1UL << (PIN_YM_PHI1 - 4);
    const uint32_t SH2 = 1UL << (PIN_YM_SH2 - 4);
    uint32_t prev = dedic_gpio_cpu_ll_read_in();
    uint32_t sr = 0;   // 32bit シフトレジスタ(Lのオフセット実験用に余裕を持たせる)
    // 割り込みは止めない(IWDT/クラッシュ回避)。tick 等でビットを落とした
    // 区間は φ1 エッジ数の検証で検出して捨てる(直前デューティ保持=聴感上無音)。
    // ステレオ時は SH2→SH1 間が16クロック。SH2未配線(モノラル)なら
    // SH1→SH1 の32クロックで従来どおり動く。
    uint16_t cnt = 0;
    int32_t nsErrR = 0, nsErrL = 0;
    // 3タップ・メディアン(1サンプルの飛び値=EMI起因のスパイク除去)
    int32_t mR1 = 256, mR2 = 256, mL1 = 256, mL2 = 256;
    auto med3 = [](int32_t a, int32_t b, int32_t c) -> int32_t {
      int32_t lo = a < b ? a : b, hi = a < b ? b : a;
      return c < lo ? lo : (c > hi ? hi : c);
    };
    while (ymCaptureRun) {
      uint32_t in = dedic_gpio_cpu_ll_read_in();
      uint32_t chg = in ^ prev;
      if ((chg & P1) && (in & P1)) {          // φ1 立ち上がり: SO を取り込み
        sr = (sr >> 1) | ((in & SO) ? 0x80000000UL : 0);
        cnt++;
      }
      if ((chg & SH) && !(in & SH)) {         // SH1 立ち下がり = RIGHT ch 確定
        ymFrameCount++;
        if (cnt < ymP1Min) ymP1Min = cnt;
        if (cnt > ymP1Max) ymP1Max = cnt;
        // 検証窓の根拠: エッジ欠落のほぼ全ては直前ラッチ処理中(=スロット
        // 先頭の捨てビット3個の区間)に起きるため 13 まで許容しても語は無傷。
        // まれな割り込み起因のデータ部欠落は e==0 検査と median-3 で吸収する。
        // 17/33 は余分な1ビットがワードの後に入った状態なので抽出位置を+1補正
        if ((cnt >= 13 && cnt <= 17) || (cnt >= 29 && cnt <= 33)) {
          int kr = ymROff + ((cnt == 17 || cnt == 33) ? 1 : 0);
          uint16_t m = (sr >> (19 - kr)) & 0x3FF; // 仮数 (B0 が LSB、B9=符号)
          uint16_t e = (sr >> (29 - kr)) & 0x07;  // 指数 (S0 が LSB)
          if (e == 0) {                           // YM3012仕様で指数000は禁止=化けたワード
            ymBadExp++;
          } else {
            int32_t duty = decodeDuty(m, e, nsErrR);
            int32_t out = duty;
            if (ymMedianEn) {
              out = med3(mR2, mR1, duty);
              int32_t d = out - duty;
              if (d > 24 || d < -24) ymMedFixR++;  // スパイク級の差し替えのみ計数
            }
            mR2 = mR1; mR1 = duty;   // 履歴は無効時も更新(再有効化時の汚染防止)
            duty = out;
            if (ymOutMuteR) duty = 256;   // R出力ミュート(中点固定)
            ledc_ll_set_duty_int_part(&LEDC, LEDC_LOW_SPEED_MODE, YM_PWM_CH_R, duty);
            ledc_ll_set_duty_start(&LEDC, LEDC_LOW_SPEED_MODE, YM_PWM_CH_R, true);
            ledc_ll_ls_channel_update(&LEDC, LEDC_LOW_SPEED_MODE, YM_PWM_CH_R);
            ymSampleCount++;
            ymLastRaw = (uint16_t)(sr >> 16);
            if (ymRawArm) { ymRawBuf[ymRawIdx++ & 63] = sr; ymRawArm--; }
            if (duty < ymDutyMin) ymDutyMin = duty;
            if (duty > ymDutyMax) ymDutyMax = duty;
          }
        }
        cnt = 0;
      }
      if ((chg & SH2) && !(in & SH2)) {       // SH2 立ち下がり = LEFT ch 確定
        if (cnt >= 13 && cnt <= 17) {
          int k = ymLOff + ((cnt == 17) ? 1 : 0); // 17は余分1ビットぶん抽出位置を補正
          uint16_t m = (sr >> (19 - k)) & 0x3FF;
          uint16_t e = (sr >> (29 - k)) & 0x07;
          if (e == 0) {
            ymBadExp++;
          } else {
            int32_t duty = decodeDuty(m, e, nsErrL);
            int32_t out = duty;
            if (ymMedianEn) {
              out = med3(mL2, mL1, duty);
              int32_t d = out - duty;
              if (d > 24 || d < -24) ymMedFixL++;
            }
            mL2 = mL1; mL1 = duty;
            duty = out;
            if (ymOutMuteL) duty = 256;   // L出力ミュート(中点固定)
            ledc_ll_set_duty_int_part(&LEDC, LEDC_LOW_SPEED_MODE, YM_PWM_CH_L, duty);
            ledc_ll_set_duty_start(&LEDC, LEDC_LOW_SPEED_MODE, YM_PWM_CH_L, true);
            ledc_ll_ls_channel_update(&LEDC, LEDC_LOW_SPEED_MODE, YM_PWM_CH_L);
            ymSampleCountL++;
            ymLastRawL = (uint16_t)(sr >> 16);
            if (duty < ymDutyMinL) ymDutyMinL = duty;
            if (duty > ymDutyMaxL) ymDutyMaxL = duty;
          }
        }
        cnt = 0;
      }
      prev = in;
    }
  }
}

static void ymAudioStart() {
  digitalWrite(PIN_OE_CHR, LOW);        // U7 有効化 → φ1/SO/SH1/SH2 が G4-G8 に届く
  delayMicroseconds(10);

  // 信号割り当ては配線固定(PIN_YM_* 参照)。自動判別は廃止。
  Serial.printf("YMMAP fixed: phi1=MD%d so=MD%d sh1=MD%d sh2=MD%d\n",
                PIN_YM_PHI1 - 4, PIN_YM_SO - 4, PIN_YM_SH1 - 4, PIN_YM_SH2 - 4);

  ledcAttachChannel(PIN_PWM_R, PWM_FREQ, PWM_RES, YM_PWM_CH_R);
  ledcAttachChannel(PIN_PWM_L, PWM_FREQ, PWM_RES, YM_PWM_CH_L);
  ledcWrite(PIN_PWM_R, 256);            // 無音(中点)
  ledcWrite(PIN_PWM_L, 256);
  ymCaptureRun = true;
}

static void ymAudioStop() {
  ymCaptureRun = false;
  delay(2);
  ledcDetach(PIN_PWM_R);
  ledcDetach(PIN_PWM_L);
  pinMode(PIN_PWM_R, OUTPUT);           // PPU /RD / /WR をバスアイドル(High)へ戻す
  pinMode(PIN_PWM_L, OUTPUT);
  digitalWrite(PIN_PWM_R, HIGH);
  digitalWrite(PIN_PWM_L, HIGH);
  digitalWrite(PIN_OE_CHR, HIGH);
}

// PWM 出力(R=G2/エッジ17、L=G3/エッジ47)の配線チェック用。
// 880Hz テストトーンを両chに1.5秒出す。
static void toneTest() {
  bool wasRunning = ymCaptureRun;
  if (wasRunning) ymAudioStop();
  // R=880Hz / L=440Hz の別トーンを3秒。左右の配線・音量差の切り分け用。
  // ch0/1 は同一タイマーなので周波数を分けるため L はタイマー2/ch4 を使う
  ledcAttachChannel(PIN_PWM_R, 880, 10, YM_PWM_CH_R);
  ledcAttachChannel(PIN_PWM_L, 440, 10, LEDC_CHANNEL_4);
  ledcWrite(PIN_PWM_R, 64);   // 6% duty の小音量
  ledcWrite(PIN_PWM_L, 64);
  delay(3000);
  ledcDetach(PIN_PWM_R);
  ledcDetach(PIN_PWM_L);
  pinMode(PIN_PWM_R, OUTPUT); digitalWrite(PIN_PWM_R, HIGH);
  pinMode(PIN_PWM_L, OUTPUT); digitalWrite(PIN_PWM_L, HIGH);
  if (wasRunning) ymAudioStart();
}

// 信号診断: G4(SO)/G5(SH1)/G6(φ1) のエッジ数を100ms数える。
// YM2151 が生きていれば無音でも φ1≒358k/100ms・SH1≒11k/100ms トグルする
// (ポーリングなので実測値は取りこぼしで少なめに出る。0か非0かが重要)。
static void ymDiag() {
  int oeWas = digitalRead(PIN_OE_CHR);
  digitalWrite(PIN_OE_CHR, LOW);           // U7 を通す
  delayMicroseconds(10);
  uint32_t p1 = 0, sh = 0, so = 0;
  uint32_t prev = REG_READ(GPIO_IN_REG);
  uint32_t t0 = millis();
  while (millis() - t0 < 100) {
    uint32_t in = REG_READ(GPIO_IN_REG);
    uint32_t chg = in ^ prev;
    if (chg & (1UL << PIN_YM_PHI1)) p1++;
    if (chg & (1UL << PIN_YM_SH1))  sh++;
    if (chg & (1UL << PIN_YM_SO))   so++;
    prev = in;
  }
  if (oeWas) digitalWrite(PIN_OE_CHR, HIGH);
  Serial.printf("DIAG clk=%d p1=%lu sh1=%lu so=%lu samples=%lu/%lu p1cnt=%u..%u last=%04X duty=%d..%d\n",
                ymClockOn ? 1 : 0,
                (unsigned long)p1, (unsigned long)sh, (unsigned long)so,
                (unsigned long)ymSampleCount, (unsigned long)ymFrameCount,
                ymP1Min, ymP1Max, ymLastRaw,
                ymDutyMin, ymDutyMax);
  Serial.printf("DIAG L: samples=%lu last=%04X duty=%d..%d\n",
                (unsigned long)ymSampleCountL, ymLastRawL, ymDutyMinL, ymDutyMaxL);
  Serial.printf("DIAG med=%d fixR=%lu fixL=%lu badExp=%lu\n",
                ymMedianEn ? 1 : 0, (unsigned long)ymMedFixR,
                (unsigned long)ymMedFixL, (unsigned long)ymBadExp);
  ymMedFixR = 0; ymMedFixL = 0; ymBadExp = 0;
  ymDutyMin = 32767; ymDutyMax = -32768;   // 次回に向けてリセット
  ymDutyMinL = 32767; ymDutyMaxL = -32768;
  ymP1Min = 0xFFFF; ymP1Max = 0;

  // MD0-7(PPU データバス 8本)全部のエッジ数。SO/SH1/φ1 が想定外の
  // CHR ROM ピンに配線されていた場合、別のビットに活動が現れる。
  digitalWrite(PIN_OE_CHR, LOW);
  delayMicroseconds(10);
  uint32_t cnt[8] = {0};
  prev = REG_READ(GPIO_IN_REG);
  t0 = millis();
  while (millis() - t0 < 100) {
    uint32_t in = REG_READ(GPIO_IN_REG);
    uint32_t chg = in ^ prev;
    for (int i = 0; i < 8; i++)
      if (chg & (1UL << PIN_D[i])) cnt[i]++;
    prev = in;
  }
  if (oeWas) digitalWrite(PIN_OE_CHR, HIGH);
  Serial.printf("DIAG MD0-7 edges:");
  for (int i = 0; i < 8; i++) Serial.printf(" %lu", (unsigned long)cnt[i]);
  Serial.print("\n");

  // MD0-2 のデューティ比(High率)。信号の種別判定用:
  //   φ1 ≒50% / SH1 ≒ 数%〜15% / SO は無音時 ≒10%前後(データ依存)
  digitalWrite(PIN_OE_CHR, LOW);
  delayMicroseconds(10);
  uint32_t hi0 = 0, hi1 = 0, hi2 = 0;
  const uint32_t N = 200000;
  for (uint32_t i = 0; i < N; i++) {
    uint32_t in = REG_READ(GPIO_IN_REG);
    if (in & (1UL << PIN_D[0])) hi0++;
    if (in & (1UL << PIN_D[1])) hi1++;
    if (in & (1UL << PIN_D[2])) hi2++;
  }
  if (oeWas) digitalWrite(PIN_OE_CHR, HIGH);
  Serial.printf("DIAG duty MD0=%lu%% MD1=%lu%% MD2=%lu%% (phi1=50%%, SH1/SO=low)\n",
                (unsigned long)(hi0 * 100 / N), (unsigned long)(hi1 * 100 / N),
                (unsigned long)(hi2 * 100 / N));

  // /IC を押さえたまま MD0 を測る。φ1 はリセット中も走り続けるが、
  // SO/SH1 はリセット中は停止するはず → MD0 の正体を確定できる。
  if (ymClockOn) {
    srWrite32(srCpuAddr(YM_WR_N));            // /IC=L
    delay(2);
    digitalWrite(PIN_OE_CHR, LOW);
    delayMicroseconds(10);
    uint32_t icEdges = 0, icHi = 0;
    prev = REG_READ(GPIO_IN_REG);
    t0 = millis();
    while (millis() - t0 < 50) {
      uint32_t in = REG_READ(GPIO_IN_REG);
      if ((in ^ prev) & (1UL << PIN_D[0])) icEdges++;
      if (in & (1UL << PIN_D[0])) icHi++;
      prev = in;
    }
    srWrite32(srCpuAddr(YM_WR_N | YM_IC_N));  // /IC 解除
    delay(2);
    if (oeWas) digitalWrite(PIN_OE_CHR, HIGH);
    Serial.printf("DIAG IC-hold MD0 edges/50ms=%lu (toggling=phi1, still=SO)\n",
                  (unsigned long)icEdges);
  }

  // M2(G42) の読み戻しで φM が実際に出ているか確認。
  // IO_MUX の入力イネーブルだけ立てるので LEDC 出力は壊さない。
  gpio_ll_input_enable(&GPIO, (gpio_num_t)PIN_M2);
  uint32_t m2edges = 0;
  uint32_t prev1 = REG_READ(GPIO_IN1_REG);
  t0 = millis();
  while (millis() - t0 < 20) {
    uint32_t in1 = REG_READ(GPIO_IN1_REG);
    if ((in1 ^ prev1) & (1UL << (PIN_M2 - 32))) m2edges++;
    prev1 = in1;
  }
  Serial.printf("DIAG M2(phiM) edges/20ms=%lu (expect >50000 if clocking)\n",
                (unsigned long)m2edges);

  // 音声PWM(R=G2)の出力エッジも数える。YMモード中なら約3100/20ms 出るはず。
  gpio_ll_input_enable(&GPIO, (gpio_num_t)PIN_PWM_R);
  uint32_t pwmEdges = 0;
  prev = REG_READ(GPIO_IN_REG);
  t0 = millis();
  while (millis() - t0 < 20) {
    uint32_t in0 = REG_READ(GPIO_IN_REG);
    if ((in0 ^ prev) & (1UL << PIN_PWM_R)) pwmEdges++;
    prev = in0;
  }
  Serial.printf("DIAG PWM-R(G2) edges/20ms=%lu (expect ~3100 in YM mode)\n",
                (unsigned long)pwmEdges);

  // 音声PWM(L=G41)の出力エッジ。G41は32以上なので GPIO_IN1 で読む
  gpio_ll_input_enable(&GPIO, (gpio_num_t)PIN_PWM_L);
  uint32_t pwmEdgesL = 0;
  uint32_t prevL = REG_READ(GPIO_IN1_REG);
  t0 = millis();
  while (millis() - t0 < 20) {
    uint32_t in1 = REG_READ(GPIO_IN1_REG);
    if ((in1 ^ prevL) & (1UL << (PIN_PWM_L - 32))) pwmEdgesL++;
    prevL = in1;
  }
  Serial.printf("DIAG PWM-L(G41) edges/20ms=%lu (expect ~3100 in YM mode)\n",
                (unsigned long)pwmEdgesL);
}

static bool ymInit();       // 前方宣言
static bool playbackInputCheck();
static void ymToggleMute(int ch);
static bool ymDoKey(int c);
static void ymClockStop();
static void ymWriteReg(uint8_t reg, uint8_t val);

// 内蔵曲(ys2_song.h)を1回再生する。シリアル入力で中断。
static void playEmbedded() {
  if (!ymClockOn && !ymInit()) { ledError(); return; }
  ledBusy();
  uint32_t next = micros();
  const uint8_t *p = EMBEDDED_SONG;
  const uint8_t *end = EMBEDDED_SONG + sizeof(EMBEDDED_SONG);
  while (p + 4 <= end && !playbackInputCheck()) {
    uint16_t dt = p[0] | ((uint16_t)p[1] << 8);
    if (dt == 0xFFFF && p[2] == 0xFF && p[3] == 0xFF) break;
    next += (uint32_t)dt * 100;
    while ((int32_t)(next - micros()) > 0) {
      if ((int32_t)(next - micros()) > 2000) delay(1);
    }
    if (p[2] != 0xFE) ymWriteReg(p[2], p[3]);
    p += 4;
  }
  for (int ch = 0; ch < 8; ch++) ymWriteReg(0x08, ch);  // 全chキーオフ
  ledReady();
}

// --- WiFi SoftAP + Web UI ---
// SSID "YM2151" / pass "ym2151jukebox" / http://192.168.4.1
// 操作は仮想キー注入なので X ストリーム再生中でも効く。
static WebServer webServer(80);

static const char WEB_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>YM2151 Jukebox</title><style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:8px}
h1{font-size:1.2em;color:#8cf}
button{font-size:1.1em;margin:4px;padding:10px 14px;border-radius:8px;border:0;background:#345;color:#fff}
button.on{background:#2a7}button.off{background:#833}
.big{font-size:1.4em;padding:14px 22px}
#st{color:#9f9;font-size:.9em;margin-top:8px;white-space:pre}
</style></head><body>
<h1>YM2151 JUKEBOX</h1>
<div><button class="big" onclick="k('P')">&#9654; PLAY</button>
<button class="big" onclick="k('S')">&#9632; STOP</button></div>
<div id="ch"></div>
<div><button onclick="k('A')">ALL ON</button>
<button id="L" onclick="k('L')">L</button>
<button id="R" onclick="k('R')">R</button></div>
<div><button onclick="k('-')">PITCH -</button><span id="clk">-</span>
<button onclick="k('+')">PITCH +</button></div>
<div id="st"></div>
<script>
for(let i=1;i<=8;i++){let b=document.createElement('button');b.id='c'+i;b.textContent='CH'+i;
b.onclick=()=>k(String(i));document.getElementById('ch').appendChild(b);}
function k(c){fetch('/k?c='+encodeURIComponent(c)).then(upd)}
function upd(){fetch('/status').then(r=>r.json()).then(j=>{
document.getElementById('clk').textContent=(j.clk/1e6).toFixed(2)+'MHz';
for(let i=1;i<=8;i++)document.getElementById('c'+i).className=(j.mute>>(i-1))&1?'off':'on';
document.getElementById('L').className=j.outL?'on':'off';
document.getElementById('R').className=j.outR?'on':'off';
document.getElementById('st').textContent=(j.on?'YM ON':'YM OFF')+'  samples='+j.smp;
})}
setInterval(upd,2000);upd();
</script></body></html>)HTML";

static void webTask(void*) {
  webServer.on("/", []() {
    webServer.send_P(200, "text/html", WEB_PAGE);
  });
  webServer.on("/k", []() {
    String c = webServer.arg("c");
    if (c.length() == 1) pushVKey((uint8_t)c[0]);
    webServer.send(200, "text/plain", "ok");
  });
  webServer.on("/status", []() {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"clk\":%lu,\"mute\":%u,\"outL\":%d,\"outR\":%d,\"on\":%d,\"smp\":%lu}",
             (unsigned long)ymClockHz, ymMuteMask,
             ymOutMuteL ? 1 : 0, ymOutMuteR ? 1 : 0,
             ymClockOn ? 1 : 0, (unsigned long)ymSampleCount);
    webServer.send(200, "application/json", buf);
  });
  webServer.begin();
  for (;;) { webServer.handleClient(); vTaskDelay(pdMS_TO_TICKS(3)); }
}

// WiFi はオンデマンド起動(W コマンド)。常時ONだと送信スパイクの電流で
// カート接続時に5Vがサグしブートループに陥るため、必要時のみ点ける。
static bool wifiOn = false;
static bool webTaskStarted = false;
static void toggleWifi() {
  if (!wifiOn) {
    WiFi.softAP("YM2151", "ym2151jukebox");
    WiFi.setTxPower(WIFI_POWER_7dBm);   // 電流スパイク低減(同室内なら十分)
    if (!webTaskStarted) {
      xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 1);
      webTaskStarted = true;
    }
    wifiOn = true;
    Serial.print("WIFI ON  SSID=YM2151 pass=ym2151jukebox http://192.168.4.1\n");
  } else {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiOn = false;
    Serial.print("WIFI OFF\n");
  }
}

// φM をリアルタイム変更する。変わるのは主にピッチ(イベント時刻は micros()
// 基準なので曲の進行速度は不変)。100kHz〜4.5MHz にクランプ。
// キャプチャはSHエッジ同期なので自動追従する。
static void ymClockSet(uint32_t hz) {
  if (hz < 100000) hz = 100000;
  if (hz > 4500000) hz = 4500000;
  if (ymClockOn) {
    if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3, hz) != ESP_OK) {
      Serial.print("ERR CLK\n");   // 失敗時は内部状態を更新しない
      return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
    ymClockHz = hz;
    ymBusyUs = 68000000UL / hz + 5;
    Serial.printf("CLK %lu\n",
                  (unsigned long)ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3));
  } else {
    ymClockHz = hz;                  // 停止中は次回 ymClockStart で反映
    ymBusyUs = 68000000UL / hz + 5;
    Serial.printf("CLK %lu (stored)\n", (unsigned long)hz);
  }
}

// 再生ループ内の入力処理: +/-(と直後の改行)はその場でクロック操作として
// 消費し、それ以外の入力が来たら true を返す(=再生停止要求)。
// これにより E/Q の再生を止めずにピッチを動かせる。X ストリーム中は
// バイナリレコードと区別できないため対象外(ホスト側から操作する)。
static bool playbackInputCheck() {
  int v;
  while ((v = popVKey()) >= 0) {          // Web UI からの仮想キー
    if (v == 'S') { ymStopReq = true; return true; }
    if (v == 'Q') { ymQuitReq = true; return true; }
    if (v == 'P') { ymRestartReq = true; return true; }
    ymDoKey(v);
  }
  while (Serial.available()) {
    int c = Serial.peek();
    if (c == '\r' || c == '\n') { Serial.read(); continue; }  // 即時キー後の改行
    if (c == 'S') { Serial.read(); ymStopReq = true; return true; }     // 曲停止
    if (c == 'Q') { Serial.read(); ymQuitReq = true; return true; }     // モード終了
    if (c == 'P') { Serial.read(); ymRestartReq = true; return true; }  // 頭から
    if (ymDoKey(c)) { Serial.read(); continue; }
    return true;   // それ以外は通常コマンド → 再生停止して行パーサへ渡す
  }
  return false;
}

// 内蔵曲の再生制御ラッパ: P で頭から再生し直し、S で完全停止する
static void playEmbeddedControlled() {
  do {
    ymRestartReq = false;
    Serial.print("PLAY\n");
    playEmbedded();
  } while (ymRestartReq);
  if (ymQuitReq) {                 // Q: YMモード完全終了(自動では戻らない)
    ymQuitReq = false;
    ymClockStop();
    busIdle();
    Serial.print("QUIT\n");
  } else if (ymStopReq) {          // S: 曲停止のみ(YMモード維持、Pで再開可)
    ymStopReq = false;
    Serial.print("STOP\n");
  }
}

// φM を止めて M2 を通常の GPIO(High) に戻す。カートリッジコマンドと共存するため。
static void ymClockStop() {
  if (!ymClockOn) return;
  ymAudioStop();
  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 1);
  ymClockOn = false;
  pinMode(PIN_M2, OUTPUT);
  digitalWrite(PIN_M2, HIGH);
}

// BUSY は読めないので最悪値で待つ。BUSY 期間は φM 68サイクル ≒ 19µs。
static void ymWaitBusy() { delayMicroseconds(ymBusyUs); }  // BUSY=68φMサイクル+マージン

// データと A0 をアドレス線に確定させ、A9 で /WR パルスを作る。
// セットアップ/ホールドは srWrite32 の所要時間(数µs)で自然に満たされる。
static void ymWriteBus(bool a0, uint8_t v) {
  uint16_t base = (uint16_t)v | (a0 ? 0x100 : 0) | YM_IC_N;
  srWrite32(srCpuAddr(base | YM_WR_N));  // データ確定、/WR=H
  srWrite32(srCpuAddr(base));            // /WR=L
  srWrite32(srCpuAddr(base | YM_WR_N));  // /WR 立ち上がりで取り込み
}

// トラック(FMチャンネル)ミュート: RLイネーブル($20-$27 bit7-6)を横取りして
// ミュート中のchはRL=00で書く。エンベロープ等は走り続けるので復帰も自然。
static uint8_t ymRegRLShadow[8] = {0};       // 各chの $20+ch 最終書き込み値

static void ymWriteReg(uint8_t reg, uint8_t val) {
  if (reg >= 0x20 && reg <= 0x27) {
    ymRegRLShadow[reg & 7] = val;            // 原値を保存してから
    if (ymMuteMask & (1 << (reg & 7))) val &= 0x3F;  // ミュート中はRLを落とす
  }
  ymWriteBus(false, reg);   // A0=0: アドレス (BUSYは立たないので待ち不要)
  ymWriteBus(true, val);    // A0=1: データ
  ymWaitBusy();             // データ書き込み後のみ BUSY 待ち
}

// ミュート切替(数字キー1-8)。即時にRLを書き換えて反映する
static void ymToggleMute(int ch) {
  ymMuteMask ^= (1 << ch);
  Serial.printf("CH%d %s\n", ch + 1, (ymMuteMask & (1 << ch)) ? "OFF" : "ON");
  if (ymClockOn) ymWriteReg(0x20 + ch, ymRegRLShadow[ch]);
}

static void ymPrintHelp() {
  Serial.print(
    "-- YM keys (YMモード中・改行不要) --\n"
    " 1-8 : FMチャンネル ミュートトグル\n"
    " A   : 全チャンネル ミュート解除\n"
    " L/R : 左/右出力 トグル\n"
    " +/- : phiM +-50kHz (ピッチ)\n"
    " P   : 内蔵曲を頭から再生\n"
    " S   : 曲停止 (YMモード維持、Pで再開)\n"
    " Q   : Quit = YMモード完全終了\n"
    " H   : このヘルプ\n");
}

// YMモード中の即時キー(+,-,1-8,H,L,R,A)。処理したら true。
// S/P は再生ループ制御に関わるため呼び出し側で扱う。
static bool ymDoKey(int c) {
  if (c == '+' || c == '-') {
    ymClockSet(ymClockHz + (c == '+' ? 50000 : -50000));
    return true;
  }
  if (c >= '1' && c <= '8') { ymToggleMute(c - '1'); return true; }
  if (c == 'H') { ymPrintHelp(); return true; }
  if (c == 'A') {
    ymMuteMask = 0;
    if (ymClockOn)
      for (int ch = 0; ch < 8; ch++) ymWriteReg(0x20 + ch, ymRegRLShadow[ch]);
    Serial.print("ALL CH ON\n");
    return true;
  }
  if (c == 'L') {
    ymOutMuteL = !ymOutMuteL;
    Serial.printf("OUT-L %s\n", ymOutMuteL ? "OFF" : "ON");
    return true;
  }
  if (c == 'R') {
    ymOutMuteR = !ymOutMuteR;
    Serial.printf("OUT-R %s\n", ymOutMuteR ? "OFF" : "ON");
    return true;
  }
  return false;
}

// φM 供給開始 + /IC リセット。YM2151 はリセット中もクロックが必要。
// クロック設定に失敗したら false を返し、YMモードには入らない。
static bool ymInit() {
  busIdle();
  if (!ymClockStart()) return false;
  srWrite32(srCpuAddr(YM_WR_N));           // /IC=L (A10=0)、/WR=H
  delay(2);                                // 最低 100µs 以上
  srWrite32(srCpuAddr(YM_WR_N | YM_IC_N)); // /IC 解除
  delay(2);
  ymAudioStart();                          // YM3012 シミュレーション開始
  return true;
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
  while (!playbackInputCheck()) {   // 次のコマンドが来るまでループ(+/-は素通し)
    for (int i = 0; i < 8 && !playbackInputCheck(); i++) {
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
  // キャプチャ中は Core 0 の idle が回らないためタスク WDT を無効化する。
  // 【既知のトレードオフ】idle0 だけ購読解除する方法は、core 3.x の idle
  // フックが解除後も esp_task_wdt_reset() を呼び続けて "task not found" を
  // 洪水させ、esp_log_level_set でも止められなかったため断念。全体 deinit は
  // 他タスクの見張りも失うが、本機はハング時に物理リセットで復旧できる
  // 用途のため実害よりログ汚染の方が問題と判断した。
  esp_task_wdt_deinit();
  xTaskCreatePinnedToCore(ymCaptureLoop, "ymcap", 4096, nullptr, 3, nullptr, 0);
  Serial.setRxBufferSize(8192);  // X コマンドのストリーム受信用に拡大
  Serial.begin(115200);
  // WiFi は自動起動しない(W コマンドでオンデマンド起動)。
  // 常時ONだとカート接続時に電流スパイクでブートループする事故があった
  // 予期しないリセットの診断用(1=PowerOn 3=SW 4=Panic 5=IntWdt 6=TaskWdt
  // 7=WdtOther 8=DeepSleep 9=Brownout 10=SDIO)
  Serial.printf("RST reason=%d\n", (int)esp_reset_reason());
  // 起動確認の短いSE(ピロリ♪)を両ch PWM で鳴らす。
  // デューティを3%に絞って音量を下げる(50%だとフルスイングで大きすぎる)
  static const uint16_t bootSe[3] = {1319, 1760, 2093};  // E6 A6 C7
  ledcAttachChannel(PIN_PWM_R, bootSe[0], 10, YM_PWM_CH_R);
  ledcAttachChannel(PIN_PWM_L, bootSe[0], 10, YM_PWM_CH_L);
  for (int i = 0; i < 3; i++) {
    ledcChangeFrequency(PIN_PWM_R, bootSe[i], 10);  // ch0/1は同一タイマーなので両chに効く
    ledcWrite(PIN_PWM_R, 12);   // 12/1024 ≒ 1.2% duty(控えめな起動音)
    ledcWrite(PIN_PWM_L, 12);
    delay(70);
  }
  ledcDetach(PIN_PWM_R);
  ledcDetach(PIN_PWM_L);
  pinMode(PIN_PWM_R, OUTPUT); digitalWrite(PIN_PWM_R, HIGH);
  pinMode(PIN_PWM_L, OUTPUT); digitalWrite(PIN_PWM_L, HIGH);
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
  // 電源ONから2秒以内にシリアル入力がなければ、内蔵曲(Ys2 エンディング2)を
  // ループ再生する(曲間3秒)。シリアル入力で停止して通常コマンドモードへ。
  static bool autoPlayChecked = false;
  if (!autoPlayChecked) {
    if (Serial.available()) {
      autoPlayChecked = true;
    } else if (millis() > 2000) {
      autoPlayChecked = true;
      while (!playbackInputCheck()) {
        playEmbeddedControlled();
        if (!ymClockOn) break;   // S で完全停止された
        for (int i = 0; i < 30 && !playbackInputCheck(); i++) delay(100);
      }
      ymClockStop();
      busIdle();
    } else {
      return;
    }
  }

  // Web UI からの仮想キー(アイドル時)。P は再生開始として扱う
  {
    int v;
    while ((v = popVKey()) >= 0) {
      if (v == 'P') { playEmbeddedControlled(); }
      else if (v == 'S') {
        if (ymClockOn) {
          for (int ch = 0; ch < 8; ch++) ymWriteReg(0x08, ch);
          Serial.print("STOP\n");
        }
      } else if (v == 'Q') {
        if (ymClockOn) {
          for (int ch = 0; ch < 8; ch++) ymWriteReg(0x08, ch);
          ymClockStop(); busIdle(); Serial.print("QUIT\n");
        }
      } else ymDoKey(v);
    }
  }

  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    // YMモード中の即時キー(改行不要): +,-,1-8,H,L,R,A,P,S。
    // 非YMモード時は従来の行コマンド(A=トーン, S=ステータス, R=PRG読み等)を邪魔しない
    if (line.length() == 0 && ymClockOn) {
      if (c == 'P') { playEmbeddedControlled(); continue; }
      if (c == 'S') {
        for (int ch = 0; ch < 8; ch++) ymWriteReg(0x08, ch);  // 全chキーオフ
        Serial.print("STOP\n");                              // YMモードは維持
        continue;
      }
      if (c == 'Q') {
        for (int ch = 0; ch < 8; ch++) ymWriteReg(0x08, ch);
        ymClockStop(); busIdle(); Serial.print("QUIT\n");    // 完全終了
        continue;
      }
      if (ymDoKey(c)) continue;
    }
    if (line.length() == 0 && (c == '+' || c == '-')) {   // +/- はYM外でも保存だけ効く
      ymClockSet(ymClockHz + (c == '+' ? 50000 : -50000));
      continue;
    }
    if (c != '\n') {
      if (c != '\r') line += c;
      continue;
    }
    char cmd = line.length() ? line[0] : 0;
    uint32_t addr = 0, len = 0;
    sscanf(line.c_str() + 1, "%lx %lx", (unsigned long*)&addr, (unsigned long*)&len);
    long decArg = atol(line.c_str() + 1);   // K コマンド用(10進)
    line = "";
    // カートリッジ系コマンドは YM モード(M2=クロック出力)と両立しないので、
    // 実行前に自動で YM モードを解除して通常のダンパー状態へ戻す。
    if (cmd && ymClockOn && strchr("RCWMTSBP", cmd)) ymClockStop();
    switch (cmd) {
      case 'V': Serial.printf("famidump v0.6 rev%d rst=%d\n", BOARD_REV, (int)esp_reset_reason()); break;
      case 'R': handleRead('R', addr, len); break;
      case 'C': handleRead('C', addr, len); break;
      case 'W':
        if (addr == 0 && len == 0) toggleWifi();       // 引数なし: WiFiトグル
        else handleRead('W', addr, len);               // 引数あり: WRAM読み(従来)
        break;
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
        if (ymInit()) Serial.print("YMRDY\n");
        else { Serial.print("ERR YMCLOCK\n"); ledError(); }
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
      case 'D':
        ymDiag();
        break;
      // L チャンネルのデコード位置調整(SH2 位相差の実験用)。
      // O <hex 0-6> -> オフセット -3..+3 を設定し "LOFF n" を返す
      // R ラッチ時の生ワード64個をダンプ(アライメント解析用)。
      // 通常再生の負荷を避けるため、Gを受けた時だけ64サンプル記録する
      case 'G': {
        ymRawIdx = 0;      // 0..63 の時系列順で埋まるようリセットしてからアーム
        ymRawArm = 64;
        delay(50);         // 62.5kHz なら64サンプルは約1msで揃う
        uint32_t snap[64]; // 表示中の上書きを防ぐスナップショット
        for (int i = 0; i < 64; i++) snap[i] = ymRawBuf[i];
        for (int i = 0; i < 64; i++) {
          Serial.printf("%08lX%c", (unsigned long)snap[i], (i % 8 == 7) ? '\n' : ' ');
        }
        Serial.print("GDONE\n");
        break;
      }
      // φM クロックのリアルタイム変更。K <kHz>(10進、例: K 3579)。
      // K 単独で現在値表示。行頭の +/- キーでも ±50kHz(改行不要)
      case 'K':
        if (decArg > 0) ymClockSet((uint32_t)decArg * 1000);
        else Serial.printf("CLK %lu\n", (unsigned long)ymClockHz);
        break;
      // メディアンフィルタの有効/無効(効果の A/B 比較用)
      case 'N':
        ymMedianEn = (addr != 0);
        Serial.printf("MEDIAN %d\n", ymMedianEn ? 1 : 0);
        break;
      case 'O': {
        int offL = (int)(addr & 7) - 3;
        int offR = (int)(len & 7) - 3;
        // -3 だと 29-(-3)=32 の右シフトが未定義動作になるため -2 までに制限
        if (offL < -2) offL = -2; if (offL > 3) offL = 3;
        if (offR < -2) offR = -2; if (offR > 3) offR = 3;
        ymLOff = offL;
        ymROff = offR;
        Serial.printf("LOFF %d ROFF %d\n", offL, offR);
        break;
      }
      // 内蔵曲(ys2_song.h)のスタンドアロン再生。USB 通信を一切使わないので、
      // ストリーミング再生との比較で「再生中の再起動」の切り分けにも使う。
      // 途中で止めるには何かコマンドを送る。
      case 'E':
        Serial.print("ESTART\n");
        playEmbeddedControlled();
        Serial.print("EDONE\n");
        break;
      // MDX 等のレジスタイベントストリーム再生。
      // "XSTART" 応答後、4バイトレコード [dt_lo][dt_hi][addr][data] を受信。
      //   dt: 直前イベントからの遅延 (100µs単位)。addr=0xFE は遅延のみ。
      //   dt=0xFFFF & addr=0xFF & data=0xFF で終了 -> "XDONE"
      // タイミングはファーム側で目標時刻方式で刻む(ホストはバッファを
      // 切らさず送るだけ。USB CDC のフロー制御が自然なペーシングになる)。
      case 'X': {
        if (!ymClockOn && !ymInit()) { Serial.print("ERR YMCLOCK\n"); ledError(); break; }
        Serial.print("XSTART\n");
        ledBusy();
        uint32_t next = micros();
        uint32_t consumed = 0;   // フロー制御: 1KB 消費ごとに 'K' を返す
        for (;;) {
          uint8_t rec[4];
          int got = 0;
          uint32_t tw = millis();
          while (got < 4) {
            if (Serial.available()) { rec[got++] = Serial.read(); tw = millis(); }
            else if (millis() - tw > 5000) { got = -1; break; }
          }
          if (got < 0) { Serial.print("XTIMEOUT\n"); break; }
          consumed += 4;
          if (consumed >= 1024) { consumed -= 1024; Serial.write('K'); }
          uint16_t dt = rec[0] | ((uint16_t)rec[1] << 8);
          if (dt == 0xFFFF && rec[2] == 0xFF && rec[3] == 0xFF) {
            Serial.print("XDONE\n");
            break;
          }
          next += (uint32_t)dt * 100;
          while ((int32_t)(next - micros()) > 0) {
            if ((int32_t)(next - micros()) > 2000) delay(1);
          }
          if (rec[2] != 0xFE) ymWriteReg(rec[2], rec[3]);
        }
        for (int ch = 0; ch < 8; ch++) ymWriteReg(0x08, ch);  // 全chキーオフ
        ledReady();
        break;
      }
      default:
        if (cmd) { Serial.print("ERR\n"); ledError(); }  // 不正コマンド=赤点滅(空行は無視)
        break;
    }
  }
}
