-- the name, played out on the coach's board: a shot off the underside of the crossbar and straight down over
-- the line. the board writes itself once, then the shot goes in on a loop and the lamp goes on
local P = kiln.palette
local B = kiln.use("board")
local f = string.format

local W, H = 1200, 440
local b = B.board(W, H, "bardown: a shot drawn on a coach's board goes off the crossbar and straight down into the net")
local doc = b.doc

-- the goal line printed on the board
local GL = 362
b:stroke(B.line(30, GL, W - 30, GL, 0), P.ghost_red, 4, { draw = false })

b:write(64, 214, "bardown", 132, P.chalk, "marker", "start", -3)
b:stroke(B.line(78, 246, 520, 236, 1.2, 6), P.yellow, 4)
b:write(80, 296, "NHL 14 for the Xbox 360, running as a native program", 28, P.dim)
b:write(80, 330, "on Linux and Windows, with no emulation underneath", 28, P.dim)

-- the net from the front: posts, the crossbar, mesh scribbled in light
local NX0, NX1, BAR = 850, 1100, 150
-- diagonal both ways, the diamond mesh a net has, cut off at the frame by hand rather than exactly
local mesh = {}
local mh = GL - BAR - 14
for k = -mh, NX1 - NX0, 26 do
  for _, dir in ipairs({ 1, -1 }) do
    local x0 = dir == 1 and NX0 + k or NX1 - k
    local ya, yb, xa, xb = BAR + 8, GL - 6, x0, x0 + dir * mh
    if dir == 1 then
      if xa < NX0 + 6 then ya, xa = ya + (NX0 + 6 - xa), NX0 + 6 end
      if xb > NX1 - 6 then yb, xb = yb - (xb - NX1 + 6), NX1 - 6 end
    else
      if xa > NX1 - 6 then ya, xa = ya + (xa - NX1 + 6), NX1 - 6 end
      if xb < NX0 + 6 then yb, xb = yb - (NX0 + 6 - xb), NX0 + 6 end
    end
    if yb - ya > 12 then mesh[#mesh + 1] = B.line(xa, ya, xb, yb, 0.5) end
  end
end
b:stroke(mesh, P.chalk, 1.4, { together = true, opacity = 0.28 })
b:stroke({ B.line(NX0, GL + 4, NX0, BAR, 0.8, 3), B.line(NX0 - 4, BAR, NX1 + 4, BAR + 1, 0.8, 3),
  B.line(NX1, BAR + 1, NX1, GL + 4, 0.8, 3) }, P.red, 7, { together = true })

-- the lamp behind the net
local LX, LY = 1140, 96
b:stroke(B.ellipse(LX, LY, 16, 16, 0.05), P.red, 3)
b:stroke(B.line(LX, LY + 16, LX, BAR - 6, 0.4), P.dim, 2)

-- the shot, on its own loop after the board has drawn: in off the bar
local HIT_X = 990
local fly = f("M640 %d Q860 330 %d %d", GL - 8, HIT_X, BAR + 9)
local drop = B.line(HIT_X, BAR + 9, HIT_X + 4, GL - 10, 0.5)
doc:add(f('<path class="shot fly" d="%s" fill="none" stroke="%s" stroke-width="3.4" stroke-linecap="round" '
  .. 'pathLength="1"/>', fly, P.yellow))
doc:add(f('<path class="shot drop" d="%s" fill="none" stroke="%s" stroke-width="3.4" stroke-linecap="round" '
  .. 'pathLength="1"/>', drop, P.yellow))
local sparks = {}
for _, a in ipairs({ -160, -120, -60, -20 }) do
  local r = math.rad(a)
  sparks[#sparks + 1] = B.line(HIT_X + 12 * math.cos(r), BAR - 4 + 12 * math.sin(r), HIT_X + 30 * math.cos(r),
    BAR - 4 + 30 * math.sin(r), 0.3)
end
for _, d in ipairs(sparks) do
  doc:add(f('<path class="shot ding" d="%s" fill="none" stroke="%s" stroke-width="3" stroke-linecap="round"/>', d,
    P.yellow))
end
doc:text(HIT_X + 30, BAR - 30, "ding", 28, 400, P.yellow, "start", ' class="shot ding"', "Patrick Hand")
local puck = B.ellipse(HIT_X + 4, GL - 6, 16, 6, 0.03, 0.02) .. "Z"
doc:add(f('<path class="shot puck" d="%s" fill="%s"/>', puck, P.chalk))
doc:add(f('<path class="shot lamp" d="%s" fill="%s"/>', B.ellipse(LX, LY, 13, 13, 0.03, 0.02) .. "Z", P.red))

-- 7 s a shot: the flight draws, the bar rings, it drops, the lamp blinks, then the board's wiped for the next.
-- with reduced motion it's the finished play, lamp on
local start = b.order * 0.09 + 0.6
doc:def("shot", "<style>"
  .. ".shot{opacity:1}.fly,.drop{stroke-dashoffset:0}"
  .. "@media (prefers-reduced-motion:no-preference){"
  .. f(".shot{animation:7s linear %.2fs infinite both}", start)
  .. ".fly{animation-name:fly}.drop{animation-name:drop}.ding{animation-name:ding}.puck{animation-name:puck}"
  .. ".lamp{animation-name:lamp}"
  .. "@keyframes fly{0%{opacity:0;stroke-dasharray:0 1}1%{opacity:1}22%{stroke-dasharray:1 0}90%{opacity:1;stroke-dasharray:1 0}"
  .. "97%,100%{opacity:0;stroke-dasharray:1 0}}"
  .. "@keyframes drop{0%,22%{opacity:0;stroke-dasharray:0 1}23%{opacity:1;stroke-dasharray:0 1}31%,90%{opacity:1;stroke-dasharray:1 0}"
  .. "97%,100%{opacity:0;stroke-dasharray:1 0}}"
  .. "@keyframes ding{0%,21%{opacity:0}23%,38%{opacity:1}46%,100%{opacity:0}}"
  .. "@keyframes puck{0%,30%{opacity:0}32%,90%{opacity:1}97%,100%{opacity:0}}"
  .. "@keyframes lamp{0%,33%{opacity:0}35%,44%{opacity:1}46%,54%{opacity:0}56%,65%{opacity:1}67%,75%{opacity:0}"
  .. "77%,90%{opacity:1}97%,100%{opacity:0}}}</style>")

return b:finish(0.09)
