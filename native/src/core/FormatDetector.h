#pragma once

#include <QString>

struct FormatInfo {
    QString sourceSuffix;
    QString actualSuffix;
    QString effectiveSuffix;
    bool mismatch = false;
};

class FormatDetector {
public:
    static QString normalizeSuffix(const QString &suffix);
    // Qt image plugins register "jpeg", not "jpg".
    static QByteArray qtPluginFormat(const QString &suffix);
    static FormatInfo detect(const QString &path);
};
