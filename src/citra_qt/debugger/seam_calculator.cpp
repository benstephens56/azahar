// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <fmt/format.h>
#include "citra_qt/debugger/seam_calculator.h"
#include "common/file_util.h"
#include "core/core.h"
#include "core/memory_editor.h"

namespace {

// Offsets in the Actor struct (see include/z3D/z3Dactor.h of the OoT3D practice menu)
constexpr VAddr OffsetWorldPos = 0x028;
constexpr VAddr OffsetFloorHeight = 0x084;
constexpr VAddr OffsetBgCheckFlags = 0x090;
constexpr VAddr OffsetPrevPos = 0x108;
constexpr VAddr OffsetWorldRotY = 0x036;
constexpr VAddr OffsetSpeedXZ = 0x06C;
constexpr VAddr OffsetActorType = 0x002;
constexpr u8 ActorTypePlayer = 2;

// Offsets in the GlobalContext and Camera structs (see include/z3D/z3D.h of the practice menu)
constexpr VAddr OffsetMainCamera = 0x364;
constexpr VAddr OffsetCameraGlobalContext = 0x0D4;
constexpr VAddr OffsetCameraPtrs = 0xA54;
constexpr VAddr OffsetActiveCamera = 0xA64;
constexpr VAddr OffsetCameraInputYaw = 0x17E; // inputDir.y
/// actorCtx (0x208C) .actorList (0x0C) [ACTORTYPE_PLAYER] (2 * 8) .first (4)
constexpr VAddr OffsetPlayerActor = 0x208C + 0x0C + 2 * 8 + 4;

constexpr u16 BgCheckGround = 0x0001;

QString SettingsPath() {
    return QString::fromStdString(
        fmt::format("{}seam_calculator.ini", FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir)));
}

QString FormatVertex(const std::array<s16, 3>& v) {
    return QStringLiteral("%1, %2, %3").arg(v[0]).arg(v[1]).arg(v[2]);
}

std::optional<std::array<int, 3>> ParseTriple(const QString& text) {
    static const QRegularExpression separator(QStringLiteral("[,;\\s]+"));
    const auto parts = text.split(separator, Qt::SkipEmptyParts);
    if (parts.size() != 3) {
        return std::nullopt;
    }
    std::array<int, 3> values{};
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        values[i] = parts[i].toInt(&ok);
        if (!ok || values[i] < -32768 || values[i] > 32767) {
            return std::nullopt;
        }
    }
    return values;
}

std::vector<u8> FloatBytes(float value) {
    std::vector<u8> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

QString Colored(const QString& text, const char* color) {
    return QStringLiteral("<span style=\"color:%1\">%2</span>")
        .arg(QString::fromLatin1(color), text.toHtmlEscaped());
}

} // namespace

SeamCalculatorWidget::SeamCalculatorWidget(Core::System& system_, QWidget* parent)
    : QDockWidget(tr("Seam Calculator"), parent), system{system_} {
    setObjectName(QStringLiteral("SeamCalculatorWidget"));

    auto* contents = new QWidget;
    auto* layout = new QVBoxLayout(contents);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(CreateLinkGroup());
    layout->addWidget(CreateTriangleGroup(), 1);
    layout->addWidget(CreateLiveGroup());
    layout->addWidget(CreateClimbGroup());

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(contents);
    setWidget(scroll);

    QSettings settings(SettingsPath(), QSettings::IniFormat);
    context_edit->setText(settings.value(QStringLiteral("global_context")).toString());
    address_edit->setText(settings.value(QStringLiteral("actor_address")).toString());
    plan_speed_spin->setValue(settings.value(QStringLiteral("plan_speed"), 1.0).toDouble());
    aim_spin->setValue(settings.value(QStringLiteral("aim_gain"), 25.0).toDouble());
    stick_magnitude_spin->setValue(settings.value(QStringLiteral("stick_magnitude"), 150).toInt());
    radius_spin->setValue(settings.value(QStringLiteral("radius"), 200).toInt());
    const QString path = settings.value(QStringLiteral("collision_file")).toString();
    if (!path.isEmpty() && QFile::exists(path)) {
        LoadCollision(path);
    }

    update_timer.setInterval(16);
    connect(&update_timer, &QTimer::timeout, this, &SeamCalculatorWidget::Update);
    Update();
}

SeamCalculatorWidget::~SeamCalculatorWidget() {
    SaveSettings();
}

void SeamCalculatorWidget::SaveSettings() const {
    QSettings settings(SettingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("global_context"), context_edit->text());
    settings.setValue(QStringLiteral("actor_address"), address_edit->text());
    settings.setValue(QStringLiteral("plan_speed"), plan_speed_spin->value());
    settings.setValue(QStringLiteral("aim_gain"), aim_spin->value());
    settings.setValue(QStringLiteral("stick_magnitude"), stick_magnitude_spin->value());
    settings.setValue(QStringLiteral("radius"), radius_spin->value());
    settings.setValue(QStringLiteral("collision_file"), collision_path);
}

