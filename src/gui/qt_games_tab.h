#pragma once

#include <functional>

#include <QString>
#include <QWidget>

class QTableWidget;

class GamesTab : public QWidget {
public:
    explicit GamesTab(std::function<void(const QString&)> launchGame, QWidget* parent = nullptr);
    void recordGameStarted(const QString& path);
    void addPlayedTime(const QString& path, qint64 seconds);

private:
    void addGame(const QString& path, const QString& name = {}, const QString& lastPlayed = {}, qint64 timePlayedSeconds = 0, bool save = true);
    void loadGames();
    void saveGames() const;

    std::function<void(const QString&)> launchGame_;
    QTableWidget* gameTable_;
};