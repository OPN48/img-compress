#include "EngineRegistry.h"

#include "core/MozJpegEncoder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QScopedPointer>
#include <QCryptographicHash>
#include <QImageReader>

namespace {
const int kProcessTimeoutMs = 180000;
QString normalizeProfile(const QString &profile) {
    if (profile.contains("强")) {
        return "strong";
    }
    if (profile.contains("均衡")) {
        return "balanced";
    }
    if (profile == "strong" || profile == "balanced" || profile == "high") {
        return profile;
    }
    return "high";
}

int adjustQuality(int quality, const QString &profile) {
    const QString normalized = normalizeProfile(profile);
    if (normalized == "strong") {
        return qMax(8, quality - 18);
    }
    if (normalized == "balanced") {
        return qMax(10, quality - 10);
    }
    return quality;
}

QPair<int, int> getPngquantSettings(const QString &profile, int quality) {
    const QString normalized = normalizeProfile(profile);
    int rangeSize = 14;
    int speed = 3;
    if (normalized == "strong") {
        rangeSize = 34;
        speed = 5;
    } else if (normalized == "balanced") {
        rangeSize = 24;
        speed = 4;
    }
    const int minQ = qMax(20, quality - rangeSize);
    return qMakePair(minQ, speed);
}

int adjustLossy(const QString &profile, int lossy) {
    const QString normalized = normalizeProfile(profile);
    if (normalized == "strong") {
        return qMin(200, static_cast<int>(lossy * 1.6));
    }
    if (normalized == "balanced") {
        return qMin(200, static_cast<int>(lossy * 1.35));
    }
    return lossy;
}

int adjustColors(const QString &profile, int colors) {
    const QString normalized = normalizeProfile(profile);
    if (normalized == "strong") {
        return qMax(32, static_cast<int>(colors * 0.6));
    }
    if (normalized == "balanced") {
        return qMax(32, static_cast<int>(colors * 0.75));
    }
    return colors;
}

QString normalizeSuffix(const QString &suffix) {
    if (suffix == "jpeg") {
        return "jpg";
    }
    return suffix;
}

bool isSameFormat(const QString &outputFormat, const QString &suffix) {
    return outputFormat.isEmpty() || outputFormat == "original" || outputFormat == suffix;
}

bool isCorruptedInput(const QString &output) {
    const QString text = output.toLower();
    return text.contains("corrupt")
        || text.contains("corrupted")
        || text.contains("premature end")
        || text.contains("invalid")
        || text.contains("bad huffman")
        || text.contains("unexpected end")
        || text.contains("read error")
        || text.contains("missing");
}

QString detectPlatform() {
    const QString product = QSysInfo::productType().toLower();
    if (product == "osx" || product == "macos" || product == "darwin") {
        return "macos";
    }
    if (product == "windows" || product == "win") {
        return "windows";
    }
    if (product == "linux") {
        return "linux";
    }
    return product;
}

QString detectArch() {
    const QString arch = QSysInfo::currentCpuArchitecture().toLower();
    if (arch.contains("arm64") || arch.contains("aarch64")) {
        return "arm64";
    }
    if (arch.contains("x86_64") || arch.contains("amd64")) {
        return "x64";
    }
    return arch;
}

QStringList collectVendorBases(const QDir &startDir, const QString &platformKey, const QString &archKey) {
    QStringList bases;
    QDir current = startDir;
    for (int depth = 0; depth < 8; ++depth) {
        const QString vendorRoot = current.filePath("vendor");
        if (QDir(vendorRoot).exists()) {
            bases << vendorRoot
                  << QDir(vendorRoot).filePath(QString("%1/%2").arg(platformKey, archKey))
                  << QDir(vendorRoot).filePath(QString("%1").arg(platformKey));
        }
        if (!current.cdUp()) {
            break;
        }
    }
    return bases;
}

QString findTool(const QStringList &names) {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString platformKey = detectPlatform();
    const QString archKey = detectArch();
    QStringList baseDirs = {
        appDir,
        QDir(appDir).filePath("vendor"),
        QDir(appDir).filePath(QString("vendor/%1/%2").arg(platformKey, archKey)),
        QDir(appDir).filePath(QString("vendor/%1").arg(platformKey)),
        QDir(appDir).filePath("../Resources"),
        QDir(appDir).filePath("../Resources/vendor"),
        QDir(appDir).filePath(QString("../Resources/vendor/%1/%2").arg(platformKey, archKey)),
        QDir(appDir).filePath(QString("../Resources/vendor/%1").arg(platformKey)),
        QDir(appDir).filePath("../MacOS"),
        QDir(appDir).filePath("../MacOS/vendor"),
        QDir(appDir).filePath(QString("../MacOS/vendor/%1/%2").arg(platformKey, archKey)),
        QDir(appDir).filePath(QString("../MacOS/vendor/%1").arg(platformKey)),
        QDir(appDir).filePath("../Frameworks"),
        QDir(appDir).filePath("../Frameworks/vendor"),
        QDir(appDir).filePath(QString("../Frameworks/vendor/%1/%2").arg(platformKey, archKey)),
        QDir(appDir).filePath(QString("../Frameworks/vendor/%1").arg(platformKey)),
    };
    baseDirs += collectVendorBases(QDir(appDir), platformKey, archKey);
    for (const QString &base : baseDirs) {
        for (const QString &name : names) {
            const QString candidate = QDir(base).filePath(name);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
            const QString exeCandidate = QDir(base).filePath(name + ".exe");
            if (QFileInfo::exists(exeCandidate)) {
                return exeCandidate;
            }
        }
    }
    return {};
}

bool runProcess(const QString &program, const QStringList &args) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QPair<bool, QString> runProcessWithOutput(const QString &program, const QStringList &args) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    bool finished = process.waitForFinished(kProcessTimeoutMs);
    if (!finished) {
        process.kill();
        process.waitForFinished(2000);
    }
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    return qMakePair(ok, output);
}

