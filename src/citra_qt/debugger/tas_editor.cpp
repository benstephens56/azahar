// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include "citra_qt/debugger/tas_editor.h"
#include "citra_qt/uisettings.h"
#include "core/core.h"

using TasFrame = Core::Movie::TasFrame;

namespace {

struct ButtonColumn {
    int column;
    const char* name;
    int bit; ///< Bit in TasFrame::buttons, or -1 for ZL (-2 for ZR)
};

constexpr std::array<ButtonColumn, 14> ButtonColumns{{
    {TasEditorModel::ColumnA, "A", 0},
    {TasEditorModel::ColumnB, "B", 1},
    {TasEditorModel::ColumnX, "X", 10},
    {TasEditorModel::ColumnY, "Y", 11},
    {TasEditorModel::ColumnL, "L", 9},
    {TasEditorModel::ColumnR, "R", 8},
    {TasEditorModel::ColumnZL, "ZL", -1},
    {TasEditorModel::ColumnZR, "ZR", -2},
    {TasEditorModel::ColumnStart, "St", 3},
    {TasEditorModel::ColumnSelect, "Se", 2},
    {TasEditorModel::ColumnUp, "↑", 6},
    {TasEditorModel::ColumnDown, "↓", 7},
    {TasEditorModel::ColumnLeft, "←", 5},
    {TasEditorModel::ColumnRight, "→", 4},
}};

/// Background of the row of the current frame, distinct from the selection color
const QColor CurrentFrameColor(230, 0, 160, 60);
/// Outline of the row of the current frame, drawn over selected cells too
const QColor CurrentFrameOutline(230, 0, 160);

const ButtonColumn* FindButton(int column) {
    for (const auto& button : ButtonColumns) {
        if (button.column == column) {
            return &button;
        }
    }
    return nullptr;
}

QString FormatVector(const std::array<s16, 3>& v) {
    return QStringLiteral("%1, %2, %3").arg(v[0]).arg(v[1]).arg(v[2]);
}

/// Parses "a, b" or "a, b, c" (any separator) into integers
std::vector<int> ParseNumbers(const QString& text) {
    std::vector<int> numbers;
    static const QRegularExpression separator(QStringLiteral("[,;\\s]+"));
    for (const auto& part : text.split(separator, Qt::SkipEmptyParts)) {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok) {
            return {};
        }
        numbers.push_back(value);
    }
    return numbers;
}

// Frames are put on the system clipboard as JSON in the format used by CTM Studio, so they can be
// copied and pasted between the two:
//   {"format": "ctm-frames", "version": 1, "frames": [{"buttons": 0, "cx": 0, "cy": 0, ...}]}
// "buttons" uses the bits of the movie file (see Core::Movie::TasFrame). CTM Studio does not
// know about the C-stick, ZL and ZR, which are added as "csx", "csy", "zl" and "zr".
const QString ClipboardFormat = QStringLiteral("ctm-frames");

QString FramesToJson(const std::vector<TasFrame>& frames) {
    QJsonArray array;
    for (const auto& f : frames) {
        array.append(QJsonObject{
            {QStringLiteral("buttons"), f.buttons},
            {QStringLiteral("cx"), f.circle_x},
            {QStringLiteral("cy"), f.circle_y},
            {QStringLiteral("tx"), f.touch_x},
            {QStringLiteral("ty"), f.touch_y},
            {QStringLiteral("tvalid"), f.touch ? 1 : 0},
            {QStringLiteral("ax"), f.accel[0]},
            {QStringLiteral("ay"), f.accel[1]},
            {QStringLiteral("az"), f.accel[2]},
            {QStringLiteral("gx"), f.gyro[0]},
            {QStringLiteral("gy"), f.gyro[1]},
            {QStringLiteral("gz"), f.gyro[2]},
            {QStringLiteral("zl"), f.zl ? 1 : 0},
            {QStringLiteral("zr"), f.zr ? 1 : 0},
            {QStringLiteral("csx"), f.c_stick_x},
            {QStringLiteral("csy"), f.c_stick_y},
        });
    }
    const QJsonObject root{
        {QStringLiteral("format"), ClipboardFormat},
        {QStringLiteral("version"), 1},
        {QStringLiteral("savedAt"), QDateTime::currentMSecsSinceEpoch()},
        {QStringLiteral("frames"), array},
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

/// Parses frames copied by CTM Studio or this editor. Returns std::nullopt for other text.
std::optional<std::vector<TasFrame>> FramesFromJson(const QString& text) {
    const auto document = QJsonDocument::fromJson(text.toUtf8());
    const auto root = document.object();
    if (root.value(QStringLiteral("format")).toString() != ClipboardFormat ||
        !root.value(QStringLiteral("frames")).isArray()) {
        return std::nullopt;
    }
    std::vector<TasFrame> frames;
    for (const auto& value : root.value(QStringLiteral("frames")).toArray()) {
        const auto o = value.toObject();
        const auto get = [&o](const char* key) {
            return o.value(QString::fromLatin1(key)).toInt(0);
        };
        const auto s16_of = [](int v) { return static_cast<s16>(std::clamp(v, -32768, 32767)); };
        TasFrame f;
        f.buttons = static_cast<u16>(get("buttons") & 0x0FFF);
        f.circle_x = s16_of(get("cx"));
        f.circle_y = s16_of(get("cy"));
        f.touch = get("tvalid") != 0;
        f.touch_x = static_cast<u16>(std::clamp(get("tx"), 0, 319));
        f.touch_y = static_cast<u16>(std::clamp(get("ty"), 0, 239));
        f.accel = {s16_of(get("ax")), s16_of(get("ay")), s16_of(get("az"))};
        f.gyro = {s16_of(get("gx")), s16_of(get("gy")), s16_of(get("gz"))};
        f.zl = get("zl") != 0;
        f.zr = get("zr") != 0;
        f.c_stick_x = s16_of(get("csx"));
        f.c_stick_y = s16_of(get("csy"));
        frames.push_back(f);
    }
    return frames;
}

} // namespace

TasEditorModel::TasEditorModel(Core::Movie& movie_, QObject* parent)
    : QAbstractTableModel(parent), movie{movie_} {}

int TasEditorModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows;
}

int TasEditorModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

