#include "qt_log_tab.h"

#include <QPlainTextEdit>
#include <QTextCursor>
#include <QVBoxLayout>

GameLogTab::GameLogTab(QWidget* parent) : QWidget(parent), output_(new QPlainTextEdit(this)) {
    output_->setReadOnly(true);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setPlaceholderText("Game log output will appear here...");

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(output_);
}

void GameLogTab::appendText(const QString& text) {
    if (text.isEmpty()) {
        return;
    }

    output_->moveCursor(QTextCursor::End);
    output_->insertPlainText(text);
    output_->moveCursor(QTextCursor::End);
}

void GameLogTab::clearLog() {
    output_->clear();
}
