#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "qt_games_tab.h"
#include "qt_log_tab.h"
#include "qt_variables_tab.h"

extern "C" {
    int guiMainImpl(int argc, char* argv[]);
}

static QProcess* g_gameProcess = nullptr;
static QString g_lastGamePath;
static QString g_processOutputBuffer;
static bool g_variableSnapshotRequestPending = false;
static bool g_variablesTabOpen = false;
static QTimer* g_variableSnapshotTimeoutTimer = nullptr;

static constexpr int kLiveVariableRefreshMode = 0;
static constexpr int kEverySecondVariableRefreshMode = 1;

static void requestVariableSnapshot() {
    if (!g_variablesTabOpen || g_gameProcess == nullptr || g_gameProcess->state() != QProcess::Running || g_variableSnapshotRequestPending) {
        return;
    }

    if (g_gameProcess->write("BS_REQUEST_VARS\n") > 0) {
        g_variableSnapshotRequestPending = true;
        if (g_variableSnapshotTimeoutTimer != nullptr) {
            g_variableSnapshotTimeoutTimer->start();
        }
    }
}

static void stopGameProcess() {
    if (g_gameProcess == nullptr) {
        return;
    }

    if (g_gameProcess->state() != QProcess::NotRunning) {
        g_gameProcess->terminate();
        if (!g_gameProcess->waitForFinished(1000)) {
            g_gameProcess->kill();
            g_gameProcess->waitForFinished(1000);
        }
    }
    delete g_gameProcess;
    g_gameProcess = nullptr;
    g_variableSnapshotRequestPending = false;
}

static QString resolveGameExecutablePath() {
    const QString localBinary = QCoreApplication::applicationDirPath() + QStringLiteral("/butterscotch");
    if (QFileInfo(localBinary).exists() && QFileInfo(localBinary).isFile()) {
        return localBinary;
    }
    return QStringLiteral("butterscotch");
}

static QString chooseGameFile(QWidget* parent) {
    QFileDialog dialog(parent,
                       "Open a data.win or game.unx file",
                       QDir::homePath(),
                       "Game files (*.win *.unx);;All files (*)");
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);

    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }

    const QStringList selectedFiles = dialog.selectedFiles();
    if (selectedFiles.isEmpty()) {
        return {};
    }

    return selectedFiles.constFirst();
}

static void launchGameFromPathProcess(const QString& path, VariablesTab* variablesTab, GameLogTab* logTab, QTabWidget* tabs) {
    if (path.isEmpty()) {
        return;
    }

    g_lastGamePath = path;
    tabs->setCurrentWidget(logTab);

    stopGameProcess();

    g_gameProcess = new QProcess(QCoreApplication::instance());
    g_processOutputBuffer.clear();
    g_variableSnapshotRequestPending = false;
    logTab->clearLog();
    variablesTab->setSnapshot(QString());
    variablesTab->setProcessRunning(false);
    g_gameProcess->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(g_gameProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [variablesTab](int exitCode, QProcess::ExitStatus status) {
                         Q_UNUSED(status);
                         variablesTab->setProcessRunning(false);
                         g_variableSnapshotRequestPending = false;
                         if (g_variableSnapshotTimeoutTimer != nullptr) {
                             g_variableSnapshotTimeoutTimer->stop();
                         }
                         if (exitCode != 0) {
                             qWarning() << "Game process exited with code" << exitCode;
                         }
                     });
    QObject::connect(g_gameProcess, &QProcess::readyReadStandardOutput, [variablesTab, logTab]() {
        QByteArray data = g_gameProcess->readAllStandardOutput();
        if (data.isEmpty()) {
            return;
        }

        g_processOutputBuffer += QString::fromUtf8(data);
        while (true) {
            const qsizetype newlineIndex = g_processOutputBuffer.indexOf('\n');
            if (newlineIndex < 0) {
                break;
            }

            QString line = g_processOutputBuffer.left(newlineIndex);
            g_processOutputBuffer.remove(0, newlineIndex + 1);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (!line.startsWith(QStringLiteral("BS_VARS_JSON "))) {
                logTab->appendText(line + QStringLiteral("\n"));
                continue;
            }
            const QString payload = line.mid(QStringLiteral("BS_VARS_JSON ").size());
            variablesTab->setSnapshot(payload);
            g_variableSnapshotRequestPending = false;
            if (g_variableSnapshotTimeoutTimer != nullptr) {
                g_variableSnapshotTimeoutTimer->stop();
            }
        }
    });
    QObject::connect(g_gameProcess, &QProcess::started, [variablesTab]() {
        variablesTab->setProcessRunning(true);
        QTimer::singleShot(150, [variablesTab]() {
            if (variablesTab->refreshModeSelector()->currentIndex() != kLiveVariableRefreshMode) {
                return;
            }
            requestVariableSnapshot();
        });
    });

    const QString executablePath = resolveGameExecutablePath();
    g_gameProcess->start(executablePath, QStringList{
        path,
        QStringLiteral("--host-child"),
        QStringLiteral("--host-vars-json-on-demand")
    });
}

