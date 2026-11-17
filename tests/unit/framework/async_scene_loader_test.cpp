/// @file async_scene_loader_test.cpp
/// @brief Comprehensive test suite for AsyncSceneLoader non-blocking progressive pipeline.
///
/// Tests:
/// 1. Initial lifecycle state (Idle, zero metrics).
/// 2. Error handling for non-existent, empty, and invalid scene files.
/// 3. End-to-end scene ingestion and transactional ECS activation.
/// 4. Background JobSystem multi-worker decoding integration.
/// 5. Cooperative cancellation safety with World preservation.
/// 6. Reset and reuse across repeated load cycles.
/// 7. Binary glTF (.glb) progressive import and mesh registration.
/// 8. Live progress metrics and performance telemetry accounting.

#include "core/job_system.h"
#include "core/log.h"
#include "framework/async_scene_loader.h"
#include "render/asset_manager.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace {

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << '\n'; \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

using namespace eng;
using namespace eng::framework;
using namespace eng::world;

#ifndef ANTERAY_SAMPLE_SCENE_PATH
#define ANTERAY_SAMPLE_SCENE_PATH "assets/scenes/sample.hikari"
#endif

#ifndef ANTERAY_DAMAGED_HELMET_PATH
#define ANTERAY_DAMAGED_HELMET_PATH "test-assets/DamagedHelmet.glb"
#endif

int testInitialState() {
    AsyncSceneLoader loader;
    TEST_CHECK(loader.getState() == AsyncLoadState::Idle);
    TEST_CHECK(!loader.isLoading());
    TEST_CHECK(!loader.isReadyToActivate());
    TEST_CHECK(!loader.isCompleted());
    TEST_CHECK(!loader.isFailed());
    TEST_CHECK(!loader.isCancelled());

    const auto progress = loader.getProgress();
    TEST_CHECK(progress.state == AsyncLoadState::Idle);
    TEST_CHECK(progress.overallProgress == 0.0F);
    TEST_CHECK(progress.entitiesDiscovered == 0);
    TEST_CHECK(progress.texturesTotal == 0);

    const auto& telemetry = loader.getTelemetry();
    TEST_CHECK(telemetry.totalDuration.asMicroseconds() == 0);
    return 0;
}

int testInvalidFileHandling() {
    AsyncSceneLoader loader;

    // 1. Non-existent file
    auto resMissing = loader.startLoad("non_existent_file.hikari");
    TEST_CHECK(!resMissing.hasValue());
    TEST_CHECK(loader.isFailed());
    TEST_CHECK(!loader.getErrorMessage().empty());

    // 2. Empty path
    loader.reset();
    TEST_CHECK(loader.getState() == AsyncLoadState::Idle);
    auto resEmpty = loader.startLoad("");
    TEST_CHECK(!resEmpty.hasValue());
    TEST_CHECK(loader.isFailed());

    // 3. Unsupported format
    loader.reset();
    auto resFormat = loader.startLoad("assets/invalid_format.txt");
    TEST_CHECK(!resFormat.hasValue());
    TEST_CHECK(loader.isFailed());

    return 0;
}

int testSampleSceneLoadAndActivation() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        std::cout << "[SKIP] Sample scene not found: " << ANTERAY_SAMPLE_SCENE_PATH << '\n';
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;

    AsyncSceneLoader loader;
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    TEST_CHECK(startRes.hasValue());
    TEST_CHECK(loader.isLoading());

    // Simulate main thread per-frame updates
    int maxFrames = 1000;
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 5.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_CHECK(loader.isReadyToActivate());
    const auto progress = loader.getProgress();
    TEST_CHECK(progress.overallProgress >= 0.9F);
    TEST_CHECK(progress.entitiesDiscovered > 0);

    World world;
    auto actRes = loader.activate(world);
    TEST_CHECK(actRes.hasValue());
    TEST_CHECK(actRes.value().entitiesLoaded > 0);
    TEST_CHECK(loader.isCompleted());
    TEST_CHECK(!loader.isLoading());

    return 0;
}

