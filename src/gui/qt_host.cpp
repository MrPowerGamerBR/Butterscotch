#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
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
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "qt_games_tab.h"
#include "qt_instances_tab.h"
#include "qt_log_tab.h"
#include "qt_variables_tab.h"

extern "C" {
    int guiMainImpl(int argc, char* argv[]);
}

static QProcess* g_gameProcess = nullptr;
static QString g_lastGamePath;
static QString g_processOutputBuffer;
static bool g_variableSnapshotRequestPending = false;
static bool g_instanceSnapshotRequestPending = false;
static bool g_variablesTabOpen = false;
static bool g_instancesTabOpen = false;
static QTimer* g_variableSnapshotTimeoutTimer = nullptr;
static GamesTab* g_gamesTab = nullptr;
static QDateTime g_gameStartedAt;

static constexpr int kLiveVariableRefreshMode = 0;
static constexpr int kEverySecondVariableRefreshMode = 1;

static QString resolveGameExecutablePath();
static QStringList makeHostChildLaunchArgs(const QString& gamePath) {
    return QStringList{ gamePath, QStringLiteral("--host-child") };
}

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

static void requestInstanceSnapshot() {
    if (!g_instancesTabOpen || g_gameProcess == nullptr || g_gameProcess->state() != QProcess::Running || g_instanceSnapshotRequestPending) {
        return;
    }

    if (g_gameProcess->write("BS_REQUEST_INSTANCES\n") > 0) {
        g_instanceSnapshotRequestPending = true;
        if (g_variableSnapshotTimeoutTimer != nullptr) {
            g_variableSnapshotTimeoutTimer->start();
        }
    }
}

static void sendPauseCommand(bool paused) {
    if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning) {
        return;
    }

    const QByteArray payload = QByteArray("BS_PAUSE ") + (paused ? "1\n" : "0\n");
    g_gameProcess->write(payload);
}

static void resetGameProcess() {
    if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning || g_lastGamePath.isEmpty()) {
        return;
    }

    g_gameProcess->terminate();
    if (!g_gameProcess->waitForFinished(1000)) {
        g_gameProcess->kill();
        g_gameProcess->waitForFinished(1000);
    }

    if (g_gamesTab != nullptr && g_gameStartedAt.isValid()) {
        g_gamesTab->addPlayedTime(g_lastGamePath, g_gameStartedAt.secsTo(QDateTime::currentDateTime()));
        g_gameStartedAt = {};
    }

    const QString executablePath = resolveGameExecutablePath();
    g_gameProcess->start(executablePath, makeHostChildLaunchArgs(g_lastGamePath));
}

