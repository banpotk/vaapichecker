#include "core/va_probe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_str.h>
#include <va/va_vpp.h>

namespace checker {
namespace {

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

QString pciName(const QString &vendorId, const QString &deviceId)
{
    const auto vendor = vendorId.mid(2).toLower();
    const auto device = deviceId.mid(2).toLower();
    for (const auto &path : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"}) {
        QFile file(QString::fromLatin1(path));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QTextStream stream(&file);
        QString vendorName;
        bool insideVendor = false;
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            if (!line.startsWith('\t')) {
                if (insideVendor)
                    break;
                insideVendor = line.startsWith(vendor + "  ");
                if (insideVendor)
                    vendorName = line.mid(6).trimmed();
            } else if (insideVendor && line.startsWith('\t' + device + "  ")) {
                return vendorName + " · " + line.mid(7).trimmed();
            }
        }
        if (!vendorName.isEmpty())
            return vendorName + " · " + deviceId;
    }
    const QMap<QString, QString> vendors{{"0x8086", "Intel"}, {"0x1002", "AMD"},
        {"0x10de", "NVIDIA"}, {"0x1af4", "Virtio"}};
    return vendors.value(vendorId, "GPU") + " · " + (deviceId.isEmpty() ? "DRM device" : deviceId);
}

void logEvent(DeviceReport &report, const QString &event, const QString &detail, bool error = false)
{
    report.events.append({QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), "libva", event, detail, error});
}

bool checked(DeviceReport &report, VAStatus status, const QString &call)
{
    logEvent(report, call, QString::fromLatin1(vaErrorStr(status)), status != VA_STATUS_SUCCESS);
    return status == VA_STATUS_SUCCESS;
}

class Session {
public:
    int fd = -1;
    VADisplay display = nullptr;
    ~Session()
    {
        if (display) {
            vaSetInfoCallback(display, nullptr, nullptr);
            vaSetErrorCallback(display, nullptr, nullptr);
            vaTerminate(display);
        }
        if (fd >= 0)
            close(fd);
    }
    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
};

class Config {
public:
    VADisplay display;
    VAConfigID id = VA_INVALID_ID;
    explicit Config(VADisplay d) : display(d) {}
    ~Config() { if (id != VA_INVALID_ID) vaDestroyConfig(display, id); }
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;
};

class Context {
public:
    VADisplay display;
    VAContextID id = VA_INVALID_ID;
    explicit Context(VADisplay d) : display(d) {}
    ~Context() { if (id != VA_INVALID_ID) vaDestroyContext(display, id); }
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
};

void infoCallback(void *data, const char *message)
{
    logEvent(*static_cast<DeviceReport *>(data), "Driver message", QString::fromUtf8(message).trimmed());
}

void errorCallback(void *data, const char *message)
{
    logEvent(*static_cast<DeviceReport *>(data), "Driver error", QString::fromUtf8(message).trimmed(), true);
}

QString operation(VAEntrypoint entry)
{
    switch (entry) {
    case VAEntrypointVLD:
    case VAEntrypointIZZ:
    case VAEntrypointIDCT:
    case VAEntrypointMoComp:
    case VAEntrypointDeblocking: return "Decode";
    case VAEntrypointEncSlice:
    case VAEntrypointEncPicture:
    case VAEntrypointEncSliceLP: return "Encode";
    case VAEntrypointVideoProc: return "Processing";
    default: return "Other";
    }
}

QString attributeValue(const VAConfigAttrib &attribute)
{
    if (attribute.type == VAConfigAttribRTFormat)
        return renderFormatNames(attribute.value).join(", ");
    if (attribute.type == VAConfigAttribRateControl) {
        QStringList names;
        for (const auto &[flag, name] : QList<QPair<unsigned int, QString>>{
                 {VA_RC_NONE, "None"}, {VA_RC_CBR, "CBR"}, {VA_RC_VBR, "VBR"},
                 {VA_RC_VCM, "VCM"}, {VA_RC_CQP, "CQP"}, {VA_RC_ICQ, "ICQ"},
                 {VA_RC_QVBR, "QVBR"}, {VA_RC_AVBR, "AVBR"}}) {
            if (attribute.value & flag)
                names.append(name);
        }
        return names.join(" / ") + QStringLiteral(" (0x%1)").arg(attribute.value, 0, 16);
    }
    if (attribute.type == VAConfigAttribEncMaxRefFrames)
        return QStringLiteral("L0: %1, L1: %2 (0x%3)")
            .arg(attribute.value & 0xffff).arg(attribute.value >> 16).arg(attribute.value, 0, 16);
    return QStringLiteral("%1 (0x%2)").arg(attribute.value).arg(attribute.value, 0, 16);
}

