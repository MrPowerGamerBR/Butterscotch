#pragma once

#include <QWidget>
#include <QString>
#include <QSet>
#include <QTreeWidget>

class QComboBox;
class QLineEdit;
class QPushButton;

class InstancesTab : public QWidget {
public:
    explicit InstancesTab(QWidget* parent = nullptr);

    QTreeWidget* tableView() const;
    QComboBox* refreshModeSelector() const;
    QPushButton* refreshButton() const;
    void setProcessRunning(bool running);
    void setSnapshot(const QString& jsonText);
    void refresh();

private:
    QTreeWidget* tableView_;
    QLineEdit* searchBox_;
    QComboBox* refreshModeSelector_;
    QPushButton* refreshButton_;
    QString snapshot_;
    bool processRunning_ = false;
    QString filterText_;
    QSet<QString> expandedInstanceIds_;
};
