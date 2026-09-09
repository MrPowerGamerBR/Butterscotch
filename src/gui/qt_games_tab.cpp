#include "qt_games_tab.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

#include "qt_game_file_dialog.h"

extern "C" {
#include "data_win.h"
}

namespace {

constexpr int kLastPlayedRole = Qt::UserRole + 1;
constexpr int kTimePlayedSecondsRole = Qt::UserRole + 2;

QString formatPlayedTime(qint64 seconds) {
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (hours == 0) {
        return QStringLiteral("%1 %2").arg(minutes).arg(minutes == 1 ? "minute" : "minutes");
    }
    return QStringLiteral("%1 %2 and %3 %4")
        .arg(hours)
        .arg(hours == 1 ? "hour" : "hours")
        .arg(minutes)
        .arg(minutes == 1 ? "minute" : "minutes");
}

QString formatLastPlayed(const QString& timestamp) {
    const QDateTime dateTime = QDateTime::fromString(timestamp, Qt::ISODate);
    return dateTime.isValid() ? QLocale::system().toString(dateTime.date(), QLocale::ShortFormat) : QString();
}

QString gameTitleFromDataWin(const QString& path) {
    const QByteArray encodedPath = QFile::encodeName(path);
    DataWinParserOptions options{};
    options.parseGen8 = true;
    options.parseStrg = true;
    options.suppressUnknownChunkLogs = true;

    DataWin* dataWin = DataWin_parse(encodedPath.constData(), options);
    if (dataWin == nullptr) {
        return {};
    }

    const char* title = dataWin->gen8.displayName;
    if (title == nullptr || title[0] == '\0') {
        title = dataWin->gen8.name;
    }
    const QString gameTitle = title == nullptr ? QString() : QString::fromUtf8(title);
    DataWin_free(dataWin);
    return gameTitle;
}

} // namespace

GamesTab::GamesTab(std::function<void(const QString&)> launchGame, QWidget* parent)
    : QWidget(parent), launchGame_(std::move(launchGame)), gameTable_(new QTableWidget(this)) {
    gameTable_->setColumnCount(4);
    gameTable_->setHorizontalHeaderLabels({"Game", "Path", "Last played", "Time played"});
    gameTable_->setAlternatingRowColors(true);
    gameTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gameTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    gameTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gameTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    gameTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    gameTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    gameTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    gameTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    gameTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    gameTable_->verticalHeader()->setVisible(false);

    auto* addGameButton = new QPushButton("Add game...", this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(gameTable_);
    layout->addWidget(addGameButton);

    connect(addGameButton, &QPushButton::clicked, this, [this]() {
        addGame(chooseGameFile(this));
    });
    connect(gameTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        launchGame_(gameTable_->item(row, 0)->data(Qt::UserRole).toString());
    });
    connect(gameTable_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTableWidgetItem* item = gameTable_->itemAt(position);
        if (item == nullptr) {
            return;
        }

        const int row = item->row();
        gameTable_->selectRow(row);
        QMenu menu(this);
        QAction* renameAction = menu.addAction("Rename...");
        QAction* removeAction = menu.addAction("Remove");
        QAction* selectedAction = menu.exec(gameTable_->viewport()->mapToGlobal(position));
        if (selectedAction == renameAction) {
            QTableWidgetItem* titleItem = gameTable_->item(row, 0);
            bool accepted = false;
            const QString name = QInputDialog::getText(this, "Rename game", "Game name:",
                                                       QLineEdit::Normal, titleItem->text(), &accepted);
            if (accepted && !name.trimmed().isEmpty()) {
                titleItem->setText(name);
                saveGames();
            }
        } else if (selectedAction == removeAction) {
            gameTable_->removeRow(row);
            saveGames();
        }
    });

    loadGames();
}

