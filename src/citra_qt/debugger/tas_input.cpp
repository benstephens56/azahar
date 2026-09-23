// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include "citra_qt/debugger/tas_input.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"

namespace {

/// Stick range in HID units. The circle pad reaches about 0x9A and the c-stick 0x9C.
constexpr int StickRange = 0x9C;
constexpr int CirclePadMax = 0x9A;
constexpr int CStickMax = 0x9C;
constexpr int TouchWidth = static_cast<int>(Core::kScreenBottomWidth);
constexpr int TouchHeight = static_cast<int>(Core::kScreenBottomHeight);
/// Motion ranges in HID units (512 per g for the accelerometer, 14.375 per degree per second for
/// the gyroscope)
constexpr int AccelRange = 2048;
constexpr int GyroRange = 32767;

/// Profile button for each override button
constexpr std::array<int, Core::InputOverride::NumButtons> ProfileButtons{
    Settings::NativeButton::A,    Settings::NativeButton::B,     Settings::NativeButton::X,
    Settings::NativeButton::Y,    Settings::NativeButton::Up,    Settings::NativeButton::Down,
    Settings::NativeButton::Left, Settings::NativeButton::Right, Settings::NativeButton::L,
    Settings::NativeButton::R,    Settings::NativeButton::Start, Settings::NativeButton::Select,
    Settings::NativeButton::ZL,   Settings::NativeButton::ZR,
};

constexpr std::array<const char*, Core::InputOverride::NumButtons> ButtonNames{
    "A", "B", "X", "Y", "Up", "Down", "Left", "Right", "L", "R", "Start", "Select", "ZL", "ZR",
};

/// Marks widgets whose value is set in the window, as opposed to showing live input
void SetOverriddenStyle(QWidget* widget, bool overridden) {
    const bool current = widget->property("tas_overridden").toBool();
    if (current == overridden) {
        return;
    }
    widget->setProperty("tas_overridden", overridden);
    QFont font = widget->font();
    font.setBold(overridden);
    widget->setFont(font);
}

void SetSpinValue(QSpinBox* spin, int value) {
    // Don't overwrite what the user is typing
    if (!spin->hasFocus() && spin->value() != value) {
        spin->setValue(value);
    }
}

} // namespace

TasInputPad::TasInputPad(Shape shape_, int min_x_, int max_x_, int min_y_, int max_y_,
                         bool invert_y_, QWidget* parent)
    : QWidget(parent), shape{shape_}, min_x{min_x_}, max_x{max_x_}, min_y{min_y_}, max_y{max_y_},
      invert_y{invert_y_} {
    setMinimumSize(sizeHint());
    setCursor(Qt::CrossCursor);
    setToolTip(tr("Left click or drag to set, right click to clear"));
}

void TasInputPad::SetLimit(double fraction) {
    limit = std::clamp(fraction, 0.0, 1.0);
    update();
}

QSize TasInputPad::sizeHint() const {
    return shape == Shape::Circle ? QSize(130, 130)
                                  : QSize(TouchWidth / 2 + 2, TouchHeight / 2 + 2);
}

void TasInputPad::SetPoint(std::optional<std::pair<int, int>> new_point, bool new_overridden) {
    if (point == new_point && overridden == new_overridden) {
        return;
    }
    point = new_point;
    overridden = new_overridden;
    update();
}

QRectF TasInputPad::PadRect() const {
    const QRectF area = QRectF(rect()).adjusted(1, 1, -1, -1);
    // Keep the aspect ratio of the input range
    const double aspect = static_cast<double>(max_x - min_x) / (max_y - min_y);
    double w = area.width();
    double h = w / aspect;
    if (h > area.height()) {
        h = area.height();
        w = h * aspect;
    }
    return QRectF(area.center().x() - w / 2, area.center().y() - h / 2, w, h);
}

