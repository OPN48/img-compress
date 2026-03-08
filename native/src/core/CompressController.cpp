#include "CompressController.h"

#include <QDir>
#include <QFileInfo>

CompressController::CompressController(QObject *parent)
    : QObject(parent), running(false), thread(nullptr), worker(nullptr) {}

void CompressController::start(
    const QString &inputDir,
    const QString &outputDir,
    const QStringList &formats,
    bool lossless,
    int quality,
    const QString &profile,
    const QString &outputFormat,
    int concurrency,
    bool resizeEnabled,
    int targetWidth,
    int targetHeight,
    int resizeMode
) {
    if (running) {
        emit logMessage("已有任务进行中");
        return;
    }
    const QString inputText = inputDir.trimmed();
    const QString outputText = outputDir.trimmed();
    if (inputText.isEmpty() || !QDir(inputText).exists()) {
        emit logMessage("请输入有效的输入目录");
        return;
    }
    if (outputText.isEmpty()) {
        emit logMessage("请输入有效的输出目录");
        return;
    }
    if (formats.isEmpty()) {
        emit logMessage("请选择至少一种格式");
        return;
    }
    QDir outputRoot(outputText);
    if (!outputRoot.exists()) {
        if (!outputRoot.mkpath(".")) {
            emit logMessage("无法创建输出目录");
            return;
        }
    }
    CompressionOptions options{lossless, quality, profile, outputFormat, concurrency, resizeEnabled, targetWidth, targetHeight, resizeMode};
    thread = new QThread(this);
    worker = new CompressWorker();
    worker->configure(inputText, outputText, formats, options);
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &CompressWorker::run);
    connect(worker, &CompressWorker::logMessage, this, &CompressController::logMessage);
    connect(worker, &CompressWorker::progressChanged, this, &CompressController::progressChanged);
    connect(worker, &CompressWorker::finished, this, [this](int, qint64, qint64, qint64) {
        running = false;
        emit finished();
        if (thread) {
            thread->quit();
        }
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        thread = nullptr;
        worker = nullptr;
    });
    running = true;
    thread->start();
}

void CompressController::startFiles(
    const QStringList &files,
    const QString &baseDir,
    const QString &outputDir,
    const QStringList &formats,
    bool lossless,
    int quality,
    const QString &profile,
    const QString &outputFormat,
    int concurrency,
    bool resizeEnabled,
    int targetWidth,
    int targetHeight,
    int resizeMode
) {
    if (running) {
        emit logMessage("已有任务进行中");
        return;
    }
    QStringList validFiles;
    for (const QString &file : files) {
        QFileInfo info(file);
        if (info.exists() && info.isFile()) {
            validFiles.append(info.absoluteFilePath());
        }
    }
    if (validFiles.isEmpty()) {
        emit logMessage("未找到可压缩图片");
        return;
    }
    const QString baseText = baseDir.trimmed();
    const QString outputText = outputDir.trimmed();
    if (baseText.isEmpty() || !QDir(baseText).exists()) {
        emit logMessage("请输入有效的输入目录");
        return;
    }
    if (outputText.isEmpty()) {
        emit logMessage("请输入有效的输出目录");
        return;
    }
    if (formats.isEmpty()) {
        emit logMessage("请选择至少一种格式");
        return;
    }
    QDir outputRoot(outputText);
    if (!outputRoot.exists()) {
        if (!outputRoot.mkpath(".")) {
            emit logMessage("无法创建输出目录");
            return;
        }
    }
    CompressionOptions options{lossless, quality, profile, outputFormat, concurrency, resizeEnabled, targetWidth, targetHeight, resizeMode};
    thread = new QThread(this);
    worker = new CompressWorker();
    worker->configureFiles(validFiles, baseText, outputText, formats, options);
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &CompressWorker::run);
    connect(worker, &CompressWorker::logMessage, this, &CompressController::logMessage);
    connect(worker, &CompressWorker::progressChanged, this, &CompressController::progressChanged);
    connect(worker, &CompressWorker::finished, this, [this](int, qint64, qint64, qint64) {
        running = false;
        emit finished();
        if (thread) {
            thread->quit();
        }
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        thread = nullptr;
        worker = nullptr;
    });
    running = true;
    thread->start();
}