void querySurfaces(DeviceReport &report, VADisplay display, VAConfigID config, Capability &cap)
{
    unsigned int count = 0;
    VAStatus status = vaQuerySurfaceAttributes(display, config, nullptr, &count);
    if (status != VA_STATUS_SUCCESS || count > 4096) {
        cap.warnings.append("Surface attributes unavailable: " + QString::fromLatin1(vaErrorStr(status)));
        logEvent(report, "vaQuerySurfaceAttributes", cap.warnings.last(), true);
        return;
    }
    std::vector<VASurfaceAttrib> attributes(count);
    status = vaQuerySurfaceAttributes(display, config, attributes.data(), &count);
    if (!checked(report, status, "vaQuerySurfaceAttributes") || count > attributes.size()) {
        cap.warnings.append("Surface format / resolution query failed");
        return;
    }
    for (unsigned int i = 0; i < count; ++i) {
        const auto &attribute = attributes[i];
        if (!attribute.flags || attribute.value.type != VAGenericValueTypeInteger)
            continue;
        const int value = attribute.value.value.i;
        switch (attribute.type) {
        case VASurfaceAttribPixelFormat: cap.pixelFormats.append(fourccName(static_cast<unsigned int>(value))); break;
        case VASurfaceAttribMinWidth: cap.minWidth = value; break;
        case VASurfaceAttribMinHeight: cap.minHeight = value; break;
        case VASurfaceAttribMaxWidth: if (value > 0) cap.maxWidth = value; break;
        case VASurfaceAttribMaxHeight: if (value > 0) cap.maxHeight = value; break;
        case VASurfaceAttribMemoryType:
            cap.attributes.append(QPair<QString, QString>{"Surface memory types", QStringLiteral("0x%1").arg(value, 0, 16)}); break;
        default: break;
        }
    }
    cap.pixelFormats.removeDuplicates();
}

QString filterName(VAProcFilterType type)
{
    switch (type) {
    case VAProcFilterNoiseReduction: return "Noise reduction";
    case VAProcFilterDeinterlacing: return "Deinterlacing";
    case VAProcFilterSharpening: return "Sharpening";
    case VAProcFilterColorBalance: return "Color balance";
    case VAProcFilterSkinToneEnhancement: return "Skin tone enhancement";
    case VAProcFilterTotalColorCorrection: return "Total color correction";
    case VAProcFilterHVSNoiseReduction: return "HVS noise reduction";
    case VAProcFilterHighDynamicRangeToneMapping: return "HDR tone mapping";
    case VAProcFilter3DLUT: return "3D LUT";
    default: return QStringLiteral("Filter %1").arg(type);
    }
}

QString rangeText(const VAProcFilterValueRange &range)
{
    return QStringLiteral("%1 … %2; default %3; step %4")
        .arg(range.min_value).arg(range.max_value).arg(range.default_value).arg(range.step);
}

template<class T, class Formatter>
void filterCaps(DeviceReport &report, VADisplay display, VAContextID context,
                VAProcFilterType type, FilterInfo &filter, Formatter format)
{
    std::vector<T> caps(64);
    unsigned int count = static_cast<unsigned int>(caps.size());
    VAStatus status = vaQueryVideoProcFilterCaps(display, context, type, caps.data(), &count);
    if (status == VA_STATUS_ERROR_MAX_NUM_EXCEEDED && count <= 4096) {
        caps.resize(count);
        status = vaQueryVideoProcFilterCaps(display, context, type, caps.data(), &count);
    }
    if (!checked(report, status, "vaQueryVideoProcFilterCaps · " + filter.name) || count > caps.size()) {
        filter.details.append(QPair<QString, QString>{"Details", "Driver did not return capabilities"});
        return;
    }
    for (unsigned int i = 0; i < count; ++i)
        format(caps[i], filter.details);
    if (!count)
        filter.details.append(QPair<QString, QString>{"Details", "No additional values reported"});
}

QString colorStandard(VAProcColorStandardType type)
{
    static const QStringList names{"None", "BT.601", "BT.709", "BT.470 M", "BT.470 BG",
        "SMPTE 170M", "SMPTE 240M", "Generic film", "sRGB", "stRGB", "xvYCC 601",
        "xvYCC 709", "BT.2020", "Explicit"};
    return type >= 0 && type < names.size() ? names.at(type) : QString::number(type);
}

