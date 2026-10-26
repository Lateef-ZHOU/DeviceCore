#pragma once
#include <concepts>
#include <functional>
#include <memory>
#include <utility>
#include <QByteArray>
#include <QList>
#include <QString>

#include "Device/Base/RxTime.h"

// ============================================================
// RunBase.h — 一次 run 的词汇底座：一个契约基类（Collector）+ 配套机制词
// （DoneFn<C> 完成回调 / DeviceLock 锁令牌 / CollectorFamily 族内判据 / CollectorLike 钉扣 / NoWatchdogMs）。
// 与邻居分层：HandleBase.h = 连接层契约，本文件 = 运行层契约，DeviceBase.h = 设备层实现，
// DeviceCoro.h = 协程编排层。
//
// 两条支柱：
// ① **一次 run 的一切都在 Collector 上**：命令（buildStart/buildStop）、时限（timeoutMs）、
//    攒料判齐（feed）、自造下一只（makeSelf）。一斗 = 一次尝试；换斗即重试，新斗由旧斗照自己的
//    **指示区**现造，参数遂不必另立一个类来存。
// ② **run 状态住 Device 的单槽，而本文件不认识那个状态**：斗与完成回调怎么装在一起、装在哪，
//    是设备内政（私有嵌套聚合 `Device::State` + 看门狗表，见 DeviceBase.h）。运行层契约对外
//    只有两件词汇——**一只斗**与**一个完成回调**，下单方交出这两件即止。状态在岗
//    ⟺ run 进行中，CAS 摘出即注销——摘状态一个动作同时取得投递权与结果所有权，**一道闸**。
// ============================================================

// ── DeviceLock：锁令牌持有占用者名称与取消槽，指针表示身份。
//    · 身份：**指针本身即 identity**——设备 lockMatches 比对的就是它（全链 shared_ptr，比对是
//      一次 `==`）；
//    · 占用者名称：构造时确定，随令牌存在；设备只读，不另存副本。
//    · 取消槽：槽**就是本类的成员**（一支可换弹的铃）。
//    协程的准备模块（`DeviceCoro::promise_type` 的构造函数）先造一枚、逐台
//    试锁，全员锁上才留下——故"帧里有令牌 ⟺ 本帧持锁"。
//    **放锁不在本类**：那是后处理模块（promise 的析构与 final_suspend）显式做的一件事——逐台
//    releaseLock；某台设备释放份额，不清空令牌上的名称。
//    arm / ring 均由主线程调用；ring 在调用线程同步执行回调，可能当场恢复协程。
//    const 仅限制换装回调的权限，不提供线程安全；主动关闭 / 用户停 / ~DeviceCoro 均沿主线程调用。
//    令牌可被多次触发，回调不在 ring 后移空。
//    **换弹权由 const 结构性把守，界线画在协程帧上**：`arm` 非 const、`ring` const；
//    **帧内**（promise / 等待器 / 协程体）持 `shared_ptr<DeviceLock>`，**设备一侧一律
//    `shared_ptr<const DeviceLock>`**——设备只调得动 ring，编译期就换不了弹（等待器要 arm 得了，
//    而这道守卫要挡的是设备）。
//    当前 AwaiterBase 安装的取消回调路径：
//        shared_ptr<const DeviceLock> → ring() → 铃 → Cell → resume
//    回调通过弱引用观测 Cell，等待器销毁后调用会静默返回；安全性来自回调实现，
//    DeviceLock 不检查捕获对象或代管它们的寿命。安装者须保证回调依赖存活或能够安全失效，
//    并允许同步重入及重复触发；取消回调不应向撤单或析构路径抛出异常。
class DeviceLock
{
public:
    explicit DeviceLock(QString holder) : holder_(std::move(holder)) {}

    // 令牌以对象地址标识身份；只共享份额，不复制或搬移令牌及其取消铃。
    DeviceLock(const DeviceLock &) = delete;
    DeviceLock &operator=(const DeviceLock &) = delete;
    DeviceLock(DeviceLock &&) = delete;
    DeviceLock &operator=(DeviceLock &&) = delete;

    [[nodiscard]] const QString &holder() const noexcept { return holder_; }

    // 换弹 [仅主]：安装者负责回调寿命与失效安全；AwaiterBase 每次挂起时装入取消铃。
    // **非 const 是设计**：见类注——它把"谁能换弹"变成编译期的事。
    void arm(std::function<void()> bell) { bell_ = std::move(bell); }

    // [仅主，同步调用] 主动关闭 / 停止 → 打断当前挂起的等待。**具名而非 operator()**：本类恒经 shared_ptr 触及
    // （设备与协程各持一份 shared），`(*p)()` 那种拼写既难读又是一次裸解引用，`p->ring()` 直陈其事。
    // 拷局部再调：促醒的协程可能同步开下一个 await 并当场换弹——被调的那支铃必须持过整调。
    void ring() const
    {
        if (const auto bell = bell_)
            bell();
    }

private:
    const QString holder_; // 随锁构造和销毁，不提供改名入口。

