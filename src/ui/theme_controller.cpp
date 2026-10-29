#include "ui/theme_controller.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace checker {
namespace {

constexpr auto portalService = "org.freedesktop.portal.Desktop";
constexpr auto portalPath = "/org/freedesktop/portal/desktop";
constexpr auto settingsInterface = "org.freedesktop.portal.Settings";
constexpr auto appearanceNamespace = "org.freedesktop.appearance";

unsigned int colorScheme(QVariant value)
{
    // The older Settings.Read method wraps its result in two DBus variants.
    while (value.metaType() == QMetaType::fromType<QDBusVariant>())
        value = qvariant_cast<QDBusVariant>(value).variant();
    bool ok = false;
    const auto scheme = value.toUInt(&ok);
    return ok && scheme <= 2 ? scheme : 0;
}

QPalette themePalette(bool dark)
{
    QPalette palette = QApplication::style()->standardPalette();
    const auto set = [&palette, dark](QPalette::ColorRole role, const char *light, const char *night) {
        palette.setColor(role, QColor(dark ? night : light));
    };
    set(QPalette::Window, "#f2f3f5", "#20242b");
    set(QPalette::WindowText, "#1f2937", "#e6eaf0");
    set(QPalette::Base, "#ffffff", "#171b21");
    set(QPalette::AlternateBase, "#e9edf2", "#282e38");
    set(QPalette::Text, "#1f2937", "#e6eaf0");
    set(QPalette::Button, "#f2f3f5", "#2c323d");
    set(QPalette::ButtonText, "#1f2937", "#e6eaf0");
    set(QPalette::ToolTipBase, "#ffffff", "#303744");
    set(QPalette::ToolTipText, "#1f2937", "#f8fafc");
    set(QPalette::Highlight, "#356ac3", "#356ac3");
    set(QPalette::HighlightedText, "#ffffff", "#ffffff");
    set(QPalette::Link, "#185abc", "#8bbcff");
    set(QPalette::LinkVisited, "#7e3ca5", "#cea5ef");
    set(QPalette::PlaceholderText, "#727d8b", "#97a1af");
    set(QPalette::Light, "#ffffff", "#495362");
    set(QPalette::Midlight, "#e0e4e9", "#3b4452");
    set(QPalette::Mid, "#b5bdc8", "#27303b");
    set(QPalette::Dark, "#8994a4", "#131820");
    set(QPalette::Shadow, "#727d8b", "#0b0f14");
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    set(QPalette::Accent, "#356ac3", "#8bbcff");
#endif
    const QColor disabled(dark ? "#8994a4" : "#727d8b");
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(dark ? "#35445b" : "#d3dae5"));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return palette;
}

} // namespace

ThemeController::ThemeController(QObject *parent)
    : QObject(parent), nativePalette_(QApplication::palette())
{
    // Fusion consistently respects QPalette on both X11 and Wayland.
    QApplication::setStyle(QStyleFactory::create("Fusion"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == ThemeMode::System) apply();
    });
#endif
    auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        bus.connect(portalService, portalPath, settingsInterface, "SettingChanged", this,
            SLOT(portalSettingChanged(QString,QString,QDBusVariant)));
        auto *watcher = new QDBusServiceWatcher(portalService, bus,
            QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { readPortal(); });
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
            ++portalRevision_;
            portalScheme_ = 0;
            if (mode_ == ThemeMode::System) apply();
        });
        readPortal();
    }
    apply();
}

void ThemeController::setMode(ThemeMode mode)
{
    if (mode_ == mode) return;
    mode_ = mode;
    apply();
}

void ThemeController::readPortal(bool legacy)
{
    auto request = QDBusMessage::createMethodCall(portalService, portalPath, settingsInterface,
        legacy ? "Read" : "ReadOne");
    request << QString(appearanceNamespace) << QString("color-scheme");
    const auto revision = portalRevision_;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(request, 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, legacy, revision](QDBusPendingCallWatcher *call) {
        const QDBusPendingReply<QDBusVariant> reply = *call;
        call->deleteLater();
        if (revision != portalRevision_) return;
        if (reply.isError()) {
            if (!legacy && reply.error().name() == "org.freedesktop.DBus.Error.UnknownMethod") readPortal(true);
            return;
        }
        portalSettingChanged(appearanceNamespace, "color-scheme", reply.value());
    });
}

void ThemeController::portalSettingChanged(const QString &nameSpace, const QString &key, const QDBusVariant &value)
{
    if (nameSpace != appearanceNamespace || key != "color-scheme") return;
    ++portalRevision_;
    portalScheme_ = colorScheme(value.variant());
    if (mode_ == ThemeMode::System) apply();
}

void ThemeController::apply()
{
    unsigned int scheme = portalScheme_;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (scheme == 0) {
        switch (QApplication::styleHints()->colorScheme()) {
        case Qt::ColorScheme::Dark: scheme = 1; break;
        case Qt::ColorScheme::Light: scheme = 2; break;
        default: break;
        }
    }
#endif
    const QPalette palette = mode_ == ThemeMode::System && scheme == 0 ? nativePalette_
        : themePalette(mode_ == ThemeMode::Dark || (mode_ == ThemeMode::System && scheme == 1));
    QApplication::setPalette(palette);
}

} // namespace checker
