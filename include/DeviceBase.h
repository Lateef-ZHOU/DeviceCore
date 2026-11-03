#pragma once
#include <atomic>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <QByteArray>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <HandleBase.h> // Handle<> / HandleLike
#include "RunBase.h"    // 运行词汇底座：DoneFn / DeviceLock / Collector / CollectorLike

// 锁令牌 DeviceLock（住 RunBase.h，持占用者名称和取消铃，指针表示身份）：
// 每帧至多一把，名下多台设备持同一把。
// **加锁 / 放锁都是显式的**：协程的**准备模块**逐台 tryLock、**后处理模块**逐台 releaseLock
// （`DeviceCoro::promise_type` 的构造 / 析构 + final_suspend，见 DeviceCoro.h）——各设备就地清槽 + 喊
// 一声，于是"放锁"对界面是个事件。设备主动关闭或析构时经 dropLock 摇令牌。
//
// **锁的存活不与协程的存活挂钩**（用户定）：令牌全链 `shared_ptr`，设备不观测"协程还在不在"——
// 打断走铃（铃只弱观测等待器的 Cell，不碰协程帧）、放锁走后处理模块，两条都不问协程死活。
// **保证放锁的是 `~DeviceCoro` 那两拍**（先 ring 让协程体面收尾、再 destroy，而 destroy 会跑 promise
// 的析构函数 ⇒ 后处理模块必跑）。**代价明码**：没有"持有者猝死即自动空闲"那层兜底。

// DeviceBase — 设备非模板基类：锁层骨架 + identity 鉴权。
// 模板无关的锁职责收在这；"下级是否就绪"以纯虚 isOpen 下放模板层。
// 不继承 QObject；唯一 QObject 来自 Handle，具体末端提供锁信号与界面属性。
// 鉴权口径：lockMatches(p) = 空闲 OR 锁匹配，叠加 isOpen = canEngage——
// 空闲时任何锁（含 null）都过；被 A 占用时 B（或 null）过不了，跨模块 kill 天然失效。
//
// 只读属性按需读取 Handle 和锁槽，不保存缓存，也不冒用异型通知参数。
// 界面监听 Handle.connectionChanged 与本层 lockChanged，在完成阶段回读。
class DeviceBase
{
public:
    DeviceBase(QString name, QString detail)
        : displayName(std::move(name)), subtitle(std::move(detail)) {}
    Q_DISABLE_COPY_MOVE(DeviceBase)

    // 下级就绪谓词，模板层实现（→ HandleT::isOpen）。层级不变量：上级加锁的前提是下级已激活。
    // **公开**：界面据此显示连接状态——按"纯转发皮不留，虚函数直接公开"，不另包一层。
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;

    const QString displayName;
    const QString subtitle;

    // 纯 C++ 契约；具体末端直接以 Qt 信号覆写，不另设通知对象。
    virtual void lockChanged(HandleCore::ResourcePhase phase) = 0;
    virtual ~DeviceBase() = default;

    // 加锁：canEngage 通过则持有（按值收份额 + move 入槽）。
    // 占用者名字由锁在创建时持有，设备只接收锁份额。
    // **空令牌是空动作**（返 true、不落槽、不喊话——见定义处），故本口幂等；
    // 返 false 只有一个意思：**此刻这台设备用不了**（别人占着 / 没连接）。
    [[nodiscard]] bool tryLock(std::shared_ptr<const DeviceLock> lock);

    // ── 锁的只读面：给界面显示"占没占、谁占着"。[主]
    // **只作显示，裁判仍是 tryLock**：界面收到锁完成事件时现问一遍，点开始那一刻才真正抢锁。
    // 无值表示空闲；有值表示占用，名称允许为空。不借出锁令牌，不另存显示缓存。
    [[nodiscard]] std::optional<QString> holder() const
    {
        if (lock_) return lock_->holder();
        return std::nullopt;
    }

