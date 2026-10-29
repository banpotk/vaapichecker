#include "core/media_tools.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

namespace checker {

QList<CodecInfo> parseCodecs(const QString &output, const QString &kind)
{
    static const QRegularExpression row("^\\s*V[A-Z.]{5}\\s+(\\S+)\\s+(.+)$");
    QList<CodecInfo> result;
    for (const auto &line : output.split('\n')) {
        const auto match = row.match(line);
        if (!match.hasMatch() || match.captured(1) == "=")
            continue;
        const QString name = match.captured(1);
        QString implementation = "FFmpeg";
        if (name.contains("vaapi")) implementation = "VAAPI";
        else if (name.contains("qsv")) implementation = "Intel QSV";
        else if (name.contains("nvenc") || name.contains("cuvid")) implementation = "NVIDIA";
        else if (name.contains("v4l2")) implementation = "V4L2";
        else if (name.contains("vulkan")) implementation = "Vulkan";
        result.append({name, kind, implementation, match.captured(2).trimmed()});
    }
    return result;
}

QStringList decodeArguments(const QString &file, const QString &device, int seconds)
{
    return {"-hide_banner", "-nostdin", "-loglevel", "verbose", "-benchmark", "-nostats",
        "-progress", "pipe:1", "-hwaccel", "vaapi", "-hwaccel_device", device,
        "-hwaccel_output_format", "vaapi", "-i", QFileInfo(file).absoluteFilePath(),
        "-map", "0:v:0", "-an", "-sn", "-dn", "-t", QString::number(std::clamp(seconds, 1, 600)),
        // This filter requires VAAPI frames. A software fallback cannot pass this graph.
        "-vf", "scale_vaapi=w=iw:h=ih", "-c:v", "wrapped_avframe", "-f", "null", "-"};
}

MediaTools::MediaTools(QObject *parent, QString ffmpeg, QString ffprobe)
    : QObject(parent), ffmpeg_(ffmpeg.isEmpty() ? QStandardPaths::findExecutable("ffmpeg") : std::move(ffmpeg)),
      ffprobe_(ffprobe.isEmpty() ? QStandardPaths::findExecutable("ffprobe") : std::move(ffprobe))
{
    watchdog_.setSingleShot(true);
    connect(&watchdog_, &QTimer::timeout, this, [this] {
        timedOut_ = true;
        emit logLine("FFmpeg", "Process timed out; stopping it.", true);
        process_.kill();
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &MediaTools::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &MediaTools::readOutput);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &MediaTools::finish);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            failToStart();
    });
}

MediaTools::~MediaTools()
{
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
}

bool MediaTools::busy() const { return job_ != Job::None; }
bool MediaTools::ffmpegAvailable() const { return !ffmpeg_.isEmpty(); }

void MediaTools::start(Job job, const QString &program, const QStringList &arguments, int timeoutMs)
{
    const bool wasBusy = busy();
    job_ = job;
    stdout_.clear(); stderr_.clear(); errorLines_.clear(); progressLines_.clear();
    cancelled_ = false; timedOut_ = false;
    process_.setProgram(program);
    process_.setArguments(arguments);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C");
    environment.insert("AV_LOG_FORCE_NOCOLOR", "1");
    if (job == Job::Decode && traceDirectory_ && traceDirectory_->isValid())
        environment.insert("LIBVA_TRACE", traceDirectory_->path() + "/vaapi-trace");
    process_.setProcessEnvironment(environment);
    if (!wasBusy)
        emit busyChanged(true);
    elapsed_.start();
    watchdog_.start(timeoutMs);
    process_.start();
}

void MediaTools::inventory()
{
    if (busy()) return;
    inventory_ = {};
    if (!ffmpegAvailable()) {
        inventory_.error = "FFmpeg was not found in PATH. Install ffmpeg to inspect media components and run decode tests.";
        emit inventoryReady(inventory_);
        return;
    }
    start(Job::Version, ffmpeg_, {"-hide_banner", "-version"});
}

void MediaTools::codecDetails(const CodecInfo &codec)
{
    if (busy() || !ffmpegAvailable()) return;
    start(Job::Details, ffmpeg_, {"-hide_banner", "-h", (codec.kind == "Decoder" ? "decoder=" : "encoder=") + codec.name});
}

