#include "qt_games_tab.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

#if defined(Q_OS_MAC)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "qt_game_file_dialog.h"
#include "qt_game_os_type.h"

extern "C" {
#include "data_win.h"
}

namespace {

constexpr int kLastPlayedRole = Qt::UserRole + 1;
constexpr int kTimePlayedSecondsRole = Qt::UserRole + 2;
constexpr int kSaveFolderRole = Qt::UserRole + 3;
constexpr int kOsTypeRole = Qt::UserRole + 4;

QString gameTitleFromDataWin(const QString& path);

QString bundleIdentifierFromGamePath(const QString& gamePath) {
#if defined(Q_OS_MAC)
    const QFileInfo gameInfo(gamePath);
    const QString infoPlistPath = gameInfo.dir().absoluteFilePath(QStringLiteral("../Info.plist"));
    QFile infoPlist(infoPlistPath);
    if (!infoPlist.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QByteArray plistData = infoPlist.readAll();
    CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault,
                                    reinterpret_cast<const UInt8*>(plistData.constData()),
                                    plistData.size());
    if (dataRef == nullptr) {
        return {};
    }

    CFErrorRef error = nullptr;
    CFPropertyListRef propertyList = CFPropertyListCreateWithData(kCFAllocatorDefault,
                                                                  dataRef,
                                                                  kCFPropertyListImmutable,
                                                                  nullptr,
                                                                  &error);
    CFRelease(dataRef);

    if (propertyList == nullptr || error != nullptr) {
        if (error != nullptr) {
            CFRelease(error);
        }
        return {};
    }

    if (CFGetTypeID(propertyList) != CFDictionaryGetTypeID()) {
        CFRelease(propertyList);
        return {};
    }

    const void* bundleId = CFDictionaryGetValue((CFDictionaryRef) propertyList, CFSTR("CFBundleIdentifier"));
    if (bundleId == nullptr || CFGetTypeID(bundleId) != CFStringGetTypeID()) {
        CFRelease(propertyList);
        return {};
    }

    const QString bundleIdentifier = QString::fromCFString((CFStringRef) bundleId);
    CFRelease(propertyList);
    return bundleIdentifier;
#else
    Q_UNUSED(gamePath);
    return {};
#endif
}

QString defaultGameSaveFolder(const QString& gamePath) {
    QString safeGameName;

#if defined(Q_OS_MAC)
    safeGameName = bundleIdentifierFromGamePath(gamePath);
#endif

    if (safeGameName.isEmpty()) {
        const QString gameName = gameTitleFromDataWin(gamePath);
        safeGameName = gameName.isEmpty() ? QFileInfo(gamePath).completeBaseName() : gameName;
    }

#if defined(Q_OS_WIN)
    const QByteArray localAppData = qgetenv("LOCALAPPDATA");
    const QString basePath = localAppData.isEmpty() ? QDir::homePath() + QStringLiteral("/AppData/Local") : QString::fromLocal8Bit(localAppData);
    return QDir(basePath).filePath(safeGameName);
#elif defined(Q_OS_MAC)
    return QDir(QDir::homePath() + QStringLiteral("/Library/Application Support")).filePath(safeGameName);
#else
    return QDir(QDir::homePath() + QStringLiteral("/.config")).filePath(safeGameName);
#endif
}

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

GamesTab::GamesTab(std::function<void(const QString&, const QString&, const QString&)> launchGame, QWidget* parent)
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
        const QString selectedPath = chooseGameFile(this);
        if (selectedPath.isEmpty()) {
            return;
        }

        const QString path = resolveGamePath(selectedPath);
        if (path.isEmpty()) {
            return;
        }

