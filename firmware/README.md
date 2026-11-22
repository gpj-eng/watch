# 黄山派老人求助手表固件

目标板为 `sf32lb52-lchspi-ulp`，基于 SiFli SDK v2.5.2 和 LVGL v9。一级表盘在全屏 EZIP 图片上以 120 px 等宽布局显示 24 小时时钟，以 30 px 字体显示日期和星期。短按 KEY1 切到二级求助页，可长按求助或点按「切换壁纸」。静置约 15 秒进入息屏监测，按键、疑似跌倒和手动 SOS 均可点亮屏幕。KEY1 按住 1 秒显示关机圆环，约 3 秒达到满圈并熄屏，松手后进入 Hibernate。期间处理器、跌倒监测与 BLE 停止，RTC 依靠 VBAT 继续走时。再次长按 KEY1 约 1 秒开机。

疑似跌倒确认后，马达和喇叭立即启动。马达使用 1 kHz、50% PWM，前 20 秒震 600 ms、停 900 ms，后 10 秒震 500 ms、停 400 ms。在这 30 秒内按 KEY2 或点屏幕上的「我没事」会关闭声振并取消提交。倒计时结束后，原 SOS 写入闪存队列并转发给 Jetson。手表同时切到震 300 ms、停 700 ms，并循环播放三连警报音和离线语音「我需要帮助」。按住 KEY2 3 秒解除，松手不足 3 秒会重置进度。解除后声振立即停止，屏幕提示约 2 秒后回到一级表盘。待机时长按 KEY2 约 1.2 秒或长按屏幕按钮可直接提交手动 SOS。