bool TasEditorModel::IsButtonColumn(int column) {
    return FindButton(column) != nullptr;
}

bool TasEditorModel::GetButton(const TasFrame& frame, int column) {
    const auto* button = FindButton(column);
    if (!button) {
        return false;
    }
    if (button->bit == -1) {
        return frame.zl;
    }
    if (button->bit == -2) {
        return frame.zr;
    }
    return (frame.buttons >> button->bit) & 1;
}

void TasEditorModel::SetButton(TasFrame& frame, int column, bool pressed) {
    const auto* button = FindButton(column);
    if (!button) {
        return;
    }
    if (button->bit == -1) {
        frame.zl = pressed;
    } else if (button->bit == -2) {
        frame.zr = pressed;
    } else if (pressed) {
        frame.buttons |= static_cast<u16>(1u << button->bit);
    } else {
        frame.buttons &= static_cast<u16>(~(1u << button->bit));
    }
}

TasFrame TasEditorModel::GetFrame(int row) const {
    if (row >= frame_count) {
        return BlankFrame();
    }
    if (const auto it = cache.find(row); it != cache.end()) {
        return it->second;
    }
    auto frame = movie.TasGetFrame(static_cast<std::size_t>(row));
    cache.emplace(row, frame);
    return frame;
}

TasFrame TasEditorModel::BlankFrame() const {
    TasFrame frame{};
    if (frame_count > 0) {
        // Keep the motion sensors where they were
        const auto last = movie.TasGetFrame(static_cast<std::size_t>(frame_count - 1));
        frame.accel = last.accel;
        frame.gyro = last.gyro;
    }
    return frame;
}

bool TasEditorModel::IsEditable(int row) const {
    return movie.IsTasEditorEnabled() && movie.GetPlayMode() == Core::Movie::PlayMode::Recording &&
           row >= first_frame;
}

bool TasEditorModel::SetFrame(int row, const TasFrame& frame) {
    if (!IsEditable(row)) {
        return false;
    }
    if (row < frame_count && GetFrame(row) == frame) {
        // Nothing changes (e.g. drawing over a button that is already set)
        return true;
    }
    if (row >= frame_count) {
        // Append frames up to the edited one
        const int start = frame_count;
        std::vector<TasFrame> frames(static_cast<std::size_t>(row - start + 1), BlankFrame());
        frames.back() = frame;
        auto before = movie.TasCopyFrames(static_cast<std::size_t>(start), 0);
        movie.TasInsertFrames(static_cast<std::size_t>(start), frames);
        Record({start, std::move(before),
                movie.TasCopyFrames(static_cast<std::size_t>(start), frames.size())});
    } else {
        auto before = movie.TasCopyFrames(static_cast<std::size_t>(row), 1);
        movie.TasSetFrame(static_cast<std::size_t>(row), frame);
        Record({row, std::move(before), movie.TasCopyFrames(static_cast<std::size_t>(row), 1)});
    }
    cache.erase(row);
    edited_cache.erase(row);
    Refresh(-1, -1);
    const QModelIndex left = index(row, 0);
    const QModelIndex right = index(row, ColumnCount - 1);
    emit dataChanged(left, right);
    return true;
}

bool TasEditorModel::InsertFrames(int row, const std::vector<TasFrame>& frames) {
    if (frames.empty() || !IsEditable(row)) {
        return false;
    }
    int start = row;
    std::vector<TasFrame> inserted;
    if (row > frame_count) {
        // Fill the gap before the inserted frames
        inserted.assign(static_cast<std::size_t>(row - frame_count), BlankFrame());
        start = frame_count;
    }
    inserted.insert(inserted.end(), frames.begin(), frames.end());
    auto before = movie.TasCopyFrames(static_cast<std::size_t>(start), 0);
    movie.TasInsertFrames(static_cast<std::size_t>(start), inserted);
    Record({start, std::move(before),
            movie.TasCopyFrames(static_cast<std::size_t>(start), inserted.size())});
    Reload();
    return true;
}

bool TasEditorModel::DeleteFrames(int row, int count) {
    if (count <= 0 || row >= frame_count || !IsEditable(row)) {
        return false;
    }
    count = std::min(count, frame_count - row);
    auto before =
        movie.TasCopyFrames(static_cast<std::size_t>(row), static_cast<std::size_t>(count));
    movie.TasDeleteFrames(static_cast<std::size_t>(row), static_cast<std::size_t>(count));
    Record({row, std::move(before), movie.TasCopyFrames(static_cast<std::size_t>(row), 0)});
    Reload();
    return true;
}

void TasEditorModel::Reload() {
    cache.clear();
    edited_cache.clear();
    Refresh(-1, -1);
    if (rows > 0) {
        emit dataChanged(index(0, 0), index(rows - 1, ColumnCount - 1));
    }
}

void TasEditorModel::CheckSession() {
    const u64 id = movie.TasSessionId();
    if (id != session_id) {
        session_id = id;
        undo_stack.clear();
        redo_stack.clear();
        pending_step.clear();
        frames_load_count = movie.TasFramesLoadCount();
        movie.TasTakeFramesLoadUndo();
    }
}

void TasEditorModel::Record(UndoChange change) {
    CheckSession();
    if (step_depth > 0) {
        pending_step.push_back(std::move(change));
    } else {
        PushStep({std::move(change)});
    }
}

void TasEditorModel::PushStep(UndoStep step) {
    undo_stack.push_back(std::move(step));
    if (undo_stack.size() > MaxUndoSteps) {
        undo_stack.erase(undo_stack.begin());
    }
    redo_stack.clear();
}

void TasEditorModel::BeginStep() {
    CheckSession();
    ++step_depth;
}

void TasEditorModel::EndStep() {
    if (step_depth == 0 || --step_depth > 0) {
        return;
    }
    if (!pending_step.empty()) {
        PushStep(std::move(pending_step));
        pending_step.clear();
    }
}

int TasEditorModel::Undo() {
    CheckSession();
    if (undo_stack.empty() || step_depth > 0 || !IsEditable(first_frame)) {
        return -1;
    }
    UndoStep step = std::move(undo_stack.back());
    undo_stack.pop_back();
    int first = std::numeric_limits<int>::max();
    for (auto it = step.rbegin(); it != step.rend(); ++it) {
        movie.TasReplaceFrames(static_cast<std::size_t>(it->start),
                               Core::Movie::TasFrameBlockSize(*it->after), *it->before);
        first = std::min(first, it->start);
    }
    redo_stack.push_back(std::move(step));
    Reload();
    return first;
}

