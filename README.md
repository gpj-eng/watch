# Watch

黄山派手表固件及 Jetson 网关项目。

- `firmware/`：手表固件源码、构建配置及核心测试。
- `jetson/`：BLE 数据接收、SOS 记录和短信网关；使用方法见 [Jetson 接入说明](jetson/README.md)。
- `.motor-debug/`：电机调试脚本及开发板参考资料。
- 内部原型方案与采购清单不纳入版本库，仅保留在本地。

本机 SDK、工具链、编译产物、日志和 Python 缓存通过 `.gitignore` 排除，保留在本地。
