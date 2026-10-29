# 彩叶（基础版） HueLeaf Base

OSPTEK 墨水屏开发套件 · ESP32-C5 主控 · 3.7″ 六色墨水屏 · 双频 Wi-Fi 6

<p align="center">
  <img src="docs/images/banner.png" alt="OSPTEK 彩叶 HueLeaf 墨水屏开发套件 基础版" width="100%">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/MCU-ESP32--C5-1E1E1E?style=for-the-badge" alt="MCU">
  <img src="https://img.shields.io/badge/PMU-AXP2101-D93A2B?style=for-the-badge" alt="PMU">
  <img src="https://img.shields.io/badge/E--Paper-3.7%22%206--Color-F0BE32?style=for-the-badge" alt="Display">
  <img src="https://img.shields.io/badge/Wi--Fi%206-2.4%20%2F%205%20GHz-2058C4?style=for-the-badge" alt="Wi-Fi">
  <img src="https://img.shields.io/badge/RTC-PCF8563-2E8B4A?style=for-the-badge" alt="RTC">
</p>

<p align="center">
  <b>简体中文</b> &nbsp;|&nbsp; <a href="README_EN.md">English</a>
</p>

<img src="docs/images/divider.svg" alt="" width="100%" height="6">

## 简介

彩叶 HueLeaf 是 OSPTEK 推出的墨水屏开发套件，本仓库对应其基础版。

基础版以 ESP32-C5 为主控，搭载一块 3.7 英寸六色墨水屏，可显示黑、白、红、黄、蓝、绿六种颜色。板上集成 AXP2101 电源管理芯片、PCF8563 RTC 时钟、MSA321 加速度计、ST25R3916 NFC 读卡芯片、4 MB Flash 和 TF 卡槽，ESP32-C5 提供 2.4 GHz / 5 GHz 双频 Wi-Fi 6 连接。

整机采用挂牌式外壳，可以挂在胸前当电子工牌，也可以放在桌面上使用。墨水屏只在刷新画面时耗电，配合 AXP2101 的锂电池管理和 PCF8563 的定时唤醒，设备可以长时间休眠、按需刷新，适合用电池长期供电。

本仓库面向二次开发，提供基于 ESP-IDF 的示例代码、出厂固件和硬件资料。

### 典型用途

- 电子工牌、会议胸牌
- 桌面日历、天气时钟
- 信息看板、消息提醒
- 电子价签、门牌标识

## 硬件规格

| 项目 | 参数 |
| :--- | :--- |
| 主控 | ESP32-C5（ESP32-C5HF4） |
| 显示屏 | 3.7″ 六色墨水屏 EPD037A02，480 × 720（黑、白、红、黄、蓝、绿） |
| 无线 | 双频 Wi-Fi 6，2.4 / 5 GHz |
| 电源管理 | AXP2101 |
| RTC 时钟 | PCF8563 |
| 加速度计 | MSA321 |
| NFC | ST25R3916 |
| 存储 | 4 MB Flash，TF 卡 |
| USB | Type-C，ESP32-C5 原生 USB |
| 按键 | 复位键、电源键 |
| 指示灯 | 充电指示（蓝）、开机指示（绿） |

完整原理图见 [hardware/HueLeaf_Base_Schematic_V1.0.pdf](hardware/HueLeaf_Base_Schematic_V1.0.pdf)，元件位置见位号图 [hardware/HueLeaf_Base_Placement.pdf](hardware/HueLeaf_Base_Placement.pdf)。

### 引脚分配

| 功能 | GPIO | 说明 |
| :--- | :--- | :--- |
| SPI MOSI | 0 | 墨水屏与 TF 卡共用 |
| SPI CLK | 1 | 墨水屏与 TF 卡共用 |
| SPI MISO | 9 | TF 卡 |
| 墨水屏 CS / DC / RST / BUSY | 5 / 6 / 7 / 8 | |
| TF 卡 CS | 10 | |
| I2C SDA / SCL | 2 / 3 | AXP2101、PCF8563、MSA321、ST25R3916 共用 |
| 加速度计中断 | 4 | MSA321 |
| RTC 中断 | 23 | PCF8563 |
| NFC 中断 | 24 | ST25R3916 |
| PMIC 中断 | 25 | AXP2101 |
| NFC 使能 | 26 | |

