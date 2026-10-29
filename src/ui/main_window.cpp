#include "ui/main_window.h"
#include "core/va_probe.h"

#include "ui_capability_tab.h"
#include "ui_main_window.h"
#include "ui_media_tab.h"
#include "ui_processing_tab.h"
#include "ui_trace_tab.h"
#include "ui_usage_tab.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QTabWidget>
#include <QTextStream>
#include <QTreeWidget>
#include <QtConcurrent/QtConcurrentRun>

namespace checker {
namespace {

constexpr int TraceErrorRole = Qt::UserRole + 1;

class ColumnFilter : public QSortFilterProxyModel {
public:
    int column = -1;
    QString value;
    bool vaapiOnly = false;
    explicit ColumnFilter(QObject *parent) : QSortFilterProxyModel(parent)
    {
        setFilterCaseSensitivity(Qt::CaseInsensitive);
        setFilterKeyColumn(-1);
        setSortCaseSensitivity(Qt::CaseInsensitive);
    }
    void select(int col, const QString &text, bool onlyVaapi = false)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
#endif
        column = col; value = text; vaapiOnly = onlyVaapi;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        invalidateFilter();
#endif
    }
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (column >= 0 && !value.isEmpty() && sourceModel()->index(row, column, parent).data().toString() != value)
            return false;
        if (vaapiOnly && sourceModel()->index(row, 2, parent).data().toString() != "VAAPI")
            return false;
        return QSortFilterProxyModel::filterAcceptsRow(row, parent);
    }
};

// View behaviour that uic cannot configure (the .ui files carry model-free settings
// such as alternating rows, no grid and row selection).
void configureView(QTableView *view, QAbstractItemModel *model, bool sorting)
{
    view->setModel(model);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(29);
    view->horizontalHeader()->setStretchLastSection(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->setSortingEnabled(sorting);
}

void configurePropertyTree(QTreeWidget *tree)
{
    tree->header()->setStretchLastSection(true);
    tree->setColumnWidth(0, 235);
}

void properties(QTreeWidget *tree, const Properties &values)
{
    for (const auto &[key, value] : values) {
        auto *item = new QTreeWidgetItem(tree, {key, value});
        item->setToolTip(1, value);
    }
}

QList<QStandardItem *> items(const QStringList &values)
{
    QList<QStandardItem *> result;
    for (const auto &value : values) {
        auto *item = new QStandardItem(value);
        item->setToolTip(value);
        result.append(item);
    }
    return result;
}

QString friendlyProfile(QString profile)
{
    profile.remove("VAProfile");
    static const QRegularExpression boundary("([a-z0-9])([A-Z])");
    profile.replace(boundary, "\\1 \\2");
    profile.replace(QRegularExpression("(Profile|Main)([0-9])"), "\\1 \\2");
    profile.replace("JPEGBaseline", "JPEG Baseline");
    profile.replace("MPEG2", "MPEG-2");
    profile.replace("H264", "H.264");
    return profile;
}

bool saveBytes(QWidget *parent, const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::warning(parent, "Could not save", file.errorString());
        return false;
    }
    return true;
}

QString tableText(const QTableView *view)
{
    const auto *model = view->model();
    QStringList lines;
    QStringList header;
    for (int col = 0; col < model->columnCount(); ++col)
        header.append(model->headerData(col, Qt::Horizontal).toString());
    lines.append(header.join('\t'));
    for (int row = 0; row < model->rowCount(); ++row) {
        QStringList values;
        for (int col = 0; col < model->columnCount(); ++col) {
            QString value = model->index(row, col).data().toString();
            values.append(value.replace('\t', ' ').replace('\n', ' ').replace('\r', ' '));
        }
        lines.append(values.join('\t'));
    }
    return lines.join('\n') + '\n';
}

} // namespace

