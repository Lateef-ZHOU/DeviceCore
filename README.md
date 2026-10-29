# DeviceCore

设备运行基类、Collector 收集契约、DeviceLock 和运行槽。Windows / Linux，C++20，CMake ≥ 3.28。

## 边界与依赖

- Qt ≥ 6.8.3；Qt 由使用环境提供，不在仓库中打包。
- 上游：[HandleCore](https://github.com/Lateef-ZHOU/HandleCore)。已有同名 CMake 目标时复用；否则 FetchContent 拉取固定完整提交。
- 本仓库不依赖 LiDAR_DataACQ 源码、应用目录或设备型号；不包含第三方二进制。
- 仅依赖 Qt Core 与 HandleCore 基础模块，不启用串口、Pcap、仿真传输，也不依赖 Qt GUI/QML。
- 原型与完成结果只读共享；运行区由 makeSelf 独立创建。派生设备析构须先停止访问派生状态的执行源，再清理状态。

## 组合与线程契约

Device<H,C> 是可直接组合的 final 机制对象，构造时接收名称、onStream 与 onConsume 回调。流回调在 Handle 线程执行，完成消费在主线程先于等待器通知执行；无需消费的族传空消费回调。Device 不认识设备族，不再提供 onCompleted/onStream 业务虚口。

拥有者在自身派生状态销毁前删除 Device；Device 析构先同步停止 Handle，再发 retiring 和撤销运行/锁。QObject 父子所有权仅作构造失败兜底。open/start/send/stop 是拥有者使用的 C++ 入口，不是 Qt 槽。setOnConsume 仅用于首次启动前的同步安装和关闭后的主线程清空，不支持运行中并发更换。

2026-10-29：Handle 在 stop 促断前同步发出 false，Device 此时撤单和释放锁，停机指令仍可发送。close 和换参 open 共用此路径，不再有旧的排队 false 撤掉新任务。线程 finished 仅触发 statusChanged 回读实际状态，不撤单、不缓存状态。已经完成的帧仍消费；关闭开始后才到达、尚未完成的帧随本轮撤销，不新增排空。

本轮验证：Windows Qt 6.11.2 / MSVC 与 Linux Qt 6.12.0 / GCC 独立构建、公开头与 CTest 1/1 通过；Windows 主程序设备生命周期回归通过（31.74 秒），覆盖同步撤单、换参重连、旧 finished 不取消新任务、看门狗继续重试及完成帧保留。Windows 独立构建从远端固定拉取 HandleCore 3470a1d；无实物验证。


## 独立构建和验证

安装对应 Qt 开发包，然后执行（将路径替换为本机 Qt 前缀）：

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/platform -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Windows 在安装 MSVC 的环境运行以上命令，Qt 选择对应 MSVC 构建；Linux 使用 GCC 与 OpenMP 开发环境。
测试逐个独立编译公开头、拒绝意外加载 GUI/QML，并运行契约检查。GitHub Actions 在 Windows/Linux 使用 Qt 6.11.2 验证；最低版本声明与最新验证版本是不同概念。

## 接入应用

```cmake
include(FetchContent)
FetchContent_Declare(lidar_devicecore
    GIT_REPOSITORY https://github.com/Lateef-ZHOU/DeviceCore.git
    GIT_TAG <经过验证的完整提交号>)
FetchContent_MakeAvailable(lidar_devicecore)
target_link_libraries(my_app PRIVATE Lidar::DeviceCore)
```

从仓库提交列表选择固定版本；示例占位值必须替换，不使用浮动分支保证可复现。多个模块共用已有上游目标，应用若需覆盖上游版本应先统一声明并创建它们。标准 `FETCHCONTENT_SOURCE_DIR_LIDAR_模块名`（模块名大写无分隔）可用于本地开发，发布验证应取消覆盖并实际拉取远端。

## 来源与许可

从 [LiDAR_DataACQ](https://github.com/Lateef-ZHOU/LiDAR_DataACQ) 的模块分离提交 `94ad6f8` 提取；初始分仓保持运行逻辑，独立整理构建、验证与发布入口。

自有代码采用 [MIT](LICENSE)，Copyright (c) 2026 周涛 (ZHOU TAO)。第三方依赖不随本仓库改为 MIT，参见 [依赖说明](THIRD_PARTY_NOTICES.md)。
