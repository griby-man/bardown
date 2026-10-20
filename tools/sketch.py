#!/usr/bin/env python3
# what play.py draws with: a whiteboard, marker lines that wobble like a hand did them, and two
# handwriting fonts embedded (cut down to the letters used) so the pictures look the same on any machine.
# fonts: Permanent Marker (Apache 2.0) and Patrick Hand (OFL), licences beside them in tools/fonts
import base64
import html
import io
import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONTS = {"marker": ROOT / "tools/fonts/PermanentMarker.ttf", "hand": ROOT / "tools/fonts/PatrickHand.ttf"}

# dry-erase colours, a bit faded the way they are on a board that's been wiped a hundred times
BOARD = "#fbfbf8"
GHOST = "#e4e7ea"      # the half rink printed on the board, and old marker that never wiped off
BLACK = "#22262b"
BLUE = "#2456a6"
RED = "#c63131"
GREEN = "#2f8a4c"
FRAME = "#b9bec4"

esc = html.escape


class Hand:
    """a marker in someone's hand: the same seed draws the same wobble every time"""

    def __init__(self, seed):
        self.r = random.Random(seed)

    def _wave(self, amp):
        # two slow sines with random phase: drift, not jitter
        a, b = self.r.uniform(0, 6.3), self.r.uniform(0, 6.3)
        f1, f2 = self.r.uniform(0.6, 1.4), self.r.uniform(1.8, 3.2)
        return lambda t: amp * (0.7 * math.sin(f1 * math.pi * t + a) + 0.3 * math.sin(f2 * math.pi * t + b))

    def line(self, x0, y0, x1, y1, amp=1.6, overshoot=0.0):
        """a straight-ish stroke; overshoot runs it a little past its ends, like a corner drawn fast"""
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy) or 1
        ux, uy = dx / n, dy / n
        x0, y0 = x0 - ux * overshoot * self.r.uniform(0.2, 1), y0 - uy * overshoot * self.r.uniform(0.2, 1)
        x1, y1 = x1 + ux * overshoot * self.r.uniform(0.2, 1), y1 + uy * overshoot * self.r.uniform(0.2, 1)
        wave = self._wave(amp)
        steps = max(3, int(n / 40))
        pts = []
        for i in range(steps + 1):
            t = i / steps
            off = wave(t) * math.sin(math.pi * t) ** 0.5
            pts.append((x0 + dx * t - uy * off, y0 + dy * t + ux * off))
        return smooth(pts)

    def poly(self, pts, amp=1.6, overshoot=4):
        """a closed shape as separate strokes per side, the way you'd draw a box"""
        return [self.line(*pts[i], *pts[(i + 1) % len(pts)], amp=amp, overshoot=overshoot) for i in range(len(pts))]

    def ellipse(self, cx, cy, rx, ry, amp=0.06, lap=0.12):
        """one stroke round, a little off true, ending past where it started"""
        start = self.r.uniform(0, 6.3)
        wave = self._wave(amp)
        pts = []
        steps = 28
        for i in range(steps + 1):
            t = i / steps * (1 + lap)
            a = start + t * 2 * math.pi
            k = 1 + wave(t)
            pts.append((cx + rx * k * math.cos(a), cy + ry * k * math.sin(a)))
        return smooth(pts)

    def rounded(self, x, y, w, h, r, amp=1.8):
        """a rink: straight sides and quarter-circle corners, drawn as one long stroke that doesn't quite close"""
        pts = []
        corners = [(x + w - r, y + r, -90), (x + w - r, y + h - r, 0), (x + r, y + h - r, 90), (x + r, y + r, 180)]
        for cx, cy, a0 in corners:
            for i in range(7):
                a = math.radians(a0 + i * 15)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        pts.append(pts[0])
        pts.append((pts[0][0] + 14, pts[0][1] + 0.6))
        out = []
        for i, (px, py) in enumerate(pts):
            j = self.r.uniform(-amp, amp) * 0.6
            out.append((px + j, py + self.r.uniform(-amp, amp) * 0.6))
        return smooth(out)

    def arrow(self, x0, y0, x1, y1, bend=0.12, head=13):
        """a bent arrow and its head as two quick ticks"""
        mx, my = (x0 + x1) / 2, (y0 + y1) / 2
        dx, dy = x1 - x0, y1 - y0
        cx, cy = mx - dy * bend, my + dx * bend
        pts = [((1 - t) ** 2 * x0 + 2 * (1 - t) * t * cx + t * t * x1,
                (1 - t) ** 2 * y0 + 2 * (1 - t) * t * cy + t * t * y1) for t in [i / 12 for i in range(13)]]
        pts = [(px + self.r.uniform(-0.8, 0.8), py + self.r.uniform(-0.8, 0.8)) for px, py in pts]
        tx, ty = x1 - (2 * (1 - 0.92) * (cx - x0) + 2 * 0.92 * (x1 - cx)) * 0.01, y1
        ang = math.atan2(y1 - pts[-3][1], x1 - pts[-3][0])
        heads = []
        for side in (-1, 1):
            a = ang + math.pi - side * self.r.uniform(0.42, 0.55)
            heads.append(self.line(x1, y1, x1 + head * math.cos(a), y1 + head * math.sin(a), amp=0.6))
        return [smooth(pts)] + heads

    def scribble(self, x, y, w, h, gap=9, slant=16):
        """back-and-forth marker filling a box, the way you colour something in fast: slanted passes joined by
        narrow turns, lifted every so often, uneven spacing, never quite reaching the edges"""
        out, pts, cx, up, zigs = [], [], x + self.r.uniform(1, 5), False, 0
        lift = self.r.randint(10, 16)
        while cx < x + w - slant:
            if up:
                pts.append((cx + slant + self.r.uniform(-2, 2), y + self.r.uniform(0, 8)))
            else:
                pts.append((cx + self.r.uniform(-2, 2), y + h - self.r.uniform(0, 8)))
            cx += gap * self.r.uniform(0.7, 1.3) / 2
            up = not up
            zigs += 1
            if zigs >= lift:
                out.append(pts)
                pts, zigs, lift = [], 0, self.r.randint(10, 16)
                cx += self.r.uniform(-4, 2)
        if len(pts) > 1:
            out.append(pts)
        return ["M" + "L".join("%.1f %.1f" % p for p in seg) for seg in out if len(seg) > 1]

    def hatch(self, x, y, w, h, gap=11, slant=0.55, amp=0.8):
        """quick diagonal scribble filling a box, clipped by hand rather than exactly"""
        out = []
        k = 0
        xs = -h * slant
        while xs < w:
            a = (x + xs, y + h)
            b = (x + xs + h * slant, y)
            # cut to the box, then let the ends wander a little
            (ax, ay), (bx, by) = clip(a, b, x, y, w, h)
            if bx - ax > 2:
                j = self.r.uniform(-2, 2)
                out.append(self.line(ax + j, ay, bx - j, by, amp=amp))
            xs += gap + self.r.uniform(-1.5, 1.5)
            k += 1
        return out


