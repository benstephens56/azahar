// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include "common/common_types.h"

namespace Core {

class System;

/**
 * Lets frontend tools (memory watch / memory search) read, edit and freeze memory of the running
 * application without affecting emulation.
 *
 * - Reads only go through the raw page table pointers of the application process. They never
 *   touch the rasterizer cache or any other emulated state, so they can be done from any thread
 *   at any time. Pages that are not plain memory (e.g. pages cached by the GPU) cannot be read.
 * - Writes and freezes are only queued by the frontend and applied on the emulator thread, at the
 *   start of a run loop iteration or while paused by frame advancing. When nothing is queued or
 *   frozen, the emulator thread does nothing, so emulation stays deterministic.
 */
class MemoryEditor {
public:
    /// A contiguous copy of readable memory, starting at `base`.
    struct MemoryRegion {
        VAddr base;
        std::vector<u8> data;
    };

    explicit MemoryEditor(System& system);
    ~MemoryEditor();

    /// Reads `size` bytes of the application process memory without side effects. Returns
    /// std::nullopt if any byte is not readable as plain memory.
    std::optional<std::vector<u8>> Peek(VAddr address, std::size_t size) const;

    /// Returns whether the page containing `address` can be read with Peek.
    bool IsPeekable(VAddr address) const;

    /// Queues a one-time write, applied on the emulator thread.
    void QueueWrite(VAddr address, std::vector<u8> bytes);

    /// Freezes `address` to `bytes`, rewriting them on every run loop iteration.
    void Freeze(VAddr address, std::vector<u8> bytes);
    void Unfreeze(VAddr address);
    bool IsFrozen(VAddr address) const;
    std::optional<std::vector<u8>> GetFrozenBytes(VAddr address) const;
    void ClearAll();

    /**
     * Copies all writable plain memory of the application process (excluding VRAM and pages
     * cached by the GPU) on the emulator thread, so that the copy is consistent. Blocks for at
     * most `timeout`, returning std::nullopt if the emulator thread did not get to it in time
     * (e.g. while paused by the debugger) or no application is running.
     */
    std::optional<std::vector<MemoryRegion>> SnapshotWritableMemory(
        std::chrono::milliseconds timeout);

    /// Called from the emulator thread. Applies queued writes and frozen values, if any.
    void Apply();

    /// Whether there is anything for Apply to do.
    bool HasWork() const {
        return has_work.load(std::memory_order_relaxed);
    }

private:
    void UpdateHasWork();

    System& system;

    mutable std::mutex mutex;
    std::vector<std::pair<VAddr, std::vector<u8>>> pending_writes;
    std::map<VAddr, std::vector<u8>> frozen;
    std::vector<std::function<void()>> tasks;
    std::atomic<bool> has_work{false};
};

} // namespace Core
