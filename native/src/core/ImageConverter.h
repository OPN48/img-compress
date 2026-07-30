#pragma once

#include <QString>

#include "engine/EngineRegistry.h"

struct ConvertResult {
    bool success = false;
    QString message;
    QString engine;
};

class ImageConverter {
public:
    // Decode (+ optional resize/crop) and write target format to destPath.
    // JPG targets are encoded once via mozjpeg (PPM→cjpeg); no Qt JPG fallback.
    static ConvertResult convert(
        const QString &sourcePath,
        const QString &destPath,
        const QString &sourceFormat,
        const QString &targetFormat,
        const CompressionOptions &options
    );
};
