-- rozOS: Star catcher - a small game in Lua (help luagame)
--   luagame /usr/share/games/stars.lua
-- Copy it, change it, run your copy:  cp /usr/share/games/stars.lua /home; luagame /home/stars.lua
-- Move the basket with the mouse or the arrows; catch the stars, miss 5 and the game ends.

local W, H = 640, 480
local basket = { x = 300, w = 90 }
local stars, score, missed, spawn, over = {}, 0, 0, 0, false
local best = game.score_load("stars")
local sparks = game.particles(400)
local apple = game.image("snake-apple.png")          -- pictures work the same as in C

local function new_star()
  local gold = game.rand_int(1, 6) == 1              -- a rare gold one is worth 5
  stars[#stars + 1] = { x = game.rand_int(20, W - 20), y = -20,
                        vy = game.rand_float(120, 220) + score * 2, gold = gold,
                        spin = game.rand_float(0, 6.28) }
end

local function restart()
  stars, score, missed, spawn, over = {}, 0, 0, 0, false
end

local function update(dt)
  if over then
    if game.key_pressed("enter") or game.key_pressed("space") or game.mouse_pressed() then restart() end
    if game.key_pressed("escape") then game.quit() end
    return
  end
  if game.key_pressed("escape") then game.quit() end
  local mx = game.mouse()
  if game.key_down("left") or game.key_down("a") then basket.x = basket.x - 400 * dt end
  if game.key_down("right") or game.key_down("d") then basket.x = basket.x + 400 * dt end
  if game.mouse_down() then basket.x = mx - basket.w / 2 end
  basket.x = game.clamp(basket.x, 0, W - basket.w)

  spawn = spawn - dt
  if spawn <= 0 then new_star(); spawn = math.max(0.25, 0.9 - score * 0.01) end
  for i = #stars, 1, -1 do
    local s = stars[i]
    s.y = s.y + s.vy * dt
    s.spin = s.spin + dt * 3
    if game.overlap(basket.x, H - 50, basket.w, 20, s.x - 10, s.y - 10, 20, 20) then
      score = score + (s.gold and 5 or 1)
      sparks:burst(s.x, s.y, 16, { color = s.gold and game.rgb(255, 200, 40) or game.rgb(180, 220, 255), speed = 200 })
      table.remove(stars, i)
    elseif s.y > H + 20 then
      missed = missed + 1
      table.remove(stars, i)
      if missed >= 5 then
        over = true
        game.score_save("stars", score)
        best = game.score_load("stars")
      end
    end
  end
  sparks:update(dt)
end

local function draw()
  game.gradient(0, 0, W, H, game.rgb(10, 14, 40), game.rgb(40, 20, 70))
  for _, s in ipairs(stars) do
    local r = 10 + math.sin(s.spin) * 2
    game.circle(s.x, s.y, r, s.gold and game.rgb(255, 200, 40) or game.rgb(220, 230, 255))
    game.circle(s.x - 3, s.y - 3, 3, game.rgb(255, 255, 255))
  end
  sparks:draw()
  game.fill_rect(basket.x, H - 50, basket.w, 20, game.rgb(200, 120, 60))
  game.fill_rect(basket.x + 4, H - 46, basket.w - 8, 4, game.rgb(240, 180, 120))
  if apple then apple:draw(10, 8) end
  game.ttf(22, 48, 10, "Score " .. score, game.rgb(255, 255, 255))
  game.ttf(22, W - 220, 10, "Missed " .. missed .. " / 5", game.rgb(255, 160, 160))
  if over then
    game.fill_rect(0, 0, W, H, game.rgb(0, 0, 0, 150))
    game.ttf_center(52, 160, "Game over", game.rgb(255, 120, 100))
    game.ttf_center(26, 240, "Score " .. score .. "   Best " .. best, game.rgb(255, 230, 150))
    game.ttf_center(18, 300, "Enter to play again, Esc quits", game.rgb(220, 220, 220))
  end
end

game.run{ title = "Star catcher (Lua)", update = update, draw = draw }
