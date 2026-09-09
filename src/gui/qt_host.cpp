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

#include "qt_game_file_dialog.h"
#include "qt_game_process.h"
#include "qt_games_tab.h"
#include "qt_instances_tab.h"
#include "qt_log_tab.h"
#include "qt_variables_tab.h"

extern "C" {
    int guiMainImpl(int argc, char* argv[]);
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

        if (activeModeSelector->currentIndex() == qt_game_process::kEverySecondVariableRefreshMode) {
            refreshTimer_->start(1000);
        } else if (activeModeSelector->currentIndex() == qt_game_process::kLiveVariableRefreshMode) {
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
        qt_game_process::launchGameFromPathProcess(path, variablesTab, gameLog, tabs, instancesTab);
    }, &hostWindow);
    qt_game_process::g_gamesTab = gamesTab;
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
    qt_game_process::g_variableSnapshotTimeoutTimer = &variableSnapshotTimeoutTimer;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        qt_game_process::stopGameProcess();
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
        if (!qt_game_process::g_variablesTabOpen) {
            return;
        }
        if (refreshModeSelector->currentIndex() == qt_game_process::kLiveVariableRefreshMode) {
            variableSnapshotTimer.start(100);
        } else if (refreshModeSelector->currentIndex() == qt_game_process::kEverySecondVariableRefreshMode) {
            variableSnapshotTimer.start(1000);
        }
    });

    QPushButton* refreshButton = variablesTab->refreshButton();
    QObject::connect(refreshButton, &QPushButton::clicked, [variablesTab, instancesTab, gameLog, tabs]() {
        if (qt_game_process::g_gameProcess == nullptr || qt_game_process::g_gameProcess->state() == QProcess::NotRunning) {
            if (!qt_game_process::g_lastGamePath.isEmpty()) {
                qt_game_process::launchGameFromPathProcess(qt_game_process::g_lastGamePath, variablesTab, gameLog, tabs, instancesTab);
            }
        }
        qt_game_process::requestVariableSnapshot();
        variablesTab->refresh();
    });
    QPushButton* instancesRefreshButton = instancesTab->refreshButton();
    QObject::connect(instancesRefreshButton, &QPushButton::clicked, [variablesTab, instancesTab, gameLog, tabs]() {
        if (qt_game_process::g_gameProcess == nullptr || qt_game_process::g_gameProcess->state() == QProcess::NotRunning) {
            if (!qt_game_process::g_lastGamePath.isEmpty()) {
                qt_game_process::launchGameFromPathProcess(qt_game_process::g_lastGamePath, variablesTab, gameLog, tabs, instancesTab);
            }
        }
        qt_game_process::requestInstanceSnapshot();
        if (instancesTab != nullptr) {
            instancesTab->refresh();
        }
    });
    QObject::connect(gameLog->pauseButton(), &QPushButton::clicked, [gameLog]() {
        if (qt_game_process::g_gameProcess == nullptr || qt_game_process::g_gameProcess->state() == QProcess::NotRunning) {
            return;
        }

        const bool paused = !gameLog->isPaused();
        qt_game_process::sendPauseCommand(paused);
        gameLog->setPaused(paused);
    });
    QObject::connect(gameLog->resetButton(), &QPushButton::clicked, [variablesTab, instancesTab, gameLog, tabs]() {
        qt_game_process::resetGameProcess();
    });
    QObject::connect(gameLog->quitButton(), &QPushButton::clicked, []() {
        qt_game_process::stopGameProcess();
    });
    QObject::connect(&variableSnapshotTimer, &QTimer::timeout, []() {
        qt_game_process::requestVariableSnapshot();
        qt_game_process::requestInstanceSnapshot();
    });
    QObject::connect(&variableSnapshotTimeoutTimer, &QTimer::timeout, []() {
        qt_game_process::g_variableSnapshotRequestPending = false;
        qt_game_process::g_instanceSnapshotRequestPending = false;
    });
    auto updateSnapshotTimerFromActiveTab = [&]() {
        if (!qt_game_process::g_variablesTabOpen && !qt_game_process::g_instancesTabOpen) {
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

        if (activeModeSelector->currentIndex() == qt_game_process::kEverySecondVariableRefreshMode) {
            variableSnapshotTimer.start(1000);
        } else if (activeModeSelector->currentIndex() == qt_game_process::kLiveVariableRefreshMode) {
            variableSnapshotTimer.start(100);
        } else {
            variableSnapshotTimer.stop();
        }
    };
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged,
                     [tabs, variablesTab, instancesTab, &updateSnapshotTimerFromActiveTab](Qt::ApplicationState) {
                         if (!qt_game_process::g_variablesTabOpen && !qt_game_process::g_instancesTabOpen) {
                             return;
                         }
                         updateSnapshotTimerFromActiveTab();
                         if (tabs->currentWidget() == variablesTab) {
                             qt_game_process::requestVariableSnapshot();
                         }
                         if (tabs->currentWidget() == instancesTab) {
                             qt_game_process::requestInstanceSnapshot();
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
        qt_game_process::g_variablesTabOpen = tabs->currentWidget() == variablesTab;
        qt_game_process::g_instancesTabOpen = tabs->currentWidget() == instancesTab;
        if (!qt_game_process::g_variablesTabOpen && !qt_game_process::g_instancesTabOpen) {
            variableSnapshotTimer.stop();
            qt_game_process::g_variableSnapshotRequestPending = false;
            qt_game_process::g_instanceSnapshotRequestPending = false;
            if (qt_game_process::g_variableSnapshotTimeoutTimer != nullptr) {
                qt_game_process::g_variableSnapshotTimeoutTimer->stop();
            }
            return;
        }

        updateSnapshotTimerFromActiveTab();
        if (qt_game_process::g_variablesTabOpen) {
            qt_game_process::requestVariableSnapshot();
        }
        if (qt_game_process::g_instancesTabOpen) {
            qt_game_process::requestInstanceSnapshot();
        }
    });

    layout->addWidget(menuBar);
    layout->addWidget(tabs);

    QObject::connect(openAction, &QAction::triggered, [&hostWindow, variablesTab, instancesTab, gameLog, tabs]() {
        QString selectedPath = chooseGameFile(&hostWindow);
        if (selectedPath.isEmpty()) {
            return;
        }

        qt_game_process::g_lastGamePath = selectedPath;
        qt_game_process::launchGameFromPathProcess(selectedPath, variablesTab, gameLog, tabs, instancesTab);
    });

    hostWindow.show();

    QTimer::singleShot(0, [argc, argv, variablesTab, instancesTab, gameLog, tabs]() {
        if (argc <= 1) {
            return;
        }

        QString launchPath = QString::fromLocal8Bit(argv[1]);
        qt_game_process::g_lastGamePath = launchPath;
        qt_game_process::launchGameFromPathProcess(launchPath, variablesTab, gameLog, tabs, instancesTab);
    });

    return app.exec();
}
