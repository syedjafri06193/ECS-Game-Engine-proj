-- Lights orbit the scene. Edit this file while the demo runs: it reloads
-- instantly, and `state` survives the reload (design doc §3.1).
return {
  name = "orbiting-lights",
  reads = { "PointLight" },
  writes = { "Position" },

  update = function(world, dt, state)
    state.t = (state.t or 0) + dt
    local t = state.t
    local speed = 0.35
    local i = 0
    world:each({ "Position", "PointLight" }, function(e, pos, light)
      i = i + 1
      local ring = 6 + (i % 7) * 4                 -- concentric rings
      local phase = i * 2.39996                    -- golden angle
      local a = phase + t * speed * (1 + (i % 3) * 0.3)
      pos.x = math.cos(a) * ring
      pos.z = math.sin(a) * ring
      pos.y = 1.0 + math.sin(t * 0.8 + phase) * 0.6
    end)
  end,
}