前 30 秒为误报取消窗口，仍需在目标用户试戴时测量按键或点击的实际完成时间。[取消窗口试点研究](https://pmc.ncbi.nlm.nih.gov/articles/PMC12835406/) · [Apple Watch 跌倒检测](https://support.apple.com/guide/watch/manage-fall-detection-apd34c409704/watchos)

板载 LSM6DS3TR-C 按 52 Hz 配置。当前规则以冲击、方向变化和约 3 秒静止生成疑似跌倒事件。阈值只用于原型，应使用腕部样本量化漏报和误报。单独振动前先开片上 LDO3，再使能 PA20 的 PWM。手机号通过 FlashDB 存在板载 NOR 分区中，最多三位。BLE 使用带配对认证的加密链路，待发送事件持久化，重发沿用同一事件编号。短信由 Jetson 执行；「接收端已收」表示已入库，「短信服务已受理」表示提供商接收请求。

## 编译

按 SiFli SDK v2.5.2 的安装和导出流程准备开发环境，在 `firmware/project` 运行：

```powershell
scons --board=sf32lb52-lchspi-ulp -j8
```

在生成的 `build_sf32lb52-lchspi-ulp_hcpu` 中使用官方 `uart_download.bat` 下载。若工作区已有旧固件，需要保留旧固件镜像以便回退。Flash 分区和下载地址以生成的 `ImgBurnList.ini` / `uart_download.bat` 为准。UI 入口采用官方板卡支持的 `littlevgl2rtt_init("lcd")`。[SiFli LVGL v9 示例](https://docs.sifli.com/projects/sdk/latest/en/sf32lb52x/example/multimedia/lvgl/lvgl_v9_examples/README.html)

## 替换表盘壁纸

[壁纸目录](assets/wallpapers/)内置星空、雪山、海岸。准备 2 至 5 张不透明的 390×450 PNG，按 `wallpaper_01.png` 起连续编号。替换同名文件就会保留原有位置。可以在同目录的 `wallpapers.json` 改名称。要显示未内置的汉字，需要把相应字形加入 `watch_wallpaper_font`。

Windows 上执行正常编译命令即可一键转换。SCons 使用已初始化的 SDK 中的 `tools\png2ezip\ezip.exe`，对每张 PNG 运行 `-convert <原图> -rgb565 -cfile 2 -lvgl_version 9 -chip sf52x -outdir <输出目录>`，再把生成的 LVGL v9 资源表自动编入 `main.bin`。可在仓库根目录用下列命令单独检查转换，省略 `--sdk-root` 时使用 `SIFLI_SDK` 环境变量。

```powershell
python firmware/tools/convert_wallpapers.py --out "$env:TEMP/watch_wallpapers_generated.c"
```

1. 用自定义 PNG 替换 `wallpaper_02.png`，重新构建。检查构建日志里的各图 EZIP 字节数，并确认 `main.bin` 在 8 MiB 的 HCPU 分区内。
2. 根据生成的烧录清单核对 HCPU 地址，备份旧镜像后烧录并校验。保留存放壁纸索引和已有配置的 FlashDB 分区。
3. 短按 KEY1 进入求助页，再点「切换壁纸」轮换到第 2 张。核对编号和名称，短按 KEY1 回表盘核对全屏图片。继续点击可验证末张回到首张。
4. 保持第 2 张，用 KEY1 关机后重新开机，再完全断电重启一次。两次都应显示第 2 张，串口应记录 `WATCH_WALLPAPER load index=1 ... saved=1`。日志索引从零开始计数。

## 功能菜单与触控验收

1. 短按 KEY1 进入「长按求助」。在这一页从右向左划到功能菜单，从左向右划回表盘。左上返回按钮也能回表盘。触摸屏未滚动时，可分辨约 30 px 的水平滑动。
2. 点按进入设置、壁纸或天气。各子页右滑或者点左上按钮返回上一级。在能完整显示的页面上下滑，画面应留在原位。可加入第五张壁纸测试图库溢出时上推查看下面的缩略图，下拉回顶部，期间页头和「应用壁纸」固定。
3. 在壁纸图库先选中缩略图，再点「应用壁纸」，返回表盘查看全屏结果。返回前若未应用，表盘继续显示上一次保存的壁纸。
4. 在设置中测试联系人键盘、音量试听和亮度。11 位国内手机号会添加 `+86`，国际号码可直接用 `+` 开头。测试联系人时请用保留号码并在联调后清除。滑动音量和亮度调节条应就地调整，松手保存，重启后可核对。亮度范围为 10% 至 100%，普通试听使用通知音量。
5. 天气页当前显示无数据状态，预留今、明、后三天位置。滑动时检查求助页按钮和菜单按钮都没有被误触发。

## 现场联调

1. 接 USB 数据线和 SPK 喇叭。COM 号在设备管理器查看。串口助手设 1000000、8N1、无流控，命令末尾发送回车。
2. 输入 `watch_status`。先检查 `imu=1 flash=1 display=1`，重复查询看 `samples` 增加。
3. 输入 `watch_demo` 检查本机演练。按 KEY2 验证取消，也可以让倒计时走完测试后续报警页面。IMU 实际检出时可查 `WATCH_FALL candidate` 和 `WATCH_PAGE alarm`。
4. 进入蓝牙联调时设置保留测试号码 `watch_contacts +12025550100 +12025550101`。用 `watch_pair` 开启 60 秒配对窗口，在 Jetson 输入手表显示的六位配对码。用 `watch_bench` 验证带测试号码的倒计时取消和超时上报，详细步骤见 [Jetson 接入说明](../jetson/README.md)。
5. 测试完成后用 `watch_contacts clear` 清空联系人设置。使用真实联系人前，为 Jetson 配好账号、短信签名、模板和号码白名单，并用已同意的测试联系人验证接收回执。

## 时钟与 KEY1 验收

1. 升级后首次校时前，或彻底断电导致 RTC 无效后，表盘时间显示 `--:--`，收到有效的北京时间后显示日期和时间。保持电池供电时，后续重启和 Hibernate 唤醒直接显示有效的硬件 RTC 时间。报警拥有显示优先级。
2. 启动已配对的 Jetson 网关，或从仓库根目录运行 `python -m firmware.tools.sync_pc_clock 手表蓝牙地址`（需安装 Bleak 1.0 以上版本）。首次在 Windows 配对时，先用串口命令 `watch_pair` 打开窗口，再在 Windows 蓝牙设置添加 `HSPI-SOS` 并输入手表显示的六位配对码。手表端以 `WATCH_TIME_SYNC source=ble rtc=ok` 确认硬件已校准。停用校时端，保留电池供电并重启，应看到 `WATCH_TIME_BOOT` 中的 `display=rtc` 和 `overwritten=0`。彻底断电后重新上电，应看到 `display=wait_ble`，连接后对照发送设备确认表盘时间。
3. 在纯 USB 和电池供电条件下分别记录 `WATCH_POWER`。短按 KEY1 两次，检查 `WATCH_KEY1 short`、`WATCH_PAGE function`、`WATCH_KEY1 short`、`WATCH_PAGE dial`。在二级页确认求助按钮的位置。
4. USB 实测时，先按住 KEY1 约 2 秒后松手，确认出现「长按关机」圆环，松手回到原页面。再按住约 3 秒，等满圈熄屏即松手，串口应记录 `WATCH_KEY1 power=confirmed` 和 `WATCH_HIBERNATE enter`。关机后先短按确认屏幕保持黑屏，再长按约 1 秒，确认开机日志显示 `reason=hibernate`。
5. 验证电池保持 RTC 时，完成一次校时，维持锂电池连接并拔掉 USB，停用校时端。用 KEY1 长按关机，等 180 秒，再长按开机。表盘应直接显示继续走过这段时间后的读数。用 `watch_clock` 核对 `reason=hibernate`、`display=rtc` 和硬件时间。若接回 USB 触发了另一次复位，通过独立的 3.3 V 串口适配器只接地和监听 RX，连续记录 `WATCH_HIBERNATE enter` 与唤醒日志。

## 超时报警与本机解除验收

1. 接好喇叭，串口输入 `watch_demo` 做纯本机演练。等满 30 秒，查 `WATCH_SOS_TIMEOUT`、`WATCH_SOS_AUDIO … volume=12 pa42=1` 和 `WATCH_VIB` 边沿。听两轮三连警报音和「我需要帮助」，确认约 300 ms 震、700 ms 停。默认音量为 12/15 档，参数为 `alarm_cfg_default.sos_audio_volume`。
2. 按 KEY2 约 1 秒后松手，确认圆圈隐藏，串口出现 `WATCH_KEY2_HOLD … state=reset`，声振继续。
3. 再按住 KEY2 约 3 秒，确认满圈后立刻停声振，串口出现 `WATCH_SOS_AUDIO … muted pa42=0` 和 `WATCH_RESOLVED`，约 2 秒后回到 `WATCH_PAGE dial`。

`watch_demo` 不提交 SOS。原 SOS 联调沿用 `watch_bench` 与保留的测试联系人。真实跌倒 SOS 的本机解除记录关联原 SOS 编号并单独保存。当前固件的 `WATCH_SOS_RESOLVED_UPLINK` 默认关闭，适合继续与旧网关联调。

串口的 `show_date` 可读取当前 RTC。蓝牙首次校时是表盘启用的依据。此后在电池维持 RTC 期间，表盘可以离线走时。

## 大数字表盘与息屏验收

- 一级表盘使用独立的 120 px Montserrat Bold 数字子集和 30 px Noto Sans SC 日期子集，背景由 LVGL 绘制双色垂直渐变。确认 `11:11` 和 `20:08` 均居中，一级页没有求助按钮。
- `CONFIG_WATCH_SCREEN_TIMEOUT_MS` 位于 `firmware/project/proj.conf`，默认 15000。无按键操作，且合加速度在 0.90–1.10 g、合角速度不超过 12°/秒时开始计算静止时长。静置至黑屏后，串口应记录 `WATCH_DISPLAY sleep reason=idle`。用两次 `watch_status` 查看 `samples` 继续增长，已配对时同时查看 Jetson 持续接收六轴样本。
- 垂腕熄屏与跌倒检测共用已读取的 52 Hz 六轴样本。疑似跌倒倒计时、SOS 报警、手动 SOS 通知和解除提示期间，报警页面保持点亮。
- 息屏将 AMOLED 亮度设为零并暂停 LVGL 重绘，触控供电保持开启。分别在息屏时按 KEY1 和 KEY2，应立即回到一级表盘。再次息屏，输入 `watch_demo`，确认 `WATCH_DISPLAY wake reason=alarm` 和 `WATCH_PAGE alarm`，短点屏幕上的「我没事」，确认 `WATCH_TOUCH cancel`、`WATCH_CANCELLED` 和 `WATCH_PAGE dial`。从表盘短按 KEY1 进入功能页，长按屏幕上的「长按求助」，确认 `WATCH_TOUCH manual`。这项验证使用未配置紧急联系人的测试表。

## 垂腕熄屏标定与测试

1. 串口设为 1000000、8N1。表盘朝上水平静放，输入 `watch_rw cal left flat`。自然垂臂后输入 `watch_rw cal left down`，抬起手腕到平时看时间的角度后输入 `watch_rw cal left view`。每次保持姿势约一秒，保存 `RW_CAL` 的三轴均值（g）和 `RW_END`。右手换成 `right`。若 `stable=0`、采样不足 52 帧或有丢帧，请重复这一姿态。
2. 平放时原始 Z 应接近 −1 g，固件将其换算为表盘朝上的正值。垂臂并让手腕伸直时，检查原始 X、Y 哪一轴最接近 ±1 g，可用 `watch_rw axis x` 或 `watch_rw axis y` 选择前臂轴。`watch_rw` 显示当前设置，重启时恢复固件默认值。
   本表左手佩戴各取 52 个样本，原始 (X, Y, Z) 垂臂均值为 (+0.121, −0.994, −0.129) g，看表均值为 (−0.564, −0.132, −0.803) g，据此默认前臂轴为 Y。换人或改变佩戴方向后重新测量。
3. 按键点亮后，先看表再垂腕。看表窗口要求滤波后 Z ≥ 0.70 g 且前臂轴绝对值 ≤ 0.65 g。之后 Z ≤ 0.35 g 连续 16 帧时，屏幕熄灭，串口记录 `WATCH_DISPLAY sleep reason=wrist_down`。加速度 EMA 系数为 0.20。
4. 可用 `watch_rw trace 120` 记录逐帧姿态。跟踪行中 `a` 为原始加速度（mg），`w` 为原始角速度（0.1°/s），`g` 为修正表盘方向并滤波后的重力（mg），`n` 为垂腕计数，`evt=SLEEP` 为熄屏请求，`screen=2` 表示报警保护。实际熄屏以 `WATCH_DISPLAY` 为准。`RW_END` 汇总采样数、队列丢帧与采样间断。
5. 使用本机演练 `watch_demo`，分别在疑似跌倒倒计时和超时进入 SOS 页面后垂腕超过一秒。两个页面都应保持可见。演练结束回到表盘后，再测试一次垂腕熄屏。

中文字体为 SDK 附带的 Noto Sans SC 字形子集，许可见 `Font-OFL.txt`。构建脚手架来自 SiFli SDK BLE 外围设备示例。