void TasInputPad::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF pad = PadRect();
    const QPalette& pal = palette();

    painter.setPen(QPen(pal.color(QPalette::Mid), 1));
    painter.setBrush(pal.color(QPalette::Base));
    if (shape == Shape::Circle) {
        painter.drawEllipse(pad);
    } else {
        painter.drawRect(pad);
    }

    // Crosshair through the center
    painter.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DashLine));
    painter.drawLine(QPointF(pad.left(), pad.center().y()), QPointF(pad.right(), pad.center().y()));
    painter.drawLine(QPointF(pad.center().x(), pad.top()), QPointF(pad.center().x(), pad.bottom()));

    // Circle the stick is limited to by its maximum output
    if (shape == Shape::Circle && limit < 1.0) {
        painter.setPen(QPen(pal.color(QPalette::Highlight), 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(pad.center(), pad.width() / 2 * limit, pad.height() / 2 * limit);
    }

    if (!point) {
        return;
    }
    const double fx = static_cast<double>(point->first - min_x) / (max_x - min_x);
    double fy = static_cast<double>(point->second - min_y) / (max_y - min_y);
    if (invert_y) {
        fy = 1.0 - fy;
    }
    const QPointF pos(pad.left() + fx * pad.width(), pad.top() + fy * pad.height());
    const QColor color = overridden ? QColor(220, 50, 50) : pal.color(QPalette::Highlight);

    painter.setPen(QPen(color, 1));
    if (shape == Shape::Circle) {
        painter.drawLine(pad.center(), pos);
    } else {
        // The touch point is shown with a crosshair
        painter.drawLine(QPointF(pad.left(), pos.y()), QPointF(pad.right(), pos.y()));
        painter.drawLine(QPointF(pos.x(), pad.top()), QPointF(pos.x(), pad.bottom()));
    }
    painter.setBrush(color);
    painter.drawEllipse(pos, 4, 4);
}

void TasInputPad::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        emit Cleared();
    } else if (event->button() == Qt::LeftButton) {
        SetFromMouse(event->position());
    }
}

void TasInputPad::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        SetFromMouse(event->position());
    }
}

void TasInputPad::SetFromMouse(const QPointF& pos) {
    const QRectF pad = PadRect();
    double fx = (pos.x() - pad.left()) / pad.width();
    double fy = (pos.y() - pad.top()) / pad.height();
    if (shape == Shape::Circle) {
        // Keep the point inside the circle
        const double dx = fx - 0.5;
        const double dy = fy - 0.5;
        const double length = std::sqrt(dx * dx + dy * dy);
        const double max_length = 0.5 * limit;
        if (length > max_length) {
            fx = 0.5 + (length > 0 ? dx / length * max_length : 0);
            fy = 0.5 + (length > 0 ? dy / length * max_length : 0);
        }
    }
    fx = std::clamp(fx, 0.0, 1.0);
    fy = std::clamp(fy, 0.0, 1.0);
    if (invert_y) {
        fy = 1.0 - fy;
    }
    const int x = static_cast<int>(std::lround(min_x + fx * (max_x - min_x)));
    const int y = static_cast<int>(std::lround(min_y + fy * (max_y - min_y)));
    emit PointSet(std::clamp(x, min_x, max_x), std::clamp(y, min_y, max_y));
}

