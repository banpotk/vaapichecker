#pragma once

#include "core/report.h"
#include <QMap>
#include <optional>

namespace checker {

struct DrmClient {
    QString driver;
    QString pciAddress;
    QString id;
    int pid = 0;
    QString process;
    QMap<QString, quint64> engineNs;
    QMap<QString, unsigned int> capacities;
};

struct UsageRow {
    int pid = 0;
    QString process;
    int clients = 0;
    QMap<QString, std::optional<double>> utilization;
};

struct UsageSnapshot {
    QList<UsageRow> rows;
    QStringList engines;
    int inaccessible = 0;
    int sharedClients = 0;
    QString status;
};

DrmClient parseFdInfo(const QString &text);

class UsageSampler {
public:
    explicit UsageSampler(DeviceInfo device, QString procRoot = "/proc");
    UsageSnapshot sample();
    // Exposed independently from /proc so accounting can be verified without a GPU.
    UsageSnapshot calculate(const QList<DrmClient> &clients, qint64 monotonicNs);

private:
    DeviceInfo device_;
    QString procRoot_;
    QMap<QString, DrmClient> previous_;
    qint64 previousNs_ = 0;
};

} // namespace checker
