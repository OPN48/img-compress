#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

#include "engine/EngineRegistry.h"

struct PipelineOutcome {
    QString fileName;
    QString filePath;
    CompressionResult result;
    bool hasResult = false;
    QStringList logs;
};

class CompressPipeline {
public:
    static PipelineOutcome run(
        const QString &file,
        const QDir &inputRoot,
        const QDir &outputRoot,
        const CompressionOptions &options
    );
};
