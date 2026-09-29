// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <QAbstractTableModel>
#include <QDockWidget>
#include <QItemSelection>
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
    /// Inserts frames before a row, filling the gap with blank frames if the row is past the end
    bool InsertFrames(int row, const std::vector<Core::Movie::TasFrame>& frames);
    bool DeleteFrames(int row, int count);
    /// Clears the cached frames and repaints the whole table
    void Reload();

    // Undo and redo. Edits made between BeginStep and EndStep are undone together.
    void BeginStep();
    void EndStep();
    /// Returns the first row changed, or -1 if there was nothing to undo or redo
    int Undo();
    int Redo();
    bool CanUndo();
    bool CanRedo();
    void ClearUndo();
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
    /// Emitted when loading a savestate replaced the frames with the ones it was made with
    void FramesLoaded();

private:
    struct UndoChange {
        int start;
        std::shared_ptr<const Core::Movie::TasFrameBlock> before;
        std::shared_ptr<const Core::Movie::TasFrameBlock> after;
    };
    using UndoStep = std::vector<UndoChange>;
    static constexpr std::size_t MaxUndoSteps = 1000;

    void Record(UndoChange change);
    void PushStep(UndoStep step);
    /// Forgets the undo history if the frames of the editor were recreated
    void CheckSession();

    Core::Movie& movie;
    std::vector<UndoStep> undo_stack;
    std::vector<UndoStep> redo_stack;
    UndoStep pending_step;
    int step_depth = 0;
    u64 session_id = 0;
    u64 frames_load_count = 0;

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

    /// Rows with at least one selected cell
    std::vector<int> SelectedRows() const;
    void CopySelection();
    void Paste(bool insert);
    void InsertBlank();
    void DeleteFrames();
    /// Clears the inputs of the selected cells (the whole frame for fully selected rows)
    void ClearSelectedInputs();
    /// Clears all the inputs of the frames with a selected cell
    void ClearFrames();
    void Undo();
    void Redo();
    void GoToFrame(int frame);
    /// Selects the rows between the drag anchor and `row`, keeping the selection the drag started
    /// with
    void SelectRowRange(int row);
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
    QSpinBox* goto_spin;
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

    // Selecting rows by clicking or dragging on the frame column
    bool row_dragging = false;
    int row_anchor = -1;
    QItemSelection row_drag_base;

    u64 last_current_frame = 0;

    /// Message shown in the status line for a while instead of the status
    QString notice;
    QTimer notice_timer;
};
