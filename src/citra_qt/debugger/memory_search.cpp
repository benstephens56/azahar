// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include "citra_qt/debugger/memory_search.h"
#include "citra_qt/debugger/memory_watch.h"
#include "core/core.h"

using namespace MemoryTools;

namespace {

/// Maximum number of results kept by a scan. Keep narrowing with a more specific scan if hit.
constexpr std::size_t MaxCandidates = 10'000'000;
/// Maximum number of results shown in the table
constexpr int MaxDisplayedResults = 1000;
/// How long to wait for the emulator thread to copy memory for a scan
constexpr std::chrono::milliseconds SnapshotTimeout{3000};

bool NearlyEqual(double a, double b) {
    return std::fabs(a - b) <= 1e-4 * std::max(1.0, std::fabs(b));
}

/// Finds the bytes of `address` in a snapshot, or nullptr if it is not in it
const u8* FindInSnapshot(const std::vector<Core::MemoryEditor::MemoryRegion>& snapshot,
                         VAddr address, std::size_t size) {
    auto it = std::upper_bound(snapshot.begin(), snapshot.end(), address,
                               [](VAddr addr, const auto& region) { return addr < region.base; });
    if (it == snapshot.begin()) {
        return nullptr;
    }
    --it;
    const std::size_t offset = address - it->base;
    if (offset + size > it->data.size()) {
        return nullptr;
    }
    return it->data.data() + offset;
}

} // namespace