void MediaTools::inspectFile(const QString &path)
{
    if (busy()) return;
    if (ffprobe_.isEmpty()) {
        emit fileReady({}, "ffprobe was not found in PATH.");
        return;
    }
    start(Job::Inspect, ffprobe_, {"-v", "error", "-select_streams", "v:0", "-show_streams", "-show_format",
        "-of", "json", QFileInfo(path).absoluteFilePath()});
}

void MediaTools::decodeFile(const QString &path, const QString &device, int seconds, bool trace)
{
    if (busy()) return;
    if (!ffmpegAvailable() || !QFileInfo(path).isFile() || device.isEmpty() || device == "demo") {
        DecodeResult result;
        result.summary = "A readable local video, a real render node and FFmpeg are required.";
        emit decodeFinished(result);
        return;
    }
    frames_ = 0; speed_.clear(); time_.clear();
    traceDirectory_.reset();
    if (trace) {
        traceDirectory_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/vaapi-checker-trace-XXXXXX");
        if (!traceDirectory_->isValid()) {
            DecodeResult result;
            result.summary = "Could not create the temporary libva trace directory.";
            emit decodeFinished(result);
            return;
        }
    }
    emit logLine("Test", "VAAPI decode test: " + QFileInfo(path).absoluteFilePath() + " on " + device, false);
    start(Job::Decode, ffmpeg_, decodeArguments(path, device, seconds), 120000 + std::clamp(seconds, 1, 600) * 1000);
}

void MediaTools::cancel()
{
    if (!busy()) return;
    cancelled_ = true;
    process_.terminate();
    QTimer::singleShot(1500, this, [this] {
        if (cancelled_ && process_.state() != QProcess::NotRunning)
            process_.kill();
    });
}

void MediaTools::readOutput()
{
    const QByteArray out = process_.readAllStandardOutput();
    const QByteArray err = process_.readAllStandardError();
    stdout_.append(out);
    stderr_.append(err);
    if (stdout_.size() > 4 * 1024 * 1024) stdout_ = stdout_.right(4 * 1024 * 1024);
    if (stderr_.size() > 1024 * 1024) stderr_ = stderr_.right(1024 * 1024);
    if (job_ != Job::Decode) return;
    errorLines_.append(err);
    int newline = -1;
    while ((newline = errorLines_.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(errorLines_.left(newline)).trimmed();
        errorLines_.remove(0, newline + 1);
        if (!line.isEmpty())
            emit logLine("FFmpeg", line, line.contains("error", Qt::CaseInsensitive) || line.contains("failed", Qt::CaseInsensitive));
    }
    if (errorLines_.size() > 65536) errorLines_ = errorLines_.right(65536);
    progressLines_.append(out);
    while ((newline = progressLines_.indexOf('\n')) >= 0) {
        const auto line = QString::fromUtf8(progressLines_.left(newline)).trimmed();
        progressLines_.remove(0, newline + 1);
        const int equals = line.indexOf('=');
        const QString key = line.left(equals);
        const QString value = line.mid(equals + 1);
        if (key == "frame") frames_ = value.toInt();
        else if (key == "speed") speed_ = value;
        else if (key == "out_time") time_ = value;
        else if (key == "progress") emit progress(frames_, speed_, time_);
    }
    if (progressLines_.size() > 65536) progressLines_ = progressLines_.right(65536);
}

void MediaTools::readTrace()
{
    if (!traceDirectory_ || !traceDirectory_->isValid()) return;
    qint64 remaining = 2 * 1024 * 1024;
    const QDir directory(traceDirectory_->path());
    for (const auto &name : directory.entryList(QDir::Files)) {
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) continue;
        while (!file.atEnd() && remaining > 0) {
            const auto bytes = file.readLine(std::min<qint64>(remaining, 8192));
            remaining -= bytes.size();
            const QString line = QString::fromUtf8(bytes).trimmed();
            if (!line.isEmpty()) emit logLine("VA trace", line, false);
        }
        if (remaining <= 0) {
            emit logLine("VA trace", "Trace display truncated at 2 MiB.", false);
            break;
        }
    }
    traceDirectory_.reset();
}

