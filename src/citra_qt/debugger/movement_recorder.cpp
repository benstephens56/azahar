// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <fmt/format.h>
#include "citra_qt/debugger/movement_recorder.h"
#include "citra_qt/debugger/oot3d_memory.h"
#include "common/file_util.h"
#include "core/core.h"
#include "core/memory_editor.h"

namespace {

using namespace OoT3D;

QString SettingsPath() {
    return QString::fromStdString(fmt::format(
        "{}movement_recorder.ini", FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir)));
}

/// The seam calculator finds the same GlobalContext: use it until this tool has its own
QString SeamCalculatorSettingsPath() {
    return QString::fromStdString(
        fmt::format("{}seam_calculator.ini", FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir)));
}

QString Hex16(u16 value) {
    return QStringLiteral("0x") +
           QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0')).toUpper();
}

/// Exact text of a float (round trips)
QString Exact(double value) {
    return QString::number(value, 'g', 9);
}

QString Short(double value) {
    return QString::number(value, 'f', 3);
}

double YawToRadians(u16 yaw) {
    return yaw * (2.0 * std::numbers::pi / 65536.0);
}

enum Column {
    ColumnIndex,
    ColumnGameFrame,
    ColumnX,
    ColumnY,
    ColumnZ,
    ColumnDeltaForward,
    ColumnDeltaLeft,
    ColumnDeltaY,
    ColumnForward,
    ColumnLeft,
    ColumnFacing,
    ColumnMoveYaw,
    ColumnSpeed,
    ColumnVelocityY,
    ColumnGround,
    ColumnCount,
};

} // namespace

MovementRecorderWidget::MovementRecorderWidget(Core::System& system_, QWidget* parent)
    : QDockWidget(tr("Movement Recorder"), parent), system{system_} {
    setObjectName(QStringLiteral("MovementRecorderWidget"));

    auto* contents = new QWidget;
    auto* layout = new QVBoxLayout(contents);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(CreateLinkGroup());
    layout->addWidget(CreateRecordGroup(), 1);
    setWidget(contents);

    QSettings settings(SettingsPath(), QSettings::IniFormat);
    QString context = settings.value(QStringLiteral("global_context")).toString();
    if (context.isEmpty()) {
        QSettings seam_settings(SeamCalculatorSettingsPath(), QSettings::IniFormat);
        context = seam_settings.value(QStringLiteral("global_context")).toString();
    }
    context_edit->setText(context);
    name_edit->setText(settings.value(QStringLiteral("name")).toString());
    follow_check->setChecked(settings.value(QStringLiteral("follow"), true).toBool());
    context_address = ContextAddress().value_or(0);

    update_timer.setInterval(100);
    connect(&update_timer, &QTimer::timeout, this, [this] {
        DrainSamples();
        UpdateLive();
    });
    UpdateLive();
}

MovementRecorderWidget::~MovementRecorderWidget() {
    recording = false;
    system.SetFrameEndCallback({});
    SaveSettings();
}

void MovementRecorderWidget::SaveSettings() const {
    QSettings settings(SettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("global_context"), context_edit->text());
    settings.setValue(QStringLiteral("name"), name_edit->text());
    settings.setValue(QStringLiteral("follow"), follow_check->isChecked());
}

QWidget* MovementRecorderWidget::CreateLinkGroup() {
    auto* group = new QGroupBox(tr("Link"));
    auto* form = new QFormLayout(group);

    auto* context_row = new QHBoxLayout;
    context_edit = new QLineEdit(group);
    context_edit->setPlaceholderText(tr("Click Find while in game"));
    context_edit->setToolTip(tr("Address of the GlobalContext (OoT3D). Link's actor and the game's "
                                "frame counter are read through it."));
    connect(context_edit, &QLineEdit::editingFinished, this, [this] {
        context_address = ContextAddress().value_or(0);
        SaveSettings();
        UpdateLive();
    });
    context_row->addWidget(context_edit, 1);
    auto* find_button = new QPushButton(tr("Find"), group);
    connect(find_button, &QPushButton::clicked, this, &MovementRecorderWidget::FindGlobalContext);
    context_row->addWidget(find_button);
    form->addRow(tr("GlobalContext"), context_row);

    live_label = new QLabel(group);
    live_label->setWordWrap(true);
    live_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Now"), live_label);
    return group;
}

