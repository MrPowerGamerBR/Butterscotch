#pragma once

#include <QPushButton>
#include <QTextCharFormat>
#include <QWidget>

class QPlainTextEdit;

class GameLogTab : public QWidget {
public:
    explicit GameLogTab(QWidget* parent = nullptr);

    void appendText(const QString& text);
    void clearLog();
    void setPaused(bool paused);
    bool isPaused() const;
    QPushButton* pauseButton() const;
    QPushButton* resetButton() const;
    QPushButton* quitButton() const;

private:
    QPlainTextEdit* output_;
    QPushButton* pauseButton_;
    QPushButton* resetButton_;
    QPushButton* quitButton_;
    QTextCharFormat textFormat_;
    bool paused_ = false;
};
