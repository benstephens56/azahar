// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>
#include <fmt/format.h>
#include "citra_qt/debugger/memory_watch.h"
#include "common/file_util.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/memory_editor.h"

using namespace MemoryTools;

namespace {

/// Edits the type column with a combo box
class TypeDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex&) const override {
        auto* combo = new QComboBox(parent);
        combo->addItems(TypeNames());
        return combo;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        static_cast<QComboBox*>(editor)->setCurrentText(index.data().toString());
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        model->setData(index, static_cast<QComboBox*>(editor)->currentText());
    }
};

} // namespace

MemoryWatchWidget::MemoryWatchWidget(Core::System& system_, QWidget* parent)
    : QDockWidget(tr("Memory Watch"), parent), system{system_} {
    setObjectName(QStringLiteral("MemoryWatchWidget"));

    auto* main_widget = new QWidget(this);
    auto* layout = new QVBoxLayout(main_widget);
    layout->setContentsMargins(4, 4, 4, 4);

    table = new QTableWidget(0, ColumnCount, main_widget);
    table->setHorizontalHeaderLabels(
        {tr("Freeze"), tr("Label"), tr("Address"), tr("Type"), tr("Value")});
    table->horizontalHeader()->setSectionResizeMode(ColumnFreeze, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                           QAbstractItemView::AnyKeyPressed);
    table->setItemDelegateForColumn(ColumnType, new TypeDelegate(table));
    connect(table, &QTableWidget::itemChanged, this, &MemoryWatchWidget::OnItemChanged);
    layout->addWidget(table);

    auto* button_row = new QHBoxLayout();
    const auto add_button = [&](const QString& text, const QString& tooltip, auto&& slot) {
        auto* button = new QPushButton(text, main_widget);
        button->setToolTip(tooltip);
        connect(button, &QPushButton::clicked, this, slot);
        button_row->addWidget(button);
    };
    add_button(tr("Add"), tr("Add a new watch"), [this] { OnAdd(); });
    add_button(tr("Remove"), tr("Remove the selected watches"), [this] { OnRemove(); });
    add_button(tr("Up"), tr("Move the selected watch up"), [this] { OnMove(-1); });
    add_button(tr("Down"), tr("Move the selected watch down"), [this] { OnMove(1); });
    add_button(tr("Clear"), tr("Remove all watches"), [this] { OnClear(); });
    add_button(tr("Import..."), tr("Load a watch list from a file"), [this] { OnImport(); });
    add_button(tr("Export..."), tr("Save the watch list to a file"), [this] { OnExport(); });
    button_row->addStretch();
    hex_check = new QCheckBox(tr("Hex"), main_widget);
    hex_check->setToolTip(tr("Show values in hexadecimal"));
    connect(hex_check, &QCheckBox::toggled, this, [this] { UpdateValues(); });
    button_row->addWidget(hex_check);
    layout->addLayout(button_row);

    setWidget(main_widget);

    update_timer.setInterval(16);
    connect(&update_timer, &QTimer::timeout, this, &MemoryWatchWidget::UpdateValues);

    LoadList(ListPath());
}

MemoryWatchWidget::~MemoryWatchWidget() {
    SaveList(ListPath());
}

QString MemoryWatchWidget::ListPath() const {
    const std::string name = title_id == 0 ? "global" : fmt::format("{:016X}", title_id);
    return QString::fromStdString(fmt::format(
        "{}memory_watch/{}.ini", FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir), name));
}

void MemoryWatchWidget::LoadList(const QString& path) {
    entries.clear();
    QSettings settings(path, QSettings::IniFormat);
    const int count = settings.beginReadArray(QStringLiteral("Watches"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        WatchEntry entry;
        entry.label = settings.value(QStringLiteral("label")).toString();
        entry.address = settings.value(QStringLiteral("address"), 0).toUInt();
        entry.type =
            static_cast<ValueType>(std::clamp(settings.value(QStringLiteral("type"), 4).toInt(), 0,
                                              static_cast<int>(ValueType::Float)));
        entries.push_back(entry);
    }
    settings.endArray();
    RebuildTable();
}

void MemoryWatchWidget::SaveList(const QString& path) const {
    FileUtil::CreateFullPath(path.toStdString());
    QSettings settings(path, QSettings::IniFormat);
    settings.remove(QStringLiteral("Watches"));
    settings.beginWriteArray(QStringLiteral("Watches"), static_cast<int>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        settings.setArrayIndex(static_cast<int>(i));
        settings.setValue(QStringLiteral("label"), entries[i].label);
        settings.setValue(QStringLiteral("address"), entries[i].address);
        settings.setValue(QStringLiteral("type"), static_cast<int>(entries[i].type));
    }
    settings.endArray();
}

void MemoryWatchWidget::OnEmulationStarting(EmuThread*) {
    // Save the list of the previous application and load the one of the new application. If the
    // new application has no list yet, it starts with the current one.
    SaveList(ListPath());
    title_id = 0;
    system.GetAppLoader().ReadProgramId(title_id);
    if (QFile::exists(ListPath())) {
        LoadList(ListPath());
    } else {
        SaveList(ListPath());
    }

    emulation_running = true;
    UpdateTimerState();
    UpdateValues();
}

void MemoryWatchWidget::OnEmulationStopping() {
    SaveList(ListPath());
    emulation_running = false;
    UpdateTimerState();
    UpdateValues();
}

void MemoryWatchWidget::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    UpdateTimerState();
}

void MemoryWatchWidget::hideEvent(QHideEvent* event) {
    QDockWidget::hideEvent(event);
    UpdateTimerState();
}

void MemoryWatchWidget::UpdateTimerState() {
    if (emulation_running && isVisible()) {
        update_timer.start();
    } else {
        update_timer.stop();
    }
}