def clip(a, b, x, y, w, h):
    (ax, ay), (bx, by) = a, b
    def at(t):
        return ax + (bx - ax) * t, ay + (by - ay) * t
    t0, t1 = 0.0, 1.0
    for p, q in ((-(bx - ax), ax - x), (bx - ax, x + w - ax), (-(by - ay), ay - y), (by - ay, y + h - ay)):
        if p == 0:
            continue
        r = q / p
        if p < 0:
            t0 = max(t0, r)
        else:
            t1 = min(t1, r)
    return at(t0), at(min(t1, 1.0))


def smooth(pts):
    """points into a path through their midpoints, so a hand line has no corners it didn't mean"""
    if len(pts) < 3:
        return "M%.1f %.1fL%.1f %.1f" % (*pts[0], *pts[-1])
    d = ["M%.1f %.1f" % pts[0]]
    for (x0, y0), (x1, y1) in zip(pts[1:-1], pts[2:]):
        d.append("Q%.1f %.1f %.1f %.1f" % (x0, y0, (x0 + x1) / 2, (y0 + y1) / 2))
    d.append("L%.1f %.1f" % pts[-1])
    return "".join(d)


# --- fonts ---

_metrics = {}


def width(text, size, font="hand"):
    if font not in _metrics:
        from fontTools.ttLib import TTFont
        f = TTFont(FONTS[font])
        _metrics[font] = (f.getBestCmap(), f["hmtx"], f["head"].unitsPerEm)
    cmap, hmtx, upm = _metrics[font]
    return sum(hmtx[cmap.get(ord(c), cmap[ord("?")])][0] for c in text) * size / upm


