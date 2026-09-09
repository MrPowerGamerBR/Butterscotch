#pragma once

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QString>

class QWidget;

inline QString resolveGamePath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }

    const QFileInfo fileInfo(path);
    if (fileInfo.exists() && fileInfo.isFile()) {
        return path;
    }

#if defined(Q_OS_MAC)
    if (fileInfo.exists() && fileInfo.isDir()) {
        const QDir bundleDir(fileInfo.absoluteFilePath());
        const QString baseName = fileInfo.completeBaseName();
        const QStringList candidates = {
            bundleDir.filePath(QStringLiteral("Contents/Resources/game.ios")),
            bundleDir.filePath(QStringLiteral("Contents/Resources/")) + baseName + QStringLiteral(".ios"),
            bundleDir.filePath(QStringLiteral("Contents/Resources/game.unx")),
            bundleDir.filePath(QStringLiteral("../Resources/game.ios")),
            bundleDir.filePath(QStringLiteral("../Resources/")) + baseName + QStringLiteral(".ios"),
            bundleDir.filePath(QStringLiteral("game.ios")),
        };

        for (const QString& candidate : candidates) {
            const QFileInfo candidateInfo(candidate);
            if (candidateInfo.exists() && candidateInfo.isFile()) {
                return candidate;
            }
        }
    }
#endif

    return path;
}

inline QString chooseGameFile(QWidget* parent) {
    QFileDialog dialog(parent,
                       "Open a game bundle or game file",
                       QDir::homePath(),
                       "Game files (*.win *.unx *.ios *.app);;All files (*)");
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);

    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }

    const QStringList selectedFiles = dialog.selectedFiles();
    if (selectedFiles.isEmpty()) {
        return {};
    }

    const QString selectedPath = selectedFiles.constFirst();
    return resolveGamePath(selectedPath);
}
