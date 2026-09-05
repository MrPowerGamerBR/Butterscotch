#include "qt_variables_tab.h"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxVisibleVariableRows = 250;
struct VariableTableRow {
    QString name;
    QString value;
};

std::vector<VariableTableRow> parseVariableRowsFromJson(const QString& jsonText, const QString& filterText) {
    std::vector<VariableTableRow> rows;
    if (jsonText.trimmed().isEmpty()) {
        return rows;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
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
        if (!lowerFilter.isEmpty() && !(nameText + " " + valueText).toLower().contains(lowerFilter)) {
            continue;
        }
        rows.push_back({nameText, valueText});
    }
    return rows;
}

} // namespace

class VariablesTab::Model : public QAbstractTableModel {
public:
    explicit Model(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

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
        if (!index.isValid() || role != Qt::DisplayRole || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
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

VariablesTab::VariablesTab(QWidget* parent) : QWidget(parent), model_(new Model(this)) {
    tableView_ = new QTableView(this);
    tableView_->setModel(model_);
    tableView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    tableView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableView_->setColumnWidth(0, 260);
    tableView_->setAlternatingRowColors(true);
    tableView_->setSelectionMode(QAbstractItemView::NoSelection);
    tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tableView_->setSortingEnabled(false);
    tableView_->verticalHeader()->setVisible(false);

    auto* searchBox = new QLineEdit(this);
    searchBox->setPlaceholderText("Search variables...");

    refreshModeSelector_ = new QComboBox(this);
    refreshModeSelector_->addItem("Live update");
    refreshModeSelector_->addItem("Update every second");
    refreshModeSelector_->addItem("Button only");

    auto* refreshButton = new QPushButton("Refresh variables", this);
    auto* toolbarLayout = new QHBoxLayout();
    toolbarLayout->addWidget(searchBox, 1);
    toolbarLayout->addWidget(refreshModeSelector_);
    toolbarLayout->addWidget(refreshButton);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(toolbarLayout);
    layout->addWidget(tableView_);

    connect(searchBox, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterText_ = text;
        refresh();
    });
}

QTableView* VariablesTab::tableView() const {
    return tableView_;
}

QComboBox* VariablesTab::refreshModeSelector() const {
    return refreshModeSelector_;
}

QPushButton* VariablesTab::refreshButton() const {
    return findChild<QPushButton*>();
}

void VariablesTab::setProcessRunning(bool running) {
    processRunning_ = running;
    refresh();
}

void VariablesTab::setSnapshot(const QString& jsonText) {
    snapshot_ = jsonText;
    refresh();
}

void VariablesTab::refresh() {
    if (processRunning_ && !snapshot_.isEmpty()) {
        std::vector<VariableTableRow> rows = parseVariableRowsFromJson(snapshot_, filterText_);
        if (rows.empty()) {
            rows.push_back({filterText_.trimmed().isEmpty() ? "No global variables yet" : "No matching variables", ""});
        } else if (rows.size() > static_cast<size_t>(kMaxVisibleVariableRows)) {
            rows.resize(kMaxVisibleVariableRows);
        }
        model_->setRows(std::move(rows));
        tableView_->verticalHeader()->setVisible(false);
        return;
    }

    const QString statusText = processRunning_ ? QStringLiteral("Waiting for variable stream...") : QStringLiteral("No running game");
    const QString detailText = processRunning_ ? QStringLiteral("Process is running; waiting for IPC snapshot") : QString();
    model_->setRows({{statusText, detailText}});
}
