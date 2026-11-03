#include <QCoreApplication>
#include <type_traits>
#include <DeviceBase.h>
static_assert(!std::is_copy_constructible_v<DeviceLock>);
static_assert(!std::is_move_constructible_v<DeviceLock>);
static_assert(!std::derived_from<DeviceBase, QObject>);
static_assert(!std::is_copy_constructible_v<DeviceBase>);
static_assert(std::same_as<decltype(DeviceBase::displayName), const QString>);
static_assert(std::same_as<decltype(DeviceBase::subtitle), const QString>);
static_assert(std::same_as<decltype(std::declval<const DeviceBase &>().holder()), std::optional<QString>>);
class ProbeDevice final : public DeviceBase {
public:
    ProbeDevice() : DeviceBase(QStringLiteral("设备"), QStringLiteral("规格")) {}
    bool isOpen() const noexcept override { return true; }
    void lockChanged(HandleCore::ResourcePhase) override {}
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ProbeDevice device;
    if (device.displayName != QStringLiteral("设备") || device.subtitle != QStringLiteral("规格") || device.holder()) return 4;
    const auto unnamed = std::make_shared<DeviceLock>(QString());
    if (!device.tryLock(unnamed) || !device.holder() || !device.holder()->isEmpty()) return 5;
    const auto other = std::make_shared<DeviceLock>(QStringLiteral("other"));
    device.releaseLock(other);
    if (!device.holder()) return 6;
    device.releaseLock(unnamed);
    if (device.holder()) return 7;
    if (!device.tryLock(other) || device.holder() != QStringLiteral("other")) return 8;
    device.releaseLock(other);
    if (!device.tryLock({}) || device.holder()) return 9;
    DeviceLock lock(QStringLiteral("test"));
    int calls = 0;
    lock.arm([&] { ++calls; lock.arm([&] { calls += 10; }); });
    lock.ring();
    if (calls != 1) return 1;
    lock.ring();
    if (calls != 11 || lock.holder() != QStringLiteral("test")) return 2;
    lock.arm({});
    lock.ring();
    return calls == 11 ? 0 : 3;
}