MainWindow::MainWindow(WindowOptions options, QWidget *parent)
    : QMainWindow(parent), options_(std::move(options)),
      uiMain_(new Ui::MainWindow), uiCapability_(new Ui::CapabilityTab),
      uiProcessing_(new Ui::ProcessingTab), uiMedia_(new Ui::MediaTab),
      uiTrace_(new Ui::TraceTab), uiUsage_(new Ui::UsageTab),
      actionExportJson_(*uiMain_->actionSaveJson),
      actionExportText_(*uiMain_->actionSaveText),
      actionCopyReport_(*uiMain_->actionCopyReport),
      tabs_(*uiMain_->mainTabs),
      driverInfo_(*uiMain_->driverInfo),
      deviceStatus_(*uiMain_->deviceStatus),
      decoderCount_(*uiMain_->decoderStat),
      encoderCount_(*uiMain_->encoderStat),
      processorCount_(*uiMain_->processorStat),
      apiVersion_(*uiMain_->apiStat),
      deviceCombo_(*uiMain_->deviceCombo),
      refresh_(*uiMain_->rescanButton),
      export_(*uiMain_->exportButton),
      chooseNode_(*uiMain_->chooseNodeButton),
      closeButton_(*uiMain_->closeButton),
      themeCombo_(*uiMain_->themeSelector),
      operationCombo_(*uiCapability_->operationFilter),
      capSearch_(*uiCapability_->capabilitySearch),
      copyTable_(*uiCapability_->copyTableButton),
      capTable_(*uiCapability_->capabilityTable),
      capDetails_(*uiCapability_->capabilityDetails),
      capStatus_(*uiCapability_->capabilityStatus),
      processorTree_(*uiProcessing_->processorTree),
      processorDetails_(*uiProcessing_->processorDetails),
      imageFormats_(*uiProcessing_->imageFormats),
      processorStatus_(*uiProcessing_->processorStatus),
      codecKind_(*uiMedia_->codecFilter),
      codecSearch_(*uiMedia_->codecSearch),
      checkMedia_(*uiMedia_->checkMediaButton),
      mediaStatus_(*uiMedia_->mediaStatus),
      codecTable_(*uiMedia_->codecTable),
      codecDetails_(*uiMedia_->codecDetails),
      videoPath_(*uiMedia_->videoPath),
      browseVideo_(*uiMedia_->openVideoButton),
      inspectVideo_(*uiMedia_->inspectVideoButton),
      videoInfo_(*uiMedia_->videoInfo),
      testSeconds_(*uiMedia_->testSeconds),
      captureTrace_(*uiMedia_->captureTrace),
      runTest_(*uiMedia_->runTestButton),
      stopTest_(*uiMedia_->stopTestButton),
      testStatus_(*uiMedia_->testStatus),
      traceSource_(*uiTrace_->traceSource),
      traceSearch_(*uiTrace_->traceSearch),
      openTrace_(*uiTrace_->openTraceButton),
      saveTrace_(*uiTrace_->saveTraceButton),
      clearTrace_(*uiTrace_->clearTraceButton),
      traceTable_(*uiTrace_->traceTable),
      traceDetails_(*uiTrace_->traceDetails),
      traceCount_(*uiTrace_->eventCount),
      followTrace_(*uiTrace_->followLatest),
      usageInterval_(*uiUsage_->sampleInterval),
      saveUsage_(*uiUsage_->saveUsageButton),
      usageStart_(*uiUsage_->startMonitorButton),
      usageTable_(*uiUsage_->usageTable),
      usageStatus_(*uiUsage_->usageStatus)
{
    // formsReady_ already ran createUi(), so all widget aliases are live here.
    setWindowIcon(QIcon(":/icons/packaging/vaapi-checker.svg"));
    theme_ = new ThemeController(this);
    theme_->setMode(options_.theme);

    // Connections whose senders or contexts are members constructed after createUi().
    connect(&probeWatcher_, &QFutureWatcher<DeviceReport>::finished, this, [this] {
        applyReport(probeWatcher_.result());
    });
    connect(&usageTimer_, &QTimer::timeout, this, &MainWindow::sampleUsage);
    connect(&usageWatcher_, &QFutureWatcher<UsageSnapshot>::finished, this, [this] {
        if (sampleGeneration_ == deviceGeneration_ && usageTimer_.isActive())
            applyUsage(usageWatcher_.result());
    });
    connect(&media_, &MediaTools::inventoryReady, this, &MainWindow::applyInventory);
    connect(&media_, &MediaTools::detailsReady, this, [this](const QString &details) {
        const auto selected = codecTable_.selectionModel()->selectedRows();
        if (!selected.isEmpty()
            && codecProxy_->mapToSource(selected.first()).data(Qt::UserRole).toInt() == requestedCodecIndex_)
            codecDetails_.setPlainText(details);
        requestedCodecIndex_ = -1;
    });
    connect(&media_, &MediaTools::busyChanged, this, [this](bool busy) {
        updateMediaControls();
        if (!busy && !pendingInspect_.isEmpty()) {
            const QString file = pendingInspect_;
            pendingInspect_.clear();
            QTimer::singleShot(0, this, [this, file] { if (videoFile_ == file && !media_.busy()) inspectVideo(); });
        }
        if (!busy && pendingCodecIndex_ >= 0)
            QTimer::singleShot(0, this, &MainWindow::querySelectedCodec);
    });
    connect(&media_, &MediaTools::logLine, this, [this](const QString &source, const QString &line, bool error) {
        addEvent({QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), source, source == "VA trace" ? "VAAPI call" : "Message", line, error});
    });
    connect(&media_, &MediaTools::fileReady, this, [this](const QJsonObject &metadata, const QString &error) {
        if (!error.isEmpty()) {
            videoInfo_.setText(error);
            addEvent({QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), "Test", "Inspect failed", error, true});
            return;
        }
        const auto streams = metadata.value("streams").toArray();
        if (streams.isEmpty()) { videoInfo_.setText("This file contains no video stream."); return; }
        const auto stream = streams.first().toObject();
        const auto format = metadata.value("format").toObject();
        videoInfo_.setText(QStringLiteral("%1 · %2 · %3 × %4 · %5 · %6 fps · %7 s")
            .arg(stream.value("codec_name").toString(), stream.value("profile").toString())
            .arg(stream.value("width").toInt()).arg(stream.value("height").toInt())
            .arg(stream.value("pix_fmt").toString(), stream.value("avg_frame_rate").toString(), format.value("duration").toString("unknown")));
    });
    connect(&media_, &MediaTools::progress, this, [this](int frames, const QString &speed, const QString &time) {
        testStatus_.setText(QStringLiteral("Decoding · %1 frames · %2 · speed %3").arg(frames).arg(time, speed));
    });
    connect(&media_, &MediaTools::decodeFinished, this, [this](const DecodeResult &result) {
        testRunning_ = false;
        testStatus_.setText(result.summary);
        testStatus_.setToolTip(result.diagnostics);
        addEvent({QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), "Test", "Decode result", result.summary, !result.success && !result.cancelled});
        updateMediaControls();
    });
    connect(&checkMedia_, &QPushButton::clicked, &media_, &MediaTools::inventory);
    connect(&stopTest_, &QPushButton::clicked, &media_, &MediaTools::cancel);
    connect(&usageInterval_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) { usageTimer_.setInterval(value); });

    reloadDevices(options_.device);
    updateMediaControls();
    QTimer::singleShot(0, this, &MainWindow::rescan);
    QTimer::singleShot(0, &media_, &MediaTools::inventory);
}

