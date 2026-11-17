/// @file script_components.cpp
/// @brief Implementation of Flecs ECS components and execution systems for entity scripting.

#include "script/script_components.h"

#include "core/log.h"
#include "script/script_engine.h"

namespace eng::script {

void registerScriptComponents(world::World& world) {
    auto& ecs = world.getRawWorld();
    ecs.component<ScriptComponent>("ScriptComponent");
}

void registerScriptSystems(world::World& world, ScriptEngine& scriptEngine,
                           world::SystemGroup group) {
    auto& ecs = world.getRawWorld();

    auto system =
        ecs.system<ScriptComponent>("ScriptUpdateSystem")
            .kind(flecs::OnUpdate)
            .each([&scriptEngine](flecs::entity e, ScriptComponent& comp) {
                if (!comp.enabled || comp.scriptName[0] == '\0') {
                    return;
                }

                float dt = 0.016f;
                if (comp.updateInterval > 0.0f) {
                    comp.timeSinceLastUpdate += dt;
                    if (comp.timeSinceLastUpdate < comp.updateInterval) {
                        return;
                    }
                    comp.timeSinceLastUpdate = 0.0f;
                }

                world::EntityHandle handle{e.id()};
                const std::string_view scriptView(comp.scriptName);
                const std::string_view funcName =
                    (scriptView.find(".lua") != std::string_view::npos)
                        ? std::string_view("on_update")
                        : scriptView;
                auto result = scriptEngine.callEntityUpdate(funcName, handle, dt);
                if (!result.hasValue()) {
                    // Isolate failure: log error once and disable component to prevent frame log
                    // spam
                    ENG_LOG_FORMAT(LogLevel::Warning, "Script",
                                   "Entity %llu script '%s' failed: %.*s. Disabling component.",
                                   static_cast<unsigned long long>(handle.id), comp.scriptName,
                                   static_cast<int>(scriptEngine.lastErrorMessage().size()),
                                   scriptEngine.lastErrorMessage().data());
                    comp.enabled = false;
                }
            });

    world.registerSystem(group, system);
}

} // namespace eng::script
