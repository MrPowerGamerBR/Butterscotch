#pragma once

#include <QDateTime>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>

#include "qt_games_tab.h"
#include "qt_instances_tab.h"
#include "qt_log_tab.h"
#include "qt_variables_tab.h"

namespace qt_game_process {

extern QProcess* g_gameProcess;
extern QString g_lastGamePath;
extern QString g_processOutputBuffer;
extern bool g_variableSnapshotRequestPending;
extern bool g_instanceSnapshotRequestPending;
extern bool g_variablesTabOpen;
extern bool g_instancesTabOpen;
extern QTimer* g_variableSnapshotTimeoutTimer;
extern GamesTab* g_gamesTab;
extern QDateTime g_gameStartedAt;

inline constexpr int kLiveVariableRefreshMode = 0;
inline constexpr int kEverySecondVariableRefreshMode = 1;

QString resolveGameExecutablePath();
QStringList makeHostChildLaunchArgs(const QString& gamePath);
void requestVariableSnapshot();
void requestInstanceSnapshot();
void sendPauseCommand(bool paused);
void resetGameProcess();
void stopGameProcess();
void launchGameFromPathProcess(const QString& path,
                              VariablesTab* variablesTab,
                              GameLogTab* logTab,
                              QTabWidget* tabs,
                              InstancesTab* instancesTab = nullptr);

} // namespace qt_game_process
