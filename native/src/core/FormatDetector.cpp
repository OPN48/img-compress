#include "FormatDetector.h"

#include <QFileInfo>
#include <QImageReader>

QString FormatDetector::normalizeSuffix(const QString &suffix) {
    if (suffix == "jpeg") {
        return "jpg";
    }
    return suffix;
}

QByteArray FormatDetector::qtPluginFormat(const QString &suffix) {
    if (suffix == "jpg") {
        return QByteArrayLiteral("jpeg");
    }
    return suffix.toLatin1();
}

FormatInfo FormatDetector::detect(const QString &path) {
    FormatInfo info;
    info.sourceSuffix = normalizeSuffix(QFileInfo(path).suffix().toLower());
    const QByteArray detected = QImageReader::imageFormat(path);
    info.actualSuffix = normalizeSuffix(QString::fromLatin1(detected).toLower());
    info.mismatch = !info.actualSuffix.isEmpty() && info.actualSuffix != info.sourceSuffix;
    info.effectiveSuffix = info.actualSuffix.isEmpty() ? info.sourceSuffix : info.actualSuffix;
    return info;
}
