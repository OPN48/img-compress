#include "CompressWorker.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTemporaryFile>
#include <QScopedPointer>
#include <QVector>
#include <QWaitCondition>
#include <QQueue>
#include <algorithm>

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

int adjustQuality(int quality, const QString &profile) {
    if (profile.contains("强")) {
        return qMax(8, quality - 18);
    }
    if (profile.contains("均衡")) {
        return qMax(10, quality - 10);
    }
    return quality;
}

QString normalizeSuffix(const QString &suffix) {
    if (suffix == "jpeg") {
        return "jpg";
    }
    return suffix;
}

struct TaskOutcome {
    QString fileName;
    QString filePath;
    CompressionResult result;
    bool hasResult;
    QStringList logs;
    qint64 elapsedMs;
};

TaskOutcome compressSingle(
    const QString &file,
    const QDir &inputRoot,
    const QDir &outputRoot,
    const CompressionOptions &options
) {
    TaskOutcome outcome;
    const QFileInfo sourceInfo(file);
    outcome.fileName = sourceInfo.fileName();
    outcome.filePath = sourceInfo.absoluteFilePath();
    const QString relativePath = inputRoot.relativeFilePath(file);
    const QFileInfo relativeInfo(relativePath);
    const QString sourceSuffix = normalizeSuffix(sourceInfo.suffix().toLower());
    const QByteArray detectedFormat = QImageReader::imageFormat(file);
    const QString actualSuffix = normalizeSuffix(QString::fromLatin1(detectedFormat).toLower());
    const bool formatMismatch = !actualSuffix.isEmpty() && actualSuffix != sourceSuffix;
    const QString effectiveSuffix = actualSuffix.isEmpty() ? sourceSuffix : actualSuffix;
    if (formatMismatch) {
        outcome.logs << QString("%1 实际格式为 %2，与扩展名 %3 不一致，将按实际格式输出并压缩")
                            .arg(sourceInfo.fileName())
                            .arg(actualSuffix)
                            .arg(sourceSuffix);
    }
    const QString rawOutputFormat = options.outputFormat.toLower();
    const QString normalizedOutputFormat = normalizeSuffix(rawOutputFormat);
    const QString targetFormat = rawOutputFormat.isEmpty() || rawOutputFormat == "original"
        ? effectiveSuffix
        : normalizedOutputFormat;
    const QString baseName = sourceInfo.completeBaseName();
    const QString relativeDir = relativeInfo.path();
    const QString outputFileName = targetFormat.isEmpty() ? baseName : baseName + "." + targetFormat;
    QString outputPath = relativeDir == "."
        ? outputRoot.filePath(outputFileName)
        : outputRoot.filePath(relativeDir + "/" + outputFileName);
    outputPath = ensureUniquePath(outputPath, sourceInfo.absoluteFilePath(), baseName, targetFormat);
    QFileInfo outputInfo(outputPath);
    QDir outputDirInfo = outputInfo.dir();
    if (!outputDirInfo.exists()) {
        outputDirInfo.mkpath(".");
    }
    const qint64 sourceSize = sourceInfo.size();
    outcome.result = {false, sourceSize, sourceSize, "无", "失败"};
    outcome.hasResult = true;
    const bool convertToWebp = targetFormat == "webp" && effectiveSuffix != "webp";
    const bool convertToGif = targetFormat == "gif" && effectiveSuffix != "gif";
    const bool convertFromWebp = effectiveSuffix == "webp"
        && (targetFormat == "jpg" || targetFormat == "png");
    if (convertToGif) {
        outcome.logs << QString("%1 转换失败：不支持转换为GIF").arg(sourceInfo.fileName());
        outcome.hasResult = false;
        return outcome;
    }
    if (options.resizeEnabled && (effectiveSuffix == "webp" || targetFormat == "webp")) {
        outcome.logs << QString("%1 转换失败：启用尺寸裁剪/缩放时不支持 WebP（需要 Qt WebP 插件）").arg(sourceInfo.fileName());
        outcome.hasResult = false;
        return outcome;
    }
    if ((convertToWebp || convertFromWebp) && !options.resizeEnabled) {
        outcome.result = EngineRegistry::compressFile(file, outputPath, options);
        if (!outcome.result.success) {
            QImageReader reader(file);
            reader.setAutoTransform(true);
            if (!actualSuffix.isEmpty()) {
                reader.setFormat(actualSuffix.toLatin1());
            }
            QImage image = reader.read();
            if (!image.isNull()) {
                QString tempFormat = !actualSuffix.isEmpty() ? actualSuffix : "png";
                QScopedPointer<QTemporaryFile> temp(new QTemporaryFile(outputRoot.filePath(".imgcompress_tmp_XXXXXX." + tempFormat)));
                temp->setAutoRemove(true);
                if (!temp->open()) {
                    temp.reset(new QTemporaryFile(QDir(QDir::tempPath()).filePath("imgcompress_tmp_XXXXXX." + tempFormat)));
                    temp->setAutoRemove(true);
                    if (!temp->open()) {
                        outcome.logs << QString("%1 转换失败：无法创建临时文件").arg(sourceInfo.fileName());
                        outcome.hasResult = false;
                        return outcome;
                    }
                }
                {
                    const QString tempPath = temp->fileName();
                    temp->close();
                    QImageWriter writer(tempPath, tempFormat.toLatin1());
                    const int quality = options.lossless
                        ? 100
                        : qBound(1, adjustQuality(options.quality, options.profile), 100);
                    writer.setQuality(quality);
                    if (writer.write(image)) {
                        outcome.result = EngineRegistry::compressFile(tempPath, outputPath, options);
                        if (!outcome.result.success) {
                            QFile::remove(outputPath);
                            QFile::copy(tempPath, outputPath);
                            outcome.result = {true, sourceSize, QFileInfo(outputPath).size(), "Qt", "已转换"};
                        } else {
                            outcome.result.originalSize = sourceSize;
                            outcome.result.outputSize = QFileInfo(outputPath).size();
                        }
                    } else {
                        outcome.logs << QString("%1 转换失败：无法写入格式").arg(sourceInfo.fileName());
                        outcome.hasResult = false;
                        return outcome;
                    }
                }
            }
        }
    } else if (options.resizeEnabled || targetFormat != effectiveSuffix || formatMismatch) {
        if (!options.resizeEnabled && formatMismatch && targetFormat == effectiveSuffix) {
            QScopedPointer<QTemporaryFile> temp(new QTemporaryFile(outputRoot.filePath(".imgcompress_tmp_XXXXXX." + effectiveSuffix)));
            temp->setAutoRemove(true);
            if (!temp->open()) {
                temp.reset(new QTemporaryFile(QDir(QDir::tempPath()).filePath("imgcompress_tmp_XXXXXX." + effectiveSuffix)));
                temp->setAutoRemove(true);
                if (!temp->open()) {
                    QImageReader reader(file);
                    reader.setAutoTransform(true);
                    if (!actualSuffix.isEmpty()) {
                        reader.setFormat(actualSuffix.toLatin1());
                    }
                    QImage image = reader.read();
                    if (image.isNull()) {
                        outcome.logs << QString("%1 转换失败：无法读取图片").arg(sourceInfo.fileName());
                        outcome.hasResult = false;
                        return outcome;
                    }
                    QImageWriter writer(outputPath, effectiveSuffix.toLatin1());
                    const int quality = options.lossless
                        ? 100
                        : qBound(1, adjustQuality(options.quality, options.profile), 100);
                    writer.setQuality(quality);
                    if (!writer.write(image)) {
                        outcome.logs << QString("%1 转换失败：无法写入格式").arg(sourceInfo.fileName());
                        outcome.hasResult = false;
                        return outcome;
                    }
                    outcome.result = {true, sourceSize, QFileInfo(outputPath).size(), "Qt", "已按实际格式输出"};
                    return outcome;
                }
            }
            const QString tempPath = temp->fileName();
            temp->close();
            QFile::remove(tempPath);
            if (!QFile::copy(file, tempPath)) {
                outcome.logs << QString("%1 转换失败：无法创建临时文件").arg(sourceInfo.fileName());
                outcome.hasResult = false;
                return outcome;
            }
            outcome.result = EngineRegistry::compressFile(tempPath, outputPath, options);
            if (!outcome.result.success) {
                QFile::remove(outputPath);
                QFile::copy(tempPath, outputPath);
                outcome.result = {true, sourceSize, QFileInfo(outputPath).size(), "原图", "已按实际格式输出"};
            } else {
                outcome.result.originalSize = sourceSize;
                outcome.result.outputSize = QFileInfo(outputPath).size();
            }
        } else {
            QImageReader reader(file);
            reader.setAutoTransform(true);
            if (effectiveSuffix == "webp") {
                reader.setFormat("webp");
            }
            QImage image = reader.read();
            if (image.isNull()) {
                if (effectiveSuffix == "webp") {
                    outcome.logs << QString("%1 转换失败：WebP 解码不可用（缺少 dwebp 或 Qt WebP 插件）").arg(sourceInfo.fileName());
                } else {
                    outcome.logs << QString("%1 转换失败：无法读取图片").arg(sourceInfo.fileName());
                }
                outcome.hasResult = false;
                return outcome;
            }
            if (options.resizeEnabled) {
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
                    image = image.copy(QRect(offsetX, offsetY, cropWidth, cropHeight));
                } else if (options.resizeMode == 1) {
                    image = image.scaled(
                        options.targetWidth,
                        options.targetHeight,
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation
                    );
                }
            }
            QString tempFormat = targetFormat;
            bool allowTempFallback = true;
            if (convertToWebp || convertToGif) {
                tempFormat = effectiveSuffix.isEmpty() ? "png" : effectiveSuffix;
                allowTempFallback = false;
            }
            QScopedPointer<QTemporaryFile> temp(new QTemporaryFile(outputRoot.filePath(".imgcompress_tmp_XXXXXX." + tempFormat)));
            temp->setAutoRemove(true);
            if (!temp->open()) {
                temp.reset(new QTemporaryFile(QDir(QDir::tempPath()).filePath("imgcompress_tmp_XXXXXX." + tempFormat)));
                temp->setAutoRemove(true);
                if (!temp->open()) {
                    outcome.logs << QString("%1 转换失败：无法创建临时文件").arg(sourceInfo.fileName());
                    outcome.hasResult = false;
                    return outcome;
                }
            }
            const QString tempPath = temp->fileName();
            temp->close();
            QImageWriter writer(tempPath, tempFormat.toLatin1());
            const int quality = options.lossless
                ? 100
                : qBound(1, adjustQuality(options.quality, options.profile), 100);
            writer.setQuality(quality);
            if (!writer.write(image)) {
                outcome.logs << QString("%1 转换失败：无法写入格式").arg(sourceInfo.fileName());
                outcome.hasResult = false;
                return outcome;
            }
            outcome.result = EngineRegistry::compressFile(tempPath, outputPath, options);
            if (!outcome.result.success && allowTempFallback) {
                QFile::remove(outputPath);
                QFile::copy(tempPath, outputPath);
                outcome.result = {true, sourceSize, QFileInfo(outputPath).size(), "Qt", "已转换"};
            } else {
                outcome.result.originalSize = sourceSize;
                outcome.result.outputSize = QFileInfo(outputPath).size();
            }
        }
    } else {
        outcome.result = EngineRegistry::compressFile(file, outputPath, options);
        if (!outcome.result.success && effectiveSuffix == "jpg") {
            QImageReader reader(file);
            reader.setAutoTransform(true);
            QImage image = reader.read();
            if (!image.isNull()) {
                const QString tempFormat = "jpg";
                QScopedPointer<QTemporaryFile> temp(new QTemporaryFile(outputRoot.filePath(".imgcompress_tmp_XXXXXX." + tempFormat)));
                temp->setAutoRemove(true);
                if (!temp->open()) {
                    temp.reset(new QTemporaryFile(QDir(QDir::tempPath()).filePath("imgcompress_tmp_XXXXXX." + tempFormat)));
                    temp->setAutoRemove(true);
                    if (!temp->open()) {
                        outcome.logs << QString("%1 转换失败：无法创建临时文件").arg(sourceInfo.fileName());
                        outcome.hasResult = false;
                        return outcome;
                    }
                }
                const QString tempPath = temp->fileName();
                temp->close();
                QImageWriter writer(tempPath, tempFormat.toLatin1());
                const int quality = options.lossless
                    ? 100
                    : qBound(1, adjustQuality(options.quality, options.profile), 100);
                writer.setQuality(quality);
                if (writer.write(image)) {
                    QFile::remove(outputPath);
                    QFile::copy(tempPath, outputPath);
                    outcome.result = {true, sourceSize, QFileInfo(outputPath).size(), "Qt", "已压缩"};
                } else {
                    outcome.logs << QString("%1 转换失败：无法写入格式").arg(sourceInfo.fileName());
                    outcome.hasResult = false;
                    return outcome;
                }
            }
        }
    }
    if (outcome.result.success && outcome.result.outputSize > outcome.result.originalSize) {
        QFile::remove(outputPath);
        QFile::copy(sourceInfo.absoluteFilePath(), outputPath);
        outcome.result.outputSize = QFileInfo(outputPath).size();
        outcome.result.engine = "原图";
        outcome.result.message = "已保留原图";
    }
    return outcome;
}

