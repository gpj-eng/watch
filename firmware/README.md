# 黄山派老人求助手表固件

目标板为 `sf32lb52-lchspi-ulp`，基于 SiFli SDK v2.5.2 和 LVGL v9。一级表盘以 120 px 等宽布局显示 24 小时时钟，以 30 px 字体显示日期和星期。短按 KEY1 切到二级功能页，二级页提供长按求助。长按 KEY1 约 3 秒或手表静置约 15 秒进入息屏监测。抬腕、按键、疑似跌倒和手动 SOS 均可点亮屏幕。

疑似跌倒确认后，马达和喇叭立即启动。马达的 1 kHz、50% PWM 在 10 秒后停止。喇叭间歇响铃至用户取消或 30 秒倒计时结束。在这 30 秒内按 KEY2 或点屏幕上的「我没事」会关闭声振并取消提交。倒计时结束后将 SOS 写入闪存队列，转发给 Jetson。待机时长按 KEY2 约 1.2 秒或长按屏幕按钮可直接提交手动 SOS。

时长由项目确认。设计比较了智能手表声音/触觉提醒的取消研究和 Apple Watch 的 30 秒跌倒倒计时。研究对象和场景与本项目不同。10 秒是本项目的马达提醒时长，30 秒是取消窗口，需要在目标用户试戴时测量按键或点击的实际完成时间。[取消窗口试点研究](https://pmc.ncbi.nlm.nih.gov/articles/PMC12835406/) · [Apple Watch 跌倒检测](https://support.apple.com/guide/watch/manage-fall-detection-apd34c409704/watchos)

板载 LSM6DS3TR-C 按 52 Hz 配置。当前规则以冲击、方向变化和约 3 秒静止生成疑似跌倒事件。阈值只用于原型，应使用腕部样本量化漏报和误报。单独振动前先开片上 LDO3，再使能 PA20 的 PWM。手机号通过 FlashDB 存在板载 NOR 分区中，最多三位。BLE 使用带配对认证的加密链路，待发送事件持久化，重发沿用同一事件编号。短信由 Jetson 执行；「接收端已收」表示已入库，「短信服务已受理」表示提供商接收请求。

## 编译

按 SiFli SDK v2.5.2 的安装和导出流程准备开发环境，在 `firmware/project` 运行：

```powershell
scons --board=sf32lb52-lchspi-ulp -j8
```

在生成的 `build_sf32lb52-lchspi-ulp_hcpu` 中使用官方 `uart_download.bat` 下载。若工作区已有旧固件，需要保留旧固件镜像以便回退。Flash 分区和下载地址以生成的 `ImgBurnList.ini` / `uart_download.bat` 为准。UI 入口采用官方板卡支持的 `littlevgl2rtt_init("lcd")`。[SiFli LVGL v9 示例](https://docs.sifli.com/projects/sdk/latest/en/sf32lb52x/example/multimedia/lvgl/lvgl_v9_examples/README.html)

## 现场联调

1. 接 USB 数据线和 SPK 喇叭。COM 号在设备管理器查看。串口助手设 1000000、8N1、无流控，命令末尾发送回车。
2. 输入 `watch_status`。先检查 `imu=1 flash=1 display=1`，重复查询看 `samples` 增加。
3. 输入 `watch_demo` 检查本机演练。按 KEY2 验证取消，也可以让倒计时走完测试后续报警页面。IMU 实际检出时可查 `WATCH_FALL candidate` 和 `WATCH_PAGE alarm`。
4. 进入蓝牙联调时设置保留测试号码 `watch_contacts +12025550100 +12025550101`。用 `watch_pair` 开启 60 秒配对窗口，在 Jetson 输入手表显示的六位配对码。用 `watch_bench` 验证带测试号码的倒计时取消和超时上报，详细步骤见 [Jetson 接入说明](../jetson/README.md)。
5. 测试完成后用 `watch_contacts clear` 清空联系人设置。使用真实联系人前，为 Jetson 配好账号、短信签名、模板和号码白名单，并用已同意的测试联系人验证接收回执。

## 时钟与 KEY1 验收

1. 启动更新后的配对网关。Jetson 打印 `TIME_SYNC sent` 后，以手表串口的 `WATCH_TIME_SYNC source=ble rtc=ok flash=saved` 确认首次校时落盘。近期重复校时可以得到 `flash=unchanged` 或 `flash=deferred`。Jetson 以北京时间为手表校时。长时间使用时，手表每六小时保存一次有效时间检查点。
2. 暂停网关，保持供电，执行五次 `reboot`。每次确认 `WATCH_TIME_BOOT source=rtc`、`WATCH_PAGE dial`，检查首屏日期、星期和时间。若 RTC 在某次重启后失效，应看到 `source=flash`，且仍显示有效时间。校时前的新环境若没有有效 RTC 和 Flash 记录，应看到 `source=build`，表盘小点提示尚未完成同步。
3. 在纯 USB 和电池供电条件下分别记录 `WATCH_POWER`。短按 KEY1 两次，检查 `WATCH_KEY1 short`、`WATCH_PAGE function`、`WATCH_KEY1 short`、`WATCH_PAGE dial`。两页各确认一次求助按钮的位置。
4. 长按 KEY1 约 3 秒后松手，检查 `WATCH_KEY1 long mode=monitor_sleep`、`WATCH_PAGE sleep`。短按可亮屏回表盘。重复息屏并输入 `watch_demo`，核对屏幕点亮、`WATCH_PAGE alarm`。报警期间按 KEY1 应记录 `WATCH_KEY1 ignored=alarm`。按 KEY2 取消，或等待本机报警流程结束，应回到 `WATCH_PAGE dial`。纯 USB 若长按无响应，记录 `vbus=1` 和按键实际表现。

串口的 `show_date` 用于读出 RTC。硬件完全断电时，表盘从 Flash 保存的值恢复走时。断电期间的时间由 Jetson 在再次连接时校准。

## 大数字表盘与息屏验收

- 一级表盘使用独立的 120 px Montserrat Bold 数字子集和 30 px Noto Sans SC 日期子集，背景由 LVGL 绘制双色垂直渐变。确认 `11:11` 和 `20:08` 均居中，一级页没有求助按钮。
- `CONFIG_WATCH_SCREEN_TIMEOUT_MS` 位于 `firmware/project/proj.conf`，默认 15000。无按键操作，且合加速度在 0.90–1.10 g、合角速度不超过 12°/秒时开始计算静止时长。静置至黑屏后，串口应记录 `WATCH_DISPLAY sleep reason=idle`。用两次 `watch_status` 查看 `samples` 继续增长，已配对时同时查看 Jetson 持续接收六轴样本。
- 屏幕朝上静放时，`watch_pose` 的 `face_up` 应接近正 1000 mg。现有配置按传感器负 Z 方向为朝上。抬腕应从表盘偏离朝上超过约 55° 开始，X/Y 合角速度连续两个样本达到 80°/秒。随后在 800 ms 内翻到朝上约 40° 以内，并满足 0.8–1.2 g、合角速度不超过 18°/秒连续八个样本。亮屏时应记录 `WATCH_DISPLAY wake reason=wrist` 和 `WATCH_PAGE dial`。用日常走路摆臂检查是否误亮。
- 息屏将 AMOLED 亮度设为零并暂停 LVGL 重绘，触控供电保持开启。分别在息屏时按 KEY1 和 KEY2，应立即回到一级表盘。再次息屏，输入 `watch_demo`，确认 `WATCH_DISPLAY wake reason=alarm` 和 `WATCH_PAGE alarm`，短点屏幕上的「我没事」，确认 `WATCH_TOUCH cancel`、`WATCH_CANCELLED` 和 `WATCH_PAGE dial`。从表盘短按 KEY1 进入功能页，长按屏幕上的「长按求助」，确认 `WATCH_TOUCH manual`。这项验证使用未配置紧急联系人的测试表。

中文字体为 SDK 附带的 Noto Sans SC 字形子集，许可见 `Font-OFL.txt`。构建脚手架来自 SiFli SDK BLE 外围设备示例。
