#pragma once

#include <QWidget>
#include <QString>

class QComboBox;
class QLineEdit;
class QPushButton;
class QTableView;

class InstancesTab : public QWidget {
public:
    explicit InstancesTab(QWidget* parent = nullptr);

    QTableView* tableView() const;
    QComboBox* refreshModeSelector() const;
    QPushButton* refreshButton() const;
    void setProcessRunning(bool running);
    void setSnapshot(const QString& jsonText);
    void refresh();

private:
    class Model;
    Model* model_;
    QTableView* tableView_;
    QLineEdit* searchBox_;
    QComboBox* refreshModeSelector_;
    QPushButton* refreshButton_;
    QString snapshot_;
    bool processRunning_ = false;
    QString filterText_;
};