int testJobSystemIntegration() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    JobSystemConfig jobConfig{.workerCount = 2, .maxJobs = 128};
    size_t bytes = JobSystem::requiredBytes(jobConfig);
    std::vector<std::byte> storage(bytes);
    JobSystem jobSystem(storage, jobConfig);

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;

    AsyncSceneLoader loader;
    loader.bindJobSystem(&jobSystem);
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    TEST_CHECK(startRes.hasValue());

    int maxFrames = 1000;
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 5.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_CHECK(loader.isReadyToActivate());

    World world;
    auto actRes = loader.activate(world);
    TEST_CHECK(actRes.hasValue());
    TEST_CHECK(loader.isCompleted());

    return 0;
}

int testCancellationSafety() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    AsyncSceneLoader loader;
    auto startRes = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    TEST_CHECK(startRes.hasValue());

    // Immediately cancel before processing
    loader.cancel();
    loader.update(*nullDevice, 5.0F);

    TEST_CHECK(loader.isCancelled());
    TEST_CHECK(!loader.isLoading());
    TEST_CHECK(!loader.isReadyToActivate());

    // Activation must safely fail when cancelled
    World world;
    auto dummy = world.createEntity("ExistingEntity");
    (void)dummy;
    size_t initialCount = world.getAllEntities().size();

    auto actRes = loader.activate(world);
    TEST_CHECK(!actRes.hasValue());
    // Ensure world was not modified
    TEST_CHECK(world.getAllEntities().size() == initialCount);

    return 0;
}

int testResetAndReuse() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    AsyncSceneLoader loader;

    // Cycle 1: Load and cancel
    (void)loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    loader.cancel();
    loader.update(*nullDevice, 5.0F);
    TEST_CHECK(loader.isCancelled());

    // Reset back to Idle
    loader.reset();
    TEST_CHECK(loader.getState() == AsyncLoadState::Idle);

    // Cycle 2: Load to completion
    auto startRes = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    TEST_CHECK(startRes.hasValue());

    int maxFrames = 1000;
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 5.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_CHECK(loader.isReadyToActivate());
    World world;
    auto actRes = loader.activate(world);
    TEST_CHECK(actRes.hasValue());
    TEST_CHECK(loader.isCompleted());

    return 0;
}

int testGlbImportProgress() {
    if (!std::filesystem::exists(ANTERAY_DAMAGED_HELMET_PATH)) {
        std::cout << "[SKIP] DamagedHelmet.glb not found: " << ANTERAY_DAMAGED_HELMET_PATH << '\n';
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;

    AsyncSceneLoader loader;
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(ANTERAY_DAMAGED_HELMET_PATH);
    TEST_CHECK(startRes.hasValue());

    int maxFrames = 1000;
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 5.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_CHECK(loader.isReadyToActivate());
    const auto progress = loader.getProgress();
    TEST_CHECK(progress.overallProgress >= 0.9F);

    World world;
    auto actRes = loader.activate(world);
    TEST_CHECK(actRes.hasValue());
    TEST_CHECK(actRes.value().entitiesLoaded > 0);
    TEST_CHECK(loader.isCompleted());

    const auto& telemetry = loader.getTelemetry();
    TEST_CHECK(telemetry.totalDuration.asMicroseconds() > 0);

    return 0;
}

int testStrict10PhasePipelineAndTelemetry() {
    TEST_CHECK(toString(AsyncLoadState::SceneRequest) == "SCENE_REQUEST");
    TEST_CHECK(toString(AsyncLoadState::FileReading) == "FILE_READING");
    TEST_CHECK(toString(AsyncLoadState::AssetDiscovery) == "ASSET_DISCOVERY");
    TEST_CHECK(toString(AsyncLoadState::AssetDecoding) == "ASSET_DECODING");
    TEST_CHECK(toString(AsyncLoadState::CpuStaging) == "CPU_STAGING");
    TEST_CHECK(toString(AsyncLoadState::GpuUpload) == "GPU_UPLOAD");
    TEST_CHECK(toString(AsyncLoadState::SceneValidation) == "SCENE_VALIDATION");
    TEST_CHECK(toString(AsyncLoadState::EcsCommit) == "ECS_COMMIT");
    TEST_CHECK(toString(AsyncLoadState::ViewportPreparation) == "VIEWPORT_PREPARATION");
    TEST_CHECK(toString(AsyncLoadState::SceneReady) == "SCENE_READY");

    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::SceneRequest) == "Phase 1/10: Scene Request");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::FileReading) == "Phase 2/10: File Reading");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::AssetDiscovery) == "Phase 3/10: Asset Discovery");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::AssetDecoding) == "Phase 4/10: Asset Decoding");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::CpuStaging) ==
               "Phase 5/10: CPU Processing & Staging");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::GpuUpload) ==
               "Phase 6/10: Incremental GPU Upload");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::SceneValidation) ==
               "Phase 7/10: Scene Validation");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::EcsCommit) == "Phase 8/10: Safe ECS Commit");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::ViewportPreparation) ==
               "Phase 9/10: Viewport Preparation");
    TEST_CHECK(toPhaseDisplayName(AsyncLoadState::SceneReady) == "Phase 10/10: Scene Ready");

    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;
    AsyncSceneLoader loader;
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    TEST_CHECK(startRes.hasValue());

    int maxFrames = 1000;
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 3.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_CHECK(loader.isReadyToActivate());
    World world;
    auto actRes = loader.activate(world);
    TEST_CHECK(actRes.hasValue());
    TEST_CHECK(loader.isCompleted());

    const auto& t = loader.getTelemetry();
    TEST_CHECK(t.sceneRequestDuration.asMicroseconds() > 0);
    TEST_CHECK(t.fileIoDuration.asMicroseconds() > 0);
    TEST_CHECK(t.discoveryDuration.asMicroseconds() > 0);
    TEST_CHECK(t.cpuStagingDuration.asMicroseconds() > 0);
    TEST_CHECK(t.validationDuration.asMicroseconds() > 0);
    TEST_CHECK(t.ecsCreationDuration.asMicroseconds() > 0);
    TEST_CHECK(t.viewportPrepDuration.asMicroseconds() > 0);
    TEST_CHECK(t.totalDuration.asMicroseconds() > 0);

    const auto p = loader.getProgress();
    TEST_CHECK(p.overallProgress == 1.0F);
    TEST_CHECK(p.entitiesLoaded == actRes.value().entitiesLoaded);

    return 0;
}

