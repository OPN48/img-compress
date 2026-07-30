#include "MozJpegEncoder.h"

#include "FormatDetector.h"
#include "WorkTemp.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QIODevice>
#include <QProcess>
#include <QScopedPointer>
#include <QTemporaryFile>

namespace {
const int kProcessTimeoutMs = 180000;

// Same preset semantics as EngineRegistry (pngquant/cwebp): do not dark-reduce
// JPEG quality to chase pngquant save% — quality is a user choice.
int adjustJpegQuality(int quality, const QString &profile) {
    if (profile.contains(QStringLiteral("强")) || profile == QLatin1String("strong")) {
        return qMax(8, quality - 18);
    }
    if (profile.contains(QStringLiteral("均衡")) || profile == QLatin1String("balanced")) {
        return qMax(10, quality - 10);
    }
    return quality;
}

QString runToolCapture(const QString &program, const QStringList &args, bool *ok) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        *ok = false;
        return QStringLiteral("执行超时");
    }
    *ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
}

QImage compositeOnWhite(QImage image) {
    if (!image.hasAlphaChannel()) {
        if (image.format() != QImage::Format_RGB888) {
            return image.convertToFormat(QImage::Format_RGB888);
        }
        return image;
    }
    QImage base(image.size(), QImage::Format_RGB888);
    base.fill(Qt::white);
    QImage src = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *out = base.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            const int a = qAlpha(in[x]);
            const int ia = 255 - a;
            out[x * 3 + 0] = static_cast<uchar>((qRed(in[x]) * a + 255 * ia) / 255);
            out[x * 3 + 1] = static_cast<uchar>((qGreen(in[x]) * a + 255 * ia) / 255);
            out[x * 3 + 2] = static_cast<uchar>((qBlue(in[x]) * a + 255 * ia) / 255);
        }
    }
    return base;
}

bool writeBinaryPpm(const QImage &rgb888, const QString &ppmPath, QString *error) {
    if (rgb888.format() != QImage::Format_RGB888 || rgb888.isNull()) {
        *error = QStringLiteral("PPM 需要 RGB888 图像");
        return false;
    }
    QFile file(ppmPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("无法创建 PPM 文件");
        return false;
    }
    const QByteArray header = QStringLiteral("P6\n%1 %2\n255\n")
                                  .arg(rgb888.width())
                                  .arg(rgb888.height())
                                  .toLatin1();
    if (file.write(header) != header.size()) {
        *error = QStringLiteral("写入 PPM 头失败");
        return false;
    }
    const int rowBytes = rgb888.width() * 3;
    for (int y = 0; y < rgb888.height(); ++y) {
        const uchar *line = rgb888.constScanLine(y);
        if (file.write(reinterpret_cast<const char *>(line), rowBytes) != rowBytes) {
            *error = QStringLiteral("写入 PPM 像素失败");
            return false;
        }
    }
    return true;
}
}

int MozJpegEncoder::effectiveQuality(const CompressionOptions &options) {
    if (options.lossless) {
        return 100;
    }
    return qBound(1, adjustJpegQuality(options.quality, options.profile), 100);
}

MozJpegEncodeResult MozJpegEncoder::encodeImage(QImage image, const QString &destPath, int quality) {
    MozJpegEncodeResult result;
    result.qualityUsed = qBound(1, quality, 100);

    const QString cjpeg = EngineRegistry::resolveTool({"cjpeg", "mozjpeg"});
    if (cjpeg.isEmpty()) {
        result.message = QStringLiteral("缺少引擎 mozjpeg/cjpeg");
        return result;
    }

    image = compositeOnWhite(image);
    if (image.isNull()) {
        result.message = QStringLiteral("无法准备 RGB 图像");
        return result;
    }

    QScopedPointer<QTemporaryFile> ppmTemp;
    if (!WorkTemp::open(ppmTemp, QStringLiteral("ppm"), QFileInfo(destPath).absoluteDir())) {
        result.message = QStringLiteral("无法创建 PPM 临时文件");
        return result;
    }
    const QString ppmPath = ppmTemp->fileName();
    ppmTemp->close();
    QFile::remove(ppmPath);

    QString ppmError;
    if (!writeBinaryPpm(image, ppmPath, &ppmError)) {
        result.message = ppmError;
        return result;
    }

    QFile::remove(destPath);
    bool ok = false;
    const QString toolOut = runToolCapture(
        cjpeg,
        {
            QStringLiteral("-quality"),
            QString::number(result.qualityUsed),
            QStringLiteral("-progressive"),
            QStringLiteral("-optimize"),
            QStringLiteral("-outfile"),
            destPath,
            ppmPath
        },
        &ok
    );
    if (!ok) {
        result.message = toolOut.isEmpty()
            ? QStringLiteral("mozjpeg 编码失败")
            : QStringLiteral("mozjpeg 编码失败：%1").arg(toolOut);
        return result;
    }
    if (!QFileInfo::exists(destPath) || QFileInfo(destPath).size() <= 0) {
        result.message = QStringLiteral("mozjpeg 未生成有效输出");
        return result;
    }

    result.success = true;
    result.message = QStringLiteral("成功（q%1）").arg(result.qualityUsed);
    return result;
}

MozJpegEncodeResult MozJpegEncoder::encodeFile(
    const QString &sourcePath,
    const QString &destPath,
    const QString &sourceFormat,
    const CompressionOptions &options
) {
    QImageReader reader(sourcePath);
    reader.setAutoTransform(true);
    if (!sourceFormat.isEmpty()) {
        reader.setFormat(FormatDetector::qtPluginFormat(sourceFormat));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        MozJpegEncodeResult result;
        result.message = sourceFormat == QLatin1String("webp")
            ? QStringLiteral("WebP 解码失败（缺少 Qt WebP 插件或 dwebp）")
            : QStringLiteral("无法读取图片");
        return result;
    }
    return encodeImage(image, destPath, effectiveQuality(options));
}
