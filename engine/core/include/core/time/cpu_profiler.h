/// @file cpu_profiler.h
/// @brief Low-overhead scoped CPU frame profiler with percentile statistics.
///
/// Purpose:
/// Provides non-allocating, stack-based CPU scope timing for diagnosing frame-time
/// bottlenecks across the engine, renderer, RHI, and editor.
///
/// Contract:
/// Call configure() before the frame loop to pre-allocate sample buffers.
/// Call beginFrame() / endFrame() once per frame on the main thread.
/// ScopedCpuTimer instances must be strictly LIFO (stack-allocated) on the main thread.
///
/// Thread-safety:
/// Main-thread only. Calls from worker threads are ignored safely without locking.
///
/// Cost:
/// Zero heap allocations during beginFrame(), endFrame(), and ScopedCpuTimer when configured.

#pragma once

#include <array>
#include <core/time/timestamp.h>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace eng {

enum class CpuScope : uint8_t {
    MainFrameLoop = 0,
    CpuFrame,
    PollEvents,
    AcquireNextImage,
    EcsSystemsAndQueries,
    TransformPropagation,
    EditorBeginFrame,
    EditorDrawPanels,
    SceneHierarchyPanel,
    InspectorPanel,
    ContentBrowserPanel,
    ConsoleAndTelemetryPanels,
    ViewportPanel,
    CameraUpdateAndInput,
    SceneRendererPrepare,
    VisibilityAndCulling,
    ShadowPreparation,
    RenderGraphCompileExec,
    PostProcess,
    RhiCommandRecordAndSubmit,
    VulkanSyncAndWaits,
    PresentAndFramePacing,
    Count
};

struct CpuScopeStats {
    uint32_t sampleCount{0};
    double avgMs{0.0};
    double p50Ms{0.0};
    double p95Ms{0.0};
    double p99Ms{0.0};
    double maxMs{0.0};
};

class CpuProfiler {
public:
    static constexpr uint32_t kScopeCount = static_cast<uint32_t>(CpuScope::Count);
    static constexpr uint32_t kMaxStackDepth = 64;

    static CpuProfiler& instance() noexcept;

    /// @brief Pre-allocates sample buffers for maxFrames frames. Does not start recording.
    void configure(uint32_t maxFrames);

    /// @brief Enables or disables sample recording.
    void setRecording(bool enabled) noexcept;

    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }
    [[nodiscard]] bool isRecording() const noexcept { return recording_; }
    [[nodiscard]] uint32_t recordedFrames() const noexcept { return recordedFrames_; }
    [[nodiscard]] uint32_t maxFrames() const noexcept { return maxFrames_; }

    /// @brief Resets recorded samples without reallocating.
    void resetSamples() noexcept;

    /// @brief Marks the start of a frame on the main thread.
    void beginFrame() noexcept;

    /// @brief Marks the end of a frame and commits per-scope totals if recording.
    void endFrame() noexcept;

    /// @brief Pushes a timing scope onto the active stack. Returns stack index or UINT32_MAX.
    uint32_t pushScope(CpuScope scope) noexcept;

    /// @brief Pops a timing scope from the active stack and records inclusive/exclusive ns.
    void popScope(uint32_t stackIndex) noexcept;

    /// @brief Computes percentile statistics for a scope across recorded frames.
    [[nodiscard]] CpuScopeStats computeStats(CpuScope scope, bool exclusive = false) const;

    /// @brief Returns human-readable name for a scope.
    [[nodiscard]] static std::string_view scopeName(CpuScope scope) noexcept;

    /// @brief Prints a formatted report of all scopes to stdout.
    void printReport(std::string_view scenarioName) const;

private:
    CpuProfiler() = default;

    struct ActiveEntry {
        CpuScope scope{CpuScope::MainFrameLoop};
        int64_t startNs{0};
        int64_t childNs{0};
    };

    bool enabled_{false};
    bool recording_{false};
    uint32_t maxFrames_{0};
    uint32_t recordedFrames_{0};
    std::thread::id mainThreadId_{};

    std::array<ActiveEntry, kMaxStackDepth> stack_{};
    uint32_t stackDepth_{0};

    std::array<int64_t, kScopeCount> frameInclusiveNs_{};
    std::array<int64_t, kScopeCount> frameExclusiveNs_{};

    std::array<std::vector<int64_t>, kScopeCount> inclusiveSamplesNs_{};
    std::array<std::vector<int64_t>, kScopeCount> exclusiveSamplesNs_{};
};

class ScopedCpuTimer {
public:
    explicit ScopedCpuTimer(CpuScope scope) noexcept
        : stackIndex_(CpuProfiler::instance().isEnabled() ? CpuProfiler::instance().pushScope(scope)
                                                          : UINT32_MAX) {}

    ~ScopedCpuTimer() noexcept {
        if (stackIndex_ != UINT32_MAX) {
            CpuProfiler::instance().popScope(stackIndex_);
        }
    }

    ScopedCpuTimer(const ScopedCpuTimer&) = delete;
    ScopedCpuTimer& operator=(const ScopedCpuTimer&) = delete;

private:
    uint32_t stackIndex_{UINT32_MAX};
};

} // namespace eng

#define ENG_CPU_PROFILE_CONCAT_IMPL(a, b) a##b
#define ENG_CPU_PROFILE_CONCAT(a, b) ENG_CPU_PROFILE_CONCAT_IMPL(a, b)
#define ENG_CPU_PROFILE_SCOPE(scope)                                                               \
    ::eng::ScopedCpuTimer ENG_CPU_PROFILE_CONCAT(engCpuTimer_, __LINE__)(::eng::CpuScope::scope)