int TasEditorModel::Redo() {
    CheckSession();
    if (redo_stack.empty() || step_depth > 0 || !IsEditable(first_frame)) {
        return -1;
    }
    UndoStep step = std::move(redo_stack.back());
    redo_stack.pop_back();
    int first = std::numeric_limits<int>::max();
    for (const auto& change : step) {
        movie.TasReplaceFrames(static_cast<std::size_t>(change.start),
                               Core::Movie::TasFrameBlockSize(*change.before), *change.after);
        first = std::min(first, change.start);
    }
    undo_stack.push_back(std::move(step));
    Reload();
    return first;
}

bool TasEditorModel::CanUndo() {
    CheckSession();
    return !undo_stack.empty();
}

bool TasEditorModel::CanRedo() {
    CheckSession();
    return !redo_stack.empty();
}

void TasEditorModel::ClearUndo() {
    undo_stack.clear();
    redo_stack.clear();
    pending_step.clear();
    step_depth = 0;
}

void TasEditorModel::Refresh(int first_visible, int last_visible) {
    // Loading a savestate of the user replaced the frames with the ones it was made with, which
    // can be undone
    CheckSession();
    if (const u64 loads = movie.TasFramesLoadCount(); loads != frames_load_count) {
        frames_load_count = loads;
        if (auto undo = movie.TasTakeFramesLoadUndo()) {
            PushStep({UndoChange{0, std::move(undo->first), std::move(undo->second)}});
        }
        cache.clear();
        edited_cache.clear();
        emit FramesLoaded();
    }

    const int new_count = static_cast<int>(movie.TasFrameCount());
    const int old_first_frame = first_frame;
    const u64 old_current_frame = current_frame;
    first_frame = static_cast<int>(movie.TasFirstFrame());
    current_frame = movie.TasCurrentFrame();

    const int new_rows = std::max(new_count, static_cast<int>(current_frame) + 1) + ExtraRows;
    if (new_rows > rows) {
        beginInsertRows({}, rows, new_rows - 1);
        rows = new_rows;
        frame_count = new_count;
        endInsertRows();
    } else if (new_rows < rows) {
        beginRemoveRows({}, new_rows, rows - 1);
        rows = new_rows;
        frame_count = new_count;
        endRemoveRows();
    }
    frame_count = new_count;

    if (first_visible < 0) {
        return;
    }
    const auto old_cache = std::exchange(cache, {});
    const auto old_edited_cache = std::exchange(edited_cache, {});
    const auto old_state_cache = std::exchange(state_cache, {});
    const auto frames =
        movie.TasGetFrames(static_cast<std::size_t>(first_visible),
                           static_cast<std::size_t>(last_visible - first_visible + 1));
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const int row = first_visible + static_cast<int>(i);
        cache.emplace(row, frames[i]);
        edited_cache.emplace(row, movie.TasIsFrameEdited(static_cast<std::size_t>(row)));
    }
    for (int row = first_visible; row <= last_visible; ++row) {
        if (movie.TasHasState(static_cast<u64>(row))) {
            state_cache.insert(row);
        }
    }

    // Only repaint the rows that changed, repainting all of them several times per second is slow
    if (first_frame != old_first_frame) {
        emit dataChanged(index(first_visible, 0), index(last_visible, ColumnCount - 1));
        return;
    }
    const auto changed = [&](int row) {
        // Rows between the old and new current frame change color
        const u64 r = static_cast<u64>(row);
        if (r >= std::min(old_current_frame, current_frame) &&
            r <= std::max(old_current_frame, current_frame)) {
            return true;
        }
        const auto old_it = old_cache.find(row);
        const auto new_it = cache.find(row);
        if ((old_it == old_cache.end()) != (new_it == cache.end()) ||
            (new_it != cache.end() && old_it->second != new_it->second)) {
            return true;
        }
        const auto old_edited = old_edited_cache.find(row);
        const auto new_edited = edited_cache.find(row);
        if ((old_edited == old_edited_cache.end() ? false : old_edited->second) !=
            (new_edited == edited_cache.end() ? false : new_edited->second)) {
            return true;
        }
        return old_state_cache.contains(row) != state_cache.contains(row);
    };
    int first_changed = -1;
    int last_changed = -1;
    for (int row = first_visible; row <= last_visible; ++row) {
        if (changed(row)) {
            if (first_changed < 0) {
                first_changed = row;
            }
            last_changed = row;
        }
    }
    if (first_changed >= 0) {
        emit dataChanged(index(first_changed, 0), index(last_changed, ColumnCount - 1));
    }
}

QVariant TasEditorModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const int row = index.row();
    const int column = index.column();
    const bool exists = row < frame_count;

    if (role == Qt::TextAlignmentRole) {
        return Qt::AlignCenter;
    }

    if (role == Qt::BackgroundRole) {
        if (exists && IsButtonColumn(column) && GetButton(GetFrame(row), column)) {
            return QColor(240, 170, 60);
        }
        if (static_cast<u64>(row) == current_frame) {
            return CurrentFrameColor;
        }
        if (row < first_frame) {
            return QColor(128, 128, 128, 90);
        }
        if (column == ColumnFrame && state_cache.contains(row)) {
            return QColor(60, 170, 60, 140);
        }
        if (static_cast<u64>(row) < current_frame) {
            return QColor(90, 200, 90, 50);
        }
        return {};
    }

    if (role == Qt::ForegroundRole && column == ColumnFrame) {
        const auto it = edited_cache.find(row);
        if (it != edited_cache.end() && it->second) {
            return QColor(230, 120, 20);
        }
        return {};
    }

    if (role == Qt::ToolTipRole && column == ColumnFrame) {
        return tr("Double click to go to this frame");
    }

    if (role != Qt::DisplayRole && role != Qt::EditRole) {
        return {};
    }

    if (column == ColumnFrame) {
        return row;
    }
    if (!exists) {
        return role == Qt::EditRole ? QVariant{0} : QVariant{};
    }

    const TasFrame frame = GetFrame(row);
    if (const auto* button = FindButton(column)) {
        return GetButton(frame, column) ? QString::fromUtf8(button->name) : QString{};
    }
    switch (column) {
    case ColumnCircleX:
        return frame.circle_x;
    case ColumnCircleY:
        return frame.circle_y;
    case ColumnCStickX:
        return frame.c_stick_x;
    case ColumnCStickY:
        return frame.c_stick_y;
    case ColumnTouch:
        return frame.touch ? QStringLiteral("%1, %2").arg(frame.touch_x).arg(frame.touch_y)
                           : QString{};
    case ColumnAccel:
        return FormatVector(frame.accel);
    case ColumnGyro:
        return FormatVector(frame.gyro);
    default:
        return {};
    }
}

