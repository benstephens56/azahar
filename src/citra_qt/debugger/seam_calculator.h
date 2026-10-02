// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

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
 * Helps getting onto "invisible seams" in Ocarina of Time 3D: given the player actor's address and
 * a seam triangle (picked from the scene's collision file or entered by hand), shows live where
 * Link is relative to the seam, the seam's height under him and where to move to be at a given
 * height on it.
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
        float prev_y;
        float floor_height;
        u16 bg_check_flags;
        u16 yaw;                       ///< Direction Link moves in (world.rot.y)
        float speed;                   ///< speedXZ
        std::optional<u16> camera_yaw; ///< Input yaw of the active camera, if known
    };

    QWidget* CreateLinkGroup();
    QWidget* CreateTriangleGroup();
    QWidget* CreateLiveGroup();
    QWidget* CreateClimbGroup();

    std::optional<VAddr> ContextAddress() const;
    std::optional<VAddr> ActorAddress() const;
    std::optional<u32> ReadU32(VAddr address) const;
    std::optional<u16> ReadU16(VAddr address) const;
    /// Finds the address of the GlobalContext by scanning memory (see the .cpp)
    void FindGlobalContext();
    std::optional<LinkState> ReadLink() const;
    std::optional<float> ReadFloat(VAddr address) const;

    void LoadCollision(const QString& path);
    void FindSeams();
    void SelectTriangle(const SeamMath::Triangle& triangle);
    /// Reads the triangle from the fields, returns false if they are invalid
    bool ParseTriangleFields();
    void Update();
    void UpdateClimb(const SeamMath::Triangle& tri, const LinkState& link);
    void ClearClimb();
    void MoveLinkToTarget();
    void UpdateTimerState();
    void SaveSettings() const;

    Core::System& system;

    std::vector<SeamMath::Triangle> collision;
    std::optional<SeamMath::Triangle> triangle;
    std::optional<std::array<float, 2>> target;

    QLineEdit* context_edit;
    QLineEdit* address_edit;
    QLabel* motion_label;
    QLabel* position_label;
    QLabel* floor_label;

    QLabel* file_label;
    QSpinBox* radius_spin;
    QTableWidget* seam_table;
    std::array<QLineEdit*, 3> vertex_edits{};
    QLineEdit* normal_edit;
    QLineEdit* dist_edit;
    QLabel* triangle_label;

    QLabel* vertices_label;
    QLabel* height_label;
    QLabel* game_floor_label;
    QLabel* band_label;
    QLabel* target_label;
    QPushButton* move_button;

    QLabel* next_frame_label;
    QDoubleSpinBox* plan_speed_spin;
    QDoubleSpinBox* aim_spin;
    QSpinBox* stick_magnitude_spin;
    std::array<QLabel*, 2> direction_labels{};

    QString collision_path;
    QTimer update_timer;
    bool emulation_running = false;
};
