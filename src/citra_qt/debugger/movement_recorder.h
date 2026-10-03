// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>
#include <QDockWidget>
#include <QTimer>
#include "common/common_types.h"

class EmuThread;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace Core {
class System;
}

/**
 * Records Link's movement in Ocarina of Time 3D once per game frame: his position, the directions
 * he faces and moves in, his speeds and whether he is on the ground. Meant for measuring movement
 * techniques (rolls, sidehops, backflips, jumpslashes, ...) frame by frame, to be saved as CSV or
 * JSON for the 3D model viewer's clipfinder and setup tools.
 *
 * Link is read at the end of every emulated frame (vblank). The game runs at 30 fps, i.e. one game
 * frame every two vblanks, so a sample is only kept when the game's frame counter
 * (GameState.frames) changed. Loading a savestate that goes back in time replaces the samples from
 * that game frame on, so an attempt can simply be redone.
 */
class MovementRecorderWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit MovementRecorderWidget(Core::System& system, QWidget* parent = nullptr);
    ~MovementRecorderWidget() override;

public slots:
    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Sample {
        u32 game_frame;     ///< GameState.frames
        float x, y, z;      ///< world.pos
        float velocity_y;   ///< velocity.y
        float speed_xz;     ///< Actor.speedXZ
        float xz_speed;     ///< Player.xzSpeed
        u16 move_yaw;       ///< world.rot.y, the direction Link moves in
        u16 facing;         ///< shape.rot.y, the direction Link faces
        u16 player_yaw;     ///< Player.yaw
        u16 bg_check_flags; ///< Actor.bgCheckFlags
        float floor_height; ///< Actor.floorHeight
    };

    /// Displacements of a sample in Link's frame of reference at the first sample
    struct Local {
        double forward, left, up;
    };

    QWidget* CreateLinkGroup();
    QWidget* CreateRecordGroup();

    std::optional<VAddr> ContextAddress() const;
    void FindGlobalContext();
    std::optional<Sample> ReadSample(VAddr context) const;

    void SetRecording(bool enable);
    void UpdateFrameCallback();
    /// Called on the emulator thread at the end of every emulated frame
    void OnFrameEnd();
    /// Moves the samples taken on the emulator thread to the table
    void DrainSamples();
    void UpdateLive();
    void UpdateTimerState();

    void AppendRow(std::size_t index);
    void RebuildTable();
    void Clear();
    Local LocalDelta(std::size_t index) const;
    Local LocalTotal(std::size_t index) const;
    void SaveCsv();
    void SaveJson();
    void SaveSettings() const;

    Core::System& system;

    QLineEdit* context_edit;
    QLabel* live_label;
    QLineEdit* name_edit;
    QPushButton* record_button;
    QPushButton* clear_button;
    QPushButton* save_csv_button;
    QPushButton* save_json_button;
    QCheckBox* follow_check;
    QLabel* status_label;
    QTableWidget* table;

    QTimer update_timer;
    bool emulation_running = false;

    /// Recorded samples, in game frame order (GUI thread)
    std::vector<Sample> samples;

    // Shared with the emulator thread
    std::atomic<bool> recording{false};
    std::atomic<VAddr> context_address{0};
    /// Game frame of the last sample taken on the emulator thread, -1 for none
    std::atomic<s64> last_game_frame{-1};
    std::mutex pending_mutex;
    std::vector<Sample> pending;
};
