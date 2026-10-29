#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QPair>

namespace checker {

using Properties = QList<QPair<QString, QString>>;

struct DeviceInfo {
    QString path;
    QString name;
    QString driver;
    QString pciAddress;
    QString vendorId;
    QString deviceId;
    QString primaryNode;
};

struct Capability {
    int profileId = -1;
    int entrypointId = 0;
    QString profile;
    QString entrypoint;
    QString operation;
    QStringList renderFormats;
    QStringList pixelFormats;
    int minWidth = 0;
    int minHeight = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    Properties attributes;
    QStringList warnings;

    QString resolution() const;
    QString resolutionClasses() const;
};

struct FilterInfo {
    QString name;
    Properties details;
};

struct ProbeEvent {
    QString time;
    QString source;
    QString event;
    QString detail;
    bool error = false;
};

struct DeviceReport {
    DeviceInfo device;
    QDateTime capturedAt;
    bool available = false;
    bool demo = false;
    QString apiVersion;
    QString vendor;
    QString error;
    QList<Capability> capabilities;
    Properties processing;
    QList<FilterInfo> filters;
    QStringList imageFormats;
    QStringList warnings;
    QList<ProbeEvent> events;

    QJsonObject toJson() const;
    QString toText() const;
};

QString fourccName(unsigned int fourcc);
QStringList renderFormatNames(unsigned int mask);
DeviceReport demoReport();
QJsonObject propertiesJson(const Properties &properties);

} // namespace checker
