#include "qt_log_tab.h"

#include <QColor>
#include <QFont>
#include <QPlainTextEdit>
#include <QRegularExpression>
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

    static const QRegularExpression ansiSgr(QStringLiteral("\\x1b\\[([0-9;]*)m"));

    QTextCursor cursor = output_->textCursor();
    cursor.movePosition(QTextCursor::End);
    qsizetype position = 0;
    QRegularExpressionMatchIterator matches = ansiSgr.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        cursor.insertText(text.mid(position, match.capturedStart() - position), textFormat_);
        position = match.capturedEnd();

        const QString parameters = match.captured(1);
        if (parameters.isEmpty()) {
            textFormat_ = QTextCharFormat();
            continue;
        }

        for (const QString& parameter : parameters.split(';')) {
            bool isNumber = false;
            const int code = parameter.toInt(&isNumber);
            if (!isNumber) {
                continue;
            }

            switch (code) {
                case 0:
                    textFormat_ = QTextCharFormat();
                    break;
                case 1:
                    textFormat_.setFontWeight(QFont::Bold);
                    break;
                case 22:
                    textFormat_.setFontWeight(QFont::Normal);
                    break;
                case 31:
                    textFormat_.setForeground(QColor(Qt::red));
                    break;
                case 33:
                    textFormat_.setForeground(QColor(Qt::darkYellow));
                    break;
                case 35:
                    textFormat_.setForeground(QColor(Qt::magenta));
                    break;
                case 39:
                    textFormat_.clearForeground();
                    break;
            }
        }
    }
    cursor.insertText(text.mid(position), textFormat_);
    output_->setTextCursor(cursor);
}

void GameLogTab::clearLog() {
    output_->clear();
}
