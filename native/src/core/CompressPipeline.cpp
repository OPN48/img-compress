#include "CompressPipeline.h"

#include "FormatDetector.h"
#include "ImageConverter.h"
#include "WorkTemp.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QScopedPointer>
#include <QTemporaryFile>

namespace {
QString ensureUniquePath(const QString &candidate, const QString &sourcePath, const QString &stem, const QString &suffix) {
    const QFileInfo candidateInfo(candidate);
    const QFileInfo sourceInfo(sourcePath);
    const QString candidatePath = candidateInfo.absoluteFilePath();
    const QString sourceAbsPath = sourceInfo.absoluteFilePath();
    if (candidatePath != sourceAbsPath && !QFileInfo::exists(candidatePath)) {
        return candidatePath;
    }
    const QString ext = suffix.isEmpty() ? QString() : "." + suffix;
    QDir dir = candidateInfo.dir();
    int index = 1;
    while (true) {
        const QString name = QString("%1(%2)%3").arg(stem).arg(index).arg(ext);
        const QString nextPath = dir.filePath(name);
        if (!QFileInfo::exists(nextPath) && nextPath != sourceAbsPath) {
            return nextPath;
        }
        index += 1;
    }
}

QString resolveTargetFormat(const FormatInfo &info, const CompressionOptions &options) {
    const QString raw = options.outputFormat.toLower();
    const QString normalized = FormatDetector::normalizeSuffix(raw);
    if (raw.isEmpty() || raw == "original") {
        return info.effectiveSuffix;
    }
    return normalized;
}

// Converter already applied target-quality lossy encode — do not re-encode or fake-succeed.
bool isFinalLossyEncode(const QString &engine) {
    return engine == "mozjpeg" || engine == "cwebp" || engine == "dwebp+mozjpeg";
}

bool finalizeConvertedFile(
    const QString &convertedPath,
    const QString &outputPath,
    const ConvertResult &converted,
    qint64 sourceSize,
    PipelineOutcome &outcome
) {
    QFile::remove(outputPath);
    if (!QFile::copy(convertedPath, outputPath)) {
        outcome.logs << QString("%1 转换失败：无法写入输出文件").arg(outcome.fileName);
        outcome.hasResult = false;
        return false;
    }
    outcome.result = {
        true,
        sourceSize,
        QFileInfo(outputPath).size(),
        converted.engine,
        converted.message
    };
    return true;
}

QString largerThanSourceMessage(const QString &sourceFormat, const QString &targetFormat) {
    if ((sourceFormat == QLatin1String("jpg") || sourceFormat == QLatin1String("jpeg"))
        && targetFormat == QLatin1String("png")) {
        return QStringLiteral(
            "已转换为 PNG（JPG 为有损紧凑格式，转为 PNG 后像素需完整保存，体积通常大于源 JPG）"
        );
    }
    return QStringLiteral("已转换（目标格式编码后体积大于源文件）");
}

void applyLargerThanSourceMessage(
    PipelineOutcome &outcome,
    const QString &sourceFormat,
    const QString &targetFormat
) {
    if (outcome.result.outputSize > outcome.result.originalSize) {
        outcome.result.message = largerThanSourceMessage(sourceFormat, targetFormat);
    }
}
}