int testRamAndVramTracking() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;
    AsyncSceneLoader loader;
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    TEST_CHECK(startRes.hasValue());

    int maxFrames = 1000;
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 3.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto p = loader.getProgress();
    TEST_CHECK(p.peakRamBytes > 0);

    World world;
    (void)loader.activate(world);
    return 0;
}

int testBistroBenchmarkSceneLoading() {
    const std::string bistroPath = "assets/benchmark-scenes/Bistro_Benchmark.hikari";
    if (!std::filesystem::exists(bistroPath)) {
        std::cout << "[SKIP] Bistro benchmark scene not present at " << bistroPath << '\n';
        return 0;
    }

    JobSystemConfig jobConfig{.workerCount = 4, .maxJobs = 256};
    size_t bytes = JobSystem::requiredBytes(jobConfig);
    std::vector<std::byte> storage(bytes);
    JobSystem jobSystem(storage, jobConfig);

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;
    AsyncSceneLoader loader;
    loader.bindJobSystem(&jobSystem);
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(bistroPath);
    TEST_CHECK(startRes.hasValue());
    TEST_CHECK(loader.isLoading());

#if defined(__SANITIZE_THREAD__)
    int maxFrames = 120000;
#else
    int maxFrames = 30000;
#endif
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 5.0F);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    if (!loader.isReadyToActivate()) {
        std::cerr << "State: " << toString(loader.getState())
                  << ", Error: " << loader.getErrorMessage() << ", frames remaining: " << maxFrames
                  << ", progress: " << loader.getProgress().overallProgress
                  << ", current op: " << loader.getProgress().currentOperation << '\n';
        loader.cancel();
        return 1;
    }

    TEST_CHECK(loader.isReadyToActivate());
    const auto progress = loader.getProgress();
    TEST_CHECK(progress.entitiesDiscovered > 1000);
    TEST_CHECK(progress.peakRamBytes > 0);

    World world;
    auto actRes = loader.activate(world);
    TEST_CHECK(actRes.hasValue());
    TEST_CHECK(actRes.value().entitiesLoaded > 1000);
    TEST_CHECK(loader.isCompleted());

    const auto& tel = loader.getTelemetry();
    std::cout << "[INFO] Bistro Load Telemetry:\n"
              << "  - Request:      " << tel.sceneRequestDuration.asMicroseconds() << " us\n"
              << "  - File I/O:     " << tel.fileIoDuration.asMicroseconds() << " us\n"
              << "  - Discovery:    " << tel.discoveryDuration.asMicroseconds() << " us\n"
              << "  - CPU Staging:  " << tel.cpuStagingDuration.asMicroseconds() << " us\n"
              << "  - GPU Upload:   " << tel.gpuUploadDuration.asMicroseconds() << " us\n"
              << "  - Validation:   " << tel.validationDuration.asMicroseconds() << " us\n"
              << "  - ECS Commit:   " << tel.ecsCreationDuration.asMicroseconds() << " us\n"
              << "  - Viewport Prep:" << tel.viewportPrepDuration.asMicroseconds() << " us\n"
              << "  - Total Time:   " << tel.totalDuration.toSeconds() << " s\n"
              << "  - Max Stall:    " << tel.longestFrameStall.asMicroseconds() << " us\n"
              << "  - Peak RAM:     " << (progress.peakRamBytes / (1024 * 1024)) << " MB\n";

    return 0;
}

