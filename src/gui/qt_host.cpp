#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QAbstractTableModel>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <vector>

class LiveVariableTableModel;
static void populateLiveVariablesTable(QTableView* tableView, LiveVariableTableModel* model, const QString& filterText);

extern "C" {
    int guiMainImpl(int argc, char* argv[]);
}

static constexpr int kMaxVisibleVariableRows = 250;
static QProcess* g_gameProcess = nullptr;
static QString g_lastGamePath;
static QString g_lastProcessSnapshot;
static QString g_processOutputBuffer;
static QString g_processErrorBuffer;
static bool g_variableSnapshotRequestPending = false;
static QTimer* g_variableSnapshotTimeoutTimer = nullptr;

static constexpr int kLiveVariableRefreshMode = 0;
static constexpr int kEverySecondVariableRefreshMode = 1;

static void requestVariableSnapshot() {
    if (g_gameProcess == nullptr || g_gameProcess->state() != QProcess::Running || g_variableSnapshotRequestPending) {
        return;
    }

    if (g_gameProcess->write("BS_REQUEST_VARS\n") > 0) {
        g_variableSnapshotRequestPending = true;
        if (g_variableSnapshotTimeoutTimer != nullptr) {
            g_variableSnapshotTimeoutTimer->start();
        }
    }
}

static void stopGameProcess() {
    if (g_gameProcess == nullptr) {
        return;
    }

    if (g_gameProcess->state() != QProcess::NotRunning) {
        g_gameProcess->terminate();
        if (!g_gameProcess->waitForFinished(1000)) {
            g_gameProcess->kill();
            g_gameProcess->waitForFinished(1000);
        }
    }
    delete g_gameProcess;
    g_gameProcess = nullptr;
    g_variableSnapshotRequestPending = false;
}

static void appendGameLog(QPlainTextEdit* logOutput, const QString& text) {
    if (logOutput == nullptr || text.isEmpty()) {
        return;
    }

    logOutput->moveCursor(QTextCursor::End);
    logOutput->insertPlainText(text);
    logOutput->moveCursor(QTextCursor::End);
}

struct VariableTableRow {
    QString name;
    QString value;
};

static bool isVariableSnapshotJson(const QString& jsonText) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    return parseError.error == QJsonParseError::NoError &&
        document.isObject() &&
        document.object().value(QStringLiteral("variables")).isArray();
}

static QString resolveGameExecutablePath() {
    const QString localBinary = QCoreApplication::applicationDirPath() + QStringLiteral("/butterscotch");
    if (QFileInfo(localBinary).exists() && QFileInfo(localBinary).isFile()) {
        return localBinary;
    }
    return QStringLiteral("butterscotch");
}

static std::vector<VariableTableRow> parseVariableRowsFromJson(const QString& jsonText, const QString& filterText) {
    std::vector<VariableTableRow> rows;
    if (jsonText.trimmed().isEmpty()) {
        return rows;
    }

    const QByteArray jsonBytes = jsonText.toUtf8();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return rows;
    }

    const QJsonValue variablesValue = document.object().value(QStringLiteral("variables"));
    if (!variablesValue.isArray()) {
        return rows;
    }

    const QString lowerFilter = filterText.trimmed().toLower();
    const auto array = variablesValue.toArray();
    rows.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue& entryValue : array) {
        if (!entryValue.isObject()) {
            continue;
        }
        const QJsonObject entryObject = entryValue.toObject();
        const QString nameText = entryObject.value(QStringLiteral("name")).toString();
        const QString valueText = entryObject.value(QStringLiteral("value")).toString();
        if (nameText.isEmpty()) {
            continue;
        }
        if (!lowerFilter.isEmpty()) {
            const QString haystack = (nameText + " " + valueText).toLower();
            if (!haystack.contains(lowerFilter)) {
                continue;
            }
        }
        rows.push_back({nameText, valueText});
    }
    return rows;
}

static void refreshVariableTableState(QTableView* tableView, LiveVariableTableModel* model, const QString& filterText = QString()) {
    if (tableView == nullptr || model == nullptr) {
        return;
    }

    populateLiveVariablesTable(tableView, model, filterText);
}

class LiveVariableTableModel : public QAbstractTableModel {
public:
    explicit LiveVariableTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setRows(std::vector<VariableTableRow> rows) {
        if (rows.size() == rows_.size() && std::equal(rows.begin(), rows.end(), rows_.begin(),
                                                       [](const VariableTableRow& left, const VariableTableRow& right) {
                                                           return left.name == right.name && left.value == right.value;
                                                       })) {
            return;
        }
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        Q_UNUSED(parent);
        return static_cast<int>(rows_.size());
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        Q_UNUSED(parent);
        return 2;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) {
            return {};
        }