QWidget* MovementRecorderWidget::CreateRecordGroup() {
    auto* group = new QGroupBox(tr("Recording"));
    auto* layout = new QVBoxLayout(group);

    auto* form = new QFormLayout;
    name_edit = new QLineEdit(group);
    name_edit->setPlaceholderText(tr("e.g. adult sidehop left"));
    name_edit->setToolTip(tr("Name of the technique, saved in the JSON file and used as the "
                             "default file name."));
    connect(name_edit, &QLineEdit::editingFinished, this, [this] { SaveSettings(); });
    form->addRow(tr("Name"), name_edit);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout;
    record_button = new QPushButton(tr("Record"), group);
    record_button->setCheckable(true);
    record_button->setToolTip(
        tr("Records one row per game frame (every second emulated frame at 30 fps) while checked. "
           "Works with frame advance. Loading a savestate from earlier replaces the rows from "
           "there on, so an attempt can be redone."));
    connect(record_button, &QPushButton::toggled, this, &MovementRecorderWidget::SetRecording);
    buttons->addWidget(record_button);
    clear_button = new QPushButton(tr("Clear"), group);
    connect(clear_button, &QPushButton::clicked, this, &MovementRecorderWidget::Clear);
    buttons->addWidget(clear_button);
    save_csv_button = new QPushButton(tr("Save CSV..."), group);
    connect(save_csv_button, &QPushButton::clicked, this, &MovementRecorderWidget::SaveCsv);
    buttons->addWidget(save_csv_button);
    save_json_button = new QPushButton(tr("Save JSON..."), group);
    connect(save_json_button, &QPushButton::clicked, this, &MovementRecorderWidget::SaveJson);
    buttons->addWidget(save_json_button);
    buttons->addStretch(1);
    follow_check = new QCheckBox(tr("Follow"), group);
    follow_check->setToolTip(tr("Scroll to the newest row"));
    connect(follow_check, &QCheckBox::toggled, this, [this] { SaveSettings(); });
    buttons->addWidget(follow_check);
    layout->addLayout(buttons);

    status_label = new QLabel(group);
    status_label->setWordWrap(true);
    layout->addWidget(status_label);

    table = new QTableWidget(0, ColumnCount, group);
    table->setHorizontalHeaderLabels({
        tr("#"),
        tr("Game frame"),
        tr("X"),
        tr("Y"),
        tr("Z"),
        tr("Δ Forward"),
        tr("Δ Left"),
        tr("Δ Y"),
        tr("Forward"),
        tr("Left"),
        tr("Facing"),
        tr("Move yaw"),
        tr("Speed"),
        tr("Vel. Y"),
        tr("Ground"),
    });
    const QString local_tip =
        tr("In Link's frame of reference at the first row: forward is the direction he faced "
           "(shape.rot.y), left is 90 degrees counterclockwise from it seen from above.");
    for (const int column : {ColumnDeltaForward, ColumnDeltaLeft, ColumnForward, ColumnLeft}) {
        table->horizontalHeaderItem(column)->setToolTip(local_tip);
    }
    table->horizontalHeaderItem(ColumnDeltaY)->setToolTip(tr("Change of Y since the last row"));
    table->horizontalHeaderItem(ColumnFacing)->setToolTip(tr("shape.rot.y"));
    table->horizontalHeaderItem(ColumnMoveYaw)->setToolTip(tr("world.rot.y"));
    table->horizontalHeaderItem(ColumnSpeed)->setToolTip(tr("Actor.speedXZ"));
    table->horizontalHeaderItem(ColumnVelocityY)->setToolTip(tr("velocity.y"));
    table->horizontalHeaderItem(ColumnGround)->setToolTip(tr("bgCheckFlags & 1"));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 4);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(table, 1);

    RebuildTable();
    return group;
}

std::optional<VAddr> MovementRecorderWidget::ContextAddress() const {
    QString text = context_edit->text().trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        text = text.mid(2);
    }
    bool ok = false;
    const VAddr address = text.toUInt(&ok, 16);
    if (!ok || address == 0) {
        return std::nullopt;
    }
    return address;
}

void MovementRecorderWidget::FindGlobalContext() {
    if (!emulation_running) {
        return;
    }
    VAddr found = 0;
    const auto result = OoT3D::FindGlobalContext(system.MemoryEditor(), found);
    if (result == OoT3D::FindResult::NoMemory) {
        QMessageBox::warning(this, tr("Movement Recorder"),
                             tr("Could not read the memory. Is the game running?"));
        return;
    }
    if (result == OoT3D::FindResult::NotFound) {
        QMessageBox::warning(this, tr("Movement Recorder"),
                             tr("Could not find the GlobalContext. Try again while playing (not "
                                "on the title screen or file select)."));
        return;
    }
    context_edit->setText(QStringLiteral("0x%1").arg(found, 8, 16, QLatin1Char('0')));
    context_address = found;
    SaveSettings();
    UpdateLive();
}

