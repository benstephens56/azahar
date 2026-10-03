// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <chrono>
#include <cstring>
#include <optional>
#include <type_traits>
#include "common/common_types.h"
#include "core/memory_editor.h"

/// Addresses and offsets of Ocarina of Time 3D structures, shared by the OoT3D debugger tools
namespace OoT3D {

// Offsets in the Actor struct (see include/z3D/z3Dactor.h of the OoT3D practice menu)
constexpr VAddr OffsetActorType = 0x002;
constexpr VAddr OffsetWorldPos = 0x028;
constexpr VAddr OffsetWorldRotY = 0x036; ///< Direction the actor moves in
constexpr VAddr OffsetVelocity = 0x060;  ///< Vec3f
constexpr VAddr OffsetSpeedXZ = 0x06C;
constexpr VAddr OffsetFloorHeight = 0x084;
constexpr VAddr OffsetBgCheckFlags = 0x090;
constexpr VAddr OffsetShapeRotY = 0x0BE; ///< Direction the actor faces
constexpr VAddr OffsetPrevPos = 0x108;
constexpr u8 ActorTypePlayer = 2;

// Offsets in the Player struct (the oot3d decomp, as used by the 3D model viewer's clipfinder)
constexpr VAddr OffsetPlayerXZSpeed = 0x221C; ///< Player.xzSpeed ("linear velocity")
constexpr VAddr OffsetPlayerYaw = 0x2220;     ///< Player.yaw (the decomp's unk_2220)

// Offsets in the GlobalContext and Camera structs (see include/z3D/z3D.h of the practice menu)
constexpr VAddr OffsetGameplayFrames = 0x0F8; ///< GameState.frames, one per game frame
constexpr VAddr OffsetMainCamera = 0x364;
constexpr VAddr OffsetCameraGlobalContext = 0x0D4;
constexpr VAddr OffsetCameraPtrs = 0xA54;
constexpr VAddr OffsetActiveCamera = 0xA64;
constexpr VAddr OffsetCameraInputYaw = 0x17E; // inputDir.y
/// actorCtx (0x208C) .actorList (0x0C) [ACTORTYPE_PLAYER] (2 * 8) .first (4)
constexpr VAddr OffsetPlayerActor = 0x208C + 0x0C + 2 * 8 + 4;

constexpr u16 BgCheckGround = 0x0001;

/// Reads a value of the running application without side effects (see MemoryEditor::Peek)
template <typename T>
std::optional<T> Read(const Core::MemoryEditor& memory, VAddr address) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto bytes = memory.Peek(address, sizeof(T));
    if (!bytes) {
        return std::nullopt;
    }
    T value;
    std::memcpy(&value, bytes->data(), sizeof(value));
    return value;
}

/// Link's actor, if the GlobalContext at `context` has one
inline std::optional<VAddr> PlayerActor(const Core::MemoryEditor& memory, VAddr context) {
    const auto player = Read<u32>(memory, context + OffsetPlayerActor);
    if (!player || *player == 0) {
        return std::nullopt;
    }
    const auto type = Read<u8>(memory, *player + OffsetActorType);
    if (!type || *type != ActorTypePlayer) {
        return std::nullopt;
    }
    return *player;
}

enum class FindResult { Found, NoMemory, NotFound };

/**
 * Finds the address of the GlobalContext by scanning memory. The main camera keeps a pointer to
 * the GlobalContext it is part of, so the GlobalContext is at the value of the one word that points
 * to 0x364 + 0xD4 bytes before the word itself. Only works while in game (not on the title screen
 * or file select), where the context holds Link's actor.
 */
inline FindResult FindGlobalContext(Core::MemoryEditor& memory, VAddr& out) {
    const auto snapshot = memory.SnapshotWritableMemory(std::chrono::milliseconds{2000});
    if (!snapshot) {
        return FindResult::NoMemory;
    }
    constexpr VAddr Offset = OffsetMainCamera + OffsetCameraGlobalContext;
    for (const auto& region : *snapshot) {
        for (std::size_t i = 0; i + 4 <= region.data.size(); i += 4) {
            u32 value;
            std::memcpy(&value, region.data.data() + i, sizeof(value));
            if (value == 0 || region.base + i != value + Offset) {
                continue;
            }
            if (PlayerActor(memory, value)) {
                out = value;
                return FindResult::Found;
            }
        }
    }
    return FindResult::NotFound;
}

} // namespace OoT3D
