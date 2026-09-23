// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <string>
#include <QDockWidget>
#include <QTimer>
#include <QWidget>
#include "core/frontend/input.h"
#include "core/input_override.h"

class EmuThread;
class QCheckBox;
class QSlider;
class QSpinBox;

namespace Core {
class System;
}

/**
 * A 2D input area: a circle (for sticks) or a rectangle (for the touch screen) in which a point can
 * be placed with the mouse. Left click/drag sets the point, right click clears it.
 */
class TasInputPad : public QWidget {
    Q_OBJECT

public:
    enum class Shape { Circle, Rectangle };

    /// `invert_y` makes larger y values go up (sticks) instead of down (touch screen)
    TasInputPad(Shape shape, int min_x, int max_x, int min_y, int max_y, bool invert_y,
                QWidget* parent = nullptr);

    /// Sets the displayed point. `overridden` draws it as set by the user rather than live input.
    void SetPoint(std::optional<std::pair<int, int>> point, bool overridden);

    /// Limits a circle pad to a smaller circle, as a fraction (0 to 1) of the full radius
    void SetLimit(double fraction);

    QSize sizeHint() const override;

signals:
    void PointSet(int x, int y);
    void Cleared();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QRectF PadRect() const;
    void SetFromMouse(const QPointF& pos);

    Shape shape;
    int min_x, max_x, min_y, max_y;
    bool invert_y;
    std::optional<std::pair<int, int>> point;
    bool overridden = false;
    double limit = 1.0;
};

/**
 * TAS input window (similar to BizHawk's virtual pads): lets inputs be set with checkboxes, stick
 * and touch pads, sliders and numeric fields. Inputs set here take priority over the real input
 * devices (see Core::InputOverride) and stay set until cleared. Inputs that are not set show the
 * live state of the real input devices.
 */
class TasInputWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit TasInputWidget(Core::System& system, QWidget* parent = nullptr);
    ~TasInputWidget() override;

public slots:
    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();
    /// Clears every input set in this window
    void ClearAll();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    using Override = Core::InputOverride;

    struct StickControls {
        Override::StickId id;
        TasInputPad* pad;
        QSpinBox* x;
        QSpinBox* y;
        QSpinBox* max_output;
    };

    struct AxisControls {
        QSlider* slider;
        QSpinBox* spin;
    };

    /// Input devices used to show the live state of the real inputs
    struct LiveDevices {
        std::array<std::unique_ptr<Input::ButtonDevice>, Override::NumButtons> buttons;
        std::unique_ptr<Input::AnalogDevice> circle_pad;
        std::unique_ptr<Input::AnalogDevice> c_stick;
        std::unique_ptr<Input::TouchDevice> touch;
        std::unique_ptr<Input::TouchDevice> controller_touch;
    };

    QWidget* CreateButtonsGroup();
    /// Clamps a stick position to the circle allowed by the stick's maximum output
    Override::Stick ClampStick(const StickControls& controls, int x, int y) const;

    QWidget* CreateStickGroup(const QString& title, StickControls& controls,
                              std::optional<Override::Stick> Override::State::*member);
    QWidget* CreateTouchGroup();
    QWidget* CreateMotionGroup(const QString& title, std::array<AxisControls, 3>& controls,
                               std::array<std::optional<s16>, 3> Override::State::*member,
                               int range);

    void Apply();
    void Refresh();
    void UpdateLiveDevices();
    void UpdateTimerState();

    Core::System& system;
    Override::State overrides;

    std::array<QCheckBox*, Override::NumButtons> button_checks{};
    StickControls circle_pad_controls{};
    StickControls c_stick_controls{};
    TasInputPad* touch_pad{};
    QSpinBox* touch_x{};
    QSpinBox* touch_y{};
    QCheckBox* touch_check{};
    std::array<AxisControls, 3> accel_controls{};
    std::array<AxisControls, 3> gyro_controls{};

    LiveDevices live;
    /// Input profile parameters the live devices were created from
    std::vector<std::string> live_params;

    QTimer refresh_timer;
    bool emulation_running = false;
    /// Guards against programmatic widget updates being treated as user input
    bool updating = false;
};
