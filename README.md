# ESP8266 5.83 英寸墨水屏看板

基于 ESP8266 和 600 x 448 黑白墨水屏的家庭状态看板。固件启动后保持唤醒。
Wi-Fi 已连接时，群晖 RX/TX 请求按 5 秒起点到起点调度；时间、天气、PVE 和群晖
完整数据按 10 分钟全刷起点到起点调度，并用 GxEPD2 标准白底波形清理局刷残影。
群晖 IP 和运行天数保留在底栏，只在全刷时更新。

## 屏幕布局

| 区域 | 内容 |
| --- | --- |
| 左上 | 中文年月日/星期标题、周一起始月历和居中当天高亮 |
| 右上 | 中文今天天气/日期、当前天气、8 小时和 7 天预报 |
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

串口启动时会先记录 NAS 接口发现和首个计数器基线，再出现
`Full refresh reason=startup`，随后依次出现 NTP、天气、PVE、NAS、`StandardFull`、
快速局刷模式状态和 `Full refresh complete`。运行中每次速率局刷会记录是否成功、
耗时和空闲堆；10 分钟周期使用
`Full refresh reason=scheduled`，局刷 BUSY 超时后的标准全刷恢复使用
`Full refresh reason=partial-recovery source=cached`。全刷或快速模式恢复失败后的
缓存退避重试使用 `Full refresh reason=readiness-recovery source=cached`。缓存恢复跳过
NTP、天气、PVE 和 NAS 请求，直接用最后一份内存数据执行标准全刷；10 分钟周期和
Wi-Fi 重连全刷仍会重新获取全部远端元数据。PVE 阶段和每次全刷结束还会打印空闲堆、
最大连续块、碎片率及本轮最低堆。按 `Ctrl-C` 退出监控。

典型日志如下：

```text
NAS interface index=...
NAS network rx=... tx=... rx_rate=0 tx_rate=0 valid=0
Full refresh reason=startup
NAS speed partial mode ok=1 heap=...
Full refresh complete reason=startup ready=1
Heap full refresh   free=... max=... frag=...% min=...
NAS speed partial ok=1 ms=... heap=...
```

Wi-Fi 断开后，速率窗口只局刷一次 `RX:-- TX:--`，并暂停 5 秒/10 分钟的联网周期。
手动 Wi-Fi 尝试按 30 秒起点到起点调度；单次 `connectWifi()` 最多阻塞 20 秒。
离线连接或鉴权失败产生的断开事件不会重置这次尝试的起点。
固件持久保存 ESP8266 Wi-Fi 断开事件处理器；即使在 NTP、天气、PVE 或 NAS 阻塞请求
期间掉线后自动重连，主循环仍会丢弃断网前的计数器基线、强制重新发现接口并排队
`Full refresh reason=wifi-reconnected`。重连全刷受标准全刷保护间隔约束，并把下一轮
10 分钟周期锚定到实际开始时间。全刷完成后至少等待 5 秒才开始下一次速率请求。

标准全刷、关电和快速局刷都检查 BUSY 结果。标准全刷 BUSY 成功后，固件先把当前
速率画布复制为局刷像素基线，再尝试进入快速模式；如果快速模式恢复失败，该基线仍
与已完成的全刷一致，但显示保持不可局刷，恢复成功前不会发送局刷命令。标准全刷
失败时不会复制基线。所有标准全刷尝试从上一次尝试完成起共用 60 秒保护间隔，避免
周期、重连或恢复路径连续闪屏；到期但被保护间隔阻挡的周期任务保持待处理，期间已就绪
的速率局刷仍可继续。缓存恢复不会改变原有 10 分钟周期锚点。
PVE 节点/VM 列表或 NAS 卷请求不完整时继续显示上一份有效
快照；单个 Guest Agent 请求失败时保留该 VM 的旧 IP。NTP 失败会保留上次有效时间，
冷启动且无有效时间时日历显示 `时间不可用`。

当前编译基线：静态 RAM 49116/80192（61%）、IRAM 61103/65536（93%）、Flash
759116/1048576（72%）。局刷调度仍需通过实机验收确认长期空闲堆、BUSY 时序、
断网重连和残影表现；完成实机验收前不应据此提交生产固件。

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

微雪官方 V1 Arduino 驱动只提供整帧 `DisplayFrame()`，GxEPD2 的
`GxEPD2_583` 也明确标记 `hasFastPartialUpdate=false`。本项目因此在仓库内维护
`GxEPD2_583_FastPartial` 实验驱动，不修改 Arduino 全局库。生产看板只为右下角
296 x 16 的 RX/TX 窗口保留两帧缓存并使用差分快速波形；其余区域不会在 5 秒
周期中改写。每 10 分钟仍由标准 `renderAll()` 白底全刷更新所有数据并清理残影，
不会把实验 LUT 用作自定义灰阶全帧波形。

群晖网卡索引不是写死的。固件根据 NAS IPv4 通过 `ipAdEntIfIndex` 运行时发现接口，
再读取该接口的 `ifHCInOctets` 和 `ifHCOutOctets` 64 位计数器；接口会定期重新发现，
连续请求失败或接口变化时也会重新绑定计数器 OID。首次采样只建立基线，后续采样
才显示按实际采样间隔计算的速率。孤立请求失败会保留基线；连续 3 次失败、基线超过
60 秒、Wi-Fi 断开或全刷期间检测到掉线时会强制重新建立基线，避免显示长时间平均值。

快速 LUT 未经屏厂确认。即使有 10 分钟标准全刷清理，长期运行仍可能增加残影、
缩短面板寿命或损坏面板；应先观察实机温升、BUSY 行为和残影再决定是否长期使用。

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

测试与生产局刷共用仓库内的通用快速驱动；实际刷新时间以串口
`refresh_ms`/`ms` 日志为准。快速 LUT 未经屏厂确认，存在残影、寿命缩短或损坏
面板的风险。

## 常见问题

- `PVE node HTTP 401`：检查完整 Token ID、realm、token-id 和 secret 是否匹配。
- VM IP 为 `-`：确认 VM 正在运行，并安装、启用了 QEMU Guest Agent。
- PVE 请求在 TLS 阶段失败：重新核对 PVE 当前证书指纹。
- `Missing secrets.h`：从 `secrets.example.h` 创建本地配置后再编译。
- 串口出现 WDT、Exception 或持续复位：记录最后一个 `Heap ...` 阶段，不要继续
  增大 JSON 文档或显示分页缓冲。