std::optional<MovementRecorderWidget::Sample> MovementRecorderWidget::ReadSample(
    VAddr context) const {
    const auto& memory = system.MemoryEditor();
    const auto frame = Read<u32>(memory, context + OffsetGameplayFrames);
    const auto actor = PlayerActor(memory, context);
    if (!frame || !actor) {
        return std::nullopt;
    }
    const auto x = Read<float>(memory, *actor + OffsetWorldPos);
    const auto y = Read<float>(memory, *actor + OffsetWorldPos + 4);
    const auto z = Read<float>(memory, *actor + OffsetWorldPos + 8);
    const auto velocity_y = Read<float>(memory, *actor + OffsetVelocity + 4);
    const auto speed_xz = Read<float>(memory, *actor + OffsetSpeedXZ);
    const auto xz_speed = Read<float>(memory, *actor + OffsetPlayerXZSpeed);
    const auto move_yaw = Read<u16>(memory, *actor + OffsetWorldRotY);
    const auto facing = Read<u16>(memory, *actor + OffsetShapeRotY);
    const auto player_yaw = Read<u16>(memory, *actor + OffsetPlayerYaw);
    const auto flags = Read<u16>(memory, *actor + OffsetBgCheckFlags);
    const auto floor_height = Read<float>(memory, *actor + OffsetFloorHeight);
    if (!x || !y || !z || !velocity_y || !speed_xz || !xz_speed || !move_yaw || !facing ||
        !player_yaw || !flags || !floor_height) {
        return std::nullopt;
    }
    return Sample{*frame,    *x,        *y,      *z,          *velocity_y, *speed_xz,
                  *xz_speed, *move_yaw, *facing, *player_yaw, *flags,      *floor_height};
}

void MovementRecorderWidget::SetRecording(bool enable) {
    if (enable && !context_address.load()) {
        FindGlobalContext();
        if (!context_address.load()) {
            QSignalBlocker blocker(record_button);
            record_button->setChecked(false);
            return;
        }
    }
    {
        QSignalBlocker blocker(record_button);
        record_button->setChecked(enable);
    }
    record_button->setText(enable ? tr("Stop") : tr("Record"));
    if (enable) {
        // A new attempt: the next game frame is always recorded
        last_game_frame = -1;
    }
    recording = enable;
    UpdateFrameCallback();
    if (!enable) {
        DrainSamples();
    }
    UpdateTimerState();
}

void MovementRecorderWidget::UpdateFrameCallback() {
    if (recording && emulation_running) {
        system.SetFrameEndCallback([this] { OnFrameEnd(); });
    } else {
        system.SetFrameEndCallback({});
    }
}

void MovementRecorderWidget::OnFrameEnd() {
    if (!recording) {
        return;
    }
    const VAddr context = context_address.load();
    if (!context) {
        return;
    }
    const auto frame = Read<u32>(system.MemoryEditor(), context + OffsetGameplayFrames);
    if (!frame || last_game_frame.load() == static_cast<s64>(*frame)) {
        // The game did not run a frame since the last sample (30 fps, or a lag frame)
        return;
    }
    const auto sample = ReadSample(context);
    if (!sample) {
        return;
    }
    last_game_frame = sample->game_frame;
    std::scoped_lock lock{pending_mutex};
    pending.push_back(*sample);
}

void MovementRecorderWidget::DrainSamples() {
    std::vector<Sample> taken;
    {
        std::scoped_lock lock{pending_mutex};
        taken.swap(pending);
    }
    if (taken.empty()) {
        return;
    }
    const std::size_t old_size = samples.size();
    bool rewound = false;
    for (const Sample& sample : taken) {
        if (!samples.empty() && sample.game_frame <= samples.back().game_frame) {
            // A savestate from earlier was loaded: redo from this game frame
            const auto it =
                std::lower_bound(samples.begin(), samples.end(), sample.game_frame,
                                 [](const Sample& s, u32 frame) { return s.game_frame < frame; });
            samples.erase(it, samples.end());
            rewound = true;
        }
        samples.push_back(sample);
    }
    if (rewound) {
        RebuildTable();
    } else {
        for (std::size_t i = old_size; i < samples.size(); ++i) {
            AppendRow(i);
        }
    }
    if (follow_check->isChecked() && table->rowCount() > 0) {
        table->scrollToBottom();
    }
    status_label->setText(tr("%n game frame(s) recorded", "", static_cast<int>(samples.size())));
}

