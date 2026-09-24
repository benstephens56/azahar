// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/tracer/recorder.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/renderer_base.h"

namespace VideoCore {

RendererBase::RendererBase(Core::System& system_, Frontend::EmuWindow& window,
                           Frontend::EmuWindow* secondary_window_)
    : system{system_}, render_window{window}, secondary_window{secondary_window_} {}

RendererBase::~RendererBase() = default;

u32 RendererBase::GetResolutionScaleFactor() {
    const auto graphics_api = Settings::GetWorkingGraphicsAPI();
    if (graphics_api == Settings::GraphicsAPI::Software) {
        // Software renderer always render at native resolution
        return 1;
    }

    const u32 scale_factor = Settings::values.resolution_factor.GetValue();
    return scale_factor != 0 ? scale_factor
                             : render_window.GetFramebufferLayout().GetScalingRatio();
}

void RendererBase::UpdateCurrentFramebufferLayout(bool is_portrait_mode) {
    const auto update_layout = [is_portrait_mode](Frontend::EmuWindow& window) {
        const Layout::FramebufferLayout& layout = window.GetFramebufferLayout();
        window.UpdateCurrentFramebufferLayout(layout.width, layout.height, is_portrait_mode);
    };
    update_layout(render_window);
    if (secondary_window != nullptr) {
        update_layout(*secondary_window);
    }
}

void RendererBase::EndFrame() {
    if (refreshing_screen) {
        return;
    }

    current_frame++;

    system.perf_stats->EndSystemFrame();

    render_window.PollEvents();

    // Pause when the TAS editor reached the frame it was seeking to
    if (system.Movie().IsTasEditorEnabled() && system.Movie().TasOnVBlank()) {
        system.frame_limiter.SetUnthrottled(false);
        system.frame_limiter.SetFrameAdvancing(true);
    }

    system.frame_limiter.DoFrameLimiting(system.CoreTiming().GetGlobalTimeUs());
    system.perf_stats->BeginSystemFrame();
}

void RendererBase::RefreshScreen() {
    refreshing_screen = true;
    SwapBuffers();
    refreshing_screen = false;
}

bool RendererBase::IsScreenshotPending() const {
    return settings.screenshot_requested;
}

void RendererBase::RequestScreenshot(void* data, std::function<void(bool)> callback,
                                     const Layout::FramebufferLayout& layout) {
    if (settings.screenshot_requested) {
        LOG_ERROR(Render, "A screenshot is already requested or in progress, ignoring the request");
        return;
    }
    settings.screenshot_bits = data;
    settings.screenshot_complete_callback = callback;
    settings.screenshot_framebuffer_layout = layout;
    settings.screenshot_requested = true;
}
} // namespace VideoCore