void MemoryWatchWidget::AddWatch(const QString& label, VAddr address, ValueType type) {
    entries.push_back({label, address, type});
    RebuildTable();
    table->scrollToBottom();
    SaveList(ListPath());
}

void MemoryWatchWidget::RebuildTable() {
    updating = true;
    table->setRowCount(static_cast<int>(entries.size()));
    for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
        const auto& entry = entries[row];

        auto* freeze_item = new QTableWidgetItem();
        freeze_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        freeze_item->setCheckState(Qt::Unchecked);
        table->setItem(row, ColumnFreeze, freeze_item);

        table->setItem(row, ColumnLabel, new QTableWidgetItem(entry.label));
        table->setItem(row, ColumnAddress, new QTableWidgetItem(FormatAddress(entry.address)));
        table->setItem(row, ColumnType,
                       new QTableWidgetItem(TypeNames()[static_cast<int>(entry.type)]));
        table->setItem(row, ColumnValue, new QTableWidgetItem(QStringLiteral("-")));
    }
    updating = false;
    UpdateValues();
}

void MemoryWatchWidget::UpdateValues() {
    auto& editor = system.MemoryEditor();
    const bool hex = hex_check->isChecked();

    updating = true;
    for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
        const auto& entry = entries[row];
        auto* value_item = table->item(row, ColumnValue);
        auto* freeze_item = table->item(row, ColumnFreeze);
        if (!value_item || !freeze_item) {
            continue;
        }

        // Don't overwrite a value while the user is typing in it
        if (table->indexWidget(table->indexFromItem(value_item))) {
            continue;
        }

        QString text = QStringLiteral("-");
        if (emulation_running) {
            const auto bytes = editor.Peek(entry.address, ValueSize(entry.type));
            text = bytes ? FormatValue(entry.type, RawFromBytes(entry.type, bytes->data()), hex)
                         : tr("(unreadable)");
        }
        if (value_item->text() != text) {
            value_item->setText(text);
        }

        const auto state = editor.IsFrozen(entry.address) ? Qt::Checked : Qt::Unchecked;
        if (freeze_item->checkState() != state) {
            freeze_item->setCheckState(state);
        }
    }
    updating = false;
}

void MemoryWatchWidget::SetFrozen(const WatchEntry& entry, bool frozen) {
    auto& editor = system.MemoryEditor();
    if (!frozen) {
        editor.Unfreeze(entry.address);
        return;
    }
    // Freeze the value the address currently has
    if (const auto bytes = editor.Peek(entry.address, ValueSize(entry.type))) {
        editor.Freeze(entry.address, *bytes);
    }
}

void MemoryWatchWidget::OnItemChanged(QTableWidgetItem* item) {
    if (updating || !item) {
        return;
    }
    const int row = item->row();
    if (row < 0 || row >= static_cast<int>(entries.size())) {
        return;
    }
    auto& entry = entries[row];
    auto& editor = system.MemoryEditor();

    switch (item->column()) {
    case ColumnFreeze:
        SetFrozen(entry, item->checkState() == Qt::Checked);
        break;
    case ColumnLabel:
        entry.label = item->text();
        break;
    case ColumnAddress:
        if (const auto address = ParseAddress(item->text())) {
            if (editor.IsFrozen(entry.address)) {
                editor.Unfreeze(entry.address);
            }
            entry.address = *address;
        }
        updating = true;
        item->setText(FormatAddress(entry.address));
        updating = false;
        break;
    case ColumnType: {
        const int index = TypeNames().indexOf(item->text());
        if (index >= 0) {
            entry.type = static_cast<ValueType>(index);
            if (editor.IsFrozen(entry.address)) {
                SetFrozen(entry, true);
            }
        }
        updating = true;
        item->setText(TypeNames()[static_cast<int>(entry.type)]);
        updating = false;
        break;
    }
    case ColumnValue:
        if (const auto raw = ParseValue(entry.type, item->text()); raw && emulation_running) {
            auto bytes = BytesFromRaw(entry.type, *raw);
            if (editor.IsFrozen(entry.address)) {
                editor.Freeze(entry.address, std::move(bytes));
            } else {
                editor.QueueWrite(entry.address, std::move(bytes));
            }
        }
        break;
    default:
        break;
    }
    UpdateValues();
    if (item->column() != ColumnValue && item->column() != ColumnFreeze) {
        SaveList(ListPath());
    }
}

void MemoryWatchWidget::OnAdd() {
    AddWatch(tr("New Watch"), 0, ValueType::U32);
    table->editItem(table->item(table->rowCount() - 1, ColumnAddress));
}

void MemoryWatchWidget::OnRemove() {
    auto rows = table->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex& a, const QModelIndex& b) { return a.row() > b.row(); });
    for (const auto& index : rows) {
        entries.erase(entries.begin() + index.row());
    }
    RebuildTable();
    SaveList(ListPath());
}

void MemoryWatchWidget::OnMove(int direction) {
    const int row = table->currentRow();
    const int target = row + direction;
    if (row < 0 || target < 0 || target >= static_cast<int>(entries.size())) {
        return;
    }
    std::swap(entries[row], entries[target]);
    RebuildTable();
    table->selectRow(target);
    SaveList(ListPath());
}

void MemoryWatchWidget::OnClear() {
    entries.clear();
    RebuildTable();
    SaveList(ListPath());
}

void MemoryWatchWidget::OnImport() {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Import Watch List"), {}, tr("Watch List (*.ini)"));
    if (!path.isEmpty()) {
        LoadList(path);
        SaveList(ListPath());
    }
}

void MemoryWatchWidget::OnExport() {
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export Watch List"), {}, tr("Watch List (*.ini)"));
    if (!path.isEmpty()) {
        SaveList(path);
    }
}