void GamesTab::addGame(const QString& path, const QString& name, const QString& lastPlayed, qint64 timePlayedSeconds, bool save) {
    if (path.isEmpty()) {
        return;
    }

    for (int row = 0; row < gameTable_->rowCount(); ++row) {
        if (gameTable_->item(row, 0)->data(Qt::UserRole).toString() == path) {
            gameTable_->selectRow(row);
            return;
        }
    }

    const QFileInfo gameFile(path);
    const QString gameTitle = name.isEmpty() ? gameTitleFromDataWin(path) : name;
    const int row = gameTable_->rowCount();
    gameTable_->insertRow(row);
    auto* titleItem = new QTableWidgetItem(gameTitle.isEmpty() ? gameFile.completeBaseName() : gameTitle);
    titleItem->setData(Qt::UserRole, path);
    titleItem->setData(kLastPlayedRole, lastPlayed);
    titleItem->setData(kTimePlayedSecondsRole, timePlayedSeconds);
    titleItem->setToolTip(path);
    gameTable_->setItem(row, 0, titleItem);
    auto* pathItem = new QTableWidgetItem(path);
    pathItem->setToolTip(path);
    gameTable_->setItem(row, 1, pathItem);
    gameTable_->setItem(row, 2, new QTableWidgetItem(formatLastPlayed(lastPlayed)));
    gameTable_->setItem(row, 3, new QTableWidgetItem(formatPlayedTime(timePlayedSeconds)));
    gameTable_->selectRow(row);

    if (save) {
        saveGames();
    }
}

void GamesTab::recordGameStarted(const QString& path) {
    for (int row = 0; row < gameTable_->rowCount(); ++row) {
        QTableWidgetItem* titleItem = gameTable_->item(row, 0);
        if (titleItem->data(Qt::UserRole).toString() != path) {
            continue;
        }

        const QString lastPlayed = QDateTime::currentDateTime().toString(Qt::ISODate);
        titleItem->setData(kLastPlayedRole, lastPlayed);
        gameTable_->item(row, 2)->setText(formatLastPlayed(lastPlayed));
        saveGames();
        return;
    }
}

void GamesTab::addPlayedTime(const QString& path, qint64 seconds) {
    if (seconds <= 0) {
        return;
    }

    for (int row = 0; row < gameTable_->rowCount(); ++row) {
        QTableWidgetItem* titleItem = gameTable_->item(row, 0);
        if (titleItem->data(Qt::UserRole).toString() != path) {
            continue;
        }

        const qint64 timePlayedSeconds = titleItem->data(kTimePlayedSecondsRole).toLongLong() + seconds;
        titleItem->setData(kTimePlayedSecondsRole, timePlayedSeconds);
        gameTable_->item(row, 3)->setText(formatPlayedTime(timePlayedSeconds));
        saveGames();
        return;
    }
}

void GamesTab::loadGames() {
    QSettings settings;
    bool needsSave = false;
    const int gameCount = settings.beginReadArray("games");
    for (int index = 0; index < gameCount; ++index) {
        settings.setArrayIndex(index);
        const QString name = settings.value("name").toString();
        needsSave |= name.isEmpty();
        addGame(settings.value("path").toString(), name, settings.value("lastPlayed").toString(),
            settings.value("timePlayedSeconds", 0).toLongLong(), false);
    }
    settings.endArray();
    if (needsSave) {
        saveGames();
    }
}

void GamesTab::saveGames() const {
    QSettings settings;
    settings.beginWriteArray("games", gameTable_->rowCount());
    for (int row = 0; row < gameTable_->rowCount(); ++row) {
        settings.setArrayIndex(row);
        settings.setValue("name", gameTable_->item(row, 0)->text());
        settings.setValue("path", gameTable_->item(row, 0)->data(Qt::UserRole).toString());
        settings.setValue("lastPlayed", gameTable_->item(row, 0)->data(kLastPlayedRole).toString());
        settings.setValue("timePlayedSeconds", gameTable_->item(row, 0)->data(kTimePlayedSecondsRole));
    }
    settings.endArray();
}