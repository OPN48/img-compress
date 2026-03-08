#include <QApplication>
#include <QString>

#include "app/MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    const QString appName = QString::fromUtf8(APP_DISPLAY_NAME);
    app.setApplicationDisplayName(appName);
    app.setApplicationName(appName);
    MainWindow window;
    window.show();
    return app.exec();
}