MainWindow::~MainWindow()
{
    // Widgets themselves are Qt children of the window; only the uic form structs leak otherwise.
    delete uiMain_;
    delete uiCapability_;
    delete uiProcessing_;
    delete uiMedia_;
    delete uiTrace_;
    delete uiUsage_;
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && traceModel_) updateTraceColors();
}

bool MainWindow::createUi()
{
    resize(1180, 810);
    setMinimumSize(820, 620);

    capabilityPage_ = new QWidget;
    uiCapability_->setupUi(capabilityPage_);
    processingPage_ = new QWidget;
    uiProcessing_->setupUi(processingPage_);
    mediaPage_ = new QWidget;
    uiMedia_->setupUi(mediaPage_);
    tracePage_ = new QWidget;
    uiTrace_->setupUi(tracePage_);
    usagePage_ = new QWidget;
    uiUsage_->setupUi(usagePage_);

    // The order below must match the tab names main.cpp screenshots expect.
    uiMain_->setupUi(this);
    uiMain_->mainTabs->addTab(capabilityPage_, "Decoder / Encoder");
    uiMain_->mainTabs->addTab(processingPage_, "Processor Device");
    uiMain_->mainTabs->addTab(mediaPage_, "Media Components");
    uiMain_->mainTabs->addTab(tracePage_, "Trace Log");
    uiMain_->mainTabs->addTab(usagePage_, "GPU Engine Usage");

    // Designer stores the popup menu and its actions; only the setMenu() call cannot
    // be expressed in a .ui file.
    uiMain_->exportButton->setMenu(uiMain_->exportMenu);
    connect(uiMain_->actionSaveJson, &QAction::triggered, this, [this] { exportReport(true); });
    connect(uiMain_->actionSaveText, &QAction::triggered, this, [this] { exportReport(false); });
    connect(uiMain_->actionCopyReport, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(report_.toText());
        statusBar()->showMessage("Report copied to clipboard", 3000);
    });
    uiMain_->rescanButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(uiMain_->rescanButton, &QPushButton::clicked, this, &MainWindow::rescan);
    connect(uiMain_->aboutButton, &QPushButton::clicked, this, [this] {
        QMessageBox::about(this, "About VAAPI Checker",
            "<b>VAAPI Checker 0.1.0</b><p>Native C++20 / Qt6 Widgets diagnostics for Linux.</p>"
            "<p>Queries libva through DRM render nodes. Media tests require FFmpeg and ffprobe.</p>"
            "<p>Capabilities are advertised by the driver; a decode test verifies a particular file. "
            "GPU monitoring depends on accessible DRM fdinfo counters. Trace covers this app and its test process.</p>"
            "<p><a href='https://intel.github.io/libva/'>libva API documentation</a></p>");
    });
    connect(uiMain_->chooseNodeButton, &QPushButton::clicked, this, [this] {
        if (probeWatcher_.isRunning() || testRunning_) return;
        bool accepted = false;
        const QString path = QInputDialog::getText(this, "DRM render node", "Render node path:",
            QLineEdit::Normal, "/dev/dri/renderD128", &accepted).trimmed();
        if (!accepted || path.isEmpty() || probeWatcher_.isRunning()) return;
        options_.demo = false;
        reloadDevices(path);
        rescan();
    });
    connect(uiMain_->closeButton, &QPushButton::clicked, this, &QWidget::close);

    uiMain_->themeSelector->addItem("Light", static_cast<int>(ThemeMode::Light));
    uiMain_->themeSelector->addItem("Dark", static_cast<int>(ThemeMode::Dark));
    uiMain_->themeSelector->addItem("System", static_cast<int>(ThemeMode::System));
    uiMain_->themeSelector->setCurrentIndex(uiMain_->themeSelector->findData(static_cast<int>(options_.theme)));
    connect(uiMain_->themeSelector, &QComboBox::currentIndexChanged, this, [this] {
        theme_->setMode(static_cast<ThemeMode>(uiMain_->themeSelector->currentData().toInt()));
    });
    connect(uiMain_->deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { rescan(); });

    // Capability tab.
    uiCapability_->operationFilter->addItems({"Decode", "Encode", "Processing", "Other", "All entrypoints"});
    capModel_ = new QStandardItemModel(this);
    capModel_->setHorizontalHeaderLabels({"Profile", "Entrypoint", "Operation", "Render format", "Max resolution", "Sizes"});
    auto *capFilter = new ColumnFilter(this);
    capProxy_ = capFilter;
    capFilter->setSourceModel(capModel_);
    capFilter->select(2, "Decode");
    configureView(uiCapability_->capabilityTable, capFilter, true);
    uiCapability_->capabilityTable->setColumnWidth(0, 215);
    uiCapability_->capabilityTable->setColumnWidth(1, 125);
    uiCapability_->capabilityTable->setColumnWidth(2, 85);
    uiCapability_->capabilityTable->setColumnWidth(3, 180);
    uiCapability_->capabilityTable->setColumnWidth(4, 125);
    uiCapability_->capabilityTable->sortByColumn(0, Qt::AscendingOrder);
    configurePropertyTree(uiCapability_->capabilityDetails);
    uiCapability_->capabilitySplitter->setSizes({340, 150});
    connect(uiCapability_->copyTableButton, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(tableText(uiCapability_->capabilityTable));
    });
    connect(uiCapability_->capabilitySearch, &QLineEdit::textChanged, capFilter, &QSortFilterProxyModel::setFilterFixedString);
    connect(uiCapability_->operationFilter, &QComboBox::currentTextChanged, this, [this, capFilter](const QString &value) {
        capFilter->select(2, value == "All entrypoints" ? "" : value);
        if (capFilter->rowCount()) uiCapability_->capabilityTable->selectRow(0); else uiCapability_->capabilityDetails->clear();
    });
    connect(uiCapability_->capabilityTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::showCapability);

    // Processor tab.
    configurePropertyTree(uiProcessing_->processorDetails);
    uiProcessing_->processorSplitter->setSizes({250, 780});
    connect(uiProcessing_->processorTree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::showProcessor);

    // Media tab.
    uiMedia_->codecFilter->addItems({"Decoders", "VAAPI encoders", "All video components"});
    codecModel_ = new QStandardItemModel(this);
    codecModel_->setHorizontalHeaderLabels({"Component", "Type", "Implementation", "Description"});
    auto *codecFilter = new ColumnFilter(this);
    codecProxy_ = codecFilter;
    codecFilter->setSourceModel(codecModel_);
    codecFilter->select(1, "Decoder");
    configureView(uiMedia_->codecTable, codecFilter, true);
    uiMedia_->codecTable->setColumnWidth(0, 140);
    uiMedia_->codecTable->setColumnWidth(1, 75);
    uiMedia_->codecTable->setColumnWidth(2, 100);
    uiMedia_->codecTable->sortByColumn(0, Qt::AscendingOrder);
    uiMedia_->codecDetails->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    uiMedia_->mediaSplitter->setSizes({660, 380});
    connect(uiMedia_->codecSearch, &QLineEdit::textChanged, codecFilter, &QSortFilterProxyModel::setFilterFixedString);
    connect(uiMedia_->codecFilter, &QComboBox::currentTextChanged, this, [codecFilter](const QString &value) {
        codecFilter->select(1, value == "Decoders" ? "Decoder" : value == "VAAPI encoders" ? "Encoder" : "", value == "VAAPI encoders");
    });
    connect(uiMedia_->codecTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        const auto selected = uiMedia_->codecTable->selectionModel()->selectedRows();
        if (selected.isEmpty()) { pendingCodecIndex_ = -1; return; }
        pendingCodecIndex_ = codecProxy_->mapToSource(selected.first()).data(Qt::UserRole).toInt();
        uiMedia_->codecDetails->setPlainText("Querying component details…");
        querySelectedCodec();
    });
    connect(uiMedia_->openVideoButton, &QPushButton::clicked, this, &MainWindow::selectVideo);
    connect(uiMedia_->inspectVideoButton, &QPushButton::clicked, this, &MainWindow::inspectVideo);
    connect(uiMedia_->runTestButton, &QPushButton::clicked, this, &MainWindow::runTest);

    // Trace tab.
    uiTrace_->traceSource->addItems({"All sources", "libva", "FFmpeg", "VA trace", "Test", "Imported", "Demo"});
    traceModel_ = new QStandardItemModel(this);
    traceModel_->setHorizontalHeaderLabels({"Time", "Source", "Event", "Details"});
    auto *traceFilter = new ColumnFilter(this);
    traceProxy_ = traceFilter;
    traceFilter->setSourceModel(traceModel_);
    configureView(uiTrace_->traceTable, traceFilter, false);
    uiTrace_->traceTable->setColumnWidth(0, 105);
    uiTrace_->traceTable->setColumnWidth(1, 85);
    uiTrace_->traceTable->setColumnWidth(2, 230);
    uiTrace_->traceDetails->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    uiTrace_->traceSplitter->setSizes({350, 120});
    connect(uiTrace_->traceSearch, &QLineEdit::textChanged, traceFilter, &QSortFilterProxyModel::setFilterFixedString);
    connect(uiTrace_->traceSource, &QComboBox::currentTextChanged, this, [traceFilter](const QString &source) {
        traceFilter->select(1, source == "All sources" ? "" : source);
    });
    connect(uiTrace_->openTraceButton, &QPushButton::clicked, this, &MainWindow::importTrace);
    connect(uiTrace_->saveTraceButton, &QPushButton::clicked, this, [this] { saveTable(uiTrace_->traceTable, "vaapi-trace.tsv"); });
    connect(uiTrace_->clearTraceButton, &QPushButton::clicked, this, [this] {
        traceModel_->removeRows(0, traceModel_->rowCount()); traceDetails_.clear(); traceCount_.setText("0 events · newest 5,000 retained");
    });
    connect(uiTrace_->traceTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        const auto selected = uiTrace_->traceTable->selectionModel()->selectedRows();
        if (selected.isEmpty()) return;
        const int row = selected.first().row();
        traceDetails_.setPlainText(traceProxy_->index(row, 2).data().toString() + '\n' + traceProxy_->index(row, 3).data().toString());
    });

    // GPU usage tab.
    usageModel_ = new QStandardItemModel(this);
    usageModel_->setHorizontalHeaderLabels({"Process", "PID", "DRM clients"});
    configureView(uiUsage_->usageTable, usageModel_, true);
    uiUsage_->usageTable->setColumnWidth(0, 230);
    uiUsage_->usageTable->setColumnWidth(1, 100);
    connect(uiUsage_->saveUsageButton, &QPushButton::clicked, this, [this] { saveTable(uiUsage_->usageTable, "gpu-usage.tsv"); });
    connect(uiUsage_->startMonitorButton, &QPushButton::clicked, this, &MainWindow::toggleUsage);

    statusBar()->showMessage("Ready");
    return true;
}

