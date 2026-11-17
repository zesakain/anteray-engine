/// @file framework_script_bindings.cpp
/// @brief Implementation of controlled Lua gameplay bindings for Character and Conductor.

#include "framework/framework_script_bindings.h"

#include "core/log.h"
#include "framework/character_components.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace eng::framework {
namespace {

struct BindingContext {
    world::World* world{nullptr};
    Conductor* conductor{nullptr};
    script::ScriptEngine* scriptEngine{nullptr};
};

// Static context pointer updated per bound runtime engine (simulation thread only).
BindingContext gBindingCtx{};

[[nodiscard]] const char* conductorSlotName(ActiveSlot slot) noexcept {
    return (slot == ActiveSlot::SlotA) ? "SlotA" : "SlotB";
}

[[nodiscard]] ActiveSlot oppositeSlot(ActiveSlot slot) noexcept {
    return (slot == ActiveSlot::SlotA) ? ActiveSlot::SlotB : ActiveSlot::SlotA;
}

[[nodiscard]] std::optional<ActiveSlot> parseSlotArg(std::string_view s) noexcept {
    if (s == "SlotA" || s == "Slot A" || s == "A" || s == "State A") {
        return ActiveSlot::SlotA;
    }
    if (s == "SlotB" || s == "Slot B" || s == "B" || s == "State B") {
        return ActiveSlot::SlotB;
    }
    return std::nullopt;
}

[[nodiscard]] const char* conductorStateName(ConductorState s) noexcept {
    switch (s) {
    case ConductorState::Uninitialized:
        return "Uninitialized";
    case ConductorState::Active:
        return "Active";
    case ConductorState::Preparing:
        return "Preparing";
    case ConductorState::Prepared:
        return "Prepared";
    case ConductorState::Committing:
        return "Committing";
    case ConductorState::Transitioning:
        return "Transitioning";
    case ConductorState::Terminated:
        return "Terminated";
    }
    return "Unknown";
}

int32_t luaCharFindPlayer(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.world == nullptr) {
        ctx.pushNil();
        return 1;
    }
    flecs::entity found{};
    gBindingCtx.world->getRawWorld().each<const PlayerControlledTag>(
        [&found](flecs::entity e, const PlayerControlledTag&) {
            if (!found.is_valid()) {
                found = e;
            }
        });
    if (!found.is_valid() || !found.is_alive()) {
        ctx.pushNil();
    } else {
        ctx.pushEntity(world::EntityHandle{found.id()});
    }
    return 1;
}

int32_t luaCharSetMovementIntent(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.world == nullptr) {
        return 0;
    }
    const world::EntityHandle ent = ctx.getEntity(1);
    if (!ent.isValid() || !gBindingCtx.world->isAlive(ent)) {
        ctx.setError("Invalid entity passed to character.set_movement_intent");
        return -1;
    }
    const float x = static_cast<float>(ctx.getNumber(2, 0.0));
    const float z = static_cast<float>(ctx.getNumber(3, 0.0));

    if (auto* intent =
            gBindingCtx.world->getMutableComponent<CharacterMovementIntentComponent>(ent)) {
        intent->moveAxis = Vec2{x, z};
        ctx.pushBool(true);
    } else {
        ctx.pushBool(false);
    }
    return 1;
}

int32_t luaCharRequestJump(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.world == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const world::EntityHandle ent = ctx.getEntity(1);
    if (!ent.isValid() || !gBindingCtx.world->isAlive(ent)) {
        ctx.setError("Invalid entity passed to character.request_jump");
        return -1;
    }
    if (auto* intent =
            gBindingCtx.world->getMutableComponent<CharacterMovementIntentComponent>(ent)) {
        intent->scriptJumpRequested = true;
        ctx.pushBool(true);
    } else {
        ctx.pushBool(false);
    }
    return 1;
}

int32_t luaCharIsGrounded(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.world == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const world::EntityHandle ent = ctx.getEntity(1);
    if (!ent.isValid() || !gBindingCtx.world->isAlive(ent)) {
        ctx.pushBool(false);
        return 1;
    }
    const auto* st = gBindingCtx.world->getComponent<CharacterStateComponent>(ent);
    ctx.pushBool(st != nullptr && st->isGrounded);
    return 1;
}

