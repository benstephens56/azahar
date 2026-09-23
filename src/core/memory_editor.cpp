// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <future>
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/memory.h"
#include "core/memory_editor.h"

namespace Core {

namespace {

std::shared_ptr<Kernel::Process> GetApplicationProcess(System& system) {
    if (!system.IsPoweredOn()) {
        return nullptr;
    }
    return system.Kernel().GetProcessById(system.CheatEngine().GetConnectedPID());
}

} // namespace

MemoryEditor::MemoryEditor(System& system_) : system{system_} {}

MemoryEditor::~MemoryEditor() = default;

std::optional<std::vector<u8>> MemoryEditor::Peek(VAddr address, std::size_t size) const {
    const auto process = GetApplicationProcess(system);
    if (!process) {
        return std::nullopt;
    }
    auto& pointers = process->vm_manager.page_table->GetPointerArray();

    std::vector<u8> result(size);
    std::size_t done = 0;
    while (done < size) {
        const VAddr current = address + static_cast<VAddr>(done);
        const u8* page = pointers[current >> Memory::CITRA_PAGE_BITS];
        if (!page) {
            return std::nullopt;
        }
        const std::size_t page_offset = current & Memory::CITRA_PAGE_MASK;
        const std::size_t chunk = std::min(size - done, Memory::CITRA_PAGE_SIZE - page_offset);
        std::memcpy(result.data() + done, page + page_offset, chunk);
        done += chunk;
    }
    return result;
}

bool MemoryEditor::IsPeekable(VAddr address) const {
    const auto process = GetApplicationProcess(system);
    if (!process) {
        return false;
    }
    return process->vm_manager.page_table->GetPointerArray()[address >> Memory::CITRA_PAGE_BITS] !=
           nullptr;
}

void MemoryEditor::QueueWrite(VAddr address, std::vector<u8> bytes) {
    {
        std::scoped_lock lock{mutex};
        pending_writes.emplace_back(address, std::move(bytes));
        UpdateHasWork();
    }
    // Apply right away if the emulator is paused by frame advancing
    system.frame_limiter.RequestPausedWork();
}

void MemoryEditor::Freeze(VAddr address, std::vector<u8> bytes) {
    {
        std::scoped_lock lock{mutex};
        frozen[address] = std::move(bytes);
        UpdateHasWork();
    }
    system.frame_limiter.RequestPausedWork();
}

void MemoryEditor::Unfreeze(VAddr address) {
    std::scoped_lock lock{mutex};
    frozen.erase(address);
    UpdateHasWork();
}

bool MemoryEditor::IsFrozen(VAddr address) const {
    std::scoped_lock lock{mutex};
    return frozen.contains(address);
}

std::optional<std::vector<u8>> MemoryEditor::GetFrozenBytes(VAddr address) const {
    std::scoped_lock lock{mutex};
    const auto it = frozen.find(address);
    if (it == frozen.end()) {
        return std::nullopt;
    }
    return it->second;
}

void MemoryEditor::ClearAll() {
    std::scoped_lock lock{mutex};
    pending_writes.clear();
    frozen.clear();
    tasks.clear();
    UpdateHasWork();
}

std::optional<std::vector<MemoryEditor::MemoryRegion>> MemoryEditor::SnapshotWritableMemory(
    std::chrono::milliseconds timeout) {
    if (!GetApplicationProcess(system)) {
        return std::nullopt;
    }

    auto promise = std::make_shared<std::promise<std::vector<MemoryRegion>>>();
    auto future = promise->get_future();
    auto task = [this, promise] {
        std::vector<MemoryRegion> regions;
        const auto process = GetApplicationProcess(system);
        if (process) {
            auto& pointers = process->vm_manager.page_table->GetPointerArray();
            constexpr u8 read_write = static_cast<u8>(Kernel::VMAPermission::ReadWrite);
            for (const auto& [base, vma] : process->vm_manager.vma_map) {
                if (vma.type != Kernel::VMAType::BackingMemory ||
                    (static_cast<u8>(vma.permissions) & read_write) != read_write) {
                    continue;
                }
                const VAddr end = vma.base + vma.size;
                MemoryRegion* current = nullptr;
                for (VAddr page = vma.base; page < end; page += Memory::CITRA_PAGE_SIZE) {
                    const u8* pointer = pointers[page >> Memory::CITRA_PAGE_BITS];
                    // Skip VRAM and pages that are not plain memory (e.g. cached by the GPU)
                    const bool is_vram =
                        page >= Memory::VRAM_VADDR && page < Memory::VRAM_VADDR_END;
                    if (!pointer || is_vram) {
                        current = nullptr;
                        continue;
                    }
                    if (!current) {
                        current = &regions.emplace_back(MemoryRegion{page, {}});
                    }
                    current->data.insert(current->data.end(), pointer,
                                         pointer + Memory::CITRA_PAGE_SIZE);
                }
            }
        }
        promise->set_value(std::move(regions));
    };

    {
        std::scoped_lock lock{mutex};
        tasks.emplace_back(std::move(task));
        UpdateHasWork();
    }
    system.frame_limiter.RequestPausedWork();

    if (future.wait_for(timeout) != std::future_status::ready) {
        return std::nullopt;
    }
    try {
        return future.get();
    } catch (const std::future_error&) {
        // The task was dropped, e.g. because emulation was stopped
        return std::nullopt;
    }
}

void MemoryEditor::UpdateHasWork() {
    has_work.store(!pending_writes.empty() || !frozen.empty() || !tasks.empty(),
                   std::memory_order_relaxed);
}

void MemoryEditor::Apply() {
    if (!HasWork()) {
        return;
    }
    const auto process = GetApplicationProcess(system);
    if (!process) {
        return;
    }
    auto& memory = system.Memory();

    std::scoped_lock lock{mutex};
    for (const auto& task : tasks) {
        task();
    }
    tasks.clear();

    const auto write = [&](VAddr address, const std::vector<u8>& bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            // Skip unmapped addresses instead of logging an error for every frozen write
            if (!memory.IsValidVirtualAddress(*process, address + static_cast<VAddr>(i))) {
                return;
            }
        }
        memory.WriteBlock(*process, address, bytes.data(), bytes.size());
        system.InvalidateCacheRange(address, bytes.size());
    };
    for (const auto& [address, bytes] : pending_writes) {
        write(address, bytes);
    }
    pending_writes.clear();
    for (const auto& [address, bytes] : frozen) {
        write(address, bytes);
    }
    UpdateHasWork();
}

} // namespace Core