QWidget* SeamCalculatorWidget::CreateLinkGroup() {
    auto* group = new QGroupBox(tr("Link"));
    auto* form = new QFormLayout(group);

    auto* context_row = new QHBoxLayout;
    context_edit = new QLineEdit(group);
    context_edit->setPlaceholderText(tr("Click Find while in game"));
    context_edit->setToolTip(tr("Address of the GlobalContext. With it, Link's actor and the "
                                "camera are found automatically."));
    connect(context_edit, &QLineEdit::editingFinished, this, [this] {
        SaveSettings();
        Update();
    });
    context_row->addWidget(context_edit, 1);
    auto* find_context_button = new QPushButton(tr("Find"), group);
    connect(find_context_button, &QPushButton::clicked, this,
            &SeamCalculatorWidget::FindGlobalContext);
    context_row->addWidget(find_context_button);
    form->addRow(tr("GlobalContext"), context_row);

    address_edit = new QLineEdit(group);
    address_edit->setPlaceholderText(tr("Only needed without the GlobalContext, e.g. 0x0898F9A0"));
    address_edit->setToolTip(tr("The start of the player actor. Link's X position is at +0x28, "
                                "Y at +0x2C and Z at +0x30 from it."));
    connect(address_edit, &QLineEdit::editingFinished, this, [this] {
        SaveSettings();
        Update();
    });
    form->addRow(tr("Actor address"), address_edit);
    position_label = new QLabel(group);
    position_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Position"), position_label);
    floor_label = new QLabel(group);
    form->addRow(tr("Floor"), floor_label);
    motion_label = new QLabel(group);
    form->addRow(tr("Movement"), motion_label);
    return group;
}

QWidget* SeamCalculatorWidget::CreateClimbGroup() {
    auto* group = new QGroupBox(tr("Climb"));
    auto* form = new QFormLayout(group);
    next_frame_label = new QLabel(group);
    next_frame_label->setWordWrap(true);
    next_frame_label->setToolTip(tr("Where Link's current speed and direction take him next frame. "
                                    "He falls if he gains 50 or more."));
    form->addRow(tr("Next frame"), next_frame_label);

    plan_speed_spin = new QDoubleSpinBox(group);
    plan_speed_spin->setRange(0.01, 20.0);
    plan_speed_spin->setDecimals(2);
    plan_speed_spin->setSingleStep(0.1);
    plan_speed_spin->setSuffix(tr(" units/frame"));
    plan_speed_spin->setToolTip(tr("Speed used to plan directions while Link is slower than this "
                                   "(e.g. standing still). Link's own speed is used when higher."));
    connect(plan_speed_spin, &QDoubleSpinBox::valueChanged, this, [this] { Update(); });
    form->addRow(tr("Plan for speed"), plan_speed_spin);

    aim_spin = new QDoubleSpinBox(group);
    aim_spin->setRange(0.1, 49.9);
    aim_spin->setDecimals(1);
    aim_spin->setSuffix(tr(" height per frame"));
    aim_spin->setToolTip(tr("Height to gain per frame when aiming. Under 50, with some margin for "
                            "speed changes and the stick's precision."));
    connect(aim_spin, &QDoubleSpinBox::valueChanged, this, [this] { Update(); });
    form->addRow(tr("Aim to gain"), aim_spin);

    stick_magnitude_spin = new QSpinBox(group);
    stick_magnitude_spin->setRange(1, 156);
    stick_magnitude_spin->setToolTip(
        tr("Circle pad distance from the center (as in the TAS Input window) used for the "
           "suggested positions. On a seam Link only moves with the stick quite far out."));
    connect(stick_magnitude_spin, &QSpinBox::valueChanged, this, [this] { Update(); });
    form->addRow(tr("Stick magnitude"), stick_magnitude_spin);

    for (std::size_t i = 0; i < direction_labels.size(); ++i) {
        direction_labels[i] = new QLabel(group);
        direction_labels[i]->setWordWrap(true);
        direction_labels[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(tr("Way %1").arg(i + 1), direction_labels[i]);
    }
    return group;
}

QWidget* SeamCalculatorWidget::CreateTriangleGroup() {
    auto* group = new QGroupBox(tr("Seam triangle"));
    auto* layout = new QVBoxLayout(group);

    auto* file_row = new QHBoxLayout;
    auto* load_button = new QPushButton(tr("Load Collision..."), group);
    load_button->setToolTip(tr("Loads the collision of a scene from its .zsi file (e.g. "
                               "models/OOT3D/spot00_info.zsi of exodus122's 3d_model_viewer)"));
    connect(load_button, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Load Scene Collision"), collision_path, tr("OoT3D Scene (*.zsi)"));
        if (!path.isEmpty()) {
            LoadCollision(path);
        }
    });
    file_row->addWidget(load_button);
    file_label = new QLabel(tr("No collision loaded"), group);
    file_row->addWidget(file_label, 1);
    layout->addLayout(file_row);

    auto* find_row = new QHBoxLayout;
    find_row->addWidget(new QLabel(tr("Seams within"), group));
    radius_spin = new QSpinBox(group);
    radius_spin->setRange(1, 100000);
    radius_spin->setSuffix(tr(" units of Link"));
    find_row->addWidget(radius_spin);
    auto* find_button = new QPushButton(tr("Find"), group);
    connect(find_button, &QPushButton::clicked, this, &SeamCalculatorWidget::FindSeams);
    find_row->addWidget(find_button);
    find_row->addStretch();
    layout->addLayout(find_row);

    seam_table = new QTableWidget(0, 5, group);
    seam_table->setHorizontalHeaderLabels(
        {tr("Poly"), tr("Distance"), tr("Angle"), tr("Rise / unit"), tr("Nearest vertex")});
    seam_table->verticalHeader()->setVisible(false);
    seam_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    seam_table->setSelectionMode(QAbstractItemView::SingleSelection);
    seam_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    seam_table->horizontalHeader()->setStretchLastSection(true);
    seam_table->setMinimumHeight(120);
    connect(seam_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = seam_table->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            return;
        }
        const int index = seam_table->item(rows.front().row(), 0)->data(Qt::UserRole).toInt();
        const auto it = std::find_if(collision.begin(), collision.end(),
                                     [index](const auto& tri) { return tri.index == index; });
        if (it != collision.end()) {
            SelectTriangle(*it);
        }
    });
    layout->addWidget(seam_table, 1);

    auto* form = new QFormLayout;
    for (std::size_t i = 0; i < vertex_edits.size(); ++i) {
        vertex_edits[i] = new QLineEdit(group);
        vertex_edits[i]->setPlaceholderText(QStringLiteral("x, y, z"));
        form->addRow(tr("Vertex %1").arg(i + 1), vertex_edits[i]);
    }
    normal_edit = new QLineEdit(group);
    normal_edit->setPlaceholderText(tr("x, y, z (as stored, e.g. -12161, 4, 30426)"));
    form->addRow(tr("Normal"), normal_edit);
    dist_edit = new QLineEdit(group);
    dist_edit->setPlaceholderText(tr("e.g. -477.623"));
    form->addRow(tr("Distance"), dist_edit);
    for (QLineEdit* edit :
         {vertex_edits[0], vertex_edits[1], vertex_edits[2], normal_edit, dist_edit}) {
        connect(edit, &QLineEdit::editingFinished, this, [this] {
            ParseTriangleFields();
            Update();
        });
    }
    layout->addLayout(form);
    triangle_label = new QLabel(group);
    triangle_label->setWordWrap(true);
    layout->addWidget(triangle_label);
    return group;
}

