#include "core/report.h"

#include <QJsonArray>
#include <va/va.h>
#include <va/va_str.h>

namespace checker {

QString Capability::resolution() const
{
    if (maxWidth <= 0 || maxHeight <= 0)
        return QStringLiteral("Not reported");
    return QStringLiteral("%1 × %2").arg(maxWidth).arg(maxHeight);
}

QString Capability::resolutionClasses() const
{
    if (maxWidth <= 0 || maxHeight <= 0)
        return QStringLiteral("Unknown");
    QStringList sizes;
    for (const auto &[name, size] : QList<QPair<QString, QPair<int, int>>>{
             {"SD", {720, 576}}, {"HD", {1280, 720}}, {"FHD", {1920, 1080}},
             {"4K", {3840, 2160}}, {"8K", {7680, 4320}}}) {
        if (maxWidth >= size.first && maxHeight >= size.second
            && minWidth <= size.first && minHeight <= size.second)
            sizes.append(name);
    }
    return sizes.isEmpty() ? QStringLiteral("Custom") : sizes.join(" / ");
}

QString fourccName(unsigned int value)
{
    QString result;
    for (unsigned int i = 0; i < 4; ++i) {
        const auto ch = static_cast<unsigned char>((value >> (i * 8)) & 0xff);
        if (ch < 32 || ch > 126)
            return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
        result.append(QChar(ch));
    }
    return result.trimmed();
}

QStringList renderFormatNames(unsigned int mask)
{
    const QList<QPair<unsigned int, QString>> formats = {
        {VA_RT_FORMAT_YUV420, "YUV420 8-bit"}, {VA_RT_FORMAT_YUV422, "YUV422 8-bit"},
        {VA_RT_FORMAT_YUV444, "YUV444 8-bit"}, {VA_RT_FORMAT_YUV411, "YUV411 8-bit"},
        {VA_RT_FORMAT_YUV400, "YUV400 8-bit"}, {VA_RT_FORMAT_YUV420_10, "YUV420 10-bit"},
        {VA_RT_FORMAT_YUV422_10, "YUV422 10-bit"}, {VA_RT_FORMAT_YUV444_10, "YUV444 10-bit"},
        {VA_RT_FORMAT_YUV420_12, "YUV420 12-bit"}, {VA_RT_FORMAT_YUV422_12, "YUV422 12-bit"},
        {VA_RT_FORMAT_YUV444_12, "YUV444 12-bit"}, {VA_RT_FORMAT_RGB16, "RGB16"},
        {VA_RT_FORMAT_RGB32, "RGB32"}, {VA_RT_FORMAT_RGBP, "RGB planar"},
        {VA_RT_FORMAT_RGB32_10, "RGB32 10-bit"}};
    QStringList result;
    unsigned int known = 0;
    for (const auto &[flag, name] : formats) {
        if ((mask & flag) != 0)
            result.append(name);
        known |= flag;
    }
    if (mask & ~known)
        result.append(QStringLiteral("Other: 0x%1").arg(mask & ~known, 0, 16));
    return result;
}

QJsonObject propertiesJson(const Properties &properties)
{
    QJsonObject object;
    for (const auto &[key, value] : properties) {
        if (!object.contains(key)) {
            object.insert(key, value);
        } else {
            const auto previous = object.value(key);
            QJsonArray values = previous.isArray() ? previous.toArray() : QJsonArray{previous};
            values.append(value);
            object.insert(key, values);
        }
    }
    return object;
}

QJsonObject DeviceReport::toJson() const
{
    QJsonArray caps;
    for (const auto &cap : capabilities) {
        caps.append(QJsonObject{{"profile", cap.profile}, {"profile_id", cap.profileId},
            {"entrypoint", cap.entrypoint}, {"entrypoint_id", cap.entrypointId},
            {"operation", cap.operation}, {"render_formats", QJsonArray::fromStringList(cap.renderFormats)},
            {"pixel_formats", QJsonArray::fromStringList(cap.pixelFormats)},
            {"min_width", cap.minWidth}, {"min_height", cap.minHeight},
            {"max_width", cap.maxWidth}, {"max_height", cap.maxHeight},
            {"attributes", propertiesJson(cap.attributes)},
            {"warnings", QJsonArray::fromStringList(cap.warnings)}});
    }
    QJsonArray filterArray;
    for (const auto &filter : filters)
        filterArray.append(QJsonObject{{"name", filter.name}, {"details", propertiesJson(filter.details)}});
    QJsonArray log;
    for (const auto &event : events)
        log.append(QJsonObject{{"time", event.time}, {"source", event.source},
            {"event", event.event}, {"detail", event.detail}, {"error", event.error}});
    return {{"schema_version", 1}, {"application", "VAAPI Checker"},
        {"captured_at", capturedAt.toUTC().toString(Qt::ISODateWithMs)},
        {"available", available}, {"demo", demo},
        {"device", QJsonObject{{"path", device.path}, {"name", device.name},
            {"kernel_driver", device.driver}, {"pci_address", device.pciAddress},
            {"vendor_id", device.vendorId}, {"device_id", device.deviceId}}},
        {"api_version", apiVersion}, {"driver_vendor", vendor}, {"error", error},
        {"capabilities", caps}, {"processing", propertiesJson(processing)},
        {"filters", filterArray}, {"image_formats", QJsonArray::fromStringList(imageFormats)},
        {"warnings", QJsonArray::fromStringList(warnings)}, {"events", log}};
}

QString DeviceReport::toText() const
{
    QStringList lines{"VAAPI Checker", "Captured: " + capturedAt.toString(Qt::ISODate),
        "Device: " + device.name + " (" + device.path + ")", "Kernel driver: " + device.driver,
        "VA-API: " + apiVersion, "Driver: " + vendor};
    if (demo)
        lines.append("DEMONSTRATION DATA — not a hardware measurement");
    if (!error.isEmpty())
        lines.append("Error: " + error);
    for (const auto &cap : capabilities) {
        lines.append(QStringLiteral("\n%1 | %2 | %3 | %4")
            .arg(cap.profile, cap.entrypoint, cap.operation, cap.resolution()));
        lines.append("  Render formats: " + cap.renderFormats.join(", "));
        lines.append("  Surface formats: " + cap.pixelFormats.join(", "));
        for (const auto &[key, value] : cap.attributes)
            lines.append("  " + key + ": " + value);
        for (const auto &warning : cap.warnings)
            lines.append("  Warning: " + warning);
    }
    lines.append("\nVideo processing (pipeline without filters)");
    for (const auto &[key, value] : processing)
        lines.append(key + ": " + value);
    for (const auto &filter : filters) {
        lines.append("\nFilter: " + filter.name);
        for (const auto &[key, value] : filter.details)
            lines.append("  " + key + ": " + value);
    }
    lines.append("\nImage formats: " + imageFormats.join(", "));
    for (const auto &warning : warnings)
        lines.append("Warning: " + warning);
    return lines.join('\n') + '\n';
}

DeviceReport demoReport()
{
    DeviceReport report;
    report.demo = true;
    report.available = true;
    report.capturedAt = QDateTime::currentDateTime();
    report.device = {"demo", "Sample GPU · demonstration data", "sample", "", "", "", ""};
    report.apiVersion = "1.22 (sample)";
    report.vendor = "Example VAAPI driver — preview only";
    const QList<QPair<VAProfile, QString>> profiles = {
        {VAProfileMPEG2Main, "MPEG-2 Main"}, {VAProfileH264ConstrainedBaseline, "H.264 Constrained Baseline"},
        {VAProfileH264Main, "H.264 Main"}, {VAProfileH264High, "H.264 High"},
        {VAProfileHEVCMain, "HEVC Main"}, {VAProfileHEVCMain10, "HEVC Main 10"},
        {VAProfileVP9Profile0, "VP9 Profile 0"}, {VAProfileVP9Profile2, "VP9 Profile 2"},
        {VAProfileAV1Profile0, "AV1 Profile 0"}, {VAProfileJPEGBaseline, "JPEG Baseline"}};
    for (const auto &[id, name] : profiles) {
        Capability cap;
        cap.profileId = id;
        cap.entrypointId = VAEntrypointVLD;
        Q_UNUSED(name);
        cap.profile = QString::fromLatin1(vaProfileStr(id));
        cap.entrypoint = "VAEntrypointVLD";
        cap.operation = "Decode";
        const bool highDepth = id == VAProfileHEVCMain10 || id == VAProfileVP9Profile2;
        cap.renderFormats = {highDepth ? "YUV420 10-bit" : "YUV420 8-bit"};
        cap.pixelFormats = {highDepth ? "P010" : "NV12"};
        cap.minWidth = cap.minHeight = 64;
        cap.maxWidth = id == VAProfileMPEG2Main ? 2048 : 8192;
        cap.maxHeight = id == VAProfileMPEG2Main ? 2048 : 8192;
        cap.attributes = {{"VAConfigAttribRTFormat", cap.renderFormats.join(", ")},
            {"VAConfigAttribMaxPictureWidth", QString::number(cap.maxWidth)},
            {"VAConfigAttribMaxPictureHeight", QString::number(cap.maxHeight)}};
        report.capabilities.append(cap);
        if (id == VAProfileH264High || id == VAProfileHEVCMain || id == VAProfileHEVCMain10) {
            cap.entrypointId = VAEntrypointEncSlice;
            cap.entrypoint = "VAEntrypointEncSlice";
            cap.operation = "Encode";
            cap.attributes.append(QPair<QString, QString>{"VAConfigAttribRateControl", "CBR / VBR / CQP (sample)"});
            report.capabilities.append(cap);
        }
    }
    report.processing = {{"Input pixel formats", "NV12, P010, YUY2, RGBA (sample)"},
        {"Output pixel formats", "NV12, P010, BGRA (sample)"},
        {"Input resolution", "64 × 64 … 8192 × 8192 (sample)"},
        {"Output resolution", "64 × 64 … 8192 × 8192 (sample)"},
        {"Input color standards", "BT.601, BT.709, BT.2020 (sample)"},
        {"Output color standards", "BT.601, BT.709, BT.2020 (sample)"},
        {"Rotation", "0°, 90°, 180°, 270° (sample)"}};
    report.filters = {{"Noise reduction", {{"Range", "0 … 64 (sample)"}}},
        {"Deinterlacing", {{"Algorithms", "Bob, Motion adaptive (sample)"}}},
        {"Sharpening", {{"Range", "0 … 64 (sample)"}}},
        {"Color balance", {{"Hue", "−180 … 180 (sample)"}, {"Saturation", "0 … 10 (sample)"},
            {"Brightness", "−100 … 100 (sample)"}, {"Contrast", "0 … 10 (sample)"}}}};
    report.imageFormats = {"NV12", "P010", "YUY2", "BGRA", "RGBA"};
    report.events = {{QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), "Demo", "Preview loaded",
        "All capability values in this mode are sample data. Media inventory still reads the local FFmpeg installation.", false}};
    return report;
}

} // namespace checker