int32_t luaCharGetVelocity(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.world == nullptr) {
        ctx.pushVec3(Vec3::zero());
        return 1;
    }
    const world::EntityHandle ent = ctx.getEntity(1);
    if (!ent.isValid() || !gBindingCtx.world->isAlive(ent)) {
        ctx.pushVec3(Vec3::zero());
        return 1;
    }
    const auto* st = gBindingCtx.world->getComponent<CharacterStateComponent>(ent);
    ctx.pushVec3(st != nullptr ? st->velocityMetersPerSec : Vec3::zero());
    return 1;
}

int32_t luaCharGetSpeed(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.world == nullptr) {
        ctx.pushNumber(0.0);
        return 1;
    }
    const world::EntityHandle ent = ctx.getEntity(1);
    if (!ent.isValid() || !gBindingCtx.world->isAlive(ent)) {
        ctx.pushNumber(0.0);
        return 1;
    }
    const auto* st = gBindingCtx.world->getComponent<CharacterStateComponent>(ent);
    if (st == nullptr) {
        ctx.pushNumber(0.0);
        return 1;
    }
    const float horiz = std::sqrt((st->velocityMetersPerSec.x * st->velocityMetersPerSec.x) +
                                  (st->velocityMetersPerSec.z * st->velocityMetersPerSec.z));
    ctx.pushNumber(static_cast<double>(horiz));
    return 1;
}

int32_t luaConductorGetState(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushString("Unavailable");
        return 1;
    }
    ctx.pushString(conductorStateName(gBindingCtx.conductor->getState()));
    return 1;
}

int32_t luaConductorGetActiveSlot(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushNil();
        return 1;
    }
    ctx.pushString(conductorSlotName(gBindingCtx.conductor->getActiveSlot()));
    return 1;
}

int32_t luaConductorGetStandbySlot(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushNil();
        return 1;
    }
    ctx.pushString(conductorSlotName(oppositeSlot(gBindingCtx.conductor->getActiveSlot())));
    return 1;
}

int32_t luaConductorGetActiveModeName(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushString("");
        return 1;
    }
    ctx.pushString(gBindingCtx.conductor->getActiveMode().getName());
    return 1;
}

int32_t luaConductorGetStandbyModeName(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushString("");
        return 1;
    }
    ctx.pushString(gBindingCtx.conductor->getStandbyMode().getName());
    return 1;
}

int32_t luaConductorGetSlotDisplayName(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushString("");
        return 1;
    }
    const std::string_view arg = ctx.getString(1);
    const ActiveSlot slot = parseSlotArg(arg).value_or(gBindingCtx.conductor->getActiveSlot());
    ctx.pushString(gBindingCtx.conductor->getSlotDisplayName(slot));
    return 1;
}

int32_t luaConductorIsStandby(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.scriptEngine != nullptr && gBindingCtx.scriptEngine->isStandbyMode()) {
        ctx.pushBool(true);
        return 1;
    }
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const auto st = gBindingCtx.conductor->getState();
    ctx.pushBool(st == ConductorState::Preparing || st == ConductorState::Prepared);
    return 1;
}

int32_t luaConductorIsPreparing(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushBool(gBindingCtx.conductor != nullptr &&
                 gBindingCtx.conductor->getState() == ConductorState::Preparing);
    return 1;
}

int32_t luaConductorIsPrepared(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushBool(gBindingCtx.conductor != nullptr &&
                 gBindingCtx.conductor->getState() == ConductorState::Prepared);
    return 1;
}

int32_t luaConductorIsTransitioning(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushBool(gBindingCtx.conductor != nullptr &&
                 gBindingCtx.conductor->getState() == ConductorState::Transitioning);
    return 1;
}

int32_t luaConductorHasPendingTransition(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushBool(gBindingCtx.conductor != nullptr && gBindingCtx.conductor->hasPendingTransition());
    return 1;
}

int32_t luaConductorGetPendingTargetSlot(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushNil();
        return 1;
    }
    const auto pending = gBindingCtx.conductor->getPendingTargetSlot();
    if (!pending.has_value()) {
        ctx.pushNil();
    } else {
        ctx.pushString(conductorSlotName(*pending));
    }
    return 1;
}

int32_t luaConductorGetSuccessfulTransitions(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? static_cast<double>(gBindingCtx.conductor->getSuccessfulTransitionsCount())
                       : 0.0);
    return 1;
}

int32_t luaConductorGetFailedTransitions(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? static_cast<double>(gBindingCtx.conductor->getFailedTransitionsCount())
                       : 0.0);
    return 1;
}

