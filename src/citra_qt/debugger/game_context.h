// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <QString>
#include "citra_qt/debugger/seam_math.h"
#include "common/common_types.h"

namespace Core {
class System;
}

namespace GameContext {

/// Where things are in a game's memory
struct GameLayout {
    // In the Actor struct
    VAddr world_pos;
    VAddr prev_pos;
    VAddr world_rot_y;
    VAddr speed_xz;
    VAddr floor_poly;
    VAddr floor_height;
    VAddr bg_check_flags;
    // In the GlobalContext and Camera structs
    VAddr main_camera;
    VAddr camera_global_context; ///< The camera's pointer to its GlobalContext
    VAddr camera_ptrs;
    VAddr active_camera;
    VAddr camera_input_yaw; ///< inputDir.y
    VAddr collision_header; ///< colCtx.colHeader
    VAddr header_num_polygons;
    VAddr header_poly_list;
    VAddr player_actor; ///< actorCtx.actorList[ACTORTYPE_PLAYER].first
    /// Byte of the game state that is 0 once it ends (its memory may stay as it was after that)
    VAddr state_running;
};

const GameLayout& LayoutOf(SeamMath::Game game);

/**
 * Keeps track of the game's GlobalContext, which holds Link's actor and the scene's state.
 *
 * The game makes a new GlobalContext on scene changes, and it isn't always at the same address
 * (in MM3D, e.g. after the Song of Soaring), so the one in use is checked when asked for, and
 * searched for again when it no longer holds Link.
 *
 * One instance is shared by the debugging tools (see Get).
 */
class Tracker {
public:
    static Tracker& Get(Core::System& system);

    explicit Tracker(Core::System& system);

    SeamMath::Game GetGame() const {
        return game;
    }
    void SetGame(SeamMath::Game new_game);

    /// The GlobalContext last set or found, without checking it
    std::optional<VAddr> Current() const {
        return context;
    }
    /// Sets the GlobalContext (e.g. entered by hand)
    void SetContext(std::optional<VAddr> address);

    /**
     * The GlobalContext, if it still holds Link. If it doesn't anymore, searches memory for the
     * game's new one (at most every few seconds, as searching takes a moment).
     */
    std::optional<VAddr> Context();
    /// Link's actor, from the GlobalContext
    std::optional<VAddr> PlayerActor();

    /// Searches memory for the GlobalContext of either game now, and uses it if found
    std::optional<VAddr> Find();

    /// Whether the GlobalContext (or game) changed since `generation` (see Generation)
    u64 Generation() const {
        return generation;
    }

private:
    bool IsValid(VAddr address, SeamMath::Game candidate) const;
    std::optional<u32> ReadU32(VAddr address) const;

    Core::System& system;
    SeamMath::Game game = SeamMath::Game::OoT3D;
    std::optional<VAddr> context;
    std::chrono::steady_clock::time_point last_search{};
    u64 generation = 0;
};

/**
 * Evaluates an address written as an expression: hexadecimal numbers (with or without 0x),
 * "Link" (Link's actor) and "GlobalContext", added or subtracted, with [...] reading the 32-bit
 * pointer at the address inside. E.g. "Link+24" or "[GlobalContext+AB4]+10".
 * Returns nullopt if it's invalid or can't be evaluated now.
 */
std::optional<VAddr> EvaluateAddress(
    const QString& text, const std::function<std::optional<VAddr>(const QString&)>& name,
    const std::function<std::optional<u32>(VAddr)>& read_u32);

/// Whether the text is a valid address expression (see EvaluateAddress)
bool IsValidAddress(const QString& text);

/// Evaluates an address expression (see EvaluateAddress) in the running game
std::optional<VAddr> ResolveAddress(Core::System& system, const QString& text);

/// Whether the address expression uses Link or the GlobalContext (so it can move)
bool IsRelativeAddress(const QString& text);

} // namespace GameContext