void MainWindow::reloadDevices(const QString &preferred)
{
    const QSignalBlocker blocker(deviceCombo_);
    deviceCombo_.clear(); devices_.clear();
    if (options_.demo) {
        devices_.append(demoReport().device);
    } else {
        devices_ = VaProbe::devices();
        bool found = false;
        for (const auto &device : devices_) if (device.path == preferred) found = true;
        if (!preferred.isEmpty() && !found) devices_.append(VaProbe::deviceInfo(preferred));
    }
    int selected = 0;
    for (int i = 0; i < devices_.size(); ++i) {
        const auto &device = devices_.at(i);
        deviceCombo_.addItem(device.name + " [" + QFileInfo(device.path).fileName() + "]", device.path);
        deviceCombo_.setItemData(i, device.path + '\n' + device.driver + '\n' + device.pciAddress, Qt::ToolTipRole);
        if (device.path == preferred) selected = i;
    }
    if (devices_.isEmpty()) deviceCombo_.addItem("No DRM render nodes found");
    deviceCombo_.setCurrentIndex(selected);
}

void MainWindow::rescan()
{
    if (probeWatcher_.isRunning() || testRunning_) return;
    const QString selected = deviceCombo_.currentData().toString();
    reloadDevices(selected);
    usageTimer_.stop(); usageStart_.setText("Start monitoring"); sampler_.reset();
    ++deviceGeneration_;
    usageModel_->removeRows(0, usageModel_->rowCount());
    usageStatus_.setText("Monitoring stopped. Select Start monitoring to collect a new baseline.");
    refresh_.setEnabled(false); deviceCombo_.setEnabled(false); export_.setEnabled(false);
    deviceStatus_.setText("Scanning…");
    driverInfo_.setText("Querying the VAAPI driver…");
    report_.available = false;
    updateMediaControls();
    if (options_.demo) { applyReport(demoReport()); return; }
    const int index = deviceCombo_.currentIndex();
    if (index < 0 || index >= devices_.size()) {
        DeviceReport empty;
        empty.capturedAt = QDateTime::currentDateTime();
        empty.error = "No DRM render nodes found. Check the GPU driver or expose /dev/dri to this environment.";
        applyReport(empty);
        return;
    }
    const DeviceInfo device = devices_.at(index);
    probeWatcher_.setFuture(QtConcurrent::run([device] { return VaProbe::scan(device); }));
    updateMediaControls();
}

