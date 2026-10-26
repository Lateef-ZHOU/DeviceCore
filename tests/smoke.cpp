#include <QCoreApplication>
#include <type_traits>
#include <Device/Base/DeviceBase.h>
static_assert(!std::is_copy_constructible_v<DeviceLock>);
static_assert(!std::is_move_constructible_v<DeviceLock>);
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
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
