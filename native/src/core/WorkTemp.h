#pragma once

#include <QDir>
#include <QScopedPointer>
#include <QString>
#include <QTemporaryFile>

class WorkTemp {
public:
    // Prefer system temp (more reliable on Windows), then output dir.
    static bool open(QScopedPointer<QTemporaryFile> &temp, const QString &suffix, const QDir &preferredDir);
};
