#include "ImageConverter.h"

#include "FormatDetector.h"
#include "MozJpegEncoder.h"
#include "WorkTemp.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QProcess>
#include <QRect>
#include <QScopedPointer>
#include <QTemporaryFile>

namespace {
const int kProcessTimeoutMs = 180000;

int adjustQuality(int quality, const QString &profile) {
    if (profile.contains(QStringLiteral("强")) || profile == QLatin1String("strong")) {
        return qMax(8, quality - 18);
    }
    if (profile.contains(QStringLiteral("均衡")) || profile == QLatin1String("balanced")) {
        return qMax(10, quality - 10);
    }
    return quality;
}

bool runTool(const QString &program, const QStringList &args) {
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

QImage applyResize(QImage image, const CompressionOptions &options) {
    if (!options.resizeEnabled) {
        return image;
    }
    if (options.resizeMode == 2) {
        image = image.scaled(
            options.targetWidth,
            options.targetHeight,
            Qt::KeepAspectRatioByExpanding,
            Qt::SmoothTransformation
        );
        const int cropWidth = qMin(options.targetWidth, image.width());
        const int cropHeight = qMin(options.targetHeight, image.height());
        const int offsetX = qMax(0, (image.width() - cropWidth) / 2);
        const int offsetY = qMax(0, (image.height() - cropHeight) / 2);
        return image.copy(QRect(offsetX, offsetY, cropWidth, cropHeight));
    }
    if (options.resizeMode == 1) {
        return image.scaled(
            options.targetWidth,
            options.targetHeight,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
    }
    return image;
}

QImage decodeImage(const QString &sourcePath, const QString &sourceFormat) {
    QImageReader reader(sourcePath);
    reader.setAutoTransform(true);
    if (!sourceFormat.isEmpty()) {
        reader.setFormat(FormatDetector::qtPluginFormat(sourceFormat));
    }
    return reader.read();
}

ConvertResult writeWithQt(
    const QImage &image,
    const QString &destPath,
    const QString &targetFormat,
    const CompressionOptions &options
) {
    QImageWriter writer(destPath, FormatDetector::qtPluginFormat(targetFormat));
    const int quality = options.lossless
        ? 100
        : qBound(1, adjustQuality(options.quality, options.profile), 100);
    writer.setQuality(quality);
    if (!writer.write(image)) {
        return {false, QStringLiteral("无法写入格式 %1").arg(targetFormat), QStringLiteral("Qt")};
    }
    return {true, QStringLiteral("已转换（中间文件）"), QStringLiteral("Qt")};
}

ConvertResult convertWithQt(
    const QString &sourcePath,
    const QString &destPath,
    const QString &sourceFormat,
    const QString &targetFormat,
    const CompressionOptions &options
) {
    QImage image = decodeImage(sourcePath, sourceFormat);
    if (image.isNull()) {
        if (sourceFormat == QLatin1String("webp")) {
            return {false, QStringLiteral("WebP 解码不可用（缺少 dwebp 或 Qt WebP 插件）"), QStringLiteral("Qt")};
        }
        return {false, QStringLiteral("无法读取图片"), QStringLiteral("Qt")};
    }
    image = applyResize(image, options);
    return writeWithQt(image, destPath, targetFormat, options);
}

ConvertResult convertToJpgMozjpeg(
    const QString &sourcePath,
    const QString &destPath,
    const QString &sourceFormat,
    const CompressionOptions &options
) {
    QImage image = decodeImage(sourcePath, sourceFormat);
    if (image.isNull()) {
        if (sourceFormat == QLatin1String("webp")) {
            return {false, QStringLiteral("WebP 解码失败（缺少 Qt WebP 插件）"), QStringLiteral("Qt")};
        }
        return {false, QStringLiteral("无法读取图片"), QStringLiteral("Qt")};
    }
    image = applyResize(image, options);
    const MozJpegEncodeResult encoded = MozJpegEncoder::encodeImage(
        image, destPath, MozJpegEncoder::effectiveQuality(options)
    );
    return {encoded.success, encoded.message, QStringLiteral("mozjpeg")};
}

ConvertResult convertToWebpCli(
    const QString &sourcePath,
    const QString &destPath,
    const CompressionOptions &options
) {
    const QString cwebp = EngineRegistry::resolveTool({"cwebp"});
    if (cwebp.isEmpty()) {
        return {false, QStringLiteral("缺少引擎 cwebp"), QStringLiteral("cwebp")};
    }
    QStringList args;
    if (options.lossless) {
        args = {"-lossless", "-z", "9", "-m", "5", "-metadata", "none", sourcePath, "-o", destPath};
    } else {
        const int quality = qBound(1, adjustQuality(options.quality, options.profile), 100);
        args = {"-q", QString::number(quality), "-m", "5", "-metadata", "none", sourcePath, "-o", destPath};
    }
    if (!runTool(cwebp, args)) {
        return {false, QStringLiteral("cwebp 转换失败"), QStringLiteral("cwebp")};
    }
    return {true, QStringLiteral("成功"), QStringLiteral("cwebp")};
}

ConvertResult convertFromWebpToPngCli(const QString &sourcePath, const QString &destPath) {
    const QString dwebp = EngineRegistry::resolveTool({"dwebp"});
    if (dwebp.isEmpty()) {
        return {false, QStringLiteral("缺少 dwebp"), QStringLiteral("dwebp")};
    }
    // libwebp dwebp defaults to PNG; there is no -png flag.
    if (!runTool(dwebp, {"-quiet", sourcePath, "-o", destPath})) {
        return {false, QStringLiteral("dwebp 转换失败"), QStringLiteral("dwebp")};
    }
    return {true, QStringLiteral("已转换（中间文件）"), QStringLiteral("dwebp")};
}

ConvertResult convertFromWebpToJpgCli(
    const QString &sourcePath,
    const QString &destPath,
    const CompressionOptions &options
) {
    const QString dwebp = EngineRegistry::resolveTool({"dwebp"});
    const QString cjpeg = EngineRegistry::resolveTool({"cjpeg", "mozjpeg"});
    if (dwebp.isEmpty() || cjpeg.isEmpty()) {
        return {false, QStringLiteral("缺少 dwebp 或 mozjpeg"), QStringLiteral("dwebp+mozjpeg")};
    }
    QScopedPointer<QTemporaryFile> ppmTemp;
    if (!WorkTemp::open(ppmTemp, QStringLiteral("ppm"), QFileInfo(destPath).absoluteDir())) {
        return {false, QStringLiteral("无法创建临时文件"), QStringLiteral("dwebp+mozjpeg")};
    }
    const QString ppmPath = ppmTemp->fileName();
    ppmTemp->close();
    QFile::remove(ppmPath);
    if (!runTool(dwebp, {"-quiet", "-ppm", sourcePath, "-o", ppmPath})) {
        return {false, QStringLiteral("dwebp 解码失败"), QStringLiteral("dwebp+mozjpeg")};
    }
    const int quality = MozJpegEncoder::effectiveQuality(options);
    QFile::remove(destPath);
    if (!runTool(cjpeg, {
            "-quality", QString::number(quality),
            "-progressive", "-optimize",
            "-outfile", destPath, ppmPath
        })) {
        return {false, QStringLiteral("mozjpeg 编码失败"), QStringLiteral("dwebp+mozjpeg")};
    }
    if (!QFileInfo::exists(destPath) || QFileInfo(destPath).size() <= 0) {
        return {false, QStringLiteral("mozjpeg 未生成有效输出"), QStringLiteral("dwebp+mozjpeg")};
    }
    return {true, QStringLiteral("成功（q%1）").arg(quality), QStringLiteral("dwebp+mozjpeg")};
}
}

ConvertResult ImageConverter::convert(
    const QString &sourcePath,
    const QString &destPath,
    const QString &sourceFormat,
    const QString &targetFormat,
    const CompressionOptions &options
) {
    if (targetFormat == QLatin1String("gif") && sourceFormat != QLatin1String("gif")) {
        return {false, QStringLiteral("不支持转换为GIF"), QStringLiteral("gifsicle")};
    }

    QFile::remove(destPath);

    // Same format, copy only (no resize). Resize of JPG goes through mozjpeg below.
    if (sourceFormat == targetFormat && !options.resizeEnabled) {
        if (!QFile::copy(sourcePath, destPath)) {
            return {false, QStringLiteral("无法复制文件"), QStringLiteral("文件系统")};
        }
        return {true, QStringLiteral("已按实际格式输出"), QStringLiteral("原图")};
    }

    // → JPG: decode → PPM → mozjpeg once. Prefer dwebp for WebP sources when available.
    if (targetFormat == QLatin1String("jpg")) {
        if (sourceFormat == QLatin1String("webp") && !options.resizeEnabled) {
            const ConvertResult cli = convertFromWebpToJpgCli(sourcePath, destPath, options);
            if (cli.success) {
                return cli;
            }
        }
        return convertToJpgMozjpeg(sourcePath, destPath, sourceFormat, options);
    }

    // → WebP: always finish with cwebp (true lossy/lossless encode). No Qt WebP as final.
    if (targetFormat == QLatin1String("webp")) {
        if (!options.resizeEnabled) {
            return convertToWebpCli(sourcePath, destPath, options);
        }
        QScopedPointer<QTemporaryFile> pngTemp;
        if (!WorkTemp::open(pngTemp, QStringLiteral("png"), QFileInfo(destPath).absoluteDir())) {
            return {false, QStringLiteral("无法创建临时文件"), QStringLiteral("cwebp")};
        }
        const QString pngPath = pngTemp->fileName();
        pngTemp->close();
        QFile::remove(pngPath);
        const ConvertResult mid = convertWithQt(sourcePath, pngPath, sourceFormat, QStringLiteral("png"), options);
        if (!mid.success) {
            return mid;
        }
        CompressionOptions encodeOpts = options;
        encodeOpts.resizeEnabled = false;
        return convertToWebpCli(pngPath, destPath, encodeOpts);
    }

    // WebP → PNG: dwebp preferred, else Qt intermediate for later pngquant/oxipng.
    if (sourceFormat == QLatin1String("webp") && targetFormat == QLatin1String("png") && !options.resizeEnabled) {
        const ConvertResult cli = convertFromWebpToPngCli(sourcePath, destPath);
        if (cli.success) {
            return cli;
        }
        return convertWithQt(sourcePath, destPath, sourceFormat, targetFormat, options);
    }

    // General (e.g. → PNG): Qt as intermediate only; CompressPipeline must run real compressors.
    return convertWithQt(sourcePath, destPath, sourceFormat, targetFormat, options);
}