void MovementRecorderWidget::UpdateLive() {
    const auto context = ContextAddress();
    if (!emulation_running) {
        live_label->setText(tr("Not running"));
        return;
    }
    if (!context) {
        live_label->setText(tr("Click Find while in game"));
        return;
    }
    const auto sample = ReadSample(*context);
    if (!sample) {
        live_label->setText(tr("Link's actor not found"));
        return;
    }
    live_label->setText(
        tr("(%1, %2, %3), facing %4, moving %5 at %6, %7, game frame %8")
            .arg(Short(sample->x), Short(sample->y), Short(sample->z), Hex16(sample->facing),
                 Hex16(sample->move_yaw), Short(sample->speed_xz),
                 (sample->bg_check_flags & BgCheckGround) ? tr("on the ground") : tr("in the air"))
            .arg(sample->game_frame));
}

void MovementRecorderWidget::UpdateTimerState() {
    if (emulation_running && (isVisible() || recording)) {
        update_timer.start();
    } else {
        update_timer.stop();
    }
}

MovementRecorderWidget::Local MovementRecorderWidget::LocalDelta(std::size_t index) const {
    if (index == 0) {
        return {};
    }
    const Sample& a = samples[index - 1];
    const Sample& b = samples[index];
    const double f = YawToRadians(samples.front().facing);
    const double dx = static_cast<double>(b.x) - a.x;
    const double dz = static_cast<double>(b.z) - a.z;
    return {dx * std::sin(f) + dz * std::cos(f), dx * std::cos(f) - dz * std::sin(f),
            static_cast<double>(b.y) - a.y};
}

MovementRecorderWidget::Local MovementRecorderWidget::LocalTotal(std::size_t index) const {
    const Sample& a = samples.front();
    const Sample& b = samples[index];
    const double f = YawToRadians(a.facing);
    const double dx = static_cast<double>(b.x) - a.x;
    const double dz = static_cast<double>(b.z) - a.z;
    return {dx * std::sin(f) + dz * std::cos(f), dx * std::cos(f) - dz * std::sin(f),
            static_cast<double>(b.y) - a.y};
}

void MovementRecorderWidget::AppendRow(std::size_t index) {
    const Sample& s = samples[index];
    const Local delta = LocalDelta(index);
    const Local total = LocalTotal(index);
    const int row = static_cast<int>(index);
    table->setRowCount(row + 1);
    const auto set = [&](int column, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(row, column, item);
    };
    set(ColumnIndex, QString::number(index));
    set(ColumnGameFrame, QString::number(s.game_frame));
    set(ColumnX, Short(s.x));
    set(ColumnY, Short(s.y));
    set(ColumnZ, Short(s.z));
    set(ColumnDeltaForward, Short(delta.forward));
    set(ColumnDeltaLeft, Short(delta.left));
    set(ColumnDeltaY, Short(delta.up));
    set(ColumnForward, Short(total.forward));
    set(ColumnLeft, Short(total.left));
    set(ColumnFacing, Hex16(s.facing));
    set(ColumnMoveYaw, Hex16(s.move_yaw));
    set(ColumnSpeed, Short(s.speed_xz));
    set(ColumnVelocityY, Short(s.velocity_y));
    set(ColumnGround, (s.bg_check_flags & BgCheckGround) ? tr("yes") : tr("no"));
}

void MovementRecorderWidget::RebuildTable() {
    table->setRowCount(0);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        AppendRow(i);
    }
    status_label->setText(tr("%n game frame(s) recorded", "", static_cast<int>(samples.size())));
}

void MovementRecorderWidget::Clear() {
    {
        std::scoped_lock lock{pending_mutex};
        pending.clear();
    }
    samples.clear();
    last_game_frame = -1;
    RebuildTable();
}

