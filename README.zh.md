# nes-rom-reader

[English](README.md) | [日本語](README.ja.md) | **简体中文**

**Famicom(FC 60 针)卡带 ROM 读取工具** — 开源硬件电路板 + M5Stamp S3 固件 + 浏览器 Web UI。
把卡带读取为 `.nes`、查看并编辑 CHR 图形,全部在 Chrome 中完成。

![在实机上读取](docs/images/hero-dumping.jpeg)

> ✅ **已在实机验证** — 成功读取真实卡带(元祖西游记 超级猴子大冒险 / VAP 1986),
> CRC 稳定且可复现。

### ▶ 免安装,立即试用: **https://goroman.github.io/nes-rom-reader/web/**
用 Chrome / Edge(桌面或 Android)打开,连接 M5Stamp S3 即可读取。

---

## 功能特点

- **读取 PRG / CHR** 并保存为标准 `.nes` 文件(iNES 头、自动判定镜像方式)
- **每次传输都做 CRC32 校验** — 数据出错不会被悄悄放过
- **Web UI**(WebSerial)— 无需 Python、无需驱动。Famicom 配色,中/日/英文档,适配 Android
- **🐵 Super Monkey Viewer** — 将 CHR 解码为 8×8 图块、切换调色板、**像素编辑**、
  **用卡带自带字体写入文字**
- **出厂测试** — 通过基准卡带 CRC 比对进行全数检验
- **状态 LED** — 绿=待机 / 蓝=工作中 / 红闪=错误
- **开源硬件** — 附带 EasyEDA 工程、Gerber、BOM 与贴片坐标

## 仓库结构

| 目录 | 内容 |
|---|---|
| `firmware/` | M5Stamp S3 (ESP32-S3) 固件 — PlatformIO / Arduino |
| `host/` | Python CLI:`famidump.py`(读取)、`factory_test.py`(出厂测试) |
| `web/` | 浏览器 Web UI(WebSerial),已部署到上述网址 |
| `hardware/` | EasyEDA Pro 工程、Gerber、BOM、贴片坐标 |
| `docs/` | 设计文档 — 引脚映射、总线设计、60 针引脚定义 |

## 硬件

![已布线的 PCB](docs/images/pcb-routed.png)

- **MCU**:M5Stamp S3,采用 **1.27mm 间距插座** 安装(可插拔)
- **卡带接口**:FC 60 针(2.54mm)金手指连接器 — 焊盘直接布置在板上
- **地址**:74HCT595 ×4 移位寄存器链(5V)
- **数据**:74LVC245 ×2(PRG/CHR,5V→3.3V 电平转换)
- **控制**:74HCT541 缓冲器(3.3V→5V)
- 双层板,双面铺 GND 铜,约 84 × 77 mm,USB-C 缺口,4 个 M3 安装孔

自行打样:把 [`hardware/gerber/nes-rom-reader-gerber.zip`](hardware/gerber/) 上传到 JLCPCB 等即可。
详见 [hardware/README.md](hardware/README.md) 与 [docs/hardware-design.md](docs/hardware-design.md)。

📖 **[组装指南(面向新手,分步说明)](docs/assembly.en.md)** — 所需工具、焊接顺序、检查方法与故障排查。

> ⚠️ **v0.1 已知问题**:M5Stamp S3 无法直接插入本板(Stamp 焊盘布局与实物不符)。
> 变通方法是用排针垫高后接线(组装指南中有说明)。将在 v0.2 修复。

## 支持的卡带与限制

- ✅ **mapper 0 (NROM)** — 完全支持(PRG 32KB + CHR 8KB),已在实机验证
- 🧪 **CNROM (mapper 3)** — 通过**总线冲突(bus conflict)**方式实现 CHR 换库(实验性)
- ⚠️ **UNROM / MMC1 等** — 尚未支持