PipelineOutcome CompressPipeline::run(
    const QString &file,
    const QDir &inputRoot,
    const QDir &outputRoot,
    const CompressionOptions &options
) {
    PipelineOutcome outcome;
    const QFileInfo sourceInfo(file);
    outcome.fileName = sourceInfo.fileName();
    outcome.filePath = sourceInfo.absoluteFilePath();
    outcome.hasResult = true;

    const FormatInfo info = FormatDetector::detect(file);
    if (info.mismatch) {
        outcome.logs << QString("%1 实际格式为 %2，与扩展名 %3 不一致，将按实际格式输出并压缩")
                            .arg(sourceInfo.fileName())
                            .arg(info.actualSuffix)
                            .arg(info.sourceSuffix);
    }

    const QString targetFormat = resolveTargetFormat(info, options);
    const qint64 sourceSize = sourceInfo.size();
    outcome.result = {false, sourceSize, sourceSize, "无", "失败"};

    if (targetFormat == "gif" && info.effectiveSuffix != "gif") {
        outcome.logs << QString("%1 转换失败：不支持转换为GIF").arg(sourceInfo.fileName());
        outcome.hasResult = false;
        return outcome;
    }

    const QString relativePath = inputRoot.relativeFilePath(file);
    const QFileInfo relativeInfo(relativePath);
    const QString baseName = sourceInfo.completeBaseName();
    const QString relativeDir = relativeInfo.path();
    const QString outputFileName = targetFormat.isEmpty() ? baseName : baseName + "." + targetFormat;
    QString outputPath = relativeDir == "."
        ? outputRoot.filePath(outputFileName)
        : outputRoot.filePath(relativeDir + "/" + outputFileName);
    outputPath = ensureUniquePath(outputPath, sourceInfo.absoluteFilePath(), baseName, targetFormat);
    QDir().mkpath(QFileInfo(outputPath).absolutePath());

    const bool formatChanged = targetFormat != info.effectiveSuffix;
    const bool needConvert = options.resizeEnabled || formatChanged || info.mismatch;

    CompressionOptions compressOpts = options;
    compressOpts.outputFormat = "original";
    compressOpts.resizeEnabled = false;

    if (!needConvert) {
        outcome.result = EngineRegistry::compressFile(file, outputPath, compressOpts);
        if (outcome.result.success && outcome.result.outputSize > outcome.result.originalSize) {
            QFile::remove(outputPath);
            QFile::copy(file, outputPath);
            outcome.result.outputSize = QFileInfo(outputPath).size();
            outcome.result.engine = "原图";
            outcome.result.message = "已保留原图";
        }
        outcome.result.originalSize = sourceSize;
        return outcome;
    }

    QScopedPointer<QTemporaryFile> convertedTemp;
    QString convertedPath;
    if (WorkTemp::open(convertedTemp, targetFormat, outputRoot)) {
        convertedTemp->setAutoRemove(false);
        convertedPath = convertedTemp->fileName();
        convertedTemp->close();
        QFile::remove(convertedPath);
    } else {
        convertedPath = ensureUniquePath(
            outputRoot.filePath(QString(".%1_converted.%2").arg(baseName, targetFormat)),
            sourceInfo.absoluteFilePath(),
            baseName + "_converted",
            targetFormat
        );
    }

    const ConvertResult converted = ImageConverter::convert(
        file, convertedPath, info.effectiveSuffix, targetFormat, options
    );
    if (!converted.success) {
        outcome.logs << QString("%1 转换失败：%2").arg(sourceInfo.fileName(), converted.message);
        outcome.hasResult = false;
        QFile::remove(convertedPath);
        return outcome;
    }

    // mozjpeg / cwebp already encoded at target quality — publish and stop.
    if (isFinalLossyEncode(converted.engine)) {
        finalizeConvertedFile(convertedPath, outputPath, converted, sourceSize, outcome);
        applyLargerThanSourceMessage(outcome, info.effectiveSuffix, targetFormat);
        QFile::remove(convertedPath);
        return outcome;
    }

    // Intermediate convert (Qt/dwebp PNG etc.): compress when possible; never drop a good convert.
    const qint64 convertedSize = QFileInfo(convertedPath).size();
    outcome.result = EngineRegistry::compressFile(convertedPath, outputPath, compressOpts);
    if (!outcome.result.success) {
        if (convertedSize > 0 && QFileInfo::exists(convertedPath)) {
            if (!finalizeConvertedFile(convertedPath, outputPath, converted, sourceSize, outcome)) {
                QFile::remove(convertedPath);
                return outcome;
            }
            applyLargerThanSourceMessage(outcome, info.effectiveSuffix, targetFormat);
            QFile::remove(convertedPath);
            return outcome;
        }
        outcome.logs << QString("%1 压缩失败：%2").arg(sourceInfo.fileName(), outcome.result.message);
        outcome.hasResult = false;
        QFile::remove(outputPath);
        QFile::remove(convertedPath);
        return outcome;
    }

    outcome.result.originalSize = sourceSize;
    outcome.result.outputSize = QFileInfo(outputPath).size();
    if (outcome.result.outputSize > convertedSize) {
        QFile::remove(outputPath);
        if (!QFile::copy(convertedPath, outputPath)) {
            outcome.hasResult = false;
            outcome.logs << QString("%1 压缩失败：无法回写转换结果").arg(sourceInfo.fileName());
            QFile::remove(convertedPath);
            return outcome;
        }
        outcome.result.outputSize = QFileInfo(outputPath).size();
        outcome.result.engine = converted.engine;
        outcome.result.message = largerThanSourceMessage(info.effectiveSuffix, targetFormat);
    } else if (outcome.result.outputSize > sourceSize) {
        applyLargerThanSourceMessage(outcome, info.effectiveSuffix, targetFormat);
    }

    QFile::remove(convertedPath);
    return outcome;
}
