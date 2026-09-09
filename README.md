# ESP8266 5.83 英寸墨水屏看板

基于 ESP8266 和 600 x 448 黑白墨水屏的家庭状态看板。设备保持运行，每 10 分钟获取
一次时间、天气、PVE 和群晖数据，并使用项目内的 600×448 深黑整屏刷新驱动。该
驱动采用 Waveshare V1 示例的 PLL `0x3C` 和 VCOM `0x1E` 参数。启动时先执行整屏黑、
整屏白清除残影，再居中显示“WiFi连接中”；联网和取数完成后绘制四区内容。生产
固件不使用局部刷新，群晖底栏只显示 IP 和运行天数。

## 屏幕布局

| 区域 | 内容 |
| --- | --- |
| 左上 | 月日/周几/设备 IP/下次刷新时间标题、周一起始月历和按实际周数自适应的当天高亮 |
| 右上 | 平铺的实时天气/当前气温/当天高低温/更新时间，以及带图标的未来 8 小时温度曲线 |
| 左下 | PVE 节点状态与 5 台表格化排列的 QEMU 虚拟机 |
| 右下 | 群晖存储池的已用、可用和总容量 |
| 左底栏 | 按 1:2 分栏的 PVE IP 与右对齐节点内存使用条 |
| 右底栏 | 等宽平铺的群晖 IP 和“运行时间：N天” |

PVE 虚拟机按“运行中优先、同组 VMID 升序”排列，最多显示 5 台。表格每行包含
运行状态、按“虚拟机”表头宽度截取的名称、Guest Agent IPv4、配置 CPU 数以及
当前/配置内存。运行中 VM 没有安装或启用 QEMU Guest Agent 时，IP 显示为 `-`。

## 项目结构

- `epd5in83-hanshow-arduino.ino`：唯一固件入口和显示/网络流程。
- `dashboard_model.h`：固定容量 PVE 数据模型和可在主机运行的纯 C++ 辅助函数。
- `secrets.example.h`：无敏感值的本地配置模板。
- `test/`：模型测试和源码门禁。
- `tools/build_firmware.sh`：准备 Arduino 草图目录并编译。
- `tools/flash_and_monitor.sh`：编译、烧录并立即打开串口监控。
- `docs/superpowers/`：本次 PVE 功能的设计和执行记录。

仓库不再保留旧 PlatformIO Hello World 入口，避免误用错误引脚或烧录到过时固件。

## 硬件与依赖

- ESP8266 NodeMCU v2
- 微雪/Good Display 5.83 英寸 V1 黑白屏，600 x 448
- 显示引脚：`CS=15`、`DC=0`、`RST=2`、`BUSY=4`
- Arduino CLI 1.5.1
- ESP8266 Arduino Core 3.1.2
- ArduinoJson 7.4.3
- GxEPD2 1.6.9
- U8g2 for Adafruit GFX
- Arduino SNMP Manager 1.1.13

## 配置

先创建本地配置：

```sh
cp secrets.example.h secrets.h
```

编辑 `secrets.h` 中的四项：

- `WIFI_SSID`
- `WIFI_PASS`
- `SNMP_COMMUNITY`
- `PVE_TOKEN`，格式为 `PVEAPIToken=user@realm!token-id=token-secret`

`secrets.h` 和 `build/` 已被 Git 忽略。不要把有效凭据写入 `.ino`、README 或
`secrets.example.h`。

站点相关但不敏感的配置位于草图顶部：

- `PVE_HOST`、`PVE_PORT`、`PVE_CERT_FINGERPRINT`
- `nas_ip`
- Open-Meteo 经纬度和时区

PVE Token 需要读取节点、集群 VM 资源和运行中 VM Guest Agent 网络接口的权限。
虚拟机内还需要安装并启用 QEMU Guest Agent。

## 测试与源码门禁

```sh
sh test/run_dashboard_tests.sh
sh test/verify_pve_dashboard.sh
sh test/run_partial_clock_tests.sh
sh test/verify_partial_clock.sh
git diff --check
```

`run_dashboard_tests.sh` 会用主机 C++17 编译器验证 VM 排序、Top-N 保留、节点名
容量、IPv4 过滤和内存换算。

`verify_pve_dashboard.sh` 就是本项目的“源码门禁”。它在提交前静态确认：

- PVE 与 NAS 获取/渲染调用没有被注释；
- 三类 PVE API 路径仍存在；
- VM 列表保持固定容量、完整解析后再提交；
- Guest Agent 使用选中的规范节点名；
- PVE 证书指纹校验仍启用；
- Wi-Fi、SNMP 和 PVE 凭据没有写回可跟踪草图，配置模板仍只含占位值；
- `secrets.h` 仍被 Git 忽略。

源码门禁只能防止关键结构被误删，不能替代固件编译、真实 API 请求或 ESP8266
串口内存测试。

## 编译、烧录和监控

编译：

```sh
sh tools/build_firmware.sh
```

脚本会生成被忽略的 `build/epd5in83-hanshow-arduino/` 和 `build/output/`，解决
Arduino CLI 对“草图目录名必须和 `.ino` 文件名一致”的要求。

