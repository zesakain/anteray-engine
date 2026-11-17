-- TPP Showcase Lua Gameplay Script (Milestone 12D)
-- Budgeted 20 Hz trigger pad: when PlayerCharacter steps onto LuaJumpPad,
-- Lua bindings trigger a jump and emit a gameplay event.

local trigger_cooldown = 0.0

function on_init(pad_entity)
    trigger_cooldown = 0.0
    local state = conductor.get_state()
    events.emit("tpp_showcase_ready", state)
end

function on_update(pad_entity, dt)
    if trigger_cooldown > 0.0 then
        trigger_cooldown = trigger_cooldown - dt
        return
    end

    local player = character.find_player()
    if player == nil or not entity.is_valid(player) then
        return
    end

    local pad_pos = entity.get_position(pad_entity)
    local player_pos = entity.get_position(player)
    local dx = player_pos.x - pad_pos.x
    local dz = player_pos.z - pad_pos.z
    local dist_sq = (dx * dx) + (dz * dz)

    if dist_sq <= 2.25 and character.is_grounded(player) then
        character.request_jump(player)
        events.emit("lua_jump_pad_triggered", entity.get_name(player))
        trigger_cooldown = 0.75
    end
end
