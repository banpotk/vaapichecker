#include "core/va_probe.h"
#include "ui/main_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <memory>

int main(int argc, char **argv)
{
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--report" || arg.startsWith("--report=") || arg == "--help" || arg == "-h"
            || arg == "--version" || arg == "-v") headless = true;
    }
    std::unique_ptr<QCoreApplication> app;
    if (headless) app = std::make_unique<QCoreApplication>(argc, argv);
    else app = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setApplicationName("VAAPI Checker");
    QCoreApplication::setApplicationVersion(APP_VERSION);
    QCoreApplication::setOrganizationName("VAAPIChecker");
    if (!headless) QApplication::setDesktopFileName("io.github.vaapichecker");
    QCommandLineParser parser;
    parser.setApplicationDescription("Native Qt6 / libva capability inspector and Linux video acceleration diagnostics.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"device", "DRM render node to inspect.", "path"});
    parser.addOption({"demo", "Preview explicitly labelled sample capabilities without a GPU."});
    parser.addOption({"theme", "Color theme: light, dark, or system (default).", "mode", "system"});
    parser.addOption({"report", "Write a JSON report and exit (use - for stdout). Exit code 2 means VAAPI unavailable.", "path"});
    parser.addOption({"smoke-test", "Start the GUI, wait for scans, then exit. For automated startup checks."});
    parser.addOption({"screenshot-dir", "Capture each GUI tab as PNG and exit after startup checks.", "directory"});
    parser.process(*app);
    const QString themeName = parser.value("theme").toLower();
    if (themeName != "system" && themeName != "light" && themeName != "dark") {
        QTextStream(stderr) << "Invalid theme. Choose light, dark, or system.\n";
        return 1;
    }
    const auto theme = themeName == "dark" ? checker::ThemeMode::Dark
        : themeName == "light" ? checker::ThemeMode::Light : checker::ThemeMode::System;
    if (parser.isSet("report")) {
        checker::DeviceReport report;
        if (parser.isSet("demo")) {
            report = checker::demoReport();
        } else {
            const auto devices = checker::VaProbe::devices();
            if (parser.isSet("device")) report = checker::VaProbe::scan(checker::VaProbe::deviceInfo(parser.value("device")));
            else if (!devices.isEmpty()) report = checker::VaProbe::scan(devices.first());
            else {
                report.capturedAt = QDateTime::currentDateTime();
                report.error = "No DRM render nodes found. Check the GPU driver or expose /dev/dri to this environment.";
            }
        }
        const QByteArray bytes = QJsonDocument(report.toJson()).toJson(QJsonDocument::Indented);
        if (parser.value("report") == "-") {
            QTextStream(stdout) << QString::fromUtf8(bytes);
        } else {
            QSaveFile file(parser.value("report"));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                QTextStream(stderr) << "Could not save report: " << file.errorString() << '\n';
                return 1;
            }
        }
        return report.available ? 0 : 2;
    }
    checker::MainWindow window({parser.isSet("demo"), parser.value("device"), theme});
    window.show();
    if (parser.isSet("smoke-test") || parser.isSet("screenshot-dir")) {
        QTimer startup;
        startup.setInterval(150);
        int tabIndex = -1;
        const QString output = parser.value("screenshot-dir");
        if (!output.isEmpty() && !QDir().mkpath(output)) {
            QTextStream(stderr) << "Could not create screenshot directory: " << output << '\n';
            return 1;
        }
        QObject::connect(&startup, &QTimer::timeout, &window, [&] {
            if (!window.readyForSmokeTest()) return;
            if (output.isEmpty()) { app->exit(0); return; }
            auto *tabs = window.findChild<QTabWidget *>("mainTabs");
            if (tabIndex >= 0) {
                const QString path = QDir(output).filePath(QStringLiteral("%1-%2.png")
                    .arg(tabIndex + 1).arg(QStringList{"capabilities", "processor", "media", "trace", "gpu-usage"}.at(tabIndex)));
                if (!window.grab().save(path)) { app->exit(1); return; }
                QTextStream(stdout) << "Saved " << path << '\n';
            }
            ++tabIndex;
            if (tabIndex >= tabs->count()) { app->exit(0); return; }
            tabs->setCurrentIndex(tabIndex);
        });
        QTimer::singleShot(65000, &window, [&] { QTextStream(stderr) << "GUI startup timed out.\n"; app->exit(1); });
        startup.start();
        return app->exec();
    }
    return app->exec();
}