void queryProcessing(DeviceReport &report, VADisplay display, VAConfigID config)
{
    Context context(display);
    if (!checked(report, vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context.id), "vaCreateContext · VideoProc")) {
        report.warnings.append("Video processing context could not be created.");
        return;
    }
    // Each array is caller-owned. Retry once using the driver's returned counts.
    std::vector<VAProcColorStandardType> inputs(32, VAProcColorStandardCount);
    std::vector<VAProcColorStandardType> outputs(32, VAProcColorStandardCount);
    std::vector<uint32_t> inputFormats(128), outputFormats(128);
    VAProcPipelineCaps caps{};
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    for (int attempt = 0; attempt < 2; ++attempt) {
        caps = {};
        caps.input_color_standards = inputs.data();
        caps.num_input_color_standards = static_cast<uint32_t>(inputs.size());
        caps.output_color_standards = outputs.data();
        caps.num_output_color_standards = static_cast<uint32_t>(outputs.size());
        caps.input_pixel_format = inputFormats.data();
        caps.num_input_pixel_formats = static_cast<uint32_t>(inputFormats.size());
        caps.output_pixel_format = outputFormats.data();
        caps.num_output_pixel_formats = static_cast<uint32_t>(outputFormats.size());
        status = vaQueryVideoProcPipelineCaps(display, context.id, nullptr, 0, &caps);
        if (status != VA_STATUS_ERROR_MAX_NUM_EXCEEDED)
            break;
        if (std::max({caps.num_input_color_standards, caps.num_output_color_standards,
                      caps.num_input_pixel_formats, caps.num_output_pixel_formats}) > 4096)
            break;
        inputs.resize(std::max<size_t>(1, caps.num_input_color_standards), VAProcColorStandardCount);
        outputs.resize(std::max<size_t>(1, caps.num_output_color_standards), VAProcColorStandardCount);
        inputFormats.resize(std::max<size_t>(1, caps.num_input_pixel_formats));
        outputFormats.resize(std::max<size_t>(1, caps.num_output_pixel_formats));
    }
    if (checked(report, status, "vaQueryVideoProcPipelineCaps")
        && caps.num_input_color_standards <= 4096 && caps.num_output_color_standards <= 4096
        && caps.num_input_pixel_formats <= 4096 && caps.num_output_pixel_formats <= 4096
        && (caps.input_color_standards != inputs.data() || caps.num_input_color_standards <= inputs.size())
        && (caps.output_color_standards != outputs.data() || caps.num_output_color_standards <= outputs.size())
        && (caps.input_pixel_format != inputFormats.data() || caps.num_input_pixel_formats <= inputFormats.size())
        && (caps.output_pixel_format != outputFormats.data() || caps.num_output_pixel_formats <= outputFormats.size())) {
        // Some backends replace the array pointers with driver-owned constant arrays.
        // Read the returned pointers; untouched zero FOURCCs mean the field was not filled.
        const auto pixelNames = [](const uint32_t *formats, uint32_t count) {
            QStringList names;
            if (formats)
                for (uint32_t i = 0; i < count; ++i)
                    if (formats[i]) names.append(fourccName(formats[i]));
            names.removeDuplicates();
            return names.isEmpty() ? QStringLiteral("Not reported") : names.join(", ");
        };
        const auto colorNames = [](const VAProcColorStandardType *standards, uint32_t count) {
            QStringList names;
            if (standards)
                for (uint32_t i = 0; i < count; ++i)
                    if (standards[i] != VAProcColorStandardCount) names.append(colorStandard(standards[i]));
            names.removeDuplicates();
            return names.isEmpty() ? QStringLiteral("Not reported") : names.join(", ");
        };
        const auto resolution = [](uint32_t minW, uint32_t minH, uint32_t maxW, uint32_t maxH) {
            return maxW && maxH ? QStringLiteral("%1 × %2 … %3 × %4").arg(minW).arg(minH).arg(maxW).arg(maxH)
                               : QStringLiteral("Not reported");
        };
        QStringList rotations{"0°"};
        for (int i = 1; i < 4; ++i)
            if (caps.rotation_flags & (1u << i)) rotations.append(QString::number(i * 90) + "°");
        report.processing = {{"Input pixel formats", pixelNames(caps.input_pixel_format, caps.num_input_pixel_formats)},
            {"Output pixel formats", pixelNames(caps.output_pixel_format, caps.num_output_pixel_formats)},
            {"Input resolution", resolution(caps.min_input_width, caps.min_input_height, caps.max_input_width, caps.max_input_height)},
            {"Output resolution", resolution(caps.min_output_width, caps.min_output_height, caps.max_output_width, caps.max_output_height)},
            {"Input color standards", colorNames(caps.input_color_standards, caps.num_input_color_standards)},
            {"Output color standards", colorNames(caps.output_color_standards, caps.num_output_color_standards)},
            {"Rotation", rotations.join(", ")},
            {"Mirroring flags", QStringLiteral("0x%1").arg(caps.mirror_flags, 0, 16)},
            {"Blending flags", QStringLiteral("0x%1").arg(caps.blend_flags, 0, 16)},
            {"Pipeline flags", QStringLiteral("0x%1").arg(caps.pipeline_flags, 0, 16)},
            {"Filter flags", QStringLiteral("0x%1").arg(caps.filter_flags, 0, 16)},
            {"Forward references (no filters)", QString::number(caps.num_forward_references)},
            {"Backward references (no filters)", QString::number(caps.num_backward_references)},
            {"Additional output surfaces", QString::number(caps.num_additional_outputs)}};
    } else {
        report.warnings.append("Video processing pipeline capabilities unavailable.");
    }
    std::vector<VAProcFilterType> filters(64);
    unsigned int count = static_cast<unsigned int>(filters.size());
    status = vaQueryVideoProcFilters(display, context.id, filters.data(), &count);
    if (status == VA_STATUS_ERROR_MAX_NUM_EXCEEDED && count <= 4096) {
        filters.resize(count);
        status = vaQueryVideoProcFilters(display, context.id, filters.data(), &count);
    }
    if (!checked(report, status, "vaQueryVideoProcFilters") || count > filters.size()) {
        report.warnings.append("Video processing filter list unavailable.");
        return;
    }
    for (unsigned int i = 0; i < count; ++i) {
        const auto type = filters[i];
        FilterInfo filter{filterName(type), {}};
        switch (type) {
        case VAProcFilterDeinterlacing:
            filterCaps<VAProcFilterCapDeinterlacing>(report, display, context.id, type, filter,
                [](const auto &cap, Properties &details) {
                    const QStringList names{"None", "Bob", "Weave", "Motion adaptive", "Motion compensated"};
                    details.append(QPair<QString, QString>{"Algorithm", cap.type >= 0 && cap.type < names.size() ? names.at(cap.type) : QString::number(cap.type)});
                }); break;
        case VAProcFilterColorBalance:
            filterCaps<VAProcFilterCapColorBalance>(report, display, context.id, type, filter,
                [](const auto &cap, Properties &details) {
                    const QStringList names{"None", "Hue", "Saturation", "Brightness", "Contrast",
                        "Auto saturation", "Auto brightness", "Auto contrast"};
                    details.append(QPair<QString, QString>{cap.type >= 0 && cap.type < names.size() ? names.at(cap.type) : QString::number(cap.type), rangeText(cap.range)});
                }); break;
        case VAProcFilterTotalColorCorrection:
            filterCaps<VAProcFilterCapTotalColorCorrection>(report, display, context.id, type, filter,
                [](const auto &cap, Properties &details) {
                    const QStringList names{"None", "Red", "Green", "Blue", "Cyan", "Magenta", "Yellow"};
                    details.append(QPair<QString, QString>{cap.type >= 0 && cap.type < names.size() ? names.at(cap.type) : QString::number(cap.type), rangeText(cap.range)});
                }); break;
        case VAProcFilterHighDynamicRangeToneMapping:
            filterCaps<VAProcFilterCapHighDynamicRange>(report, display, context.id, type, filter,
                [](const auto &cap, Properties &details) {
                    details.append(QPair<QString, QString>{"Metadata", cap.metadata_type == VAProcHighDynamicRangeMetadataHDR10 ? "HDR10" : QString::number(cap.metadata_type)});
                    details.append(QPair<QString, QString>{"Tone mapping flags", QStringLiteral("0x%1").arg(cap.caps_flag, 0, 16)});
                }); break;
        case VAProcFilter3DLUT:
            filterCaps<VAProcFilterCap3DLUT>(report, display, context.id, type, filter,
                [](const auto &cap, Properties &details) {
                    details.append(QPair<QString, QString>{"LUT size", QString::number(cap.lut_size)});
                    details.append(QPair<QString, QString>{"Bit depth", QString::number(cap.bit_depth)});
                    details.append(QPair<QString, QString>{"Channels", QString::number(cap.num_channel)});
                    details.append(QPair<QString, QString>{"Channel mapping", QStringLiteral("0x%1").arg(cap.channel_mapping, 0, 16)});
                }); break;
        default:
            filterCaps<VAProcFilterCap>(report, display, context.id, type, filter,
                [](const auto &cap, Properties &details) { details.append(QPair<QString, QString>{"Range", rangeText(cap.range)}); }); break;
        }
        report.filters.append(filter);
    }
}

} // namespace

