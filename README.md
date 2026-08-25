# ESP8266 5.83 英寸墨水屏看板

基于 ESP8266 和 600 x 448 黑白墨水屏的家庭状态看板。固件每 10 分钟唤醒一次，
依次获取时间、天气、PVE 和群晖数据，完成一次全屏刷新后进入深度休眠。

## 屏幕布局

| 区域 | 内容 |
| --- | --- |
| 左上 | 当月日历和当天高亮 |
| 右上 | Open-Meteo 当前天气、8 小时和 7 天预报 |
| 左下 | PVE 节点状态与 QEMU 虚拟机列表 |
| 右下 | 群晖存储池状态与容量 |
| 左底栏 | PVE IP、节点已用/总内存和占用率 |
| 右底栏 | 群晖 IP 和运行天数 |

PVE 虚拟机按“运行中优先、同组 VMID 升序”排列，最多显示 7 台。每行包含
运行状态、名称、Guest Agent IPv4、配置 CPU 数以及当前/配置内存。运行中 VM
没有安装或启用 QEMU Guest Agent 时，IP 显示为 `-`。

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

串口正常周期应依次出现 Wi-Fi、NTP、天气、PVE、NAS 和 `_Update_Full`。PVE 阶段
会打印空闲堆、最大连续块、碎片率及本轮最低堆。按 `Ctrl-C` 退出监控。

当前验证基线：静态 RAM 37804/80192（47%）、IRAM 61103/65536（93%）、Flash
432176/1048576（41%）；实机完成 PVE 后空闲堆约 40.7 KB，NAS 后约 38.8 KB，
未出现 WDT、Exception、复位循环或内存溢出。

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

## 5.83 英寸 V1 局部刷新

微雪官方 V1 Arduino 驱动只提供整帧 `DisplayFrame()`，没有局部窗口或快速局刷
接口。GxEPD2 虽能限制写入区域，但 `GxEPD2_583` 明确标记
`hasFastPartialUpdate=false`，局部与全屏刷新都使用约 15 秒波形。因此当前固件不做
分钟级时间或群晖上下行速率局刷，避免频繁闪屏和长时间保持 ESP8266 唤醒。

- [微雪官方 5.83 V1 Arduino 示例](https://github.com/waveshareteam/e-Paper/tree/master/Arduino/epd5in83)
- [GxEPD2 项目](https://github.com/ZinggJM/GxEPD2)

## 常见问题

- `PVE node HTTP 401`：检查完整 Token ID、realm、token-id 和 secret 是否匹配。
- VM IP 为 `-`：确认 VM 正在运行，并安装、启用了 QEMU Guest Agent。
- PVE 请求在 TLS 阶段失败：重新核对 PVE 当前证书指纹。
- `Missing secrets.h`：从 `secrets.example.h` 创建本地配置后再编译。
- 串口出现 WDT、Exception 或持续复位：记录最后一个 `Heap ...` 阶段，不要继续
  增大 JSON 文档或显示分页缓冲。