void MainWindow::applyReport(DeviceReport report)
{
    report_ = std::move(report);
    int decoders = 0, encoders = 0;
    capModel_->removeRows(0, capModel_->rowCount()); capDetails_.clear();
    for (int i = 0; i < report_.capabilities.size(); ++i) {
        const auto &cap = report_.capabilities.at(i);
        if (cap.operation == "Decode") ++decoders;
        if (cap.operation == "Encode") ++encoders;
        auto row = items({friendlyProfile(cap.profile), QString(cap.entrypoint).remove("VAEntrypoint"), cap.operation,
            cap.renderFormats.isEmpty() ? "Not reported" : cap.renderFormats.join(", "), cap.resolution(), cap.resolutionClasses()});
        row.first()->setData(i, Qt::UserRole); row.first()->setToolTip(cap.profile);
        capModel_->appendRow(row);
    }
    decoderCount_.setText(report_.available ? QString::number(decoders) : "—");
    encoderCount_.setText(report_.available ? QString::number(encoders) : "—");
    processorCount_.setText(report_.available ? QString::number(report_.filters.size()) : "—");
    apiVersion_.setText(report_.apiVersion.isEmpty() ? "—" : report_.apiVersion);
    driverInfo_.setText(report_.demo ? "DEMONSTRATION MODE · sample capabilities, not hardware measurements"
        : report_.available ? report_.vendor + " · " + report_.device.driver + " · " + report_.device.pciAddress
        : report_.error);
    deviceStatus_.setText(report_.demo ? "Demo data" : report_.available ? "VAAPI ready" : "VAAPI unavailable");
    capStatus_.setText(report_.available
        ? QStringLiteral("%1 decode / %2 encode entrypoints · size labels use driver-reported bounds; test a file to verify playback.").arg(decoders).arg(encoders)
        : report_.error);
    if (!report_.warnings.isEmpty()) capStatus_.setText(capStatus_.text() + "\n" + report_.warnings.join("\n"));
    if (capProxy_->rowCount()) capTable_.selectRow(0);
    processorTree_.clear(); processorDetails_.clear();
    auto *pipeline = new QTreeWidgetItem(&processorTree_, {"Pipeline (without filters)"});
    pipeline->setData(0, Qt::UserRole, -1);
    for (int i = 0; i < report_.filters.size(); ++i) {
        auto *item = new QTreeWidgetItem(&processorTree_, {report_.filters.at(i).name});
        item->setData(0, Qt::UserRole, i);
    }
    processorTree_.setCurrentItem(pipeline);
    imageFormats_.setText("Image transfer formats: " + (report_.imageFormats.isEmpty() ? "Not reported" : report_.imageFormats.join(", ")));
    processorStatus_.setText(!report_.available ? report_.error : report_.processing.isEmpty()
        ? "The driver did not return pipeline capabilities. See Trace Log for query results."
        : "Pipeline bounds are queried without filters. Individual filter combinations may impose additional requirements.");
    for (const auto &event : report_.events) addEvent(event);
    refresh_.setEnabled(true); deviceCombo_.setEnabled(!devices_.isEmpty()); export_.setEnabled(true);
    usageStart_.setEnabled(!report_.demo && !report_.device.path.isEmpty());
    if (report_.demo) usageStatus_.setText("Monitoring requires a real GPU. Restart without --demo or choose a render node.");
    statusBar()->showMessage(report_.available ? "Device scan complete" : "Device scan complete · VAAPI unavailable", 5000);
    updateMediaControls();
    emit scanCompleted(report_.available);
}