QWidget* SeamCalculatorWidget::CreateLiveGroup() {
    auto* group = new QGroupBox(tr("Live"));
    auto* form = new QFormLayout(group);
    vertices_label = new QLabel(group);
    vertices_label->setToolTip(tr("Distance from Link to each vertex in the XZ plane. The floor "
                                  "check accepts a seam triangle within 1 unit of a vertex."));
    form->addRow(tr("Vertex distances"), vertices_label);
    height_label = new QLabel(group);
    height_label->setWordWrap(true);
    form->addRow(tr("Seam under Link"), height_label);
    game_floor_label = new QLabel(group);
    game_floor_label->setWordWrap(true);
    form->addRow(tr("Game's floor"), game_floor_label);

    band_label = new QLabel(group);
    band_label->setWordWrap(true);
    band_label->setToolTip(
        tr("Link is put on the seam where its height is from his feet up to "
           "50 units above his previous Y (the start of the floor check's ray)."));
    form->addRow(tr("Pop-up band"), band_label);
    target_label = new QLabel(group);
    target_label->setWordWrap(true);
    target_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Nearest spot"), target_label);
    move_button = new QPushButton(tr("Move Link onto the Seam (memory write, for testing)"), group);
    move_button->setToolTip(
        tr("Writes the target X and Z to Link's position (and previous position). This is not an "
           "input: a movie recorded with it will not play back the same."));
    connect(move_button, &QPushButton::clicked, this, &SeamCalculatorWidget::MoveLinkToTarget);
    form->addRow(move_button);
    return group;
}

void SeamCalculatorWidget::OnEmulationStarting(EmuThread*) {
    emulation_running = true;
    UpdateTimerState();
}

void SeamCalculatorWidget::OnEmulationStopping() {
    emulation_running = false;
    UpdateTimerState();
    Update();
}

void SeamCalculatorWidget::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    UpdateTimerState();
}

void SeamCalculatorWidget::hideEvent(QHideEvent* event) {
    QDockWidget::hideEvent(event);
    UpdateTimerState();
}

void SeamCalculatorWidget::UpdateTimerState() {
    if (emulation_running && isVisible()) {
        update_timer.start();
    } else {
        update_timer.stop();
    }
}

std::optional<u32> SeamCalculatorWidget::ReadU32(VAddr address) const {
    const auto bytes = system.MemoryEditor().Peek(address, sizeof(u32));
    if (!bytes) {
        return std::nullopt;
    }
    u32 value;
    std::memcpy(&value, bytes->data(), sizeof(value));
    return value;
}

std::optional<u16> SeamCalculatorWidget::ReadU16(VAddr address) const {
    const auto bytes = system.MemoryEditor().Peek(address, sizeof(u16));
    if (!bytes) {
        return std::nullopt;
    }
    u16 value;
    std::memcpy(&value, bytes->data(), sizeof(value));
    return value;
}