    // 初始为空；非空回调也可能已失效，不能据此判断是否仍在等待。随令牌销毁，无须另行清空。
    std::function<void()> bell_;
};

template <typename FeedT>
class Collector
{
public:
    using Feed = FeedT; // 本族喂给斗的那一口是什么（Device 据此定 onStream 的返回型）

    // 进料 + 判齐一口 [worker]：true = 已收齐（Device 随即 CAS 摘状态、投递交付）；false = 未齐
    // 继续收。公共入口非虚，CollectorLike 检查型号沿用本层 feed，不得用同名声明隐藏它。
    //     已齐 → 不再吃料，直接说齐；否则吃一口，再问一次齐没齐。
    // 型号遂只写两件事：`onFeed` 怎么吃、`settled()` 怎么算齐；**"吃完了返什么"不必各写一遍**。
    //
    // **前一道防不是防御，是承重**：`settled()` 问的是"数据此刻齐不齐"，而那是个**可回退**的
    // 状态——国耀卡的 `count_` 见到新帧号就清零。是这道闸保证了"一旦收齐就不再进料"，收齐的斗
    // 遂不会被后来的字节改写。今天 Device 判齐当场 CAS 摘桩、worker 此后见空槽即 idle，到不了
    // 这道闸；但"到不了"与"调不着"是两回事，这里要的是后者。
    // **代价明码**：每口料多算一次 `settled()`。各家的判据都是几条比较（缓冲长度 / 计数 / 两个
    // 时刻之差），而进料是百赫兹量级，这笔换"型号不必自己收尾"是划算的。
    // `at` = 这一口料的**到料时刻**（Handle 层交上来，网口是内核盖的章；见 RxTime 的长注）。
    // 谁用谁决定：采集卡族拿它盖帧时刻，云台族不用（判到位量的是**时长**，见 GMBCollector）。
    bool feed(const FeedT &item, const RxTime at)
    {
        if (settled())
            return true; // 已是成品：不再吃料（收齐的斗不可被后来的料改写）
        onFeed(item, at);
        return settled();
    }

    // 数据此刻是否收齐 [判齐后跨线程只读]；false 包括初始状态和正在收集，不能据此区分两者。
    // settled 只描述这只斗的数据，不能判断它是否是本轮请求的结果：输入也可能是上轮成品。
    // RunAwaiter 因此在提交时移空原型槽，只由完成回调填入本轮结果，不以旧输入的状态判齐。
    // **问的是"数据此刻齐不齐"，不是"曾经齐过"**：消费端要读的正是数据本身，履历对它没有意义。
    // 各家都从自己的收集区直接算——两张卡看缓冲长度 / 分片计数，云台看驻留时长。于是 `feed` 的
    // 返回值与本口读的是同一份状态，不设第二本账。
    // **失效方向也因此是保守的那一侧**：国耀卡新帧来了只清计数不清缓冲，万一计数回退，按状态
    // 判会说"没齐"（数据到不了 worker），而拿履历标记判则会喊"齐了"、把撕裂的一帧交出去。
    // **前提是收集区诚实**：实测量若是零初始化裸值，"没收到过"与"收到过 0"分不开，本口会让
    // 新斗谎报成品。收集区用带空态的量（optional、"首次达标时刻"）表达"还没有"，那一类误判
    // 结构性不成立。
    // 收包线程写在前，完成后不再进料；CAS 摘槽及排队交付后，主线程与处理线程各持只读份额。
    [[nodiscard]] virtual bool settled() const = 0;

    // 启动命令表 [主]：空表 = 本次尝试不需发送启动命令，连发口对空表零次循环。
    [[nodiscard]] virtual QList<QByteArray> buildStart() const = 0;

    // 本次尝试的时限 [主]：非正 = 不设表（见 NoWatchdogMs）。
    [[nodiscard]] virtual int timeoutMs() const = 0;

    // 停机命令表 [主]，**带默认**：空表 = 无停机协议，结构性不上线（连发口对空表零次循环）。
    // 给默认体而非纯虚，是为省掉"多数型号都要写一遍 return {}"那份样板。覆写方写 `override`，
    // 拼错名字遂当场编不过。
    [[nodiscard]] virtual QList<QByteArray> buildStop() const { return {}; }


private:
    // 可变点：**吃一口料**，就这一件事——校验不过就直接返回，吃下了就把状态更新掉。
    // **返 void 是刻意的**：齐没齐由 `settled()` 一处回答，不在此处再答一遍（两处答同一个问题
    // 就是两份账）。型号侧的副作用（如盖帧时刻）也写在本口里；皮保证了已齐之后本口不再被调。
    virtual void onFeed(const FeedT &item, RxTime at) = 0;

protected:
    Collector() = default; // 契约基类不单独成活
    // 基类份额（shared_ptr<const Collector>）确会上行，但销毁恒经控制块记下的**具体删除器**
    // （具体工厂创建 shared_ptr 时确定），从不经基类指针 delete——故非虚仍安全；protected
    // 继续封死裸删门（`delete (Collector *)p` 编不过）。
    ~Collector() = default;

