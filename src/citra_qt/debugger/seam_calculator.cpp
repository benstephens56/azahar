// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstring>
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

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(contents);
    setWidget(scroll);

    QSettings settings(SettingsPath(), QSettings::IniFormat);
    address_edit->setText(settings.value(QStringLiteral("actor_address")).toString());
    radius_spin->setValue(settings.value(QStringLiteral("radius"), 200).toInt());
    target_spin->setValue(settings.value(QStringLiteral("target_gap"), 1.0).toDouble());
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
    settings.setValue(QStringLiteral("actor_address"), address_edit->text());
    settings.setValue(QStringLiteral("radius"), radius_spin->value());
    settings.setValue(QStringLiteral("target_gap"), target_spin->value());
    settings.setValue(QStringLiteral("collision_file"), collision_path);
}

QWidget* SeamCalculatorWidget::CreateLinkGroup() {
    auto* group = new QGroupBox(tr("Link"));
    auto* form = new QFormLayout(group);
    address_edit = new QLineEdit(group);
    address_edit->setPlaceholderText(tr("Address of Link's actor, e.g. 0x0898F9A0"));
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

    target_spin = new QDoubleSpinBox(group);
    target_spin->setRange(-100000.0, 100000.0);
    target_spin->setDecimals(2);
    target_spin->setSuffix(tr(" above Link's Y"));
    target_spin->setToolTip(tr("Seam height to aim for, relative to Link's feet. Link snaps up "
                               "onto a floor that is a little above his feet."));
    connect(target_spin, &QDoubleSpinBox::valueChanged, this, [this] { Update(); });
    form->addRow(tr("Target height"), target_spin);
    target_label = new QLabel(group);
    target_label->setWordWrap(true);
    target_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Target point"), target_label);
    move_button = new QPushButton(tr("Move Link to Target (memory write, for testing)"), group);
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

std::optional<VAddr> SeamCalculatorWidget::ActorAddress() const {
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
    const auto floor_height = ReadFloat(*address + OffsetFloorHeight);
    const auto flags = system.MemoryEditor().Peek(*address + OffsetBgCheckFlags, 2);
    if (!x || !y || !z || !floor_height || !flags) {
        return std::nullopt;
    }
    u16 bg_check_flags;
    std::memcpy(&bg_check_flags, flags->data(), sizeof(bg_check_flags));
    return LinkState{*x, *y, *z, *floor_height, bg_check_flags};
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
    } else {
        position_label->setText(emulation_running ? tr("Enter the address of Link's actor")
                                                  : tr("Not running"));
        floor_label->clear();
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
        target_label->clear();
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
        target_label->clear();
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

    const float height = tri.HeightAt(link->x, link->z);
    const float gap = height - link->y;
    const bool accepted = tri.ContainsXZ(link->x, link->z);
    if (accepted) {
        QString state;
        if (gap > 0.0f) {
            state = tr("above Link's feet: he snaps up onto it if it's within his step-up reach");
        } else {
            state = tr("below Link's feet");
        }
        height_label->setText(tr("height %1, %2 %3 Link's Y (%4)")
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

    const float target_height = link->y + static_cast<float>(target_spin->value());
    target = SeamMath::ClosestPointAtHeight(tri, link->x, link->z, target_height);
    if (!target) {
        target_label->setText(tr("The seam doesn't reach height %1 near any of its vertices.")
                                  .arg(target_height, 0, 'f', 2));
        return;
    }
    const float dx = (*target)[0] - link->x;
    const float dz = (*target)[1] - link->z;
    target_label->setText(tr("X %1   Z %2   (move X %3, Z %4: %5 units; seam height there %6)")
                              .arg((*target)[0], 0, 'f', 4)
                              .arg((*target)[1], 0, 'f', 4)
                              .arg(dx, 0, 'f', 4)
                              .arg(dz, 0, 'f', 4)
                              .arg(std::hypot(dx, dz), 0, 'f', 4)
                              .arg(tri.HeightAt((*target)[0], (*target)[1]), 0, 'f', 2));
    move_button->setEnabled(true);
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