**为什么 v0.1 也能做到**:本板的数据缓冲器(74LVC245)方向固定、只读,无法驱动数据总线。
但在 CNROM 上,**写周期期间 PRG-ROM 仍在驱动总线**(AND 型
[总线冲突](https://www.nesdev.org/wiki/Bus_conflict))——这正是游戏自身把库号写到
「该地址本来就存有同样数值」的 ROM 位置的原因。本工具同样:先读出 PRG,
找到数值等于目标库号的地址,在那里发出一次空写周期,卡带上的锁存器便会捕获该值。

CLI 使用 `--bank-switch`,Web UI 勾选 **CHR 换库** 即可。

> ⚠️ 带**防拷贝二极管**的 CNROM 板会制造额外的总线冲突,只有能主动驱动总线的读取器才能取胜,
> 这类卡带仍需具备写入能力的 v0.2 电路板。

**为什么 MMC1 / MMC3 系映射器无法读取**:这些映射器不会产生总线冲突
(写周期期间 PRG-ROM 不驱动总线),因此上述技巧没有可供锁存的数值。
选择库需要真正驱动数据总线,而本只读电路板做不到。仍可读取上电时恰好映射的
32KB PRG 窗口以及电池备份存档 RAM(见上文),但无法读出完整 ROM。
MMC3 衍生的非授权映射器同样如此,例如 **mapper 165**(*Fire Emblem (Unl)*,
MMC2/MMC3 混合,带 MMC2 式 CHR 锁存与 4KB CHR-RAM)。
完全支持需要能够向卡带驱动数据缓冲器的电路板改版。

## 快速开始

### 1. 烧录固件

```sh
cd firmware && pio run -t upload
```

### 2a. 在浏览器中读取(推荐)

打开 **https://goroman.github.io/nes-rom-reader/web/** → **连接** → 设置 PRG/CHR 大小 → **读取并保存 .nes**

### 2b. 用 CLI 读取

```sh
pip install -r host/requirements.txt
python host/famidump.py --port /dev/tty.usbmodem* --prg 32 --chr 8 -o game.nes
```

## Web UI

![Web UI](docs/images/webui.png)

也可以在本地运行(WebSerial 需要 `https` 或 `localhost`):

```sh
cd web && python3 -m http.server 8000   # → http://localhost:8000
```

### 🐵 Super Monkey Viewer — CHR 工具

![CHR Viewer](docs/images/chr-viewer.png)

- **图块查看器** — 将 CHR(2bpp 平面格式)解码为 8×8 图块;可调缩放与每行图块数
- **调色板** — 预设(Grayscale / Game Boy / Mario …),或从 NES 64 色主调色板中逐个指定 4 种颜色
- **像素编辑器** — **双击图块**打开 8×8 点阵编辑器(支持鼠标与触摸),编辑后应用
- **图块编辑** — 选中图块后擦除,或用其他图块覆盖粘贴
- **用卡带字体写入文字** — CHR 中的信息只是按顺序排列的字形图块,因此只要指定字体起始图块
  与字形排列顺序,输入文本即可把对应字形图块写入目标位置
- **导出** — 下载编辑后的 `.chr`,或重新生成的 `.nes`
- **离线可用** — 无需硬件,直接加载任意 `.nes` / `.chr` 文件查看与编辑

## 出厂测试(全数检验)

通过比对**基准卡带**的读取 CRC,判定组装好的板子是否正常。

```sh
# 1) 在已知良品板上学习基准 CRC
python host/factory_test.py --port /dev/cu.usbmodem* --learn --name "MyRefCart"

# 2) 检验每块板子 — 退出码 0 = PASS,1 = FAIL
python host/factory_test.py --port /dev/cu.usbmodem* --test --name "MyRefCart"

# 仅自检(无需卡带)
python host/factory_test.py --port /dev/cu.usbmodem* --selftest
```

Web UI 的 **出厂测试** 面板也能执行同样的流程,并让每个步骤间隔约 1 秒,方便观察状态 LED。
自检会回读全部输出引脚、驱动移位寄存器,最后返回 `SELFTEST PASS/FAIL`。

### 电池备份存档数据

带存档电池的卡带(火焰之纹章、塞尔达、巫术等)会把存档保存在 `$6000-$7FFF` 的 WRAM 中。
可通过 Web UI 的 **存档数据** 卡片(CLI 为 `--save-out game.sav`)读出并保存为 `.sav` 文件。

这适用于上电时 WRAM 处于启用状态的卡带。由于本板为只读、无法驱动数据总线,
因此无法写入启用 WRAM 的映射器寄存器(MMC1 为 PRG 寄存器的 bit4,MMC3 为 `$A001`)。
如果该位在上电时为关闭状态,读出的结果会是全部相同的字节,工具会给出提示。
不支持将存档数据写回卡带。

## 串口协议

USB CDC,115200 波特率。单行命令:

| 命令 | 响应 |
|---|---|
| `V` | `famidump v0.4` |
| `R <addr_hex> <len_hex>` | `OK <len>` + 原始数据 + `CRC xxxxxxxx`(PRG) |
| `C <addr_hex> <len_hex>` | 同上(CHR) |
| `M` | `H` / `V` / `?` — 镜像方式 |
| `T` | 带节奏的自检,最后输出 `SELFTEST PASS/FAIL` |
| `S` | `STATUS famidump-v0.4 mirror=? pins=PASS md=0x20` |
| `W <addr_hex> <len_hex>` | 读取电池备份 WRAM($6000-$7FFF),响应格式与 `R`/`C` 相同 |
| `B <addr_hex>` | CNROM 换库用的空写周期 → `BANK xxxx` |

CRC 为 CRC32(IEEE 802.3 / 兼容 `zlib.crc32`),8 位大写十六进制。

## 链接

- 仓库:https://github.com/GOROman/nes-rom-reader
- Web UI(在线版):https://goroman.github.io/nes-rom-reader/web/
- 作者 X:[@GOROman](https://x.com/GOROman)

## 许可证

**NULL License** — 参见 [LICENSE](LICENSE)。

非商业用途可自由使用、修改和再分发,但所有权利归 **GOROman** 所有。
如需销售或用于商业用途,请事先联系: [@GOROman](https://x.com/GOROman)。
本项目按“原样”提供,不作任何担保。

## 致谢

固件开发以及在 **EasyEDA Pro** 中的电路板设计(原理图、PCB、布线、生产数据生成)
均由 **[Claude Code](https://claude.com/claude-code)(Fable 5)** 完成,通过 WebSocket 桥接扩展
**[eext-run-api-gateway](https://github.com/easyeda/eext-run-api-gateway)** 直接操作 EasyEDA Pro。

---

Powered by Claude Code (Fable 5) × EasyEDA Pro