bool TasEditorModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || role != Qt::EditRole || !IsEditable(index.row())) {
        return false;
    }
    const int row = index.row();
    TasFrame frame = GetFrame(row);
    const auto clamp_stick = [](int v) { return static_cast<s16>(std::clamp(v, -0x9C, 0x9C)); };

    switch (index.column()) {
    case ColumnCircleX:
        frame.circle_x = clamp_stick(value.toInt());
        break;
    case ColumnCircleY:
        frame.circle_y = clamp_stick(value.toInt());
        break;
    case ColumnCStickX:
        frame.c_stick_x = clamp_stick(value.toInt());
        break;
    case ColumnCStickY:
        frame.c_stick_y = clamp_stick(value.toInt());
        break;
    case ColumnTouch: {
        const auto numbers = ParseNumbers(value.toString());
        if (numbers.size() == 2) {
            frame.touch = true;
            frame.touch_x = static_cast<u16>(std::clamp(numbers[0], 0, 319));
            frame.touch_y = static_cast<u16>(std::clamp(numbers[1], 0, 239));
        } else if (value.toString().trimmed().isEmpty()) {
            frame.touch = false;
        } else {
            return false;
        }
        break;
    }
    case ColumnAccel:
    case ColumnGyro: {
        const auto numbers = ParseNumbers(value.toString());
        if (numbers.size() != 3) {
            return false;
        }
        auto& v = index.column() == ColumnAccel ? frame.accel : frame.gyro;
        for (std::size_t i = 0; i < 3; ++i) {
            v[i] = static_cast<s16>(std::clamp(numbers[i], -32768, 32767));
        }
        break;
    }
    default:
        return false;
    }
    if (!SetFrame(row, frame)) {
        return false;
    }
    emit FramesEdited(row);
    return true;
}

QVariant TasEditorModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    if (const auto* button = FindButton(section)) {
        return QString::fromUtf8(button->name);
    }
    switch (section) {
    case ColumnFrame:
        return tr("Frame");
    case ColumnCircleX:
        return tr("Pad X");
    case ColumnCircleY:
        return tr("Pad Y");
    case ColumnCStickX:
        return tr("C X");
    case ColumnCStickY:
        return tr("C Y");
    case ColumnTouch:
        return tr("Touch");
    case ColumnAccel:
        return tr("Accel");
    case ColumnGyro:
        return tr("Gyro");
    default:
        return {};
    }
}

Qt::ItemFlags TasEditorModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() >= ColumnCircleX && IsEditable(index.row())) {
        flags |= Qt::ItemIsEditable;
    }
    return flags;
}

namespace {

/// Draws an outline around the row of the current frame, so it stays visible when selected
class CurrentFrameDelegate : public QStyledItemDelegate {
public:
    CurrentFrameDelegate(TasEditorModel* model_, QObject* parent)
        : QStyledItemDelegate(parent), model{model_} {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (static_cast<u64>(index.row()) != model->CurrentFrame()) {
            return;
        }
        painter->save();
        painter->setPen(QPen(CurrentFrameOutline, 2));
        const QRect rect = option.rect.adjusted(0, 1, 0, -1);
        painter->drawLine(rect.topLeft(), rect.topRight());
        painter->drawLine(rect.bottomLeft(), rect.bottomRight());
        if (index.column() == 0) {
            painter->drawLine(rect.topLeft(), rect.bottomLeft());
        }
        if (index.column() == TasEditorModel::ColumnCount - 1) {
            painter->drawLine(rect.topRight(), rect.bottomRight());
        }
        painter->restore();
    }

private:
    TasEditorModel* model;
};

} // namespace

