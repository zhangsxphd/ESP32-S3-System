# ESP32-S3 System

基于 PlatformIO 的 `ESP32-S3` 田间节点原型工程，当前以 `LILYGO T-Beam S3 Supreme SX1262` 为目标硬件，重点完成板级 bring-up、基础外设联调和 OLED 状态界面显示。

这个仓库目前处于“原型验证”阶段，目标是先把主控、电源管理、GPS、环境传感器、RTC 和屏幕完整跑通，再逐步扩展到 LoRa 通信、田间传感器采集和自动控水逻辑。

## 当前已实现功能

- 通过 `AXP2101` 初始化并使能关键 3.3V 电源通道
- 启动 `L76K` GPS 模块并解析 NMEA 数据
- 读取 `BME280` 温湿度数据
- 初始化 `PCF8563` RTC，并用 GPS 时间周期性校时
- 在 `SH1106 128x64 OLED` 上绘制实时状态界面
- 显示电池连接状态、充电状态和剩余电量
- 显示日期、时间、温度、湿度、海拔、卫星数量和定位状态

## 当前 OLED 界面内容

屏幕界面分为三层：

- 顶部状态栏：日期、时间、电池图标、电量百分比或 USB 供电状态
- 中间数据区：温度、湿度、海拔
- 底部信息区：卫星数量、GPS 定位状态，以及成功定位后的经纬度

如果 GPS 尚未完成定位，底部会显示 `WAITING FOR FIX`。

## 硬件基础

当前代码按以下板级资源组织：

- 主控：ESP32-S3
- 电源管理：AXP2101
- 显示屏：SH1106 OLED 128x64
- GPS：L76K
- 环境传感器：BME280
- RTC：PCF8563

## 引脚与总线分配

### 主 I2C

- `SDA = GPIO17`
- `SCL = GPIO18`

用于：

- OLED
- BME280

### PMU / RTC 独立 I2C

- `SDA = GPIO42`
- `SCL = GPIO41`

用于：

- AXP2101
- PCF8563

### GPS 串口

- `GPS_RX_PIN = GPIO9`
- `GPS_TX_PIN = GPIO8`
- `GPS_EN_PIN = GPIO7`

## 软件依赖

项目使用 PlatformIO 管理依赖，核心库如下：

- `U8g2`
- `XPowersLib`
- `TinyGPSPlus`
- `Adafruit BME280 Library`
- `RTClib`

详见 [platformio.ini](./platformio.ini)。

## 项目结构

```text
ESP32_Hello/
├── include/
├── lib/
├── src/
│   └── main.cpp
├── test/
├── .gitignore
├── platformio.ini
└── README.md
```

## 编译环境

- 开发方式：PlatformIO
- 框架：Arduino
- 目标环境：`tbeam-s3-supreme`
- 平台版本：`espressif32@6.12.0`
- 串口监视波特率：`115200`
- 上传速度：`921600`

## 如何编译与烧录

### 方式一：使用 VS Code + PlatformIO

1. 打开该项目目录
2. 等待 PlatformIO 安装依赖
3. 连接开发板
4. 点击 `Build`
5. 点击 `Upload`
6. 打开 `Serial Monitor`

### 方式二：命令行

如果本机已安装 PlatformIO CLI，可在项目根目录执行：

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## 启动流程概览

程序启动顺序如下：

1. 初始化串口
2. 初始化 PMU，开启关键供电通道
3. 初始化 GPS
4. 初始化 BME280
5. 初始化 RTC
6. 显示开机画面 `Agri-Node Started`
7. 进入循环采集与 UI 刷新

主循环中会持续：

- 解析 GPS 串口数据
- 在条件满足时用 GPS 校准 RTC
- 每 500ms 刷新一次 OLED
- 更新环境数据、卫星状态、电池状态和时间显示

## 运行效果说明

当前版本的作用不是做完整田间监测，而是完成以下 bring-up 验证：

- 板子是否正常启动
- AXP2101 是否正常供电
- OLED 是否正常显示
- GPS 串口链路是否正常
- BME280 是否可读
- RTC 是否可以被 GPS 校时

如果 OLED 已经成功点亮，说明最关键的主控、电源和显示链路已经跑通。

## 后续扩展方向

接下来建议按以下顺序继续扩展：

1. 增加串口日志，明确输出 PMU / GPS / BME / RTC 初始化状态
2. 增加更详细的电池电压、电流和充电状态显示
3. 接入 LoRa 通信链路
4. 接入田间传感器，例如温湿度、水位、张力计等
5. 加入数据打包、上传和本地容错机制

## 注意事项

- `board` 当前使用的是通用 `esp32-s3-devkitc-1` 定义，后续如果涉及更多板级特性，可以再做更细的适配
- `.pio/` 属于本地构建产物，不应提交到 Git
- `.vscode/launch.json` 通常包含本机绝对路径，当前不建议纳入版本管理

## 仓库定位

这是一个面向后续“田间智能监测节点”演进的基础仓库。当前阶段重点是把板级能力稳定打通，而不是一次性上齐所有传感器和通信模块。

后续如果原型验证通过，可以在这个仓库上继续发展为：

- 田间节点固件仓库
- LoRa 数据采集仓库
- 自动控水控制节点仓库
- 多传感器统一接入仓库
