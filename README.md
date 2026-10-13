# <img src="src/assets/cangaroo.svg" width="48" height="48"> CANgaroo-dualcan
**跨平台 CAN / CAN-FD 总线分析仪（Linux + Windows 双端构建）**

CANgaroo-dualcan 是一款面向 **汽车电子**、**机器人** 与 **工业自动化** 领域的开源 CAN 总线分析仪，支持实时抓包、解码与分析 CAN / CAN-FD 流量，并支持双通道并行工作。

> **原生支持**：Linux 上通过 **SocketCAN** 直连真实硬件；Windows 上通过 **WinUSB / CandleAPI**（gs_usb / CandleLight 适配器）直连，同时兼容 **SLCAN** 与 **CANblaster**（UDP）。

---

## 🚀 核心特性

*   **实时 CAN / CAN-FD 解码**：支持标准帧与高速灵活数据率帧。
*   **双通道并行（dualcan）**：可同时接入并监控两路独立 CAN 总线，便于网关 / 路由类场景的对照分析。
*   **广泛的硬件兼容**：
    *   Linux：**SocketCAN**（PCAN、Kvaser、CANable、vcan 等，几乎覆盖内核支持的所有接口）。
    *   Windows：**CandleAPI / WinUSB**（gs_usb 协议，如 CANable、CandleLight），以及 **SLCAN** 串口适配器。
    *   网络：**CANblaster**（UDP）远程总线。
*   **DBC 数据库**：加载多个 `.dbc` 文件，把原始帧即时解码为可读信号。
*   **多形态可视化**：时间序列曲线、散点图、文本视图与交互式仪表盘，支持缩放与实时提示。
*   **协议层重组**：UDS（ISO-TP）多帧重组、J1939 多帧（BAM/CM）重组与 PGN 识别。
*   **Python 脚本自动化**：内嵌 Python 引擎，可脚本化测量、发送与分析任务。详见 [Python API 文档](cangaroo_python_api.md)。
*   **过滤与日志**：实时过滤关键数据，导出抓包供离线分析。
*   **现代化工作区**：可停靠、多显示器友好的界面布局。

---

## 🛠️ 从源码构建

### Linux（SocketCAN）

方式一：使用自带脚本

```bash
git clone https://github.com/tguilin/CANgaroo-dualcan.git
cd CANgaroo-dualcan
./install_linux.sh
```

方式二：手动安装依赖后构建

| 发行版 | 依赖安装命令 |
| :--- | :--- |
| **Ubuntu / Debian** | `sudo apt install qt6-base-dev libqt6charts6-dev libqt6serialport6-dev build-essential libnl-3-dev libnl-route-3-dev` |
| **Fedora** | `sudo dnf install qt6-qtbase-devel qt6-qtcharts-devel qt6-qtserialport-devel libnl3-devel` |
| **Arch Linux** | `sudo pacman -S qt6-base qt6-charts qt6-serialport libnl` |

```bash
qmake cangaroo.pro
make -j$(nproc)
# 产物在 bin/cangaroo
```

> Linux 下需要 Python 3 与 pybind11（`pip install pybind11`），用于内嵌脚本引擎。

### Windows（MinGW + Qt 6）

已在本机验证通过的工具链：

| 组件 | 路径 / 版本 |
| :--- | :--- |
| Qt | Qt 6.12.0（`D:\Qt\6.12.0\mingw_64`） |
| 编译器 | MinGW 13.1.0 64-bit（`D:\Qt\Tools\mingw1310_64`） |
| 构建工具 | `qmake` + `mingw32-make` |
| Python | Python 3.15（已装 `pybind11`：`pip install pybind11`） |

**所需 Qt 模块**：Core / Gui / Widgets / Xml / Charts / SerialPort / Svg / Network。
若某模块缺失，用 `D:\Qt\MaintenanceTool.exe` 在线安装（需 Qt 账号）。

**构建步骤（PowerShell）**：

```powershell
# 1. 把 Python、Qt、MinGW 加入 PATH
$env:PATH = "C:\Users\<你>\AppData\Local\Programs\Python\Python315;" +
            "D:\Qt\Tools\mingw1310_64\bin;" +
            "D:\Qt\6.12.0\mingw_64\bin;" + $env:PATH

# 2. 进入项目根目录
cd D:\MyFile\Solution\AI\CANgaroo-dualcan

# 3. 生成 Makefile 并编译
qmake cangaroo.pro
mingw32-make -j4
# 产物：bin\cangaroo.exe
```

**运行**：由于是动态链接 Qt，启动前需保证 Qt DLL、MinGW 运行时（`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`）与 `python315.dll` 均在 PATH 中，或将上述 DLL 复制到 `bin\` 与 exe 同目录。

```powershell
$env:PATH = "C:\Users\<你>\AppData\Local\Programs\Python\Python315;" +
            "D:\Qt\Tools\mingw1310_64\bin;" +
            "D:\Qt\6.12.0\mingw_64\bin;" + $env:PATH