int32_t luaConductorGetWorldTime(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? gBindingCtx.conductor->getWorldClock().totalTime().toSeconds()
                       : 0.0);
    return 1;
}

int32_t luaConductorGetActionTime(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? gBindingCtx.conductor->getActionClock().totalTime().toSeconds()
                       : 0.0);
    return 1;
}

int32_t luaConductorGetUiTime(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? gBindingCtx.conductor->getUiClock().totalTime().toSeconds()
                       : 0.0);
    return 1;
}

int32_t luaConductorGetWorldDelta(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? gBindingCtx.conductor->getWorldClock().deltaTime().toSeconds()
                       : 0.0);
    return 1;
}

int32_t luaConductorGetActionDelta(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? gBindingCtx.conductor->getActionClock().deltaTime().toSeconds()
                       : 0.0);
    return 1;
}

int32_t luaConductorGetActionTimeScale(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(gBindingCtx.conductor != nullptr
                       ? static_cast<double>(gBindingCtx.conductor->getActionClock().timeScale())
                       : 1.0);
    return 1;
}

int32_t luaConductorRequestTransition(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const int32_t argc = ctx.getArgCount();
    if (argc == 0) {
        auto res = gBindingCtx.conductor->requestTransition(false);
        ctx.pushBool(res.hasValue());
        return 1;
    }
    const std::string_view targetStr = ctx.getString(1);
    if (!targetStr.empty()) {
        const bool autoCommit = (argc >= 2) ? ctx.getBool(2, false) : false;
        if (const auto slot = parseSlotArg(targetStr); slot.has_value()) {
            auto res = gBindingCtx.conductor->requestTransition(*slot, autoCommit);
            ctx.pushBool(res.hasValue());
        } else {
            auto res = gBindingCtx.conductor->requestTransition(targetStr, autoCommit);
            ctx.pushBool(res.hasValue());
        }
        return 1;
    }
    const bool autoCommit = ctx.getBool(1, false);
    auto res = gBindingCtx.conductor->requestTransition(autoCommit);
    ctx.pushBool(res.hasValue());
    return 1;
}

int32_t luaConductorExecuteTransition(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    auto res = gBindingCtx.conductor->executeTransition();
    ctx.pushBool(res.hasValue());
    return 1;
}

int32_t luaConductorCancelTransition(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const auto st = gBindingCtx.conductor->getState();
    if (st != ConductorState::Preparing && st != ConductorState::Prepared &&
        st != ConductorState::Transitioning) {
        ctx.pushBool(false);
        return 1;
    }
    gBindingCtx.conductor->cancelTransition();
    ctx.pushBool(true);
    return 1;
}

// Standby speculative StateContext preparation helpers (never mutate authoritative World).
int32_t luaConductorPrepareClear(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor != nullptr) {
        gBindingCtx.conductor->getStandbyStateContext().resetCache();
    }
    (void)ctx;
    return 0;
}

int32_t luaConductorPrepareSetReady(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const bool ready = ctx.getBool(1, true);
    gBindingCtx.conductor->getStandbyStateContext().cache.isReady = ready;
    ctx.pushBool(true);
    return 1;
}

int32_t luaConductorPrepareIsReady(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushBool(gBindingCtx.conductor != nullptr &&
                 gBindingCtx.conductor->getStandbyStateContext().cache.isReady);
    return 1;
}

int32_t luaConductorPrepareAddEntity(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr || gBindingCtx.world == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const world::EntityHandle ent = ctx.getEntity(1);
    if (!ent.isValid() || !gBindingCtx.world->isAlive(ent)) {
        ctx.pushBool(false);
        return 1;
    }
    auto& entities = gBindingCtx.conductor->getStandbyStateContext().cache.preparedEntities;
    if (std::find(entities.begin(), entities.end(), ent) == entities.end()) {
        entities.push_back(ent);
    }
    ctx.pushBool(true);
    return 1;
}

int32_t luaConductorPrepareGetEntityCount(script::ScriptCallContext& ctx, void* /*ud*/) {
    ctx.pushNumber(
        gBindingCtx.conductor != nullptr
            ? static_cast<double>(
                  gBindingCtx.conductor->getStandbyStateContext().cache.preparedEntities.size())
            : 0.0);
    return 1;
}

