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
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
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
constexpr VAddr OffsetFloorPoly = 0x07C;
/// colCtx (0xA98) .stat.colHeader (0)
constexpr VAddr OffsetCollisionHeader = 0xA98;
constexpr VAddr OffsetHeaderNumPolygons = 0x0E;
constexpr VAddr OffsetHeaderPolyList = 0x1C;
constexpr u32 CollisionPolySize = 0x14;
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
    layout->addWidget(CreateSetupGroup());
    layout->addWidget(CreateMountGroup());
    layout->addWidget(CreateClimbGroup());
    layout->addStretch();

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(contents);
    setWidget(scroll);

    QSettings settings(SettingsPath(), QSettings::IniFormat);
    context_edit->setText(settings.value(QStringLiteral("global_context")).toString());
    address_edit->setText(settings.value(QStringLiteral("actor_address")).toString());
    radius_spin->setValue(settings.value(QStringLiteral("radius"), 200).toInt());
    climb_rate_spin->setValue(settings.value(QStringLiteral("climb_rate"), 45.0).toDouble());
    climb_step_spin->setValue(settings.value(QStringLiteral("climb_step"), 0.03).toDouble());
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
    settings.setValue(QStringLiteral("radius"), radius_spin->value());
    settings.setValue(QStringLiteral("climb_rate"), climb_rate_spin->value());
    settings.setValue(QStringLiteral("climb_step"), climb_step_spin->value());
    settings.setValue(QStringLiteral("collision_file"), collision_path);
}

QWidget* SeamCalculatorWidget::CreateSetupGroup() {
    auto* group = new QGroupBox(tr("Setup"));
    auto* layout = new QVBoxLayout(group);

    auto* context_row = new QHBoxLayout;
    context_row->addWidget(new QLabel(tr("GlobalContext"), group));
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
    layout->addLayout(context_row);

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
    radius_spin->setSuffix(tr(" units"));
    find_row->addWidget(radius_spin);
    auto* find_button = new QPushButton(tr("Find"), group);
    connect(find_button, &QPushButton::clicked, this, &SeamCalculatorWidget::FindSeams);
    find_row->addWidget(find_button);
    find_row->addStretch();
    auto* details_button = new QPushButton(tr("Details"), group);
    details_button->setCheckable(true);
    details_button->setToolTip(tr("Shows the selected triangle's data (editable) and the address "
                                  "of Link's actor"));
    find_row->addWidget(details_button);
    layout->addLayout(find_row);

    seam_table = new QTableWidget(0, 5, group);
    seam_table->setHorizontalHeaderLabels(
        {tr("Poly"), tr("Distance"), tr("Angle"), tr("Rise / unit"), tr("Nearest vertex")});
    seam_table->verticalHeader()->setVisible(false);
    seam_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    seam_table->setSelectionMode(QAbstractItemView::SingleSelection);
    seam_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    seam_table->horizontalHeader()->setStretchLastSection(true);
    seam_table->setMinimumHeight(100);
    seam_table->setMaximumHeight(200);
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

    triangle_label = new QLabel(group);
    triangle_label->setWordWrap(true);
    layout->addWidget(triangle_label);

    details_widget = new QWidget(group);
    auto* form = new QFormLayout(details_widget);
    form->setContentsMargins(0, 0, 0, 0);
    for (std::size_t i = 0; i < vertex_edits.size(); ++i) {
        vertex_edits[i] = new QLineEdit(details_widget);
        vertex_edits[i]->setPlaceholderText(QStringLiteral("x, y, z"));
        form->addRow(tr("Vertex %1").arg(i + 1), vertex_edits[i]);
    }
    normal_edit = new QLineEdit(details_widget);
    normal_edit->setPlaceholderText(tr("x, y, z (as stored, e.g. -12161, 4, 30426)"));
    form->addRow(tr("Normal"), normal_edit);
    dist_edit = new QLineEdit(details_widget);
    dist_edit->setPlaceholderText(tr("e.g. -477.623"));
    form->addRow(tr("Distance"), dist_edit);
    for (QLineEdit* edit :
         {vertex_edits[0], vertex_edits[1], vertex_edits[2], normal_edit, dist_edit}) {
        connect(edit, &QLineEdit::editingFinished, this, [this] {
            ParseTriangleFields();
            Update();
        });
    }
    address_edit = new QLineEdit(details_widget);
    address_edit->setPlaceholderText(tr("Only needed without the GlobalContext"));
    address_edit->setToolTip(tr("The start of the player actor. Link's X position is at +0x28, "
                                "Y at +0x2C and Z at +0x30 from it."));
    connect(address_edit, &QLineEdit::editingFinished, this, [this] {
        SaveSettings();
        Update();
    });
    form->addRow(tr("Link's actor"), address_edit);
    details_widget->setVisible(false);
    connect(details_button, &QPushButton::toggled, details_widget, &QWidget::setVisible);
    layout->addWidget(details_widget);
    return group;
}

