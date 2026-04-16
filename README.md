# ESP32-S3 System

基于 PlatformIO 的 `ESP32-S3` 田间节点原型工程，当前目标硬件为 `LILYGO T-Beam S3 Supreme SX1262`。这版固件已经从“单纯点屏测试”发展到一个可独立运行的本地监测节点，具备传感采集、OLED 显示、Wi‑Fi 联网、SD 存储、本地 Web 面板和 HTTP 上报能力。

项目目前仍处于原型验证阶段，重点是把一套稳定的数据闭环先跑通：

- 传感器采集
- 本地状态显示
- 周期性写入 SD 卡
- 本地网页查看与下载
- 通过 HTTP 上报 JSON 数据

## 当前已实现功能

- 初始化 `AXP2101` 电源管理芯片并开启关键电源通道
- 启动 `L76K` GPS 模块并解析 NMEA 数据
- 读取 `BME280` 的温度、湿度、气压
- 读取 `AS7331` 的 `UVA / UVB / UVC` 物理单位值
- 初始化 `PCF8563` RTC，并尝试使用 GPS 校时
- 在 `SH1106 128x64 OLED` 上显示实时仪表盘
- 挂载 SD 卡并将采样数据写入 `/data.csv`
- 通过 `WebServer` 提供本地监控面板
- 提供 JSON 状态接口、CSV 在线查看和下载
- 通过 `HTTPClient` 将 JSON 数据上报到远端测试接口

## 当前固件能力概览

这版程序已经具备 4 个输出通道：

1. OLED：本地快速查看核心状态
2. Serial：调试日志与快捷命令
3. SD 卡：周期性写入 CSV 历史数据
4. Web：浏览器查看实时面板、历史趋势和 JSON 状态

## 硬件组成

- 主控：ESP32-S3
- 电源管理：AXP2101
- 显示屏：SH1106 OLED 128x64
- GPS：L76K
- 环境传感器：BME280
- UV 传感器：AS7331
- RTC：PCF8563
- 存储：SPI SD 卡

## 引脚定义

### 主 I2C

- `GPIO17` -> `SDA`
- `GPIO18` -> `SCL`

用于：

- OLED
- BME280
- AS7331

### PMU / RTC I2C

- `GPIO42` -> `SDA`
- `GPIO41` -> `SCL`

用于：

- AXP2101
- PCF8563

### GPS

- `GPIO9` -> GPS RX
- `GPIO8` -> GPS TX
- `GPIO7` -> GPS EN

### SD 卡 SPI

- `GPIO35` -> MOSI
- `GPIO36` -> SCLK
- `GPIO37` -> MISO
- `GPIO47` -> CS

### 其他

- `GPIO34` -> `IMU_CS`

当前代码会主动把 `IMU_CS` 拉高，以避免板载 IMU 和 SD 卡共用 SPI 时发生总线冲突。

## 主要软件依赖

项目通过 PlatformIO 管理依赖，当前核心库包括：

- `U8g2`
- `XPowersLib`
- `TinyGPSPlus`
- `Adafruit BME280 Library`
- `RTClib`
- `Adafruit SHT4x Library`
- `Adafruit AS7331 Library`

说明：

- 当前代码实际使用的是 `BME280` 和 `AS7331`
- `SHT4x` 依赖已经写入 `platformio.ini`，方便后续切换或扩展

## 当前网页功能

固件启动 Web 服务后，会在本地网络中提供一个名为 `GreenMind Local Panel` 的状态页面。

主页面展示：

- 时间、日期、时间戳
- 温度、湿度、气压
- UVA / UVB / UVC
- UV overflow 状态
- 海拔、卫星数、定位状态、经纬度
- 电量、电池电压、充电状态
- Wi‑Fi RSSI、IP 地址
- SD 状态、最近一次 SD 写入结果
- 最近一次 HTTP 上报结果
- Uptime、Free Heap、RTC 同步状态、最近采样年龄、下次记录倒计时

## 当前 Web 路由

- `/`
  实时总览面板

- `/status`
  输出当前节点状态 JSON

- `/view`
  在线预览 `/data.csv`

- `/download`
  直接下载 `/data.csv`

- `/history`
  查看历史趋势页

## CSV 日志格式

程序会自动创建 `/data.csv`，并在文件为空时写入表头。

当前表头为：

```csv
Time,TempC,HumPct,PresshPa,UVA_uWcm2,UVB_uWcm2,UVC_uWcm2,UVOverflow,AltM,Lat,Lon,Sats,GpsFix,BattPct,BattmV,Charging,WiFiRSSI
```

默认每 `60 秒` 记录一行数据。

## 串口命令

串口监视器中可使用以下快捷命令：

- `h`：打印帮助
- `s`：打印当前系统状态
- `d`：把 `/data.csv` 内容输出到串口

## 启动流程

程序启动时大致按以下顺序运行：

1. 初始化串口
2. 初始化 PMU 并开启外围供电
3. 初始化 GPS
4. 初始化 RTC
5. 初始化 BME280
6. 初始化 AS7331
7. 初始化 OLED
8. 挂载 SD 卡
9. 连接 Wi‑Fi
10. 启动本地 WebServer

进入主循环后持续执行：

- 处理串口命令
- 处理网页请求
- 持续解析 GPS 串口数据
- 检查 Wi‑Fi 重连
- 检查 SD 卡是否掉线并尝试重新挂载
- 更新缓存数据
- 刷新 OLED
- 定时写入 CSV
- 定时上报 HTTP JSON

## 当前运行节奏

- OLED 刷新间隔：`500 ms`
- UV 采样间隔：`2000 ms`
- SD 状态轮询：`3000 ms`
- 数据记录与 HTTP 上报：`60000 ms`

## Wi‑Fi 与远端 HTTP 配置

当前代码里直接写了 Wi‑Fi 账号密码：

```cpp
const char* ssid = "ChinaNet-CD5F";
const char* password = "12345678";
```

如果你要迁移到其他网络，直接修改 `src/main.cpp` 中这两行即可。

当前 HTTP 上报目标是测试接口：

```text
http://httpbin.org/post
```

这说明当前版本主要用于验证“设备能够把 JSON 发出去”，还不是最终生产接口。后续接入你自己的 GreenMind 后端时，只需要替换 `sendRemote()` 里的 URL 和鉴权逻辑。

## 编译与烧录

### 使用 VS Code + PlatformIO

1. 打开项目目录
2. 等待 PlatformIO 自动安装依赖
3. 连接开发板
4. 点击 `Build`
5. 点击 `Upload`
6. 打开 `Serial Monitor`

### 使用命令行

如果本机已安装 PlatformIO CLI：

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## 当前版本定位

这版固件已经不只是“bring-up 测试”，而是一个可以现场跑起来的本地节点原型。它最适合做以下验证：

- 传感器采集是否稳定
- OLED 界面是否可读
- SD 写卡是否可靠
- 本地网页是否方便查看状态
- HTTP 上报链路是否通畅
- GPS / RTC / PMU / Wi‑Fi 是否能共同稳定运行

## 仍然建议继续完善的地方

- 把 Wi‑Fi 凭据移到单独配置区，避免硬编码
- 把 HTTP 上报目标换成你自己的后端
- 增加设备唯一 ID、实验区编号、节点编号
- 给 Web 页面加更多诊断信息，比如最近错误日志
- 增加 LoRa 或 RS485 设备接入
- 增加更严格的数据异常值过滤

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

## 注意事项

- `.pio/` 属于构建产物，不应提交到 Git
- `.vscode/launch.json` 往往带有本机绝对路径，不建议提交
- 当前 README 以“当前本地工作区代码”为准