class CompressTask final : public QRunnable {
public:
    CompressTask(
        const QString &file,
        const QDir &inputRoot,
        const QDir &outputRoot,
        const CompressionOptions &options,
        QQueue<TaskOutcome> *queue,
        QMutex *mutex,
        QWaitCondition *condition
    )
        : filePath(file),
          input(inputRoot),
          output(outputRoot),
          opts(options),
          resultQueue(queue),
          queueMutex(mutex),
          queueCondition(condition) {
        setAutoDelete(true);
    }

    void run() override {
        const QDateTime started = QDateTime::currentDateTime();
        const TaskOutcome outcome = compressSingle(filePath, input, output, opts);
        TaskOutcome finished = outcome;
        finished.elapsedMs = started.msecsTo(QDateTime::currentDateTime());
        QMutexLocker locker(queueMutex);
        resultQueue->enqueue(finished);
        queueCondition->wakeOne();
    }

private:
    QString filePath;
    QDir input;
    QDir output;
    CompressionOptions opts;
    QQueue<TaskOutcome> *resultQueue;
    QMutex *queueMutex;
    QWaitCondition *queueCondition;
};
}

CompressWorker::CompressWorker(QObject *parent) : QObject(parent), useFileList(false) {}

void CompressWorker::configure(
    const QString &inputDirValue,
    const QString &outputDirValue,
    const QStringList &formatsValue,
    const CompressionOptions &optionsValue
) {
    inputDir = inputDirValue;
    outputDir = outputDirValue;
    formats = formatsValue;
    options = optionsValue;
    files.clear();
    useFileList = false;
}