    // [主] 归还锁 + **喊一声**：**验锁进门**（lockMatches：空闲，或出示的正是持有者的令牌），
    // 空槽由 dropLock 处理，不重复判断。
    // **与 tryLock 成对，故同样公开**——放锁的是协程的**后处理模块**
    // （`DeviceCoro::promise_type` 的析构函数 / final_suspend），而它不在设备的继承链上。
    // **安全性不靠可见性靠验锁**：外人出示不出持有者的令牌，就放不掉别人的锁。
    // **“期间已易主”由 lockMatches 当场挡下**（槽里换成了别人的**活**令牌，owner 不等），
    // 调用方遂不必自带核对——后处理模块“整张名单照放”正是靠这一条。
    // 主动关闭与析构都不借本口验锁，直接用 dropLock 收锁；
    // dropLock 在锁槽实际变动后发 Released。
    void releaseLock(const std::shared_ptr<const DeviceLock> &lock);

    // [主] 两级只读鉴权，族接口经 device() 共用，不为每个操作另造 canXXX 转发。
    // 锁谓词：**空闲，或出示的正是槽里那一枚**（一次指针比较）。
    // 空来者（"我不持锁"）对空闲槽恒过、对被占的槽恒拒——两条都由这一行直接给出。
    [[nodiscard]] bool lockMatches(const std::shared_ptr<const DeviceLock> &p) const noexcept
    {
        return !lock_ || lock_ == p;
    }
    // 严格谓词：锁验证 + 下级就绪。tryLock 与 Device::start 用。
    [[nodiscard]] bool canEngage(const std::shared_ptr<const DeviceLock> &p) const noexcept
    {
        return lockMatches(p) && isOpen();
    }

protected:
    // 仅供继承链逐层清理；外部通过虚析构进入末端的私有预析构。
    virtual void PreDestruction() noexcept { dropLock(); }

    // [主] 摘锁并摇铃；实际摘下锁时统一发 Released，重复调用为空动作。
    void dropLock() noexcept;

private:

    // 锁槽（identity 鉴权；绝不对外泄露）。**空 ⟺ 本台空闲**，一个事实一处。
    // **const 指向物是设计**：本层只读名称、比对身份与摇铃，换弹权归协程帧（见 DeviceLock 类注）。
    // **是 shared 不是 weak**（用户定）：设备不需要知道"协程还在不在"；比对是一次指针比较、摘锁
    // 不必 promote。**代价明码**：放锁彻底是显式的（协程的后处理模块逐台归还），没有"持有者
    // 猝死即自动空闲"那层兜底。
    std::shared_ptr<const DeviceLock> lock_;
};

// 类内传入成员指针类型，检查本层声明而不公开预析构入口。
// 不转换成员指针：继承来的实现仍带父类类型，不能冒充本层覆写。
// 函数体的调用顺序与父层接续仍须审查。
template <typename T, typename Member>
concept DeviceLifecycleLayer = std::derived_from<T, QObject>
    && std::has_virtual_destructor_v<T> && std::destructible<T>
    && std::same_as<Member, void (T::*)() noexcept>;

template <typename T, typename Member>
concept FinalDevice = DeviceLifecycleLayer<T, Member> && std::is_final_v<T> && (!std::is_abstract_v<T>);

inline bool DeviceBase::tryLock(std::shared_ptr<const DeviceLock> lock)
{
    if (!canEngage(lock))
        return false; // 别人占着 / 没连接：这台设备此刻用不了
    // 同一锁（包括双方为空）不改变状态、不重复通知。
    if (lock_ == lock)
        return true;
    lock_ = std::move(lock);
    Q_EMIT lockChanged(HandleCore::ResourcePhase::Acquired);
    return true;
}

inline void DeviceBase::releaseLock(const std::shared_ptr<const DeviceLock> &lock)
{
    if (!lockMatches(lock))
        return; // 不是持有者出示的令牌：不动别人的锁（"期间已易主"在此当场挡下）
    dropLock();
}

inline void DeviceBase::dropLock() noexcept
{
    const auto token = std::exchange(lock_, {}); // 摘出即防重入
    if (token)
    {
        token->ring(); // 摇令牌即取消：收协程当前的等待
        Q_EMIT lockChanged(HandleCore::ResourcePhase::Released);
    }
}

