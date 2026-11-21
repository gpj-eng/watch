# 黄山派手表接入 Jetson Nano B01

黄山派作为 BLE 外围设备发送六轴数据和 SOS。Jetson Nano 作为中心设备订阅通知，用 SQLite 保存 SOS，并向手表回写接收状态。网关默认只接收和保存数据。确需发送短信时，由 Jetson 经以太网或 Wi-Fi 调用腾讯云短信服务。

### 1. 确认 Jetson 有可用的蓝牙控制器

Jetson Nano B01 有 M.2 Key E 插槽。先确认实际装有支持 BLE 的模块，或已插入内核支持的 USB BLE 适配器。JetPack 4.6.x 基于 Ubuntu 18.04，接收程序使用系统 Python 3 和 BlueZ 的 D-Bus 接口，兼容 BlueZ 5.48 的 GATT API。无需为接收程序升级 JetPack 或安装 Bleak。[NVIDIA JetPack 4.6.6](https://developer.nvidia.com/jetpack-sdk-466) · [Jetson Nano 硬件](https://developer.nvidia.com/embedded/jetson-nano) · [BlueZ 5.48 GATT API](https://github.com/bluez/bluez/blob/5.48/doc/gatt-api.txt)

在 Jetson 终端执行：

```bash
head -n 1 /etc/nv_tegra_release
sudo apt update
sudo apt install bluez python3-dbus python3-gi rfkill
sudo systemctl enable --now bluetooth
bluetoothctl --version
bluetoothctl list
rfkill list bluetooth
```

若蓝牙显示软禁用，执行 `sudo rfkill unblock bluetooth`。若 `bluetoothctl list` 仍为空，先用 `lsusb` 检查 USB 适配器，再用 `sudo dmesg | grep -Ei 'bluetooth|btusb|hci'` 排查驱动或固件。多个控制器时，用 `sudo btmgmt info` 对照控制器序号，再给接收程序传入对应的 `--adapter hci0` 或 `--adapter hci1`。

### 2. 复制网关并在 Jetson 本机跑模拟

将整个 `jetson` 目录复制到 Jetson。若已配置 SSH，可从当前 Windows 工作区执行以下命令，将 `USER` 和 `JETSON_IP` 替换成 Jetson 用户与局域网地址：

```powershell
scp -r D:\watch\jetson USER@JETSON_IP:~/
```

在 Jetson 执行：

```bash
cd ~/jetson
/usr/bin/python3 -B gateway.py simulate
```

程序直接把模拟的 20 字节通知交给正式接收代码，使用临时 SQLite 数据库。检查日志出现 `SIM IMU PASS`、`SIM SOS PASS`、`SIM COMPLETE`。模拟包括序号回绕、丢包、畸形帧、两个联系人乱序到达、日志库关闭后重新打开、重复事件、手动 SOS 和三种回执。短信服务结果由模拟提供，无需 BLE 硬件或云服务。

### 3. 找到并配对手表

手表须运行带 `HSPI-SOS` 蓝牙广播及下文 UUID 的固件。在手表 USB 串口（1000000、8N1）输入 `watch_contacts +12025550100 +12025550101` 保存演示联系人。查看设备地址，以地址绑定网关。手机配对或调试程序可能占用连接，联调前先断开。

```text
bluetoothctl
power on
agent KeyboardOnly
default-agent
scan on
# 等待出现 HSPI-SOS，抄下自己的手表地址
scan off
# 此时在手表 USB 串口输入 watch_pair，60 秒内执行 pair
pair AA:BB:CC:DD:EE:FF
# 输入手表显示的六位配对码
trust AA:BB:CC:DD:EE:FF
info AA:BB:CC:DD:EE:FF
quit
```

用实际地址替换 `AA:BB:CC:DD:EE:FF`。确认 `Paired: yes`。黄山派固件要求带认证的加密连接，配对窗口关闭后拒绝新配对。启用真实联系人前验证未配对设备无法启用通知和写入回执。[BlueZ Device1](https://bluez.readthedocs.io/en/latest/device-api/)

### 4. 连接并订阅，保持短信关闭

```bash
cd ~/jetson
/usr/bin/python3 -B gateway.py ble --watch AA:BB:CC:DD:EE:FF \
  --adapter hci0 --imu-log motion.jsonl
```

看到 `Subscribed to HSPI-SOS` 后转动手表，检查终端的 IMU 摘要。另开终端执行 `tail -f motion.jsonl` 查看逐条记录。静止时合加速度应约为 1000 mg，角速度接近 0°/s，转动与轻移时对应轴变化。采样质量以实际连续记录为准，`seq_gap` 是当前记录前丢失的包数。断开后网关每隔约 3 秒尝试重连并重新订阅。正常停止使用 Ctrl+C。

### 5. 实机验收 SOS 和回执

1. 在手表 USB 串口输入 `watch_bench`。屏幕进入 30 秒倒计时，按 KEY2。Jetson 应无对应的 SOS 入库日志。
2. 再输入 `watch_bench`，等待倒计时结束。Jetson 应显示 `committed: reason=1` 和 `ACK ... sent: status=1`。两名演示联系人应作为同一事件入库。若仅检查本机提醒，使用 `watch_demo`。
3. 触发手动 SOS。应收到另一事件，`reason=2`。该事件无需等待跌倒取消倒计时。
4. 检查重发和重连。对同一个 `event_id`，每个联系人在一个事件中只保存一次。手表应以原编号和原联系人顺序重发同一事件。
5. 分别记录 1 分钟静止及 5 分钟日常动作，统计丢包和重连情况。后续在 Jetson 融合其他传感器时，先用 `received_monotonic_s` 做同机粗对时，保留手表 `uptime_ms` 估算偏移与漂移。BLE 到达时间包含传输延迟。

如果没有订阅日志，检查适配器、实际地址、配对状态、是否被手机占用，以及固件 NUS UUID。如果只有 IMU 而没有 SOS，检查是否配置了演示联系人、倒计时是否完成、是否收齐全部联系人帧，以及手表有没有记录本次事件编号。   

### 6. 选配真实短信

实机通过第 5 步后再启用短信。实时发送需要 Jetson 联网、腾讯云短信账号、开通短信服务并有已审核的签名及通知模板。发送大陆手机号应配置国内短信模板和套餐。网关还要求本地号码白名单，仅允许发送到配置的联系人。用 `btmon` 在重新连接时核查对应手表连接已完成链路加密，并验证未配对中心设备无法读取联系人或启用相关通知。[`SendSms` API](https://cloud.tencent.com/document/product/382/55981) · [腾讯云 Python SDK](https://cloud.tencent.com/document/product/382/43196)

```bash
sudo apt install python3-venv
cd ~/jetson
/usr/bin/python3 -m venv --system-site-packages .venv
.venv/bin/python -m pip install tencentcloud-sdk-python-common tencentcloud-sdk-python-sms
read -rsp '腾讯云 Secret ID: ' TENCENTCLOUD_SECRET_ID; echo
read -rsp '腾讯云 Secret Key: ' TENCENTCLOUD_SECRET_KEY; echo
read -rp '短信应用 ID: ' TENCENT_SMS_APP_ID
read -rp '短信签名: ' TENCENT_SMS_SIGN
read -rp '短信模板 ID: ' TENCENT_SMS_TEMPLATE_ID
read -rp '获准接收的 E.164 号码（逗号隔开）: ' HSPI_SMS_ALLOWED_NUMBERS
export TENCENTCLOUD_SECRET_ID TENCENTCLOUD_SECRET_KEY TENCENT_SMS_APP_ID
export TENCENT_SMS_SIGN TENCENT_SMS_TEMPLATE_ID HSPI_SMS_ALLOWED_NUMBERS
.venv/bin/python -B gateway.py ble --watch AA:BB:CC:DD:EE:FF \
  --live-sms --confirm-encrypted-gatt
```

模板有变量时，设置 `TENCENT_SMS_TEMPLATE_PARAMS` 为正好包含所需参数的 JSON 字符串数组。默认地域为 `ap-guangzhou`，可通过 `TENCENT_SMS_REGION` 修改。启用后，仅用已同意接收的测试联系人做一次实发，检查网关日志、手表回执和接收手机。云接口 `Ok` 代表服务受理，手机实收需要另行核实。

网关按手表地址及运行模式分别建库，避免实发之前的模拟事件。库默认在 `~/.local/state/hspi/`，联系人保存在数据库中，终端日志只记录事件编号和联系人数量。若短信请求超时或程序在请求期间退出，数据库会保留可查询的失败状态，避免送达状态未知时自动再发。用事件编号在云后台核对后再决定是否重发。

### BLE 帧协议

服务 UUID 为 `6e400001-b5a3-f393-e0a9-e50e24dcca9e`。手表的 TX 通知为 `6e400003-b5a3-f393-e0a9-e50e24dcca9e`，手表的 RX 带响应写入为 `6e400002-b5a3-f393-e0a9-e50e24dcca9e`。每条通知或写入恰好 20 字节，适用默认 ATT MTU 23。多字节整数按小端排列。

| 消息 | 字节偏移 | 数据 |
| --- | --- | --- |
| IMU，手表 → Jetson | 0、1 | `0x01`、版本号 `1` |
| | 2–3、4–7 | `uint16` 序号（可回绕）、`uint32` 采样时刻（手表启动后毫秒） |
| | 8–19 | 依次为 XYZ 三轴加速度（mg）和 XYZ 三轴角速度（0.1°/s），各为 `int16` |
| SOS，手表 → Jetson | 0、1 | `0x02`、原因（1＝跌倒倒计时到期，2＝手动 SOS） |
| | 2–9 | 非零 `uint64` 事件编号，同一事件的每个联系人共用 |
| | 10、11 | 联系人序号（从 0 开始），联系人总数（1 至 3） |
| | 12–19 | E.164 号码。去掉 `+`，每字节写两位数字，先写高四位，空余位全部填 `F` |
| ACK，Jetson → 手表 | 0、1 | `0x81`、状态（1＝全部联系人已入库，2＝短信服务已受理全部号码，3＝失败或结果待核实） |
| | 2–9、10–19 | `uint64` 事件编号、十个零 |
| 校时，Jetson → 手表 | 0、1 | `0x82`、版本号 `1` |
| | 2–5、6–7、8–19 | `uint32` UTC Unix 秒、`int16` 本地偏移分钟数（北京时间为 480）、十二个零 |

网关在每次认证连接订阅成功后写入校时帧。`TIME_SYNC sent` 表示 GATT 写入完成。手表以 `WATCH_TIME_SYNC` 记录 RTC 写入和 Flash 保存结果。

同一事件需有完整的联系人帧才发回执 1。重复通知可以重获回执。真实短信模式下，收到回执 1 后，手表可在断线重连或超时后低频重报同一事件来查询最终状态。状态 2 或 3 可能因重连先于状态 1 到达，手表可直接接受最终状态。B01 与手表的 BLE 连接、加密和实发均需按上面的步骤在实机验收。