        if (role != Qt::DisplayRole) {
            return {};
        }

        if (index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
            return {};
        }

        const VariableTableRow& row = rows_[index.row()];
        return index.column() == 0 ? row.name : row.value;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            return section == 0 ? QString("Variable") : QString("Value");
        }
        return {};
    }

private:
    std::vector<VariableTableRow> rows_;
};

static void populateLiveVariablesTable(QTableView* tableView, LiveVariableTableModel* model, const QString& filterText = QString()) {
    if (tableView == nullptr || model == nullptr) {
        return;
    }

    if (g_gameProcess != nullptr && g_gameProcess->state() == QProcess::Running && !g_lastProcessSnapshot.isEmpty()) {
        std::vector<VariableTableRow> rows = parseVariableRowsFromJson(g_lastProcessSnapshot, filterText);
        if (rows.empty()) {
            const QString emptyMessage = filterText.trimmed().isEmpty() ? "No global variables yet" : "No matching variables";
            rows.push_back({emptyMessage, ""});
        } else if (rows.size() > static_cast<size_t>(kMaxVisibleVariableRows)) {
            rows.resize(kMaxVisibleVariableRows);
        }
        model->setRows(std::move(rows));
        tableView->verticalHeader()->setVisible(false);
        return;
    }

    const QString statusText = (g_gameProcess != nullptr && g_gameProcess->state() == QProcess::Running)
        ? QStringLiteral("Waiting for variable stream...")
        : QStringLiteral("No running game");
    const QString detailText = (g_gameProcess != nullptr && g_gameProcess->state() == QProcess::Running)
        ? QStringLiteral("Process is running; waiting for IPC snapshot")
        : QStringLiteral("");
    model->setRows({{statusText, detailText}});
}

static QString chooseGameFile(QWidget* parent) {
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
    if (selectedFiles.isEmpty()) {
        return {};
    }

    return selectedFiles.constFirst();
}

static void launchGameFromPathProcess(const QString& path, QTableView* tableView = nullptr, LiveVariableTableModel* model = nullptr, QLineEdit* searchBox = nullptr, QComboBox* refreshModeSelector = nullptr, QPlainTextEdit* logOutput = nullptr) {
    if (path.isEmpty()) {
        return;
    }

    g_lastGamePath = path;

    stopGameProcess();

    g_gameProcess = new QProcess(QCoreApplication::instance());
    g_lastProcessSnapshot.clear();
    g_processOutputBuffer.clear();
    g_processErrorBuffer.clear();
    g_variableSnapshotRequestPending = false;
    if (logOutput != nullptr) {
        logOutput->clear();
    }
    g_gameProcess->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(g_gameProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [](int exitCode, QProcess::ExitStatus status) {
                         Q_UNUSED(status);
                         g_variableSnapshotRequestPending = false;
                         if (g_variableSnapshotTimeoutTimer != nullptr) {
                             g_variableSnapshotTimeoutTimer->stop();
                         }
                         if (exitCode != 0) {
                             qWarning() << "Game process exited with code" << exitCode;
                         }
                     });
    QObject::connect(g_gameProcess, &QProcess::readyReadStandardOutput, [tableView, model, searchBox, logOutput]() {
        QByteArray data = g_gameProcess->readAllStandardOutput();
        if (data.isEmpty()) {
            return;
        }

        g_processOutputBuffer += QString::fromUtf8(data);
        while (true) {
            const qsizetype newlineIndex = g_processOutputBuffer.indexOf('\n');
            if (newlineIndex < 0) {
                break;
            }

            QString line = g_processOutputBuffer.left(newlineIndex);
            g_processOutputBuffer.remove(0, newlineIndex + 1);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (!line.startsWith(QStringLiteral("BS_VARS_JSON "))) {
                appendGameLog(logOutput, line + QStringLiteral("\n"));
                continue;
            }
            const QString payload = line.mid(QStringLiteral("BS_VARS_JSON ").size());
            if (!isVariableSnapshotJson(payload)) {
                qWarning() << "Ignoring invalid variable snapshot from game process";
                continue;
            }
            g_lastProcessSnapshot = payload;
            g_variableSnapshotRequestPending = false;
            if (g_variableSnapshotTimeoutTimer != nullptr) {
                g_variableSnapshotTimeoutTimer->stop();
            }
            if (searchBox != nullptr) {
                refreshVariableTableState(tableView, model, searchBox->text());
            } else {
                refreshVariableTableState(tableView, model);
            }
        }
    });
    QObject::connect(g_gameProcess, &QProcess::readyReadStandardError, [logOutput]() {
        const QByteArray data = g_gameProcess->readAllStandardError();
        if (data.isEmpty()) {
            return;
        }

        g_processErrorBuffer += QString::fromUtf8(data);
        while (true) {
            const qsizetype newlineIndex = g_processErrorBuffer.indexOf('\n');
            if (newlineIndex < 0) {
                break;
            }

            QString line = g_processErrorBuffer.left(newlineIndex);
            g_processErrorBuffer.remove(0, newlineIndex + 1);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            appendGameLog(logOutput, line + QStringLiteral("\n"));
        }
    });
    QObject::connect(g_gameProcess, &QProcess::started, [tableView, model, searchBox, refreshModeSelector]() {
        QTimer::singleShot(150, [tableView, model, searchBox, refreshModeSelector]() {
            if (refreshModeSelector != nullptr &&
                refreshModeSelector->currentIndex() != kLiveVariableRefreshMode) {
                return;
            }
            Q_UNUSED(tableView);
            Q_UNUSED(model);
            Q_UNUSED(searchBox);
            requestVariableSnapshot();
        });
    });

    const QString executablePath = resolveGameExecutablePath();
    g_gameProcess->start(executablePath, QStringList{
        path,
        QStringLiteral("--host-child"),
        QStringLiteral("--host-vars-json-on-demand")
    });
}

