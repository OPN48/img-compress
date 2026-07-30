#include "WorkTemp.h"

bool WorkTemp::open(QScopedPointer<QTemporaryFile> &temp, const QString &suffix, const QDir &preferredDir) {
    const QString pattern = QString("imgcompress_tmp_XXXXXX.%1").arg(suffix);
    temp.reset(new QTemporaryFile(QDir(QDir::tempPath()).filePath(pattern)));
    temp->setAutoRemove(true);
    if (temp->open()) {
        return true;
    }
    temp.reset(new QTemporaryFile(preferredDir.filePath(pattern)));
    temp->setAutoRemove(true);
    if (temp->open()) {
        return true;
    }
    temp.reset();
    return false;
}