QPair<int, QString> runProcessWithCode(const QString &program, const QStringList &args) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    int code = -1;
    if (process.waitForFinished(kProcessTimeoutMs)) {
        code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    } else {
        process.kill();
        process.waitForFinished(2000);
        code = -2;
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    return qMakePair(code, output);
}

CompressionResult keepOriginal(const QString &source, const QString &output, const QString &message) {
    QFile::remove(output);
    QFile::copy(source, output);
    const qint64 originalSize = QFileInfo(source).size();
    const qint64 outputSize = QFileInfo(output).size();
    return {true, originalSize, outputSize, "原图", message};
}

CompressionResult copyOriginal(const QString &source, const QString &output) {
    QFile::remove(output);
    QFile::copy(source, output);
    const qint64 originalSize = QFileInfo(source).size();
    const qint64 outputSize = QFileInfo(output).size();
    return {true, originalSize, outputSize, "原图", "缺少引擎，已保留原图"};
}

CompressionResult missingEngine(const QString &source, const QString &engine) {
    const qint64 originalSize = QFileInfo(source).size();
    return {false, originalSize, originalSize, engine, "缺少引擎"};
}
}

QStringList EngineRegistry::availableEngines() {
    return {"jpegtran", "mozjpeg", "pngquant", "oxipng", "optipng", "gifsicle", "cwebp", "dwebp"};
}

bool EngineRegistry::toolExists(const QString &name) {
    return !findTool({name}).isEmpty();
}

QString EngineRegistry::resolveTool(const QStringList &names) {
    return findTool(names);
}