void CompressWorker::configureFiles(
    const QStringList &filesValue,
    const QString &baseDir,
    const QString &outputDirValue,
    const QStringList &formatsValue,
    const CompressionOptions &optionsValue
) {
    inputDir = baseDir;
    outputDir = outputDirValue;
    formats = formatsValue;
    options = optionsValue;
    files = filesValue;
    useFileList = true;
}

void CompressWorker::run() {
    QStringList filters;
    for (const QString &fmt : formats) {
        filters << QString("*.%1").arg(fmt.toLower());
    }
    QStringList workingFiles;
    QSet<QString> formatSet;
    for (const QString &fmt : formats) {
        formatSet.insert(fmt.toLower());
    }
    if (useFileList) {
        for (const QString &file : files) {
            const QString suffix = QFileInfo(file).suffix().toLower();
            if (formatSet.contains(suffix)) {
                workingFiles.append(file);
            }
        }
    } else {
        QDirIterator it(inputDir, filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            workingFiles.append(it.next());
        }
    }
    if (workingFiles.isEmpty()) {
        emit logMessage("未找到可压缩图片");
        emit finished(0, 0, 0, 0);
        return;
    }
    emit logMessage(QString("开始压缩 %1 张图片").arg(workingFiles.size()));
    const QDateTime started = QDateTime::currentDateTime();
    int successCount = 0;
    qint64 totalBefore = 0;
    qint64 totalAfter = 0;
    QDir inputRoot(inputDir);
    QDir outputRoot(outputDir);
    int completed = 0;
    QThreadPool pool;
    int concurrency = options.concurrency;
    if (concurrency < 1) {
        const int ideal = QThread::idealThreadCount();
        concurrency = ideal > 1 ? ideal - 1 : 1;
    }
    pool.setMaxThreadCount(concurrency);
    QQueue<TaskOutcome> outcomes;
    QMutex queueMutex;
    QWaitCondition queueCondition;
    QHash<QString, QDateTime> activeTasks;
    QDateTime lastHeartbeat = QDateTime::currentDateTime();
    for (const QString &file : workingFiles) {
        pool.start(new CompressTask(file, inputRoot, outputRoot, options, &outcomes, &queueMutex, &queueCondition));
        activeTasks.insert(file, QDateTime::currentDateTime());
    }
    const int total = workingFiles.size();
    while (completed < total) {
        queueMutex.lock();
        if (outcomes.isEmpty()) {
            queueCondition.wait(&queueMutex, 2000);
        }
        QQueue<TaskOutcome> batch;
        while (!outcomes.isEmpty()) {
            batch.enqueue(outcomes.dequeue());
        }
        queueMutex.unlock();
        if (batch.isEmpty()) {
            const QDateTime now = QDateTime::currentDateTime();
            if (lastHeartbeat.msecsTo(now) >= 10000 && !activeTasks.isEmpty()) {
                QVector<QPair<qint64, QString>> longest;
                longest.reserve(activeTasks.size());
                for (auto it = activeTasks.constBegin(); it != activeTasks.constEnd(); ++it) {
                    const qint64 elapsed = it.value().msecsTo(now);
                    longest.append(qMakePair(elapsed, QFileInfo(it.key()).fileName()));
                }
                std::sort(longest.begin(), longest.end(), [](const auto &a, const auto &b) {
                    return a.first > b.first;
                });
                const int limit = qMin(3, longest.size());
                QStringList items;
                for (int i = 0; i < limit; i += 1) {
                    items << QString("%1(%2s)").arg(longest[i].second).arg(longest[i].first / 1000.0, 0, 'f', 1);
                }
                emit logMessage(QString("处理中 %1 张，最长已运行：%2").arg(activeTasks.size()).arg(items.join("，")));
                lastHeartbeat = now;
            }
        }
        while (!batch.isEmpty()) {
            const TaskOutcome outcome = batch.dequeue();
            if (!outcome.filePath.isEmpty()) {
                activeTasks.remove(outcome.filePath);
            }
            for (const QString &line : outcome.logs) {
                emit logMessage(line);
            }
            if (outcome.hasResult) {
                if (outcome.result.success) {
                    successCount += 1;
                    totalBefore += outcome.result.originalSize;
                    totalAfter += outcome.result.outputSize;
                    const double ratio = outcome.result.originalSize > 0
                        ? 1.0 - (static_cast<double>(outcome.result.outputSize) / outcome.result.originalSize)
                        : 0.0;
                    emit logMessage(
                        QString("%1 压缩完成，节省 %2，引擎 %3，耗时 %4s")
                            .arg(outcome.fileName)
                            .arg(QString::number(ratio * 100.0, 'f', 1) + "%")
                            .arg(outcome.result.engine)
                            .arg(outcome.elapsedMs / 1000.0, 0, 'f', 1)
                    );
                } else {
                    emit logMessage(
                        QString("%1 压缩失败：%2，耗时 %3s")
                            .arg(outcome.fileName)
                            .arg(outcome.result.message)
                            .arg(outcome.elapsedMs / 1000.0, 0, 'f', 1)
                    );
                }
            }
            completed += 1;
            const int percent = static_cast<int>((static_cast<double>(completed) / total) * 100.0);
            emit progressChanged(percent);
        }
    }
    pool.waitForDone();
    emit progressChanged(100);
    const qint64 saved = totalBefore - totalAfter;
    const double totalRatio = totalBefore > 0
        ? static_cast<double>(saved) / totalBefore
        : 0.0;
    const qint64 elapsedMs = started.msecsTo(QDateTime::currentDateTime());
    emit logMessage(
        QString("完成：成功 %1 张，节省 %2，用时 %3 秒")
            .arg(successCount)
            .arg(QString::number(totalRatio * 100.0, 'f', 1) + "%")
            .arg(QString::number(elapsedMs / 1000.0, 'f', 1))
    );
    emit finished(successCount, totalBefore, totalAfter, elapsedMs);
}