MemorySearchWidget::MemorySearchWidget(Core::System& system_, MemoryWatchWidget* watch_widget_,
                                       QWidget* parent)
    : QDockWidget(tr("Memory Search"), parent), system{system_}, watch_widget{watch_widget_} {
    setObjectName(QStringLiteral("MemorySearchWidget"));

    auto* main_widget = new QWidget(this);
    auto* layout = new QVBoxLayout(main_widget);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* options = new QGridLayout();
    options->addWidget(new QLabel(tr("Type:"), main_widget), 0, 0);
    type_combo = new QComboBox(main_widget);
    type_combo->addItems(TypeNames());
    type_combo->setCurrentIndex(static_cast<int>(ValueType::U32));
    options->addWidget(type_combo, 0, 1);

    options->addWidget(new QLabel(tr("Scan:"), main_widget), 1, 0);
    compare_combo = new QComboBox(main_widget);
    compare_combo->addItem(tr("Equal to value"), static_cast<int>(Compare::Equal));
    compare_combo->addItem(tr("Not equal to value"), static_cast<int>(Compare::NotEqual));
    compare_combo->addItem(tr("Greater than value"), static_cast<int>(Compare::Greater));
    compare_combo->addItem(tr("Less than value"), static_cast<int>(Compare::Less));
    compare_combo->addItem(tr("Changed"), static_cast<int>(Compare::Changed));
    compare_combo->addItem(tr("Unchanged"), static_cast<int>(Compare::Unchanged));
    compare_combo->addItem(tr("Increased"), static_cast<int>(Compare::Increased));
    compare_combo->addItem(tr("Decreased"), static_cast<int>(Compare::Decreased));
    compare_combo->addItem(tr("Increased by value"), static_cast<int>(Compare::IncreasedBy));
    compare_combo->addItem(tr("Decreased by value"), static_cast<int>(Compare::DecreasedBy));
    compare_combo->addItem(tr("Unknown initial value"), static_cast<int>(Compare::Unknown));
    connect(compare_combo, &QComboBox::currentIndexChanged, this,
            &MemorySearchWidget::OnCompareChanged);
    options->addWidget(compare_combo, 1, 1);

    options->addWidget(new QLabel(tr("Value:"), main_widget), 2, 0);
    value_edit = new QLineEdit(main_widget);
    value_edit->setPlaceholderText(tr("Decimal, or hex with 0x prefix"));
    connect(value_edit, &QLineEdit::returnPressed, this,
            [this] { scanning ? OnNextScan() : OnFirstOrNewScan(); });
    options->addWidget(value_edit, 2, 1);
    options->setColumnStretch(1, 1);
    layout->addLayout(options);

    auto* buttons = new QHBoxLayout();
    first_scan_button = new QPushButton(tr("First Scan"), main_widget);
    connect(first_scan_button, &QPushButton::clicked, this, &MemorySearchWidget::OnFirstOrNewScan);
    buttons->addWidget(first_scan_button);
    next_scan_button = new QPushButton(tr("Next Scan"), main_widget);
    connect(next_scan_button, &QPushButton::clicked, this, &MemorySearchWidget::OnNextScan);
    buttons->addWidget(next_scan_button);
    buttons->addStretch();
    aligned_check = new QCheckBox(tr("Aligned"), main_widget);
    aligned_check->setChecked(true);
    aligned_check->setToolTip(
        tr("Only search addresses that are a multiple of the value size. Game data is almost "
           "always aligned, and this makes scans much faster."));
    buttons->addWidget(aligned_check);
    hex_check = new QCheckBox(tr("Hex"), main_widget);
    hex_check->setToolTip(tr("Show values in hexadecimal"));
    connect(hex_check, &QCheckBox::toggled, this, [this] { RebuildResults(); });
    buttons->addWidget(hex_check);
    layout->addLayout(buttons);

    status_label = new QLabel(main_widget);
    status_label->setWordWrap(true);
    layout->addWidget(status_label);

    results_table = new QTableWidget(0, ColumnCount, main_widget);
    results_table->setHorizontalHeaderLabels(
        {tr("Freeze"), tr("Address"), tr("Value"), tr("Previous")});
    results_table->horizontalHeader()->setSectionResizeMode(ColumnFreeze,
                                                            QHeaderView::ResizeToContents);
    results_table->horizontalHeader()->setStretchLastSection(true);
    results_table->verticalHeader()->setVisible(false);
    results_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_table->setEditTriggers(QAbstractItemView::DoubleClicked |
                                   QAbstractItemView::EditKeyPressed);
    connect(results_table, &QTableWidget::itemChanged, this, &MemorySearchWidget::OnItemChanged);
    connect(results_table, &QTableWidget::itemSelectionChanged, this, [this] {
        add_to_watch_button->setEnabled(results_table->selectionModel()->hasSelection());
    });
    layout->addWidget(results_table, 1);

    add_to_watch_button = new QPushButton(tr("Add Selected to Watch"), main_widget);
    add_to_watch_button->setEnabled(false);
    connect(add_to_watch_button, &QPushButton::clicked, this, &MemorySearchWidget::OnAddToWatch);
    layout->addWidget(add_to_watch_button);

    setWidget(main_widget);

    update_timer.setInterval(16);
    connect(&update_timer, &QTimer::timeout, this, &MemorySearchWidget::UpdateValues);

    Reset();
}

MemorySearchWidget::~MemorySearchWidget() = default;

void MemorySearchWidget::OnEmulationStarting(EmuThread*) {
    emulation_running = true;
    Reset();
    UpdateTimerState();
}

void MemorySearchWidget::OnEmulationStopping() {
    emulation_running = false;
    Reset();
    UpdateTimerState();
}

void MemorySearchWidget::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    UpdateTimerState();
}

void MemorySearchWidget::hideEvent(QHideEvent* event) {
    QDockWidget::hideEvent(event);
    UpdateTimerState();
}

void MemorySearchWidget::UpdateTimerState() {
    if (emulation_running && isVisible()) {
        update_timer.start();
    } else {
        update_timer.stop();
    }
}

void MemorySearchWidget::SetStatus(const QString& text) {
    status_label->setText(text);
}

