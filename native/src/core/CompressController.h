#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include "engine/EngineRegistry.h"
#include "core/CompressWorker.h"

class CompressController final : public QObject {
    Q_OBJECT

public:
    explicit CompressController(QObject *parent = nullptr);

    void start(
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
    );
    void startFiles(
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
    );

signals:
    void logMessage(const QString &message);
    void progressChanged(int percent);
    void finished();

private:
    bool running;
    QThread *thread;
    CompressWorker *worker;
};