// ============================================================
// Device<HandleT, CollectorT> — 设备模板层（双钉扣模板口 HandleLike + CollectorLike）：锁骨架
// 之上住着**一次 run 的全部状态**（看门狗重试由设备内部处理，不进入协程）：
//   · run_ **单槽**：`atomic<shared_ptr<const State>>`——State 是**私有嵌套聚合**（斗 + 完成
//     回调两员一体，**本层之外无人认识它**），**在岗 ⟺ run 进行中**（闲忙谓词即它）。主线程
//     装（start）/ 换（restart CAS）/ 清（teardown exchange），worker 只 load 与**判齐 CAS
//     摘走**。摘走 = 注销 = 独占交付权，一个动作办三件事，无独立注销仪式、无两份账。
//   · watchdog_ [主]：到点 = 本次尝试作废，造新斗 + CAS 换状态 + 重发 + 再起表（restart）。
// 型号知识全在具体 Collector（命令 / 时限 / 判齐 / 攒料 / 造下一只，见 RunBase.h）——设备零型号
// 知识；**成品形制亦然**：交付递的是斗份额本身，成品什么形制、怎么取，归具体收集器自定读口与
// 使用端，机制层不认识任何成品类型。
//
// 线程纪律：[主] 仅主线程；[worker] 仅 Handle 的收包线程，其执行方式由具体 Handle 决定。
// 状态的交接：**指向物不可变、整块换指针**（State 那份 const）；斗的收集区判齐前 worker 独
// 写，判齐即 CAS 摘状态——worker 此后不触斗，判齐前的写经 invokeMethod 队列 happens-before 移交
// 主线程只读，斗上全程单写者。makeSelf 必须只读不可变指示区、不访问正在写入的收集区，
// 才能与 worker 进料并发；这是实现契约，不是 const 自动提供的保证。
// 表（QTimer）是 [主] 资产：worker 判齐后必须投递回主线程停表 + 促发，这一跳无法省。
// ============================================================
// 族层拥有一台具体 Device，通过回调接入数据流与完成帧；机制层不认识族或官邸。
template <HandleLike HandleT, CollectorLike CollectorT>
class Device : public HandleT, public DeviceBase
{
public:
    using CollectorType = CollectorT;

    // 运行槽持具体型号 Collector；工厂和重试保持同型，族接口只在外部入口/结果处擦除。

    using OnStream = std::function<std::optional<typename CollectorT::Feed>(const QByteArray &, RxTime)>;
    using OnConsume = std::function<void(std::shared_ptr<const CollectorT>)>;

    Device(QString name, QString subtitle, OnStream onStream, OnConsume onConsume, QObject *parent = nullptr)
        : DeviceBase(std::move(name), std::move(subtitle)),
          onStream_(std::move(onStream)), onConsume_(std::move(onConsume))
    {
        this->setParent(parent);
        static_assert(DeviceLifecycleLayer<Device, decltype(&Device::PreDestruction)>);
        // AboutToRelease 由所属线程同步发出，覆盖 close 与换参 open。
        QObject::connect(this, &HandleT::connectionChanged, this, &Device::onConnectionChanged);
        watchdog_.setSingleShot(true);
        QObject::connect(&watchdog_, &QTimer::timeout, this, &Device::restart);
    }

    ~Device() override = default;
    // Qt 属性边界：无效 QVariant 表示未占用，QString（含空串）表示占用。
    [[nodiscard]] QVariant qmlHolder() const
    {
        const auto owner = holder();
        return owner ? QVariant(*owner) : QVariant();
    }

    // 初始化交接期间安装；首次 start 之前完成。关闭后由主线程清空，不与消费并发替换。
    void setOnConsume(OnConsume callback) { onConsume_ = std::move(callback); }

    // 层级不变量：加锁的前提是 Handle 已激活。公开：设备页据此显示连接状态。
    [[nodiscard]] bool isOpen() const noexcept final { return HandleT::isOpen(); }

    // [主] 官邸命令组连发口：先验 `canEngage(lock)`——锁对（空闲，或出示的正是
    // 持有者的令牌）且设备已连接，才逐帧发出（用户定：直发不走收集器，但验锁照验；设备连上了才能发数据）。
    // 设备页不持锁，传空令牌 `{}` = "我不持锁"：空闲放行，被任务占着即拒（停止也拒——任务的停止走任务页）；
    // 协程持锁时出示本帧令牌。准入拒绝统一记一条日志；实际发送失败由 Handle 报告，本层不重复记录。
    // run 自己的启动 / 看门狗重发 / 拆机停机不走本口，走私有小灶 rawSend——那几拍的锁 start 已验过，
    // 拆机则须在断联时照走。
    bool send(const QList<QByteArray> &commands, const std::shared_ptr<const DeviceLock> &lock) const
    {
        if (!canEngage(lock))
        {
            Q_EMIT this->logMessage(QStringLiteral("设备未连接或正被占用，指令未下发"));
            return false;
        }
        return rawSend(commands);
    }