QString EngineRegistry::engineStatus(bool lossless) {
    const QString jpegtran = findTool({"jpegtran"});
    const QString cjpeg = findTool({"cjpeg", "mozjpeg"});
    const QString pngquant = findTool({"pngquant"});
    const QString oxipng = findTool({"oxipng"});
    const QString optipng = findTool({"optipng"});
    const QString gifsicle = findTool({"gifsicle"});
    const QString cwebp = findTool({"cwebp"});
    const QString dwebp = findTool({"dwebp"});
    const QString jpgLossless = jpegtran.isEmpty() ? "不可用" : "jpegtran";
    const QString jpgLossy = cjpeg.isEmpty() ? "不可用" : "mozjpeg";
    const QString pngLossless = !oxipng.isEmpty() ? "oxipng" : (!optipng.isEmpty() ? "optipng" : "不可用");
    const QString pngLossy = pngquant.isEmpty() ? "不可用" : "pngquant";
    const QString gifEngine = gifsicle.isEmpty() ? "不可用" : "gifsicle";
    const QString webpEncode = cwebp.isEmpty() ? "不可用" : "cwebp";
    const QString webpDecode = dwebp.isEmpty() ? "不可用" : "dwebp";
    const QString mode = lossless ? "无损优先" : "有损优先";
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString platformKey = detectPlatform();
    const QString archKey = detectArch();
    const QString productType = QSysInfo::productType();
    const QString resourceVendor = QDir(appDir).filePath(QString("../Resources/vendor/%1/%2").arg(platformKey, archKey));
    const bool anyFound = !jpegtran.isEmpty() || !cjpeg.isEmpty() || !pngquant.isEmpty()
        || !oxipng.isEmpty() || !optipng.isEmpty() || !gifsicle.isEmpty() || !cwebp.isEmpty()
        || !dwebp.isEmpty();
    QString status = QString("引擎状态(%1)：JPG 无损(%2) 有损(%3)；PNG 无损(%4) 有损(%5)；GIF(%6)；WebP 编码(%7) 解码(%8)")
        .arg(mode, jpgLossless, jpgLossy, pngLossless, pngLossy, gifEngine, webpEncode, webpDecode);
    status += QString(" | 平台 %1/%2(%3)").arg(platformKey, archKey, productType);
    status += QString(" | vendor(Resources) %1").arg(QDir(resourceVendor).exists() ? "存在" : "缺失");
    if (!anyFound) {
        status += "。未检测到压缩工具，可能未打包或路径未包含 vendor";
    }
    return status;
}

