#pragma once

#include <functional>

#include <QString>
#include <QWidget>

class QTableWidget;

class GamesTab : public QWidget {
public:
    explicit GamesTab(std::function<void(const QString&)> launchGame, QWidget* parent = nullptr);

private:
    void addGame(const QString& path, bool save = true);
    void loadGames();
    void saveGames() const;

    std::function<void(const QString&)> launchGame_;
    QTableWidget* gameTable_;
};