void MainWindow::showCapability()
{
    capDetails_.clear();
    const auto selected = capTable_.selectionModel()->selectedRows();
    if (selected.isEmpty()) return;
    const int index = capProxy_->mapToSource(selected.first()).data(Qt::UserRole).toInt();
    if (index < 0 || index >= report_.capabilities.size()) return;
    const auto &cap = report_.capabilities.at(index);
    properties(&capDetails_, {{"Profile", cap.profile}, {"Entrypoint", cap.entrypoint},
        {"Surface pixel formats", cap.pixelFormats.isEmpty() ? "Not reported" : cap.pixelFormats.join(", ")},
        {"Minimum surface size", cap.minWidth && cap.minHeight ? QStringLiteral("%1 × %2").arg(cap.minWidth).arg(cap.minHeight) : "Not reported"},
        {"Maximum surface size", cap.resolution()}});
    properties(&capDetails_, cap.attributes);
    for (const auto &warning : cap.warnings) properties(&capDetails_, {{"Query warning", warning}});
}

void MainWindow::showProcessor()
{
    processorDetails_.clear();
    const auto selected = processorTree_.selectedItems();
    if (selected.isEmpty()) return;
    const int index = selected.first()->data(0, Qt::UserRole).toInt();
    if (index < 0) properties(&processorDetails_, report_.processing);
    else if (index < report_.filters.size()) properties(&processorDetails_, report_.filters.at(index).details);
}

