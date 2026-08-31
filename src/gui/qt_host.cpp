#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

extern "C" {
    int guiMainImpl(int argc, char* argv[]);
    typedef struct {
        char* name;
        char* value;
        int instanceId;
        int objectIndex;
        bool isArray;
    } RunnerVariableEntry;
    typedef struct {
        RunnerVariableEntry* entries;
        size_t count;
    } RunnerVariableSnapshot;
    void* Runner_getCurrentRunner(void);
    void Runner_snapshotGlobalVariables(void* runner, RunnerVariableSnapshot* out);
    void Runner_freeVariableSnapshot(RunnerVariableSnapshot* snapshot);
}

static void populateLiveVariablesTable(QTableWidget* tableWidget) {
    if (tableWidget == nullptr) {
        return;
    }

    tableWidget->setColumnCount(2);
    tableWidget->setHorizontalHeaderLabels({"Variable", "Value"});
    tableWidget->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tableWidget->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableWidget->setRowCount(0);

    void* runner = Runner_getCurrentRunner();
    if (runner == nullptr) {
        tableWidget->setRowCount(1);
        tableWidget->setItem(0, 0, new QTableWidgetItem("No running game"));
        tableWidget->setItem(0, 1, new QTableWidgetItem(""));
        return;
    }

    RunnerVariableSnapshot snapshot = {0};
    Runner_snapshotGlobalVariables(runner, &snapshot);

    if (snapshot.count == 0) {
        tableWidget->setRowCount(1);
        tableWidget->setItem(0, 0, new QTableWidgetItem("No global variables yet"));
        tableWidget->setItem(0, 1, new QTableWidgetItem(""));
        Runner_freeVariableSnapshot(&snapshot);
        return;
    }

    tableWidget->setRowCount((int)snapshot.count);
    for (size_t i = 0; i < snapshot.count; ++i) {
        const char* name = snapshot.entries[i].name;
        const char* value = snapshot.entries[i].value;
        if (name == nullptr) {
            continue;
        }

        QTableWidgetItem* nameItem = new QTableWidgetItem(QString::fromUtf8(name));
        QTableWidgetItem* valueItem = new QTableWidgetItem(value != nullptr ? QString::fromUtf8(value) : QString("undefined"));
        tableWidget->setItem((int)i, 0, nameItem);
        tableWidget->setItem((int)i, 1, valueItem);
    }

    Runner_freeVariableSnapshot(&snapshot);
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

static int launchGameFromPath(const QString& path) {
    QByteArray program = QByteArray("butterscotch");
    QByteArray gamePath = path.toUtf8();
    char* argv[] = { program.data(), gamePath.data(), nullptr };
    return guiMainImpl(2, argv);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QWidget hostWindow;
    hostWindow.resize(960, 540);
    hostWindow.setWindowTitle("Butterscotch");

    QVBoxLayout* layout = new QVBoxLayout(&hostWindow);

    QMenuBar* menuBar = new QMenuBar(&hostWindow);
    QMenu* fileMenu = menuBar->addMenu("&File");
    QAction* openAction = fileMenu->addAction("&Open data.win...");

    QTableWidget* variableTable = new QTableWidget(&hostWindow);
    variableTable->setColumnCount(2);
    variableTable->setHorizontalHeaderLabels({"Variable", "Value"});
    variableTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    variableTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    variableTable->setAlternatingRowColors(true);
    variableTable->setSelectionMode(QAbstractItemView::NoSelection);
    variableTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    QPushButton* refreshButton = new QPushButton("Refresh variables", &hostWindow);
    QObject::connect(refreshButton, &QPushButton::clicked, [variableTable]() {
        populateLiveVariablesTable(variableTable);
    });

    layout->addWidget(menuBar);
    layout->addWidget(refreshButton);
    layout->addWidget(variableTable);

    QObject::connect(openAction, &QAction::triggered, [&hostWindow]() {
        QString selectedPath = chooseGameFile(&hostWindow);

        if (selectedPath.isEmpty()) {
            return;
        }

        int ret = launchGameFromPath(selectedPath);
        QCoreApplication::exit(ret);
    });

    hostWindow.show();

    QTimer::singleShot(0, [&hostWindow, argc, argv, variableTable]() {
        if (argc <= 1) {
            QString selectedPath = chooseGameFile(&hostWindow);

            if (selectedPath.isEmpty()) {
                QCoreApplication::exit(0);
                return;
            }

            int ret = launchGameFromPath(selectedPath);
            QCoreApplication::exit(ret);
            return;
        }

        populateLiveVariablesTable(variableTable);
        int ret = guiMainImpl(argc, argv);
        QCoreApplication::exit(ret);
    });

    return app.exec();
}