class VariableTableScrollFilter : public QObject {
public:
    explicit VariableTableScrollFilter(QTimer* refreshTimer) : refreshTimer_(refreshTimer) {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);

        if (event->type() == QEvent::Wheel ||
            event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseMove ||
            event->type() == QEvent::KeyPress) {
            if (refreshTimer_ != nullptr) {
                refreshTimer_->stop();
            }
        }

        return QObject::eventFilter(watched, event);
    }

private:
    QTimer* refreshTimer_;
};

int main(int argc, char* argv[]) {
    bool isChildGameProcess = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--host-child") ||
            arg == QStringLiteral("--host-vars-json") ||
            arg.startsWith(QStringLiteral("--host-vars-json-interval"))) {
            isChildGameProcess = true;
            break;
        }
    }

    if (isChildGameProcess) {
        return guiMainImpl(argc, argv);
    }

    QApplication app(argc, argv);

    QWidget hostWindow;
    hostWindow.resize(960, 540);
    hostWindow.setWindowTitle("Butterscotch");

    QVBoxLayout* layout = new QVBoxLayout(&hostWindow);

    QMenuBar* menuBar = new QMenuBar(&hostWindow);
    QMenu* fileMenu = menuBar->addMenu("&File");
    QAction* openAction = fileMenu->addAction("&Open data.win...");

    QTableView* variableTable = new QTableView(&hostWindow);
    auto* variableModel = new LiveVariableTableModel(variableTable);
    variableTable->setModel(variableModel);
    variableTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    variableTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    variableTable->setColumnWidth(0, 260);
    variableTable->setAlternatingRowColors(true);
    variableTable->setSelectionMode(QAbstractItemView::NoSelection);
    variableTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    variableTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    variableTable->setSortingEnabled(false);
    variableTable->verticalHeader()->setVisible(false);

    QLineEdit* variableSearch = new QLineEdit(&hostWindow);
    variableSearch->setPlaceholderText("Search variables...");

    QComboBox* refreshModeSelector = new QComboBox(&hostWindow);
    refreshModeSelector->addItem("Live update");
    refreshModeSelector->addItem("Update every second");
    refreshModeSelector->addItem("Button only");

    QPlainTextEdit* gameLog = new QPlainTextEdit(&hostWindow);
    gameLog->setReadOnly(true);
    gameLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    gameLog->setPlaceholderText("Game log output will appear here...");

    QTimer variableSnapshotTimer(&hostWindow);
    variableSnapshotTimer.setInterval(100);

    QTimer variableSnapshotTimeoutTimer(&hostWindow);
    variableSnapshotTimeoutTimer.setSingleShot(true);
    variableSnapshotTimeoutTimer.setInterval(1000);
    g_variableSnapshotTimeoutTimer = &variableSnapshotTimeoutTimer;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        stopGameProcess();
    });

    VariableTableScrollFilter scrollFilter(&variableSnapshotTimer);
    variableTable->viewport()->installEventFilter(&scrollFilter);
    variableTable->installEventFilter(&scrollFilter);

    QObject::connect(variableTable->verticalScrollBar(), &QScrollBar::sliderPressed, [&variableSnapshotTimer]() {
        variableSnapshotTimer.stop();
    });
    QObject::connect(variableTable->verticalScrollBar(), &QScrollBar::sliderMoved, [&variableSnapshotTimer]() {
        variableSnapshotTimer.stop();
    });
    QObject::connect(variableTable->verticalScrollBar(), &QScrollBar::sliderReleased, [&variableSnapshotTimer, refreshModeSelector]() {
        if (refreshModeSelector->currentIndex() == kLiveVariableRefreshMode) {
            variableSnapshotTimer.start(100);
        } else if (refreshModeSelector->currentIndex() == kEverySecondVariableRefreshMode) {
            variableSnapshotTimer.start(1000);
        }
    });

    QPushButton* refreshButton = new QPushButton("Refresh variables", &hostWindow);
    QObject::connect(refreshButton, &QPushButton::clicked, [variableTable, variableModel, variableSearch, refreshModeSelector, gameLog]() {
        if (g_gameProcess == nullptr || g_gameProcess->state() == QProcess::NotRunning) {
            if (!g_lastGamePath.isEmpty()) {
                launchGameFromPathProcess(g_lastGamePath, variableTable, variableModel, variableSearch, refreshModeSelector, gameLog);
            }
        }
        requestVariableSnapshot();
        populateLiveVariablesTable(variableTable, variableModel, variableSearch->text());
    });
    QObject::connect(variableSearch, &QLineEdit::textChanged, [variableTable, variableModel, variableSearch]() {
        populateLiveVariablesTable(variableTable, variableModel, variableSearch->text());
    });
    QObject::connect(&variableSnapshotTimer, &QTimer::timeout, []() {
        requestVariableSnapshot();
    });
    QObject::connect(&variableSnapshotTimeoutTimer, &QTimer::timeout, []() {
        g_variableSnapshotRequestPending = false;
        qWarning() << "Timed out waiting for variable snapshot from game process";
    });
    QObject::connect(refreshModeSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [refreshModeSelector, &variableSnapshotTimer](int) {
                         if (refreshModeSelector->currentIndex() == kEverySecondVariableRefreshMode) {
                             variableSnapshotTimer.start(1000);
                         } else if (refreshModeSelector->currentIndex() == kLiveVariableRefreshMode) {
                             variableSnapshotTimer.start(100);
                         } else {
                             variableSnapshotTimer.stop();
                         }
                     });
    variableSnapshotTimer.start();

    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    toolbarLayout->addWidget(variableSearch, 1);
    toolbarLayout->addWidget(refreshModeSelector);
    toolbarLayout->addWidget(refreshButton);

    QWidget* variablesTab = new QWidget(&hostWindow);
    QVBoxLayout* variablesLayout = new QVBoxLayout(variablesTab);
    variablesLayout->addLayout(toolbarLayout);
    variablesLayout->addWidget(variableTable);

    QTabWidget* tabs = new QTabWidget(&hostWindow);
    tabs->addTab(gameLog, "Log");
    tabs->addTab(variablesTab, "Variables");
    tabs->setCurrentWidget(gameLog);

    layout->addWidget(menuBar);
    layout->addWidget(tabs);

    QObject::connect(openAction, &QAction::triggered, [&hostWindow, variableTable, variableModel, variableSearch, refreshModeSelector, gameLog]() {
        QString selectedPath = chooseGameFile(&hostWindow);
        if (selectedPath.isEmpty()) {
            return;
        }

        g_lastGamePath = selectedPath;
        launchGameFromPathProcess(selectedPath, variableTable, variableModel, variableSearch, refreshModeSelector, gameLog);
    });

    hostWindow.show();

    QTimer::singleShot(0, [&hostWindow, argc, argv, variableTable, variableModel, variableSearch, refreshModeSelector, gameLog]() {
        if (argc <= 1) {
            QString selectedPath = chooseGameFile(&hostWindow);
            if (selectedPath.isEmpty()) {
                QCoreApplication::exit(0);
                return;
            }

            g_lastGamePath = selectedPath;
            launchGameFromPathProcess(selectedPath, variableTable, variableModel, variableSearch, refreshModeSelector, gameLog);
            return;
        }

        QString launchPath = QString::fromLocal8Bit(argv[1]);
        g_lastGamePath = launchPath;
        populateLiveVariablesTable(variableTable, variableModel);
        launchGameFromPathProcess(launchPath, variableTable, variableModel, variableSearch, refreshModeSelector, gameLog);
    });

    return app.exec();
}
