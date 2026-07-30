#pragma once

#include <QImage>
#include <QString>

#include "engine/EngineRegistry.h"

struct MozJpegEncodeResult {
    bool success = false;
    QString message;
    int qualityUsed = 0;
};

// Mature JPEG encode: RGB → binary PPM → cjpeg/mozjpeg (never feed JPEG to cjpeg).
class MozJpegEncoder {
public:
    static int effectiveQuality(const CompressionOptions &options);

    static MozJpegEncodeResult encodeImage(
        QImage image,
        const QString &destPath,
        int quality
    );

    static MozJpegEncodeResult encodeFile(
        const QString &sourcePath,
        const QString &destPath,
        const QString &sourceFormat,
        const CompressionOptions &options
    );
};
