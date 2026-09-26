// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <functional>
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
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>
#include "citra_qt/debugger/tas_editor.h"
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
    if (row >= frame_count) {
        // Append frames up to the edited one
        std::vector<TasFrame> frames(static_cast<std::size_t>(row - frame_count + 1), BlankFrame());
        frames.back() = frame;
        movie.TasInsertFrames(static_cast<std::size_t>(frame_count), frames);
    } else {
        movie.TasSetFrame(static_cast<std::size_t>(row), frame);
    }
    cache.erase(row);
    edited_cache.erase(row);
    Refresh(-1, -1);
    const QModelIndex left = index(row, 0);
    const QModelIndex right = index(row, ColumnCount - 1);
    emit dataChanged(left, right);
    return true;
}

void TasEditorModel::Refresh(int first_visible, int last_visible) {
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
            return QColor(90, 140, 240, 140);
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
    interval_spin->setValue(60);
    interval_spin->setSuffix(tr(" frames"));
    connect(interval_spin, &QSpinBox::valueChanged, this,
            [this](int value) { system.Movie().TasSetStateInterval(static_cast<u32>(value)); });
    state_row->addWidget(interval_spin);
    state_row->addWidget(new QLabel(tr("keep at most"), contents));
    capacity_spin = new QSpinBox(contents);
    capacity_spin->setRange(2, 10000);
    capacity_spin->setValue(60);
    capacity_spin->setSuffix(tr(" states"));
    connect(capacity_spin, &QSpinBox::valueChanged, this,
            [this](int value) { system.Movie().TasSetStateCapacity(static_cast<u32>(value)); });
    state_row->addWidget(capacity_spin);
    state_row->addStretch();
    layout->addLayout(state_row);

    model = new TasEditorModel(system.Movie(), this);
    connect(model, &TasEditorModel::FramesEdited, this, &TasEditorWidget::OnFramesEdited);

    view = new QTableView(contents);
    view->setModel(model);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    view->verticalHeader()->setVisible(false);
    view->verticalHeader()->setDefaultSectionSize(view->fontMetrics().height() + 4);
    // Not QHeaderView::ResizeToContents: measuring the rows whenever the table changes (several
    // times per second while emulating) is slow enough to make emulation choppy
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setMinimumSectionSize(24);
    SizeColumns();
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QTableView::customContextMenuRequested, this, &TasEditorWidget::ShowContextMenu);
    connect(view, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() == TasEditorModel::ColumnFrame) {
            Seek(static_cast<u64>(index.row()));
        }
    });
    view->installEventFilter(this);
    view->viewport()->installEventFilter(this);
    layout->addWidget(view, 1);

    status_label = new QLabel(contents);
    layout->addWidget(status_label);

    auto* help = new QLabel(
        tr("Click or drag on buttons to toggle them. Double click a value to type it, or a frame "
           "number to go to that frame. Right click for copy, paste, insert and delete."),
        contents);
    help->setWordWrap(true);
    layout->addWidget(help);

    setWidget(contents);

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
    restore_frame.reset();
    save_after_frame.reset();
    system.Movie().TasSetStateInterval(static_cast<u32>(interval_spin->value()));
    system.Movie().TasSetStateCapacity(static_cast<u32>(capacity_spin->value()));
    system.Movie().TasSetOverwrite(overwrite_check->isChecked());
}

