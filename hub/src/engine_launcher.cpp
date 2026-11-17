/// @file engine_launcher.cpp
/// @brief Engine/Editor discovery and detached process launch implementation.

#include "hub/engine_launcher.h"

#include <array>
#include <cstdlib>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace eng::hub {

namespace {

bool isExecutableFile(const std::filesystem::path& p) {
    if (p.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
        return false;
    }
#if defined(_WIN32)
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

std::string inferBuildFlavor(const std::filesystem::path& exePath) {
    const std::string s = exePath.generic_string();
    if (s.find("/release-editor/") != std::string::npos ||
        s.find("/editor-release/") != std::string::npos) {
        return "release-editor";
    }
    if (s.find("/editor/") != std::string::npos) {
        return "editor";
    }
    if (s.find("/release/") != std::string::npos) {
        return "release";
    }
    if (s.find("/debug/") != std::string::npos) {
        return "debug";
    }
    if (s.find("/profile/") != std::string::npos) {
        return "profile";
    }
    return "local";
}

} // namespace

std::filesystem::path EngineLauncher::findRepositoryRoot(const std::filesystem::path& startDir) {
    std::error_code ec;
    std::filesystem::path cur = startDir.empty() ? std::filesystem::current_path(ec) : startDir;
    if (std::filesystem::is_regular_file(cur, ec)) {
        cur = cur.parent_path();
    }
    cur = std::filesystem::weakly_canonical(cur, ec);
    if (ec) {
        return {};
    }

    for (int depth = 0; depth < 8 && !cur.empty(); ++depth) {
        if (std::filesystem::exists(cur / "CMakeLists.txt", ec) &&
            std::filesystem::is_directory(cur / "engine", ec) &&
            std::filesystem::is_directory(cur / "assets", ec)) {
            return cur;
        }
        const auto parent = cur.parent_path();
        if (parent == cur) {
            break;
        }
        cur = parent;
    }
    return {};
}

EngineInstallation EngineLauncher::discover(const std::filesystem::path& hubExecutablePath,
                                            const std::filesystem::path& overrideExecutable,
                                            const std::filesystem::path& searchStartDir) {
    EngineInstallation info{};
    std::error_code ec;

    const std::filesystem::path cwd =
        searchStartDir.empty() ? std::filesystem::current_path(ec) : searchStartDir;

    info.repositoryRoot = findRepositoryRoot(cwd);
    if (info.repositoryRoot.empty() && !hubExecutablePath.empty()) {
        info.repositoryRoot = findRepositoryRoot(hubExecutablePath);
    }

#if defined(_WIN32)
    constexpr const char* kExeName = "anteray.exe";
#else
    constexpr const char* kExeName = "anteray";
#endif

    std::vector<std::filesystem::path> candidates;
    if (!overrideExecutable.empty()) {
        candidates.push_back(overrideExecutable);
    }
    // Prefer official Editor-enabled builds (editor-release, build/editor/bin) before
    // legacy release-editor or non-editor runtime builds (profile, release, debug).
    if (!info.repositoryRoot.empty()) {
        static constexpr std::array<const char*, 3> kEditorBins{
            "build/editor-release/bin",
            "build/editor/bin",
            "build/release-editor/bin",
        };
        for (const char* relBin : kEditorBins) {
            candidates.push_back(info.repositoryRoot / relBin / kExeName);
        }
    }
    if (!hubExecutablePath.empty()) {
        candidates.push_back(hubExecutablePath.parent_path() / kExeName);
    }
    if (!info.repositoryRoot.empty()) {
        static constexpr std::array<const char*, 3> kFallbackBins{
            "build/profile/bin",
            "build/release/bin",
            "build/debug/bin",
        };
        for (const char* relBin : kFallbackBins) {
            candidates.push_back(info.repositoryRoot / relBin / kExeName);
        }
    }

    if (const char* pathEnv = std::getenv("PATH"); pathEnv != nullptr) {
        std::stringstream ss(pathEnv);
        std::string dir;
#if defined(_WIN32)
        constexpr char kSep = ';';
#else
        constexpr char kSep = ':';
#endif
        while (std::getline(ss, dir, kSep)) {
            if (!dir.empty()) {
                candidates.push_back(std::filesystem::path(dir) / kExeName);
            }
        }
    }

    for (const auto& candidate : candidates) {
        if (isExecutableFile(candidate)) {
            std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
            info.executablePath = ec ? candidate : canonical;
            info.buildFlavor = inferBuildFlavor(info.executablePath);
            info.available = true;
            break;
        }
    }

    return info;
}

std::vector<std::string>
EngineLauncher::buildProjectLaunchArgs(const std::filesystem::path& projectDir,
                                       const ProjectMetadata& metadata) {
    std::vector<std::string> args;
    if (!metadata.startupScene.empty() && isSafeProjectRelativePath(metadata.startupScene)) {
        std::error_code ec;
        std::filesystem::path sceneAbs =
            std::filesystem::weakly_canonical(projectDir / metadata.startupScene, ec);
        if (ec) {
            sceneAbs = (projectDir / metadata.startupScene).lexically_normal();
        }
        args.emplace_back("--scene");
        args.push_back(sceneAbs.string());
    }
    return args;
}

std::vector<std::string>
EngineLauncher::buildSceneLaunchArgs(const std::filesystem::path& scenePath) {
    std::vector<std::string> args;
    if (!scenePath.empty()) {
        std::error_code ec;
        std::filesystem::path absScene = std::filesystem::weakly_canonical(scenePath, ec);
        if (ec) {
            absScene = scenePath.lexically_normal();
        }
        args.emplace_back("--scene");
        args.push_back(absScene.string());
    }
    return args;
}

Result<uint64_t> EngineLauncher::launchDetached(const std::filesystem::path& executablePath,
                                                const std::vector<std::string>& args,
                                                const std::filesystem::path& workingDir) {
    if (!isExecutableFile(executablePath)) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "ANTERAY executable not found or not executable"};
    }

    std::error_code ec;
    if (!workingDir.empty() && (!std::filesystem::exists(workingDir, ec) ||
                                !std::filesystem::is_directory(workingDir, ec))) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Launch working directory does not exist"};
    }

#if defined(_WIN32)
    std::string cmdLine = "\"" + executablePath.string() + "\"";
    for (const auto& a : args) {
        cmdLine += " \"" + a + "\"";
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const char* cwdPtr = workingDir.empty() ? nullptr : workingDir.string().c_str();
    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, cwdPtr, &si, &pi)) {
        return Error{.code = ErrorCode::InvalidState, .message = "CreateProcessA failed"};
    }
    const uint64_t pid = static_cast<uint64_t>(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
#else
    const pid_t pid = ::fork();
    if (pid < 0) {
        return Error{.code = ErrorCode::InvalidState, .message = "Failed to fork child process"};
    }
    if (pid == 0) {
        (void)::setsid();
        if (!workingDir.empty()) {
            if (::chdir(workingDir.c_str()) != 0) {
                _exit(127);
            }
        }

        std::vector<std::string> storage;
        storage.reserve(1U + args.size());
        storage.push_back(executablePath.string());
        for (const auto& a : args) {
            storage.push_back(a);
        }

        std::vector<char*> argvPtrs;
        argvPtrs.reserve(storage.size() + 1U);
        for (auto& s : storage) {
            argvPtrs.push_back(s.data());
        }
        argvPtrs.push_back(nullptr);

        ::execv(executablePath.c_str(), argvPtrs.data());
        _exit(127);
    }

    return static_cast<uint64_t>(pid);
#endif
}

} // namespace eng::hub
