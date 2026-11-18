-- what every picture draws with: a coach's board, marker lines that wobble like a hand did them (the same
-- seed draws the same wobble), and strokes that draw on in order the first time the picture shows
local P = kiln.palette
local f = string.format
local M = {}

local MARKER, HAND = "Permanent Marker", "Patrick Hand"

local function U(a, b) return a + (b - a) * kiln.random() end
M.U = U

-- points into a path through their midpoints, so a hand line has no corners it didn't mean
local function smooth(pts)
  if #pts < 3 then
    return f("M%.1f %.1fL%.1f %.1f", pts[1][1], pts[1][2], pts[#pts][1], pts[#pts][2])
  end
  local d = { f("M%.1f %.1f", pts[1][1], pts[1][2]) }
  for i = 2, #pts - 1 do
    local x0, y0, x1, y1 = pts[i][1], pts[i][2], pts[i + 1][1], pts[i + 1][2]
    d[#d + 1] = f("Q%.1f %.1f %.1f %.1f", x0, y0, (x0 + x1) / 2, (y0 + y1) / 2)
  end
  d[#d + 1] = f("L%.1f %.1f", pts[#pts][1], pts[#pts][2])
  return table.concat(d)
end
M.smooth = smooth

-- two slow sines with random phase: drift, not jitter
local function wave(amp)
  local a, b = U(0, 6.3), U(0, 6.3)
  local f1, f2 = U(0.6, 1.4), U(1.8, 3.2)
  return function(t) return amp * (0.7 * math.sin(f1 * math.pi * t + a) + 0.3 * math.sin(f2 * math.pi * t + b)) end
end

-- a straight-ish stroke; overshoot runs it a little past its ends, like a corner drawn fast
function M.line(x0, y0, x1, y1, amp, overshoot)
  amp, overshoot = amp or 1.6, overshoot or 0
  local dx, dy = x1 - x0, y1 - y0
  local n = math.sqrt(dx * dx + dy * dy)
  if n == 0 then n = 1 end
  local ux, uy = dx / n, dy / n
  local o0, o1 = overshoot * U(0.2, 1), overshoot * U(0.2, 1)
  x0, y0, x1, y1 = x0 - ux * o0, y0 - uy * o0, x1 + ux * o1, y1 + uy * o1
  dx, dy = x1 - x0, y1 - y0
  local w = wave(amp)
  local steps = math.max(3, math.floor(n / 40))
  local pts = {}
  for i = 0, steps do
    local t = i / steps
    local off = w(t) * math.sqrt(math.sin(math.pi * t))
    pts[#pts + 1] = { x0 + dx * t - uy * off, y0 + dy * t + ux * off }
  end
  return smooth(pts)
end

-- a closed shape as separate strokes per side, the way you'd draw a box
function M.poly(pts, amp, overshoot)
  local out = {}
  for i = 1, #pts do
    local a, b = pts[i], pts[i % #pts + 1]
    out[#out + 1] = M.line(a[1], a[2], b[1], b[2], amp or 1.6, overshoot or 4)
  end
  return out
end

-- a net from above: the box without its open side
function M.open(sides)
  return { sides[1], sides[2], sides[3] }
end

-- one stroke round, a little off true, ending past where it started
function M.ellipse(cx, cy, rx, ry, amp, lap)
  amp, lap = amp or 0.06, lap or 0.12
  local start, w = U(0, 6.3), wave(amp)
  local pts = {}
  for i = 0, 28 do
    local t = i / 28 * (1 + lap)
    local a = start + t * 2 * math.pi
    local k = 1 + w(t)
    pts[#pts + 1] = { cx + rx * k * math.cos(a), cy + ry * k * math.sin(a) }
  end
  return smooth(pts)
end

-- a rink: straight sides and quarter-circle corners, one long stroke that doesn't quite close
function M.rounded(x, y, w, h, r, amp)
  amp = amp or 1.8
  local pts = {}
  local corners = { { x + w - r, y + r, -90 }, { x + w - r, y + h - r, 0 },
    { x + r, y + h - r, 90 }, { x + r, y + r, 180 } }
  for _, c in ipairs(corners) do
    for i = 0, 6 do
      local a = math.rad(c[3] + i * 15)
      pts[#pts + 1] = { c[1] + r * math.cos(a), c[2] + r * math.sin(a) }
    end
  end
  pts[#pts + 1] = pts[1]
  pts[#pts + 1] = { pts[1][1] + 14, pts[1][2] + 0.6 }
  local out = {}
  for i, p in ipairs(pts) do
    out[i] = { p[1] + U(-amp, amp) * 0.6, p[2] + U(-amp, amp) * 0.6 }
  end
  return smooth(out)
end

-- a bent arrow and its head as two quick ticks
function M.arrow(x0, y0, x1, y1, bend, head)
  bend, head = bend or 0.12, head or 13
  local dx, dy = x1 - x0, y1 - y0
  local cx, cy = (x0 + x1) / 2 - dy * bend, (y0 + y1) / 2 + dx * bend
  local pts = {}
  for i = 0, 12 do
    local t = i / 12
    pts[#pts + 1] = { (1 - t) ^ 2 * x0 + 2 * (1 - t) * t * cx + t * t * x1 + U(-0.8, 0.8),
      (1 - t) ^ 2 * y0 + 2 * (1 - t) * t * cy + t * t * y1 + U(-0.8, 0.8) }
  end
  local ang = math.atan2(y1 - pts[#pts - 2][2], x1 - pts[#pts - 2][1])
  local out = { smooth(pts) }
  for _, side in ipairs({ -1, 1 }) do
    local a = ang + math.pi - side * U(0.42, 0.55)
    out[#out + 1] = M.line(x1, y1, x1 + head * math.cos(a), y1 + head * math.sin(a), 0.6)
  end
  return out
end

-- back-and-forth marker filling a box, the way you color something in fast: slanted passes, lifted every so
-- often, uneven spacing, never quite reaching the edges
function M.scribble(x, y, w, h, gap, slant)
  gap, slant = gap or 9, slant or 16
  local out, pts, cx, up, zigs = {}, {}, x + U(1, 5), false, 0
  local lift = math.floor(U(10, 17))
  while cx < x + w - slant do
    if up then
      pts[#pts + 1] = f("%.1f %.1f", cx + slant + U(-2, 2), y + U(0, 8))
    else
      pts[#pts + 1] = f("%.1f %.1f", cx + U(-2, 2), y + h - U(0, 8))
    end
    cx = cx + gap * U(0.7, 1.3) / 2
    up = not up
    zigs = zigs + 1
    if zigs >= lift then
      out[#out + 1] = pts
      pts, zigs, lift = {}, 0, math.floor(U(10, 17))
      cx = cx + U(-4, 2)
    end
  end
  if #pts > 1 then out[#out + 1] = pts end
  local paths = {}
  for _, seg in ipairs(out) do
    if #seg > 1 then paths[#paths + 1] = "M" .. table.concat(seg, "L") end
  end
  return paths
end

-- the board: strokes go down in order and each one draws on in turn
local Board = {}
Board.__index = Board

function M.board(w, h, label)
  local doc = kiln.svg(w, label)
  doc:add(f('<rect x="3" y="3" width="%d" height="%d" rx="12" fill="%s" stroke="%s" stroke-width="6"/>',
    w - 6, h - 6, P.board, P.frame))
  return setmetatable({ doc = doc, w = w, h = h, order = 0 }, Board)
end

-- together: every path draws at the same moment (a hatch, a tally) instead of one after another.
-- draw = false is the rink printed on the board, there from the start
function Board:stroke(d, color, width, opt)
  opt = opt or {}
  local paths = type(d) == "string" and { d } or d
  local draw = opt.draw ~= false
  local n0 = self.order
  for _, p in ipairs(paths) do
    local cls = ""
    if draw then
      cls = f(' class="ink" style="--n:%d"', opt.together and n0 or self.order)
      if not opt.together then self.order = self.order + 1 end
    end
    local op = opt.opacity and f(' stroke-opacity="%.2f"', opt.opacity) or ""
    self.doc:add(f('<path d="%s" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round" '
      .. 'stroke-linejoin="round" pathLength="1"%s%s/>', p, color or P.chalk, width or 3.2, op, cls))
  end
  if draw and opt.together then self.order = self.order + 1 end
end

function Board:blob(d, color)
  self.doc:add(f('<path d="%s" fill="%s" class="ink fill" style="--n:%d"/>', d, color or P.chalk, self.order))
  self.order = self.order + 1
end

-- font: "marker" or "hand"
function Board:write(x, y, s, size, color, font, anchor, rotate)
  local extra = f(' class="ink word" style="--n:%d"', self.order)
  if rotate and rotate ~= 0 then extra = extra .. f(' transform="rotate(%.1f %.1f %.1f)"', rotate, x, y) end
  self.order = self.order + 1
  self.doc:text(x, y, s, size, 400, color or P.chalk, anchor or "start", extra, font == "marker" and MARKER or HAND)
end

function M.width(s, size, font)
  return kiln.width(s, size, 400, font == "marker" and MARKER or HAND)
end

-- the marker tray, and the draw-on: each stroke's dash runs out over .55 s, one after another. still for
-- anyone who asked for less motion
function Board:finish(step)
  local w, h = self.w, self.h
  self.doc:add(f('<rect x="%d" y="%d" width="%d" height="9" rx="4" fill="%s"/>', math.floor(w * 0.18), h - 12,
    math.floor(w * 0.64), P.frame))
  self.doc:def("ink", "<style>.ink{stroke-dasharray:1;stroke-dashoffset:0}"
    .. "@media (prefers-reduced-motion:no-preference){"
    .. f(".ink{stroke-dashoffset:1;animation:draw .55s ease-out forwards;animation-delay:calc(var(--n)*%ss)}", step)
    .. ".fill,.word{stroke-dashoffset:0;opacity:0;animation:show .3s ease-out forwards;"
    .. f("animation-delay:calc(var(--n)*%ss)}", step)
    .. "@keyframes draw{to{stroke-dashoffset:0}}@keyframes show{to{opacity:1}}}</style>")
  return self.doc:finish(h, P.deep, 12)
end

return M
