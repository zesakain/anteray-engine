/// @file gameplay_input.h
/// @brief Hardware-neutral Gameplay Input API with action mapping, contexts, and analog shaping.
///
/// Purpose:
/// Translates raw per-frame platform::InputSnapshot states into named gameplay digital actions
/// and shaped analog axes without exposing SDL3 or OS-specific types to gameplay code.
///
/// Ownership and Lifetime:
/// InputSystem owns its registered contexts, bindings, and per-frame action state tables.
/// update() reads a const platform::InputSnapshot& valid for the duration of the call.
///
/// Thread-safety:
/// Main simulation thread for registration and update(). Read queries are safe after update().
///
/// Cost:
/// Registration/binding/JSON loading may allocate during setup. Per-frame update() and all
/// action/axis queries perform zero heap allocations, zero locks, and zero blocking I/O.

#pragma once

#include "core/error.h"
#include "core/math.h"
#include "core/result.h"
#include "platform/input_snapshot.h"
#include "platform/input_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eng::framework {

/// @enum InputSourceKind
/// @brief Physical input source category bound to a gameplay action or axis.
enum class InputSourceKind : uint8_t {
    Key = 0,
    MouseButton = 1,
    MouseDeltaX = 2,
    MouseDeltaY = 3,
    MouseWheelX = 4,
    MouseWheelY = 5,
    GamepadButton = 6,
    GamepadAxis = 7,
};

/// @struct InputBinding
/// @brief Single physical input binding with optional chord gate, dead zone, and scaling.
struct InputBinding {
    InputSourceKind kind{InputSourceKind::Key};
    uint16_t code{0};
    uint8_t gamepadIndex{0};
    float scale{1.0F};
    float deadZone{0.15F};
    /// Optional mouse button chord gate (MouseButton::Count = no gate required).
    platform::MouseButton gateMouseButton{platform::MouseButton::Count};

    [[nodiscard]] static constexpr InputBinding fromKey(platform::KeyCode key,
                                                        float scale = 1.0F) noexcept {
        return InputBinding{
            .kind = InputSourceKind::Key,
            .code = static_cast<uint16_t>(key),
            .scale = scale,
        };
    }

    [[nodiscard]] static constexpr InputBinding fromMouseButton(platform::MouseButton button,
                                                                float scale = 1.0F) noexcept {
        return InputBinding{
            .kind = InputSourceKind::MouseButton,
            .code = static_cast<uint16_t>(button),
            .scale = scale,
        };
    }

    [[nodiscard]] static constexpr InputBinding
    fromMouseDeltaX(float scale = 1.0F,
                    platform::MouseButton gateButton = platform::MouseButton::Count) noexcept {
        return InputBinding{
            .kind = InputSourceKind::MouseDeltaX,
            .scale = scale,
            .deadZone = 0.0F,
            .gateMouseButton = gateButton,
        };
    }

    [[nodiscard]] static constexpr InputBinding
    fromMouseDeltaY(float scale = 1.0F,
                    platform::MouseButton gateButton = platform::MouseButton::Count) noexcept {
        return InputBinding{
            .kind = InputSourceKind::MouseDeltaY,
            .scale = scale,
            .deadZone = 0.0F,
            .gateMouseButton = gateButton,
        };
    }

    [[nodiscard]] static constexpr InputBinding fromGamepadButton(platform::GamepadButton button,
                                                                  uint8_t padIndex = 0,
                                                                  float scale = 1.0F) noexcept {
        return InputBinding{
            .kind = InputSourceKind::GamepadButton,
            .code = static_cast<uint16_t>(button),
            .gamepadIndex = padIndex,
            .scale = scale,
        };
    }

    [[nodiscard]] static constexpr InputBinding fromGamepadAxis(platform::GamepadAxis axis,
                                                                uint8_t padIndex = 0,
                                                                float deadZone = 0.15F,
                                                                float scale = 1.0F) noexcept {
        return InputBinding{
            .kind = InputSourceKind::GamepadAxis,
            .code = static_cast<uint16_t>(axis),
            .gamepadIndex = padIndex,
            .scale = scale,
            .deadZone = deadZone,
        };
    }
};

/// @struct ActionState
/// @brief Frame-evaluated state for a single gameplay action or analog axis.
struct ActionState {
    float value{0.0F};
    bool pressed{false};
    bool held{false};
    bool released{false};
};

