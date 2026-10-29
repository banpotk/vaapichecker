#pragma once

#include <QObject>
#include <QPalette>
#include <QDBusVariant>

namespace checker {

enum class ThemeMode { System, Light, Dark };

class ThemeController : public QObject {
    Q_OBJECT
public:
    explicit ThemeController(QObject *parent = nullptr);
    void setMode(ThemeMode mode);
    ThemeMode mode() const { return mode_; }

private slots:
    void portalSettingChanged(const QString &nameSpace, const QString &key, const QDBusVariant &value);

private:
    ThemeMode mode_ = ThemeMode::System;
    QPalette nativePalette_;
    unsigned int portalScheme_ = 0;
    quint64 portalRevision_ = 0;
    void readPortal(bool legacy = false);
    void apply();
};

} // namespace checker
