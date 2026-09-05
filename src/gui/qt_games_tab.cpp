#include "qt_games_tab.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QPushButton>
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
    : QWidget(parent), launchGame_(std::move(launchGame)), gameList_(new QListWidget(this)) {
    gameList_->setAlternatingRowColors(true);
    gameList_->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* addGameButton = new QPushButton("Add game...", this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(gameList_);
    layout->addWidget(addGameButton);

    connect(addGameButton, &QPushButton::clicked, this, [this]() {
        addGame(chooseGameFile(this));
    });
    connect(gameList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        launchGame_(item->data(Qt::UserRole).toString());
    });
}

void GamesTab::addGame(const QString& path) {
    if (path.isEmpty()) {
        return;
    }

    for (int index = 0; index < gameList_->count(); ++index) {
        if (gameList_->item(index)->data(Qt::UserRole).toString() == path) {
            gameList_->setCurrentRow(index);
            return;
        }
    }

    const QFileInfo gameFile(path);
    const QString gameTitle = gameTitleFromDataWin(path);
    auto* item = new QListWidgetItem(gameTitle.isEmpty() ? gameFile.completeBaseName() : gameTitle);
    item->setData(Qt::UserRole, path);
    item->setToolTip(path);
    gameList_->addItem(item);
    gameList_->setCurrentItem(item);
}