    // [主] 准入后启动或替换 run：现造收集区、装入状态、设置看门狗，再发启动。
    // 发败时有看门狗则等重试；无看门狗则拒收，不能挂起一个没有恢复路径的请求。
    // 主线程直接 store 替换未完成请求，不通知其旧回调；调用者负责旧等待的寿命。
    // worker 若已 CAS 摘走旧状态，已取得的完成权不受新请求影响，仍排队交付。
    //
    // **形参是两件词汇，不是一份组装好的状态**：收**斗的原型份额** + 完成回调，
    // 状态怎么装是本层内政（State 私有嵌套）。三条收益：
    //   · 下单方（族口 begin）不必认识 State——它只管交出两件词汇；
    //   · **现造落在唯一需要它的地方**：本次 run 的那只斗由本口照 `proto` 的指示区现造
    //     （`makeSelf`），`proto` 是别人的东西，本口自始至终只读它。工厂须返回独立的新斗，
    //     使设备的收集区与下单方留着的原型隔离；此要求须由工厂实现遵守。
    //   · 完成回调按值收 + move 入槽（不是 const&）：它是**转交**不是照原型复制，多一次
    //     std::function 拷贝就可能多一次堆分配。
    //
    // **收份额原物，不收解引用后的视图**（本仓铁律"代客解引用害人害己"）：形参若是
    // `const CollectorT &`，族口 `begin` 就得写 `proto && this->start(*proto, …)`——**判空与解引用
    // 被推给了传递端**，而那正是禁令要挡的形状。空份额是"这单没下成"的正当回答（`collectorFor`
    // 够不着时返的就是它），对它的响应理应写在**原语体内**：本口判空、返 false。
    [[nodiscard]] bool start(const std::shared_ptr<const CollectorT> &proto, DoneFn<CollectorT> done,
                             const std::shared_ptr<const DeviceLock> &lock)
    {
        // 本层只报告准入拒绝；后续发送失败由 Handle 报告。
        if (!canEngage(lock))
        {
            Q_EMIT this->logMessage(QStringLiteral("设备未连接或正被占用，未启动"));
            return false;
        }
        if (!proto)
        {
            Q_EMIT this->logMessage(QStringLiteral("启动请求缺少 Collector，未启动"));
            return false;
        }
        // makeSelf 按不可变配置另造独立的收集区，不复制原斗的收集状态。
        // const 仍允许读取收集区；可读范围和新斗初始状态依赖工厂契约，见 CollectorLike。
        const auto st = std::make_shared<const State>(proto->makeSelf(), std::move(done));
        run_.store(st);                             // ① 装状态（斗 + 回调一体）
        const int ms = st->collector->timeoutMs();
        if (ms <= 0) // 按新请求的时限统一设置看门狗，再发送启动命令。
            watchdog_.stop();
        else
            watchdog_.start(ms);
        if (!rawSend(st->collector->buildStart()) && ms <= 0)
        {
            // 收包若已完成并摘走状态，仍按已排队的结果交付；否则撤销并由等待器处理拒收。
            auto pending = st;
            if (run_.compare_exchange_strong(pending, {}))
                return false;
        }
        return true;
    }

    // [主] 停止 = 打断口（族基那枚纯虚 stop 的实现，由族模板层转来；本层不立虚口）。撤除口径：唯打断路下发停机命令——交付是
    // 自然完成、拒收不是我们的锁段。验锁只 lockMatches 不含 isOpen（断联后拆机必须能走）：他人
    // 锁段一律无视（跨模块 kill 失效）；可反复调用（拆机各拍自幂等）——等待器的"撤单"
    // 正是靠这份幂等，对交付 / 取消 / 拒收回滚三条出路一视同仁。
    void stop(const std::shared_ptr<const DeviceLock> &lock)
    {
        if (!lockMatches(lock)) // 验锁：锁不对不拆机
            return;
        teardown();
    }

protected:
    void PreDestruction() noexcept override
    {
        HandleT::close();
        // 所有者在自身预析构内销毁本对象；回调依赖仍在，DAQ 处理线程已退出。
        // 借用者先撤销，再收本地等待；停机命令交给 Handle，发送失败不妨碍本地撤销。
        teardown();
        DeviceBase::PreDestruction();
    }

private:
    // [主] 只有主动关闭前同步撤单；迟到的 Released 不得取消后来启动的任务。
    void onConnectionChanged(HandleCore::ResourcePhase phase)
    {
        if (phase == HandleCore::ResourcePhase::AboutToRelease)
        {
            teardown();
            this->dropLock(); // 当前等待可能在另一台设备上，仍须通知持有者结束。
        }
    }