class VariableTableScrollFilter : public QObject {
public:
    explicit VariableTableScrollFilter(QTimer* refreshTimer) : refreshTimer_(refreshTimer) {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);

        if (event->type() == QEvent::Wheel ||
            event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseMove ||
            event->type() == QEvent::KeyPress) {
            if (refreshTimer_ != nullptr) {
                refreshTimer_->stop();
            }
        }

        return QObject::eventFilter(watched, event);
    }

private:
    QTimer* refreshTimer_;
};

int main(int argc, char* argv[]) {
    bool isChildGameProcess = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--host-child") ||
            arg == QStringLiteral("--host-vars-json") ||
            arg.startsWith(QStringLiteral("--host-vars-json-interval"))) {
            isChildGameProcess = true;
            break;
        }
    }

    if (isChildGameProcess) {
        return guiMainImpl(argc, argv);
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Butterscotch");
    QCoreApplication::setApplicationName("Butterscotch");

    QWidget hostWindow;
    hostWindow.resize(960, 540);
    hostWindow.setWindowTitle("Butterscotch");

    QVBoxLayout* layout = new QVBoxLayout(&hostWindow);

    QMenuBar* menuBar = new QMenuBar(&hostWindow);
    QMenu* fileMenu = menuBar->addMenu("&File");
    QAction* openAction = fileMenu->addAction("&Open data.win...");

    auto* variablesTab = new VariablesTab(&hostWindow);
    auto* gameLog = new GameLogTab(&hostWindow);
    auto* tabs = new QTabWidget(&hostWindow);
    auto* gamesTab = new GamesTab([variablesTab, gameLog, tabs](const QString& path) {
        launchGameFromPathProcess(path, variablesTab, gameLog, tabs);
    }, &hostWindow);
    tabs->addTab(gamesTab, "Games");
    tabs->addTab(gameLog, "Log");
    tabs->addTab(variablesTab, "Variables");
    tabs->setCurrentWidget(gamesTab);
    QTableView* variableTable = variablesTab->tableView();
    QComboBox* refreshModeSelector = variablesTab->refreshModeSelector();

    QTimer variableSnapshotTimer(&hostWindow);
    variableSnapshotTimer.setInterval(100);

    QTimer variableSnapshotTimeoutTimer(&hostWindow);
    variableSnapshotTimeoutTimer.setSingleShot(true);
    variableSnapshotTimeoutTimer.setInterval(1000);
    g_variableSnapshotTimeoutTimer = &variableSnapshotTimeoutTimer;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        stopGameProcess();
    });

    VariableTableScrollFilter scrollFilter(&variableSnapshotTimer);
    variableTable->viewport()->installEventFilter(&scrollFilter);
    variableTable->installEventFilter(&scrollFilter);

    QObject::connect(variableTable->verticalScrollBar(), &QScrollBar::sliderPressed, [&variableSnapshotTimer]() {
        variableSnapshotTimer.stop();
    });
    QObject::connect(variableTable->verticalScrollBar(), &QScrollBar::sliderMoved, [&variableSnapshotTimer]() {
        variableSnapshotTimer.stop();
    });
    QObject::connect(variableTable->verticalScrollBar(), &QScrollBar::sliderReleased, [&variableSnapshotTimer, refreshModeSelector]() {
        if (!g_variablesTabOpen) {
            return;
        }
        if (refreshModeSelector->currentIndex() == kLiveVariableRefreshMode) {
            variableSnapshotTimer.start(100);
        } else if (refreshModeSelector->currentIndex() == kEverySecondVariableRefreshMode) {
            variableSnapshotTimer.start(1000);
        }
    });

    QPushButton* refreshButton = variablesTab->refreshButton();
    QObject::connect(refreshButton, &QPushButton::clicked, [variablesTab, gameLog, tabs]() {
        if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning) {
            if (!g_lastGamePath.isEmpty()) {
                launchGameFromPathProcess(g_lastGamePath, variablesTab, gameLog, tabs);
            }
        }
        requestVariableSnapshot();
        variablesTab->refresh();
    });
    QObject::connect(&variableSnapshotTimer, &QTimer::timeout, []() {
        requestVariableSnapshot();
    });
    QObject::connect(&variableSnapshotTimeoutTimer, &QTimer::timeout, []() {
        g_variableSnapshotRequestPending = false;
        qWarning() << "Timed out waiting for variable snapshot from game process";
    });
    QObject::connect(refreshModeSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [refreshModeSelector, &variableSnapshotTimer](int) {
                         if (!g_variablesTabOpen) {
                             variableSnapshotTimer.stop();
                             return;
                         }
                         if (refreshModeSelector->currentIndex() == kEverySecondVariableRefreshMode) {
                             variableSnapshotTimer.start(1000);
                         } else if (refreshModeSelector->currentIndex() == kLiveVariableRefreshMode) {
                             variableSnapshotTimer.start(100);
                         } else {
                             variableSnapshotTimer.stop();
                         }
                     });
    QObject::connect(tabs, &QTabWidget::currentChanged, [tabs, variablesTab, refreshModeSelector, &variableSnapshotTimer](int) {
        g_variablesTabOpen = tabs->currentWidget() == variablesTab;
        if (!g_variablesTabOpen) {
            variableSnapshotTimer.stop();
            g_variableSnapshotRequestPending = false;
            if (g_variableSnapshotTimeoutTimer != nullptr) {
                g_variableSnapshotTimeoutTimer->stop();
            }
            return;
        }

        if (refreshModeSelector->currentIndex() == kEverySecondVariableRefreshMode) {
            variableSnapshotTimer.start(1000);
        } else if (refreshModeSelector->currentIndex() == kLiveVariableRefreshMode) {
            variableSnapshotTimer.start(100);
        }
        requestVariableSnapshot();
    });

    layout->addWidget(menuBar);
    layout->addWidget(tabs);

    QObject::connect(openAction, &QAction::triggered, [&hostWindow, variablesTab, gameLog, tabs]() {
        QString selectedPath = chooseGameFile(&hostWindow);
        if (selectedPath.isEmpty()) {
            return;
        }

        g_lastGamePath = selectedPath;
        launchGameFromPathProcess(selectedPath, variablesTab, gameLog, tabs);
    });

    hostWindow.show();

    QTimer::singleShot(0, [argc, argv, variablesTab, gameLog, tabs]() {
        if (argc <= 1) {
            return;
        }

        QString launchPath = QString::fromLocal8Bit(argv[1]);
        g_lastGamePath = launchPath;
        launchGameFromPathProcess(launchPath, variablesTab, gameLog, tabs);
    });

    return app.exec();
}
