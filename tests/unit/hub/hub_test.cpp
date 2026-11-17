/// @file hub_test.cpp
/// @brief Unit and filesystem integration tests for Hikari Machine Hub core subsystems.

#include "core/assert.h"
#include "core/json.h"
#include "hub/engine_launcher.h"
#include "hub/project_metadata.h"
#include "hub/project_validator.h"
#include "hub/recent_projects.h"
#include "hub/sample_catalog.h"
#include "hub/template_registry.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

using namespace eng;
using namespace eng::hub;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << #cond << "\n";      \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

namespace {

class TempDirGuard {
public:
    explicit TempDirGuard(std::string_view prefix) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                (std::string(prefix) + "_" + std::to_string(nonce));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        TEST_CHECK(!ec);
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_{};
};

void writeRawFile(const std::filesystem::path& p, std::string_view text) {
    std::ofstream out(p, std::ios::out | std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
    out << text;
    out.flush();
    TEST_CHECK(out.good());
}

void testMetadataSerializationAndValidation() {
    ProjectMetadata meta{};
    meta.formatVersion = kCurrentProjectFormatVersion;
    meta.projectName = "AlphaProject";
    meta.engineVersion = "0.1.0";
    meta.startupScene = "Scenes/Main.hikari";
    meta.assetDirectories = {"Assets", "Scenes", "Scripts", "Config"};
    meta.templateOrigin = "basic_3d";
    meta.createdTimestamp = 1731000000;
    meta.lastModifiedTimestamp = 1731000500;

    auto serRes = serializeProjectMetadata(meta);
    TEST_CHECK(serRes.hasValue());

    auto parseRes = parseProjectMetadata(serRes.value());
    TEST_CHECK(parseRes.hasValue());
    const auto& parsed = parseRes.value();
    TEST_CHECK(parsed.formatVersion == kCurrentProjectFormatVersion);
    TEST_CHECK(parsed.projectName == "AlphaProject");
    TEST_CHECK(parsed.engineVersion == "0.1.0");
    TEST_CHECK(parsed.startupScene == "Scenes/Main.hikari");
    TEST_CHECK(parsed.templateOrigin == "basic_3d");
    TEST_CHECK(parsed.assetDirectories.size() == 4U);
    TEST_CHECK(parsed.createdTimestamp == 1731000000);
    TEST_CHECK(parsed.lastModifiedTimestamp == 1731000500);

    // Invalid JSON
    TEST_CHECK(!parseProjectMetadata("{not valid json").hasValue());
    TEST_CHECK(!parseProjectMetadata("[]").hasValue());

    // Unsupported formatVersion
    const auto futureVersion =
        parseProjectMetadata(R"({"formatVersion": 99, "projectName": "FutureProj"})");
    TEST_CHECK(!futureVersion.hasValue());
    TEST_CHECK(futureVersion.error().code == ErrorCode::NotSupported);

    const auto zeroVersion =
        parseProjectMetadata(R"({"formatVersion": 0, "projectName": "ZeroProj"})");
    TEST_CHECK(!zeroVersion.hasValue());

    // Path traversal rejection
    const auto unsafeScene = parseProjectMetadata(
        R"({"formatVersion": 1, "projectName": "Evil", "startupScene": "../escape.hikari"})");
    TEST_CHECK(!unsafeScene.hasValue());

    const auto absAsset = parseProjectMetadata(
        R"({"formatVersion": 1, "projectName": "Evil", "assetDirectories": ["/tmp/abs"]})");
    TEST_CHECK(!absAsset.hasValue());

    // Project name validation
    TEST_CHECK(!validateProjectName("").hasValue());
    TEST_CHECK(!validateProjectName("   ").hasValue());
    TEST_CHECK(!validateProjectName("..").hasValue());
    TEST_CHECK(!validateProjectName("bad/name").hasValue());
    TEST_CHECK(!validateProjectName(" leading").hasValue());
    TEST_CHECK(!validateProjectName("trailing.").hasValue());
    TEST_CHECK(validateProjectName("Valid_Project-01").hasValue());

    std::cout << "[PASS] testMetadataSerializationAndValidation\n";
}

void testTemplateInstantiationProtectionAndRollback() {
    TempDirGuard tmp("hikari_hub_templates");

    // Verify all 7 built-in templates instantiate validly and produce parseable scenes
    const auto& builtins = TemplateRegistry::builtinTemplates();
    TEST_CHECK(builtins.size() == 7U);
    TEST_CHECK(TemplateRegistry::findById("third_person_character") != nullptr);

    for (const auto& tmpl : builtins) {
        TEST_CHECK(validateTemplateDesc(tmpl).hasValue());

        const std::string projName = "Proj_" + tmpl.id;
        ProjectCreateRequest req{
            .projectName = projName,
            .parentDirectory = tmp.path(),
            .templateId = tmpl.id,
        };
        auto createRes = TemplateRegistry::createProjectFromBuiltin(req);
        TEST_CHECK(createRes.hasValue());

        const auto report = validateProjectDirectory(createRes.value().projectDirectory);
        TEST_CHECK(report.status == ProjectValidationStatus::Valid);

        if (tmpl.id == "third_person_character") {
            const auto startupScene = createRes.value().projectDirectory / tmpl.defaultStartupScene;
            std::ifstream sceneIn(startupScene);
            TEST_CHECK(sceneIn.is_open());
            const std::string content((std::istreambuf_iterator<char>(sceneIn)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(content.find("PlayerCharacter") != std::string::npos);
            TEST_CHECK(content.find("Superhero_Male_FullBody.gltf") != std::string::npos);
        }
        TEST_CHECK(report.metadata.has_value());
        TEST_CHECK(report.metadata->templateOrigin == tmpl.id);

        if (!tmpl.defaultStartupScene.empty()) {
            const std::filesystem::path scenePath =
                createRes.value().projectDirectory / tmpl.defaultStartupScene;
            TEST_CHECK(std::filesystem::exists(scenePath));
            std::ifstream in(scenePath);
            std::string jsonText((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            auto sceneJson = core::JsonValue::parse(jsonText);
            TEST_CHECK(sceneJson.hasValue());
            TEST_CHECK(sceneJson.value().getInt("formatVersion", 0) == 1);
        }
    }

    // Existing-directory overwrite protection
    ProjectCreateRequest duplicateReq{
        .projectName = "Proj_empty",
        .parentDirectory = tmp.path(),
        .templateId = "empty",
    };
    const auto dupRes = TemplateRegistry::createProjectFromBuiltin(duplicateReq);
    TEST_CHECK(!dupRes.hasValue());
    TEST_CHECK(std::filesystem::exists(tmp.path() / "Proj_empty" / "Proj_empty.hikari-project"));

    // Invalid template definition rejected cleanly without creating directory
    ProjectTemplateDesc badTmpl{
        .id = "bad",
        .name = "Bad Template",
        .defaultStartupScene = "../../outside.hikari",
    };
    TEST_CHECK(!validateTemplateDesc(badTmpl).hasValue());
    ProjectCreateRequest badReq{
        .projectName = "ShouldNotExist",
        .parentDirectory = tmp.path(),
        .templateId = "bad",
    };
    TEST_CHECK(!TemplateRegistry::createProject(badReq, badTmpl).hasValue());
    TEST_CHECK(!std::filesystem::exists(tmp.path() / "ShouldNotExist"));

    // Unknown builtin template rejected cleanly
    ProjectCreateRequest unknownReq{
        .projectName = "UnknownTmplProj",
        .parentDirectory = tmp.path(),
        .templateId = "nonexistent_template",
    };
    TEST_CHECK(!TemplateRegistry::createProjectFromBuiltin(unknownReq).hasValue());
    TEST_CHECK(!std::filesystem::exists(tmp.path() / "UnknownTmplProj"));

    // Rollback on failure during creation: make parent read-only? Instead, test invalid
    // template with invalid project name inside template creation or non-existent parent
    ProjectCreateRequest missingParentReq{
        .projectName = "OrphanProject",
        .parentDirectory = tmp.path() / "does_not_exist",
        .templateId = "empty",
    };
    TEST_CHECK(!TemplateRegistry::createProjectFromBuiltin(missingParentReq).hasValue());

    // Rollback when a mid-creation write fails: pre-create a file blocking a directory? No,
    // existing target dir is already blocked; test rollback using a read-only parent directory
    // on POSIX or verify invalid manifest triggers cleanup if projectName conflicts.
#if !defined(_WIN32)
    const std::filesystem::path roParent = tmp.path() / "ro_parent";
    std::filesystem::create_directories(roParent);
    std::filesystem::permissions(roParent, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_exec);
    ProjectCreateRequest roReq{
        .projectName = "BlockedProj",
        .parentDirectory = roParent,
        .templateId = "basic_3d",
    };
    const auto roRes = TemplateRegistry::createProjectFromBuiltin(roReq);
    TEST_CHECK(!roRes.hasValue());
    TEST_CHECK(!std::filesystem::exists(roParent / "BlockedProj"));
    std::filesystem::permissions(roParent, std::filesystem::perms::owner_all);
#endif

    std::cout << "[PASS] testTemplateInstantiationProtectionAndRollback\n";
}

void testProjectValidationStates() {
    TempDirGuard tmp("hikari_hub_validation");

    // 1. Missing directory -> Invalid
    {
        const auto rep = validateProjectDirectory(tmp.path() / "MissingDir");
        TEST_CHECK(rep.status == ProjectValidationStatus::Invalid);
        TEST_CHECK(!rep.issues.empty());
    }

    // 2. Valid project, then remove optional directory -> ValidWithWarnings
    {
        ProjectCreateRequest req{
            .projectName = "WarnProj",
            .parentDirectory = tmp.path(),
            .templateId = "basic_3d",
        };
        auto res = TemplateRegistry::createProjectFromBuiltin(req);
        TEST_CHECK(res.hasValue());

        // Remove Scripts from disk and update manifest assetDirectories to not require Scripts,
        // or test engineVersion mismatch warning.
        ProjectMetadata modMeta = res.value().metadata;
        modMeta.engineVersion = "0.0.9";
        TEST_CHECK(saveProjectMetadataFile(res.value().manifestPath, modMeta).hasValue());

        const auto rep = validateProjectDirectory(res.value().projectDirectory, "0.1.0");
        TEST_CHECK(rep.status == ProjectValidationStatus::ValidWithWarnings);
        TEST_CHECK(rep.hasWarnings());
    }

    // 3. Missing startupScene -> Invalid
    {
        ProjectCreateRequest req{
            .projectName = "BrokenSceneProj",
            .parentDirectory = tmp.path(),
            .templateId = "basic_3d",
        };
        auto res = TemplateRegistry::createProjectFromBuiltin(req);
        TEST_CHECK(res.hasValue());
        std::filesystem::remove(res.value().projectDirectory / "Scenes" / "Main.hikari");

        const auto rep = validateProjectDirectory(res.value().projectDirectory);
        TEST_CHECK(rep.status == ProjectValidationStatus::Invalid);
    }

    // 4. Malformed manifest is never silently rewritten
    {
        const std::filesystem::path badDir = tmp.path() / "CorruptProj";
        std::filesystem::create_directories(badDir);
        const std::string rawCorrupt = "{corrupt_json";
        const std::filesystem::path manifest = badDir / "CorruptProj.hikari-project";
        writeRawFile(manifest, rawCorrupt);

        const auto rep = validateProjectDirectory(badDir);
        TEST_CHECK(rep.status == ProjectValidationStatus::Invalid);

        std::ifstream in(manifest);
        std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TEST_CHECK(after == rawCorrupt);
    }

    std::cout << "[PASS] testProjectValidationStates\n";
}

void testRecentProjectsPersistenceAndDeduplication() {
    TempDirGuard tmp("hikari_hub_recents");
    const std::filesystem::path storePath = tmp.path() / "hub_recent_projects.json";
    const std::filesystem::path projA = tmp.path() / "ProjectA";
    const std::filesystem::path projB = tmp.path() / "ProjectB";
    std::filesystem::create_directories(projA);
    std::filesystem::create_directories(projB);

    RecentProjectsStore store;
    store.upsert(RecentProjectEntry{
        .projectName = "ProjectA",
        .projectDirectory = projA,
        .lastOpenedTimestamp = 100,
        .engineVersion = "0.1.0",
    });
    store.upsert(RecentProjectEntry{
        .projectName = "ProjectB",
        .projectDirectory = projB,
        .lastOpenedTimestamp = 200,
        .engineVersion = "0.1.0",
    });
    // Duplicate upsert with un-normalized path should deduplicate and move ProjectA to front
    store.upsert(RecentProjectEntry{
        .projectName = "ProjectA",
        .projectDirectory = projA / ".",
        .lastOpenedTimestamp = 300,
        .engineVersion = "0.1.0",
    });
    TEST_CHECK(store.entries().size() == 2U);
    TEST_CHECK(store.entries()[0].projectName == "ProjectA");
    TEST_CHECK(store.entries()[0].lastOpenedTimestamp == 300);

    TEST_CHECK(store.saveAtomic(storePath).hasValue());

    // Delete ProjectB directory from disk; reload should preserve entry with existsOnDisk == false
    std::filesystem::remove_all(projB);
    RecentProjectsStore reloaded;
    TEST_CHECK(reloaded.load(storePath).hasValue());
    TEST_CHECK(reloaded.entries().size() == 2U);
    TEST_CHECK(reloaded.entries()[0].existsOnDisk);
    TEST_CHECK(!reloaded.entries()[1].existsOnDisk);

    // Remove from recents must not delete ProjectA files on disk
    const std::filesystem::path marker = projA / "keep_me.txt";
    writeRawFile(marker, "hello");
    TEST_CHECK(reloaded.remove(projA));
    TEST_CHECK(reloaded.entries().size() == 1U);
    TEST_CHECK(std::filesystem::exists(marker));

    std::cout << "[PASS] testRecentProjectsPersistenceAndDeduplication\n";
}

void testEngineDiscoveryLaunchArgsAndSamples() {
    TempDirGuard tmp("hikari_hub_engine");

    // Create a fake executable inside tmp to test discovery without machine-specific paths
    const std::filesystem::path fakeExe = tmp.path() / "hikari";
    writeRawFile(fakeExe, "#!/bin/sh\nexit 0\n");
#if !defined(_WIN32)
    std::filesystem::permissions(fakeExe, std::filesystem::perms::owner_all);
#endif

    const auto discovered = EngineLauncher::discover({}, fakeExe, tmp.path());
    TEST_CHECK(discovered.available);
    TEST_CHECK(!discovered.executablePath.empty());

    // Launch args with startupScene
    ProjectMetadata withScene{};
    withScene.projectName = "SceneProj";
    withScene.startupScene = "Scenes/Main.hikari";
    const auto argsWithScene = EngineLauncher::buildProjectLaunchArgs(tmp.path(), withScene);
    TEST_CHECK(argsWithScene.size() == 2U);
    TEST_CHECK(argsWithScene[0] == "--scene");
    TEST_CHECK(argsWithScene[1].find("Main.hikari") != std::string::npos);

    // Launch args without startupScene (Empty project)
    ProjectMetadata emptyMeta{};
    emptyMeta.projectName = "EmptyProj";
    emptyMeta.startupScene = "";
    const auto argsEmpty = EngineLauncher::buildProjectLaunchArgs(tmp.path(), emptyMeta);
    TEST_CHECK(argsEmpty.empty());

    // Launch failure on missing executable
    const auto failLaunch =
        EngineLauncher::launchDetached(tmp.path() / "missing_hikari", {}, tmp.path());
    TEST_CHECK(!failLaunch.hasValue());

    // Sample catalog discovery against repository root (if running from repo)
    const auto repoRoot = EngineLauncher::findRepositoryRoot(std::filesystem::current_path());
    if (!repoRoot.empty()) {
        const auto samples = SampleCatalog::discoverSamples(repoRoot);
        TEST_CHECK(samples.size() == 5U);
        TEST_CHECK(samples[0].name == "CharacterController TPP Test");
        TEST_CHECK(samples[0].canOpenInEditor);
        TEST_CHECK(samples[1].name == "Sponza");
        TEST_CHECK(samples[2].name == "DamagedHelmet");
        TEST_CHECK(samples[3].name == "Bistro");
        TEST_CHECK(samples[4].name == "MaterialLab");

        // Regression check: when build/editor/bin/hikari exists, Hub must prefer it over
        // non-editor build/release/bin/hikari.
        const auto editorExe = repoRoot / "build/editor/bin/hikari";
        if (std::filesystem::exists(editorExe)) {
            const auto editorDiscovered = EngineLauncher::discover({}, {}, repoRoot);
            TEST_CHECK(editorDiscovered.available);
            TEST_CHECK(editorDiscovered.buildFlavor == "editor" ||
                       editorDiscovered.buildFlavor == "release-editor");
        }
    }

    std::cout << "[PASS] testEngineDiscoveryLaunchArgsAndSamples\n";
}

} // namespace

int main() {
    std::cout << "Running Hikari Machine Hub core unit tests...\n";
    testMetadataSerializationAndValidation();
    testTemplateInstantiationProtectionAndRollback();
    testProjectValidationStates();
    testRecentProjectsPersistenceAndDeduplication();
    testEngineDiscoveryLaunchArgsAndSamples();
    std::cout << "All Hikari Machine Hub core tests PASSED!\n";
    return 0;
}
