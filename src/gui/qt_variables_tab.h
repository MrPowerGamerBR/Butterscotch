#pragma once

#include <QWidget>
#include <QString>

class QComboBox;
class QPushButton;
class QTableView;

class VariablesTab : public QWidget {
public:
    explicit VariablesTab(QWidget* parent = nullptr);

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
    QComboBox* refreshModeSelector_;
    QString snapshot_;
    bool processRunning_ = false;
    QString filterText_;
};
