// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include <QDockWidget>
#include <QTimer>
#include "citra_qt/debugger/memory_tools_common.h"
#include "common/common_types.h"

class EmuThread;
class QCheckBox;
class QTableWidget;
class QTableWidgetItem;

namespace Core {
class System;
}

/**
 * A RAM watch list (similar to Cheat Engine's address list or BizHawk's RAM Watch): shows the live
 * values of a list of addresses of the running application, and lets values be edited or frozen.
 *
 * Reading values has no effect on emulation. Edits and freezes are applied on the emulator thread
 * through Core::MemoryEditor, and only while the user has any of them active.
 *
 * The list is saved per application (by title ID) and restored when that application starts.
 */
class MemoryWatchWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit MemoryWatchWidget(Core::System& system, QWidget* parent = nullptr);
    ~MemoryWatchWidget() override;

    /// Adds a watch to the list, e.g. from the memory search.
    void AddWatch(const QString& label, VAddr address, MemoryTools::ValueType type);

public slots:
    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum Column : int {
        ColumnFreeze,
        ColumnLabel,
        ColumnAddress,
        ColumnType,
        ColumnValue,
        ColumnCount,
    };

    struct WatchEntry {
        QString label;
        VAddr address = 0;
        MemoryTools::ValueType type = MemoryTools::ValueType::U32;
    };

    void RebuildTable();
    void UpdateValues();
    void OnItemChanged(QTableWidgetItem* item);
    void OnAdd();
    void OnRemove();
    void OnMove(int direction);
    void OnClear();
    void OnImport();
    void OnExport();
    void UpdateTimerState();

    void SetFrozen(const WatchEntry& entry, bool frozen);

    QString ListPath() const;
    void LoadList(const QString& path);
    void SaveList(const QString& path) const;

    Core::System& system;
    std::vector<WatchEntry> entries;

    QTableWidget* table;
    QCheckBox* hex_check;
    QTimer update_timer;

    bool emulation_running = false;
    u64 title_id = 0;
    /// Guards against programmatic table updates being treated as user edits
    bool updating = false;
};
