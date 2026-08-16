#!/usr/bin/env python3
"""vgm2evt - VGM/VGZ (レジスタログ) から YM2151 イベント列 (.evt) を生成する。

usage: vgm2evt.py <in.vgz|in.vgm> [out.evt]

YM2151 (コマンド 0x54) のみ抽出。OKIM6258 等の他チップは読み飛ばす。
出力は mdxplay.py と同じ "t_us addr data" 形式。
"""
import gzip
import struct
import sys


def load(path):
    data = open(path, "rb").read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    if data[:4] != b"Vgm ":
        raise SystemExit("not a VGM file")
    return data


# 可変長・未対応コマンドのスキップ長 (コマンドバイトを除くパラメータ長)
SKIP = {}
for c in range(0x30, 0x40): SKIP[c] = 1          # 予約(1byte)
for c in range(0x40, 0x4F): SKIP[c] = 2          # Mikey等(2byte)
for c in (0x4F, 0x50): SKIP[c] = 1               # GG/PSG
for c in range(0x51, 0x60): SKIP[c] = 2          # 各種FM(2byte) ※0x54は別処理
for c in range(0xA0, 0xC0): SKIP[c] = 2          # AY/OKIM6258(0xB7)等
# DACストリーム制御 (VGM 1.60+、ADPCM再生用) — 読み飛ばし
SKIP[0x90] = 4; SKIP[0x91] = 4; SKIP[0x92] = 5
SKIP[0x93] = 10; SKIP[0x94] = 1; SKIP[0x95] = 4
for c in range(0xC0, 0xE0): SKIP[c] = 3
for c in range(0xE0, 0x100): SKIP[c] = 4


def convert(data):
    ver = struct.unpack_from("<I", data, 0x08)[0]
    if ver >= 0x150:
        ofs = 0x34 + struct.unpack_from("<I", data, 0x34)[0]
    else:
        ofs = 0x40
    clk = struct.unpack_from("<I", data, 0x30)[0] & 0x3FFFFFFF
    print(f"# YM2151 clock in VGM: {clk} Hz", file=sys.stderr)

    events = []
    t_samples = 0        # 44100Hz サンプル単位
    i = ofs
    while i < len(data):
        c = data[i]
        if c == 0x54:                       # YM2151 write
            events.append((t_samples, data[i + 1], data[i + 2]))
            i += 3
        elif c == 0x61:                     # wait n samples
            t_samples += struct.unpack_from("<H", data, i + 1)[0]
            i += 3
        elif c == 0x62:
            t_samples += 735; i += 1
        elif c == 0x63:
            t_samples += 882; i += 1
        elif 0x70 <= c <= 0x7F:
            t_samples += (c & 15) + 1; i += 1
        elif c == 0x66:                     # end of data
            break
        elif c == 0x67:                     # data block: 66 tt ss ss ss ss + data
            size = struct.unpack_from("<I", data, i + 3)[0]
            i += 7 + size
        elif c == 0x68:                     # PCM RAM write: 12バイト固定
            i += 12
        elif 0x80 <= c <= 0x8F:             # YM2612 PCM+wait (来ないはずだが安全に)
            t_samples += (c & 15); i += 1
        elif c in SKIP:
            i += 1 + SKIP[c]
        else:
            print(f"# unknown cmd {c:02X} at {i}, stopping", file=sys.stderr)
            break
    return [(s * 1000000 // 44100, a, d) for s, a, d in events]


def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else None
    data = load(src)
    events = convert(data)
    if not events:
        raise SystemExit("no YM2151 events found")
    clk = struct.unpack_from("<I", data, 0x30)[0] & 0x3FFFFFFF
    out = open(dst, "w") if dst else sys.stdout
    out.write(f"#clock {clk}\n")   # mdxplay がピッチ補正に利用
    for t, a, d in events:
        out.write(f"{t} {a} {d}\n")
    print(f"# {len(events)} events, {events[-1][0]/1e6:.1f}s", file=sys.stderr)


if __name__ == "__main__":
    main()
