#pragma once
#include <chrono>

// 收包时刻是标准库值类型；各消费边界自行决定显示或保存表示。
using RxTime = std::chrono::system_clock::time_point;