    // ── State：一次 run 的状态 = **在跑的那只斗 + 完成回调**，两员一体的裸聚合。
    //    **私有嵌套是设计**：run 状态怎么装是设备内政——运行层契约（RunBase.h）与下单件
    //    （族口 begin）的词汇里只有"一只斗 + 一个完成回调"两件，谁都不需要认识这个类型。
    //
    // **不可变快照 + 整块换指针**：主线程从不原地改，只把新造好的整份
    // 换上去；worker 只 load 与 CAS。指向物 const 即"读者拿到的那份不会在脚下变"。
    // **注意 const 只到 State 本身**：里面那只斗的**指向物非 const**——收集区正是 worker 要写
    // 的。斗**必须经指针**在此落座，值持有会把这两层 const 叠到同一根轴上（State 的 const 会
    // 顺势把整只斗冻住，连收集区一起），而斗内部"const = 指示区 / 非 const = 收集区"那道分区
    // 更值钱。这一跳指针正是隔开两套 const 的绝缘层。
    //
    // **为什么是一个槽，而不是「回调」与「斗」各一个**："run 进行中"这件事若有两份账，交付就
    // 需要两道闸，两道闸之间的语句顺序极其讲究、且顺序的理由一年后没人记得。一槽则**只剩一道
    // 闸**：CAS 摘走状态即同时取得"投递权"与"结果所有权"。交付与 teardown 抢同一个槽，谁摘到
    // 归谁——交付先到，teardown 见空即返（不发 buildStop：已收场的 run 本就不该发）；teardown
    // 先到，worker 的 CAS 失败、那只斗作废。没有"两边各走一半"的中间态。
    struct State
    {
        std::shared_ptr<CollectorT> collector; // 指示区 const、收集区 [仅 worker] 可写
        DoneFn<CollectorT> done;               // 促发即交付（斗份额非空 = 成了，空 = 没成）
    };

    // [主] 不验锁的命令组连发，供 run 启动 / 看门狗重发 / 撤单使用；官邸使用的 send
    // 先验 canEngage 再转到这里。逐帧直转 HandleT::send、短路失败。空表恒真、零发送（"无命令"结构性
    // 不产生下行）；表内空词由 Handle::onTX 词长裁决，不归本口；未连接时 HandleT::send 自己判空喊话、返 false。
    bool rawSend(const QList<QByteArray> &commands) const
    {
        for (const auto &cmd : commands)
            if (!HandleT::send(cmd))
                return false;
        return true;
    }

    // [主] 停看门狗并原子摘走当前运行；只有拿到状态的调用者能撤销等待。
    // 无条件尝试发送停机命令；连接/发送失败由 Handle 报告，仍须通知等待者本次已撤销。
    void teardown()
    {
        watchdog_.stop();
        const auto st = run_.exchange({}); // 摘状态 = 注销（一次性）
        if (!st)
            return;                          // 已拆完 / 本就没在跑
        (void)rawSend(st->collector->buildStop());
        st->done({});                        // 促发：**空斗 = 没成**（局部份额持过整调）
    }