TasInputWidget::TasInputWidget(Core::System& system_, QWidget* parent)
    : QDockWidget(tr("TAS Input"), parent), system{system_} {
    setObjectName(QStringLiteral("TasInputWidget"));

    auto* contents = new QWidget(this);
    auto* layout = new QVBoxLayout(contents);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* clear_all = new QPushButton(tr("Clear All"), contents);
    clear_all->setToolTip(tr("Clear every input set in this window"));
    connect(clear_all, &QPushButton::clicked, this, &TasInputWidget::ClearAll);
    layout->addWidget(clear_all);

    // First row: buttons and touch screen
    auto* top_row = new QHBoxLayout();
    top_row->addWidget(CreateButtonsGroup());
    top_row->addWidget(CreateTouchGroup());
    layout->addLayout(top_row);

    // Second row: sticks and motion (accelerometer above gyroscope)
    auto* bottom_row = new QHBoxLayout();
    circle_pad_controls.id = Override::StickId::CirclePad;
    c_stick_controls.id = Override::StickId::CStick;
    bottom_row->addWidget(
        CreateStickGroup(tr("Circle Pad"), circle_pad_controls, &Override::State::circle_pad));
    bottom_row->addWidget(
        CreateStickGroup(tr("C-Stick"), c_stick_controls, &Override::State::c_stick));
    auto* motion_column = new QVBoxLayout();
    motion_column->addWidget(CreateMotionGroup(tr("Accelerometer"), accel_controls,
                                               &Override::State::accel, AccelRange));
    motion_column->addWidget(
        CreateMotionGroup(tr("Gyroscope"), gyro_controls, &Override::State::gyro, GyroRange));
    bottom_row->addLayout(motion_column);
    layout->addLayout(bottom_row);

    auto* note = new QLabel(tr("Bold values are set in this window and take priority over the "
                               "controller. Right click a pad to clear it."),
                            contents);
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(contents);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    setWidget(scroll);

    refresh_timer.setInterval(16);
    connect(&refresh_timer, &QTimer::timeout, this, &TasInputWidget::Refresh);
}

TasInputWidget::~TasInputWidget() = default;

QWidget* TasInputWidget::CreateButtonsGroup() {
    auto* group = new QGroupBox(tr("Buttons"));
    auto* grid = new QGridLayout(group);
    // Button positions in the grid
    // Button positions in the grid, laid out like the console: D-pad on the left, face buttons
    // on the right
    constexpr std::array<std::pair<int, int>, Override::NumButtons> positions{{
        {2, 5}, // A
        {3, 4}, // B
        {1, 4}, // X
        {2, 3}, // Y
        {1, 1}, // Up
        {3, 1}, // Down
        {2, 0}, // Left
        {2, 2}, // Right
        {0, 0}, // L
        {0, 5}, // R
        {4, 4}, // Start
        {4, 1}, // Select
        {0, 1}, // ZL
        {0, 4}, // ZR
    }};
    for (u32 i = 0; i < Override::NumButtons; ++i) {
        auto* check = new QCheckBox(QString::fromLatin1(ButtonNames[i]), group);
        connect(check, &QCheckBox::clicked, this, [this, i] {
            overrides.buttons ^= 1u << i;
            Apply();
        });
        grid->addWidget(check, positions[i].first, positions[i].second);
        button_checks[i] = check;
    }
    return group;
}

