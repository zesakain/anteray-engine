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

#ifndef HIKARI_SAMPLE_SCENE_PATH
#define HIKARI_SAMPLE_SCENE_PATH "assets/scenes/sample.hikari"
#endif

#ifndef HIKARI_DAMAGED_HELMET_PATH
#define HIKARI_DAMAGED_HELMET_PATH "test-assets/DamagedHelmet.glb"
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
    if (!std::filesystem::exists(HIKARI_SAMPLE_SCENE_PATH)) {
        std::cout << "[SKIP] Sample scene not found: " << HIKARI_SAMPLE_SCENE_PATH << '\n';
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;

    AsyncSceneLoader loader;
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(HIKARI_SAMPLE_SCENE_PATH);
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
    if (!std::filesystem::exists(HIKARI_SAMPLE_SCENE_PATH)) {
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

    auto startRes = loader.startLoad(HIKARI_SAMPLE_SCENE_PATH);
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
    if (!std::filesystem::exists(HIKARI_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    AsyncSceneLoader loader;
    auto startRes = loader.startLoad(HIKARI_SAMPLE_SCENE_PATH);
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
    if (!std::filesystem::exists(HIKARI_SAMPLE_SCENE_PATH)) {
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    AsyncSceneLoader loader;

    // Cycle 1: Load and cancel
    (void)loader.startLoad(HIKARI_SAMPLE_SCENE_PATH);
    loader.cancel();
    loader.update(*nullDevice, 5.0F);
    TEST_CHECK(loader.isCancelled());

    // Reset back to Idle
    loader.reset();
    TEST_CHECK(loader.getState() == AsyncLoadState::Idle);

    // Cycle 2: Load to completion
    auto startRes = loader.startLoad(HIKARI_SAMPLE_SCENE_PATH);
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
    if (!std::filesystem::exists(HIKARI_DAMAGED_HELMET_PATH)) {
        std::cout << "[SKIP] DamagedHelmet.glb not found: " << HIKARI_DAMAGED_HELMET_PATH << '\n';
        return 0;
    }

    auto nullDevRes = rhi::createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    render::AssetManager assetManager;

    AsyncSceneLoader loader;
    loader.bindAssetManager(&assetManager);

    auto startRes = loader.startLoad(HIKARI_DAMAGED_HELMET_PATH);
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

    std::cout << "=== All AsyncSceneLoader Tests Passed ===\n";
    return 0;
}