int32_t luaConductorPrepareGetEntity(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushNil();
        return 1;
    }
    const int32_t oneBasedIdx = static_cast<int32_t>(ctx.getNumber(1, 0.0));
    const auto& entities = gBindingCtx.conductor->getStandbyStateContext().cache.preparedEntities;
    if (oneBasedIdx < 1 || static_cast<size_t>(oneBasedIdx) > entities.size()) {
        ctx.pushNil();
    } else {
        ctx.pushEntity(entities[static_cast<size_t>(oneBasedIdx - 1)]);
    }
    return 1;
}

int32_t luaConductorPrepareValidateEntities(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr || gBindingCtx.world == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    auto& entities = gBindingCtx.conductor->getStandbyStateContext().cache.preparedEntities;
    const size_t beforeCount = entities.size();
    entities.erase(std::remove_if(entities.begin(), entities.end(),
                                  [](world::EntityHandle h) {
                                      return !h.isValid() || !gBindingCtx.world->isAlive(h);
                                  }),
                   entities.end());
    ctx.pushBool(entities.size() == beforeCount);
    return 1;
}

int32_t luaConductorPrepareSetPrimaryTarget(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr || gBindingCtx.world == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const world::EntityHandle ent = ctx.getEntity(1);
    if (ent.isValid() && !gBindingCtx.world->isAlive(ent)) {
        ctx.pushBool(false);
        return 1;
    }
    gBindingCtx.conductor->getStandbyStateContext().targeting.primaryTarget = ent;
    ctx.pushBool(true);
    return 1;
}

int32_t luaConductorGetPrimaryTarget(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushNil();
        return 1;
    }
    const std::string_view slotArg = ctx.getString(1);
    const StateContext& stateCtx =
        slotArg.empty() ? gBindingCtx.conductor->getActiveStateContext()
                        : gBindingCtx.conductor->getStateContext(parseSlotArg(slotArg).value_or(
                              gBindingCtx.conductor->getActiveSlot()));
    const world::EntityHandle target = stateCtx.targeting.primaryTarget;
    if (!target.isValid()) {
        ctx.pushNil();
    } else {
        ctx.pushEntity(target);
    }
    return 1;
}

int32_t luaConductorGetActiveCamera(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushNil();
        return 1;
    }
    const std::string_view slotArg = ctx.getString(1);
    const StateContext& stateCtx =
        slotArg.empty() ? gBindingCtx.conductor->getActiveStateContext()
                        : gBindingCtx.conductor->getStateContext(parseSlotArg(slotArg).value_or(
                              gBindingCtx.conductor->getActiveSlot()));
    const world::EntityHandle cam = stateCtx.camera.activeCamera;
    if (!cam.isValid()) {
        ctx.pushNil();
    } else {
        ctx.pushEntity(cam);
    }
    return 1;
}

void invokeHookIfPresent(script::ScriptEngine& engine, std::string_view hookName,
                         std::span<const script::ScriptValue> args) {
    if (!engine.hasFunction(hookName)) {
        return;
    }
    auto res = engine.callFunction(hookName, args);
    if (!res.hasValue() && res.error() != script::ScriptError::NotFound) {
        ENG_LOG_FORMAT(LogLevel::Warning, "Script", "Conductor hook '%.*s' failed: %.*s",
                       static_cast<int>(hookName.size()), hookName.data(),
                       static_cast<int>(engine.lastErrorMessage().size()),
                       engine.lastErrorMessage().data());
    }
}

} // namespace

