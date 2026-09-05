#pragma once

#include <functional>

#include <QString>
#include <QWidget>

class QListWidget;

class GamesTab : public QWidget {
public:
    explicit GamesTab(std::function<void(const QString&)> launchGame, QWidget* parent = nullptr);

private:
    void addGame(const QString& path);

    std::function<void(const QString&)> launchGame_;
    QListWidget* gameList_;
};