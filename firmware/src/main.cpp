// nes-rom-reader firmware (M5Stamp S3)
//
// ホストとはUSB CDCで通信。プロトコル(1行コマンド、応答はバイナリ):
//   V                      -> "famidump v0.3\n"
//   R <addr_hex> <len_hex> -> PRG読み出し。"OK <len>\n" + 生データ + "CRC xxxxxxxx\n"
//   C <addr_hex> <len_hex> -> CHR読み出し。同上
//   M                      -> ミラーリング判定 "H\n" or "V\n" or "?\n"
//   T                      -> セルフテスト。複数行レポートの最後に
//                             "SELFTEST PASS\n" または "SELFTEST FAIL\n"
//   B <addr_hex>           -> バンク選択(CNROM等)。指定アドレスへダミーライト
//                             サイクルを発生させ "BANK xxxx\n" を返す。
//                             バスコンフリクトを利用するため、addr には
//                             「選びたいバンク番号と同じ値が入っているPRGアドレス」
//                             を指定する(ホスト側でPRGダンプから検索)。
//
// データブロック直後の "CRC xxxxxxxx\n" は生データの CRC32
// (IEEE 802.3 / zlib.crc32 互換、8桁大文字hex)。ホスト側で照合する。

#include <Arduino.h>

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

static void busIdle() {
  digitalWrite(PIN_OE_PRG, HIGH);
  digitalWrite(PIN_OE_CHR, HIGH);
  digitalWrite(PIN_ROMSEL, HIGH);
  digitalWrite(PIN_M2, HIGH);
  digitalWrite(PIN_RW, HIGH);
  digitalWrite(PIN_PPU_RD, HIGH);
  digitalWrite(PIN_PPU_WR, HIGH);
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
  busIdle();
  srWrite32(SR_PPU_A13_N);  // アイドル時も PPU /A13=1 (PPU A13=0)
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
  Serial.printf("STATUS famidump-v0.3 mirror=%c pins=%s md=0x%02X\n",
                m, pins ? "PASS" : "FAIL", md);
}

static void handleRead(bool chr, uint32_t addr, uint32_t len) {
  ledBusy();  // 青: 読み出し中
  Serial.printf("OK %lX\n", (unsigned long)len);
  uint8_t buf[256];
  uint32_t crc = 0xFFFFFFFF;
  while (len > 0) {
    uint32_t n = min(len, (uint32_t)sizeof(buf));
    for (uint32_t i = 0; i < n; i++)
      buf[i] = chr ? readChr(addr + i) : readPrg(addr + i);
    crc = crc32Update(crc, buf, n);
    Serial.write(buf, n);
    addr += n;
    len -= n;
  }
  Serial.printf("CRC %08lX\n", (unsigned long)(crc ^ 0xFFFFFFFF));
  ledReady();  // 緑: 完了
}

void loop() {
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
    switch (cmd) {
      case 'V': Serial.print("famidump v0.3\n"); break;
      case 'R': handleRead(false, addr, len); break;
      case 'C': handleRead(true, addr, len); break;
      case 'M': Serial.printf("%c\n", detectMirroring()); break;
      case 'T': handleSelfTest(); break;
      case 'S': handleStatus(); break;
      case 'B': bankSelectWrite((uint16_t)addr); Serial.printf("BANK %04X\n", (unsigned)(addr & 0xFFFF)); break;
      default:  Serial.print("ERR\n"); ledError(); break;  // 不正コマンド=赤点滅
    }
  }
}