void MainWindow::applyInventory(const MediaInventory &inventory)
{
    requestedCodecIndex_ = -1; pendingCodecIndex_ = -1;
    inventory_ = inventory;
    codecModel_->removeRows(0, codecModel_->rowCount());
    for (int i = 0; i < inventory.codecs.size(); ++i) {
        const auto &codec = inventory.codecs.at(i);
        auto row = items({codec.name, codec.kind, codec.implementation, codec.description});
        row.first()->setData(i, Qt::UserRole); codecModel_->appendRow(row);
    }
    mediaStatus_.setText(inventory.error.isEmpty()
        ? inventory.version + "\nCompiled hardware APIs: " + inventory.hardwareApis.join(", ")
        : inventory.error);
    if (!inventory.error.isEmpty()) addEvent({QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), "FFmpeg", "Inventory", inventory.error, true});
    updateMediaControls();
}

void MainWindow::querySelectedCodec()
{
    if (media_.busy() || pendingCodecIndex_ < 0 || pendingCodecIndex_ >= inventory_.codecs.size()) return;
    requestedCodecIndex_ = pendingCodecIndex_;
    pendingCodecIndex_ = -1;
    media_.codecDetails(inventory_.codecs.at(requestedCodecIndex_));
}

void MainWindow::updateMediaControls()
{
    const bool idle = !media_.busy();
    checkMedia_.setEnabled(idle);
    browseVideo_.setEnabled(idle); inspectVideo_.setEnabled(idle && !videoFile_.isEmpty());
    runTest_.setEnabled(idle && media_.ffmpegAvailable() && !videoFile_.isEmpty() && report_.available && !report_.demo);
    stopTest_.setEnabled(testRunning_ && media_.busy());
    testSeconds_.setEnabled(idle); captureTrace_.setEnabled(idle);
    deviceCombo_.setEnabled(!probeWatcher_.isRunning() && !testRunning_ && !devices_.isEmpty());
    refresh_.setEnabled(!probeWatcher_.isRunning() && !testRunning_);
}

void MainWindow::selectVideo()
{
    const QString path = QFileDialog::getOpenFileName(this, "Open video", {}, "Video files (*.mp4 *.mkv *.webm *.mov *.avi *.m4v *.ts *.mpeg *.mpg *.h264 *.hevc *.av1);;All files (*)");
    if (path.isEmpty()) return;
    videoFile_ = path; videoPath_.setText(path); testStatus_.setText("Ready to test the selected video.");
    updateMediaControls();
    if (!media_.busy()) inspectVideo(); else pendingInspect_ = path;
}

void MainWindow::inspectVideo()
{
    if (videoFile_.isEmpty() || media_.busy()) return;
    videoInfo_.setText("Inspecting video…");
    media_.inspectFile(videoFile_);
}

void MainWindow::runTest()
{
    if (media_.busy() || !report_.available || report_.demo || videoFile_.isEmpty()) return;
    testRunning_ = true; testStatus_.setText("Starting VAAPI decode…");
    media_.decodeFile(videoFile_, report_.device.path, testSeconds_.value(), captureTrace_.isChecked());
    updateMediaControls();
}

void MainWindow::addEvent(const ProbeEvent &event)
{
    if (traceModel_->rowCount() >= 5000) traceModel_->removeRow(0);
    auto row = items({event.time, event.source, event.event, event.detail});
    row.first()->setData(event.error, TraceErrorRole);
    if (event.error)
        for (auto *item : row) item->setForeground(QColor(palette().color(QPalette::Window).lightness() > 128 ? "#b42318" : "#ff9c91"));
    traceModel_->appendRow(row);
    traceCount_.setText(QStringLiteral("%1 events · newest 5,000 retained").arg(traceModel_->rowCount()));
    if (followTrace_.isChecked()) traceTable_.scrollToBottom();
}

