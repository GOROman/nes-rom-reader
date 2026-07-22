# nes-rom-reader

[日本語](README.md) | [English](README.en.md) | **简体中文**

Famicom(FC 60 针)卡带 ROM 读取工具。M5Stamp S3 + 专用电路板(用 EasyEDA Pro 设计)+ Python CLI / Web UI。

![在实机上读取](docs/images/hero-dumping.jpeg)

> 在实机上读取真实卡带(元祖西游记 超级猴子大冒险 / VAP 1986),
> 并用 Web UI 的 “Super Monkey Viewer” 显示 CHR-ROM 的图块(tiles)。

![已布线的 PCB](docs/images/pcb-routed.png)

## 目录结构

| 目录 | 内容 |
|---|---|
| `firmware/` | M5Stamp S3 (ESP32-S3) 固件(PlatformIO / Arduino) |
| `host/` | Python CLI(读取 `famidump.py` / 出厂测试 `factory_test.py`) |
| `web/` | 浏览器 Web UI(WebSerial,读取 + 出厂测试,免安装) |
| `hardware/` | EasyEDA Pro 工程、Gerber、BOM、贴片坐标 |
| `docs/` | 设计文档(引脚映射、总线设计、60 针引脚定义) |

## 架构

- **MCU**:M5Stamp S3 采用 **1.27mm 间距的插座** 安装(通过母排针可插拔)
- **卡带接口**:FC 60 针(2.54mm 间距)金手指连接器,焊盘直接布置在板上
- **总线驱动**:
  - 地址线 …… 74HCT595 ×4 移位寄存器链(5V 驱动)
  - 数据读取 …… 74LVC245 ×2(PRG/CHR,5V→3.3V 电平转换)
  - 控制线 …… 74HCT541 缓冲器(3.3V→5V)
- **Mapper**:先支持 Mapper 0 (NROM);后续计划 CNROM / UNROM / MMC1
- **上位机**:Python CLI(`host/famidump.py`),带 CRC32 校验并输出带 iNES 头的 `.nes` 文件

## 电路板

- 双层板,双面铺 GND 铜,约 84 × 77 mm
- 底边中央有 USB-C 缺口,四角有 M3 安装孔(适配 3D 打印外壳)
- 生产数据:将 [`hardware/gerber/nes-rom-reader-gerber.zip`](hardware/gerber/) 直接上传到 JLCPCB 等即可

## 使用方法

```sh
# 烧录固件
cd firmware && pio run -t upload

# 读取(NROM:PRG 32KB + CHR 8KB)
python host/famidump.py --port /dev/tty.usbmodem* --prg 32 --chr 8 -o game.nes
```

## Web UI(在浏览器中读取)

![Web UI](docs/images/webui.png)

只需在**桌面版 Chrome / Edge** 中打开 `web/index.html`。它通过 WebSerial 连接 M5Stamp S3,
用图形界面即可读取、下载 `.nes`、运行出厂测试(无需 Python)。Famicom 配色 & 点阵字体,
支持 **日文 / 英文切换**。同样支持 **Android Chrome**(自适应布局)。

### 🐵 Super Monkey Viewer(CHR-ROM 查看器)

![CHR Viewer](docs/images/chr-viewer.png)

将读取到的 CHR-ROM(2bpp 平面格式)解码为 8×8 图块显示。可从 NES 64 色主调色板中
自由更换 4 种颜色,也可切换预设(Grayscale / Game Boy / Mario 等)。还支持加载 `.nes` / `.chr`
文件,因此无需硬件也能离线浏览。

```sh
# 本地启动(file:// 也可,但 WebSerial 建议用 https 或 localhost)
cd web && python3 -m http.server 8000   # → http://localhost:8000
```

## 出厂测试(全数检验)

通过对**基准卡带(已知 CRC 的卡带)**读取并比对 CRC,判定每块组装好的板子是否正常。

```sh
# 1) 在已知良品板上学习基准 CRC(插入基准卡带)
python host/factory_test.py --port /dev/cu.usbmodem* --learn --name "MyRefCart"

# 2) 检验每块板子(同一基准卡带)。退出码 0 = PASS / 1 = FAIL
python host/factory_test.py --port /dev/cu.usbmodem* --test --name "MyRefCart"

# 仅自检,无需卡带(GPIO / 移位寄存器健康检查)
python host/factory_test.py --port /dev/cu.usbmodem* --selftest
```

固件的 `T` 命令(自检)会回读输出引脚、驱动移位寄存器、读取悬空的数据总线,
最后返回 `SELFTEST PASS/FAIL`。Web UI 的 “出厂测试” 页也可执行相同的检查。

## 文档

- 设计说明、引脚映射、60 针引脚定义:[docs/hardware-design.md](docs/hardware-design.md)
- 电路板数据详情:[hardware/README.md](hardware/README.md)

## 致谢

本项目的固件开发以及在 **EasyEDA Pro** 中的电路板设计(原理图、PCB、布线、生产数据生成)
均由 **[Claude Code](https://claude.com/claude-code)(Fable 5)** 完成。

Claude Code 通过 WebSocket 桥接扩展
**[eext-run-api-gateway](https://github.com/easyeda/eext-run-api-gateway)** 操作 EasyEDA Pro,
让 AI 能够直接控制 EasyEDA Pro 来设计电路板。

---

Powered by Claude Code (Fable 5) × EasyEDA Pro