查看串口：

```sh
arduino-cli board list
```

烧录并立即监控：

```sh
sh tools/flash_and_monitor.sh /dev/cu.usbserial-1120
```

启动时先出现两条 `Panel conditioning` 日志并显示 Wi-Fi 连接状态，随后出现
`Full refresh reason=startup`，依次获取 NTP、天气、PVE 和 NAS，最后输出
`Full refresh complete`；每 10 分钟刷新显示
`Full refresh reason=scheduled`，不会重复黑白清屏。PVE 阶段和每次全刷结束还会
打印空闲堆、最大连续块、碎片率及本轮最低堆。按 `Ctrl-C` 退出监控。

典型日志如下：

```text
Panel conditioning BLACK
Panel conditioning WHITE
Connecting WiFi... OK
Device IP: 192.168.31.x
Full refresh reason=startup
Full refresh complete reason=startup ready=1
Heap full refresh   free=... max=... frag=...% min=...
```

Wi-Fi 断开时暂停联网刷新，每 30 秒尝试重连；单次 `connectWifi()` 最多等待 20 秒。
显示器不执行显式休眠，ESP8266 也不进入深度休眠；标准整屏刷新流程自身仍会在刷新
完成后关闭面板驱动电压。

PVE 节点/VM 列表或 NAS 卷请求不完整时继续显示上一份有效
快照；单个 Guest Agent 请求失败时保留该 VM 的旧 IP。NTP 失败会保留上次有效时间，
冷启动且无有效时间时日历显示 `时间不可用`。

当前编译基线：静态 RAM 47204/80192（58%）、IRAM 61103/65536（93%）、Flash
757676/1048576（72%）。仍需通过实机验收确认 10 分钟刷新和显示深度；
完成实机验收前不应提交生产固件。

## PVE 数据流程

固件按顺序释放每次 HTTPS 和 JSON 对象，避免 ESP8266 堆峰值叠加：

1. `GET /api2/json/nodes`，选择首个在线节点。
2. `GET /api2/json/cluster/resources?type=vm`，逐对象流式解析并只保留目标节点的
   固定容量 QEMU Top-N。
3. 对可见的运行中 VM 请求
   `/nodes/{node}/qemu/{vmid}/agent/network-get-interfaces`，选择首个非 loopback、
   非 link-local IPv4。

HTTPS 在发送 Token 前使用 `PVE_CERT_FINGERPRINT` 固定服务器证书。PVE 证书更新后，
必须同步更新指纹，否则请求会被拒绝。

## 刷新策略

生产看板使用 GxEPD2 自带的 `GxEPD2_583` 标准整屏波形，每 10 分钟重新获取并绘制全部
数据。生产入口不加载实验快速 LUT，也不执行局部刷新、显式屏幕休眠或 NAS 网络速率
采样。仓库中的 `GxEPD2_583_FastPartial` 仅供下方独立时钟实验使用，不会被生产构建
脚本带入固件。

- [微雪官方 5.83 V1 Arduino 示例](https://github.com/waveshareteam/e-Paper/tree/master/Arduino/epd5in83)
- [GxEPD2 项目](https://github.com/ZinggJM/GxEPD2)

### 独立高速局刷时钟测试

`codex/partial-refresh-clock-test` 分支包含独立的 `HH:MM:SS` 测试固件。它参考
V1.2 替换驱动的快速 LUT 和差分像素编码，但只保留 424 x 112 时钟窗口的两帧
缓存，不修改全局安装的 GxEPD2，也不改变生产看板入口。

```sh
sh test/run_partial_clock_tests.sh
sh test/verify_partial_clock.sh
sh tools/build_partial_clock.sh
sh tools/flash_partial_clock.sh /dev/cu.usbserial-1120
```

启动时会执行一次全屏清白，之后每秒仅刷新屏幕中央时钟窗口；每 600 次局刷会
再次全刷以限制残影。全屏清白会检查标准全刷 BUSY 结果；失败后停止继续局刷，并按
60 秒退避重新执行完整清白和快速模式初始化。串口日志格式如下：

```text
Clock 12:34:56 partial=42 refresh_ms=755 heap=37312
```

该实验单独使用仓库内的快速驱动；生产看板不引用它。实际刷新时间以串口
`refresh_ms`/`ms` 日志为准。快速 LUT 未经屏厂确认，存在残影、寿命缩短或损坏
面板的风险。

## 常见问题

- `PVE node HTTP 401`：检查完整 Token ID、realm、token-id 和 secret 是否匹配。
- VM IP 为 `-`：确认 VM 正在运行，并安装、启用了 QEMU Guest Agent。
- PVE 请求在 TLS 阶段失败：重新核对 PVE 当前证书指纹。
- `Missing secrets.h`：从 `secrets.example.h` 创建本地配置后再编译。
- 串口出现 WDT、Exception 或持续复位：记录最后一个 `Heap ...` 阶段，不要继续
  增大 JSON 文档或显示分页缓冲。
