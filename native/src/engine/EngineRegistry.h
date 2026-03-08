#pragma once

#include <QtGlobal>
#include <QString>
#include <QStringList>

struct CompressionOptions {
    bool lossless;
    int quality;
    QString profile;
    QString outputFormat;
    int concurrency;
    bool resizeEnabled;
    int targetWidth;
    int targetHeight;
    int resizeMode;
};

struct CompressionResult {
    bool success;
    qint64 originalSize;
    qint64 outputSize;
    QString engine;
    QString message;
};

class EngineRegistry {
public:
    static QStringList availableEngines();
    static bool toolExists(const QString &name);
    static QString engineStatus(bool lossless);
    static CompressionResult compressFile(
        const QString &source,
        const QString &output,
        const CompressionOptions &options
    );
};