QWidget* TasInputWidget::CreateStickGroup(const QString& title, StickControls& controls,
                                          std::optional<Override::Stick> Override::State::*member) {
    auto* group = new QGroupBox(title);
    auto* layout = new QVBoxLayout(group);

    controls.pad = new TasInputPad(TasInputPad::Shape::Circle, -StickRange, StickRange, -StickRange,
                                   StickRange, true, group);
    layout->addWidget(controls.pad, 0, Qt::AlignHCenter);

    auto* fields = new QHBoxLayout();
    const auto make_spin = [&](const QString& label) {
        fields->addWidget(new QLabel(label, group));
        auto* spin = new QSpinBox(group);
        spin->setRange(-StickRange, StickRange);
        fields->addWidget(spin, 1);
        return spin;
    };
    controls.x = make_spin(QStringLiteral("X"));
    controls.y = make_spin(QStringLiteral("Y"));
    auto* clear = new QPushButton(tr("Clear"), group);
    fields->addWidget(clear);
    layout->addLayout(fields);

    auto* max_output_row = new QHBoxLayout();
    max_output_row->addWidget(new QLabel(tr("Max Output"), group));
    controls.max_output = new QSpinBox(group);
    controls.max_output->setRange(0, 100);
    controls.max_output->setSuffix(QStringLiteral("%"));
    controls.max_output->setValue(system.InputOverride().GetMaxOutput(controls.id));
    controls.max_output->setToolTip(
        tr("Limits the stick to a smaller circle, for both this window and the controller"));
    max_output_row->addWidget(controls.max_output);
    max_output_row->addStretch();
    layout->addLayout(max_output_row);

    const auto set = [this, member, &controls](int x, int y) {
        overrides.*member = ClampStick(controls, x, y);
        Apply();
    };
    connect(controls.max_output, &QSpinBox::valueChanged, this,
            [this, member, &controls](int value) {
                system.InputOverride().SetMaxOutput(controls.id, value);
                controls.pad->SetLimit(value / 100.0);
                // Keep a position set in the window inside the new limit
                if (overrides.*member) {
                    overrides.*member =
                        ClampStick(controls, (overrides.*member)->x, (overrides.*member)->y);
                }
                Apply();
            });
    connect(controls.pad, &TasInputPad::PointSet, this, set);
    const auto clear_stick = [this, member] {
        (overrides.*member).reset();
        Apply();
    };
    connect(controls.pad, &TasInputPad::Cleared, this, clear_stick);
    connect(clear, &QPushButton::clicked, this, clear_stick);
    // Editing one field locks the stick at the values shown in both fields
    const auto on_spin = [this, set, &controls](int) {
        if (!updating) {
            set(controls.x->value(), controls.y->value());
        }
    };
    connect(controls.x, &QSpinBox::valueChanged, this, on_spin);
    connect(controls.y, &QSpinBox::valueChanged, this, on_spin);
    return group;
}

TasInputWidget::Override::Stick TasInputWidget::ClampStick(const StickControls& controls, int x,
                                                           int y) const {
    const double max_length = StickRange * controls.max_output->value() / 100.0;
    const double length = std::sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y);
    if (length > max_length) {
        // Scale towards the center and truncate, so the result stays inside the circle
        x = static_cast<int>(x * max_length / length);
        y = static_cast<int>(y * max_length / length);
    }
    return Override::Stick{static_cast<s16>(x), static_cast<s16>(y)};
}

QWidget* TasInputWidget::CreateTouchGroup() {
    auto* group = new QGroupBox(tr("Touch Screen"));
    auto* layout = new QHBoxLayout(group);

    touch_pad = new TasInputPad(TasInputPad::Shape::Rectangle, 0, TouchWidth - 1, 0,
                                TouchHeight - 1, false, group);
    layout->addWidget(touch_pad);

    auto* fields = new QGridLayout();
    touch_check = new QCheckBox(tr("Touching"), group);
    fields->addWidget(touch_check, 0, 0, 1, 2);
    fields->addWidget(new QLabel(QStringLiteral("X"), group), 1, 0);
    touch_x = new QSpinBox(group);
    touch_x->setRange(0, TouchWidth - 1);
    fields->addWidget(touch_x, 1, 1);
    fields->addWidget(new QLabel(QStringLiteral("Y"), group), 2, 0);
    touch_y = new QSpinBox(group);
    touch_y->setRange(0, TouchHeight - 1);
    fields->addWidget(touch_y, 2, 1);
    auto* clear = new QPushButton(tr("Clear"), group);
    fields->addWidget(clear, 3, 0, 1, 2);
    fields->setRowStretch(4, 1);
    layout->addLayout(fields);
    layout->addStretch();

    const auto set = [this](int x, int y) {
        overrides.touch = Override::Touch{static_cast<u16>(x), static_cast<u16>(y)};
        Apply();
    };
    const auto clear_touch = [this] {
        overrides.touch.reset();
        Apply();
    };
    connect(touch_pad, &TasInputPad::PointSet, this, set);
    connect(touch_pad, &TasInputPad::Cleared, this, clear_touch);
    connect(clear, &QPushButton::clicked, this, clear_touch);
    connect(touch_check, &QCheckBox::clicked, this, [this, set, clear_touch](bool checked) {
        checked ? set(touch_x->value(), touch_y->value()) : clear_touch();
    });
    const auto on_spin = [this, set](int) {
        if (!updating) {
            set(touch_x->value(), touch_y->value());
        }
    };
    connect(touch_x, &QSpinBox::valueChanged, this, on_spin);
    connect(touch_y, &QSpinBox::valueChanged, this, on_spin);
    return group;
}