    // 一只 Collector 表示一次尝试，禁止复制和移动。生产链以 shared_ptr 持有，
    // 具体型号收紧构造入口：createPrototype 只交只读轻量原型，makeSelf 才创建可写运行实例。
    // **拷一只斗 = 连收集区一起拷**，而收集区正是 worker 在写的那块内存：拷到半截的攒料、
    // 拷到一半的帧、以及一次货真价实的数据竞争。**移动同理且更险**——搬走一只在跑的斗，留下的
    // 是个僵尸，而 worker 手里那根指针还指着原地。斗是"一次尝试"本身，它没有搬家的语义。
    // makeSelf 正常返回时必须交出非空、独立的新对象，保留不可变指示区并备齐初始收集区；
    // 不以空份额报告失败，也不能返回原对象。DoneFn 的空结果表示取消/失败，与工厂返回值不同。
    // const 不限制读取哪些字段；实现不得读取或复制 worker 正在写的收集区，
    // 才能与进料并发。工厂直接返回新对象的共享份额，不拷贝或移动已有 Collector。
    // 两扇赋值门一并 delete：原地换值毫无意义——换斗即换对象。
    Collector(const Collector &) = delete;
    Collector &operator=(const Collector &) = delete;
    Collector(Collector &&) = delete;
    Collector &operator=(Collector &&) = delete;

    // 各型号私有保存收集产物；本层不提供统一的数据读口。
};

// ── DoneFn<C>：run 完成回调——**唯一实参 = 收齐的那只斗**：交付路促发（斗份额）、停止路促发
//    （空份额）。
//    机制层的 C 是具体型号 Collector；族回调直接 move 交入，std::function 适配可调用签名，
//    调用时具体 shared_ptr 隐式上转族基，不手写只转参的 lambda。
//    等待器交回来的名单保留族类型，不读取型号帧数据，也不再转回设备消费。
//    这里只需 `CollectorFamily`（继承 Collector<Feed>）；工厂与进料入口由接入流水线时的
//    `CollectorLike` 检查，完成回调不调用它们。
//    **结局即载荷，不设两份账**：份额非空 ⟺ 这次 run 成了。二者恒完全相关（交付路的斗必非空
//    ——CAS 成功时 expected 不被改写；停止路必空），故再挂一枚结局枚举就是第二份账，两账一旦
//    打架谁也说不清以哪份为准。超时不是一种结局（看门狗内政化重试，对 run 不可达）；拒收也
//    不经本口（它在 begin 返 false 的当场信息最全）。
//    递的是份额不是成品：Device 摘桩后对斗**零解引用**（型号成品什么形制、要不要取，全归
//    使用端），机制层遂不认识任何成品类型。
//    寿命：CAS 摘桩后无写者，处理器与等待器可各持只读份额，最后一份释放时销毁。
template <typename C>
concept CollectorFamily = requires { typename C::Feed; } && std::derived_from<C, Collector<typename C::Feed>>;

template <CollectorFamily C>
using DoneFn = std::function<void(std::shared_ptr<const C>)>;

// ── NoWatchdogMs：Collector::timeoutMs 的哨兵——**本次 run 不设看门狗**。
// timeoutMs() > 0 才启动看门狗；任何非正值都不设表，不只此哨兵。
// 超时仍有当前尝试时，Device 由 makeSelf 创建新斗，CAS 替换成功后发送 buildStart 并重新计时；
// 重试不发送 buildStop，停机命令只走撤单路径。命令表是否为空不决定是否设表。
// 是否需要重试及具体时限由型号给出；不设表的尝试只能自然完成或被取消。
inline constexpr int NoWatchdogMs = -1;

// feed 必须来自 Collector<Feed>：按成员指针类型检查声明来源，拒绝同名隐藏或重载。
// makeSelf 检查 const 对象上的可调用性与 shared_ptr<自身> 返回类型，不检查函数体。
// 实现契约：只读不可变指示区，正常返回非空、独立的新对象及初始收集区，不访问正在写的收集区。
// concept 不证明返回值非空、对象独立或函数体并发安全。
template <typename C>
concept CollectorLike = CollectorFamily<C> && requires(const C c) {
    requires std::same_as<decltype(&C::feed), decltype(&Collector<typename C::Feed>::feed)>;
    { c.makeSelf() } -> std::same_as<std::shared_ptr<C>>;
};