        const QString osType = defaultGameOsType(path);
        const QString saveFolder = defaultGameSaveFolder(path);
        QDir().mkpath(saveFolder);
        addGame(path, saveFolder, osType);
    });
    connect(gameTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString path = gameTable_->item(row, 0)->data(Qt::UserRole).toString();
        const QString saveFolder = gameTable_->item(row, 0)->data(kSaveFolderRole).toString();
        const QString osType = gameTable_->item(row, 0)->data(kOsTypeRole).toString();
        launchGame_(path, saveFolder, osType);
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
        QAction* editSaveFolderAction = menu.addAction("Edit save folder...");
        QAction* setOsTypeAction = menu.addAction("Set OS type...");
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
        } else if (selectedAction == editSaveFolderAction) {
            QTableWidgetItem* titleItem = gameTable_->item(row, 0);
            const QString currentSaveFolder = titleItem->data(kSaveFolderRole).toString();
            const QString newSaveFolder = QFileDialog::getExistingDirectory(this, "Choose save folder",
                                                                            currentSaveFolder.isEmpty() ? QFileInfo(titleItem->data(Qt::UserRole).toString()).absolutePath() : currentSaveFolder);
            if (!newSaveFolder.isEmpty()) {
                titleItem->setData(kSaveFolderRole, newSaveFolder);
                saveGames();
            }
        } else if (selectedAction == setOsTypeAction) {
            QTableWidgetItem* titleItem = gameTable_->item(row, 0);
            const QString currentOsType = titleItem->data(kOsTypeRole).toString();
            const QStringList osTypes = allGameOsTypeNames();
            const QString defaultOsType = defaultGameOsType(titleItem->data(Qt::UserRole).toString());
            const QString currentDisplayOsType = prettyGameOsTypeName(currentOsType);
            int currentIndex = osTypes.indexOf(currentDisplayOsType);
            if (currentIndex < 0) {
                currentIndex = osTypes.indexOf(prettyGameOsTypeName(defaultOsType));
            }
            bool accepted = false;
            const QString selectedDisplayOsType = QInputDialog::getItem(this,
                                                                        "Set OS type",
                                                                        "OS type:",
                                                                        osTypes,
                                                                        std::max(currentIndex, 0),
                                                                        false,
                                                                        &accepted);
            if (accepted && !selectedDisplayOsType.isEmpty()) {
                const QString selectedCanonicalOsType = canonicalGameOsTypeName(selectedDisplayOsType);
                titleItem->setData(kOsTypeRole, selectedCanonicalOsType);
                saveGames();
            }
        } else if (selectedAction == removeAction) {
            gameTable_->removeRow(row);
            saveGames();
        }
    });

    loadGames();
}

void GamesTab::addGame(const QString& path, const QString& saveFolder, const QString& osType, const QString& name, const QString& lastPlayed, qint64 timePlayedSeconds, bool save) {
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
    const QString gameOsType = osType.isEmpty() ? defaultGameOsType(path) : osType;
    const int row = gameTable_->rowCount();
    gameTable_->insertRow(row);
    auto* titleItem = new QTableWidgetItem(gameTitle.isEmpty() ? gameFile.completeBaseName() : gameTitle);
    titleItem->setData(Qt::UserRole, path);
    titleItem->setData(kLastPlayedRole, lastPlayed);
    titleItem->setData(kTimePlayedSecondsRole, timePlayedSeconds);
    titleItem->setData(kSaveFolderRole, saveFolder);
    titleItem->setData(kOsTypeRole, gameOsType);
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
        const QString path = settings.value("path").toString();
        QString saveFolder = settings.value("saveFolder").toString();
        QString osType = settings.value("osType").toString();
        if (saveFolder.isEmpty() && !path.isEmpty()) {
            saveFolder = defaultGameSaveFolder(path);
            needsSave = true;
        }
        if (osType.isEmpty()) {
            osType = defaultGameOsType(path);
            needsSave = true;
        }
        needsSave |= name.isEmpty();
        addGame(path, saveFolder, osType, name, settings.value("lastPlayed").toString(),
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
        settings.setValue("saveFolder", gameTable_->item(row, 0)->data(kSaveFolderRole).toString());
        settings.setValue("osType", gameTable_->item(row, 0)->data(kOsTypeRole).toString());
        settings.setValue("lastPlayed", gameTable_->item(row, 0)->data(kLastPlayedRole).toString());
        settings.setValue("timePlayedSeconds", gameTable_->item(row, 0)->data(kTimePlayedSecondsRole));
    }
    settings.endArray();
}