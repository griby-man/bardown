-- how bardown works, drawn as a play on the coach's board. the disc is the puck, the recompiler turns it into
-- C++ once, and three skaters carry it to the net (your screen): the game's own code, the Xbox underneath it,
-- and the renderer. solid lines skate, dashes are passes
local P = kiln.palette
local B = kiln.use("board")

local W, H = 1200, 640
local b = B.board(W, H, "how bardown works, drawn as a hockey play on a coach's board")

-- a pass: short strokes along the line, a gap between each
local function dashes(x0, y0, x1, y1, n)
  local out = {}
  for i = 0, n - 1 do
    local t0, t1 = i / n, (i + 0.55) / n
    out[#out + 1] = B.line(x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0, x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1, 0.5)
  end
  return out
end

-- an X, two strokes, the second a little shorter the way it always is
local function skater(x, y)
  b:stroke({ B.line(x - 13, y - 14, x + 13, y + 13, 0.7), B.line(x + 12, y - 13, x - 11, y + 12, 0.7) }, P.chalk, 4,
    { together = true })
end

-- the rink printed on the board, faint, behind the play
local function printed(d, c, w) b:stroke(d, c, w, { draw = false }) end
printed(B.rounded(40, 110, 1120, 470, 120, 0.2), P.ghost, 4)
printed(B.line(1080, 112, 1080, 578, 0), P.ghost, 3)
printed(B.line(560, 112, 560, 578, 0), P.ghost_blue, 6)
for _, cy in ipairs({ 230, 460 }) do printed(B.ellipse(930, cy, 62, 62, 0, 0), P.ghost, 2.5) end
printed(B.ellipse(1080, 345, 40, 48, 0, 0), P.ghost_blue, 2.5)

b:write(60, 78, "how it works", 44, P.chalk, "marker", "start", -1)

-- the puck: your disc
b:blob(B.ellipse(110, 345, 20, 14, 0.04, 0.02) .. "Z", P.chalk)
b:write(70, 395, "your disc", 26, P.chalk)
b:write(70, 418, "NHL 14, the 360 one,", 19, P.dim)
b:write(70, 438, "no title update", 19, P.dim)

-- the recompiler
b:stroke(B.arrow(140, 343, 238, 341, 0.05), P.chalk, 3.4)
b:stroke(B.poly({ { 250, 295 }, { 468, 290 }, { 472, 392 }, { 246, 396 } }, 1.4, 6), P.chalk, 3.4)
b:write(268, 336, "recompiler", 30, P.chalk, "marker")
b:write(268, 366, "PowerPC to C++, once,", 19, P.dim)
b:write(268, 385, "before you ever play", 19, P.dim)

-- three skaters off the recompiler
local players = {
  { 640, 190, "the game", { "EA's own code, translated,", "nothing remade" } },
  { 640, 345, "the Xbox under it", { "the ReXGlue runtime: threads,", "files, sound, pads" } },
  { 640, 500, "the picture", { "bardown's renderer: the game's", "D3D calls drawn with Vulkan" } },
}
for _, p in ipairs(players) do
  local px, py = p[1], p[2]
  b:stroke(B.arrow(478, 343 + (py - 345) * 0.25, px - 22, py, py < 345 and -0.08 or 0.08), P.chalk, 3)
  skater(px, py)
  b:write(px + 26, py - 12, p[3], 26, P.chalk)
  for i, n in ipairs(p[4]) do b:write(px + 26, py + 12 + (i - 1) * 20, n, 19, P.dim) end
end

-- passes: the game hands its draws to the renderer
b:stroke(dashes(622, 214, 622, 474, 11), P.blue, 2.8, { together = true })
b:write(608, 300, "draws", 21, P.blue, "hand", "start", -90)
-- everyone to the net
b:stroke(B.arrow(930, 500, 1052, 380, -0.12), P.chalk, 3.4)
b:stroke(B.arrow(900, 190, 1050, 310, 0.14), P.chalk, 3.4)
local net = { { 1080, 318 }, { 1112, 322 }, { 1112, 370 }, { 1080, 374 } }
b:stroke(B.open(B.poly(net, 0.8, 2)), P.red, 3.4, { together = true })
b:write(1006, 432, "your screen", 26, P.chalk, "hand", "start", -4)

-- the bit that matters, circled
b:write(250, 500, "no emulation,", 28, P.red, "marker", "start", -3)
b:write(262, 534, "CPU or GPU", 28, P.red, "marker", "start", -3)
b:stroke(B.ellipse(350, 510, 125, 48, 0.05, 0.18), P.red, 3)

return b:finish(0.12)
