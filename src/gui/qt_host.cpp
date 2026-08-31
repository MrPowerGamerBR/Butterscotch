#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include <QWidget>

extern "C" {
    int guiMainImpl(int argc, char* argv[]);
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

    QMenuBar* menuBar = new QMenuBar(&hostWindow);
    QMenu* fileMenu = menuBar->addMenu("&File");
    QAction* openAction = fileMenu->addAction("&Open data.win...");

    QObject::connect(openAction, &QAction::triggered, [&hostWindow]() {
        QString selectedPath = chooseGameFile(&hostWindow);

        if (selectedPath.isEmpty()) {
            return;
        }

        int ret = launchGameFromPath(selectedPath);
        QCoreApplication::exit(ret);
    });

    hostWindow.show();

    QTimer::singleShot(0, [&hostWindow, argc, argv]() {
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

        int ret = guiMainImpl(argc, argv);
        QCoreApplication::exit(ret);
    });

    return app.exec();
}
