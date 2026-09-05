#pragma once

#include <QTextCharFormat>
#include <QWidget>

class QPlainTextEdit;

class GameLogTab : public QWidget {
public:
    explicit GameLogTab(QWidget* parent = nullptr);

    void appendText(const QString& text);
    void clearLog();

private:
    QPlainTextEdit* output_;
    QTextCharFormat textFormat_;
};
