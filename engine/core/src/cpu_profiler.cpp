#include <algorithm>
#include <core/time/cpu_profiler.h>
#include <cstdio>

namespace eng {

CpuProfiler& CpuProfiler::instance() noexcept {
    static CpuProfiler sInstance;
    return sInstance;
}

void CpuProfiler::configure(uint32_t maxFrames) {
    maxFrames_ = maxFrames;
    recordedFrames_ = 0;
    mainThreadId_ = std::this_thread::get_id();
    for (uint32_t i = 0; i < kScopeCount; ++i) {
        inclusiveSamplesNs_[i].clear();
        inclusiveSamplesNs_[i].reserve(maxFrames);
        exclusiveSamplesNs_[i].clear();
        exclusiveSamplesNs_[i].reserve(maxFrames);
    }
    frameInclusiveNs_.fill(0);
    frameExclusiveNs_.fill(0);
    stackDepth_ = 0;
    enabled_ = (maxFrames > 0);
}

void CpuProfiler::setRecording(bool enabled) noexcept {
    recording_ = enabled;
}

void CpuProfiler::resetSamples() noexcept {
    recordedFrames_ = 0;
    for (uint32_t i = 0; i < kScopeCount; ++i) {
        inclusiveSamplesNs_[i].clear();
        exclusiveSamplesNs_[i].clear();
    }
    frameInclusiveNs_.fill(0);
    frameExclusiveNs_.fill(0);
    stackDepth_ = 0;
}

void CpuProfiler::beginFrame() noexcept {
    if (!enabled_) {
        return;
    }
    frameInclusiveNs_.fill(0);
    frameExclusiveNs_.fill(0);
    stackDepth_ = 0;
}

void CpuProfiler::endFrame() noexcept {
    if (!enabled_) {
        return;
    }
    if (recording_ && recordedFrames_ < maxFrames_) {
        for (uint32_t i = 0; i < kScopeCount; ++i) {
            inclusiveSamplesNs_[i].push_back(frameInclusiveNs_[i]);
            exclusiveSamplesNs_[i].push_back(frameExclusiveNs_[i]);
        }
        recordedFrames_++;
    }
    frameInclusiveNs_.fill(0);
    frameExclusiveNs_.fill(0);
    stackDepth_ = 0;
}

uint32_t CpuProfiler::pushScope(CpuScope scope) noexcept {
    if (!enabled_ || std::this_thread::get_id() != mainThreadId_) {
        return UINT32_MAX;
    }
    if (stackDepth_ >= kMaxStackDepth) {
        return UINT32_MAX;
    }
    const uint32_t idx = stackDepth_++;
    stack_[idx].scope = scope;
    stack_[idx].startNs = Timestamp::now().nanosecondsSinceEpoch();
    stack_[idx].childNs = 0;
    return idx;
}

void CpuProfiler::popScope(uint32_t stackIndex) noexcept {
    if (!enabled_ || stackIndex == UINT32_MAX || stackDepth_ == 0) {
        return;
    }
    const int64_t endNs = Timestamp::now().nanosecondsSinceEpoch();
    // Clamp stackDepth_ safely if mismatched
    if (stackIndex >= stackDepth_) {
        return;
    }
    const ActiveEntry& entry = stack_[stackIndex];
    const int64_t inclusiveNs = std::max<int64_t>(0, endNs - entry.startNs);
    const int64_t exclusiveNs = std::max<int64_t>(0, inclusiveNs - entry.childNs);

    const uint32_t scopeIdx = static_cast<uint32_t>(entry.scope);
    frameInclusiveNs_[scopeIdx] += inclusiveNs;
    frameExclusiveNs_[scopeIdx] += exclusiveNs;

    stackDepth_ = stackIndex;
    if (stackDepth_ > 0) {
        stack_[stackDepth_ - 1].childNs += inclusiveNs;
    }
}

CpuScopeStats CpuProfiler::computeStats(CpuScope scope, bool exclusive) const {
    const uint32_t idx = static_cast<uint32_t>(scope);
    const auto& src = exclusive ? exclusiveSamplesNs_[idx] : inclusiveSamplesNs_[idx];
    CpuScopeStats stats{};
    if (src.empty()) {
        return stats;
    }

    std::vector<int64_t> sorted = src;
    std::sort(sorted.begin(), sorted.end());

    const size_t n = sorted.size();
    stats.sampleCount = static_cast<uint32_t>(n);

    double sumNs = 0.0;
    for (int64_t v : sorted) {
        sumNs += static_cast<double>(v);
    }
    constexpr double kNsToMs = 1.0 / 1'000'000.0;
    stats.avgMs = (sumNs / static_cast<double>(n)) * kNsToMs;

    auto pct = [&](double p) -> double {
        const size_t pos = static_cast<size_t>(p * static_cast<double>(n - 1));
        return static_cast<double>(sorted[pos]) * kNsToMs;
    };
    stats.p50Ms = pct(0.50);
    stats.p95Ms = pct(0.95);
    stats.p99Ms = pct(0.99);
    stats.maxMs = static_cast<double>(sorted.back()) * kNsToMs;
    return stats;
}

std::string_view CpuProfiler::scopeName(CpuScope scope) noexcept {
    switch (scope) {
    case CpuScope::MainFrameLoop:
        return "MainFrameLoop";
    case CpuScope::CpuFrame:
        return "CpuFrame";
    case CpuScope::PollEvents:
        return "PollEvents";
    case CpuScope::AcquireNextImage:
        return "AcquireNextImage";
    case CpuScope::EcsSystemsAndQueries:
        return "EcsSystemsAndQueries";
    case CpuScope::TransformPropagation:
        return "TransformPropagation";
    case CpuScope::EditorBeginFrame:
        return "EditorBeginFrame";
    case CpuScope::EditorDrawPanels:
        return "EditorDrawPanels";
    case CpuScope::SceneHierarchyPanel:
        return "SceneHierarchyPanel";
    case CpuScope::InspectorPanel:
        return "InspectorPanel";
    case CpuScope::ContentBrowserPanel:
        return "ContentBrowserPanel";
    case CpuScope::ConsoleAndTelemetryPanels:
        return "ConsoleAndTelemetryPanels";
    case CpuScope::ViewportPanel:
        return "ViewportPanel";
    case CpuScope::CameraUpdateAndInput:
        return "CameraUpdateAndInput";
    case CpuScope::SceneRendererPrepare:
        return "SceneRendererPrepare";
    case CpuScope::VisibilityAndCulling:
        return "VisibilityAndCulling";
    case CpuScope::ShadowPreparation:
        return "ShadowPreparation";
    case CpuScope::RenderGraphCompileExec:
        return "RenderGraphCompileExec";
    case CpuScope::PostProcess:
        return "PostProcess";
    case CpuScope::RhiCommandRecordAndSubmit:
        return "RhiCommandRecordAndSubmit";
    case CpuScope::VulkanSyncAndWaits:
        return "VulkanSyncAndWaits";
    case CpuScope::PresentAndFramePacing:
        return "PresentAndFramePacing";
    case CpuScope::Count:
        return "Count";
    }
    return "Unknown";
}

void CpuProfiler::printReport(std::string_view scenarioName) const {
    const CpuScopeStats mainStats = computeStats(CpuScope::MainFrameLoop, false);
    const double avgFps = mainStats.avgMs > 0.0 ? (1000.0 / mainStats.avgMs) : 0.0;
    std::printf("\n=== CPU PROFILE REPORT: %.*s (frames=%u, avgFPS=%.1f) ===\n",
                static_cast<int>(scenarioName.size()), scenarioName.data(), recordedFrames_,
                avgFps);
    std::printf("%-28s | %8s | %8s | %8s | %8s | %8s | %8s\n", "Scope (Inclusive)", "Avg(ms)",
                "P50(ms)", "P95(ms)", "P99(ms)", "Max(ms)", "SelfAvg");
    std::printf("-----------------------------+----------+----------+----------+----------+--------"
                "--+----------\n");
    double sumSelfMs = 0.0;
    for (uint32_t i = 0; i < kScopeCount; ++i) {
        const auto scope = static_cast<CpuScope>(i);
        const CpuScopeStats inc = computeStats(scope, false);
        const CpuScopeStats exc = computeStats(scope, true);
        if (inc.maxMs <= 0.0001 && exc.maxMs <= 0.0001) {
            continue;
        }
        sumSelfMs += exc.avgMs;
        const std::string_view name = scopeName(scope);
        std::printf("%-28.*s | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f\n",
                    static_cast<int>(name.size()), name.data(), inc.avgMs, inc.p50Ms, inc.p95Ms,
                    inc.p99Ms, inc.maxMs, exc.avgMs);
    }
    std::printf("-----------------------------+----------+----------+----------+----------+--------"
                "--+----------\n");
    std::printf("Sum of Exclusive SelfAvg: %.3f ms (MainFrameLoop Avg: %.3f ms)\n", sumSelfMs,
                mainStats.avgMs);
    std::fflush(stdout);
}

} // namespace eng