| I2C 设备 | 地址 |
| :--- | :--- |
| AXP2101 | `0x34` |
| PCF8563 | `0x51` |
| MSA321 | `0x62` |
| ST25R3916 | `0x50` |

## 仓库内容

| 目录 | 内容 |
| :--- | :--- |
| [`examples/`](examples) | 示例代码与出厂固件 |
| [`hardware/`](hardware) | 原理图、位号图 |

## 开发环境

示例代码基于 [ESP-IDF](https://github.com/espressif/esp-idf) 开发。

## 示例

### 3.7einkDvb 网页传图

用手机或电脑打开网页，把图片发送到墨水屏上显示。代码位于 [`examples/3.7einkDvb`](examples/3.7einkDvb)。

#### 编译烧录

通过 USB 连接开发板，在示例目录下执行：

```bash
cd examples/3.7einkDvb
idf.py build flash monitor
```

#### 配网

1. 首次上电，屏幕显示 **Scan to setup** 二维码。
2. 手机连接设备热点 **`Eink E6 3.7`**（无密码，5 GHz，手机需支持 5 GHz）。
3. 扫描屏幕上的二维码，或在浏览器打开 `http://192.168.4.1`。
4. 在「配网」中选择 Wi-Fi，填写密码后点击连接。
5. 连接成功后热点自动关闭，屏幕刷新为 **Scan to upload** 二维码，下方显示设备的局域网地址。

Wi-Fi 信息会保存在设备中，下次开机自动连接；12 秒内未连上则重新打开配网热点。

> [!NOTE]
> 示例中 Wi-Fi 国家码固定为 `CN`，配网热点使用 5 GHz 36 信道。在其他国家或地区使用时，请按当地无线电法规修改 `examples/3.7einkDvb/main/wifi_mgr/wifi_mgr.c` 中的国家码，以及 `wifi_mgr.h` 中的 `WIFI_AP_CHANNEL`。

#### 发送图片

1. 手机切回刚才配置的 Wi-Fi，扫描 **Scan to upload** 二维码进入网页。
2. 在「图片」中选择一张图片，也可以拖入或粘贴。
3. 调整画面：
   - **取景与旋转**：自动裁剪为屏幕比例，可拖动、缩放取景，或左右旋转 90°
   - **明暗与颜色**：曝光、亮度、对比度、高光、阴影、饱和度
   - **六色混色与风格**：彩色 / 灰度、抖动、降噪，可打开「查看抖动像素」预览效果
   - **文本**：在图上添加文字，可设置字号、字色、背景
4. 在「屏幕预览」中确认效果后发送，墨水屏整屏刷新显示。

#### 其他功能

| 功能 | 说明 |
| :--- | :--- |
| 清空屏幕 | 将屏幕刷成全白 |
| 区域替换 | 只替换画面中的一块区域 |
| 充电设置 | 在「设置」中选择充电档位，并填写电池额定容量（mAh） |
| NFC 读卡 | 将 NFC 卡贴近设备，读取的数据保存到 TF 卡 `/sdcard/nfc/` 目录（需插入 TF 卡） |

#### 屏幕方向

默认旋转 180°。如需修改，编辑 `examples/3.7einkDvb/main/pin_map.h` 中的 `EINK_ROTATION`（可选 `0` 或 `180`），然后重新编译烧录。

## 许可证

- 代码（`examples/`）：[Apache-2.0](LICENSE)
- 硬件资料与文档（`hardware/`、`docs/`）：[CC BY-NC-SA 4.0](LICENSE-CC-BY-NC-SA-4.0)
- 第三方组件（如 Vue）沿用其原有许可证

<img src="docs/images/divider.svg" alt="" width="100%" height="6">

<p align="center">
  <b>O S P T E K</b> &nbsp;·&nbsp; <code>HUELEAF-BASE</code>
</p>
