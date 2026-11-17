/// @file gameplay_input.cpp
/// @brief Hardware-neutral Gameplay Input API implementation.

#include "framework/gameplay_input.h"

#include "core/json.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>

namespace eng::framework {
namespace {

constexpr float kDigitalThreshold = 0.25F;

[[nodiscard]] Result<void> validateBinding(const InputBinding& binding) noexcept {
    if (!std::isfinite(binding.scale) || !std::isfinite(binding.deadZone)) {
        return Error{ErrorCode::InvalidArgument, "InputBinding scale and deadZone must be finite"};
    }
    if (binding.deadZone < 0.0F || binding.deadZone >= 0.99F) {
        return Error{ErrorCode::InvalidArgument,
                     "InputBinding deadZone must be in range [0.0, 0.99)"};
    }

    switch (binding.kind) {
    case InputSourceKind::Key:
        if (binding.code >= static_cast<uint16_t>(platform::KeyCode::Count)) {
            return Error{ErrorCode::InvalidArgument, "KeyCode out of range"};
        }
        break;
    case InputSourceKind::MouseButton:
        if (binding.code >= static_cast<uint16_t>(platform::MouseButton::Count)) {
            return Error{ErrorCode::InvalidArgument, "MouseButton out of range"};
        }
        break;
    case InputSourceKind::GamepadButton:
        if (binding.gamepadIndex >= platform::kMaxGamepads ||
            binding.code >= static_cast<uint16_t>(platform::GamepadButton::Count)) {
            return Error{ErrorCode::InvalidArgument, "GamepadButton or index out of range"};
        }
        break;
    case InputSourceKind::GamepadAxis:
        if (binding.gamepadIndex >= platform::kMaxGamepads ||
            binding.code >= static_cast<uint16_t>(platform::GamepadAxis::Count)) {
            return Error{ErrorCode::InvalidArgument, "GamepadAxis or index out of range"};
        }
        break;
    case InputSourceKind::MouseDeltaX:
    case InputSourceKind::MouseDeltaY:
    case InputSourceKind::MouseWheelX:
    case InputSourceKind::MouseWheelY:
        break;
    default:
        return Error{ErrorCode::InvalidArgument, "Unknown InputSourceKind"};
    }

    return Result<void>::success();
}

[[nodiscard]] float applyDeadZone(float rawValue, float deadZone) noexcept {
    const float absVal = std::abs(rawValue);
    if (absVal <= deadZone) {
        return 0.0F;
    }
    const float denom = 1.0F - deadZone;
    const float normalized = (denom > 1e-5F) ? ((absVal - deadZone) / denom) : absVal;
    const float clamped = std::min(1.0F, normalized);
    return (rawValue >= 0.0F) ? clamped : -clamped;
}

[[nodiscard]] float sampleBindingValue(const InputBinding& binding,
                                       const platform::InputSnapshot& snapshot) noexcept {
    if (binding.gateMouseButton != platform::MouseButton::Count &&
        !snapshot.isMouseButtonDown(binding.gateMouseButton)) {
        return 0.0F;
    }

    float raw = 0.0F;
    switch (binding.kind) {
    case InputSourceKind::Key:
        raw = snapshot.isKeyDown(static_cast<platform::KeyCode>(binding.code)) ? 1.0F : 0.0F;
        break;
    case InputSourceKind::MouseButton:
        raw = snapshot.isMouseButtonDown(static_cast<platform::MouseButton>(binding.code)) ? 1.0F
                                                                                           : 0.0F;
        break;
    case InputSourceKind::MouseDeltaX:
        raw = snapshot.mouseDeltaX;
        break;
    case InputSourceKind::MouseDeltaY:
        raw = snapshot.mouseDeltaY;
        break;
    case InputSourceKind::MouseWheelX:
        raw = snapshot.wheelDeltaX;
        break;
    case InputSourceKind::MouseWheelY:
        raw = snapshot.wheelDeltaY;
        break;
    case InputSourceKind::GamepadButton:
        raw = snapshot.isGamepadButtonDown(binding.gamepadIndex,
                                           static_cast<platform::GamepadButton>(binding.code))
                  ? 1.0F
                  : 0.0F;
        break;
    case InputSourceKind::GamepadAxis: {
        const float axisVal = snapshot.getGamepadAxis(
            binding.gamepadIndex, static_cast<platform::GamepadAxis>(binding.code));
        raw = applyDeadZone(axisVal, binding.deadZone);
        break;
    }
    }

    return raw * binding.scale;
}

[[nodiscard]] std::optional<platform::KeyCode> parseKeyName(std::string_view name) noexcept {
    if (name == "W")
        return platform::KeyCode::W;
    if (name == "A")
        return platform::KeyCode::A;
    if (name == "S")
        return platform::KeyCode::S;
    if (name == "D")
        return platform::KeyCode::D;
    if (name == "Q")
        return platform::KeyCode::Q;
    if (name == "E")
        return platform::KeyCode::E;
    if (name == "Space")
        return platform::KeyCode::Space;
    if (name == "LeftShift")
        return platform::KeyCode::LeftShift;
    if (name == "LeftCtrl")
        return platform::KeyCode::LeftCtrl;
    if (name == "Up")
        return platform::KeyCode::Up;
    if (name == "Down")
        return platform::KeyCode::Down;
    if (name == "Left")
        return platform::KeyCode::Left;
    if (name == "Right")
        return platform::KeyCode::Right;
    return std::nullopt;
}

} // namespace

int GameplayInputSystem::findContextIndex(uint64_t contextId) const noexcept {
    for (size_t i = 0; i < contextCount_; ++i) {
        if (contexts_[i].used && contexts_[i].id == contextId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int GameplayInputSystem::findActionIndex(uint64_t actionId) const noexcept {
    for (size_t i = 0; i < actionCount_; ++i) {
        if (actions_[i].used && actions_[i].id == actionId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int GameplayInputSystem::findOrCreateActionIndex(uint64_t actionId) noexcept {
    const int existing = findActionIndex(actionId);
    if (existing >= 0) {
        return existing;
    }
    if (actionCount_ >= kMaxActions) {
        return -1;
    }
    const size_t idx = actionCount_++;
    actions_[idx] = ActionEntry{};
    actions_[idx].id = actionId;
    actions_[idx].used = true;
    return static_cast<int>(idx);
}

Result<void> GameplayInputSystem::registerContext(std::string_view contextName, int32_t priority) {
    if (contextName.empty()) {
        return Error{ErrorCode::InvalidArgument, "Context name cannot be empty"};
    }
    const uint64_t ctxId = hashInputName(contextName);
    const int existing = findContextIndex(ctxId);
    if (existing >= 0) {
        contexts_[static_cast<size_t>(existing)].priority = priority;
        return Result<void>::success();
    }
    if (contextCount_ >= kMaxContexts) {
        return Error{ErrorCode::OutOfBounds, "Maximum number of input contexts reached"};
    }
    contexts_[contextCount_++] = ContextEntry{
        .id = ctxId,
        .priority = priority,
        .used = true,
        .enabled = true,
    };
    return Result<void>::success();
}

Result<void> GameplayInputSystem::setContextEnabled(std::string_view contextName, bool enabled) {
    const int idx = findContextIndex(hashInputName(contextName));
    if (idx < 0) {
        return Error{ErrorCode::InvalidArgument, "Input context not registered"};
    }
    contexts_[static_cast<size_t>(idx)].enabled = enabled;
    return Result<void>::success();
}

bool GameplayInputSystem::isContextEnabled(std::string_view contextName) const noexcept {
    const int idx = findContextIndex(hashInputName(contextName));
    return idx >= 0 && contexts_[static_cast<size_t>(idx)].enabled;
}

Result<void> GameplayInputSystem::bindAction(std::string_view contextName,
                                             std::string_view actionName,
                                             const InputBinding& binding) {
    if (actionName.empty()) {
        return Error{ErrorCode::InvalidArgument, "Action name cannot be empty"};
    }
    const int ctxIdx = findContextIndex(hashInputName(contextName));
    if (ctxIdx < 0) {
        return Error{ErrorCode::InvalidArgument, "Input context not registered"};
    }
    auto valRes = validateBinding(binding);
    if (!valRes.hasValue()) {
        return valRes;
    }

    const int actionIdx = findOrCreateActionIndex(hashInputName(actionName));
    if (actionIdx < 0) {
        return Error{ErrorCode::OutOfBounds, "Maximum number of input actions reached"};
    }

    auto& entry = actions_[static_cast<size_t>(actionIdx)];
    if (entry.bindingCount >= kMaxBindingsPerAction) {
        return Error{ErrorCode::OutOfBounds, "Maximum bindings per action reached"};
    }

    entry.bindings[entry.bindingCount++] = BindingSlot{
        .contextId = contexts_[static_cast<size_t>(ctxIdx)].id,
        .binding = binding,
        .used = true,
    };
    return Result<void>::success();
}

Result<void> GameplayInputSystem::rebindAction(std::string_view contextName,
                                               std::string_view actionName,
                                               const InputBinding& binding) {
    auto valRes = validateBinding(binding);
    if (!valRes.hasValue()) {
        return valRes;
    }
    auto unbindRes = unbindAction(contextName, actionName);
    if (!unbindRes.hasValue()) {
        return unbindRes;
    }
    return bindAction(contextName, actionName, binding);
}

Result<void> GameplayInputSystem::unbindAction(std::string_view contextName,
                                               std::string_view actionName) {
    const int ctxIdx = findContextIndex(hashInputName(contextName));
    if (ctxIdx < 0) {
        return Error{ErrorCode::InvalidArgument, "Input context not registered"};
    }
    const uint64_t ctxId = contexts_[static_cast<size_t>(ctxIdx)].id;
    const int actionIdx = findActionIndex(hashInputName(actionName));
    if (actionIdx < 0) {
        return Result<void>::success();
    }

    auto& entry = actions_[static_cast<size_t>(actionIdx)];
    size_t writeIdx = 0;
    for (size_t i = 0; i < entry.bindingCount; ++i) {
        if (entry.bindings[i].used && entry.bindings[i].contextId != ctxId) {
            entry.bindings[writeIdx++] = entry.bindings[i];
        }
    }
    for (size_t i = writeIdx; i < entry.bindingCount; ++i) {
        entry.bindings[i] = BindingSlot{};
    }
    entry.bindingCount = writeIdx;
    return Result<void>::success();
}

Result<void> GameplayInputSystem::clearContext(std::string_view contextName) {
    const int ctxIdx = findContextIndex(hashInputName(contextName));
    if (ctxIdx < 0) {
        return Error{ErrorCode::InvalidArgument, "Input context not registered"};
    }
    const uint64_t ctxId = contexts_[static_cast<size_t>(ctxIdx)].id;
    for (size_t a = 0; a < actionCount_; ++a) {
        auto& entry = actions_[a];
        size_t writeIdx = 0;
        for (size_t i = 0; i < entry.bindingCount; ++i) {
            if (entry.bindings[i].used && entry.bindings[i].contextId != ctxId) {
                entry.bindings[writeIdx++] = entry.bindings[i];
            }
        }
        for (size_t i = writeIdx; i < entry.bindingCount; ++i) {
            entry.bindings[i] = BindingSlot{};
        }
        entry.bindingCount = writeIdx;
    }
    return Result<void>::success();
}

Result<void> GameplayInputSystem::loadMappingsFromJsonString(std::string_view jsonText) {
    auto parseRes = core::JsonValue::parse(jsonText);
    if (!parseRes.hasValue()) {
        return Error{ErrorCode::InvalidArgument, "Failed to parse input mapping JSON"};
    }
    const auto& root = parseRes.value();
    if (!root.isObject()) {
        return Error{ErrorCode::InvalidArgument, "Input mapping JSON root must be an object"};
    }

    const auto* contextsArr = root.getArray("contexts");
    if (contextsArr == nullptr) {
        return Error{ErrorCode::InvalidArgument, "Input mapping JSON missing 'contexts' array"};
    }

    for (size_t c = 0; c < contextsArr->size(); ++c) {
        const auto& ctxVal = (*contextsArr)[c];
        if (!ctxVal.isObject()) {
            return Error{ErrorCode::InvalidArgument, "Input context entry must be an object"};
        }
        const std::string_view ctxName = ctxVal.getString("name", "");
        const int32_t priority = ctxVal.getInt("priority", 0);
        auto regRes = registerContext(ctxName, priority);
        if (!regRes.hasValue()) {
            return regRes;
        }
        (void)setContextEnabled(ctxName, ctxVal.getBool("enabled", true));

        const auto* bindingsArr = ctxVal.getArray("bindings");
        if (bindingsArr == nullptr) {
            continue;
        }
        for (size_t b = 0; b < bindingsArr->size(); ++b) {
            const auto& bindVal = (*bindingsArr)[b];
            if (!bindVal.isObject()) {
                return Error{ErrorCode::InvalidArgument, "Input binding entry must be an object"};
            }
            const std::string_view actionName = bindVal.getString("action", "");
            const std::string_view sourceType = bindVal.getString("source", "Key");
            const float scale = bindVal.getFloat("scale", 1.0F);
            const float deadZone = bindVal.getFloat("deadZone", 0.15F);

            InputBinding binding{};
            binding.scale = scale;
            binding.deadZone = deadZone;
            if (sourceType == "Key") {
                binding.kind = InputSourceKind::Key;
                const auto keyOpt = parseKeyName(bindVal.getString("key", ""));
                if (!keyOpt.has_value()) {
                    return Error{ErrorCode::InvalidArgument, "Unknown key name in input mapping"};
                }
                binding.code = static_cast<uint16_t>(*keyOpt);
            } else if (sourceType == "MouseDeltaX") {
                binding.kind = InputSourceKind::MouseDeltaX;
                if (bindVal.getBool("requireRightMouse", false)) {
                    binding.gateMouseButton = platform::MouseButton::Right;
                }
            } else if (sourceType == "MouseDeltaY") {
                binding.kind = InputSourceKind::MouseDeltaY;
                if (bindVal.getBool("requireRightMouse", false)) {
                    binding.gateMouseButton = platform::MouseButton::Right;
                }
            } else {
                return Error{ErrorCode::InvalidArgument, "Unsupported source type in input JSON"};
            }

            auto bindRes = bindAction(ctxName, actionName, binding);
            if (!bindRes.hasValue()) {
                return bindRes;
            }
        }
    }

    return Result<void>::success();
}

Result<void> GameplayInputSystem::loadMappingsFromFile(const std::filesystem::path& filepath) {
    std::ifstream ifs(filepath, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return Error{ErrorCode::InvalidArgument, "Failed to open input mapping file"};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return loadMappingsFromJsonString(oss.str());
}

void GameplayInputSystem::update(const platform::InputSnapshot& snapshot) noexcept {
    for (size_t a = 0; a < actionCount_; ++a) {
        auto& entry = actions_[a];
        if (!entry.used) {
            continue;
        }

        float accumulatedValue = 0.0F;
        bool anyHeld = false;
        int32_t highestActivePriority = -2147483648;
        bool hasActiveBinding = false;

        for (size_t b = 0; b < entry.bindingCount; ++b) {
            const auto& slot = entry.bindings[b];
            if (!slot.used) {
                continue;
            }
            const int ctxIdx = findContextIndex(slot.contextId);
            if (ctxIdx < 0 || !contexts_[static_cast<size_t>(ctxIdx)].enabled) {
                continue;
            }
            const int32_t prio = contexts_[static_cast<size_t>(ctxIdx)].priority;
            if (hasActiveBinding && prio < highestActivePriority) {
                continue;
            }
            if (!hasActiveBinding || prio > highestActivePriority) {
                highestActivePriority = prio;
                hasActiveBinding = true;
                accumulatedValue = 0.0F;
                anyHeld = false;
            }

            const float val = sampleBindingValue(slot.binding, snapshot);
            if (std::abs(val) > std::abs(accumulatedValue)) {
                accumulatedValue = val;
            }
            if (std::abs(val) >= kDigitalThreshold) {
                anyHeld = true;
            }
        }

        entry.current.value = accumulatedValue;
        entry.current.held = anyHeld;
        entry.current.pressed = anyHeld && !entry.prevHeld;
        entry.current.released = !anyHeld && entry.prevHeld;
        entry.prevHeld = anyHeld;
    }
}

void GameplayInputSystem::resetStates() noexcept {
    for (size_t a = 0; a < actionCount_; ++a) {
        actions_[a].current = ActionState{};
        actions_[a].prevHeld = false;
    }
}

ActionState GameplayInputSystem::getActionState(std::string_view actionName) const noexcept {
    const int idx = findActionIndex(hashInputName(actionName));
    if (idx < 0) {
        return ActionState{};
    }
    return actions_[static_cast<size_t>(idx)].current;
}

bool GameplayInputSystem::isActionPressed(std::string_view actionName) const noexcept {
    return getActionState(actionName).pressed;
}

bool GameplayInputSystem::isActionHeld(std::string_view actionName) const noexcept {
    return getActionState(actionName).held;
}

bool GameplayInputSystem::isActionReleased(std::string_view actionName) const noexcept {
    return getActionState(actionName).released;
}

float GameplayInputSystem::getActionValue(std::string_view actionName) const noexcept {
    return getActionState(actionName).value;
}

float GameplayInputSystem::getAxis(std::string_view negativeAction,
                                   std::string_view positiveAction) const noexcept {
    return getActionValue(positiveAction) - getActionValue(negativeAction);
}

Vec2 GameplayInputSystem::getAxis2D(std::string_view leftAction, std::string_view rightAction,
                                    std::string_view downAction,
                                    std::string_view upAction) const noexcept {
    return Vec2{
        getAxis(leftAction, rightAction),
        getAxis(downAction, upAction),
    };
}

void GameplayInputSystem::installDefaultThirdPersonBindings() {
    (void)registerContext("Gameplay", 0);
    (void)clearContext("Gameplay");

    (void)bindAction("Gameplay", "MoveForward", InputBinding::fromKey(platform::KeyCode::W));
    (void)bindAction("Gameplay", "MoveBackward", InputBinding::fromKey(platform::KeyCode::S));
    (void)bindAction("Gameplay", "MoveLeft", InputBinding::fromKey(platform::KeyCode::A));
    (void)bindAction("Gameplay", "MoveRight", InputBinding::fromKey(platform::KeyCode::D));
    (void)bindAction("Gameplay", "Jump", InputBinding::fromKey(platform::KeyCode::Space));

    (void)bindAction("Gameplay", "CameraOrbitLeft", InputBinding::fromKey(platform::KeyCode::Q));
    (void)bindAction("Gameplay", "CameraOrbitRight", InputBinding::fromKey(platform::KeyCode::E));
    (void)bindAction("Gameplay", "CameraLookX",
                     InputBinding::fromMouseDeltaX(-0.004F, platform::MouseButton::Right));
    (void)bindAction("Gameplay", "CameraLookY",
                     InputBinding::fromMouseDeltaY(0.004F, platform::MouseButton::Right));

    // Optional gamepad defaults on port 0
    (void)bindAction("Gameplay", "Jump",
                     InputBinding::fromGamepadButton(platform::GamepadButton::South));
    (void)bindAction("Gameplay", "MoveRight",
                     InputBinding::fromGamepadAxis(platform::GamepadAxis::LeftX, 0, 0.15F, 1.0F));
    (void)bindAction("Gameplay", "MoveForward",
                     InputBinding::fromGamepadAxis(platform::GamepadAxis::LeftY, 0, 0.15F, -1.0F));
}

} // namespace eng::framework