def font_faces(used):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    rules = []
    for name, chars in used.items():
        if not chars:
            continue
        font = TTFont(FONTS[name])
        opts = subset.Options()
        opts.flavor = "woff2"
        opts.name_IDs = ["*"]
        sub = subset.Subsetter(opts)
        sub.populate(text="".join(sorted(chars)))
        sub.subset(font)
        buf = io.BytesIO()
        font.save(buf)
        rules.append("@font-face{font-family:'%s';src:url(data:font/woff2;base64,%s) format('woff2')}"
                     % (name, base64.b64encode(buf.getvalue()).decode()))
    return "".join(rules)


class Board:
    """the whiteboard: strokes go in order, and each one gets drawn on in turn the first time it's shown"""

    def __init__(self, w, h, label, seed):
        self.w, self.h, self.label = w, h, label
        self.hand = Hand(seed)
        self.out = []
        self.used = {"marker": set(), "hand": set()}
        self.order = 0

    def stroke(self, d, colour=BLACK, width_=3.2, draw=True, dash=None, opacity=1.0, together=False):
        """together: every path in d draws at the same moment (a hatch, a tally) instead of one after another"""
        paths = [d] if isinstance(d, str) else d
        n0 = self.order
        for p in paths:
            cls = ""
            if draw:
                cls = ' class="ink" style="--n:%d"' % (n0 if together else self.order)
                if not together:
                    self.order += 1
            extra = ' stroke-dasharray="%s"' % dash if dash else ""
            op = ' stroke-opacity="%.2f"' % opacity if opacity < 1 else ""
            self.out.append('<path d="%s" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round" '
                            'stroke-linejoin="round"%s%s pathLength="1"%s/>' % (p, colour, width_, extra, op, cls))
        if draw and together:
            self.order += 1

    def blob(self, d, colour=BLACK):
        self.out.append('<path d="%s" fill="%s" class="ink fill" style="--n:%d"/>' % (d, colour, self.order))
        self.order += 1

    def write(self, x, y, s, size, colour=BLACK, font="hand", anchor="start", rotate=0.0, draw=True):
        self.used[font].update(s)
        a = "" if anchor == "start" else ' text-anchor="%s"' % anchor
        r = ' transform="rotate(%.1f %.1f %.1f)"' % (rotate, x, y) if rotate else ""
        cls = ' class="ink word" style="--n:%d"' % self.order if draw else ""
        if draw:
            self.order += 1
        self.out.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="%.1f" fill="%s"%s%s%s>%s</text>'
                        % (x, y, font, size, colour, a, r, cls, esc(s)))

    def render(self, step=0.09):
        # the board itself: a plain white surface in a grey frame with the marker tray at the bottom
        frame = ('<rect x="3" y="3" width="%d" height="%d" rx="10" fill="%s" stroke="%s" stroke-width="6"/>'
                 % (self.w - 6, self.h - 6, BOARD, FRAME))
        tray = ('<rect x="%d" y="%d" width="%d" height="9" rx="4" fill="%s"/>'
                % (self.w * 0.18, self.h - 12, self.w * 0.64, FRAME))
        css = (font_faces(self.used) +
               ".ink{stroke-dasharray:1;stroke-dashoffset:0}"
               "@media (prefers-reduced-motion:no-preference){"
               ".ink{stroke-dashoffset:1;animation:draw .55s ease-out forwards;animation-delay:calc(var(--n)*%ss)}"
               ".fill,.word{stroke-dashoffset:0;opacity:0;animation:show .3s ease-out forwards;"
               "animation-delay:calc(var(--n)*%ss)}"
               "@keyframes draw{to{stroke-dashoffset:0}}@keyframes show{to{opacity:1}}}" % (step, step))
        return "\n".join([
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" role="img" '
            'aria-label="%s">' % (self.w, self.h, self.w, self.h, esc(self.label)),
            "<style>" + css + "</style>", frame, tray, *self.out, "</svg>", ""])