QWidget* TasInputWidget::CreateMotionGroup(
    const QString& title, std::array<AxisControls, 3>& controls,
    std::array<std::optional<s16>, 3> Override::State::*member, int range) {
    auto* group = new QGroupBox(title);
    auto* grid = new QGridLayout(group);
    constexpr std::array<const char*, 3> axis_names{"X", "Y", "Z"};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        grid->addWidget(new QLabel(QString::fromLatin1(axis_names[axis]), group),
                        static_cast<int>(axis), 0);

        auto& control = controls[axis];
        control.slider = new QSlider(Qt::Horizontal, group);
        control.slider->setRange(-range, range);
        control.slider->setMinimumWidth(100);
        grid->addWidget(control.slider, static_cast<int>(axis), 1);

        control.spin = new QSpinBox(group);
        control.spin->setRange(-range, range);
        grid->addWidget(control.spin, static_cast<int>(axis), 2);

        const auto set = [this, member, axis](int value) {
            if (!updating) {
                (overrides.*member)[axis] = static_cast<s16>(value);
                Apply();
            }
        };
        connect(control.slider, &QSlider::valueChanged, this, set);
        connect(control.spin, &QSpinBox::valueChanged, this, set);
    }
    auto* clear = new QPushButton(tr("Clear"), group);
    connect(clear, &QPushButton::clicked, this, [this, member] {
        (overrides.*member).fill(std::nullopt);
        Apply();
    });
    grid->addWidget(clear, 3, 0, 1, 3);
    return group;
}

void TasInputWidget::ClearAll() {
    overrides = {};
    Apply();
}

void TasInputWidget::Apply() {
    system.InputOverride().SetState(overrides);
    Refresh();
}

void TasInputWidget::OnEmulationStarting(EmuThread*) {
    emulation_running = true;
    ClearAll();
}

void TasInputWidget::OnEmulationStopping() {
    emulation_running = false;
    ClearAll();
}

void TasInputWidget::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    UpdateTimerState();
}

void TasInputWidget::hideEvent(QHideEvent* event) {
    QDockWidget::hideEvent(event);
    UpdateTimerState();
}

void TasInputWidget::UpdateTimerState() {
    // Live input is shown even while no application is running
    if (isVisible()) {
        refresh_timer.start();
        Refresh();
    } else {
        refresh_timer.stop();
    }
}

void TasInputWidget::UpdateLiveDevices() {
    const auto& profile = Settings::values.current_input_profile;
    std::vector<std::string> params;
    for (const int button : ProfileButtons) {
        params.push_back(profile.buttons[button]);
    }
    params.push_back(profile.analogs[Settings::NativeAnalog::CirclePad]);
    params.push_back(profile.analogs[Settings::NativeAnalog::CStick]);
    params.push_back(profile.touch_device);
    params.push_back(profile.use_touchpad ? profile.controller_touch_device : std::string{});
    if (params == live_params) {
        return;
    }

    // The input profile changed (or this is the first update), recreate the devices
    live_params = std::move(params);
    for (std::size_t i = 0; i < ProfileButtons.size(); ++i) {
        live.buttons[i] = Input::CreateDevice<Input::ButtonDevice>(live_params[i]);
    }
    live.circle_pad = Input::CreateDevice<Input::AnalogDevice>(
        profile.analogs[Settings::NativeAnalog::CirclePad]);
    live.c_stick =
        Input::CreateDevice<Input::AnalogDevice>(profile.analogs[Settings::NativeAnalog::CStick]);
    live.touch = Input::CreateDevice<Input::TouchDevice>(profile.touch_device);
    live.controller_touch.reset();
    if (profile.use_touchpad && !profile.controller_touch_device.empty()) {
        live.controller_touch =
            Input::CreateDevice<Input::TouchDevice>(profile.controller_touch_device);
    }
}