void MovementRecorderWidget::SaveCsv() {
    DrainSamples();
    if (samples.empty()) {
        QMessageBox::information(this, tr("Movement Recorder"), tr("Nothing recorded yet."));
        return;
    }
    const QString name = name_edit->text().trimmed();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Recording"),
        name.isEmpty() ? QStringLiteral("movement.csv") : name + QStringLiteral(".csv"),
        tr("CSV files (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Movement Recorder"), tr("Could not write %1.").arg(path));
        return;
    }
    QTextStream out(&file);
    out << "index,game_frame,x,y,z,d_forward,d_left,d_y,forward,left,up,facing,move_yaw,"
           "player_yaw,speed_xz,xz_speed,velocity_y,bg_check_flags,on_ground,floor_height\n";
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        const Local delta = LocalDelta(i);
        const Local total = LocalTotal(i);
        out << i << ',' << s.game_frame << ',' << Exact(s.x) << ',' << Exact(s.y) << ','
            << Exact(s.z) << ',' << Exact(delta.forward) << ',' << Exact(delta.left) << ','
            << Exact(delta.up) << ',' << Exact(total.forward) << ',' << Exact(total.left) << ','
            << Exact(total.up) << ',' << s.facing << ',' << s.move_yaw << ',' << s.player_yaw << ','
            << Exact(s.speed_xz) << ',' << Exact(s.xz_speed) << ',' << Exact(s.velocity_y) << ','
            << s.bg_check_flags << ',' << ((s.bg_check_flags & BgCheckGround) ? 1 : 0) << ','
            << Exact(s.floor_height) << '\n';
    }
}

void MovementRecorderWidget::SaveJson() {
    DrainSamples();
    if (samples.empty()) {
        QMessageBox::information(this, tr("Movement Recorder"), tr("Nothing recorded yet."));
        return;
    }
    const QString name = name_edit->text().trimmed();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Recording"),
        name.isEmpty() ? QStringLiteral("movement.json") : name + QStringLiteral(".json"),
        tr("JSON files (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    const auto vec = [](double a, double b, double c) { return QJsonArray{a, b, c}; };
    QJsonArray frames;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        const Local delta = LocalDelta(i);
        const Local total = LocalTotal(i);
        frames.append(QJsonObject{
            {QStringLiteral("game_frame"), static_cast<qint64>(s.game_frame)},
            {QStringLiteral("pos"), vec(s.x, s.y, s.z)},
            {QStringLiteral("delta_local"), vec(delta.forward, delta.left, delta.up)},
            {QStringLiteral("local"), vec(total.forward, total.left, total.up)},
            {QStringLiteral("facing"), s.facing},
            {QStringLiteral("move_yaw"), s.move_yaw},
            {QStringLiteral("player_yaw"), s.player_yaw},
            {QStringLiteral("speed_xz"), static_cast<double>(s.speed_xz)},
            {QStringLiteral("xz_speed"), static_cast<double>(s.xz_speed)},
            {QStringLiteral("velocity_y"), static_cast<double>(s.velocity_y)},
            {QStringLiteral("bg_check_flags"), s.bg_check_flags},
            {QStringLiteral("on_ground"), (s.bg_check_flags & BgCheckGround) != 0},
            {QStringLiteral("floor_height"), static_cast<double>(s.floor_height)},
        });
    }
    const Sample& first = samples.front();
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("azahar-oot3d-movement")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("game"), QStringLiteral("OOT3D")},
        {QStringLiteral("name"), name},
        {QStringLiteral("note"),
         QStringLiteral("One entry per game frame (30 fps). local / delta_local: [forward, left, "
                        "up] relative to the first frame's facing (shape.rot.y; yaw 0 = +Z, "
                        "0x4000 = +X), left being 90 degrees counterclockwise seen from above.")},
        {QStringLiteral("reference"),
         QJsonObject{{QStringLiteral("facing"), first.facing},
                     {QStringLiteral("pos"), vec(first.x, first.y, first.z)}}},
        {QStringLiteral("frames"), frames},
    };
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Movement Recorder"), tr("Could not write %1.").arg(path));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MovementRecorderWidget::OnEmulationStarting(EmuThread*) {
    emulation_running = true;
    UpdateFrameCallback();
    UpdateTimerState();
}

void MovementRecorderWidget::OnEmulationStopping() {
    emulation_running = false;
    if (recording) {
        SetRecording(false);
    }
    UpdateFrameCallback();
    UpdateTimerState();
    UpdateLive();
}

void MovementRecorderWidget::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    UpdateTimerState();
}

void MovementRecorderWidget::hideEvent(QHideEvent* event) {
    QDockWidget::hideEvent(event);
    UpdateTimerState();
}