TasEditorWidget::TasEditorWidget(Core::System& system_, QWidget* parent)
    : QDockWidget(tr("TAS Editor"), parent), system{system_} {
    setObjectName(QStringLiteral("TasEditorWidget"));

    auto* contents = new QWidget(this);
    auto* layout = new QVBoxLayout(contents);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* top_row = new QHBoxLayout();
    enable_check = new QCheckBox(tr("Enable TAS editor"), contents);
    enable_check->setToolTip(
        tr("Enable before recording or playing a movie, so that the whole movie can be edited. "
           "Playing a movie captures it into the editor as fast as possible."));
    connect(enable_check, &QCheckBox::toggled, this, &TasEditorWidget::SetEnabled);
    top_row->addWidget(enable_check);

    follow_check = new QCheckBox(tr("Follow cursor"), contents);
    follow_check->setChecked(true);
    top_row->addWidget(follow_check);

    overwrite_check = new QCheckBox(tr("Record over frames"), contents);
    overwrite_check->setToolTip(tr("Record the live input over frames that already have inputs, "
                                   "instead of playing their inputs back"));
    connect(overwrite_check, &QCheckBox::toggled, this,
            [this](bool checked) { system.Movie().TasSetOverwrite(checked); });
    top_row->addWidget(overwrite_check);
    top_row->addStretch();

    save_button = new QPushButton(tr("Save Movie As..."), contents);
    connect(save_button, &QPushButton::clicked, this, &TasEditorWidget::SaveMovie);
    top_row->addWidget(save_button);
    layout->addLayout(top_row);

    auto* state_row = new QHBoxLayout();
    state_row->addWidget(new QLabel(tr("Savestate every"), contents));
    interval_spin = new QSpinBox(contents);
    interval_spin->setRange(1, 3600);
    interval_spin->setValue(static_cast<int>(UISettings::values.tas_state_interval.GetValue()));
    interval_spin->setSuffix(tr(" frames"));
    connect(interval_spin, &QSpinBox::valueChanged, this, [this](int value) {
        UISettings::values.tas_state_interval = static_cast<u32>(value);
        system.Movie().TasSetStateInterval(static_cast<u32>(value));
    });
    state_row->addWidget(interval_spin);
    state_row->addWidget(new QLabel(tr("keep at most"), contents));
    capacity_spin = new QSpinBox(contents);
    capacity_spin->setRange(2, 10000);
    capacity_spin->setValue(static_cast<int>(UISettings::values.tas_state_capacity.GetValue()));
    capacity_spin->setSuffix(tr(" states"));
    connect(capacity_spin, &QSpinBox::valueChanged, this, [this](int value) {
        UISettings::values.tas_state_capacity = static_cast<u32>(value);
        system.Movie().TasSetStateCapacity(static_cast<u32>(value));
    });
    state_row->addWidget(capacity_spin);
    state_row->addStretch();

    state_row->addWidget(new QLabel(tr("Go to frame"), contents));
    goto_spin = new QSpinBox(contents);
    goto_spin->setRange(0, std::numeric_limits<int>::max());
    goto_spin->setToolTip(tr("Scrolls to and selects a frame (Ctrl+G). Double click a frame "
                             "number to make the emulator go to that frame."));
    state_row->addWidget(goto_spin);
    auto* goto_button = new QPushButton(tr("Go"), contents);
    state_row->addWidget(goto_button);
    connect(goto_button, &QPushButton::clicked, this, [this] { GoToFrame(goto_spin->value()); });
    connect(goto_spin, &QSpinBox::editingFinished, this, [this] {
        // Enter in the field (not just leaving it)
        if (goto_spin->hasFocus()) {
            GoToFrame(goto_spin->value());
        }
    });
    layout->addLayout(state_row);

    model = new TasEditorModel(system.Movie(), this);
    connect(model, &TasEditorModel::FramesEdited, this, &TasEditorWidget::OnFramesEdited);

    view = new QTableView(contents);
    view->setModel(model);
    // Cells are selected individually, rows by clicking or dragging on the frame column
    view->setSelectionBehavior(QAbstractItemView::SelectItems);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    view->verticalHeader()->setVisible(false);
    view->verticalHeader()->setDefaultSectionSize(view->fontMetrics().height() + 4);
    // Not QHeaderView::ResizeToContents: measuring the rows whenever the table changes (several
    // times per second while emulating) is slow enough to make emulation choppy
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setMinimumSectionSize(24);
    SizeColumns();
    view->setItemDelegate(new CurrentFrameDelegate(model, view));
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QTableView::customContextMenuRequested, this, &TasEditorWidget::ShowContextMenu);
    view->installEventFilter(this);
    view->viewport()->installEventFilter(this);
    layout->addWidget(view, 1);

    status_label = new QLabel(contents);
    layout->addWidget(status_label);

    auto* help = new QLabel(
        tr("Click or drag on buttons to toggle them, on frame numbers to select frames, and on "
           "values to select them. Double click a value to type it, or a frame number to go to "
           "that frame. Right click for copy, paste, insert, delete, clear, undo and redo."),
        contents);
    help->setWordWrap(true);
    layout->addWidget(help);

    setWidget(contents);

    notice_timer.setSingleShot(true);
    notice_timer.setInterval(6000);
    connect(&notice_timer, &QTimer::timeout, this, [this] { notice.clear(); });
    connect(model, &TasEditorModel::FramesLoaded, this, [this] {
        notice = tr("Loaded the inputs the savestate was made with (Ctrl+Z to undo).");
        notice_timer.start();
    });

    refresh_timer.setInterval(50);
    connect(&refresh_timer, &QTimer::timeout, this, &TasEditorWidget::Refresh);
    restore_timer.setSingleShot(true);
    restore_timer.setInterval(150);
    connect(&restore_timer, &QTimer::timeout, this, &TasEditorWidget::RestorePosition);
}

TasEditorWidget::~TasEditorWidget() = default;

void TasEditorWidget::SizeColumns() {
    const QFontMetrics metrics = view->fontMetrics();
    const QFontMetrics header_metrics = view->horizontalHeader()->fontMetrics();
    const int padding = metrics.horizontalAdvance(QStringLiteral("00"));
    for (int column = 0; column < TasEditorModel::ColumnCount; ++column) {
        QString widest;
        switch (column) {
        case TasEditorModel::ColumnFrame:
            widest = QStringLiteral("0000000");
            break;
        case TasEditorModel::ColumnCircleX:
        case TasEditorModel::ColumnCircleY:
        case TasEditorModel::ColumnCStickX:
        case TasEditorModel::ColumnCStickY:
            widest = QStringLiteral("-000");
            break;
        case TasEditorModel::ColumnTouch:
            widest = QStringLiteral("000, 000");
            break;
        case TasEditorModel::ColumnAccel:
        case TasEditorModel::ColumnGyro:
            widest = FormatVector({-32768, -32768, -32768});
            break;
        default:
            // Buttons show their name, like the header
            break;
        }
        const QString header =
            model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
        const int width =
            std::max(metrics.horizontalAdvance(widest), header_metrics.horizontalAdvance(header)) +
            padding;
        view->horizontalHeader()->resizeSection(column, width);
    }
}

void TasEditorWidget::OnEmulationStarting(EmuThread*) {
    model->ClearUndo();
    restore_frame.reset();
    save_after_frame.reset();
    system.Movie().TasSetStateInterval(static_cast<u32>(interval_spin->value()));
    system.Movie().TasSetStateCapacity(static_cast<u32>(capacity_spin->value()));
    system.Movie().TasSetOverwrite(overwrite_check->isChecked());
}

void TasEditorWidget::OnEmulationStopping() {
    model->ClearUndo();
    restore_frame.reset();
    save_after_frame.reset();
}

void TasEditorWidget::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    refresh_timer.start();
}

void TasEditorWidget::hideEvent(QHideEvent* event) {
    QDockWidget::hideEvent(event);
    refresh_timer.stop();
}

void TasEditorWidget::SetEnabled(bool enabled) {
    auto& movie = system.Movie();
    movie.EnableTasEditor(enabled);
    model->ClearUndo();
    if (enabled) {
        movie.TasSetStateInterval(static_cast<u32>(interval_spin->value()));
        movie.TasSetStateCapacity(static_cast<u32>(capacity_spin->value()));
        movie.TasSetOverwrite(overwrite_check->isChecked());
    }
    Refresh();
}

