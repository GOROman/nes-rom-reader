#!/usr/bin/env python3
"""mdxplay - mdxdump のイベント列(.evt)を実 YM2151 へストリーミング再生する。

usage: mdxplay.py <file.evt> [--port /dev/cu.usbmodem1101] [--tune HZ | --no-tune]

ピッチ補正: MDX は X68000 (φM=4MHz) 前提。ファームは現在 4MHz 駆動なので
既定では補正なし。3.58MHz 系で駆動する場合のみ --tune に実測の φM を渡すと
KC/KF を 1/64半音単位でシフトして本来のピッチに合わせる。
"""
import argparse
import math
import struct
import sys
import time

import serial


def note_to_linear(n):
    return n - (n >> 2)          # KCの音名コード(3,7,11,15欠番)→ 0-11


def linear_to_note(lin):
    return lin + lin // 3


def transform_events(events, offset64):
    """KC(0x28+ch)/KF(0x30+ch) をチャンネル状態を追いながらシフトする"""
    if offset64 == 0:
        return events
    kc = [0] * 8
    kf = [0] * 8
    out = []
    max_total = (7 * 12 + 11) * 64 + 63
    for t, addr, data in events:
        if 0x28 <= addr <= 0x2F or 0x30 <= addr <= 0x37:
            ch = addr & 7
            if addr < 0x30:
                kc[ch] = data
            else:
                kf[ch] = data >> 2
            oct_ = (kc[ch] >> 4) & 7
            lin = oct_ * 12 + note_to_linear(kc[ch] & 15)
            total = lin * 64 + kf[ch] + offset64
            total = max(0, min(max_total, total))
            nlin, nkf = divmod(total, 64)
            noct, nnote = divmod(nlin, 12)
            out.append((t, 0x28 + ch, (noct << 4) | linear_to_note(nnote)))
            out.append((t, 0x30 + ch, nkf << 2))
        else:
            out.append((t, addr, data))
    return out


def encode(events):
    """(t_us, addr, data) -> 4バイトレコード列 (dt は100µs単位、剰余繰越で無ドリフト)"""
    buf = bytearray()
    prev_units = 0
    for t, addr, data in events:
        units = t // 100
        dt = units - prev_units
        prev_units = units
        while dt > 0xFFFE:
            buf += struct.pack("<HBB", 0xFFFE, 0xFE, 0)   # 遅延のみレコード
            dt -= 0xFFFE
        buf += struct.pack("<HBB", dt, addr & 0xFF, data & 0xFF)
    buf += struct.pack("<HBB", 0xFFFF, 0xFF, 0xFF)        # 終端
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("evt")
    ap.add_argument("--port", default="/dev/cu.usbmodem1101")
    ap.add_argument("--tune", type=float, default=4000000.0,
                    help="実チップの φM 実測値 [Hz]。4MHz 相当へピッチ補正する"
                         "(既定=4MHz なので補正なし。3.58MHz駆動時は実測値を渡す)")
    ap.add_argument("--no-tune", action="store_true", help="ピッチ補正しない")
    ap.add_argument("--no-monomix", action="store_true",
                    help="パンをそのまま送る(既定は全chのRLを両ONにして、"
                         "モノラル出力での片ch欠落を防ぐ)")
    args = ap.parse_args()

    events = []
    with open(args.evt) as f:
        for line in f:
            t, a, d = line.split()
            events.append((int(t), int(a), int(d)))
    print(f"{len(events)} events, {events[-1][0]/1e6:.1f} s")

    offset64 = 0
    if not args.no_tune:
        offset64 = round(768 * math.log2(4000000.0 / args.tune))
        print(f"pitch offset: {offset64} (1/64 semitone)")
    events = transform_events(events, offset64)
    if not args.no_monomix:
        # レジスタ $20-$27 の bit7-6 (RL出力イネーブル) を両ONに強制。
        # 現状の出力はモノラル(RIGHTch)なので、左パンのパートが欠落しないように。
        events = [(t, a, (d | 0xC0) if 0x20 <= a <= 0x27 else d)
                  for t, a, d in events]
    data = encode(events)
    print(f"stream: {len(data)} bytes")

    s = serial.Serial(args.port, 115200, timeout=5)
    time.sleep(0.8)                 # ポートオープンでリセットが掛かる場合の猶予
    s.reset_input_buffer()
    s.write(b"X\n")
    while True:
        line = s.readline()
        if not line:
            print("no XSTART (timeout)"); sys.exit(1)
        print(line.decode(errors="replace").strip())
        if b"XSTART" in line:
            break
    # フロー制御: デバイスは1KB消費ごとに 'K' を返す。ESP32 の USB-Serial-JTAG は
    # バッファ溢れ時にデータを黙って捨てるため、在庫(inflight)を6KB以下に保つ。
    t0 = time.time()
    CHUNK = 1024
    WINDOW = 6 * 1024
    sent = 0
    inflight = 0
    s.timeout = 10
    while sent < len(data):
        while inflight < WINDOW and sent < len(data):
            n = min(CHUNK, len(data) - sent)
            s.write(data[sent:sent + n])
            sent += n
            inflight += n
        if sent >= len(data):
            break          # 全量送信済み。K待ちするとXDONEの先頭を食うので抜ける
        b = s.read(1)
        if b == b"K":
            inflight -= 1024
        elif b == b"":
            print("flow-control timeout"); sys.exit(1)
        # 'K' 以外(XTIMEOUT等のテキスト)はそのまま読み飛ばし
    print(f"sent in {time.time()-t0:.1f}s, playing to end...")
    s.timeout = events[-1][0] / 1e6 + 15   # 曲長+マージンまで XDONE を待つ
    while True:
        line = s.readline()
        if not line:
            print("(no response)"); break
        print(line.decode(errors="replace").strip())
        if b"XDONE" in line or b"XTIMEOUT" in line:
            break
    s.close()


if __name__ == "__main__":
    main()