static void stopGameProcess() {
    if (g_gameProcess == nullptr) {
        return;
    }

    if (g_gamesTab != nullptr && g_gameStartedAt.isValid()) {
        g_gamesTab->addPlayedTime(g_lastGamePath, g_gameStartedAt.secsTo(QDateTime::currentDateTime()));
        g_gameStartedAt = {};
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
                       "Game files (*.win *.unx, *.ios);;All files (*)");
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

static void launchGameFromPathProcess(const QString& path, VariablesTab* variablesTab, GameLogTab* logTab, QTabWidget* tabs, InstancesTab* instancesTab = nullptr) {
    if (path.isEmpty()) {
        return;
    }

    g_lastGamePath = path;
    tabs->setCurrentWidget(logTab);

    stopGameProcess();

    g_gameProcess = new QProcess(QCoreApplication::instance());
    g_processOutputBuffer.clear();
    g_variableSnapshotRequestPending = false;
    g_instanceSnapshotRequestPending = false;
    logTab->clearLog();
    logTab->setPaused(false);
    logTab->pauseButton()->setEnabled(true);
    logTab->resetButton()->setEnabled(true);
    logTab->quitButton()->setEnabled(true);
    if (variablesTab != nullptr) {
        variablesTab->setSnapshot(QString());
        variablesTab->setProcessRunning(false);
    }
    if (instancesTab != nullptr) {
        instancesTab->setSnapshot(QString());
        instancesTab->setProcessRunning(false);
    }
    g_gameProcess->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(g_gameProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [variablesTab, instancesTab, logTab, path](int exitCode, QProcess::ExitStatus status) {
                         Q_UNUSED(status);
                         if (g_gamesTab != nullptr && g_gameStartedAt.isValid()) {
                             g_gamesTab->addPlayedTime(path, g_gameStartedAt.secsTo(QDateTime::currentDateTime()));
                             g_gameStartedAt = {};
                         }
                         logTab->setPaused(false);
                         logTab->pauseButton()->setEnabled(false);
                         logTab->resetButton()->setEnabled(false);
                         logTab->quitButton()->setEnabled(false);
                         if (variablesTab != nullptr) {
                             variablesTab->setProcessRunning(false);
                         }
                         if (instancesTab != nullptr) {
                             instancesTab->setProcessRunning(false);
                         }
                         g_variableSnapshotRequestPending = false;
                         g_instanceSnapshotRequestPending = false;
                         if (g_variableSnapshotTimeoutTimer != nullptr) {
                             g_variableSnapshotTimeoutTimer->stop();
                         }
                         if (exitCode != 0) {
                             qWarning() << "Game process exited with code" << exitCode;
                         }
                     });
    QObject::connect(g_gameProcess, &QProcess::readyReadStandardOutput, [variablesTab, instancesTab, logTab]() {
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
            if (line.startsWith(QStringLiteral("BS_VARS_JSON "))) {
                const QString payload = line.mid(QStringLiteral("BS_VARS_JSON ").size());
                if (variablesTab != nullptr) {
                    variablesTab->setSnapshot(payload);
                }
                g_variableSnapshotRequestPending = false;
                if (g_variableSnapshotTimeoutTimer != nullptr) {
                    g_variableSnapshotTimeoutTimer->stop();
                }
                continue;
            }
            if (line.startsWith(QStringLiteral("BS_INSTANCES_JSON "))) {
                const QString payload = line.mid(QStringLiteral("BS_INSTANCES_JSON ").size());
                if (instancesTab != nullptr) {
                    instancesTab->setSnapshot(payload);
                }
                g_instanceSnapshotRequestPending = false;
                if (g_variableSnapshotTimeoutTimer != nullptr) {
                    g_variableSnapshotTimeoutTimer->stop();
                }
                continue;
            }
            logTab->appendText(line + QStringLiteral("\n"));
        }
    });
    QObject::connect(g_gameProcess, &QProcess::started, [variablesTab, instancesTab, logTab, path]() {
        g_gameStartedAt = QDateTime::currentDateTime();
        g_gamesTab->recordGameStarted(path);
        logTab->setPaused(false);
        logTab->pauseButton()->setEnabled(true);
        logTab->resetButton()->setEnabled(true);
        logTab->quitButton()->setEnabled(true);
        if (variablesTab != nullptr) {
            variablesTab->setProcessRunning(true);
        }
        if (instancesTab != nullptr) {
            instancesTab->setProcessRunning(true);
        }
        QTimer::singleShot(150, [variablesTab]() {
            if (variablesTab == nullptr) {
                return;
            }
            if (variablesTab->refreshModeSelector()->currentIndex() != kLiveVariableRefreshMode) {
                return;
            }
            requestVariableSnapshot();
        });
    });

    const QString executablePath = resolveGameExecutablePath();
    g_gameProcess->start(executablePath, makeHostChildLaunchArgs(path));
}

class VariableTableScrollFilter : public QObject {
public:
    VariableTableScrollFilter(QTimer* refreshTimer,
                             QTabWidget* tabs,
                             QWidget* variablesTab,
                             QWidget* instancesTab,
                             QComboBox* variableRefreshModeSelector,
                             QComboBox* instanceRefreshModeSelector,
                             QObject* instanceTreeWidget,
                             QObject* instanceTreeViewport)
        : refreshTimer_(refreshTimer),
          tabs_(tabs),
          variablesTab_(variablesTab),
          instancesTab_(instancesTab),
          variableRefreshModeSelector_(variableRefreshModeSelector),
          instanceRefreshModeSelector_(instanceRefreshModeSelector),
          instanceTreeWidget_(instanceTreeWidget),
          instanceTreeViewport_(instanceTreeViewport) {}