void MainWindow::updateTraceColors()
{
    const QColor color(palette().color(QPalette::Window).lightness() > 128 ? "#b42318" : "#ff9c91");
    for (int row = 0; row < traceModel_->rowCount(); ++row) {
        if (!traceModel_->item(row, 0)->data(TraceErrorRole).toBool()) continue;
        for (int column = 0; column < traceModel_->columnCount(); ++column)
            traceModel_->item(row, column)->setForeground(color);
    }
}

void MainWindow::exportReport(bool json)
{
    const QString path = QFileDialog::getSaveFileName(this, "Save capability report", json ? "vaapi-report.json" : "vaapi-report.txt",
        json ? "JSON (*.json)" : "Text (*.txt)");
    if (path.isEmpty()) return;
    const auto bytes = json ? QJsonDocument(report_.toJson()).toJson(QJsonDocument::Indented) : report_.toText().toUtf8();
    if (saveBytes(this, path, bytes)) statusBar()->showMessage("Report saved to " + path, 5000);
}

void MainWindow::saveTable(QTableView *view, const QString &suggested)
{
    const QString path = QFileDialog::getSaveFileName(this, "Save displayed rows", suggested, "Tab-separated text (*.tsv);;Text (*.txt)");
    if (!path.isEmpty() && saveBytes(this, path, tableText(view).toUtf8())) statusBar()->showMessage("Displayed rows saved", 3000);
}

void MainWindow::importTrace()
{
    const QString path = QFileDialog::getOpenFileName(this, "Open libva trace", {}, "Trace files (*.log *.txt *.trace);;All files (*)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, "Could not open trace", file.errorString()); return; }
    const auto bytes = file.read(16 * 1024 * 1024);
    for (const auto &line : QString::fromUtf8(bytes).split('\n'))
        if (!line.trimmed().isEmpty()) addEvent({"—", "Imported", QFileInfo(path).fileName(), line.trimmed(), false});
    if (!file.atEnd()) addEvent({"—", "Imported", "Truncated", "Imported only the first 16 MiB. Newest 5,000 events are retained.", false});
}

void MainWindow::toggleUsage()
{
    if (usageTimer_.isActive()) {
        usageTimer_.stop(); usageStart_.setText("Start monitoring"); usageStatus_.setText("Monitoring stopped. Last sample retained.");
        return;
    }
    if (report_.demo || report_.device.path.isEmpty()) return;
    sampler_ = std::make_shared<UsageSampler>(report_.device);
    ++deviceGeneration_; // Invalidate any in-flight sample from a previous session.
    usageStart_.setText("Stop monitoring"); usageTimer_.start(usageInterval_.value());
    usageStatus_.setText("Collecting a baseline…"); sampleUsage();
}

void MainWindow::sampleUsage()
{
    if (usageWatcher_.isRunning() || !sampler_ || !usageTimer_.isActive()) return;
    sampleGeneration_ = deviceGeneration_;
    auto sampler = sampler_;
    usageWatcher_.setFuture(QtConcurrent::run([sampler] { return sampler->sample(); }));
}

void MainWindow::applyUsage(const UsageSnapshot &snapshot)
{
    const int sortColumn = usageTable_.horizontalHeader()->sortIndicatorSection();
    const auto sortOrder = usageTable_.horizontalHeader()->sortIndicatorOrder();
    usageTable_.setSortingEnabled(false);
    usageModel_->clear();
    QStringList headers{"Process", "PID", "DRM clients"};
    for (const auto &engine : snapshot.engines) headers.append(engine + " (%)");
    usageModel_->setHorizontalHeaderLabels(headers);
    for (const auto &process : snapshot.rows) {
        auto row = items({process.process});
        auto *pid = new QStandardItem; pid->setData(process.pid, Qt::DisplayRole); row.append(pid);
        auto *clients = new QStandardItem; clients->setData(process.clients, Qt::DisplayRole); row.append(clients);
        for (const auto &engine : snapshot.engines) {
            auto *item = new QStandardItem;
            const auto value = process.utilization.value(engine);
            if (value) item->setData(qRound(*value * 10) / 10.0, Qt::DisplayRole);
            else item->setText("—");
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            item->setToolTip(value ? "Busy time / elapsed time / engine capacity × 100" : "No comparable counter in the previous sample");
            row.append(item);
        }
        usageModel_->appendRow(row);
    }
    usageTable_.setColumnWidth(0, 230); usageTable_.setColumnWidth(1, 90);
    for (int col = 2; col < usageModel_->columnCount(); ++col) usageTable_.setColumnWidth(col, 130);
    usageTable_.setSortingEnabled(true);
    if (sortColumn >= 0 && sortColumn < usageModel_->columnCount()) usageTable_.sortByColumn(sortColumn, sortOrder);
    usageStatus_.setText(snapshot.status);
}

} // namespace checker
