# DeviceCore

2026-11-09 用户定（DAQ 第二步）：删除只读原型的 start 重载，start 只收调用方新造、独占的可写 Collector；启动不再 makeSelf，看门狗重试仍 makeSelf。

2026-11-09 用户定：Device 新增 `start(std::shared_ptr<CollectorT>, done, lock)` 重载，接收调用方新造、独占的可写 Collector 直接装入运行槽，不在启动时 makeSelf；看门狗重试仍 makeSelf。原只读原型重载保留给尚未改造的 DAQ，改造完成后删除。准入、替换与看门狗语义不变。

设备运行基类、Collector 收集契约、DeviceLock 和运行槽。Windows / Linux，C++20，CMake ≥ 3.28。

## 边界与依赖

- Qt ≥ 6.8.3；Qt 由使用环境提供，不在仓库中打包。
- 上游：[HandleCore](https://github.com/Lateef-ZHOU/HandleCore)。已有同名 CMake 目标时复用；否则 FetchContent 拉取固定完整提交。
- 本仓库不依赖 LiDAR_DataACQ 源码、应用目录或设备型号；不包含第三方二进制。
- 仅依赖 Qt Core 与 HandleCore 基础模块，不启用串口、Pcap、仿真传输，也不依赖 Qt GUI/QML。
- 原型与完成结果只读共享；运行区由 makeSelf 独立创建。派生设备析构须先停止访问派生状态的执行源，再清理状态。

## 继承、所有权与线程契约

Device<H,C> 继承 H 与非 QObject 的 DeviceBase，直接重写上传 onData；具体官邸提供 final Qt 末端，构造时接收名称、onStream 与 onConsume 回调。流回调在 Handle 线程执行，完成消费在主线程先于等待器通知执行；无需消费的族传空消费回调。Device 不认识设备族，不再提供 onCompleted/onStream 业务虚口。

拥有者在自身派生状态销毁前删除 Device；末端析构首句调用 PreDestruction，先同步停止 Handle，再撤销运行/锁。中间 Device/传输层析构不重复执行预析构；移除 retiring，名单通过 QObject::destroyed 在族对象清理前移除。QObject 父子所有权仅作构造失败兜底。open/close 是继承的连接入口，start/send/stop 是采集与发令入口，均为 C++ 入口，不是 Qt 槽。setOnConsume 仅用于首次启动前的同步安装和关闭后的主线程清空，不支持运行中并发更换。

2026-11-02：Handle 的 connectionChanged(ResourcePhase) 中，AboutToRelease 在 stop 促断前同步触发 Device 撤单/释放锁。DeviceBase 声明纯虚 lockChanged(ResourcePhase)，Qt 末端直接以信号实现，只在锁槽实际变化后发 Acquired/Released；重复取得同一锁或重复归还不另通知。删除 statusChanged 和 finished 中转。isOpen/isLocked/holder 为按需回读属性，不把枚举冒充布尔属性的 NOTIFY，不保存另一份连接或锁状态。迟到的完成阶段不能撤掉新任务，已完成帧仍消费，退出不新增排空。

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

2026-11-02 本地验证：Windows 主程序 Release 与生命周期/三任务回归 7/7 通过，Linux Qt Core 6.8.3 独立构建、公开头、CTest 1/1 通过。本轮依赖提交尚未推送；本地联调通过 FetchContent 源目录覆盖供入对应 HandleCore。


2026-11-03 验证：Linux Qt Core 6.8.3 独立构建、公开头及 module_smoke 1/1；Windows Qt 6.11.2 主程序 Release、生命周期/等待器/任务回归 7/7 通过。主程序与 15 组 QML 检查通过。新依赖为相邻仓库的本地提交，尚未推送；联调使用 FetchContent 源码覆盖。

## 源码目录（2026-11-03）

公开文件直接放在 `include/`，引用时使用文件名，不再携带原单仓库的业务目录前缀。CMake 目标名称、C++ 类型和 QML 模块名保持不变；使用方须同步更新头文件路径及固定依赖版本。

目录整理验证（2026-11-03）：Windows Qt 6.11.2 / MSVC：公开头独立编译、CTest 1/1 通过。本轮通过本地源码覆盖验证，固定依赖同步更新到本地提交；尚未推送，不代表已验证远端拉取。

## 2026-11-04：设备日志直接接线（用户定）

DeviceBase 删除 logSourceObject 契约，Device 删除 logSource() 自引用读口。设备机制层直接调用继承自 Handle 的 logMessage，不增设日志源、转发或状态。 本轮仅本地整理，未推送；Windows 集成验证记录见主程序维护操作。

## 2026-11-04：设备身份与占用查询（用户定）

DeviceBase 在构造时接收名称与副标题，保存为 public const QString 成员；Device 模板不再重复保存。占用查询合为 optional<QString> holder()，nullopt 表示空闲，空名称仍表示占用；锁身份和验锁行为不变。Qt 具体设备用 MEMBER 暴露名称常量，holder 的 Qt 边界用无效 QVariant / QString 区分空闲与占用，不另存状态。

本轮仅本地整理；集成验证记录见主程序 docs/maintenance/validation.md，不推送。

## 2026-11-04：简化重复加锁判断（用户定）

tryLock 先执行 canEngage，再用 lock_ == lock 同时覆盖同一令牌和双方为空；删除冗余 !lock 分支与长篇注释。未连接、异锁拒绝及实际加锁时的 Acquired 通知保持不变。仅本地整理，不推送。

## 2026-11-04：释放锁只保留验锁（用户定）

releaseLock 验证令牌后直接调用 dropLock，不为已有幂等行为增加空槽判断；空槽不摇铃、不发 Released 的保证统一由 dropLock 实现。释放和通知行为保持，未推送。


2026-11-04：Device 删除 HandleType、FeedType 简单别名，收包回调直接使用 CollectorT::Feed。CollectorType 暂留：改为族模板显式接收 Collector 类型后，MSVC 对结构体模板参数与协变返回的组合报错；已独立复现。用户确认暂留 CollectorType，避免扩大改动，不增加编译期类型提取或调整协变接口。qmlHolder、收发及运行行为不变，未推送。


## 2026-11-04：Handle 连接入口统一（用户定）

Device 删除 open/close 薄转发，直接继承 Handle 的连接入口；isOpen 保留为 DeviceBase 契约实现。采集 start/stop 不改。setOnConsume 暂留，仍只用于处理线程交接期间安装及关闭后的清空，族基和官邸后续整理时再审查。

验证：Windows Qt 6.11.2/MSVC 的公开头和独立构建、CTest 1/1 通过（build-identity/open-close-build.log）。HandleCore 固定本地 9d74a19b91494354b3709bed04603a61ed3408bd，联调用相邻源码覆盖；未推送。


## 2026-11-04：start 在线替换（用户定）

验证：Windows/MSVC/Qt 6.11.2 主程序增量构建和八秒离屏冒烟通过；主程序生命周期专项 `start_replacement` 通过，覆盖准入顺序、同锁替换、正/非正新时限、发送失败、旧完成消息和在途收集被替换。不代表 Linux 或硬件验证。

先 canEngage 验连接/锁，再检查原型非空；删除已有 run 的拒收。通过准入即可直接 store 新状态，未完成旧请求被替换、不额外下发旧停机或通知旧回调；调用方负责旧等待寿命。worker 已 CAS 摘走的完成帧仍按原消费/通知链交付。

删除 start 开头无条件停看门狗：新时限为正时直接 start(ms) 重置，非正时 stop 撤掉旧表，停表/起表合并为 if/else，统一位于 HandleT::send 之后。发送失败只由 Handle 报告，不再在 start 内 CAS 撤销；返回 true 只表示请求已接受，非正时限不保证传输可靠性。主线程同步启动过程不泵事件，不新增运行状态、锁或转发。
2026-11-04 用户定：删除 Device.rawSend；命令列表发送归 HandleCore 的 send(QList<QByteArray>) 重载。Device 的验锁发送、采集启动、重试和撤单直接调用 HandleT::send，保留空表零发送、首败短路及当前启动/取消语义。

## 2026-11-04：进料在消费端取值（用户定）

Device::onData 将 onStream 返回的 optional 原样交给 Collector::feed。feed 统一拦截空输入，并保证完成后不再进料；非空 optional 继续原样传入 onFeed，由实际消费端取值。run_ 仍为 atomic<shared_ptr<const State>>，teardown 的 exchange 原子摘槽，不增加锁或状态。

本轮仅本地整理，不推送；集成验证见主程序维护记录。
