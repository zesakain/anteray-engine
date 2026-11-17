/// @file framework_script_bindings.cpp
/// @brief Implementation of controlled Lua gameplay bindings for Character and Conductor.

#include "framework/framework_script_bindings.h"

#include "framework/character_components.h"

#include <array>
#include <cmath>

namespace eng::framework {
namespace {

struct BindingContext {
    world::World* world{nullptr};
    Conductor* conductor{nullptr};
};

// Static context pointer updated per bound runtime engine (simulation thread only).
BindingContext gBindingCtx{};

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
        intent->jumpRequested = true;
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

int32_t luaConductorIsStandby(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    const auto st = gBindingCtx.conductor->getState();
    ctx.pushBool(st == ConductorState::Preparing || st == ConductorState::Prepared);
    return 1;
}

int32_t luaConductorRequestTransition(script::ScriptCallContext& ctx, void* /*ud*/) {
    if (gBindingCtx.conductor == nullptr) {
        ctx.pushBool(false);
        return 1;
    }
    auto res = gBindingCtx.conductor->requestTransition();
    ctx.pushBool(res.hasValue());
    return 1;
}

} // namespace

Result<void, script::ScriptError>
registerFrameworkScriptBindings(script::ScriptEngine& scriptEngine, world::World& world,
                                Conductor* conductor) {
    gBindingCtx.world = &world;
    gBindingCtx.conductor = conductor;

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

    const std::array<script::ScriptHostFunction, 3> condFns{{
        {.name = "get_state", .callback = luaConductorGetState, .mutatesWorld = false},
        {.name = "is_standby", .callback = luaConductorIsStandby, .mutatesWorld = false},
        {.name = "request_transition",
         .callback = luaConductorRequestTransition,
         .mutatesWorld = true},
    }};
    return scriptEngine.registerModule("conductor", condFns);
}

} // namespace eng::framework