Start-Process ".\bin\cangaroo.exe"
```

> 也可用 `D:\Qt\Tools\QtCreator\bin\qtcreator.exe` 打开 `cangaroo.pro`，在 IDE 内选择 MinGW 64 Kit 直接构建运行。

---

## 🔌 硬件支持

### Linux（SocketCAN）
Cangaroo 走 Linux 内核的 **SocketCAN** 子系统，内核支持的 CAN 接口基本都能用。把接口拉起到 500k：

```bash
sudo ip link set can0 up type can bitrate 500000
```

### Windows（CandleAPI / WinUSB）
原生支持运行 **gs_usb** 协议的适配器（如 CANable、CandleLight），通过 WinUSB + CandleAPI 驱动接入。插上后在“设置 → CAN 接口”里选择对应设备即可。

### SLCAN（串口适配器）
Linux 上用 `slcand` 把串口适配器桥接成 SocketCAN 接口：

```bash
sudo slcand -o -s6 -t hw -S 115200 /dev/ttyUSB0 slcan0
sudo ip link set slcan0 up
```

### 支持的硬件清单
*   **PEAK-System (PCAN)**：PCAN-USB / Pro / PCIe 等（Linux 驱动 `peak_usb`）。
*   **原生 USB-CAN**：CANable（CandleLight 固件）、Kvaser USB/CAN Leaf、各类 CandleLight 兼容设备。
*   **SLCAN 串口**：CANable（默认 SLCAN 固件）、Arduino CAN 扩展板（SLCAN sketch）。
*   **工业 / 嵌入式**：PCIe/mPCIe CAN 卡、SoC 上的 CAN 控制器（如树莓派 + MCP2515）。
*   **远程 / 网络**：[CANblaster](https://github.com/OpenAutoDiagLabs/CANblaster)（UDP）。

---

## 🚦 快速上手

### 1. 无硬件测试（虚拟 CAN，Linux）
```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```
然后在 Cangaroo 里连接 `vcan0` 即可。

### 2. 远程 CAN 监控（SSH 管道）
把远端机器（如车上的树莓派）的总线流量转发到本地虚拟接口：

```bash
ssh user@remote-ip "candump -L can0" | canplayer vcan0=can0 -t
```
本地 Cangaroo 连接 `vcan0` 即可看到远端流量。

#### 多跳 SSH 隧道
当目标设备在跳板机后时：

```bash
# local-pc -> jump-host -> target-device
ssh -N -L localhost:9999:<目标设备IP>:22 user@jump-host-ip
# 再通过隧道转发 CAN
ssh -p 9999 root@localhost "stdbuf -o0 candump -L can0" | canplayer vcan0=can0 -t
```

### 3. ARXML 转 DBC
Cangaroo 原生使用 DBC，ARXML 可用 `canconvert` 转换：

```bash
pip install canconvert
canconvert TCU.arxml TCU.dbc
```

---

## 🐍 Python 自动化

CANgaroo-dualcan 内嵌 Python 环境，可脚本化重复任务、自动化复杂发送序列或程序化分析流入流量。完整 API 与示例见 [Python API 文档](cangaroo_python_api.md)。

---

## 📁 项目结构

```
cangaroo.pro            # 顶层 subdirs 工程
src/
├── src.pro             # 应用主工程
├── core/               # 核心：CAN 帧、总线、后端、PythonEngine
├── driver/             # 驱动适配层
│   ├── SocketCanDriver #   Linux SocketCAN
│   ├── CandleApiDriver #   Windows CandleAPI/WinUSB
│   ├── SLCANDriver     #   串口 SLCAN
│   ├── CANBlastDriver  #   UDP CANblaster
│   └── GrIPDriver      #   GrIP
├── parser/             # DBC / ARXML 解析
├── decoders/           # UDS(ISO-TP) / J1939 解码
├── window/             # UI 各窗口（Trace / Graph / Generator / Setup …）
├── assets/             # 图标、QSS 主题
└── translations/       # 多语言 .ts / .qm
```

---

## 🤝 贡献

欢迎提交 Issue 与 PR：
*   **问题反馈**：[GitHub Issues](https://github.com/tguilin/CANgaroo-dualcan/issues)
*   **代码贡献**：Fork 仓库后提交 Pull Request，开发约定见 [CONTRIBUTING.md](CONTRIBUTING.md)。

---

## 📜 致谢

*   **原作者**：Hubert Denkmair（hubert@denkmair.de）
*   **上游维护**：[OpenAutoDiagLabs/cangaroo](https://github.com/OpenAutoDiagLabs/cangaroo)
*   **CANgaroo-dualcan**：在上游基础上增加双通道并行与 Windows 原生构建支持，仓库地址 [github.com/tguilin/CANgaroo-dualcan](https://github.com/tguilin/CANgaroo-dualcan)。

---

**License**: [GPL-3.0+](LICENSE)