CompressionResult EngineRegistry::compressFile(
    const QString &source,
    const QString &output,
    const CompressionOptions &options
) {
    const QString suffixFromName = normalizeSuffix(QFileInfo(source).suffix().toLower());
    const QByteArray detected = QImageReader::imageFormat(source);
    const QString actualSuffix = normalizeSuffix(QString::fromLatin1(detected).toLower());
    const QString suffix = actualSuffix.isEmpty() ? suffixFromName : actualSuffix;
    const qint64 originalSize = QFileInfo(source).size();
    const QString outputFormat = normalizeSuffix(options.outputFormat.toLower());
    // Cross-format conversion belongs to ImageConverter / CompressPipeline.
    if (!outputFormat.isEmpty() && outputFormat != "original" && outputFormat != suffix) {
        return {
            false,
            originalSize,
            originalSize,
            "pipeline",
            QString("格式不一致（%1→%2），应由转换流水线处理").arg(suffix, outputFormat)
        };
    }
    if (suffix == "jpg") {
        if (options.lossless) {
            const QString jpegtran = findTool({"jpegtran"});
            if (jpegtran.isEmpty()) {
                return missingEngine(source, "jpegtran");
            }
            QStringList args = {"-copy", "none", "-optimize", "-progressive"};
            if (detectPlatform() == "windows") {
                args << "-trim";
            }
            args << "-outfile" << output << source;
            const auto res = runProcessWithCode(jpegtran, args);
            const bool ok = res.first == 0;
            const qint64 outputSize = QFileInfo(output).size();
            if (res.first == -2) {
                if (isSameFormat(outputFormat, suffix)) {
                    return keepOriginal(source, output, "jpegtran 超时，已保留原图");
                }
                return {false, originalSize, outputSize, "jpegtran", "执行超时"};
            }
            if (!ok && isSameFormat(outputFormat, suffix) && isCorruptedInput(res.second)) {
                return keepOriginal(source, output, "源文件异常，已保留原图");
            }
            if (ok && outputSize >= originalSize) {
                if (detectPlatform() == "windows") {
                    const QString jpegoptim = findTool({"jpegoptim"});
                    if (!jpegoptim.isEmpty()) {
                        const QStringList optArgs = {"--strip-all", "--all-progressive", output};
                        const auto optRes = runProcessWithCode(jpegoptim, optArgs);
                        if (optRes.first == 0) {
                            const qint64 newSize = QFileInfo(output).size();
                            if (newSize < outputSize) {
                                return {true, originalSize, newSize, "jpegoptim", "成功（Windows兜底）"};
                            }
                        }
                    }
                }
                QFile srcFile(source);
                QFile outFile(output);
                QByteArray srcHash;
                QByteArray outHash;
                if (srcFile.open(QIODevice::ReadOnly)) {
                    QCryptographicHash h(QCryptographicHash::Sha1);
                    while (!srcFile.atEnd()) {
                        h.addData(srcFile.read(1 << 16));
                    }
                    srcHash = h.result();
                    srcFile.close();
                }
                if (outFile.open(QIODevice::ReadOnly)) {
                    QCryptographicHash h(QCryptographicHash::Sha1);
                    while (!outFile.atEnd()) {
                        h.addData(outFile.read(1 << 16));
                    }
                    outHash = h.result();
                    outFile.close();
                }
                const QString tail = res.second.trimmed();
                if (!srcHash.isEmpty() && !outHash.isEmpty() && srcHash == outHash) {
                    QString msg = "无损无收益（图像未变化）";
                    if (!tail.isEmpty()) {
                        msg = QString("%1：%2").arg(msg, tail);
                    }
                    return {true, originalSize, outputSize, "jpegtran", msg};
                } else {
                    QString msg = "已优化但无体积收益";
                    if (!tail.isEmpty()) {
                        msg = QString("%1：%2").arg(msg, tail);
                    }
                    return {true, originalSize, outputSize, "jpegtran", msg};
                }
            }
            return {ok, originalSize, outputSize, "jpegtran", ok ? "成功" : "失败"};
        }
        // cjpeg cannot read JPEG; mature path is decode → PPM → cjpeg.
        if (findTool({"cjpeg", "mozjpeg"}).isEmpty()) {
            return missingEngine(source, "mozjpeg");
        }
        const MozJpegEncodeResult encoded = MozJpegEncoder::encodeFile(
            source, output, QStringLiteral("jpg"), options
        );
        const qint64 outputSize = QFileInfo(output).size();
        if (!encoded.success) {
            if (isSameFormat(outputFormat, suffix)
                && (encoded.message.contains(QStringLiteral("无法读取"))
                    || encoded.message.contains(QStringLiteral("解码")))) {
                return keepOriginal(source, output, "源文件异常，已保留原图");
            }
            return {false, originalSize, outputSize, "mozjpeg", encoded.message};
        }
        return {true, originalSize, outputSize, "mozjpeg", encoded.message};
    }
    if (suffix == "png") {
        const QString pngquant = findTool({"pngquant"});
        const QString oxipng = findTool({"oxipng"});
        if (!options.lossless) {
            if (pngquant.isEmpty() && oxipng.isEmpty()) {
                return missingEngine(source, "pngquant/oxipng");
            }
            if (!pngquant.isEmpty()) {
                const int quality = qBound(10, adjustQuality(options.quality, options.profile), 100);
                const auto settings = getPngquantSettings(options.profile, quality);
                const QStringList args = {
                    "--quality",
                    QString("%1-%2").arg(settings.first).arg(quality),
                    "--speed",
                    QString::number(settings.second),
                    "--strip",
                    "--skip-if-larger",
                    "--output", output,
                    "--force",
                    source
                };
                const auto res = runProcessWithCode(pngquant, args);
                if (res.first == -2) {
                    if (isSameFormat(outputFormat, suffix)) {
                        return keepOriginal(source, output, "pngquant 超时，已保留原图");
                    }
                    return {false, originalSize, QFileInfo(output).size(), "pngquant", "执行超时"};
                }
                if (res.first == 0) {
                    return {true, originalSize, QFileInfo(output).size(), "pngquant", "成功"};
                }
                if (res.first == 99) {
                    return keepOriginal(source, output, "当前质量下无法进一步缩小，已保留 PNG");
                }
                if (isSameFormat(outputFormat, suffix) && isCorruptedInput(res.second)) {
                    return keepOriginal(source, output, "源文件异常，已保留原图");
                }
                // Fall through: try oxipng, else keep PNG with readable reason.
                if (oxipng.isEmpty()) {
                    const QString tail = res.second.trimmed();
                    QString msg = QString("pngquant 无法处理该图（退出码 %1），已保留 PNG").arg(res.first);
                    if (!tail.isEmpty()) {
                        msg = QString("%1：%2").arg(msg, tail.left(120));
                    }
                    return keepOriginal(source, output, msg);
                }
            }
            // Lossy path oxipng fallback (or pngquant missing).
            if (!oxipng.isEmpty()) {
                const QString normalized = normalizeProfile(options.profile);
                const QString level = normalized == "strong" ? "3" : (normalized == "balanced" ? "2" : "1");
                QStringList args;
                if (source == output) {
                    args = {"-o", level, "--strip", "safe", source};
                } else {
                    args = {"-o", level, "--strip", "safe", "--out", output, source};
                }
                const auto res = runProcessWithCode(oxipng, args);
                if (res.first == 0) {
                    return {true, originalSize, QFileInfo(output).size(), "oxipng", "成功"};
                }
                if (res.first == -2) {
                    if (isSameFormat(outputFormat, suffix)) {
                        return keepOriginal(source, output, "oxipng 超时，已保留原图");
                    }
                    return {false, originalSize, QFileInfo(output).size(), "oxipng", "执行超时"};
                }
            }
            return keepOriginal(source, output, "当前质量下无法进一步缩小，已保留 PNG");
        }
        QString optimizer = oxipng;
        QStringList args;
        const QString normalized = normalizeProfile(options.profile);
        if (!optimizer.isEmpty()) {
                const QString level = normalized == "strong" ? "3" : (normalized == "balanced" ? "2" : "1");
                if (source == output) {
                    args = {"-o", level, "--strip", "safe", source};
                } else {
                    args = {"-o", level, "--strip", "safe", "--out", output, source};
                }
            const auto res = runProcessWithCode(optimizer, args);
            const bool ok = res.first == 0;
            const qint64 outputSize = QFileInfo(output).size();
            if (res.first == -2) {
                if (isSameFormat(outputFormat, suffix)) {
                    return keepOriginal(source, output, "oxipng 超时，已保留原图");
                }
                return {false, originalSize, outputSize, "oxipng", "执行超时"};
            }
            if (!ok && isSameFormat(outputFormat, suffix) && isCorruptedInput(res.second)) {
                return keepOriginal(source, output, "源文件异常，已保留原图");
            }
            if (ok) {
                return {true, originalSize, outputSize, "oxipng", "成功"};
            }
        }
        if (detectPlatform() == "windows") {
            optimizer = findTool({"optipng"});
            if (!optimizer.isEmpty()) {
                    if (source == output) {
                        args = {"-o7", "-strip", "all", source};
                    } else {
                        args = {"-o7", "-strip", "all", "-out", output, source};
                    }
                const auto res = runProcessWithCode(optimizer, args);
                const bool ok = res.first == 0;
                const qint64 outputSize = QFileInfo(output).size();
                if (res.first == -2) {
                    if (isSameFormat(outputFormat, suffix)) {
                        return keepOriginal(source, output, "optipng 超时，已保留原图");
                    }
                    return {false, originalSize, outputSize, "optipng", "执行超时"};
                }
                if (!ok && isSameFormat(outputFormat, suffix) && isCorruptedInput(res.second)) {
                    return keepOriginal(source, output, "源文件异常，已保留原图");
                }
                return {ok, originalSize, outputSize, "optipng", ok ? "成功" : "失败"};
            }
        }
        return missingEngine(source, "oxipng/optipng");
    }
    if (suffix == "gif") {
        const QString gifsicle = findTool({"gifsicle"});
        if (gifsicle.isEmpty()) {
            return missingEngine(source, "gifsicle");
        }
        const QStringList baseArgs = {"-O3", "--no-comments", "--no-names", "--no-extensions"};
        QStringList args = baseArgs;
        const bool useLossy = !options.lossless;
        int lossy = 0;
        int colors = 0;
        if (useLossy) {
            const int quality = qBound(1, adjustQuality(options.quality, options.profile), 100);
            lossy = qMax(0, static_cast<int>((100 - quality) * 2));
            lossy = adjustLossy(options.profile, lossy);
            colors = qMax(32, static_cast<int>(256 * quality / 100));
            colors = adjustColors(options.profile, colors);
            args << QString("--lossy=%1").arg(lossy) << QString("--colors=%1").arg(colors);
        }
        args << source << "-o" << output;
        auto res = runProcessWithOutput(gifsicle, args);
        bool ok = res.first;
        bool usedLossy = useLossy;
        if (!ok && useLossy) {
            QStringList retryArgs = baseArgs;
            retryArgs << source << "-o" << output;
            res = runProcessWithOutput(gifsicle, retryArgs);
            ok = res.first;
            usedLossy = false;
        }
        qint64 outputSize = QFileInfo(output).size();
        if (ok && usedLossy && outputSize >= originalSize) {
            const int retryLossy = qMin(200, static_cast<int>(lossy * 1.3) + 5);
            const int retryColors = qMax(32, static_cast<int>(colors * 0.8));
            QScopedPointer<QTemporaryFile> temp(
                new QTemporaryFile(QDir(QDir::tempPath()).filePath("imgcompress_gif_XXXXXX.gif"))
            );
            temp->setAutoRemove(true);
            bool opened = temp->open();
            if (!opened) {
                temp.reset(new QTemporaryFile(QDir(QFileInfo(output).absolutePath()).filePath("imgcompress_gif_XXXXXX.gif")));
                temp->setAutoRemove(true);
                opened = temp->open();
            }
            if (opened) {
                const QString tempPath = temp->fileName();
                temp->close();
                QStringList retryArgs = baseArgs;
                retryArgs << QString("--lossy=%1").arg(retryLossy) << QString("--colors=%1").arg(retryColors);
                retryArgs << source << "-o" << tempPath;
                const auto retryRes = runProcessWithOutput(gifsicle, retryArgs);
                if (retryRes.first) {
                    const qint64 retrySize = QFileInfo(tempPath).size();
                    if (retrySize > 0 && retrySize < outputSize) {
                        QFile::remove(output);
                        QFile::copy(tempPath, output);
                        outputSize = retrySize;
                        res = retryRes;
                        ok = true;
                    }
                }
            }
        }
        if (!ok && isSameFormat(outputFormat, suffix) && isCorruptedInput(res.second)) {
            return keepOriginal(source, output, "源文件异常，已保留原图");
        }
        QString msg = ok ? "成功" : "失败";
        if (!ok) {
            QString tail = res.second.trimmed();
            if (!tail.isEmpty()) {
                msg = tail;
            }
        }
        return {ok, originalSize, outputSize, "gifsicle", msg};
    }
    if (suffix == "webp") {
        const QString cwebp = findTool({"cwebp"});
        if (cwebp.isEmpty()) {
            return missingEngine(source, "cwebp");
        }
        QStringList args;
        if (options.lossless) {
            args = {"-lossless", "-z", "9", "-m", "5", "-metadata", "none", source, "-o", output};
        } else {
            const int quality = qBound(1, adjustQuality(options.quality, options.profile), 100);
            args = {"-q", QString::number(quality), "-m", "5", "-metadata", "none", source, "-o", output};
        }
        const auto res = runProcessWithCode(cwebp, args);
        const bool ok = res.first == 0;
        const qint64 outputSize = QFileInfo(output).size();
        if (!ok) {
            const QString tail = res.second.trimmed();
            const bool noOutput = !QFileInfo::exists(output);
            if (res.first == -2) {
                if (isSameFormat(outputFormat, suffix)) {
                    return keepOriginal(source, output, "cwebp 超时，已保留原图");
                }
                return {false, originalSize, outputSize, "cwebp", "执行超时"};
            }
            if (isSameFormat(outputFormat, suffix) && (isCorruptedInput(tail) || noOutput)) {
                const QString msg = tail.isEmpty() ? "cwebp 失败，已保留原图" : QString("cwebp 失败，已保留原图：%1").arg(tail);
                return keepOriginal(source, output, msg);
            }
            const QString msg = tail.isEmpty() ? "失败" : tail;
            return {false, originalSize, outputSize, "cwebp", msg};
        }
        return {true, originalSize, outputSize, "cwebp", "成功"};
    }
    return {false, originalSize, originalSize, "无", "不支持的格式"};
}