void MemorySearchWidget::Reset() {
    scanning = false;
    all_candidates = false;
    baseline.clear();
    baseline.shrink_to_fit();
    candidates.clear();
    candidates.shrink_to_fit();

    first_scan_button->setText(tr("First Scan"));
    next_scan_button->setEnabled(false);
    type_combo->setEnabled(true);
    aligned_check->setEnabled(true);
    OnCompareChanged();
    RebuildResults();
    SetStatus(emulation_running ? tr("Choose a scan type and click First Scan.")
                                : tr("Start an application to search its memory."));
}

void MemorySearchWidget::OnCompareChanged() {
    const auto compare = static_cast<Compare>(compare_combo->currentData().toInt());
    const bool needs_value = compare == Compare::Equal || compare == Compare::NotEqual ||
                             compare == Compare::Greater || compare == Compare::Less ||
                             compare == Compare::IncreasedBy || compare == Compare::DecreasedBy;
    value_edit->setEnabled(needs_value);
}

std::optional<MemorySearchWidget::Snapshot> MemorySearchWidget::TakeSnapshot() {
    auto snapshot = system.MemoryEditor().SnapshotWritableMemory(SnapshotTimeout);
    if (!snapshot) {
        SetStatus(tr("Could not read memory. Make sure the application is running (or paused "
                     "with frame advance) and try again."));
    }
    return snapshot;
}

bool MemorySearchWidget::Matches(Compare compare, u32 current, u32 previous, u32 target) const {
    const bool is_float = scan_type == ValueType::Float;
    const double cur = NumericValue(scan_type, current);
    const double prev = NumericValue(scan_type, previous);
    const double value = NumericValue(scan_type, target);
    const auto equal = [is_float](double a, double b) {
        return is_float ? NearlyEqual(a, b) : a == b;
    };
    switch (compare) {
    case Compare::Equal:
        return equal(cur, value);
    case Compare::NotEqual:
        return !equal(cur, value);
    case Compare::Greater:
        return cur > value;
    case Compare::Less:
        return cur < value;
    case Compare::Changed:
        return current != previous;
    case Compare::Unchanged:
        return current == previous;
    case Compare::Increased:
        return cur > prev;
    case Compare::Decreased:
        return cur < prev;
    case Compare::IncreasedBy:
        return equal(cur - prev, value);
    case Compare::DecreasedBy:
        return equal(prev - cur, value);
    case Compare::Unknown:
        return true;
    }
    return false;
}