QWidget* SeamCalculatorWidget::CreateMountGroup() {
    auto* group = new QGroupBox(tr("Get onto the seam"));
    auto* layout = new QVBoxLayout(group);
    mount_status_label = new QLabel(group);
    mount_status_label->setWordWrap(true);
    QFont font = mount_status_label->font();
    font.setPointSizeF(font.pointSizeF() * 1.2);
    font.setBold(true);
    mount_status_label->setFont(font);
    layout->addWidget(mount_status_label);

    // Target, Link and the difference, for X and Z
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    const QStringList columns{tr("Target"), tr("Link"), tr("Move")};
    for (int column = 0; column < 3; ++column) {
        auto* header = new QLabel(QStringLiteral("<b>%1</b>").arg(columns[column]), group);
        grid->addWidget(header, 0, column + 1, Qt::AlignRight);
    }
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (int row = 0; row < 2; ++row) {
        grid->addWidget(new QLabel(row == 0 ? tr("X") : tr("Z"), group), row + 1, 0);
        for (int column = 0; column < 3; ++column) {
            auto* cell = new QLabel(group);
            cell->setFont(fixed);
            cell->setTextInteractionFlags(Qt::TextSelectableByMouse);
            grid->addWidget(cell, row + 1, column + 1, Qt::AlignRight);
            target_cells[row * 3 + column] = cell;
        }
    }
    grid->setColumnStretch(4, 1);
    layout->addLayout(grid);

    auto* form = new QFormLayout;
    angle_label = new QLabel(group);
    angle_label->setFont(fixed);
    angle_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    angle_label->setToolTip(tr("Direction from Link to the target"));
    form->addRow(tr("Angle"), angle_label);
    layout->addLayout(form);

    move_button = new QPushButton(tr("Move Link to the Target (memory write, for testing)"), group);
    move_button->setToolTip(
        tr("Writes the target X and Z to Link's position (and previous position). This is not an "
           "input: a movie recorded with it will not play back the same."));
    connect(move_button, &QPushButton::clicked, this, &SeamCalculatorWidget::MoveLinkToTarget);
    layout->addWidget(move_button);
    return group;
}