    // [主·看门狗到点] 状态在岗才动（run 已结 → 静默，表不再起）→ 造新斗、CAS 顶替（旧斗此后即便
    // 判齐，worker 的交付 CAS 也必失败 = 作废；若 worker 抢先摘走了状态，则本次 CAS 失败、交付照常）
    // → 重发启动命令 → 再起表。超时重试由设备内部处理，不唤醒协程。
    void restart()
    {
        auto old = run_.load(); // ① 读现状（CAS 要可写 expected，故非 const——同收包回调）。
        if (!old)
            return; // run 已结（交付 / 拆机）→ 静默，表不再起
        // ② 造新斗 + 拷回调 → 组一份**全新的不可变状态**（原地改 old 是不行的：worker 正读着它）。
        //    并发安全依赖 makeSelf 只读不可变指示区、不访问 worker 正在写的收集区，不能由 const 推出。
        auto next = std::make_shared<const State>(
            old->collector->makeSelf(), old->done);
        // ③ 比较并交换：**必须 CAS 不能 store**——若这中间 worker 已把状态摘走（交付）或
        //    teardown 已清空，store 会把一个已了结的 run **复活成僵尸**（无人再促发、无人再拆）。
        //    CAS 失败即那两件事之一已发生，本次重发作废，且**不再起表**。
        //    **首参是出参不是入参**：`compare_exchange_strong(T &expected, T desired)` 按非 const
        //    引用收它，**失败时把当时的实际值写回去**（那正是"失败即告诉你现在是什么"）。故
        //    `old` 不能是 const，且失败路的 `old` 已被改写成现值、绝不可再用——本口失败即返回，
        //    此后只读 next，故直接把 old 交出去即可，不必另立一个只为过一遍类型的局部量。
        if (!run_.compare_exchange_strong(old, next))
            return;
        rawSend(next->collector->buildStart()); // ④ 重发启动命令（同一个 run，锁在 start 时已验）
        // ⑤ 起表——**非正时限即不设表**（NoWatchdogMs，见 RunBase.h）：结构上没有终点的 run
        // （连续运动）不存在"卡住"，起了表只会周期性重发。不起表 ⇒ 本口不再被触发，
        // 未自然完成的 run 由取消或主动关闭结束。
        if (const int ms = next->collector->timeoutMs(); ms > 0)
            watchdog_.start(ms);
    }

    const OnStream onStream_; // Handle 线程调用；所有者先销毁本 Device 再拆回调依赖。
    OnConsume onConsume_;     // 主线程调用；空回调表示本族无需消费完成帧。

    // Handle 在收包线程直接虚派发，不保存 this 闭包。
    void onData(const QByteArray &raw, RxTime at) final;

    // [主写 / worker 读+摘] 当前 run 的状态**单槽**：在岗 ⟺ run 进行中；CAS 摘出即注销。
    // **不可变快照 + 整块换指针**：谁都不原地改指向物，主线程换新的、worker 摘空的（见上面的
    // State 注）。
    std::atomic<std::shared_ptr<const State>> run_;

    QTimer watchdog_; // [主] 看门狗：单发，到点 restart（换斗重发，不促醒协程）

    // 末端析构首句调用 PreDestruction，保护上传所访问的 Device 与官邸状态。
};

template <HandleLike HandleT, CollectorLike CollectorT>
void Device<HandleT, CollectorT>::onData(const QByteArray &raw, const RxTime at)
{
    // ① 常态旁路：先于闲忙门，没在跑也照跑；交回的是**喂斗的那一口**（空 = 没有给斗的）
    const auto fed = this->onStream_(raw, at);
    if (!fed)
        return;
    auto st = this->run_.load(); // ② 状态促取：空 = idle 丢（CAS 要可写 expected，故非 const）
    if (!st || !st->collector->feed(*fed, at)) // ③ 喂 + 判齐一口：false = 未齐继续收（本函数是喂料的使用端，解引用归这里）
        return;
    // ④ **一道闸**：CAS 摘状态 = 同时取得"投递权"与"这次 run 的结果所有权"。
    //    失败 = 状态已易主（看门狗换斗顶替 / teardown 清空），本斗已废、不投递（失败路
    //    expected 已被改写成现值，绝不可再用）。成功即独占：此后 worker 见空槽即 idle，
    //    份额上行后使用端只读零竞态由此奠基。
    if (!this->run_.compare_exchange_strong(st, {}))
        return;
    QMetaObject::invokeMethod(
        this,
        [this, st] // 携状态投递（CAS 一次性 ⇒ 同一份状态恒至多一次投递）
        {
            // [主] st 已赢得完成权，仍须交付；run_ 可能已装入后来启动的尝试。
            // 仅空槽时停表：主线程在此不泵事件，worker 只能摘空，不能插入新尝试。
            if (!this->run_.load())
                this->watchdog_.stop();
            if (this->onConsume_)
                this->onConsume_(st->collector);
            // 消费已提交；族级等待器只接收完成结果，失效也不撤回已提交的帧。
            st->done(st->collector);
        },
        Qt::QueuedConnection);
}

// 族接口经组合持有的 Device 启动与撤单；单据和等待器见 TaskCore 的 DeviceCoro.h。
