// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <QAbstractTableModel>
#include <QDockWidget>
#include <QTimer>
#include "core/movie.h"

class EmuThread;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableView;

namespace Core {
class System;
}

/// Table model showing the frames of the TAS editor (Core::Movie) as a piano roll
class TasEditorModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column : int {
        ColumnFrame,
        ColumnA,
        ColumnB,
        ColumnX,
        ColumnY,
        ColumnL,
        ColumnR,
        ColumnZL,
        ColumnZR,
        ColumnStart,
        ColumnSelect,
        ColumnUp,
        ColumnDown,
        ColumnLeft,
        ColumnRight,
        ColumnCircleX,
        ColumnCircleY,
        ColumnCStickX,
        ColumnCStickY,
        ColumnTouch,
        ColumnAccel,
        ColumnGyro,
        ColumnCount,
    };

    /// Extra empty rows shown after the last frame, so inputs can be entered ahead
    static constexpr int ExtraRows = 120;

    explicit TasEditorModel(Core::Movie& movie, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    static bool IsButtonColumn(int column);
    static bool GetButton(const Core::Movie::TasFrame& frame, int column);
    static void SetButton(Core::Movie::TasFrame& frame, int column, bool pressed);

    /// Updates the row count and the cached frames of the given (visible) rows
    void Refresh(int first_visible, int last_visible);

    Core::Movie::TasFrame GetFrame(int row) const;
    /// Sets the inputs of a frame, appending frames up to it if needed. Returns false if the frame
    /// can't be edited.
    bool SetFrame(int row, const Core::Movie::TasFrame& frame);
    /// Frame to use for new frames (neutral inputs, motion copied from the last frame)
    Core::Movie::TasFrame BlankFrame() const;
    bool IsEditable(int row) const;

    u64 CurrentFrame() const {
        return current_frame;
    }
    int FrameCount() const {
        return frame_count;
    }

signals:
    /// Emitted after the user changed the inputs of a frame
    void FramesEdited(int first_row);

private:
    Core::Movie& movie;
    int frame_count = 0;
    int first_frame = 0;
    u64 current_frame = 0;
    int rows = ExtraRows;
    mutable std::unordered_map<int, Core::Movie::TasFrame> cache;
    mutable std::unordered_map<int, bool> edited_cache;
    /// Rows (among the visible ones) that have a savestate
    std::unordered_set<int> state_cache;
};

/**
 * TAS editor (similar to BizHawk's TAStudio): a piano roll of the inputs of the movie being
 * recorded, where inputs can be toggled, drawn, typed, copied, pasted, inserted and deleted.
 * Editing a frame that was already emulated makes the emulator go back to it (by loading a
 * savestate kept in memory) and emulate forward again with the new inputs.
 */
class TasEditorWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit TasEditorWidget(Core::System& system, QWidget* parent = nullptr);
    ~TasEditorWidget() override;

public slots:
    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void Refresh();
    void SetEnabled(bool enabled);
    void Seek(u64 frame);
    void OnFramesEdited(int first_row);
    void RestorePosition();
    /// Sets the widths of the columns to fit the widest values they can have
    void SizeColumns();

    std::vector<int> SelectedRows() const;
    void CopySelection();
    void Paste(bool insert);
    void InsertBlank();
    void DeleteFrames();
    void ClearInputs();
    void SaveMovie();
    void ShowContextMenu(const QPoint& pos);

    Core::System& system;
    TasEditorModel* model;
    QTableView* view;
    QCheckBox* enable_check;
    QCheckBox* follow_check;
    QCheckBox* overwrite_check;
    QSpinBox* interval_spin;
    QSpinBox* capacity_spin;
    QLabel* status_label;
    QPushButton* save_button;
    QTimer refresh_timer;
    QTimer restore_timer;

    std::vector<Core::Movie::TasFrame> clipboard;

    /// Frame to go back to after emulating an edited frame again
    std::optional<u64> restore_frame;
    /// Frame to reach before saving the movie
    std::optional<u64> save_after_frame;

    // Drawing buttons with the mouse
    bool drawing = false;
    int draw_column = -1;
    int draw_last_row = -1;
    bool draw_value = false;
    int draw_first_row = -1;

    u64 last_current_frame = 0;
};
