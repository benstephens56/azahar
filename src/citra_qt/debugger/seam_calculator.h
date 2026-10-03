// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <optional>
#include <vector>
#include <QDockWidget>
#include <QTimer>
#include "citra_qt/debugger/seam_math.h"
#include "common/common_types.h"

class EmuThread;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace Core {
class System;
}

/**
 * Helps getting onto and climbing "invisible seams" in Ocarina of Time 3D: with the scene's
 * collision and the game's GlobalContext (for Link's actor and the camera), shows live where Link
 * is relative to a seam, where and in which direction to walk to get onto it, and the directions
 * (and circle pad positions) that climb it.
 */
class SeamCalculatorWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit SeamCalculatorWidget(Core::System& system, QWidget* parent = nullptr);
    ~SeamCalculatorWidget() override;

public slots:
    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct LinkState {
        float x, y, z;
        float prev_x, prev_y, prev_z; ///< Position before his last frame's movement
        float floor_height;
        u16 bg_check_flags;
        u16 yaw;                       ///< Direction Link moves in (world.rot.y)
        float speed;                   ///< speedXZ
        std::optional<u16> camera_yaw; ///< Input yaw of the active camera, if known
        /// Index of the scene collision triangle Link's floor check found, if known
        std::optional<int> floor_poly;
    };

    QWidget* CreateSetupGroup();
    QWidget* CreateMountGroup();
    QWidget* CreateClimbGroup();

    std::optional<VAddr> ContextAddress() const;
    std::optional<VAddr> ActorAddress() const;
    std::optional<u32> ReadU32(VAddr address) const;
    std::optional<u16> ReadU16(VAddr address) const;
    std::optional<float> ReadFloat(VAddr address) const;
    std::optional<LinkState> ReadLink() const;
    /// Finds the address of the GlobalContext by scanning memory (see the .cpp)
    void FindGlobalContext();

    void LoadCollision(const QString& path);
    void FindSeams();
    void SelectTriangle(const SeamMath::Triangle& triangle);
    /// Reads the triangle from the fields, returns false if they are invalid
    bool ParseTriangleFields();

    void Update();
    void UpdateMount(const SeamMath::Triangle& tri, const LinkState& link);
    void UpdateClimb(const SeamMath::Triangle& tri, const LinkState& link);
    void ClearLive();
    /// Shows which triangle Link stands on and how he actually moved last frame
    void UpdateCheck(const LinkState& link);
    /// Circle pad position (in TAS Input units) that makes Link go along `yaw`, if the camera is
    /// known. Returns {x, y, resulting yaw}.
    std::optional<std::array<int, 3>> StickFor(u16 yaw, const LinkState& link) const;
    /// Most the seam may rise per unit walked at the climbing speed, for Link to stay on it
    double ClimbMaxRise() const;
    void MoveLinkToTarget();
    void UpdateTimerState();
    void SaveSettings() const;

    Core::System& system;

    std::vector<SeamMath::Triangle> collision;
    std::optional<SeamMath::Triangle> triangle;
    std::optional<std::array<float, 2>> target;

    // Setup
    QLineEdit* context_edit;
    QLabel* file_label;
    QSpinBox* radius_spin;
    QTableWidget* seam_table;
    QWidget* details_widget;
    QLineEdit* address_edit;
    std::array<QLineEdit*, 3> vertex_edits{};
    QLineEdit* normal_edit;
    QLineEdit* dist_edit;
    QLabel* triangle_label;
    QLabel* link_label;

    // Getting onto the seam
    QLabel* mount_status_label;
    QLabel* target_label;
    QLabel* walk_label;
    QPushButton* move_button;

    // Climbing
    QLabel* next_frame_label;
    QLabel* check_label;
    QPushButton* use_floor_button;
    QDoubleSpinBox* climb_speed_spin;
    /// Distance Link moves per frame per unit of speed, as last measured
    double step_ratio = 1.0;
    std::array<QLabel*, 2> way_labels{};
    QSpinBox* stick_magnitude_spin;

    QString collision_path;
    QTimer update_timer;
    bool emulation_running = false;
};