std::optional<VAddr> SeamCalculatorWidget::ContextAddress() const {
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

void SeamCalculatorWidget::FindGlobalContext() {
    if (!emulation_running) {
        return;
    }
    // The main camera keeps a pointer to the GlobalContext it is part of, so the GlobalContext is
    // at the value of the one word that points to 0x364 + 0xD4 bytes before the word itself
    auto snapshot = system.MemoryEditor().SnapshotWritableMemory(std::chrono::milliseconds{2000});
    if (!snapshot) {
        QMessageBox::warning(this, tr("Seam Calculator"),
                             tr("Could not read the memory. Is the game running?"));
        return;
    }
    constexpr VAddr Offset = OffsetMainCamera + OffsetCameraGlobalContext;
    std::optional<VAddr> found;
    for (const auto& region : *snapshot) {
        for (std::size_t i = 0; i + 4 <= region.data.size() && !found; i += 4) {
            u32 value;
            std::memcpy(&value, region.data.data() + i, sizeof(value));
            if (value == 0 || region.base + i != value + Offset) {
                continue;
            }
            // Check that it holds Link's actor
            const auto player = ReadU32(value + OffsetPlayerActor);
            const auto type = player && *player
                                  ? system.MemoryEditor().Peek(*player + OffsetActorType, 1)
                                  : std::nullopt;
            if (type && (*type)[0] == ActorTypePlayer) {
                found = value;
            }
        }
    }
    if (!found) {
        QMessageBox::warning(this, tr("Seam Calculator"),
                             tr("Could not find the GlobalContext. Try again while playing (not "
                                "on the title screen or file select)."));
        return;
    }
    context_edit->setText(QStringLiteral("0x%1").arg(*found, 8, 16, QLatin1Char('0')));
    SaveSettings();
    Update();
}

std::optional<VAddr> SeamCalculatorWidget::ActorAddress() const {
    if (const auto context = ContextAddress()) {
        if (const auto player = ReadU32(*context + OffsetPlayerActor); player && *player) {
            return *player;
        }
    }
    QString text = address_edit->text().trimmed();
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

std::optional<float> SeamCalculatorWidget::ReadFloat(VAddr address) const {
    const auto bytes = system.MemoryEditor().Peek(address, sizeof(float));
    if (!bytes) {
        return std::nullopt;
    }
    float value;
    std::memcpy(&value, bytes->data(), sizeof(value));
    return value;
}

std::optional<SeamCalculatorWidget::LinkState> SeamCalculatorWidget::ReadLink() const {
    const auto address = ActorAddress();
    if (!emulation_running || !address) {
        return std::nullopt;
    }
    const auto x = ReadFloat(*address + OffsetWorldPos);
    const auto y = ReadFloat(*address + OffsetWorldPos + 4);
    const auto z = ReadFloat(*address + OffsetWorldPos + 8);
    const auto prev_y = ReadFloat(*address + OffsetPrevPos + 4);
    const auto floor_height = ReadFloat(*address + OffsetFloorHeight);
    const auto flags = system.MemoryEditor().Peek(*address + OffsetBgCheckFlags, 2);
    if (!x || !y || !z || !prev_y || !floor_height || !flags) {
        return std::nullopt;
    }
    u16 bg_check_flags;
    std::memcpy(&bg_check_flags, flags->data(), sizeof(bg_check_flags));
    const auto yaw = ReadU16(*address + OffsetWorldRotY);
    const auto speed = ReadFloat(*address + OffsetSpeedXZ);
    if (!yaw || !speed) {
        return std::nullopt;
    }

    // Input yaw of the active camera (cameraPtrs[activeCamera]->inputDir.y)
    std::optional<u16> camera_yaw;
    if (const auto context = ContextAddress()) {
        const auto active = ReadU16(*context + OffsetActiveCamera);
        if (active && *active < 4) {
            const auto camera = ReadU32(*context + OffsetCameraPtrs + *active * 4);
            if (camera && *camera) {
                camera_yaw = ReadU16(*camera + OffsetCameraInputYaw);
            }
        }
    }
    return LinkState{*x, *y, *z, *prev_y, *floor_height, bg_check_flags, *yaw, *speed, camera_yaw};
}

void SeamCalculatorWidget::LoadCollision(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Seam Calculator"), tr("Could not open %1.").arg(path));
        return;
    }
    const QByteArray data = file.readAll();
    auto triangles = SeamMath::LoadZsi(
        std::span<const u8>(reinterpret_cast<const u8*>(data.constData()), data.size()));
    if (!triangles) {
        QMessageBox::warning(this, tr("Seam Calculator"),
                             tr("%1 is not an OoT3D scene file with collision.").arg(path));
        return;
    }
    collision = std::move(*triangles);
    collision_path = path;
    const auto seams = std::count_if(collision.begin(), collision.end(),
                                     [](const auto& tri) { return tri.IsSeam(); });
    file_label->setText(tr("%1: %2 triangles, %3 seam triangles")
                            .arg(QFileInfo(path).fileName())
                            .arg(collision.size())
                            .arg(seams));
    SaveSettings();
    FindSeams();
}

void SeamCalculatorWidget::FindSeams() {
    seam_table->setRowCount(0);
    if (collision.empty()) {
        return;
    }
    const auto link = ReadLink();
    struct Found {
        const SeamMath::Triangle* tri;
        float distance;
        std::size_t vertex;
    };
    std::vector<Found> found;
    for (const auto& tri : collision) {
        if (!tri.IsSeam()) {
            continue;
        }
        Found entry{&tri, 0.0f, 0};
        if (link) {
            entry.distance = std::numeric_limits<float>::max();
            for (std::size_t v = 0; v < 3; ++v) {
                const float distance = tri.VertexDistance(v, link->x, link->z);
                if (distance < entry.distance) {
                    entry.distance = distance;
                    entry.vertex = v;
                }
            }
            if (entry.distance > radius_spin->value()) {
                continue;
            }
        }
        found.push_back(entry);
    }
    // Nearest first, or steepest first without Link's position
    std::sort(found.begin(), found.end(), [&link](const Found& a, const Found& b) {
        return link ? a.distance < b.distance : a.tri->RisePerUnit() > b.tri->RisePerUnit();
    });
    constexpr std::size_t MaxRows = 500;
    if (found.size() > MaxRows) {
        found.resize(MaxRows);
    }

    seam_table->setRowCount(static_cast<int>(found.size()));
    for (int row = 0; row < static_cast<int>(found.size()); ++row) {
        const auto& [tri, distance, vertex] = found[row];
        auto* index_item = new QTableWidgetItem(QString::number(tri->index));
        index_item->setData(Qt::UserRole, tri->index);
        seam_table->setItem(row, 0, index_item);
        seam_table->setItem(
            row, 1,
            new QTableWidgetItem(link ? QString::number(distance, 'f', 1) : QStringLiteral("-")));
        const double angle = std::acos(std::clamp(static_cast<double>(tri->Ny()), -1.0, 1.0)) *
                             180.0 / 3.14159265358979323846;
        seam_table->setItem(row, 2, new QTableWidgetItem(QString::number(angle, 'f', 3)));
        seam_table->setItem(row, 3,
                            new QTableWidgetItem(QString::number(tri->RisePerUnit(), 'f', 1)));
        seam_table->setItem(row, 4, new QTableWidgetItem(FormatVertex(tri->vertices[vertex])));
    }
    seam_table->resizeColumnsToContents();
}

void SeamCalculatorWidget::SelectTriangle(const SeamMath::Triangle& tri) {
    triangle = tri;
    for (std::size_t i = 0; i < vertex_edits.size(); ++i) {
        vertex_edits[i]->setText(FormatVertex(tri.vertices[i]));
    }
    normal_edit->setText(FormatVertex(tri.normal));
    dist_edit->setText(QString::number(tri.dist, 'g', 9));
    Update();
}

bool SeamCalculatorWidget::ParseTriangleFields() {
    SeamMath::Triangle tri;
    for (std::size_t i = 0; i < vertex_edits.size(); ++i) {
        const auto values = ParseTriple(vertex_edits[i]->text());
        if (!values) {
            triangle.reset();
            return false;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            tri.vertices[i][c] = static_cast<s16>((*values)[c]);
        }
    }
    const auto normal = ParseTriple(normal_edit->text());
    bool ok = false;
    const float dist = dist_edit->text().trimmed().toFloat(&ok);
    if (!normal || !ok) {
        triangle.reset();
        return false;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        tri.normal[c] = static_cast<s16>((*normal)[c]);
    }
    tri.dist = dist;
    // Keep the index if this is still the triangle picked from the collision
    if (triangle && triangle->vertices == tri.vertices && triangle->normal == tri.normal &&
        triangle->dist == tri.dist) {
        tri.index = triangle->index;
    }
    triangle = tri;
    return true;
}

void SeamCalculatorWidget::Update() {
    const auto link = ReadLink();
    if (link) {
        position_label->setText(QStringLiteral("X %1   Y %2   Z %3")
                                    .arg(link->x, 0, 'f', 4)
                                    .arg(link->y, 0, 'f', 3)
                                    .arg(link->z, 0, 'f', 4));
        const bool ground = link->bg_check_flags & BgCheckGround;
        floor_label->setText(tr("height %1, %2 (flags 0x%3)")
                                 .arg(link->floor_height, 0, 'f', 3)
                                 .arg(ground ? tr("on the ground") : tr("in the air"))
                                 .arg(link->bg_check_flags, 4, 16, QLatin1Char('0')));
        motion_label->setText(
            tr("speed %1, direction 0x%2, camera 0x%3")
                .arg(link->speed, 0, 'f', 3)
                .arg(link->yaw, 4, 16, QLatin1Char('0'))
                .arg(link->camera_yaw
                         ? QStringLiteral("%1").arg(*link->camera_yaw, 4, 16, QLatin1Char('0'))
                         : tr("unknown (needs the GlobalContext)")));
    } else {
        position_label->setText(emulation_running
                                    ? tr("Find the GlobalContext, or enter the address of "
                                         "Link's actor")
                                    : tr("Not running"));
        floor_label->clear();
        motion_label->clear();
    }

    target.reset();
    move_button->setEnabled(false);
    if (!triangle) {
        triangle_label->setText(
            tr("Pick a seam from the list, or enter a triangle (vertices, normal and distance as "
               "stored in the collision)."));
        vertices_label->clear();
        height_label->clear();
        game_floor_label->clear();
        band_label->clear();
        target_label->clear();
        ClearClimb();
        return;
    }

    const auto& tri = *triangle;
    const auto [ux, uz] = tri.UphillDirection();
    QString info = tri.index >= 0 ? tr("Poly %1: ").arg(tri.index) : QString{};
    if (!tri.IsStandable()) {
        info += Colored(tr("faces down or has a normal Y of zero, the floor check never uses it."),
                        "#c03030");
    } else {
        info += tr("rises %1 units per unit in XZ, uphill direction (X %2, Z %3)%4")
                    .arg(tri.RisePerUnit(), 0, 'f', 1)
                    .arg(ux, 0, 'f', 4)
                    .arg(uz, 0, 'f', 4)
                    .arg(tri.IsSeam() ? QString{} : tr(" (a floor, not a seam)"));
    }
    triangle_label->setText(info);

    if (!link || !tri.IsStandable()) {
        vertices_label->clear();
        height_label->clear();
        game_floor_label->clear();
        band_label->clear();
        target_label->clear();
        ClearClimb();
        return;
    }

    QStringList distances;
    for (std::size_t v = 0; v < 3; ++v) {
        const float distance = tri.VertexDistance(v, link->x, link->z);
        const QString text = QString::number(distance, 'f', distance < 10.0f ? 4 : 1);
        distances << (distance < SeamMath::CheckDist
                          ? Colored(text + QStringLiteral(" ✓"), "#20a020")
                          : text.toHtmlEscaped());
    }
    vertices_label->setText(distances.join(QStringLiteral(" | ")));
    UpdateClimb(tri, *link);

    const float height = tri.HeightAt(link->x, link->z);
    const float gap = height - link->y;
    const bool accepted = tri.ContainsXZ(link->x, link->z);
    if (accepted) {
        // The seam is a candidate if it's below the start of the floor ray, and Link is put on it
        // if it's also at or above his feet (and it's the highest candidate)
        const float ray_start = link->prev_y + SeamMath::FloorCheckHeight;
        QString state;
        if (height >= ray_start) {
            state = tr("too high: the floor check only sees floors below %1 (previous Y + 50)")
                        .arg(ray_start, 0, 'f', 2);
        } else if (gap >= 0.0f) {
            state =
                Colored(tr("in pop-up range, below %1 (previous Y + 50)").arg(ray_start, 0, 'f', 2),
                        "#20a020");
        } else {
            state = tr("below Link's feet");
        }
        height_label->setText(tr("height %1, %2 %3 Link's Y: %4")
                                  .arg(height, 0, 'f', 2)
                                  .arg(std::fabs(gap), 0, 'f', 2)
                                  .arg(gap >= 0.0f ? tr("above") : tr("below"))
                                  .arg(state));
    } else {
        height_label->setText(
            tr("Link is outside the seam (plane height here would be %1)").arg(height, 0, 'f', 1));
    }

    if (accepted && std::fabs(link->floor_height - height) < 0.01f) {
        game_floor_label->setText(Colored(
            tr("%1, the game's floor check found this seam").arg(link->floor_height, 0, 'f', 3),
            "#20a020"));
    } else {
        game_floor_label->setText(tr("%1").arg(link->floor_height, 0, 'f', 3));
    }

    // Where Link pops up onto the seam: from his feet up to the start of the floor check's ray
    const float band_min = link->y;
    const float band_max = link->prev_y + SeamMath::FloorCheckHeight;
    const float band_width = (band_max - band_min) / tri.RisePerUnit();
    const bool in_band = accepted && height >= band_min && height < band_max;
    band_label->setText(
        tr("heights %1 to %2, %3 units wide in XZ%4")
            .arg(band_min, 0, 'f', 2)
            .arg(band_max, 0, 'f', 2)
            .arg(band_width, 0, 'f', band_width < 0.1f ? 5 : 3)
            .arg(in_band ? QStringLiteral(" — ") + Colored(tr("Link is in it ✓"), "#20a020")
                         : QString{}));

    // Aim for the middle half of the band: at its edges, rounding of the position (almost one unit
    // of height per step here) can leave Link just outside of it
    const float margin = (band_max - band_min) / 4.0f;
    target = SeamMath::ClosestPointInHeightRange(tri, link->x, link->z, band_min + margin,
                                                 band_max - margin);
    move_button->setEnabled(target.has_value());
    if (in_band) {
        target_label->setText(tr("Link is in the band."));
    } else if (!target) {
        target_label->setText(tr("The seam doesn't reach these heights near any of its vertices."));
    } else {
        const float dx = (*target)[0] - link->x;
        const float dz = (*target)[1] - link->z;
        target_label->setText(
            tr("X %1   Z %2   (move X %3, Z %4: %5 units; seam height there %6). Walking can't "
               "make such a small move directly: use Way 1 or 2 below.")
                .arg((*target)[0], 0, 'f', 5)
                .arg((*target)[1], 0, 'f', 5)
                .arg(dx, 0, 'f', 5)
                .arg(dz, 0, 'f', 5)
                .arg(std::hypot(dx, dz), 0, 'f', 5)
                .arg(tri.HeightAt((*target)[0], (*target)[1]), 0, 'f', 2));
    }
}

void SeamCalculatorWidget::ClearClimb() {
    next_frame_label->clear();
    for (QLabel* label : direction_labels) {
        label->clear();
    }
}

void SeamCalculatorWidget::UpdateClimb(const SeamMath::Triangle& tri, const LinkState& link) {
    using namespace SeamMath;
    const double rise = tri.RisePerUnit();
    const auto [ux, uz] = tri.UphillDirection();
    const u16 uphill_yaw = YawOf(ux, uz);
    // Height gained per frame moving at `speed` along `yaw`
    const auto gain_along = [&](double speed, u16 yaw) {
        return speed * rise * std::cos(YawToRadians(YawDifference(yaw, uphill_yaw)));
    };
    const auto hex = [](u16 value) {
        return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0'));
    };

    // Link is either on the seam (climbing it), or still on other ground (getting onto it). The
    // band of seam heights Link is put on is from his feet up to his previous Y + 50.
    const float band_min = link.y;
    const float band_max = link.prev_y + FloorCheckHeight;
    const float band_middle = (band_min + band_max) / 2.0f;
    const float here = tri.HeightAt(link.x, link.z);
    const bool on_seam = tri.ContainsXZ(link.x, link.z) && (link.bg_check_flags & BgCheckGround) &&
                         std::fabs(here - link.y) < 0.5f;

    // Next frame, at Link's current speed and direction (the game moves Link by
    // speed * (sin, cos) of his direction)
    if (link.speed == 0.0f) {
        next_frame_label->setText(tr("Link isn't moving."));
    } else {
        const double yaw = YawToRadians(link.yaw);
        const float next_x = link.x + static_cast<float>(link.speed * std::sin(yaw));
        const float next_z = link.z + static_cast<float>(link.speed * std::cos(yaw));
        const float next = tri.HeightAt(next_x, next_z);
        const float gain = next - link.y;
        if (!tri.ContainsXZ(next_x, next_z)) {
            next_frame_label->setText(
                on_seam ? Colored(tr("leaves the seam's vertex circles: Link falls off"), "#c03030")
                        : tr("outside the seam's vertex circles"));
        } else if (next >= band_max) {
            next_frame_label->setText(
                on_seam ? Colored(tr("gains %1: too much (50 or more), Link falls off")
                                      .arg(gain, 0, 'f', 2),
                                  "#c03030")
                        : tr("seam %1 above the band (seam height %2)")
                              .arg(next - band_max, 0, 'f', 2)
                              .arg(next, 0, 'f', 2));
        } else if (next >= band_min) {
            next_frame_label->setText(
                on_seam ? Colored(tr("gains %1 ✓").arg(gain, 0, 'f', 2), "#20a020")
                        : Colored(tr("seam height %1 is in the band: Link gets onto the seam ✓")
                                      .arg(next, 0, 'f', 2),
                                  "#20a020"));
        } else if (!on_seam && here >= band_max) {
            next_frame_label->setText(
                Colored(tr("skips over the band (seam height %1 to %2): move more slowly across it")
                            .arg(here, 0, 'f', 1)
                            .arg(next, 0, 'f', 1),
                        "#c03030"));
        } else {
            next_frame_label->setText(on_seam ? tr("goes down %1").arg(-gain, 0, 'f', 2)
                                              : tr("seam %1 below Link's feet (seam height %2)")
                                                    .arg(band_min - next, 0, 'f', 2)
                                                    .arg(next, 0, 'f', 2));
        }
    }

    // The two ways to go: close to the seam's level line, one each way, tilted slightly uphill to
    // climb (or towards the band to get onto the seam), so that the seam height under Link changes
    // by `change` per frame. The angle from the uphill direction is acos(change / (speed * rise)).
    const double speed = std::max<double>(link.speed, plan_speed_spin->value());
    const double change = on_seam || here < band_middle ? aim_spin->value() : -aim_spin->value();
    // Most the seam height may change per frame: under 50 when climbing, and less than the band's
    // height when getting onto it (or Link could skip over it)
    const double max_change = on_seam ? FloorCheckHeight : band_max - band_min;
    const u16 aim_offset_up =
        RadiansToYaw(std::acos(std::min(1.0, std::fabs(change) / (speed * rise))));
    const u16 limit_offset_up = RadiansToYaw(std::acos(std::min(1.0, max_change / (speed * rise))));
    // Offsets from the uphill direction (0x4000 = along the level line)
    const u16 aim_offset = change > 0 ? aim_offset_up : static_cast<u16>(0x8000 - aim_offset_up);
    const u16 limit_offset = change > 0 ? static_cast<u16>(limit_offset_up + 1)
                                        : static_cast<u16>(0x8000 - limit_offset_up - 1);
    const int magnitude = stick_magnitude_spin->value();

    for (std::size_t i = 0; i < direction_labels.size(); ++i) {
        const int side = i == 0 ? 1 : -1;
        const u16 aim_yaw = static_cast<u16>(uphill_yaw + side * aim_offset);
        const u16 limit_yaw = static_cast<u16>(uphill_yaw + side * limit_offset);
        const u16 level_yaw = static_cast<u16>(uphill_yaw + side * 0x4000);
        const u16 window_from = (side > 0) == (change > 0) ? limit_yaw : level_yaw;
        const u16 window_to = (side > 0) == (change > 0) ? level_yaw : limit_yaw;

        // Frames until leaving the vertex circles (or, getting onto the seam, reaching the band)
        int frames = 0;
        bool reaches_band = false;
        double x = link.x;
        double z = link.z;
        const double step_x = speed * std::sin(YawToRadians(aim_yaw));
        const double step_z = speed * std::cos(YawToRadians(aim_yaw));
        while (frames < 100000) {
            const float next_x = static_cast<float>(x + step_x);
            const float next_z = static_cast<float>(z + step_z);
            if (!tri.ContainsXZ(next_x, next_z)) {
                break;
            }
            x += step_x;
            z += step_z;
            ++frames;
            const float height = tri.HeightAt(next_x, next_z);
            if (!on_seam && height >= band_min && height < band_max) {
                reaches_band = true;
                break;
            }
        }
        const double per_frame = gain_along(speed, aim_yaw);
        const u16 tilt = static_cast<u16>(std::abs(0x4000 - static_cast<int>(aim_offset)));
        QString text;
        if (on_seam) {
            text = tr("direction %1, %2 uphill of the level line (window %3 to %4); gains %5 "
                      "per frame; %6 frames of room (about %7 higher)")
                       .arg(hex(aim_yaw), hex(tilt), hex(window_from), hex(window_to))
                       .arg(per_frame, 0, 'f', 1)
                       .arg(frames)
                       .arg(per_frame * frames, 0, 'f', 0)
                       .toHtmlEscaped();
        } else {
            text = tr("direction %1, %2 towards the band from the level line (window %3 to %4); "
                      "seam height changes %5 per frame; %6")
                       .arg(hex(aim_yaw), hex(tilt), hex(window_from), hex(window_to))
                       .arg(per_frame, 0, 'f', 1)
                       .arg(reaches_band
                                ? tr("reaches the band in %1 frames").arg(frames)
                                : tr("leaves the vertex circles after %1 frames first").arg(frames))
                       .toHtmlEscaped();
        }
        // Whether Link's direction is in this window
        const double link_change = gain_along(speed, link.yaw);
        if ((YawDifference(link.yaw, uphill_yaw) > 0) == (side > 0) &&
            (change > 0 ? link_change > 0 : link_change < 0) &&
            std::fabs(link_change) < max_change) {
            text += QStringLiteral("<br>") +
                    Colored(tr("Link's direction is in this window ✓"), "#20a020");
        }

        // Circle pad position giving that direction: the stick angle is the direction minus the
        // camera's input yaw (Player_ProcessControlStick)
        if (link.camera_yaw) {
            const u16 stick_angle = static_cast<u16>(aim_yaw - *link.camera_yaw);
            const double angle = YawToRadians(stick_angle);
            const int ideal_x = static_cast<int>(std::lround(-magnitude * std::sin(angle)));
            const int ideal_y = static_cast<int>(std::lround(magnitude * std::cos(angle)));
            int best_x = ideal_x;
            int best_y = ideal_y;
            int best_error = std::numeric_limits<int>::max();
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const int px = ideal_x + dx;
                    const int py = ideal_y + dy;
                    if (px * px + py * py > 156 * 156) {
                        continue;
                    }
                    const int error = std::abs(YawDifference(StickAngle(px, py), stick_angle));
                    if (error < best_error) {
                        best_error = error;
                        best_x = px;
                        best_y = py;
                    }
                }
            }
            const u16 resulting_yaw =
                static_cast<u16>(*link.camera_yaw + StickAngle(best_x, best_y));
            text += QStringLiteral("<br>") +
                    tr("Circle pad X %1, Y %2 (direction %3, gains %4 per frame)")
                        .arg(best_x)
                        .arg(best_y)
                        .arg(hex(resulting_yaw))
                        .arg(gain_along(speed, resulting_yaw), 0, 'f', 1)
                        .toHtmlEscaped();
        } else {
            text += QStringLiteral("<br>") +
                    tr("Find the GlobalContext to get circle pad positions.").toHtmlEscaped();
        }
        direction_labels[i]->setText(text);
    }
}

void SeamCalculatorWidget::MoveLinkToTarget() {
    const auto address = ActorAddress();
    if (!target || !address || !emulation_running) {
        return;
    }
    auto& editor = system.MemoryEditor();
    const auto [x, z] = *target;
    // The previous position too, so the move isn't checked against walls as movement
    for (const VAddr base : {*address + OffsetWorldPos, *address + OffsetPrevPos}) {
        editor.QueueWrite(base, FloatBytes(x));
        editor.QueueWrite(base + 8, FloatBytes(z));
    }
}
