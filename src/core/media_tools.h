#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <memory>
#include "core/report.h"

namespace checker {

struct CodecInfo {
    QString name;
    QString kind;
    QString implementation;
    QString description;
};

struct MediaInventory {
    QString version;
    QStringList hardwareApis;
    QList<CodecInfo> codecs;
    QString error;
};

struct DecodeResult {
    bool success = false;
    bool cancelled = false;
    int frames = 0;
    double elapsedSeconds = 0;
    QString summary;
    QString diagnostics;
};

QList<CodecInfo> parseCodecs(const QString &output, const QString &kind);
QStringList decodeArguments(const QString &file, const QString &device, int seconds);

class MediaTools : public QObject {
    Q_OBJECT
public:
    explicit MediaTools(QObject *parent = nullptr, QString ffmpeg = {}, QString ffprobe = {});
    ~MediaTools() override;
    bool busy() const;
    bool ffmpegAvailable() const;
    void inventory();
    void codecDetails(const CodecInfo &codec);
    void inspectFile(const QString &path);
    void decodeFile(const QString &path, const QString &device, int seconds, bool trace);
    void cancel();

signals:
    void inventoryReady(const checker::MediaInventory &inventory);
    void detailsReady(const QString &details);
    void fileReady(const QJsonObject &metadata, const QString &error);
    void decodeFinished(const checker::DecodeResult &result);
    void logLine(const QString &source, const QString &line, bool error);
    void progress(int frames, const QString &speed, const QString &time);
    void busyChanged(bool busy);

private:
    enum class Job { None, Version, HardwareApis, Decoders, Encoders, Details, Inspect, Decode };
    QProcess process_;
    QTimer watchdog_;
    QElapsedTimer elapsed_;
    QString ffmpeg_;
    QString ffprobe_;
    Job job_ = Job::None;
    MediaInventory inventory_;
    QByteArray stdout_;
    QByteArray stderr_;
    QByteArray errorLines_;
    QByteArray progressLines_;
    bool cancelled_ = false;
    bool timedOut_ = false;
    int frames_ = 0;
    QString speed_;
    QString time_;
    std::unique_ptr<QTemporaryDir> traceDirectory_;

    void start(Job job, const QString &program, const QStringList &arguments, int timeoutMs = 15000);
    void readOutput();
    void finish(int exitCode, QProcess::ExitStatus exitStatus);
    void failToStart();
    void readTrace();
};

} // namespace checker

Q_DECLARE_METATYPE(checker::MediaInventory)
Q_DECLARE_METATYPE(checker::DecodeResult)
