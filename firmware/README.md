# 黄山派老人求助手表固件

目标板为 `sf32lb52-lchspi-ulp`，基于 SiFli SDK v2.5.2 和 LVGL v9。待机页显示 RTC 日期和 24 小时时钟。IMU 检出疑似跌倒后切到红底倒计时页，取消或倒计时结束后回到时钟。串口演练显示「演示提醒」。28 px 中文字号和 82 px 高按钮适配 390×450 面板。

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
3. 输入 `watch_demo` 检查声振和倒计时，不会提交 SOS。第一次按 KEY2 验证取消。第二次让倒计时走完，核对 10 秒停振、30 秒结束提醒。IMU 实际检出时可查 `WATCH_FALL candidate` 和 `WATCH_SCREEN fall`。
4. 进入蓝牙联调时设置保留测试号码 `watch_contacts +12025550100 +12025550101`。用 `watch_pair` 开启 60 秒配对窗口，在 Jetson 输入手表显示的六位配对码。用 `watch_bench` 验证带测试号码的倒计时取消和超时上报，详细步骤见 [Jetson 接入说明](../jetson/README.md)。
5. 测试完成后用 `watch_contacts clear` 清空联系人设置。使用真实联系人前，为 Jetson 配好账号、短信签名、模板和号码白名单，并用已同意的测试联系人验证接收回执。

看到 `--:--` 或需要校时时，通过串口执行 `show_date 年 月 日 时 分 秒`，再输入 `show_date` 读回本地时间。

中文字体为 SDK 附带的 Noto Sans SC 字形子集，许可见 `Font-OFL.txt`。构建脚手架来自 SiFli SDK BLE 外围设备示例。
