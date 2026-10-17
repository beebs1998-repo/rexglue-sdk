/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/graphics_system.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",
                    "Store shaders persistently and load them when loading games to avoid "
                    "runtime spikes and freezes when playing the game not for the first time.");

// Games that pace simulation off VBlanks implement slowdown as frame-skipping
// (one update per N VBlanks), which halves the effective framerate. When this
// is enabled the VBlank worker detects swaps arriving every 2+ VBlanks and
// temporarily doubles the VBlank rate, restoring ~full motion speed during
// heavy scenes while leaving normal-pace pacing untouched.
// NOTE: Testing (dfkbl, 2026-08-25) proved the game's slowdown depth is
// independent of VBlank rate (multiplier 1 vs 3 produced identical results),
// i.e. the mechanic is guest-time-based, not vblank-count-based. Boost
// therefore defaults OFF; kept available since some titles DO pace off
// vblanks and it only ever engages on detected skip patterns.
REXCVAR_DEFINE_BOOL(slowdown_pacing_boost, false, "GPU",
                    "Detect game-side frame skipping (slowdown) and temporarily raise the "
                    "VBlank rate to compensate pacing. Only helps titles whose slowdown is "
                    "vblank-count-based; has no effect on time-based slowdown mechanics.");

// Per-second telemetry correlating guest swap cadence against VBlank cadence.
// Use it during a heavy section: if swaps/s collapses while vblanks/s stays
// constant, the game's plateau pacing is self-timed (kernel waits), and the
// measured swaps/s IS the assist's target rate.
REXCVAR_DEFINE_BOOL(slowdown_debug_log, false, "GPU",
                    "Log per-second VBlank/swap cadence and boost state to diagnose "
                    "game slowdown mechanics.");

REXCVAR_DEFINE_DOUBLE(slowdown_pacing_boost_multiplier, 1.5, "GPU",
                      "VBlank rate multiplier applied while slowdown boost is engaged. "
                      "Effective slowdown sim rate ~= 30 Hz * multiplier at a 60 Hz video "
                      "mode (1.5 ~= 45 fps, 2.0 = parity with normal pace). Values above "
                      "2.0 make heavy scenes run FASTER than normal pace. Read every "
                      "frame, so it can also be tuned live via the settings overlay (F4).")
    .range(1.0, 3.0);

namespace {

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode;
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();

        // Adaptive slowdown compensation state. The game skips simulation
        // updates during heavy scenes (a swap every >=2 VBlanks); raising the
        // VBlank rate while that persists restores motion speed. Detection
        // uses a windowed time-weighted average of the vblank-to-swap gap:
        // per-vblank swaps average ~0.5, half-rate skipping ~1.5, so the
        // thresholds below cleanly separate normal play from slowdown.
        bool boost_engaged = false;
        uint32_t boost_slow_windows = 0;
        uint32_t boost_fast_windows = 0;
        double gap_window_sum = 0.0;
        uint32_t gap_window_samples = 0;
        uint32_t gap_window_ticks = 0;
        constexpr uint32_t kGapWindowTicks = 96;  // ~96 ms per decision window
        const double recent_swap_window_seconds = 2.0;
        const uint64_t recent_swap_max_gap =
            std::max<uint64_t>(1, uint64_t(refresh_rate_hz * recent_swap_window_seconds));

        // Debug telemetry state (slowdown_debug_log).
        const bool debug_log = REXCVAR_GET(slowdown_debug_log);
        uint32_t debug_tick = 0;
        constexpr uint32_t kDebugLogTicks = 1000;  // ~1 s at the 1 ms worker cadence
        uint64_t dbg_last_vb = 0;
        uint64_t dbg_last_swaps = 0;
        double dbg_gap_sum = 0.0;
        uint32_t dbg_gap_n = 0;

        while (vsync_worker_running_) {
          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          uint64_t interval_ticks =
              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;

          if (REXCVAR_GET(vsync) && REXCVAR_GET(slowdown_pacing_boost)) {
            const uint64_t vb = vblank_count_.load(std::memory_order_relaxed);
            const uint64_t last_swap = last_swap_vblank_.load(std::memory_order_relaxed);
            const uint64_t gap = vb - last_swap;
            const bool actively_rendering = gap <= recent_swap_max_gap;

            ++gap_window_ticks;
            gap_window_sum += static_cast<double>(gap);
            ++gap_window_samples;
            if (gap_window_ticks >= kGapWindowTicks) {
              const double avg_gap =
                  gap_window_samples ? gap_window_sum / gap_window_samples : 0.0;
              gap_window_sum = 0.0;
              gap_window_samples = 0;
              gap_window_ticks = 0;
              if (actively_rendering && avg_gap >= 1.3) {
                boost_fast_windows = 0;
                if (++boost_slow_windows >= 2) {
                  boost_engaged = true;
                }
              } else if (!actively_rendering || avg_gap <= 0.9) {
                boost_slow_windows = 0;
                if (++boost_fast_windows >= 2) {
                  boost_engaged = false;
                }
              }
            }

            if (boost_engaged) {
              const double mult = REXCVAR_GET(slowdown_pacing_boost_multiplier);
              if (mult > 1.001) {
                interval_ticks = std::max(
                    uint64_t(1), uint64_t(double(interval_ticks) / mult));
              }
            }
          }

          while (current_time - last_frame_time >= interval_ticks) {
            MarkVblank();
            last_frame_time += interval_ticks;
          }

          if (debug_log) {
            ++debug_tick;
            const uint64_t vb_now = vblank_count_.load(std::memory_order_relaxed);
            const uint64_t last_swap_now =
                last_swap_vblank_.load(std::memory_order_relaxed);
            dbg_gap_sum += static_cast<double>(vb_now - last_swap_now);
            ++dbg_gap_n;
            if (debug_tick >= kDebugLogTicks) {
              const uint64_t sw_now = swap_count_.load(std::memory_order_relaxed);
              const double avg_gap =
                  dbg_gap_n ? dbg_gap_sum / dbg_gap_n : 0.0;
              REXGPU_INFO(
                  "[slowdown-debug] vb/s={} swaps/s={} avg_gap={:.2f} boost={}",
                  vb_now - dbg_last_vb, sw_now - dbg_last_swaps, avg_gap,
                  boost_engaged ? "on" : "off");
              dbg_last_vb = vb_now;
              dbg_last_swaps = sw_now;
              dbg_gap_sum = 0.0;
              dbg_gap_n = 0;
              debug_tick = 0;
            }
          }

          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

void GraphicsSystem::NotifySwapSubmitted() {
  swap_count_.fetch_add(1, std::memory_order_relaxed);
  last_swap_vblank_.store(vblank_count_.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  vblank_count_.fetch_add(1, std::memory_order_relaxed);
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!REXCVAR_GET(store_shaders)) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