/// @brief Computes deterministic 64-bit FNV-1a hash for an action or context name.
[[nodiscard]] constexpr uint64_t hashInputName(std::string_view name) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : name) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// @class GameplayInputSystem
/// @brief Context-aware, rebindable gameplay input evaluator over platform::InputSnapshot.
class GameplayInputSystem {
public:
    static constexpr size_t kMaxContexts = 16;
    static constexpr size_t kMaxActions = 128;
    static constexpr size_t kMaxBindingsPerAction = 8;

    GameplayInputSystem() = default;

    /// Registers a named mapping context with priority (higher priority evaluated first).
    [[nodiscard]] Result<void> registerContext(std::string_view contextName, int32_t priority = 0);

    /// Enables or disables an existing mapping context without losing bindings.
    [[nodiscard]] Result<void> setContextEnabled(std::string_view contextName, bool enabled);

    /// Returns whether the named context exists and is currently enabled.
    [[nodiscard]] bool isContextEnabled(std::string_view contextName) const noexcept;

    /// Appends a binding to an action within the specified context.
    [[nodiscard]] Result<void> bindAction(std::string_view contextName, std::string_view actionName,
                                          const InputBinding& binding);

    /// Replaces all bindings for an action within the specified context with a single binding.
    [[nodiscard]] Result<void> rebindAction(std::string_view contextName,
                                            std::string_view actionName,
                                            const InputBinding& binding);

    /// Removes all bindings for an action within the specified context.
    [[nodiscard]] Result<void> unbindAction(std::string_view contextName,
                                            std::string_view actionName);

    /// Removes all bindings from the specified context.
    [[nodiscard]] Result<void> clearContext(std::string_view contextName);

    /// Loads context and action mappings from a JSON string.
    [[nodiscard]] Result<void> loadMappingsFromJsonString(std::string_view jsonText);

    /// Loads context and action mappings from a JSON file on disk.
    [[nodiscard]] Result<void> loadMappingsFromFile(const std::filesystem::path& filepath);

    /// Evaluates all enabled contexts against the current frame's InputSnapshot.
    /// Zero heap allocations, zero locks, zero blocking I/O.
    void update(const platform::InputSnapshot& snapshot) noexcept;

    /// Resets all current and previous action states to inactive without removing bindings.
    void resetStates() noexcept;

    [[nodiscard]] ActionState getActionState(std::string_view actionName) const noexcept;
    [[nodiscard]] bool isActionPressed(std::string_view actionName) const noexcept;
    [[nodiscard]] bool isActionHeld(std::string_view actionName) const noexcept;
    [[nodiscard]] bool isActionReleased(std::string_view actionName) const noexcept;
    [[nodiscard]] float getActionValue(std::string_view actionName) const noexcept;

    /// Computes a signed 1D axis in [-1, 1] (or scaled) from negative and positive actions.
    [[nodiscard]] float getAxis(std::string_view negativeAction,
                                std::string_view positiveAction) const noexcept;

    /// Computes a 2D axis from four directional actions (x = right - left, y = up - down).
    [[nodiscard]] Vec2 getAxis2D(std::string_view leftAction, std::string_view rightAction,
                                 std::string_view downAction,
                                 std::string_view upAction) const noexcept;

    /// Registers standard third-person movement and camera bindings in "Gameplay" context.
    void installDefaultThirdPersonBindings();

private:
    struct ContextEntry {
        uint64_t id{0};
        int32_t priority{0};
        bool used{false};
        bool enabled{true};
    };

    struct BindingSlot {
        uint64_t contextId{0};
        InputBinding binding{};
        bool used{false};
    };

    struct ActionEntry {
        uint64_t id{0};
        bool used{false};
        std::array<BindingSlot, kMaxBindingsPerAction> bindings{};
        size_t bindingCount{0};
        ActionState current{};
        bool prevHeld{false};
    };

    [[nodiscard]] int findContextIndex(uint64_t contextId) const noexcept;
    [[nodiscard]] int findActionIndex(uint64_t actionId) const noexcept;
    [[nodiscard]] int findOrCreateActionIndex(uint64_t actionId) noexcept;

    std::array<ContextEntry, kMaxContexts> contexts_{};
    size_t contextCount_{0};
    std::array<ActionEntry, kMaxActions> actions_{};
    size_t actionCount_{0};
};

} // namespace eng::framework
