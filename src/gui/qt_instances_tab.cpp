#include "qt_instances_tab.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <utility>
#include <vector>

namespace {

constexpr int kMaxVisibleInstanceRows = 250;

class SelfVarValueDelegate : public QStyledItemDelegate {
public:
    explicit SelfVarValueDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.parent().isValid() && index.column() == 2) {
            const auto* tree = qobject_cast<const QTreeWidget*>(option.widget);
            if (tree != nullptr) {
                QRect spanRect = option.rect;
                const int left = tree->columnViewportPosition(2);
                const int right = tree->columnViewportPosition(4) + tree->columnWidth(4);
                spanRect.setLeft(left - tree->viewport()->x());
                spanRect.setRight(right - tree->viewport()->x());

                QStyleOptionViewItem modOption = option;
                modOption.rect = spanRect;
                modOption.text = index.data(Qt::DisplayRole).toString();
                modOption.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
                modOption.state &= ~QStyle::State_HasFocus;

                QStyledItemDelegate::paint(painter, modOption, index);
                return;
            }
        }

        QStyledItemDelegate::paint(painter, option, index);
    }
};

struct InstanceSelfVarRow {
    QString name;
    QString value;
};

struct InstanceTableRow {
    QString id;
    QString objectName;
    QString x;
    QString y;
    QString depth;
    std::vector<InstanceSelfVarRow> selfVariables;
    bool expanded = false;
};

struct VisibleInstanceRow {
    enum class Kind {
        Instance,
        SelfVariable
    };
    int instanceIndex = -1;
    int selfVariableIndex = -1;
    Kind kind = Kind::Instance;
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

        InstanceTableRow row{ idText, objectNameText, xText, yText, depthText, {}, false };
        const QJsonValue selfVariablesValue = entryObject.value(QStringLiteral("selfVariables"));
        if (selfVariablesValue.isObject()) {
            const QJsonObject selfVariablesObject = selfVariablesValue.toObject();
            for (auto it = selfVariablesObject.constBegin(); it != selfVariablesObject.constEnd(); ++it) {
                row.selfVariables.push_back({it.key(), jsonValueToDisplayString(it.value())});
            }
        }

        if (!lowerFilter.isEmpty()) {
            const QString candidate = QStringLiteral("%1 %2 %3 %4 %5").arg(idText, objectNameText, xText, yText, depthText).toLower();
            if (!candidate.contains(lowerFilter)) {
                bool matchedSelfVar = false;
                for (const auto& selfVar : row.selfVariables) {
                    if ((selfVar.name + QStringLiteral(" ") + selfVar.value).toLower().contains(lowerFilter)) {
                        matchedSelfVar = true;
                        break;
                    }
                }
                if (!matchedSelfVar) {
                    continue;
                }
            }
        }

        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

InstancesTab::InstancesTab(QWidget* parent) : QWidget(parent) {
    tableView_ = new QTreeWidget(this);
    tableView_->setColumnCount(5);
    tableView_->setHeaderLabels({QStringLiteral("ID"), QStringLiteral("Object"), QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Depth")});
    tableView_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tableView_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableView_->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tableView_->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    tableView_->header()->setSectionResizeMode(4, QHeaderView::Interactive);
    tableView_->setColumnWidth(0, 80);
    tableView_->setColumnWidth(1, 180);
    tableView_->setColumnWidth(2, 180);
    tableView_->setColumnWidth(3, 180);
    tableView_->setColumnWidth(4, 180);
    tableView_->setAlternatingRowColors(true);
    tableView_->setSelectionMode(QAbstractItemView::NoSelection);
    tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tableView_->setSortingEnabled(false);
    tableView_->setRootIsDecorated(true);
    tableView_->setUniformRowHeights(true);
    tableView_->setAnimated(true);
    tableView_->setItemDelegateForColumn(2, new SelfVarValueDelegate(this));

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

    connect(tableView_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        Q_UNUSED(column);
        if (item == nullptr || item->parent() != nullptr) {
            return;
        }
        if (item->childCount() == 0) {
            return;
        }

        const QString instanceId = item->text(0).trimmed();
        const bool wasExpanded = item->isExpanded();

        if (wasExpanded) {
            tableView_->collapseItem(item);
            if (!instanceId.isEmpty()) {
                expandedInstanceIds_.remove(instanceId);
            }
        } else {
            tableView_->expandItem(item);
            if (!instanceId.isEmpty()) {
                expandedInstanceIds_.insert(instanceId);
            }
        }
    });

    connect(searchBox_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterText_ = text;
        refresh();
    });
}

QTreeWidget* InstancesTab::tableView() const {
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
    QSet<QString> expandedBeforeRefresh;
    for (int i = 0; i < tableView_->topLevelItemCount(); ++i) {
        auto* item = tableView_->topLevelItem(i);
        if (item != nullptr && item->childCount() > 0) {
            const QString id = item->text(0).trimmed();
            if (!id.isEmpty() && item->isExpanded()) {
                expandedBeforeRefresh.insert(id);
            }
        }
    }
    if (!expandedBeforeRefresh.isEmpty()) {
        expandedInstanceIds_ = expandedBeforeRefresh;
    }

    tableView_->clear();
    tableView_->setHeaderLabels({QStringLiteral("ID"), QStringLiteral("Object"), QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Depth")});

    if (processRunning_ && !snapshot_.isEmpty()) {
        std::vector<InstanceTableRow> rows = parseInstanceRowsFromJson(snapshot_, filterText_);
        if (rows.empty()) {
            auto* statusItem = new QTreeWidgetItem(tableView_);
            statusItem->setText(1, filterText_.trimmed().isEmpty() ? QStringLiteral("No active instances") : QStringLiteral("No matching instances"));
            statusItem->setExpanded(false);
            return;
        }
        if (rows.size() > static_cast<size_t>(kMaxVisibleInstanceRows)) {
            rows.resize(kMaxVisibleInstanceRows);
        }

        for (const auto& row : rows) {
            auto* instanceItem = new QTreeWidgetItem(tableView_);
            instanceItem->setText(0, row.id);
            instanceItem->setText(1, row.objectName);
            instanceItem->setText(2, row.x);
            instanceItem->setText(3, row.y);
            instanceItem->setText(4, row.depth);

            if (!row.selfVariables.empty()) {
                for (const auto& selfVar : row.selfVariables) {
                    auto* selfVarItem = new QTreeWidgetItem(instanceItem);
                    selfVarItem->setText(0, QString());
                    selfVarItem->setText(1, selfVar.name);
                    selfVarItem->setText(2, selfVar.value);
                    selfVarItem->setText(3, QString());
                    selfVarItem->setText(4, QString());
                }

                const bool shouldExpand = !row.id.isEmpty() && expandedInstanceIds_.contains(row.id);
                instanceItem->setExpanded(shouldExpand);
            }
        }
        return;
    }

    const QString statusText = processRunning_ ? QStringLiteral("Waiting for instance stream...") : QStringLiteral("No running game");
    const QString detailText = processRunning_ ? QStringLiteral("Process is running; waiting for IPC snapshot") : QString();

    auto* statusItem = new QTreeWidgetItem(tableView_);
    statusItem->setText(1, statusText);
    if (!detailText.isEmpty()) {
        auto* detailItem = new QTreeWidgetItem(statusItem);
        detailItem->setText(1, detailText);
    }
}