Result<void, script::ScriptError>
registerFrameworkScriptBindings(script::ScriptEngine& scriptEngine, world::World& world,
                                Conductor* conductor) {
    gBindingCtx.world = &world;
    gBindingCtx.conductor = conductor;
    gBindingCtx.scriptEngine = &scriptEngine;

    const std::array<script::ScriptHostFunction, 6> charFns{{
        {.name = "find_player", .callback = luaCharFindPlayer, .mutatesWorld = false},
        {.name = "set_movement_intent", .callback = luaCharSetMovementIntent, .mutatesWorld = true},
        {.name = "request_jump", .callback = luaCharRequestJump, .mutatesWorld = true},
        {.name = "is_grounded", .callback = luaCharIsGrounded, .mutatesWorld = false},
        {.name = "get_velocity", .callback = luaCharGetVelocity, .mutatesWorld = false},
        {.name = "get_speed", .callback = luaCharGetSpeed, .mutatesWorld = false},
    }};
    auto charRes = scriptEngine.registerModule("character", charFns);
    if (!charRes.hasValue()) {
        return charRes;
    }

    const std::array<script::ScriptHostFunction, 33> condFns{{
        {.name = "get_state", .callback = luaConductorGetState, .mutatesWorld = false},
        {.name = "get_active_slot", .callback = luaConductorGetActiveSlot, .mutatesWorld = false},
        {.name = "get_standby_slot", .callback = luaConductorGetStandbySlot, .mutatesWorld = false},
        {.name = "get_active_mode_name",
         .callback = luaConductorGetActiveModeName,
         .mutatesWorld = false},
        {.name = "get_standby_mode_name",
         .callback = luaConductorGetStandbyModeName,
         .mutatesWorld = false},
        {.name = "get_slot_display_name",
         .callback = luaConductorGetSlotDisplayName,
         .mutatesWorld = false},
        {.name = "is_standby", .callback = luaConductorIsStandby, .mutatesWorld = false},
        {.name = "is_preparing", .callback = luaConductorIsPreparing, .mutatesWorld = false},
        {.name = "is_prepared", .callback = luaConductorIsPrepared, .mutatesWorld = false},
        {.name = "is_transitioning",
         .callback = luaConductorIsTransitioning,
         .mutatesWorld = false},
        {.name = "has_pending_transition",
         .callback = luaConductorHasPendingTransition,
         .mutatesWorld = false},
        {.name = "get_pending_target_slot",
         .callback = luaConductorGetPendingTargetSlot,
         .mutatesWorld = false},
        {.name = "get_successful_transitions",
         .callback = luaConductorGetSuccessfulTransitions,
         .mutatesWorld = false},
        {.name = "get_failed_transitions",
         .callback = luaConductorGetFailedTransitions,
         .mutatesWorld = false},
        {.name = "get_world_time", .callback = luaConductorGetWorldTime, .mutatesWorld = false},
        {.name = "get_action_time", .callback = luaConductorGetActionTime, .mutatesWorld = false},
        {.name = "get_ui_time", .callback = luaConductorGetUiTime, .mutatesWorld = false},
        {.name = "get_world_delta", .callback = luaConductorGetWorldDelta, .mutatesWorld = false},
        {.name = "get_action_delta", .callback = luaConductorGetActionDelta, .mutatesWorld = false},
        {.name = "get_action_time_scale",
         .callback = luaConductorGetActionTimeScale,
         .mutatesWorld = false},
        {.name = "request_transition",
         .callback = luaConductorRequestTransition,
         .mutatesWorld = true},
        {.name = "execute_transition",
         .callback = luaConductorExecuteTransition,
         .mutatesWorld = true},
        {.name = "cancel_transition",
         .callback = luaConductorCancelTransition,
         .mutatesWorld = true},
        {.name = "prepare_clear", .callback = luaConductorPrepareClear, .mutatesWorld = false},
        {.name = "prepare_set_ready",
         .callback = luaConductorPrepareSetReady,
         .mutatesWorld = false},
        {.name = "prepare_is_ready", .callback = luaConductorPrepareIsReady, .mutatesWorld = false},
        {.name = "prepare_add_entity",
         .callback = luaConductorPrepareAddEntity,
         .mutatesWorld = false},
        {.name = "prepare_get_entity_count",
         .callback = luaConductorPrepareGetEntityCount,
         .mutatesWorld = false},
        {.name = "prepare_get_entity",
         .callback = luaConductorPrepareGetEntity,
         .mutatesWorld = false},
        {.name = "prepare_validate_entities",
         .callback = luaConductorPrepareValidateEntities,
         .mutatesWorld = false},
        {.name = "prepare_set_primary_target",
         .callback = luaConductorPrepareSetPrimaryTarget,
         .mutatesWorld = false},
        {.name = "get_primary_target",
         .callback = luaConductorGetPrimaryTarget,
         .mutatesWorld = false},
        {.name = "get_active_camera",
         .callback = luaConductorGetActiveCamera,
         .mutatesWorld = false},
    }};
    return scriptEngine.registerModule("conductor", condFns);
}

void ConductorScriptBridge::bind(script::ScriptEngine* scriptEngine,
                                 Conductor* conductor) noexcept {
    scriptEngine_ = scriptEngine;
    conductor_ = conductor;
    if (conductor_ != nullptr) {
        prevState_ = conductor_->getState();
        prevActiveSlot_ = conductor_->getActiveSlot();
        prevSuccessCount_ = conductor_->getSuccessfulTransitionsCount();
        prevFailCount_ = conductor_->getFailedTransitionsCount();
        initialized_ = true;
    } else {
        initialized_ = false;
    }
}

