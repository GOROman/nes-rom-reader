#!/usr/bin/env python3
"""ファミコンカセット吸い出しCLI。

M5Stamp S3 ファームウェア (firmware/) とUSB CDCで通信し、
iNESヘッダ付き .nes ファイルを出力する。

プロトコル: R/C コマンドの応答は "OK <len>\n" + 生データ +
"CRC xxxxxxxx\n" (生データの CRC32、zlib.crc32 互換)。受信後に照合する。

例 (NROM-256: PRG 32KB + CHR 8KB):
    python famidump.py --port /dev/tty.usbmodem1101 --prg 32 --chr 8 -o game.nes
"""

import argparse
import sys
import zlib

import serial


def read_exact(ser: serial.Serial, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            raise TimeoutError(f"シリアル受信タイムアウト ({len(buf)}/{n} bytes)")
        buf += chunk
    return buf


def dump(ser: serial.Serial, cmd: str, addr: int, length: int) -> bytes:
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
        raise RuntimeError(
            f"{cmd}: CRC不一致 (device={dev_crc:08X} host={host_crc:08X})。"
            "配線・接触を確認して再実行してください")
    print(f"  {cmd}: {length // 1024}KB 受信 (CRC {host_crc:08X} OK)",
          file=sys.stderr)
    return data


def ines_header(prg_kb: int, chr_kb: int, mapper: int, vertical: bool) -> bytes:
    flags6 = ((mapper & 0x0F) << 4) | (1 if vertical else 0)
    flags7 = mapper & 0xF0
    return bytes([0x4E, 0x45, 0x53, 0x1A,  # "NES\x1a"
                  prg_kb // 16, chr_kb // 8, flags6, flags7]) + bytes(8)


def sanity_check(args) -> None:
    """--mapper/--prg/--chr の妥当性チェック。致命的なら終了、疑わしければ警告。"""
    if args.prg <= 0 or args.prg % 16 != 0:
        sys.exit(f"エラー: --prg は16の倍数(KB)を指定してください: {args.prg}")
    if args.chr_kb < 0 or args.chr_kb % 8 != 0:
        sys.exit(f"エラー: --chr は0または8の倍数(KB)を指定してください: {args.chr_kb}")
    if not 0 <= args.mapper <= 255:
        sys.exit(f"エラー: --mapper は0-255で指定してください: {args.mapper}")
    if args.mapper == 0:
        if args.prg > 32:
            print(f"警告: mapper 0 (NROM) のPRGは最大32KBです (--prg {args.prg})。"
                  "バンク切替が必要なカセットは正しく吸い出せません", file=sys.stderr)
        if args.chr_kb > 8:
            print(f"警告: mapper 0 (NROM) のCHRは最大8KBです (--chr {args.chr_kb})",
                  file=sys.stderr)
    else:
        print(f"警告: 現行ファームウェアはバンク切替未対応です。mapper {args.mapper} "
              "では先頭バンクの内容しか読めない可能性があります", file=sys.stderr)
    if args.chr_kb == 0:
        print("情報: --chr 0 → CHR-RAM搭載カセットとして扱います(CHRは吸い出しません)",
              file=sys.stderr)


def main() -> None:
    p = argparse.ArgumentParser(description="ファミコンカセット吸い出しツール")
    p.add_argument("--port", required=True, help="シリアルポート")
    p.add_argument("--prg", type=int, default=32, help="PRGサイズ KB (default: 32)")
    p.add_argument("--chr", type=int, default=8, dest="chr_kb",
                   help="CHRサイズ KB (default: 8)")
    p.add_argument("--mapper", type=int, default=0, help="iNESマッパー番号 (default: 0)")
    p.add_argument("-o", "--output", required=True, help="出力 .nes ファイル")
    args = p.parse_args()

    sanity_check(args)

    with serial.Serial(args.port, 115200, timeout=5) as ser:
        ser.reset_input_buffer()
        ser.write(b"V\n")
        version = ser.readline().decode().strip()
        if "famidump" not in version:
            sys.exit(f"デバイスが応答しません: {version!r}")
        print(f"接続: {version}", file=sys.stderr)

        ser.write(b"M\n")
        mirror = ser.readline().decode().strip()
        print(f"ミラーリング: {mirror}", file=sys.stderr)
        if mirror == "?":
            print("警告: ミラーリング判定不能(4画面/未接続/接触不良の可能性)。"
                  "iNESヘッダは水平ミラーとして書き出します", file=sys.stderr)

        prg = dump(ser, "R", 0x8000, args.prg * 1024)
        chr_data = dump(ser, "C", 0x0000, args.chr_kb * 1024) if args.chr_kb else b""

    if len(set(prg)) == 1:
        print(f"警告: PRGデータが全バイト同一 (0x{prg[0]:02X})。"
              "カセット未挿入または接触不良の可能性があります", file=sys.stderr)

    with open(args.output, "wb") as f:
        f.write(ines_header(args.prg, args.chr_kb, args.mapper, mirror == "V"))
        f.write(prg)
        f.write(chr_data)
    print(f"書き出し完了: {args.output} "
          f"(PRG {args.prg}KB / CHR {args.chr_kb}KB / mapper {args.mapper})",
          file=sys.stderr)


if __name__ == "__main__":
    main()