void TasEditorWidget::Refresh() {
    const int first = std::max(view->rowAt(0), 0);
    int last = view->rowAt(view->viewport()->height() - 1);
    if (last < 0) {
        last = std::min(first + 60, model->rowCount() - 1);
    }
    model->Refresh(first, std::max(first, last));

    auto& movie = system.Movie();
    const u64 current = model->CurrentFrame();
    if (follow_check->isChecked() && current != last_current_frame && !drawing) {
        view->scrollTo(model->index(static_cast<int>(current), 0),
                       QAbstractItemView::PositionAtCenter);
    }
    last_current_frame = current;

    // Save once the end of the movie was reached
    if (save_after_frame && !movie.TasIsSeeking() && current >= *save_after_frame) {
        save_after_frame.reset();
        movie.SaveMovie();
        QMessageBox::information(this, tr("TAS Editor"), tr("The movie was saved."));
    }

    QString status;
    if (!movie.IsTasEditorEnabled()) {
        status = tr("The TAS editor is disabled.");
    } else if (movie.GetPlayMode() == Core::Movie::PlayMode::Playing) {
        status = tr("Capturing the movie... frame %1").arg(current);
    } else if (movie.GetPlayMode() != Core::Movie::PlayMode::Recording) {
        status = tr("Record or play a movie to edit it.");
    } else {
        status = tr("Frame %1 of %2 | %3 savestates (%4 MB)%5")
                     .arg(current)
                     .arg(model->FrameCount())
                     .arg(movie.TasStateCount())
                     .arg(movie.TasStateMemoryUsage() / (1024 * 1024))
                     .arg(movie.TasIsSeeking() ? tr(" | Seeking...") : QString{});
    }
    status_label->setText(notice.isEmpty() ? status : notice);
    save_button->setEnabled(movie.IsTasEditorEnabled() &&
                            movie.GetPlayMode() == Core::Movie::PlayMode::Recording);
}

void TasEditorWidget::Seek(u64 frame) {
    auto& movie = system.Movie();
    if (!system.IsPoweredOn() || movie.GetPlayMode() != Core::Movie::PlayMode::Recording) {
        return;
    }
    if (frame == model->CurrentFrame() && system.frame_limiter.IsFrameAdvancing() &&
        !movie.TasIsSeeking()) {
        return;
    }
    movie.TasRequestSeek(frame);
    // Wake up the emulator if it is paused, it pauses again once it reaches the frame
    system.frame_limiter.SetFrameAdvancing(false);
}

void TasEditorWidget::OnFramesEdited(int first_row) {
    // Emulate the edited frames again if they were already emulated, then come back
    const u64 current = model->CurrentFrame();
    if (static_cast<u64>(first_row) < current) {
        if (!restore_frame) {
            restore_frame = current;
        }
        restore_timer.start();
    }
}

void TasEditorWidget::RestorePosition() {
    if (drawing) {
        restore_timer.start();
        return;
    }
    if (restore_frame) {
        Seek(*restore_frame);
        restore_frame.reset();
    }
}

