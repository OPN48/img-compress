#pragma once

#include <QMainWindow>
#include <QFrame>
#include <QSet>
#include <QStringList>

class QLineEdit;
class QPushButton;
class QCheckBox;
class QComboBox;
class QSlider;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QIntValidator;
class QSpinBox;

class CompressController;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;

class DropArea final : public QFrame {
    Q_OBJECT

public:
    explicit DropArea(QWidget *parent = nullptr);

signals:
    void dropped(const QStringList &paths);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void pickInputDir();
    void pickOutputDir();
    void pickFiles();
    void clearSelectedFiles();
    void startCompression();
    void onLogMessage(const QString &message);
    void onProgressChanged(int percent);
    void onFinished();
    void onDropPaths(const QStringList &paths);

private:
    void setupUi();
    void updateSelectionMode();
    void updateCompressionOptionsState();
    void updateInputFormatsFromSelection();
    void updateResizeModeOptions();
    void updateOutputFormatOptions();
    void setResizeModeEnabled(int index, bool enabled);
    void setOutputFormatEnabled(const QString &format, bool enabled);
    bool isResizeModeEnabled(int index) const;
    bool isOutputFormatEnabled(int index) const;
    void updateLogSearchHighlights();
    bool readResizeSize(int &width, int &height);
    void setSelectedFiles(const QStringList &files);
    void updateFileSummary();
    QStringList collectFilesFromPaths(const QStringList &paths) const;
    QStringList collectUnsupportedFilesFromPaths(const QStringList &paths) const;
    void logUnsupportedFiles(const QStringList &files);
    QSet<QString> collectInputFormatsFromFiles(const QStringList &files) const;
    QSet<QString> collectInputFormatsFromDir(const QString &dir) const;
    QStringList buildFormatsForWorker() const;
    QString commonBaseDir(const QStringList &files) const;
    QString selectedOutputFormat() const;
    QString openDirectoryDialog(const QString &title, const QString &initialDir);
    QStringList openFilesDialog(const QString &title);
    bool startDirCompression(
        const QString &inputDir,
        const QString &outputDir,
        const QStringList &formats
    );
    bool startFilesCompression(
        const QStringList &files,
        const QString &baseDir,
        const QString &outputDir,
        const QStringList &formats
    );

    QLineEdit *inputLine;
    QLineEdit *outputLine;
    QLineEdit *filesLine;
    QCheckBox *losslessCheck;
    QComboBox *profileCombo;
    QComboBox *outputFormatCombo;
    QComboBox *resizeModeCombo;
    QLineEdit *widthInput;
    QLineEdit *heightInput;
    QIntValidator *sizeValidator;
    QLabel *sizeLabel;
    QSlider *qualitySlider;
    QLabel *qualityValue;
    QComboBox *engineLevelCombo;
    QPushButton *startButton;
    QPushButton *filesButton;
    QProgressBar *progressBar;
    QPlainTextEdit *logArea;
    QLineEdit *logSearchInput;
    CompressController *controller;
    DropArea *dropArea;
    QStringList selectedFiles;
    QSet<QString> inputFormats;
    bool isRunning;
};
