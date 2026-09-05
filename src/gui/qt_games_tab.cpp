#include "qt_games_tab.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

extern "C" {
#include "data_win.h"
}

namespace {

QString chooseGameFile(QWidget* parent) {
    QFileDialog dialog(parent,
                       "Open a data.win or game.unx file",
                       QDir::homePath(),
                       "Game files (*.win *.unx);;All files (*)");
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);

    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }

    const QStringList selectedFiles = dialog.selectedFiles();
    return selectedFiles.isEmpty() ? QString() : selectedFiles.constFirst();
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
    gameTable_->setColumnCount(2);
    gameTable_->setHorizontalHeaderLabels({"Game", "Path"});
    gameTable_->setAlternatingRowColors(true);
    gameTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gameTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    gameTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gameTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    gameTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    gameTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
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

    loadGames();
}

void GamesTab::addGame(const QString& path, bool save) {
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
    const QString gameTitle = gameTitleFromDataWin(path);
    const int row = gameTable_->rowCount();
    gameTable_->insertRow(row);
    auto* titleItem = new QTableWidgetItem(gameTitle.isEmpty() ? gameFile.completeBaseName() : gameTitle);
    titleItem->setData(Qt::UserRole, path);
    titleItem->setToolTip(path);
    gameTable_->setItem(row, 0, titleItem);
    auto* pathItem = new QTableWidgetItem(path);
    pathItem->setToolTip(path);
    gameTable_->setItem(row, 1, pathItem);
    gameTable_->selectRow(row);

    if (save) {
        saveGames();
    }
}

void GamesTab::loadGames() {
    QSettings settings;
    const int gameCount = settings.beginReadArray("games");
    for (int index = 0; index < gameCount; ++index) {
        settings.setArrayIndex(index);
        addGame(settings.value("path").toString(), false);
    }
    settings.endArray();
}

void GamesTab::saveGames() const {
    QSettings settings;
    settings.beginWriteArray("games", gameTable_->rowCount());
    for (int row = 0; row < gameTable_->rowCount(); ++row) {
        settings.setArrayIndex(row);
        settings.setValue("path", gameTable_->item(row, 0)->data(Qt::UserRole).toString());
    }
    settings.endArray();
}