std::vector<int> TasEditorWidget::SelectedRows() const {
    std::vector<int> rows;
    for (const auto& index : view->selectionModel()->selectedIndexes()) {
        rows.push_back(index.row());
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

void TasEditorWidget::CopySelection() {
    const auto rows = SelectedRows();
    if (rows.empty()) {
        return;
    }
    // Whole frames are copied, as that is what CTM Studio and pasting work with
    clipboard.clear();
    for (const int row : rows) {
        clipboard.push_back(model->GetFrame(row));
    }
    QApplication::clipboard()->setText(FramesToJson(clipboard));
}

void TasEditorWidget::Paste(bool insert) {
    // Frames copied in CTM Studio (or another Azahar) take priority over the internal clipboard
    if (auto frames = FramesFromJson(QApplication::clipboard()->text())) {
        clipboard = std::move(*frames);
    }
    auto rows = SelectedRows();
    if (rows.empty() && view->currentIndex().isValid()) {
        rows.push_back(view->currentIndex().row());
    }
    auto& movie = system.Movie();
    QString error;
    if (clipboard.empty()) {
        error = tr("There are no frames to paste. Copy frames in this editor or in CTM Studio "
                   "first.");
    } else if (!movie.IsTasEditorEnabled() ||
               movie.GetPlayMode() != Core::Movie::PlayMode::Recording) {
        error = tr("Frames can only be pasted while a movie is being recorded, with the TAS "
                   "editor enabled.");
    } else if (rows.empty()) {
        error = tr("Select the frame to paste at first.");
    } else if (!model->IsEditable(rows.front())) {
        error = tr("Frames can't be pasted before frame %1, which was emulated before the TAS "
                   "editor was enabled.")
                    .arg(movie.TasFirstFrame());
    }
    if (!error.isEmpty()) {
        QMessageBox::information(this, tr("TAS Editor"), error);
        return;
    }
    const int start = rows.front();
    if (insert) {
        model->InsertFrames(start, clipboard);
    } else {
        model->BeginStep();
        for (std::size_t i = 0; i < clipboard.size(); ++i) {
            model->SetFrame(start + static_cast<int>(i), clipboard[i]);
        }
        model->EndStep();
    }
    // Select the pasted frames
    const int last = start + static_cast<int>(clipboard.size()) - 1;
    view->selectionModel()->select(
        QItemSelection(model->index(start, 0), model->index(std::min(last, model->rowCount() - 1),
                                                            TasEditorModel::ColumnCount - 1)),
        QItemSelectionModel::ClearAndSelect);
    OnFramesEdited(start);
}

void TasEditorWidget::InsertBlank() {
    const auto rows = SelectedRows();
    if (rows.empty() || !model->IsEditable(rows.front()) || rows.front() > model->FrameCount()) {
        return;
    }
    std::vector<TasFrame> frames(rows.size(), model->BlankFrame());
    model->InsertFrames(rows.front(), frames);
    OnFramesEdited(rows.front());
}

void TasEditorWidget::DeleteFrames() {
    const auto rows = SelectedRows();
    if (rows.empty() || !model->IsEditable(rows.front())) {
        return;
    }
    model->BeginStep();
    // Delete from the bottom so the indices of the remaining rows stay valid
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        if (*it < model->FrameCount()) {
            model->DeleteFrames(*it, 1);
        }
    }
    model->EndStep();
    OnFramesEdited(rows.front());
}

namespace {

/// Clears all the inputs of a frame, keeping the motion sensors where they were
TasFrame ClearedFrame(const TasFrame& frame) {
    TasFrame cleared{};
    cleared.accel = frame.accel;
    cleared.gyro = frame.gyro;
    return cleared;
}

} // namespace

void TasEditorWidget::ClearSelectedInputs() {
    std::map<int, std::vector<int>> columns_of_row;
    for (const auto& index : view->selectionModel()->selectedIndexes()) {
        columns_of_row[index.row()].push_back(index.column());
    }
    if (columns_of_row.empty() || !model->IsEditable(columns_of_row.begin()->first)) {
        return;
    }
    model->BeginStep();
    for (const auto& [row, columns] : columns_of_row) {
        if (row >= model->FrameCount()) {
            break;
        }
        TasFrame frame = model->GetFrame(row);
        const bool whole_row =
            static_cast<int>(columns.size()) == TasEditorModel::ColumnCount ||
            std::find(columns.begin(), columns.end(), TasEditorModel::ColumnFrame) != columns.end();
        if (whole_row) {
            frame = ClearedFrame(frame);
        } else {
            for (const int column : columns) {
                if (TasEditorModel::IsButtonColumn(column)) {
                    TasEditorModel::SetButton(frame, column, false);
                    continue;
                }
                switch (column) {
                case TasEditorModel::ColumnCircleX:
                    frame.circle_x = 0;
                    break;
                case TasEditorModel::ColumnCircleY:
                    frame.circle_y = 0;
                    break;
                case TasEditorModel::ColumnCStickX:
                    frame.c_stick_x = 0;
                    break;
                case TasEditorModel::ColumnCStickY:
                    frame.c_stick_y = 0;
                    break;
                case TasEditorModel::ColumnTouch:
                    frame.touch = false;
                    frame.touch_x = 0;
                    frame.touch_y = 0;
                    break;
                case TasEditorModel::ColumnAccel:
                    frame.accel = {};
                    break;
                case TasEditorModel::ColumnGyro:
                    frame.gyro = {};
                    break;
                default:
                    break;
                }
            }
        }
        model->SetFrame(row, frame);
    }
    model->EndStep();
    OnFramesEdited(columns_of_row.begin()->first);
}

void TasEditorWidget::ClearFrames() {
    const auto rows = SelectedRows();
    if (rows.empty() || !model->IsEditable(rows.front())) {
        return;
    }
    model->BeginStep();
    for (const int row : rows) {
        if (row >= model->FrameCount()) {
            break;
        }
        model->SetFrame(row, ClearedFrame(model->GetFrame(row)));
    }
    model->EndStep();
    OnFramesEdited(rows.front());
}

void TasEditorWidget::Undo() {
    const int row = model->Undo();
    if (row >= 0) {
        OnFramesEdited(row);
    }
}

void TasEditorWidget::Redo() {
    const int row = model->Redo();
    if (row >= 0) {
        OnFramesEdited(row);
    }
}

void TasEditorWidget::GoToFrame(int frame) {
    model->Refresh(-1, -1);
    const int row = std::clamp(frame, 0, model->rowCount() - 1);
    const QModelIndex index = model->index(row, TasEditorModel::ColumnFrame);
    view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
    view->scrollTo(index, QAbstractItemView::PositionAtCenter);
    row_anchor = row;
    view->setFocus(Qt::OtherFocusReason);
}

void TasEditorWidget::SelectRowRange(int row) {
    row = std::clamp(row, 0, model->rowCount() - 1);
    const int first = std::min(row_anchor, row);
    const int last = std::max(row_anchor, row);
    QItemSelection selection = row_drag_base;
    selection.merge(
        QItemSelection(model->index(first, 0), model->index(last, TasEditorModel::ColumnCount - 1)),
        QItemSelectionModel::Select);
    auto* selection_model = view->selectionModel();
    selection_model->select(selection, QItemSelectionModel::ClearAndSelect);
    selection_model->setCurrentIndex(model->index(row, TasEditorModel::ColumnFrame),
                                     QItemSelectionModel::NoUpdate);
}

void TasEditorWidget::SaveMovie() {
    auto& movie = system.Movie();
    if (movie.GetPlayMode() != Core::Movie::PlayMode::Recording) {
        return;
    }
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Save Movie"), {}, tr("Citra TAS Movie (*.ctm)"));
    if (path.isEmpty()) {
        return;
    }
    movie.SetMovieFile(path.toStdString());

    // The movie file contains the inputs up to the frame emulated last, so emulate the remaining
    // frames of the editor first
    const u64 end = static_cast<u64>(model->FrameCount());
    if (model->CurrentFrame() < end) {
        save_after_frame = end;
        Seek(end);
    } else {
        movie.SaveMovie();
        QMessageBox::information(this, tr("TAS Editor"), tr("The movie was saved."));
    }
}

void TasEditorWidget::ShowContextMenu(const QPoint& pos) {
    QMenu menu(this);
    const QModelIndex index = view->indexAt(pos);
    if (index.isValid()) {
        menu.addAction(tr("Go to Frame %1").arg(index.row()),
                       [this, row = index.row()] { Seek(static_cast<u64>(row)); });
        menu.addSeparator();
    }
    menu.addAction(tr("Undo\tCtrl+Z"), this, &TasEditorWidget::Undo)->setEnabled(model->CanUndo());
    menu.addAction(tr("Redo\tCtrl+Y"), this, &TasEditorWidget::Redo)->setEnabled(model->CanRedo());
    menu.addSeparator();
    menu.addAction(tr("Copy\tCtrl+C"), this, &TasEditorWidget::CopySelection);
    const bool can_paste =
        !clipboard.empty() || FramesFromJson(QApplication::clipboard()->text()).has_value();
    menu.addAction(tr("Paste\tCtrl+V"), this, [this] { Paste(false); })->setEnabled(can_paste);
    menu.addAction(tr("Paste Insert\tCtrl+Shift+V"), this, [this] { Paste(true); })
        ->setEnabled(can_paste);
    menu.addSeparator();
    menu.addAction(tr("Insert Blank Frames\tInsert"), this, &TasEditorWidget::InsertBlank);
    menu.addAction(tr("Delete Frames\tCtrl+Delete"), this, &TasEditorWidget::DeleteFrames);
    menu.addAction(tr("Clear Selected Inputs\tDelete"), this,
                   &TasEditorWidget::ClearSelectedInputs);
    menu.addAction(tr("Clear Frames\tShift+Delete"), this, &TasEditorWidget::ClearFrames);
    menu.exec(view->viewport()->mapToGlobal(pos));
}

bool TasEditorWidget::eventFilter(QObject* object, QEvent* event) {
    // Keyboard shortcuts of the table. They are handled here (and ShortcutOverride accepted) so
    // they take priority over the application wide hotkeys using the same keys.
    if (object == view &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        const auto combination = key_event->keyCombination();
        const auto is = [&combination](Qt::KeyboardModifiers modifiers, Qt::Key key) {
            return combination == QKeyCombination(modifiers, key);
        };
        std::function<void()> action;
        if (is(Qt::ControlModifier, Qt::Key_C)) {
            action = [this] { CopySelection(); };
        } else if (is(Qt::ControlModifier, Qt::Key_V)) {
            action = [this] { Paste(false); };
        } else if (is(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_V)) {
            action = [this] { Paste(true); };
        } else if (is(Qt::ControlModifier, Qt::Key_Z)) {
            action = [this] { Undo(); };
        } else if (is(Qt::ControlModifier, Qt::Key_Y) ||
                   is(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Z)) {
            action = [this] { Redo(); };
        } else if (is(Qt::ControlModifier, Qt::Key_G)) {
            action = [this] {
                goto_spin->setFocus(Qt::ShortcutFocusReason);
                goto_spin->selectAll();
            };
        } else if (is(Qt::NoModifier, Qt::Key_Insert)) {
            action = [this] { InsertBlank(); };
        } else if (is(Qt::ControlModifier, Qt::Key_Delete)) {
            action = [this] { DeleteFrames(); };
        } else if (is(Qt::ShiftModifier, Qt::Key_Delete)) {
            action = [this] { ClearFrames(); };
        } else if (is(Qt::NoModifier, Qt::Key_Delete)) {
            action = [this] { ClearSelectedInputs(); };
        }
        const bool editing = view->indexWidget(view->currentIndex()) != nullptr;
        if (action && !editing) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
            } else {
                action();
            }
            return true;
        }
    }

    if (object != view->viewport()) {
        return QDockWidget::eventFilter(object, event);
    }
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick &&
        type != QEvent::MouseMove && type != QEvent::MouseButtonRelease) {
        return QDockWidget::eventFilter(object, event);
    }
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    const QPoint pos = mouse_event->position().toPoint();

    if ((type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) &&
        mouse_event->button() == Qt::LeftButton) {
        const QModelIndex index = view->indexAt(pos);
        if (!index.isValid()) {
            return QDockWidget::eventFilter(object, event);
        }

        // Frame column: select whole rows, double click goes to the frame
        if (index.column() == TasEditorModel::ColumnFrame) {
            if (type == QEvent::MouseButtonDblClick) {
                Seek(static_cast<u64>(index.row()));
                return true;
            }
            view->setFocus(Qt::MouseFocusReason);
            const auto modifiers = mouse_event->modifiers();
            if (!(modifiers & Qt::ShiftModifier) || row_anchor < 0) {
                row_anchor = index.row();
            }
            row_drag_base = (modifiers & Qt::ControlModifier) ? view->selectionModel()->selection()
                                                              : QItemSelection{};
            SelectRowRange(index.row());
            row_dragging = true;
            return true;
        }

        // Button columns: toggle the button, dragging sets the other frames the same way
        if (TasEditorModel::IsButtonColumn(index.column()) &&
            mouse_event->modifiers() == Qt::NoModifier && model->IsEditable(index.row())) {
            // The click is not passed to the table, select the cell and take the focus here so
            // the keyboard shortcuts (e.g. paste) apply to it
            view->setFocus(Qt::MouseFocusReason);
            view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
            row_anchor = index.row();
            TasFrame frame = model->GetFrame(index.row());
            draw_value = !TasEditorModel::GetButton(frame, index.column());
            TasEditorModel::SetButton(frame, index.column(), draw_value);
            model->BeginStep();
            model->SetFrame(index.row(), frame);
            drawing = true;
            draw_column = index.column();
            draw_last_row = index.row();
            draw_first_row = index.row();
            return true;
        }
        // Value columns: normal cell selection
        row_anchor = index.row();
        return QDockWidget::eventFilter(object, event);
    }

    if (type == QEvent::MouseMove && (drawing || row_dragging)) {
        // Scroll when dragging past the top or bottom
        auto* scroll_bar = view->verticalScrollBar();
        if (pos.y() < 0) {
            scroll_bar->setValue(scroll_bar->value() - 1);
        } else if (pos.y() >= view->viewport()->height()) {
            scroll_bar->setValue(scroll_bar->value() + 1);
        }
        const int y = std::clamp(pos.y(), 0, view->viewport()->height() - 1);
        const int row = view->rowAt(y);
        if (row < 0) {
            return true;
        }
        if (row_dragging) {
            SelectRowRange(row);
            return true;
        }
        if (row != draw_last_row) {
            const int step = row > draw_last_row ? 1 : -1;
            for (int r = draw_last_row + step; r != row + step; r += step) {
                if (!model->IsEditable(r)) {
                    continue;
                }
                TasFrame frame = model->GetFrame(r);
                TasEditorModel::SetButton(frame, draw_column, draw_value);
                model->SetFrame(r, frame);
                draw_first_row = std::min(draw_first_row, r);
            }
            draw_last_row = row;
        }
        return true;
    }

    if (type == QEvent::MouseButtonRelease && mouse_event->button() == Qt::LeftButton) {
        if (row_dragging) {
            row_dragging = false;
            return true;
        }
        if (drawing) {
            drawing = false;
            model->EndStep();
            OnFramesEdited(draw_first_row);
            return true;
        }
    }
    return QDockWidget::eventFilter(object, event);
}