void MemorySearchWidget::OnFirstOrNewScan() {
    if (scanning) {
        Reset();
        return;
    }
    if (!emulation_running) {
        SetStatus(tr("Start an application to search its memory."));
        return;
    }

    const auto compare = static_cast<Compare>(compare_combo->currentData().toInt());
    const bool compares_previous = compare == Compare::Changed || compare == Compare::Unchanged ||
                                   compare == Compare::Increased || compare == Compare::Decreased ||
                                   compare == Compare::IncreasedBy ||
                                   compare == Compare::DecreasedBy;
    if (compares_previous) {
        SetStatus(tr("The first scan has no previous values to compare with. Use \"Unknown "
                     "initial value\" for the first scan instead."));
        return;
    }

    scan_type = static_cast<ValueType>(type_combo->currentIndex());
    scan_step = aligned_check->isChecked() ? ValueSize(scan_type) : 1;
    u32 target = 0;
    if (compare != Compare::Unknown) {
        const auto parsed = ParseValue(scan_type, value_edit->text());
        if (!parsed) {
            SetStatus(tr("Enter a valid value to search for."));
            return;
        }
        target = *parsed;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto snapshot = TakeSnapshot();
    if (!snapshot) {
        QApplication::restoreOverrideCursor();
        return;
    }

    candidates.clear();
    const std::size_t size = ValueSize(scan_type);
    bool overflowed = false;
    if (compare == Compare::Unknown) {
        all_candidates = true;
        baseline = std::move(*snapshot);
    } else {
        all_candidates = false;
        for (const auto& region : *snapshot) {
            for (std::size_t offset = 0; offset + size <= region.data.size(); offset += scan_step) {
                const u32 current = RawFromBytes(scan_type, region.data.data() + offset);
                if (!Matches(compare, current, current, target)) {
                    continue;
                }
                if (candidates.size() >= MaxCandidates) {
                    overflowed = true;
                    break;
                }
                candidates.push_back({region.base + static_cast<VAddr>(offset), current});
            }
            if (overflowed) {
                break;
            }
        }
    }
    QApplication::restoreOverrideCursor();

    scanning = true;
    first_scan_button->setText(tr("New Scan"));
    next_scan_button->setEnabled(true);
    type_combo->setEnabled(false);
    aligned_check->setEnabled(false);
    if (compare == Compare::Unknown) {
        compare_combo->setCurrentIndex(compare_combo->findData(static_cast<int>(Compare::Changed)));
    }
    RebuildResults();
    if (overflowed) {
        SetStatus(
            tr("Stopped after %1 results. Narrow them down with Next Scan.").arg(MaxCandidates));
    }
}

void MemorySearchWidget::OnNextScan() {
    if (!scanning || !emulation_running) {
        return;
    }
    const auto compare = static_cast<Compare>(compare_combo->currentData().toInt());
    if (compare == Compare::Unknown) {
        SetStatus(tr("\"Unknown initial value\" can only be used for the first scan."));
        return;
    }
    u32 target = 0;
    if (value_edit->isEnabled()) {
        const auto parsed = ParseValue(scan_type, value_edit->text());
        if (!parsed) {
            SetStatus(tr("Enter a valid value to compare with."));
            return;
        }
        target = *parsed;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto snapshot = TakeSnapshot();
    if (!snapshot) {
        QApplication::restoreOverrideCursor();
        return;
    }

    const std::size_t size = ValueSize(scan_type);
    std::vector<Candidate> survivors;
    bool overflowed = false;
    if (all_candidates) {
        for (const auto& region : baseline) {
            for (std::size_t offset = 0; offset + size <= region.data.size(); offset += scan_step) {
                const VAddr address = region.base + static_cast<VAddr>(offset);
                const u8* current_bytes = FindInSnapshot(*snapshot, address, size);
                if (!current_bytes) {
                    continue;
                }
                const u32 previous = RawFromBytes(scan_type, region.data.data() + offset);
                const u32 current = RawFromBytes(scan_type, current_bytes);
                if (!Matches(compare, current, previous, target)) {
                    continue;
                }
                if (survivors.size() >= MaxCandidates) {
                    overflowed = true;
                    break;
                }
                survivors.push_back({address, current});
            }
            if (overflowed) {
                break;
            }
        }
        all_candidates = false;
        baseline.clear();
        baseline.shrink_to_fit();
    } else {
        for (const auto& candidate : candidates) {
            const u8* current_bytes = FindInSnapshot(*snapshot, candidate.address, size);
            if (!current_bytes) {
                continue;
            }
            const u32 current = RawFromBytes(scan_type, current_bytes);
            if (Matches(compare, current, candidate.previous, target)) {
                survivors.push_back({candidate.address, current});
            }
        }
    }
    candidates = std::move(survivors);
    QApplication::restoreOverrideCursor();

    RebuildResults();
    if (overflowed) {
        SetStatus(
            tr("Stopped after %1 results. Narrow them down with Next Scan.").arg(MaxCandidates));
    }
}

void MemorySearchWidget::RebuildResults() {
    updating = true;
    const int shown =
        all_candidates
            ? 0
            : static_cast<int>(std::min<std::size_t>(candidates.size(), MaxDisplayedResults));
    results_table->setRowCount(shown);
    const bool hex = hex_check->isChecked();
    for (int row = 0; row < shown; ++row) {
        const auto& candidate = candidates[row];

        auto* freeze_item = new QTableWidgetItem();
        freeze_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        freeze_item->setCheckState(Qt::Unchecked);
        results_table->setItem(row, ColumnFreeze, freeze_item);

        auto* address_item = new QTableWidgetItem(FormatAddress(candidate.address));
        address_item->setFlags(address_item->flags() & ~Qt::ItemIsEditable);
        results_table->setItem(row, ColumnAddress, address_item);

        results_table->setItem(row, ColumnValue, new QTableWidgetItem());

        auto* previous_item = new QTableWidgetItem(FormatValue(scan_type, candidate.previous, hex));
        previous_item->setFlags(previous_item->flags() & ~Qt::ItemIsEditable);
        results_table->setItem(row, ColumnPrevious, previous_item);
    }
    updating = false;
    UpdateValues();

    if (!scanning) {
        return;
    }
    if (all_candidates) {
        std::size_t total = 0;
        for (const auto& region : baseline) {
            total += region.data.size() / scan_step;
        }
        SetStatus(tr("Captured %1 addresses with unknown values. Let the value change, then use "
                     "Next Scan.")
                      .arg(total));
    } else if (candidates.size() > static_cast<std::size_t>(MaxDisplayedResults)) {
        SetStatus(tr("%1 results (showing the first %2).")
                      .arg(candidates.size())
                      .arg(MaxDisplayedResults));
    } else {
        SetStatus(tr("%n result(s).", "", static_cast<int>(candidates.size())));
    }
}

void MemorySearchWidget::UpdateValues() {
    auto& editor = system.MemoryEditor();
    const bool hex = hex_check->isChecked();
    const std::size_t size = ValueSize(scan_type);

    updating = true;
    for (int row = 0; row < results_table->rowCount(); ++row) {
        const VAddr address = candidates[row].address;
        auto* value_item = results_table->item(row, ColumnValue);
        auto* freeze_item = results_table->item(row, ColumnFreeze);

        // Don't overwrite a value while the user is typing in it
        const bool editing = results_table->indexWidget(results_table->indexFromItem(value_item));
        if (!editing) {
            QString text = QStringLiteral("-");
            if (emulation_running) {
                const auto bytes = editor.Peek(address, size);
                text = bytes ? FormatValue(scan_type, RawFromBytes(scan_type, bytes->data()), hex)
                             : tr("(unreadable)");
            }
            if (value_item->text() != text) {
                value_item->setText(text);
            }
        }

        const auto state = editor.IsFrozen(address) ? Qt::Checked : Qt::Unchecked;
        if (freeze_item->checkState() != state) {
            freeze_item->setCheckState(state);
        }
    }
    updating = false;
}

void MemorySearchWidget::OnItemChanged(QTableWidgetItem* item) {
    if (updating || !item || item->row() >= static_cast<int>(candidates.size())) {
        return;
    }
    const VAddr address = candidates[item->row()].address;
    const std::size_t size = ValueSize(scan_type);
    auto& editor = system.MemoryEditor();

    if (item->column() == ColumnFreeze) {
        if (item->checkState() == Qt::Checked) {
            if (const auto bytes = editor.Peek(address, size)) {
                editor.Freeze(address, *bytes);
            }
        } else {
            editor.Unfreeze(address);
        }
    } else if (item->column() == ColumnValue) {
        if (const auto raw = ParseValue(scan_type, item->text()); raw && emulation_running) {
            auto bytes = BytesFromRaw(scan_type, *raw);
            if (editor.IsFrozen(address)) {
                editor.Freeze(address, std::move(bytes));
            } else {
                editor.QueueWrite(address, std::move(bytes));
            }
        }
    }
    UpdateValues();
}

void MemorySearchWidget::OnAddToWatch() {
    if (!watch_widget) {
        return;
    }
    auto rows = results_table->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    for (const auto& index : rows) {
        const VAddr address = candidates[index.row()].address;
        watch_widget->AddWatch(FormatAddress(address), address, scan_type);
    }
    if (!rows.isEmpty()) {
        watch_widget->show();
        watch_widget->raise();
    }
}
