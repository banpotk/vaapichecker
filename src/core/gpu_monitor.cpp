#include "core/gpu_monitor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <chrono>

namespace checker {

DrmClient parseFdInfo(const QString &text)
{
    DrmClient client;
    for (const auto &line : text.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon < 1)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (key == "drm-driver") client.driver = value;
        else if (key == "drm-pdev") client.pciAddress = value;
        else if (key == "drm-client-id") client.id = value;
        else if (key.startsWith("drm-engine-capacity-")) {
            bool ok = false;
            const unsigned int capacity = value.toUInt(&ok);
            if (ok && capacity > 0)
                client.capacities.insert(key.mid(20), capacity);
        } else if (key.startsWith("drm-engine-")) {
            const auto parts = value.simplified().split(' ');
            bool ok = false;
            const quint64 count = parts.value(0).toULongLong(&ok);
            if (ok && parts.size() == 2 && parts.at(1) == "ns")
                client.engineNs.insert(key.mid(11), count);
        }
    }
    return client;
}

UsageSampler::UsageSampler(DeviceInfo device, QString procRoot)
    : device_(std::move(device)), procRoot_(std::move(procRoot))
{
}

UsageSnapshot UsageSampler::calculate(const QList<DrmClient> &clients, qint64 monotonicNs)
{
    UsageSnapshot snapshot;
    QMap<QString, DrmClient> next;
    QMap<int, UsageRow> processes;
    const qint64 elapsed = monotonicNs - previousNs_;
    const bool baseline = previousNs_ > 0 && elapsed > 0;
    auto ordered = clients;
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.pid < b.pid; });
    for (const auto &client : ordered) {
        // Without a client ID duplicated FDs cannot be accounted accurately.
        if (client.id.isEmpty() || client.driver.isEmpty())
            continue;
        const QString identity = (client.pciAddress.isEmpty() ? device_.path : client.pciAddress) + ":" + client.id;
        if (next.contains(identity)) {
            if (next.value(identity).pid != client.pid)
                ++snapshot.sharedClients;
            continue;
        }
        DrmClient saved = client;
        auto &row = processes[client.pid];
        row.pid = client.pid;
        row.process = client.process;
        ++row.clients;
        const auto old = previous_.constFind(identity);
        for (auto it = client.engineNs.cbegin(); it != client.engineNs.cend(); ++it) {
            if (!snapshot.engines.contains(it.key()))
                snapshot.engines.append(it.key());
            std::optional<double> percent;
            if (baseline && old != previous_.cend() && old->engineNs.contains(it.key())) {
                const quint64 last = old->engineNs.value(it.key());
                // Kernel counters may temporarily regress. Keep their high watermark.
                const quint64 current = std::max(last, it.value());
                saved.engineNs[it.key()] = current;
                const unsigned int capacity = std::max(1u, client.capacities.value(it.key(), 1));
                percent = std::clamp(100.0 * static_cast<double>(current - last)
                    / (static_cast<double>(elapsed) * capacity), 0.0, 100.0);
            }
            auto existing = row.utilization.find(it.key());
            if (existing == row.utilization.end())
                row.utilization.insert(it.key(), percent);
            else if (existing.value() && percent)
                existing.value() = std::min(100.0, *existing.value() + *percent);
            else
                existing.value() = std::nullopt;
        }
        next.insert(identity, saved);
    }
    previous_ = next;
    previousNs_ = monotonicNs;
    std::sort(snapshot.engines.begin(), snapshot.engines.end());
    for (const auto &row : processes)
        snapshot.rows.append(row);
    snapshot.status = snapshot.rows.isEmpty() ? "No readable DRM clients for the selected GPU."
        : snapshot.engines.isEmpty() ? "The driver does not export drm-engine counters in nanoseconds."
        : !baseline ? "Collecting baseline; percentages appear after the next sample."
        : QStringLiteral("%1 processes · %2 engines · sampled from DRM fdinfo")
              .arg(snapshot.rows.size()).arg(snapshot.engines.size());
    if (snapshot.sharedClients > 0)
        snapshot.status += " · shared clients counted once under the lowest visible PID";
    return snapshot;
}

UsageSnapshot UsageSampler::sample()
{
    QList<DrmClient> clients;
    int inaccessible = 0;
    const auto pids = QDir(procRoot_).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &pidText : pids) {
        bool numeric = false;
        const int pid = pidText.toInt(&numeric);
        if (!numeric || pid <= 0)
            continue;
        const QString base = procRoot_ + '/' + pidText;
        const QDir infos(base + "/fdinfo");
        if (!infos.isReadable()) {
            ++inaccessible;
            continue;
        }
        QFile comm(base + "/comm");
        const QString name = comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed() : pidText;
        for (const auto &fd : infos.entryList(QDir::Files)) {
            QFile info(infos.filePath(fd));
            if (!info.open(QIODevice::ReadOnly)) {
                ++inaccessible;
                continue;
            }
            DrmClient client = parseFdInfo(QString::fromUtf8(info.readAll()));
            if (client.driver.isEmpty() || client.id.isEmpty())
                continue;
            bool matches = !device_.pciAddress.isEmpty() && client.pciAddress == device_.pciAddress;
            if (!matches && client.pciAddress.isEmpty()) {
                const QString target = QFileInfo(base + "/fd/" + fd).symLinkTarget();
                matches = target == device_.path || (!device_.primaryNode.isEmpty() && target == device_.primaryNode);
            }
            if (!matches)
                continue;
            client.pid = pid;
            client.process = name;
            clients.append(client);
        }
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto result = calculate(clients, std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    result.inaccessible = inaccessible;
    if (inaccessible > 0)
        result.status += QStringLiteral(" · %1 unreadable process/FD entries").arg(inaccessible);
    return result;
}

} // namespace checker
