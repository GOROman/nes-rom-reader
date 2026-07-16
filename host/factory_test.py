#!/usr/bin/env python3
"""出荷テスト(全数検査)CLI。

組み立て後の基板が正しく吸い出せるかを、基準カセット(既知CRCのカセット)で検証する。

使い方:
  1. 既知良品の基板で基準CRCを学習(基準カセットを挿して):
       python factory_test.py --port /dev/cu.usbmodem1201 --learn --name "DragonQuest"
     → golden.json に PRG/CHR の基準CRCを保存

  2. 各基板を検査(同じ基準カセットを挿して):
       python factory_test.py --port /dev/cu.usbmodem1201 --test --name "DragonQuest"
     → PASS / FAIL を判定(終了コード 0=PASS, 1=FAIL)

セルフテスト(カセット不要、Tコマンド)のみ:
       python factory_test.py --port /dev/cu.usbmodem1201 --selftest
"""

import argparse
import json
import os
import sys
import zlib

import serial

GOLDEN_PATH = os.path.join(os.path.dirname(__file__), "golden.json")


def read_exact(ser, n):
    buf = b""
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            raise TimeoutError(f"受信タイムアウト ({len(buf)}/{n} bytes)")
        buf += chunk
    return buf


def dump(ser, cmd, addr, length):
    ser.write(f"{cmd} {addr:X} {length:X}\n".encode())
    status = ser.readline().decode().strip()
    if not status.startswith("OK"):
        raise RuntimeError(f"デバイスエラー応答: {status!r}")
    data = read_exact(ser, length)
    crc_line = ser.readline().decode().strip()
    if not crc_line.startswith("CRC "):
        raise RuntimeError(f"CRC行が受信できません: {crc_line!r}")
    dev_crc = int(crc_line.split()[1], 16)
    host_crc = zlib.crc32(data) & 0xFFFFFFFF
    if dev_crc != host_crc:
        raise RuntimeError(f"転送CRC不一致 (device={dev_crc:08X} host={host_crc:08X})")
    return data, host_crc


def connect(port):
    ser = serial.Serial(port, 115200, timeout=5)
    ser.reset_input_buffer()
    ser.write(b"V\n")
    ver = ser.readline().decode().strip()
    if "famidump" not in ver:
        ser.close()
        sys.exit(f"✗ デバイスが応答しません: {ver!r}")
    print(f"接続: {ver}", file=sys.stderr)
    return ser


def selftest(ser):
    print("--- セルフテスト(T) ---", file=sys.stderr)
    ser.write(b"T\n")
    passed = None
    while True:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            break
        print("  " + line, file=sys.stderr)
        if line.startswith("SELFTEST"):
            passed = line.endswith("PASS")
            break
    return passed


def load_golden():
    if os.path.exists(GOLDEN_PATH):
        with open(GOLDEN_PATH) as f:
            return json.load(f)
    return {}


def save_golden(g):
    with open(GOLDEN_PATH, "w") as f:
        json.dump(g, f, indent=2, ensure_ascii=False)


def main():
    p = argparse.ArgumentParser(description="出荷テスト(全数検査)")
    p.add_argument("--port", required=True)
    p.add_argument("--prg", type=int, default=32, help="基準カセットのPRG KB")
    p.add_argument("--chr", type=int, default=8, dest="chr_kb", help="基準カセットのCHR KB")
    p.add_argument("--name", default="reference", help="基準カセット名(golden.jsonのキー)")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--learn", action="store_true", help="基準CRCを学習して保存")
    g.add_argument("--test", action="store_true", help="基準CRCと照合して合否判定")
    g.add_argument("--selftest", action="store_true", help="Tセルフテストのみ(カセット不要)")
    args = p.parse_args()

    ser = connect(args.port)
    try:
        if args.selftest:
            ok = selftest(ser)
            print("PASS ✅" if ok else "FAIL ❌")
            sys.exit(0 if ok else 1)

        # セルフテストは常に前段で実行
        st = selftest(ser)
        if st is False:
            print("FAIL ❌ (セルフテストNG)")
            sys.exit(1)

        _, prg_crc = dump(ser, "R", 0x8000, args.prg * 1024)
        chr_crc = None
        if args.chr_kb:
            _, chr_crc = dump(ser, "C", 0x0000, args.chr_kb * 1024)
        print(f"PRG-CRC={prg_crc:08X}" + (f" CHR-CRC={chr_crc:08X}" if chr_crc is not None else ""),
              file=sys.stderr)

        golden = load_golden()
        if args.learn:
            golden[args.name] = {
                "prg_kb": args.prg, "chr_kb": args.chr_kb,
                "prg_crc": f"{prg_crc:08X}",
                "chr_crc": f"{chr_crc:08X}" if chr_crc is not None else None,
            }
            save_golden(golden)
            print(f"✅ 学習完了: '{args.name}' を golden.json に保存")
            print(f"   PRG-CRC={prg_crc:08X}" +
                  (f" CHR-CRC={chr_crc:08X}" if chr_crc is not None else ""))
            sys.exit(0)

        # --test
        ref = golden.get(args.name)
        if not ref:
            sys.exit(f"✗ 基準 '{args.name}' が未登録です。先に --learn してください")
        ok = True
        if f"{prg_crc:08X}" != ref["prg_crc"]:
            print(f"✗ PRG-CRC不一致 (got {prg_crc:08X} / 基準 {ref['prg_crc']})")
            ok = False
        else:
            print(f"✓ PRG-CRC 一致 {ref['prg_crc']}")
        if ref.get("chr_crc") and chr_crc is not None:
            if f"{chr_crc:08X}" != ref["chr_crc"]:
                print(f"✗ CHR-CRC不一致 (got {chr_crc:08X} / 基準 {ref['chr_crc']})")
                ok = False
            else:
                print(f"✓ CHR-CRC 一致 {ref['chr_crc']}")
        print("=== PASS ✅ ===" if ok else "=== FAIL ❌ ===")
        sys.exit(0 if ok else 1)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