DeviceInfo VaProbe::deviceInfo(const QString &path)
{
    DeviceInfo device;
    device.path = path;
    const QString node = QFileInfo(path).fileName();
    const QString sys = "/sys/class/drm/" + node + "/device";
    device.vendorId = readText(sys + "/vendor");
    device.deviceId = readText(sys + "/device");
    device.driver = QFileInfo(sys + "/driver").symLinkTarget().section('/', -1);
    const QString canonical = QFileInfo(sys).canonicalFilePath().section('/', -1);
    static const QRegularExpression pciPattern("^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]$");
    if (pciPattern.match(canonical).hasMatch())
        device.pciAddress = canonical;
    device.name = pciName(device.vendorId, device.deviceId);
    const auto nodes = QDir(sys + "/drm").entryList({"card*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    if (!nodes.isEmpty())
        device.primaryNode = "/dev/dri/" + nodes.first();
    return device;
}

QList<DeviceInfo> VaProbe::devices()
{
    // sysfs also describes nodes hidden by a container or device sandbox.
    QStringList nodes = QDir("/dev/dri").entryList({"renderD*"}, QDir::System | QDir::Files);
    nodes.append(QDir("/sys/class/drm").entryList({"renderD*"}, QDir::Dirs | QDir::NoDotAndDotDot));
    nodes.removeDuplicates();
    std::sort(nodes.begin(), nodes.end());
    QList<DeviceInfo> result;
    for (const auto &node : nodes)
        result.append(deviceInfo("/dev/dri/" + node));
    return result;
}

DeviceReport VaProbe::scan(const DeviceInfo &device)
{
    DeviceReport report;
    report.device = device;
    report.capturedAt = QDateTime::currentDateTime();
    Session session;
    session.fd = open(QFile::encodeName(device.path).constData(), O_RDWR | O_CLOEXEC);
    if (session.fd < 0) {
        const int reason = errno;
        report.error = "Cannot open " + device.path + ": " + QString::fromLocal8Bit(std::strerror(reason));
        if (reason == EACCES || reason == EPERM)
            report.error += ". Check the device ACL and your render/video group membership, then sign in again.";
        else if (reason == ENOENT)
            report.error += ". The render node is missing or is not exposed to this environment.";
        logEvent(report, "open DRM device", report.error, true);
        return report;
    }
    session.display = vaGetDisplayDRM(session.fd);
    if (!session.display) {
        report.error = "vaGetDisplayDRM did not return a display for " + device.path;
        logEvent(report, "vaGetDisplayDRM", report.error, true);
        return report;
    }
    vaSetInfoCallback(session.display, infoCallback, &report);
    vaSetErrorCallback(session.display, errorCallback, &report);
    int major = 0, minor = 0;
    const VAStatus init = vaInitialize(session.display, &major, &minor);
    if (!checked(report, init, "vaInitialize")) {
        report.error = "VAAPI initialization failed: " + QString::fromLatin1(vaErrorStr(init))
            + ". Verify the VAAPI driver package and LIBVA_DRIVER_NAME / LIBVA_DRIVERS_PATH.";
        return report;
    }
    report.available = true;
    report.apiVersion = QStringLiteral("%1.%2").arg(major).arg(minor);
    const char *vendor = vaQueryVendorString(session.display);
    report.vendor = vendor ? QString::fromUtf8(vendor) : QStringLiteral("Not reported");
    const int maxProfiles = vaMaxNumProfiles(session.display);
    const int maxEntries = vaMaxNumEntrypoints(session.display);
    if (maxProfiles <= 0 || maxEntries <= 0 || maxProfiles > 4096 || maxEntries > 4096) {
        report.error = "Driver returned invalid profile / entrypoint counts.";
        report.available = false;
        return report;
    }
    std::vector<VAProfile> profiles(static_cast<size_t>(maxProfiles));
    int profileCount = 0;
    if (!checked(report, vaQueryConfigProfiles(session.display, profiles.data(), &profileCount), "vaQueryConfigProfiles")
        || profileCount < 0 || profileCount > maxProfiles) {
        report.error = "Unable to enumerate VAAPI profiles.";
        report.available = false;
        return report;
    }
    bool processingQueried = false;
    for (int p = 0; p < profileCount; ++p) {
        const auto profile = profiles[static_cast<size_t>(p)];
        std::vector<VAEntrypoint> entries(static_cast<size_t>(maxEntries));
        int entryCount = 0;
        if (!checked(report, vaQueryConfigEntrypoints(session.display, profile, entries.data(), &entryCount),
                     "vaQueryConfigEntrypoints · " + QString::fromLatin1(vaProfileStr(profile)))
            || entryCount < 0 || entryCount > maxEntries) {
            report.warnings.append("Entrypoints unavailable for " + QString::fromLatin1(vaProfileStr(profile)));
            continue;
        }
        for (int e = 0; e < entryCount; ++e) {
            const auto entry = entries[static_cast<size_t>(e)];
            Capability cap;
            cap.profileId = profile;
            cap.entrypointId = entry;
            cap.profile = QString::fromLatin1(vaProfileStr(profile));
            cap.entrypoint = QString::fromLatin1(vaEntrypointStr(entry));
            cap.operation = operation(entry);
            std::vector<VAConfigAttrib> attributes;
            for (int t = 0; t < VAConfigAttribTypeMax; ++t)
                attributes.push_back({static_cast<VAConfigAttribType>(t), VA_ATTRIB_NOT_SUPPORTED});
            if (checked(report, vaGetConfigAttributes(session.display, profile, entry, attributes.data(),
                                                     static_cast<int>(attributes.size())), "vaGetConfigAttributes · " + cap.profile)) {
                for (const auto &attribute : attributes) {
                    if (attribute.value == VA_ATTRIB_NOT_SUPPORTED)
                        continue;
                    cap.attributes.append(QPair<QString, QString>{QString::fromLatin1(vaConfigAttribTypeStr(attribute.type)), attributeValue(attribute)});
                    if (attribute.type == VAConfigAttribRTFormat)
                        cap.renderFormats = renderFormatNames(attribute.value);
                    if (attribute.type == VAConfigAttribMaxPictureWidth)
                        cap.maxWidth = static_cast<int>(attribute.value);
                    if (attribute.type == VAConfigAttribMaxPictureHeight)
                        cap.maxHeight = static_cast<int>(attribute.value);
                }
            } else {
                cap.warnings.append("Configuration attributes unavailable");
            }
            Config config(session.display);
            if (checked(report, vaCreateConfig(session.display, profile, entry, nullptr, 0, &config.id),
                        "vaCreateConfig · " + cap.profile + " / " + cap.entrypoint)) {
                querySurfaces(report, session.display, config.id, cap);
                if (entry == VAEntrypointVideoProc && !processingQueried) {
                    queryProcessing(report, session.display, config.id);
                    processingQueried = true;
                }
            } else {
                cap.warnings.append("Default configuration could not be created; this entrypoint is advertised only");
            }
            report.capabilities.append(cap);
        }
    }
    const int maxImages = vaMaxNumImageFormats(session.display);
    if (maxImages > 0 && maxImages <= 4096) {
        std::vector<VAImageFormat> formats(static_cast<size_t>(maxImages));
        int count = 0;
        if (checked(report, vaQueryImageFormats(session.display, formats.data(), &count), "vaQueryImageFormats")
            && count >= 0 && count <= maxImages) {
            for (int i = 0; i < count; ++i)
                report.imageFormats.append(fourccName(formats[static_cast<size_t>(i)].fourcc));
            report.imageFormats.removeDuplicates();
        } else {
            report.warnings.append("Image formats unavailable");
        }
    }
    if (report.capabilities.isEmpty())
        report.warnings.append("Driver initialized but reported no supported entrypoints.");
    // Disable callbacks before report is moved/returned; teardown must not touch the moved object.
    vaSetInfoCallback(session.display, nullptr, nullptr);
    vaSetErrorCallback(session.display, nullptr, nullptr);
    return report;
}

} // namespace checker