void MediaTools::finish(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (job_ == Job::None) return;
    watchdog_.stop();
    readOutput();
    const Job completed = job_;
    const bool success = exitCode == 0 && exitStatus == QProcess::NormalExit && !cancelled_ && !timedOut_;
    const QString output = QString::fromUtf8(stdout_);
    const QString diagnostics = QString::fromUtf8(stderr_).trimmed();
    if (completed == Job::Decode) {
        if (!errorLines_.trimmed().isEmpty()) emit logLine("FFmpeg", QString::fromUtf8(errorLines_).trimmed(), false);
        readTrace();
        DecodeResult result;
        result.success = success && frames_ > 0;
        result.cancelled = cancelled_;
        result.frames = frames_;
        result.elapsedSeconds = elapsed_.elapsed() / 1000.0;
        result.diagnostics = diagnostics;
        result.summary = cancelled_ ? "Decode test stopped by user."
            : timedOut_ ? "Decode test timed out."
            : result.success ? QStringLiteral("VAAPI confirmed · %1 frames · %2 s · %3 fps (decode + VAAPI filter)")
                .arg(frames_).arg(result.elapsedSeconds, 0, 'f', 2)
                .arg(frames_ / std::max(0.001, result.elapsedSeconds), 0, 'f', 1)
            : QStringLiteral("VAAPI test failed (exit %1, %2 frames). See Trace Log for driver / FFmpeg details.").arg(exitCode).arg(frames_);
        job_ = Job::None;
        emit busyChanged(false);
        emit decodeFinished(result);
        return;
    }
    if (!success) {
        const QString error = cancelled_ ? "Operation cancelled." : timedOut_ ? "Media tool timed out."
            : diagnostics.isEmpty() ? "Media tool failed (exit " + QString::number(exitCode) + ")." : diagnostics;
        job_ = Job::None;
        emit busyChanged(false);
        if (completed == Job::Inspect) emit fileReady({}, error);
        else if (completed == Job::Details) emit detailsReady(error);
        else { inventory_.error = error; emit inventoryReady(inventory_); }
        return;
    }
    switch (completed) {
    case Job::Version:
        inventory_.version = output.section('\n', 0, 0);
        start(Job::HardwareApis, ffmpeg_, {"-hide_banner", "-hwaccels"}); return;
    case Job::HardwareApis:
        for (const auto &line : output.split('\n')) {
            const auto value = line.trimmed();
            if (!value.isEmpty() && !value.contains(':')) inventory_.hardwareApis.append(value);
        }
        start(Job::Decoders, ffmpeg_, {"-hide_banner", "-decoders"}); return;
    case Job::Decoders:
        inventory_.codecs = parseCodecs(output, "Decoder");
        start(Job::Encoders, ffmpeg_, {"-hide_banner", "-encoders"}); return;
    case Job::Encoders:
        inventory_.codecs.append(parseCodecs(output, "Encoder"));
        break;
    default: break;
    }
    job_ = Job::None;
    emit busyChanged(false);
    if (completed == Job::Encoders) emit inventoryReady(inventory_);
    else if (completed == Job::Details) emit detailsReady(output + (diagnostics.isEmpty() ? "" : '\n' + diagnostics));
    else if (completed == Job::Inspect) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(stdout_, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            emit fileReady({}, "Invalid ffprobe response: " + parseError.errorString());
        else emit fileReady(document.object(), {});
    }
}

void MediaTools::failToStart()
{
    if (job_ == Job::None) return;
    watchdog_.stop();
    const Job failed = job_;
    const QString error = "Could not start " + process_.program() + ": " + process_.errorString();
    job_ = Job::None;
    traceDirectory_.reset();
    emit busyChanged(false);
    if (failed == Job::Decode) {
        DecodeResult result;
        result.summary = error;
        emit logLine("FFmpeg", error, true);
        emit decodeFinished(result);
    } else if (failed == Job::Inspect) emit fileReady({}, error);
    else if (failed == Job::Details) emit detailsReady(error);
    else { inventory_.error = error; emit inventoryReady(inventory_); }
}

} // namespace checker
