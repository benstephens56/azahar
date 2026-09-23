// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include "common/common_types.h"

namespace Core {

/**
 * Inputs set from a frontend input window (e.g. for TAS), which take priority over the inputs of
 * the real input devices. The HID/IR services apply them right after reading the input devices
 * and before the movie recorder sees the input, so they are recorded like any other input.
 *
 * Buttons set here are pressed in addition to the real buttons. Sticks, touch and motion axes set
 * here replace the real values. Anything not set passes the real input through, and while nothing
 * is set the input is not modified at all.
 */
class InputOverride {
public:
    enum Button : u32 {
        A,
        B,
        X,
        Y,
        Up,
        Down,
        Left,
        Right,
        L,
        R,
        Start,
        Select,
        ZL,
        ZR,
        NumButtons,
    };

    struct Stick {
        s16 x;
        s16 y;
    };

    struct Touch {
        u16 x;
        u16 y;
    };

    struct State {
        /// Bit mask of pressed buttons (1 << Button)
        u32 buttons = 0;
        /// Circle pad in HID units (-0x9C to 0x9C)
        std::optional<Stick> circle_pad;
        /// C-stick in IR:RST units (-0x9C to 0x9C)
        std::optional<Stick> c_stick;
        /// Touch position in bottom screen pixels. Set means the screen is touched.
        std::optional<Touch> touch;
        /// Accelerometer and gyroscope axes in HID units, each can be set separately
        std::array<std::optional<s16>, 3> accel;
        std::array<std::optional<s16>, 3> gyro;

        bool IsEmpty() const {
            return buttons == 0 && !circle_pad && !c_stick && !touch &&
                   std::none_of(accel.begin(), accel.end(),
                                [](auto& v) { return v.has_value(); }) &&
                   std::none_of(gyro.begin(), gyro.end(), [](auto& v) { return v.has_value(); });
        }
    };

    void SetState(const State& new_state) {
        std::scoped_lock lock{mutex};
        state = new_state;
        active = !state.IsEmpty();
    }

    State GetState() const {
        std::scoped_lock lock{mutex};
        return state;
    }

    void Clear() {
        SetState({});
    }

    /// Returns the overrides if any are set. Cheap to call when none are set.
    std::optional<State> GetActive() const {
        if (!active.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }
        return GetState();
    }

    static constexpr bool IsPressed(const State& state, Button button) {
        return (state.buttons >> button) & 1;
    }

    /// Motion values last read by the HID service, used by the frontend to display the motion of
    /// the real input devices.
    void SetLiveAccel(s16 x, s16 y, s16 z) {
        live_accel[0] = x;
        live_accel[1] = y;
        live_accel[2] = z;
    }
    void SetLiveGyro(s16 x, s16 y, s16 z) {
        live_gyro[0] = x;
        live_gyro[1] = y;
        live_gyro[2] = z;
    }
    std::array<s16, 3> GetLiveAccel() const {
        return {live_accel[0], live_accel[1], live_accel[2]};
    }
    std::array<s16, 3> GetLiveGyro() const {
        return {live_gyro[0], live_gyro[1], live_gyro[2]};
    }

private:
    mutable std::mutex mutex;
    State state;
    std::atomic<bool> active{false};

    std::array<std::atomic<s16>, 3> live_accel{};
    std::array<std::atomic<s16>, 3> live_gyro{};
};

} // namespace Core
