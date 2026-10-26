# DeviceCore

设备运行基类、Collector 收集契约、DeviceLock 和运行槽。Windows / Linux，C++20，CMake ≥ 3.28。

## 边界与依赖

- Qt ≥ 6.8.3；Qt 由使用环境提供，不在仓库中打包。
- 上游：[HandleCore](https://github.com/Lateef-ZHOU/HandleCore)。已有同名 CMake 目标时复用；否则 FetchContent 拉取固定完整提交。
- 本仓库不依赖 LiDAR_DataACQ 源码、应用目录或设备型号；不包含第三方二进制。
- 仅依赖 Qt Core 与 HandleCore 基础模块，不启用串口、Pcap、仿真传输，也不依赖 Qt GUI/QML。
- 原型与完成结果只读共享；运行区由 makeSelf 独立创建。派生设备析构须先停止访问派生状态的执行源，再清理状态。

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
