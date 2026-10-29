#pragma once

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <memory>
#include "core/gpu_monitor.h"
#include "core/media_tools.h"
#include "ui/theme_controller.h"

namespace Ui {
class MainWindow;
class CapabilityTab;
class ProcessingTab;
class MediaTab;
class TraceTab;
class UsageTab;
} // namespace Ui

namespace checker {

struct WindowOptions {
    bool demo = false;
    QString device;
    ThemeMode theme = ThemeMode::System;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(WindowOptions options = {}, QWidget *parent = nullptr);
    ~MainWindow() override;
    const DeviceReport &report() const { return report_; }
    bool readyForSmokeTest() const { return report_.capturedAt.isValid() && !probeWatcher_.isRunning() && !media_.busy(); }

signals:
    void scanCompleted(bool available);

private:
    WindowOptions options_;

    // Designer .ui forms; createUi() loads them with the generated setupUi().
    Ui::MainWindow *uiMain_ = nullptr;
    Ui::CapabilityTab *uiCapability_ = nullptr;
    Ui::ProcessingTab *uiProcessing_ = nullptr;
    Ui::MediaTab *uiMedia_ = nullptr;
    Ui::TraceTab *uiTrace_ = nullptr;
    Ui::UsageTab *uiUsage_ = nullptr;

    // Tab pages and models owned by createUi(); declared before formsReady_ so they
    // are initialised before it fills them.
    QWidget *capabilityPage_ = nullptr;
    QWidget *processingPage_ = nullptr;
    QWidget *mediaPage_ = nullptr;
    QWidget *tracePage_ = nullptr;
    QWidget *usagePage_ = nullptr;
    QStandardItemModel *capModel_ = nullptr;
    QSortFilterProxyModel *capProxy_ = nullptr;
    QStandardItemModel *codecModel_ = nullptr;
    QSortFilterProxyModel *codecProxy_ = nullptr;
    QStandardItemModel *traceModel_ = nullptr;
    QSortFilterProxyModel *traceProxy_ = nullptr;
    QStandardItemModel *usageModel_ = nullptr;

    // Members initialise in declaration order: running createUi() here makes every
    // widget from the .ui files exist before the reference members below bind to it.
    const bool formsReady_ = createUi();

    // Shorthand aliases into the generated forms (kept from the pre-.ui code so the
    // logic further down needs no renames).
    QAction &actionExportJson_;
    QAction &actionExportText_;
    QAction &actionCopyReport_;
    QTabWidget &tabs_;
    QLabel &driverInfo_;
    QLabel &deviceStatus_;
    QLabel &decoderCount_;
    QLabel &encoderCount_;
    QLabel &processorCount_;
    QLabel &apiVersion_;
    QComboBox &deviceCombo_;
    QPushButton &refresh_;
    QPushButton &export_;
    QPushButton &chooseNode_;
    QPushButton &closeButton_;
    QComboBox &themeCombo_;

    QComboBox &operationCombo_;
    QLineEdit &capSearch_;
    QPushButton &copyTable_;
    QTableView &capTable_;
    QTreeWidget &capDetails_;
    QLabel &capStatus_;

    QTreeWidget &processorTree_;
    QTreeWidget &processorDetails_;
    QLabel &imageFormats_;
    QLabel &processorStatus_;

    QComboBox &codecKind_;
    QLineEdit &codecSearch_;
    QPushButton &checkMedia_;
    QLabel &mediaStatus_;
    QTableView &codecTable_;
    QPlainTextEdit &codecDetails_;
    QLineEdit &videoPath_;
    QPushButton &browseVideo_;
    QPushButton &inspectVideo_;
    QLabel &videoInfo_;
    QSpinBox &testSeconds_;
    QCheckBox &captureTrace_;
    QPushButton &runTest_;
    QPushButton &stopTest_;
    QLabel &testStatus_;

    QComboBox &traceSource_;
    QLineEdit &traceSearch_;
    QPushButton &openTrace_;
    QPushButton &saveTrace_;
    QPushButton &clearTrace_;
    QTableView &traceTable_;
    QPlainTextEdit &traceDetails_;
    QLabel &traceCount_;
    QCheckBox &followTrace_;

    QSpinBox &usageInterval_;
    QPushButton &saveUsage_;
    QPushButton &usageStart_;
    QTableView &usageTable_;
    QLabel &usageStatus_;

    QList<DeviceInfo> devices_;
    DeviceReport report_;
    MediaInventory inventory_;
    MediaTools media_{this};
    ThemeController *theme_ = nullptr;
    QFutureWatcher<DeviceReport> probeWatcher_;
    QFutureWatcher<UsageSnapshot> usageWatcher_;
    std::shared_ptr<UsageSampler> sampler_;
    QTimer usageTimer_;
    int deviceGeneration_ = 0;
    int sampleGeneration_ = 0;
    QString videoFile_;
    QString pendingInspect_;
    bool testRunning_ = false;
    int requestedCodecIndex_ = -1;
    int pendingCodecIndex_ = -1;

    // Loads all six .ui forms (setupUi) and wires per-widget setup that uic cannot
    // express: models, filters, header tuning, splitter sizes and menu wiring.
    // Returns true only so formsReady_'s initialiser can order it before the aliases.
    bool createUi();
    void changeEvent(QEvent *event) override;
    void reloadDevices(const QString &preferred);
    void rescan();
    void applyReport(DeviceReport report);
    void showCapability();
    void showProcessor();
    void applyInventory(const MediaInventory &inventory);
    void querySelectedCodec();
    void updateMediaControls();
    void selectVideo();
    void inspectVideo();
    void runTest();
    void addEvent(const ProbeEvent &event);
    void updateTraceColors();
    void exportReport(bool json);
    void saveTable(QTableView *table, const QString &suggested);
    void importTrace();
    void toggleUsage();
    void sampleUsage();
    void applyUsage(const UsageSnapshot &snapshot);
};

} // namespace checker