    void resumeRefreshTimerForCurrentTab() {
        if (refreshTimer_ == nullptr || tabs_ == nullptr) {
            return;
        }

        QComboBox* activeModeSelector = nullptr;
        if (tabs_->currentWidget() == variablesTab_) {
            activeModeSelector = variableRefreshModeSelector_;
        } else if (tabs_->currentWidget() == instancesTab_) {
            activeModeSelector = instanceRefreshModeSelector_;
        }

        if (activeModeSelector == nullptr) {
            refreshTimer_->stop();
            return;
        }

        if (activeModeSelector->currentIndex() == kEverySecondVariableRefreshMode) {
            refreshTimer_->start(1000);
        } else if (activeModeSelector->currentIndex() == kLiveVariableRefreshMode) {
            refreshTimer_->start(100);
        } else {
            refreshTimer_->stop();
        }
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Wheel ||
            event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseMove ||
            event->type() == QEvent::KeyPress) {
            if (refreshTimer_ != nullptr) {
                refreshTimer_->stop();
            }
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::KeyRelease) {
            if (watched == instanceTreeWidget_ || watched == instanceTreeViewport_) {
                QTimer::singleShot(150, this, [this]() {
                    resumeRefreshTimerForCurrentTab();
                });
                return QObject::eventFilter(watched, event);
            }

            resumeRefreshTimerForCurrentTab();
            return QObject::eventFilter(watched, event);
        }

        return QObject::eventFilter(watched, event);
    }

private:
    QTimer* refreshTimer_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QWidget* variablesTab_ = nullptr;
    QWidget* instancesTab_ = nullptr;
    QComboBox* variableRefreshModeSelector_ = nullptr;
    QComboBox* instanceRefreshModeSelector_ = nullptr;
    QObject* instanceTreeWidget_ = nullptr;
    QObject* instanceTreeViewport_ = nullptr;
};

