#!/usr/bin/env python3
# how bardown works, drawn as a play on the coach's whiteboard: docs/images/architecture.svg. the disc is the
# puck, the recompiler turns it into C++ once, and three skaters carry it to the net (your screen): the game's
# own code, the Xbox underneath it, and the renderer. solid lines skate, dashes are passes
#   tools/play.py
from pathlib import Path

from sketch import BLACK, BLUE, GHOST, RED, Board

ROOT = Path(__file__).resolve().parent.parent
W, H = 1200, 640
GREY = "#6b7178"


def dashes(h, x0, y0, x1, y1, n=9):
    """a pass: short strokes along the line, a gap between each"""
    out = []
    for i in range(n):
        t0, t1 = i / n, (i + 0.55) / n
        out.append(h.line(x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0, x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1,
                          amp=0.5))
    return out


def skater(b, x, y):
    """an X, two strokes, the second one a little shorter the way it always is"""
    h = b.hand
    b.stroke([h.line(x - 13, y - 14, x + 13, y + 13, amp=0.7), h.line(x + 12, y - 13, x - 11, y + 12, amp=0.7)],
             BLACK, 4, together=True)


def main():
    b = Board(W, H, "how bardown works, drawn as a hockey play on a whiteboard", seed=91)
    h = b.hand

    # the rink printed on the board, faint, behind the play
    b.stroke(h.rounded(40, 110, 1120, 470, 120, amp=0.2), GHOST, 4, draw=False)
    b.stroke(h.line(1080, 112, 1080, 578, amp=0), GHOST, 3, draw=False)
    b.stroke(h.line(560, 112, 560, 578, amp=0), "#d4dfee", 6, draw=False)
    for cy in (230, 460):
        b.stroke(h.ellipse(930, cy, 62, 62, amp=0, lap=0), GHOST, 2.5, draw=False)
    b.stroke(h.ellipse(1080, 345, 40, 48, amp=0, lap=0), "#dbe8f3", 2.5, draw=False)

    b.write(60, 78, "how it works", 44, BLACK, "marker", rotate=-1)

    # the puck: your disc
    b.blob(h.ellipse(110, 345, 20, 14, amp=0.04, lap=0.02) + "Z", BLACK)
    b.write(70, 395, "your disc", 26, BLACK)
    b.write(70, 418, "NHL 14, the 360 one,", 19, GREY)
    b.write(70, 438, "no title update", 19, GREY)

    # the recompiler
    b.stroke(h.arrow(140, 343, 238, 341, bend=0.05), BLACK, 3.4)
    b.stroke(h.poly([(250, 295), (468, 290), (472, 392), (246, 396)], amp=1.4, overshoot=6), BLACK, 3.4)
    b.write(268, 336, "recompiler", 30, BLACK, "marker")
    b.write(268, 366, "PowerPC to C++, once,", 19, GREY)
    b.write(268, 385, "before you ever play", 19, GREY)

    # three skaters off the recompiler
    players = [
        (640, 190, "the game", ["EA's own code, translated,", "nothing remade"]),
        (640, 345, "the Xbox under it", ["the ReXGlue runtime: threads,", "files, sound, pads"]),
        (640, 500, "the picture", ["bardown's renderer: the game's", "D3D calls drawn with Vulkan"]),
    ]
    for px, py, name, notes in players:
        b.stroke(h.arrow(478, 343 + (py - 345) * 0.25, px - 22, py, bend=-0.08 if py < 345 else 0.08), BLACK, 3)
        skater(b, px, py)
        b.write(px + 26, py - 12, name, 26, BLACK)
        for i, n in enumerate(notes):
            b.write(px + 26, py + 12 + i * 20, n, 19, GREY)

    # passes: the game hands its draws to the renderer, and leans on the Xbox for everything else
    b.stroke(dashes(h, 622, 214, 622, 474, 11), BLUE, 2.8, together=True)
    b.write(608, 300, "draws", 21, BLUE, rotate=-90)
    # everyone to the net
    b.stroke(h.arrow(930, 500, 1052, 380, bend=-0.12), BLACK, 3.4)
    b.stroke(h.arrow(900, 190, 1050, 310, bend=0.14), BLACK, 3.4)
    net = [(1080, 318), (1112, 322), (1112, 370), (1080, 374)]
    b.stroke(h.poly(net, amp=0.8, overshoot=2)[:3], RED, 3.4, together=True)
    b.write(1006, 432, "your screen", 26, BLACK, rotate=-4)

    # the bit that matters, circled
    b.write(250, 500, "no emulation,", 28, RED, "marker", rotate=-3)
    b.write(262, 534, "CPU or GPU", 28, RED, "marker", rotate=-3)
    b.stroke(h.ellipse(350, 510, 125, 48, amp=0.05, lap=0.18), RED, 3)

    out = ROOT / "docs/images/architecture.svg"
    out.write_text(b.render(step=0.12))
    print("wrote", out.relative_to(ROOT))


if __name__ == "__main__":
    main()
