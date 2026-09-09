#include "qt_game_process.h"

#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QTabWidget>
#include <QTimer>

namespace qt_game_process {

QProcess* g_gameProcess = nullptr;
QString g_lastGamePath;
QString g_processOutputBuffer;
bool g_variableSnapshotRequestPending = false;
bool g_instanceSnapshotRequestPending = false;
bool g_variablesTabOpen = false;
bool g_instancesTabOpen = false;
QTimer* g_variableSnapshotTimeoutTimer = nullptr;
GamesTab* g_gamesTab = nullptr;
QDateTime g_gameStartedAt;

QString resolveGameExecutablePath() {
    const QString localBinary = QCoreApplication::applicationDirPath() + QStringLiteral("/butterscotch");
    if (QFileInfo(localBinary).exists() && QFileInfo(localBinary).isFile()) {
        return localBinary;
    }
    return QStringLiteral("butterscotch");
}

QStringList makeHostChildLaunchArgs(const QString& gamePath) {
    return QStringList{ gamePath, QStringLiteral("--host-child") };
}

void requestVariableSnapshot() {
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

void requestInstanceSnapshot() {
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

void sendPauseCommand(bool paused) {
    if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning) {
        return;
    }

    const QByteArray payload = QByteArray("BS_PAUSE ") + (paused ? "1\n" : "0\n");
    g_gameProcess->write(payload);
}

void resetGameProcess() {
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

void stopGameProcess() {
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

void launchGameFromPathProcess(const QString& path,
                              VariablesTab* variablesTab,
                              GameLogTab* logTab,
                              QTabWidget* tabs,
                              InstancesTab* instancesTab) {
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

} // namespace qt_game_process
