/// @file script_types.h
/// @brief Fundamental types, errors, and value variants for the Lua scripting subsystem.
///
/// Purpose:
/// Defines hardware-neutral value types, error codes, and conversion variants for safe
/// communication between C++ engine subsystems and sandboxed Lua script environments.
///
/// Ownership and Lifetime:
/// Value types with standard layout or small standard container variants. Trivially copyable
/// or moveable without hidden dynamic allocation in the hot path.
///
/// Thread-safety:
/// Value types are thread-safe for concurrent read access. Script evaluation and state mutation
/// must be pinned to the simulation thread or protected by mode boundaries.
///
/// Performance:
/// Fits small-string optimization and variant storage. Zero heap allocation for numeric,
/// boolean, entity handle, and vector values.

#pragma once

#include "core/math.h"
#include "world/entity.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace eng::script {

/// @enum ScriptError
/// @brief Error codes returned by script loading, compilation, execution, and call operations.
enum class ScriptError : uint8_t {
    /// Operation succeeded without error.
    None = 0,

    /// Syntax error encountered while parsing script source chunk.
    SyntaxError,

    /// Runtime error during script execution.
    RuntimeError,

    /// Script exceeded the allocated CPU instruction budget (infinite loop / watchdog trigger).
    BudgetExceeded,

    /// Memory allocation budget exceeded by script runtime.
    OutOfMemory,

    /// Requested function, table, or global variable was not found.
    NotFound,

    /// Value type mismatch between C++ caller and Lua expectation.
    TypeError,

    /// Script engine was in an uninitialized or corrupted state.
    InvalidState
};

/// Human-readable diagnostic description of a ScriptError code.
[[nodiscard]] constexpr std::string_view getScriptErrorMessage(ScriptError error) noexcept {
    switch (error) {
    case ScriptError::None:
        return "None";
    case ScriptError::SyntaxError:
        return "Lua syntax error during chunk compilation";
    case ScriptError::RuntimeError:
        return "Lua runtime error during execution";
    case ScriptError::BudgetExceeded:
        return "Script instruction watchdog budget exceeded";
    case ScriptError::OutOfMemory:
        return "Script engine memory budget exceeded";
    case ScriptError::NotFound:
        return "Requested script symbol or function not found";
    case ScriptError::TypeError:
        return "Type mismatch during script parameter or return value conversion";
    case ScriptError::InvalidState:
        return "Script engine is uninitialized or invalid";
    }
    return "Unknown script error";
}

/// @struct Nil
/// @brief Tag type representing Lua nil / null.
struct Nil {
    constexpr bool operator==(const Nil&) const noexcept = default;
};

/// @typedef ScriptValue
/// @brief Safe C++20 variant representing any exchangeable value between C++ and Lua.
///
/// Supports nil, bool, 64-bit float, string, EntityHandle, and Vec3 math vectors.
using ScriptValue = std::variant<Nil, bool, double, std::string, world::EntityHandle, Vec3>;

/// @struct ScriptConfig
/// @brief Configuration settings for initializing the ScriptEngine.
struct ScriptConfig {
    /// Maximum number of Lua VM bytecode instructions allowed before watchdog triggers.
    /// Default: 100,000 instructions per execution chunk.
    uint32_t instructionLimit{100'000};

    /// Total memory budget in bytes for the Lua state allocator (0 = unlimited general heap).
    size_t memoryBudgetBytes{16 * 1024 * 1024}; // 16 MiB default

    /// Garbage collector step multiplier (higher values perform more aggressive GC).
    int32_t gcStepMultiplier{200};
};

/// @enum ScriptExecutionMode
/// @brief Governs whether Lua scripts have active-mode write access or standby read-only access.
enum class ScriptExecutionMode : uint8_t {
    Active = 0,
    Standby = 1,
};

/// @struct ScriptEvent
/// @brief Lightweight event emitted from or delivered to Lua gameplay scripts.
struct ScriptEvent {
    std::string name;
    std::string payload;
};

/// @class ScriptCallContext
/// @brief Opaque argument/return-value bridge for C++ host functions exposed to Lua.
///
/// Header self-containment: wraps an opaque Lua state pointer without including <lua.h>.
class ScriptCallContext {
public:
    explicit ScriptCallContext(void* opaqueState) noexcept : state_(opaqueState) {}

    [[nodiscard]] int32_t getArgCount() const noexcept;
    [[nodiscard]] bool getBool(int32_t argIndex, bool defaultVal = false) const noexcept;
    [[nodiscard]] double getNumber(int32_t argIndex, double defaultVal = 0.0) const noexcept;
    [[nodiscard]] std::string_view getString(int32_t argIndex) const noexcept;
    [[nodiscard]] world::EntityHandle getEntity(int32_t argIndex) const noexcept;
    [[nodiscard]] Vec3 getVec3(int32_t argIndex) const noexcept;

    void pushNil() noexcept;
    void pushBool(bool v) noexcept;
    void pushNumber(double v) noexcept;
    void pushString(std::string_view s) noexcept;
    void pushEntity(world::EntityHandle e) noexcept;
    void pushVec3(const Vec3& v) noexcept;

    void setError(std::string_view message) noexcept;
    [[nodiscard]] std::string_view errorMessage() const noexcept { return errorMessage_; }
    [[nodiscard]] bool hasError() const noexcept { return hasError_; }

private:
    void* state_{nullptr};
    std::string errorMessage_{};
    bool hasError_{false};
};

/// Host callback signature: returns number of Lua return values pushed, or < 0 on error.
using ScriptHostCallback = int32_t (*)(ScriptCallContext& ctx, void* userData);

/// @struct ScriptHostFunction
/// @brief Descriptor for registering a C++ host function into a Lua module table.
struct ScriptHostFunction {
    std::string_view name;
    ScriptHostCallback callback{nullptr};
    void* userData{nullptr};
    bool mutatesWorld{false};
};

} // namespace eng::script