void TasEditorWidget::OnEmulationStopping() {
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
    status_label->setText(status);
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
    for (const auto& index : view->selectionModel()->selectedRows()) {
        rows.push_back(index.row());
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

void TasEditorWidget::CopySelection() {
    const auto rows = SelectedRows();
    if (rows.empty()) {
        return;
    }
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
    const auto rows = SelectedRows();
    if (clipboard.empty() || rows.empty() || !model->IsEditable(rows.front())) {
        return;
    }
    const int start = rows.front();
    auto& movie = system.Movie();
    if (insert) {
        if (start > model->FrameCount()) {
            // Fill the gap before the pasted frames
            std::vector<TasFrame> gap(static_cast<std::size_t>(start - model->FrameCount()),
                                      model->BlankFrame());
            movie.TasInsertFrames(static_cast<std::size_t>(model->FrameCount()), gap);
        }
        movie.TasInsertFrames(static_cast<std::size_t>(start), clipboard);
        model->Refresh(-1, -1);
    } else {
        for (std::size_t i = 0; i < clipboard.size(); ++i) {
            model->SetFrame(start + static_cast<int>(i), clipboard[i]);
        }
    }
    OnFramesEdited(start);
}

void TasEditorWidget::InsertBlank() {
    const auto rows = SelectedRows();
    if (rows.empty() || !model->IsEditable(rows.front()) || rows.front() > model->FrameCount()) {
        return;
    }
    std::vector<TasFrame> frames(rows.size(), model->BlankFrame());
    system.Movie().TasInsertFrames(static_cast<std::size_t>(rows.front()), frames);
    model->Refresh(-1, -1);
    OnFramesEdited(rows.front());
}

void TasEditorWidget::DeleteFrames() {
    const auto rows = SelectedRows();
    if (rows.empty() || !model->IsEditable(rows.front())) {
        return;
    }
    // Delete from the bottom so the indices of the remaining rows stay valid
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        if (*it < model->FrameCount()) {
            system.Movie().TasDeleteFrames(static_cast<std::size_t>(*it), 1);
        }
    }
    model->Refresh(-1, -1);
    OnFramesEdited(rows.front());
}

void TasEditorWidget::ClearInputs() {
    const auto rows = SelectedRows();
    if (rows.empty() || !model->IsEditable(rows.front())) {
        return;
    }
    for (const int row : rows) {
        if (row >= model->FrameCount()) {
            break;
        }
        TasFrame frame = model->GetFrame(row);
        // Keep the motion sensors, clear the controls
        const auto accel = frame.accel;
        const auto gyro = frame.gyro;
        frame = TasFrame{};
        frame.accel = accel;
        frame.gyro = gyro;
        model->SetFrame(row, frame);
    }
    OnFramesEdited(rows.front());
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
    menu.addAction(tr("Copy\tCtrl+C"), this, &TasEditorWidget::CopySelection);
    const bool can_paste =
        !clipboard.empty() || FramesFromJson(QApplication::clipboard()->text()).has_value();
    menu.addAction(tr("Paste\tCtrl+V"), this, [this] { Paste(false); })->setEnabled(can_paste);
    menu.addAction(tr("Paste Insert\tCtrl+Shift+V"), this, [this] { Paste(true); })
        ->setEnabled(can_paste);
    menu.addSeparator();
    menu.addAction(tr("Insert Blank Frames\tInsert"), this, &TasEditorWidget::InsertBlank);
    menu.addAction(tr("Delete Frames\tCtrl+Delete"), this, &TasEditorWidget::DeleteFrames);
    menu.addAction(tr("Clear Inputs\tDelete"), this, &TasEditorWidget::ClearInputs);
    menu.exec(view->viewport()->mapToGlobal(pos));
}

bool TasEditorWidget::eventFilter(QObject* object, QEvent* event) {
    // Keyboard shortcuts of the table. They are handled here (and ShortcutOverride accepted) so
    // they take priority over the application wide hotkeys using the same keys.
    if (object == view &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        const auto combination = key_event->keyCombination();
        std::function<void()> action;
        if (combination == QKeyCombination(Qt::ControlModifier, Qt::Key_C)) {
            action = [this] { CopySelection(); };
        } else if (combination == QKeyCombination(Qt::ControlModifier, Qt::Key_V)) {
            action = [this] { Paste(false); };
        } else if (combination ==
                   QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_V)) {
            action = [this] { Paste(true); };
        } else if (combination == QKeyCombination(Qt::Key_Insert)) {
            action = [this] { InsertBlank(); };
        } else if (combination == QKeyCombination(Qt::ControlModifier, Qt::Key_Delete)) {
            action = [this] { DeleteFrames(); };
        } else if (combination == QKeyCombination(Qt::Key_Delete)) {
            action = [this] { ClearInputs(); };
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

    // Toggling and drawing buttons with the mouse
    if (object == view->viewport()) {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress && mouse_event->button() == Qt::LeftButton &&
            mouse_event->modifiers() == Qt::NoModifier) {
            const QModelIndex index = view->indexAt(mouse_event->position().toPoint());
            if (index.isValid() && TasEditorModel::IsButtonColumn(index.column()) &&
                model->IsEditable(index.row())) {
                TasFrame frame = model->GetFrame(index.row());
                draw_value = !TasEditorModel::GetButton(frame, index.column());
                TasEditorModel::SetButton(frame, index.column(), draw_value);
                model->SetFrame(index.row(), frame);
                drawing = true;
                draw_column = index.column();
                draw_last_row = index.row();
                draw_first_row = index.row();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && drawing) {
            const int row = view->rowAt(static_cast<int>(mouse_event->position().y()));
            if (row >= 0 && row != draw_last_row) {
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
        } else if (event->type() == QEvent::MouseButtonRelease && drawing) {
            drawing = false;
            OnFramesEdited(draw_first_row);
            return true;
        }
    }
    return QDockWidget::eventFilter(object, event);
}
