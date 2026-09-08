#include "qt_instances_tab.h"

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

constexpr int kMaxVisibleInstanceRows = 250;

struct InstanceTableRow {
    QString id;
    QString objectName;
    QString x;
    QString y;
    QString depth;
};

static QString jsonValueToDisplayString(const QJsonValue& value) {
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 15);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isString()) {
        return value.toString();
    }
    if (value.isNull()) {
        return QStringLiteral("null");
    }
    return value.toVariant().toString();
}

std::vector<InstanceTableRow> parseInstanceRowsFromJson(const QString& jsonText, const QString& filterText) {
    std::vector<InstanceTableRow> rows;
    if (jsonText.trimmed().isEmpty()) {
        return rows;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return rows;
    }

    const QJsonValue instancesValue = document.object().value(QStringLiteral("instances"));
    if (!instancesValue.isArray()) {
        return rows;
    }

    const QString lowerFilter = filterText.trimmed().toLower();
    const auto array = instancesValue.toArray();
    rows.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue& entryValue : array) {
        if (!entryValue.isObject()) {
            continue;
        }

        const QJsonObject entryObject = entryValue.toObject();
        const QString idText = jsonValueToDisplayString(entryObject.value(QStringLiteral("instanceId")));
        const QString objectNameText = entryObject.value(QStringLiteral("objectName")).toString();
        const QString xText = jsonValueToDisplayString(entryObject.value(QStringLiteral("x")));
        const QString yText = jsonValueToDisplayString(entryObject.value(QStringLiteral("y")));
        const QString depthText = jsonValueToDisplayString(entryObject.value(QStringLiteral("depth")));
        if (idText.isEmpty() && objectNameText.isEmpty()) {
            continue;
        }
        if (!lowerFilter.isEmpty()) {
            const QString candidate = QStringLiteral("%1 %2 %3 %4 %5").arg(idText, objectNameText, xText, yText, depthText).toLower();
            if (!candidate.contains(lowerFilter)) {
                continue;
            }
        }
        rows.push_back({idText, objectNameText, xText, yText, depthText});
    }
    return rows;
}

} // namespace

class InstancesTab::Model : public QAbstractTableModel {
public:
    explicit Model(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setRows(std::vector<InstanceTableRow> rows) {
        if (rows.size() == rows_.size() && std::equal(rows.begin(), rows.end(), rows_.begin(),
                                                    [](const InstanceTableRow& left, const InstanceTableRow& right) {
                                                        return left.id == right.id && left.objectName == right.objectName &&
                                                               left.x == right.x && left.y == right.y && left.depth == right.depth;
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
        return 5;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
            return {};
        }

        const InstanceTableRow& row = rows_[index.row()];
        switch (index.column()) {
        case 0: return row.id;
        case 1: return row.objectName;
        case 2: return row.x;
        case 3: return row.y;
        case 4: return row.depth;
        default: return {};
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
            case 0: return QStringLiteral("ID");
            case 1: return QStringLiteral("Object");
            case 2: return QStringLiteral("X");
            case 3: return QStringLiteral("Y");
            case 4: return QStringLiteral("Depth");
            default: return {};
            }
        }
        return {};
    }

private:
    std::vector<InstanceTableRow> rows_;
};

InstancesTab::InstancesTab(QWidget* parent) : QWidget(parent), model_(new Model(this)) {
    tableView_ = new QTableView(this);
    tableView_->setModel(model_);
    tableView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    tableView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableView_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    tableView_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    tableView_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    tableView_->setColumnWidth(0, 80);
    tableView_->setColumnWidth(2, 100);
    tableView_->setColumnWidth(3, 100);
    tableView_->setColumnWidth(4, 80);
    tableView_->setAlternatingRowColors(true);
    tableView_->setSelectionMode(QAbstractItemView::NoSelection);
    tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tableView_->setSortingEnabled(false);
    tableView_->verticalHeader()->setVisible(false);

    searchBox_ = new QLineEdit(this);
    searchBox_->setPlaceholderText("Search instances...");

    refreshModeSelector_ = new QComboBox(this);
    refreshModeSelector_->addItem("Live update");
    refreshModeSelector_->addItem("Update every second");
    refreshModeSelector_->addItem("Button only");

    refreshButton_ = new QPushButton("Refresh instances", this);
    auto* toolbarLayout = new QHBoxLayout();
    toolbarLayout->addWidget(searchBox_, 1);
    toolbarLayout->addWidget(refreshModeSelector_);
    toolbarLayout->addWidget(refreshButton_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(toolbarLayout);
    layout->addWidget(tableView_);

    connect(searchBox_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterText_ = text;
        refresh();
    });
}

QTableView* InstancesTab::tableView() const {
    return tableView_;
}

QComboBox* InstancesTab::refreshModeSelector() const {
    return refreshModeSelector_;
}

QPushButton* InstancesTab::refreshButton() const {
    return refreshButton_;
}

void InstancesTab::setProcessRunning(bool running) {
    processRunning_ = running;
    refresh();
}

void InstancesTab::setSnapshot(const QString& jsonText) {
    snapshot_ = jsonText;
    refresh();
}

void InstancesTab::refresh() {
    if (processRunning_ && !snapshot_.isEmpty()) {
        std::vector<InstanceTableRow> rows = parseInstanceRowsFromJson(snapshot_, filterText_);
        if (rows.empty()) {
            rows.push_back({QStringLiteral(""), filterText_.trimmed().isEmpty() ? QStringLiteral("No active instances") : QStringLiteral("No matching instances"), QStringLiteral(""), QStringLiteral(""), QStringLiteral("")});
        } else if (rows.size() > static_cast<size_t>(kMaxVisibleInstanceRows)) {
            rows.resize(kMaxVisibleInstanceRows);
        }
        model_->setRows(std::move(rows));
        tableView_->verticalHeader()->setVisible(false);
        return;
    }

    const QString statusText = processRunning_ ? QStringLiteral("Waiting for instance stream...") : QStringLiteral("No running game");
    const QString detailText = processRunning_ ? QStringLiteral("Process is running; waiting for IPC snapshot") : QString();
    model_->setRows({{QStringLiteral(""), statusText, QStringLiteral(""), QStringLiteral(""), QStringLiteral("")},
                     {QStringLiteral(""), detailText, QStringLiteral(""), QStringLiteral(""), QStringLiteral("")}});
}
