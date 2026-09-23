// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include <QDockWidget>
#include <QTimer>
#include "citra_qt/debugger/memory_tools_common.h"
#include "common/common_types.h"
#include "core/memory_editor.h"

class EmuThread;
class MemoryWatchWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

namespace Core {
class System;
}

/**
 * A RAM search (similar to Cheat Engine's scanner or BizHawk's RAM Search): scans the writable
 * memory of the running application for a value, then narrows the results down with further
 * scans comparing against a value or the previous scan. Results can be edited, frozen and added
 * to the memory watch list.
 *
 * Scans copy memory on the emulator thread without modifying anything, so they have no effect on
 * emulation. VRAM and memory currently cached by the GPU (graphics data) are not searched.
 */
class MemorySearchWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit MemorySearchWidget(Core::System& system, MemoryWatchWidget* watch_widget,
                                QWidget* parent = nullptr);
    ~MemorySearchWidget() override;

public slots:
    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum class Compare : int {
        Equal,
        NotEqual,
        Greater,
        Less,
        Changed,
        Unchanged,
        Increased,
        Decreased,
        IncreasedBy,
        DecreasedBy,
        Unknown,
    };

    enum Column : int {
        ColumnFreeze,
        ColumnAddress,
        ColumnValue,
        ColumnPrevious,
        ColumnCount,
    };

    struct Candidate {
        VAddr address;
        u32 previous; ///< Value at the previous scan
    };

    using Snapshot = std::vector<Core::MemoryEditor::MemoryRegion>;

    void OnFirstOrNewScan();
    void OnNextScan();
    void OnAddToWatch();
    void OnItemChanged(QTableWidgetItem* item);
    void OnCompareChanged();

    void Reset();
    std::optional<Snapshot> TakeSnapshot();
    bool Matches(Compare compare, u32 current, u32 previous, u32 target) const;
    void RebuildResults();
    void UpdateValues();
    void UpdateTimerState();
    void SetStatus(const QString& text);

    Core::System& system;
    MemoryWatchWidget* watch_widget;

    QComboBox* type_combo;
    QComboBox* compare_combo;
    QLineEdit* value_edit;
    QPushButton* first_scan_button;
    QPushButton* next_scan_button;
    QCheckBox* aligned_check;
    QCheckBox* hex_check;
    QLabel* status_label;
    QTableWidget* results_table;
    QPushButton* add_to_watch_button;
    QTimer update_timer;

    // Scan state
    bool scanning = false;
    MemoryTools::ValueType scan_type = MemoryTools::ValueType::U32;
    std::size_t scan_step = 4;
    /// Set after an "Unknown initial value" first scan: every address is a candidate and the
    /// previous values are in `baseline`.
    bool all_candidates = false;
    Snapshot baseline;
    std::vector<Candidate> candidates;

    bool emulation_running = false;
    bool updating = false;
};