void ConductorScriptBridge::reset() noexcept {
    scriptEngine_ = nullptr;
    conductor_ = nullptr;
    prevState_ = ConductorState::Uninitialized;
    prevActiveSlot_ = ActiveSlot::SlotA;
    prevSuccessCount_ = 0;
    prevFailCount_ = 0;
    initialized_ = false;
}

void ConductorScriptBridge::update() {
    if (!initialized_ || scriptEngine_ == nullptr || !scriptEngine_->isInitialized() ||
        conductor_ == nullptr) {
        return;
    }

    const ConductorState curState = conductor_->getState();
    const ActiveSlot curSlot = conductor_->getActiveSlot();
    const uint32_t curSuccess = conductor_->getSuccessfulTransitionsCount();
    const uint32_t curFail = conductor_->getFailedTransitionsCount();

    if (curState != prevState_ || curSlot != prevActiveSlot_ || curSuccess != prevSuccessCount_ ||
        curFail != prevFailCount_) {
        if (prevState_ == ConductorState::Active && curState == ConductorState::Preparing) {
            const ActiveSlot toSlot = oppositeSlot(curSlot);
            const std::array<script::ScriptValue, 3> args{{
                std::string(conductorSlotName(curSlot)),
                std::string(conductorSlotName(toSlot)),
                std::string(conductor_->getStandbyMode().getName()),
            }};
            invokeHookIfPresent(*scriptEngine_, "on_transition_requested", args);
        }

        if (prevState_ == ConductorState::Preparing && curState == ConductorState::Prepared) {
            const ActiveSlot standbySlot = oppositeSlot(curSlot);
            const std::array<script::ScriptValue, 2> args{{
                std::string(conductorSlotName(standbySlot)),
                std::string(conductor_->getStandbyMode().getName()),
            }};
            invokeHookIfPresent(*scriptEngine_, "on_transition_prepared", args);
        }

        if (curSuccess > prevSuccessCount_) {
            const std::array<script::ScriptValue, 3> args{{
                std::string(conductorSlotName(prevActiveSlot_)),
                std::string(conductorSlotName(curSlot)),
                std::string(conductor_->getActiveMode().getName()),
            }};
            invokeHookIfPresent(*scriptEngine_, "on_transition_committed", args);
        }

        if (curFail > prevFailCount_) {
            const std::array<script::ScriptValue, 2> args{{
                std::string(conductorSlotName(curSlot)),
                std::string(conductor_->getActiveMode().getName()),
            }};
            invokeHookIfPresent(*scriptEngine_, "on_transition_failed", args);
        } else if ((prevState_ == ConductorState::Preparing ||
                    prevState_ == ConductorState::Prepared) &&
                   curState == ConductorState::Active && curSuccess == prevSuccessCount_) {
            const std::array<script::ScriptValue, 2> args{{
                std::string(conductorSlotName(curSlot)),
                std::string(conductor_->getActiveMode().getName()),
            }};
            invokeHookIfPresent(*scriptEngine_, "on_transition_cancelled", args);
        } else if (prevState_ == ConductorState::Transitioning &&
                   curState == ConductorState::Active) {
            const std::array<script::ScriptValue, 2> args{{
                std::string(conductorSlotName(curSlot)),
                std::string(conductor_->getActiveMode().getName()),
            }};
            invokeHookIfPresent(*scriptEngine_, "on_transition_completed", args);
        }

        prevState_ = curState;
        prevActiveSlot_ = curSlot;
        prevSuccessCount_ = curSuccess;
        prevFailCount_ = curFail;
    }

    if (curState == ConductorState::Preparing &&
        scriptEngine_->hasFunction("on_conductor_prepare")) {
        const ActiveSlot standbySlot = oppositeSlot(curSlot);
        const double budgetMs =
            static_cast<double>(
                conductor_->getStandbyStateContext().execution.prepareBudget.asMicroseconds()) *
            0.001;
        const std::array<script::ScriptValue, 3> args{{
            std::string(conductorSlotName(standbySlot)),
            std::string(conductor_->getStandbyMode().getName()),
            budgetMs,
        }};
        const auto prevExecMode = scriptEngine_->getExecutionMode();
        scriptEngine_->setExecutionMode(script::ScriptExecutionMode::Standby);
        invokeHookIfPresent(*scriptEngine_, "on_conductor_prepare", args);
        scriptEngine_->setExecutionMode(prevExecMode);
    }
}

} // namespace eng::framework