QWidget* SeamCalculatorWidget::CreateClimbGroup() {
    auto* group = new QGroupBox(tr("Climb"));
    auto* layout = new QVBoxLayout(group);
    auto* form = new QFormLayout;
    const auto make_label = [group] {
        auto* label = new QLabel(group);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    next_frame_label = make_label();
    next_frame_label->setToolTip(tr("What Link's current speed and direction do next frame. He "
                                    "falls off if he gains 50 or more."));
    form->addRow(tr("Next frame"), next_frame_label);
    remaining_label = make_label();
    remaining_label->setToolTip(
        tr("How many more frames Link stays on the seam going on like last frame (same direction "
           "and distance per frame), and the height he gets to"));
    form->addRow(tr("Remaining"), remaining_label);
    best_line_label = make_label();
    best_line_label->setToolTip(tr("The straight line from Link's position that gains the most "
                                   "height, at the climb speed and slowest step below"));
    form->addRow(tr("Best line"), best_line_label);
    options_label = make_label();
    options_label->setToolTip(
        tr("The best line for other slowest steps (XZ distance Link moves per frame). The lines "
           "for steps smaller than Link can actually move make him walk off the seam: he gains 50 "
           "or more in his first frame on them, so the floor check doesn't see the seam."));
    form->addRow(tr("Slower steps"), options_label);
    layout->addLayout(form);

    auto* settings_form = new QFormLayout;
    climb_rate_spin = new QDoubleSpinBox(group);
    climb_rate_spin->setRange(1.0, 49.9);
    climb_rate_spin->setDecimals(1);
    climb_rate_spin->setSuffix(tr(" per frame"));
    climb_rate_spin->setToolTip(tr("How much height you want Link to gain per frame. He falls "
                                   "off at 50 or more."));
    connect(climb_rate_spin, &QDoubleSpinBox::valueChanged, this, [this] { Update(); });
    settings_form->addRow(tr("Climb speed (Y)"), climb_rate_spin);

    auto* step_row = new QHBoxLayout;
    climb_step_spin = new QDoubleSpinBox(group);
    climb_step_spin->setRange(0.0001, 10.0);
    climb_step_spin->setDecimals(4);
    climb_step_spin->setSingleStep(0.005);
    climb_step_spin->setSuffix(tr(" per frame"));
    climb_step_spin->setToolTip(
        tr("The smallest distance (in XZ) you can make Link move per frame while climbing. The "
           "slower he moves, the steeper (and higher) the lines he can climb at the climb speed."));
    connect(climb_step_spin, &QDoubleSpinBox::valueChanged, this, [this] { Update(); });
    step_row->addWidget(climb_step_spin);
    auto* use_step_button = new QPushButton(tr("Use Last Frame's"), group);
    use_step_button->setToolTip(tr("Takes the XZ distance Link moved last frame"));
    connect(use_step_button, &QPushButton::clicked, this, [this] {
        if (const auto link = ReadLink()) {
            const double step = std::hypot(static_cast<double>(link->x) - link->prev_x,
                                           static_cast<double>(link->z) - link->prev_z);
            if (step > 0.0) {
                climb_step_spin->setValue(step);
            }
        }
    });
    step_row->addWidget(use_step_button);
    settings_form->addRow(tr("Slowest step (XZ)"), step_row);
    layout->addLayout(settings_form);
    return group;
}

double SeamCalculatorWidget::ClimbMaxRise() const {
    // Moving the slowest step per frame, Link gains the climb speed per frame on lines rising this
    // much per unit
    return climb_rate_spin->value() / climb_step_spin->value();
}

void SeamCalculatorWidget::ClearLive() {
    for (QLabel* label : {mount_status_label, angle_label, next_frame_label, remaining_label,
                          best_line_label, options_label}) {
        label->clear();
    }
    for (QLabel* cell : target_cells) {
        cell->clear();
    }
    target.reset();
    move_button->setEnabled(false);
}

void SeamCalculatorWidget::Update() {
    const auto link = ReadLink();
    if (link) {
        UpdateStepRatio(*link);
    }
    if (!triangle) {
        triangle_label->setText(tr("Pick a seam from the list."));
        ClearLive();
        return;
    }
    const auto& tri = *triangle;
    QString info = tri.index >= 0 ? tr("Poly %1: ").arg(tri.index) : QString{};
    if (!tri.IsStandable()) {
        info += tr("faces down or has a normal Y of zero, the floor check never uses it.");
    } else {
        info += tr("rises %1 per unit in XZ").arg(tri.RisePerUnit(), 0, 'f', 1);
        if (link) {
            QStringList distances;
            for (std::size_t v = 0; v < 3; ++v) {
                distances << QString::number(tri.VertexDistance(v, link->x, link->z), 'f', 3);
            }
            info += tr("; Link is %1 from its vertices (on it within 1)")
                        .arg(distances.join(QStringLiteral(" / ")));
        } else if (emulation_running) {
            info += tr("; find the GlobalContext to see Link");
        }
    }
    triangle_label->setText(info);

    if (!link || !tri.IsStandable()) {
        ClearLive();
        return;
    }
    UpdateMount(tri, *link);
    UpdateClimb(tri, *link);
}

void SeamCalculatorWidget::UpdateStepRatio(const LinkState& link) {
    // Distance Link moved last frame per unit of speed (the game sets prevPos to pos before
    // moving him), for predicting the next frame
    const double step = std::hypot(static_cast<double>(link.x) - link.prev_x,
                                   static_cast<double>(link.z) - link.prev_z);
    if (step > 0.0 && link.speed > 0.001f && (link.bg_check_flags & BgCheckGround)) {
        const double ratio = step / link.speed;
        if (ratio > 0.25 && ratio < 4.0) {
            step_ratio = std::round(ratio * 20.0) / 20.0;
        }
    }
}

void SeamCalculatorWidget::UpdateMount(const SeamMath::Triangle& tri, const LinkState& link) {
    using namespace SeamMath;
    const auto hex = [](u16 value) {
        return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0'));
    };
    const float here = tri.HeightAt(link.x, link.z);
    const bool inside = tri.ContainsXZ(link.x, link.z);
    const bool on_seam = inside && (link.bg_check_flags & BgCheckGround) &&
                         std::fabs(here - link.y) < 0.5f &&
                         std::fabs(link.floor_height - here) < 0.01f;

    if (on_seam) {
        mount_status_label->setText(Colored(tr("Link is on the seam ✓"), "#20a020"));
    } else if (inside) {
        const float gap = here - link.y;
        mount_status_label->setText(tr("Seam is %1 %2 Link's feet (target: 0.00)")
                                        .arg(std::fabs(gap), 0, 'f', 2)
                                        .arg(gap >= 0.0f ? tr("above") : tr("below"))
                                        .toHtmlEscaped());
    } else {
        mount_status_label->setText(tr("Link is outside the seam").toHtmlEscaped());
    }

    // Where the seam is right at the height of the floor Link stands on (Link only reliably gets
    // onto a seam there, not anywhere he'd be moved up onto it). With the collision loaded, that's
    // where the seam's plane meets the floor's plane: a fixed spot that doesn't depend on where
    // Link is. Without it, the closest spot where the seam is at Link's current height.
    const SeamMath::Triangle* floor = nullptr;
    target.reset();
    // With the collision, prefer the spot with the straight line that climbs the most at the
    // climbing speed
    std::optional<MountClimb> climb;
    if (!on_seam) {
        const std::array<double, 4> key{static_cast<double>(tri.index), tri.dist,
                                        static_cast<double>(collision.size()), ClimbMaxRise()};
        if (key != mount_climb_key) {
            mount_climb_key = key;
            mount_climb = BestMountClimb(tri, collision, ClimbMaxRise());
        }
        climb = mount_climb;
    }
    if (climb) {
        target = climb->spot.point;
        const auto it = std::find_if(collision.begin(), collision.end(), [&](const auto& other) {
            return other.index == climb->spot.floor_index;
        });
        floor = it != collision.end() ? &*it : nullptr;
    } else if (!on_seam) {
        if (const auto spot = FindMountSpot(tri, collision, link.x, link.z)) {
            target = spot->point;
            const auto it =
                std::find_if(collision.begin(), collision.end(),
                             [&](const auto& other) { return other.index == spot->floor_index; });
            floor = it != collision.end() ? &*it : nullptr;
        } else {
            target = ClosestPointInHeightRange(tri, link.x, link.z, link.y, link.y);
        }
    }
    // Height Link would be at, standing at (x, z)
    const auto feet_at = [&](float x, float z) { return floor ? floor->HeightAt(x, z) : link.y; };
    if (target) {
        // Positions are floats: near the target, each step of X or Z can change a steep seam's
        // height by almost a unit. Pick the nearby position (as stored) where the seam is closest
        // to Link's feet without being below them.
        const float tx = (*target)[0];
        const float tz = (*target)[1];
        std::optional<std::array<float, 2>> best;
        float best_gap = 0.0f;
        float best_distance = 0.0f;
        float x = tx;
        for (int i = 0; i < 6; ++i) {
            x = std::nextafter(x, -std::numeric_limits<float>::infinity());
        }
        for (int i = 0; i < 13;
             ++i, x = std::nextafter(x, std::numeric_limits<float>::infinity())) {
            float z = tz;
            for (int j = 0; j < 6; ++j) {
                z = std::nextafter(z, -std::numeric_limits<float>::infinity());
            }
            for (int j = 0; j < 13;
                 ++j, z = std::nextafter(z, std::numeric_limits<float>::infinity())) {
                const float gap = tri.HeightAt(x, z) - feet_at(x, z);
                const float distance = std::hypot(x - tx, z - tz);
                if (gap < 0.0f || !tri.ContainsXZ(x, z)) {
                    continue;
                }
                if (!best || gap < best_gap - 0.01f ||
                    (gap < best_gap + 0.01f && distance < best_distance)) {
                    best = std::array<float, 2>{x, z};
                    best_gap = gap;
                    best_distance = distance;
                }
            }
        }
        target = best;
    }
    move_button->setEnabled(target.has_value());
    const auto number = [](double value, bool sign = false) {
        QString text = QString::number(value, 'f', 4);
        if (sign && value >= 0.0) {
            text.prepend(QLatin1Char('+'));
        }
        return text;
    };
    target_cells[1]->setText(number(link.x));
    target_cells[4]->setText(number(link.z));
    if (!target) {
        for (const std::size_t i : {0, 2, 3, 5}) {
            target_cells[i]->setText(QStringLiteral("-"));
        }
        angle_label->setText(on_seam ? QString{} : tr("No spot to get on from"));
        return;
    }
    const float tx = (*target)[0];
    const float tz = (*target)[1];
    const float dx = tx - link.x;
    const float dz = tz - link.z;
    target_cells[0]->setText(number(tx));
    target_cells[2]->setText(number(dx, true));
    target_cells[3]->setText(number(tz));
    target_cells[5]->setText(number(dz, true));
    angle_label->setText(hex(YawOf(dx, dz)));
}

void SeamCalculatorWidget::UpdateClimb(const SeamMath::Triangle& tri, const LinkState& link) {
    using namespace SeamMath;
    const auto hex = [](u16 value) {
        return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0'));
    };
    const double rise = tri.RisePerUnit();
    const auto [ux, uz] = tri.UphillDirection();
    const float here = tri.HeightAt(link.x, link.z);
    const bool on_seam = tri.ContainsXZ(link.x, link.z) && (link.bg_check_flags & BgCheckGround) &&
                         std::fabs(here - link.y) < 0.5f;

    // Next frame, at Link's current speed and direction (the game moves Link by
    // speed * (sin, cos) of his direction)
    if (!on_seam) {
        next_frame_label->setText(tr("Link isn't on the seam."));
    } else if (link.speed == 0.0f) {
        next_frame_label->setText(tr("Link isn't moving."));
    } else {
        const double yaw = YawToRadians(link.yaw);
        const double step = link.speed * step_ratio;
        const float next_x = link.x + static_cast<float>(step * std::sin(yaw));
        const float next_z = link.z + static_cast<float>(step * std::cos(yaw));
        const float gain = tri.HeightAt(next_x, next_z) - link.y;
        if (!tri.ContainsXZ(next_x, next_z)) {
            next_frame_label->setText(Colored(tr("leaves the seam: Link falls off"), "#c03030"));
        } else if (gain >= FloorCheckHeight) {
            next_frame_label->setText(
                Colored(tr("gains %1: too much (50 or more), Link falls off").arg(gain, 0, 'f', 2),
                        "#c03030"));
        } else if (gain >= 0.0f) {
            next_frame_label->setText(Colored(tr("gains %1 ✓").arg(gain, 0, 'f', 2), "#20a020"));
        } else {
            next_frame_label->setText(tr("goes down %1").arg(-gain, 0, 'f', 2).toHtmlEscaped());
        }
    }

    // Frames left going on like last frame: the game set prevPos to pos before moving Link
    const double move_x = static_cast<double>(link.x) - link.prev_x;
    const double move_z = static_cast<double>(link.z) - link.prev_z;
    const double step = std::hypot(move_x, move_z);
    if (!on_seam) {
        remaining_label->clear();
    } else if (step == 0.0) {
        remaining_label->setText(tr("Link isn't moving."));
    } else {
        const double dir_x = move_x / step;
        const double dir_z = move_z / step;
        const double rise_along = rise * (dir_x * ux + dir_z * uz);
        const double per_frame = rise_along * step;
        const double length = StraightLineLength(tri, link.x, link.z, dir_x, dir_z);
        const double frames = std::floor(length / step);
        if (per_frame >= FloorCheckHeight) {
            remaining_label->setText(
                Colored(tr("Too fast: +%1 per frame, Link walks off").arg(per_frame, 0, 'f', 1),
                        "#c03030"));
        } else {
            remaining_label->setText(tr("<b>%1 frames</b> (to Y %2, %3%4 per frame)")
                                         .arg(frames, 0, 'f', 0)
                                         .arg(link.y + rise_along * frames * step, 0, 'f', 0)
                                         .arg(per_frame >= 0.0 ? QStringLiteral("+") : QString{})
                                         .arg(per_frame, 0, 'f', 1));
        }
    }

    // The straight lines that gain the most height: how far Link can walk before leaving the
    // seam (its triangle or vertex circles), times how much the seam rises per unit in that
    // direction. Link falls off if he gains 50 or more in a frame, so the steeper the line, the
    // slower he has to walk it.
    const std::array<double, 4> key{static_cast<double>(tri.index), tri.dist, link.x, link.z};
    if (key != climb_lines_key) {
        climb_lines_key = key;
        climb_lines = AllClimbLines(tri, link.x, link.z);
    }
    const double rate = climb_rate_spin->value();
    const auto best = PickClimbLine(climb_lines, ClimbMaxRise());
    if (!best) {
        best_line_label->setText(
            (tri.ContainsXZ(link.x, link.z)
                 ? tr("No straight line climbs from here.")
                 : tr("Link is off the seam: get on it first, then the best line shows here."))
                .toHtmlEscaped());
        options_label->clear();
        return;
    }
    const double rise_along = best->gain / best->length;
    QString text = tr("<b>%1</b>: +%2 over %3 units, +%4 per frame at a step of %5")
                       .arg(hex(best->yaw))
                       .arg(best->gain, 0, 'f', 0)
                       .arg(best->length, 0, 'f', 3)
                       .arg(rise_along * climb_step_spin->value(), 0, 'f', 1)
                       .arg(climb_step_spin->value());
    text += QStringLiteral("<br>") +
            tr("Ends at height %1").arg(link.y + best->gain, 0, 'f', 0).toHtmlEscaped();
    best_line_label->setText(text);

    // The best lines for slower steps, to see what walking slower would give. Ones slower than
    // the slowest step are greyed out: at the steps Link can actually move, they gain 50 or more
    // per frame and he walks off.
    const double slowest = climb_step_spin->value();
    QStringList options;
    std::optional<u16> previous;
    for (const double option_step : {0.2, 0.1, 0.05, 0.03, 0.02, 0.01, 0.005, 0.002}) {
        const auto line = PickClimbLine(climb_lines, rate / option_step);
        if (!line || line->yaw == previous) {
            continue;
        }
        previous = line->yaw;
        const QString row = tr("step %1: %2 +%3")
                                .arg(option_step)
                                .arg(hex(line->yaw))
                                .arg(line->gain, 0, 'f', 0)
                                .toHtmlEscaped();
        options << (option_step < slowest * 0.999 ? Colored(row, "#909090") : row);
    }
    options_label->setText(options.join(QStringLiteral("<br>")));
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
    const auto prev_x = ReadFloat(*address + OffsetPrevPos);
    const auto prev_y = ReadFloat(*address + OffsetPrevPos + 4);
    const auto prev_z = ReadFloat(*address + OffsetPrevPos + 8);
    const auto floor_height = ReadFloat(*address + OffsetFloorHeight);
    const auto flags = system.MemoryEditor().Peek(*address + OffsetBgCheckFlags, 2);
    if (!x || !y || !z || !prev_x || !prev_y || !prev_z || !floor_height || !flags) {
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

    // Index of Link's floor triangle: floorPoly points into the scene collision's polygon list
    std::optional<int> floor_poly;
    if (const auto context = ContextAddress()) {
        const auto poly = ReadU32(*address + OffsetFloorPoly);
        const auto header = ReadU32(*context + OffsetCollisionHeader);
        if (poly && *poly && header && *header) {
            const auto poly_list = ReadU32(*header + OffsetHeaderPolyList);
            const auto count = ReadU16(*header + OffsetHeaderNumPolygons);
            if (poly_list && count && *poly >= *poly_list &&
                (*poly - *poly_list) % CollisionPolySize == 0 &&
                (*poly - *poly_list) / CollisionPolySize < *count) {
                floor_poly = static_cast<int>((*poly - *poly_list) / CollisionPolySize);
            }
        }
    }
    return LinkState{
        *x,   *y,     *z,         *prev_x,   *prev_y, *prev_z, *floor_height, bg_check_flags,
        *yaw, *speed, camera_yaw, floor_poly};
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