int main(int argc, char* argv[]) {
    bool isChildGameProcess = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--host-child")) {
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
    auto* instancesTab = new InstancesTab(&hostWindow);
    auto* gameLog = new GameLogTab(&hostWindow);
    auto* tabs = new QTabWidget(&hostWindow);
    auto* gamesTab = new GamesTab([variablesTab, instancesTab, gameLog, tabs](const QString& path) {
        launchGameFromPathProcess(path, variablesTab, gameLog, tabs, instancesTab);
    }, &hostWindow);
    g_gamesTab = gamesTab;
    tabs->addTab(gamesTab, "Games");
    tabs->addTab(gameLog, "Log");
    tabs->addTab(variablesTab, "Variables");
    tabs->addTab(instancesTab, "Instances");
    tabs->setCurrentWidget(gamesTab);
    QTableView* variableTable = variablesTab->tableView();
    QComboBox* refreshModeSelector = variablesTab->refreshModeSelector();
    QComboBox* instanceRefreshModeSelector = instancesTab->refreshModeSelector();

    QTimer variableSnapshotTimer(&hostWindow);
    variableSnapshotTimer.setInterval(100);

    QTimer variableSnapshotTimeoutTimer(&hostWindow);
    variableSnapshotTimeoutTimer.setSingleShot(true);
    variableSnapshotTimeoutTimer.setInterval(1000);
    g_variableSnapshotTimeoutTimer = &variableSnapshotTimeoutTimer;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        stopGameProcess();
    });

    auto* instanceTreeWidget = instancesTab->tableView();
    auto* tableInteractionFilter = new VariableTableScrollFilter(&variableSnapshotTimer,
                                                                tabs,
                                                                variablesTab,
                                                                instancesTab,
                                                                refreshModeSelector,
                                                                instanceRefreshModeSelector,
                                                                instanceTreeWidget,
                                                                instanceTreeWidget->viewport());
    variableTable->viewport()->installEventFilter(tableInteractionFilter);
    variableTable->installEventFilter(tableInteractionFilter);
    instanceTreeWidget->viewport()->installEventFilter(tableInteractionFilter);
    instanceTreeWidget->installEventFilter(tableInteractionFilter);

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
    QObject::connect(refreshButton, &QPushButton::clicked, [variablesTab, instancesTab, gameLog, tabs]() {
        if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning) {
            if (!g_lastGamePath.isEmpty()) {
                launchGameFromPathProcess(g_lastGamePath, variablesTab, gameLog, tabs, instancesTab);
            }
        }
        requestVariableSnapshot();
        variablesTab->refresh();
    });
    QPushButton* instancesRefreshButton = instancesTab->refreshButton();
    QObject::connect(instancesRefreshButton, &QPushButton::clicked, [variablesTab, instancesTab, gameLog, tabs]() {
        if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning) {
            if (!g_lastGamePath.isEmpty()) {
                launchGameFromPathProcess(g_lastGamePath, variablesTab, gameLog, tabs, instancesTab);
            }
        }
        requestInstanceSnapshot();
        if (instancesTab != nullptr) {
            instancesTab->refresh();
        }
    });
    QObject::connect(gameLog->pauseButton(), &QPushButton::clicked, [gameLog]() {
        if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning) {
            return;
        }

        const bool paused = !gameLog->isPaused();
        sendPauseCommand(paused);
        gameLog->setPaused(paused);
    });
    QObject::connect(gameLog->resetButton(), &QPushButton::clicked, [variablesTab, instancesTab, gameLog, tabs]() {
        if (g_lastGamePath.isEmpty()) {
            return;
        }

        launchGameFromPathProcess(g_lastGamePath, variablesTab, gameLog, tabs, instancesTab);
    });
    QObject::connect(gameLog->quitButton(), &QPushButton::clicked, []() {
        stopGameProcess();
    });
    QObject::connect(&variableSnapshotTimer, &QTimer::timeout, []() {
        requestVariableSnapshot();
        requestInstanceSnapshot();
    });
    QObject::connect(&variableSnapshotTimeoutTimer, &QTimer::timeout, []() {
        g_variableSnapshotRequestPending = false;
        g_instanceSnapshotRequestPending = false;
    });
    auto updateSnapshotTimerFromActiveTab = [&]() {
        if (!g_variablesTabOpen && !g_instancesTabOpen) {
            variableSnapshotTimer.stop();
            return;
        }

        QComboBox* activeModeSelector = nullptr;
        if (tabs->currentWidget() == variablesTab) {
            activeModeSelector = refreshModeSelector;
        } else if (tabs->currentWidget() == instancesTab) {
            activeModeSelector = instanceRefreshModeSelector;
        }

        if (activeModeSelector == nullptr) {
            variableSnapshotTimer.stop();
            return;
        }

        if (activeModeSelector->currentIndex() == kEverySecondVariableRefreshMode) {
            variableSnapshotTimer.start(1000);
        } else if (activeModeSelector->currentIndex() == kLiveVariableRefreshMode) {
            variableSnapshotTimer.start(100);
        } else {
            variableSnapshotTimer.stop();
        }
    };
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged,
                     [tabs, variablesTab, instancesTab, &updateSnapshotTimerFromActiveTab](Qt::ApplicationState) {
                         if (!g_variablesTabOpen && !g_instancesTabOpen) {
                             return;
                         }
                         updateSnapshotTimerFromActiveTab();
                         if (tabs->currentWidget() == variablesTab) {
                             requestVariableSnapshot();
                         }
                         if (tabs->currentWidget() == instancesTab) {
                             requestInstanceSnapshot();
                         }
                     });

    QObject::connect(refreshModeSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [updateSnapshotTimerFromActiveTab](int) {
                         updateSnapshotTimerFromActiveTab();
                     });
    QObject::connect(instanceRefreshModeSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [updateSnapshotTimerFromActiveTab](int) {
                         updateSnapshotTimerFromActiveTab();
                     });
    QObject::connect(tabs, &QTabWidget::currentChanged, [tabs, variablesTab, instancesTab, &variableSnapshotTimer, &updateSnapshotTimerFromActiveTab](int) {
        g_variablesTabOpen = tabs->currentWidget() == variablesTab;
        g_instancesTabOpen = tabs->currentWidget() == instancesTab;
        if (!g_variablesTabOpen && !g_instancesTabOpen) {
            variableSnapshotTimer.stop();
            g_variableSnapshotRequestPending = false;
            g_instanceSnapshotRequestPending = false;
            if (g_variableSnapshotTimeoutTimer != nullptr) {
                g_variableSnapshotTimeoutTimer->stop();
            }
            return;
        }

        updateSnapshotTimerFromActiveTab();
        if (g_variablesTabOpen) {
            requestVariableSnapshot();
        }
        if (g_instancesTabOpen) {
            requestInstanceSnapshot();
        }
    });

    layout->addWidget(menuBar);
    layout->addWidget(tabs);

    QObject::connect(openAction, &QAction::triggered, [&hostWindow, variablesTab, instancesTab, gameLog, tabs]() {
        QString selectedPath = chooseGameFile(&hostWindow);
        if (selectedPath.isEmpty()) {
            return;
        }

        g_lastGamePath = selectedPath;
        launchGameFromPathProcess(selectedPath, variablesTab, gameLog, tabs, instancesTab);
    });

    hostWindow.show();

    QTimer::singleShot(0, [argc, argv, variablesTab, instancesTab, gameLog, tabs]() {
        if (argc <= 1) {
            return;
        }

        QString launchPath = QString::fromLocal8Bit(argv[1]);
        g_lastGamePath = launchPath;
        launchGameFromPathProcess(launchPath, variablesTab, gameLog, tabs, instancesTab);
    });

    return app.exec();
}