void TasInputWidget::Refresh() {
    UpdateLiveDevices();
    updating = true;

    for (u32 i = 0; i < Override::NumButtons; ++i) {
        const bool set = Override::IsPressed(overrides, static_cast<Override::Button>(i));
        const bool pressed = set || (live.buttons[i] && live.buttons[i]->GetStatus());
        if (button_checks[i]->isChecked() != pressed) {
            button_checks[i]->setChecked(pressed);
        }
        SetOverriddenStyle(button_checks[i], set);
    }

    const auto refresh_stick = [&](StickControls& controls,
                                   const std::optional<Override::Stick>& set,
                                   const std::unique_ptr<Input::AnalogDevice>& device, int scale) {
        int x = 0, y = 0;
        if (set) {
            x = set->x;
            y = set->y;
        } else if (device) {
            // Show the controller position as the game gets it, limited by the max output
            const auto [fx, fy] = device->GetStatus();
            const float limit = system.InputOverride().GetStickScale(controls.id);
            x = static_cast<int>(std::lround(fx * limit * scale));
            y = static_cast<int>(std::lround(fy * limit * scale));
        }
        controls.pad->SetPoint(std::make_pair(x, y), set.has_value());
        SetSpinValue(controls.x, x);
        SetSpinValue(controls.y, y);
        SetOverriddenStyle(controls.x, set.has_value());
        SetOverriddenStyle(controls.y, set.has_value());
    };
    refresh_stick(circle_pad_controls, overrides.circle_pad, live.circle_pad, CirclePadMax);
    refresh_stick(c_stick_controls, overrides.c_stick, live.c_stick, CStickMax);

    std::optional<std::pair<int, int>> touch_point;
    if (overrides.touch) {
        touch_point = std::make_pair(overrides.touch->x, overrides.touch->y);
    } else {
        for (const auto* device : {live.touch.get(), live.controller_touch.get()}) {
            if (!device) {
                continue;
            }
            const auto [fx, fy, pressed] = device->GetStatus();
            if (pressed) {
                touch_point = std::make_pair(static_cast<int>(fx * TouchWidth),
                                             static_cast<int>(fy * TouchHeight));
                break;
            }
        }
    }
    touch_pad->SetPoint(touch_point, overrides.touch.has_value());
    if (touch_check->isChecked() != touch_point.has_value()) {
        touch_check->setChecked(touch_point.has_value());
    }
    if (touch_point) {
        SetSpinValue(touch_x, touch_point->first);
        SetSpinValue(touch_y, touch_point->second);
    }
    for (QWidget* widget : std::initializer_list<QWidget*>{touch_check, touch_x, touch_y}) {
        SetOverriddenStyle(widget, overrides.touch.has_value());
    }

    // Motion is shown as last read by the emulated system, so it only updates while running
    auto& input_override = system.InputOverride();
    const auto refresh_motion = [&](std::array<AxisControls, 3>& controls,
                                    const std::array<std::optional<s16>, 3>& set,
                                    const std::array<s16, 3>& live_values) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const int value = set[axis].value_or(emulation_running ? live_values[axis] : 0);
            if (!controls[axis].slider->isSliderDown() && controls[axis].slider->value() != value) {
                controls[axis].slider->setValue(value);
            }
            SetSpinValue(controls[axis].spin, value);
            SetOverriddenStyle(controls[axis].spin, set[axis].has_value());
        }
    };
    refresh_motion(accel_controls, overrides.accel, input_override.GetLiveAccel());
    refresh_motion(gyro_controls, overrides.gyro, input_override.GetLiveGyro());

    updating = false;
}