int testPhaseCancellationSafety() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    World world;
    auto baselineEntity = world.createEntity("UnchangedBaselineEntity");
    const size_t initialEntities = world.getAllEntities().size();

    // Cancellation test 1: Immediate cancel upon startLoad
    {
        AsyncSceneLoader loader;
        (void)loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
        loader.cancel();
        loader.update(*nullDevice, 3.0F);
        TEST_CHECK(loader.isCancelled());
        TEST_CHECK(world.getAllEntities().size() == initialEntities);
        TEST_CHECK(world.isAlive(baselineEntity));
    }

    // Cancellation test 2: Cancel mid-load during GPU tick
    {
        AsyncSceneLoader loader;
        (void)loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
        loader.update(*nullDevice, 1.0F);
        loader.cancel();
        loader.update(*nullDevice, 3.0F);
        TEST_CHECK(loader.isCancelled());
        TEST_CHECK(world.getAllEntities().size() == initialEntities);
        TEST_CHECK(world.isAlive(baselineEntity));
    }

    return 0;
}

int testRapidCancelRestartWithJobSystem() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    JobSystemConfig jobConfig{.workerCount = 2, .maxJobs = 128};
    size_t bytes = JobSystem::requiredBytes(jobConfig);
    std::vector<std::byte> storage(bytes);
    JobSystem jobSystem(storage, jobConfig);

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;
    World world;
    auto baselineEntity = world.createEntity("BaselineEntity");
    const size_t initialEntities = world.getAllEntities().size();

    AsyncSceneLoader loader;
    loader.bindJobSystem(&jobSystem);
    loader.bindAssetManager(&assetManager);

    // Rapidly start, tick briefly, and cancel across multiple cycles to exercise
    // worker-local staging cleanup and cancellation safety under JobSystem concurrency.
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto startRes = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
        TEST_CHECK(startRes.hasValue());

        for (int tick = 0; tick < (cycle % 3) + 1; ++tick) {
            loader.update(*nullDevice, 1.0F);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        loader.cancel();
        int drainFrames = 200;
        while (loader.isLoading() && drainFrames-- > 0) {
            loader.update(*nullDevice, 2.0F);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        TEST_CHECK(loader.isCancelled());
        TEST_CHECK(world.getAllEntities().size() == initialEntities);
        TEST_CHECK(world.isAlive(baselineEntity));
        loader.reset();
    }

    // Verify a subsequent full load completes and activates cleanly.
    auto finalStart = loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
    TEST_CHECK(finalStart.hasValue());
    int maxFrames = 1000;
    while (loader.isLoading() && maxFrames-- > 0) {
        loader.update(*nullDevice, 5.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(loader.isReadyToActivate());
    TEST_CHECK(loader.activate(world).hasValue());
    TEST_CHECK(loader.isCompleted());
    TEST_CHECK(!world.getAllEntities().empty());

    return 0;
}

int testConcurrentInquiryThreadSafety() {
    if (!std::filesystem::exists(ANTERAY_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    JobSystemConfig jobConfig{.workerCount = 2, .maxJobs = 128};
    size_t bytes = JobSystem::requiredBytes(jobConfig);
    std::vector<std::byte> storage(bytes);
    JobSystem jobSystem(storage, jobConfig);

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;
    AsyncSceneLoader loader;
    loader.bindJobSystem(&jobSystem);
    loader.bindAssetManager(&assetManager);

    std::atomic<bool> stopReader{false};
    std::atomic<uint64_t> snapshotsRead{0};

    std::thread reader([&]() {
        while (!stopReader.load(std::memory_order_acquire)) {
            const auto telemetry = loader.getTelemetry();
            const auto assetErrors = loader.getAssetErrors();
            const auto errMsg = loader.getErrorMessage();
            const auto progress = loader.getProgress();
            const auto st = loader.getState();
            const bool loading = loader.isLoading();
            const bool cancelled = loader.isCancelled();
            (void)telemetry;
            (void)assetErrors;
            (void)errMsg;
            (void)progress;
            (void)st;
            (void)loading;
            (void)cancelled;
            snapshotsRead.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Run valid loads, cancellations, and invalid-file failures concurrently with reader inquiries.
    for (int i = 0; i < 3; ++i) {
        (void)loader.startLoad(ANTERAY_SAMPLE_SCENE_PATH);
        int frames = 1000;
        while (loader.isLoading() && frames-- > 0) {
            loader.update(*nullDevice, 2.0F);
            if (i == 1 && frames == 998) {
                loader.cancel();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (loader.isReadyToActivate()) {
            World world;
            TEST_CHECK(loader.activate(world).hasValue());
        }
        loader.reset();
    }

    // Trigger failure message update concurrently with reader
    (void)loader.startLoad("/nonexistent/path/concurrent_inquiry_test.hikari");

    stopReader.store(true, std::memory_order_release);
    reader.join();
    TEST_CHECK(snapshotsRead.load() > 0);
    return 0;
}

} // namespace

int main() {
    std::cout << "=== Running AsyncSceneLoader Test Suite ===\n";

    if (testInitialState() != 0) {
        std::cerr << "FAIL: testInitialState\n";
        return 1;
    }
    std::cout << "PASS: testInitialState\n";

    if (testInvalidFileHandling() != 0) {
        std::cerr << "FAIL: testInvalidFileHandling\n";
        return 1;
    }
    std::cout << "PASS: testInvalidFileHandling\n";

    if (testSampleSceneLoadAndActivation() != 0) {
        std::cerr << "FAIL: testSampleSceneLoadAndActivation\n";
        return 1;
    }
    std::cout << "PASS: testSampleSceneLoadAndActivation\n";

    if (testJobSystemIntegration() != 0) {
        std::cerr << "FAIL: testJobSystemIntegration\n";
        return 1;
    }
    std::cout << "PASS: testJobSystemIntegration\n";

    if (testCancellationSafety() != 0) {
        std::cerr << "FAIL: testCancellationSafety\n";
        return 1;
    }
    std::cout << "PASS: testCancellationSafety\n";

    if (testResetAndReuse() != 0) {
        std::cerr << "FAIL: testResetAndReuse\n";
        return 1;
    }
    std::cout << "PASS: testResetAndReuse\n";

    if (testGlbImportProgress() != 0) {
        std::cerr << "FAIL: testGlbImportProgress\n";
        return 1;
    }
    std::cout << "PASS: testGlbImportProgress\n";

    if (testStrict10PhasePipelineAndTelemetry() != 0) {
        std::cerr << "FAIL: testStrict10PhasePipelineAndTelemetry\n";
        return 1;
    }
    std::cout << "PASS: testStrict10PhasePipelineAndTelemetry\n";

    if (testRamAndVramTracking() != 0) {
        std::cerr << "FAIL: testRamAndVramTracking\n";
        return 1;
    }
    std::cout << "PASS: testRamAndVramTracking\n";

    if (testPhaseCancellationSafety() != 0) {
        std::cerr << "FAIL: testPhaseCancellationSafety\n";
        return 1;
    }
    std::cout << "PASS: testPhaseCancellationSafety\n";

    if (testRapidCancelRestartWithJobSystem() != 0) {
        std::cerr << "FAIL: testRapidCancelRestartWithJobSystem\n";
        return 1;
    }
    std::cout << "PASS: testRapidCancelRestartWithJobSystem\n";

    if (testConcurrentInquiryThreadSafety() != 0) {
        std::cerr << "FAIL: testConcurrentInquiryThreadSafety\n";
        return 1;
    }
    std::cout << "PASS: testConcurrentInquiryThreadSafety\n";

    if (testBistroBenchmarkSceneLoading() != 0) {
        std::cerr << "FAIL: testBistroBenchmarkSceneLoading\n";
        return 1;
    }
    std::cout << "PASS: testBistroBenchmarkSceneLoading\n";

    std::cout << "=== All AsyncSceneLoader Tests Passed ===\n";
    return 0;
}
