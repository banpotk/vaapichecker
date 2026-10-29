#pragma once

#include "core/report.h"

namespace checker {

class VaProbe {
public:
    static QList<DeviceInfo> devices();
    static DeviceInfo deviceInfo(const QString &path);
    static DeviceReport scan(const DeviceInfo &device);
};

} // namespace checker
