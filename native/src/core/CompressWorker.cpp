#include "CompressWorker.h"

#include "CompressPipeline.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QVector>
#include <QWaitCondition>
#include <QQueue>
#include <algorithm>

namespace {
struct TaskOutcome {
    QString fileName;
    QString filePath;
    CompressionResult result;
    bool hasResult;
    QStringList logs;
    qint64 elapsedMs;
};

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
        const PipelineOutcome pipeline = CompressPipeline::run(filePath, input, output, opts);
        TaskOutcome finished;
        finished.fileName = pipeline.fileName;
        finished.filePath = pipeline.filePath;
        finished.result = pipeline.result;
        finished.hasResult = pipeline.hasResult;
        finished.logs = pipeline.logs;
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
