# marks

The shell's glyph set: the four face buttons, START, SELECT, L, R and HOME,
the badges a list row can carry, and one icon per tab. One RGBA PNG per mark,
white strokes with the anti-aliasing in the alpha, drawn at the size it is
shown at — 11x11 for a face button, 15x15 for a tab, 17x7 for the pills.

The rules, taken from the XMB's own foreground glyphs: strokes are two pixels
and never one, terminals are rounded, the colour lives in the ground and never
in the glyph, and the highlighted state is the same glyph with a wider softer
light behind it rather than a brighter glyph. `gui/marks.c` adds the dark copy
one pixel down and right that every XMB glyph ships a blurred twin for.

To change one, edit its PNG at 1x and regenerate the table the build reads:

    python3 tools/marks/embed.py     # from app/, rewrites gui/marks_data.h

`gui/marks_data.h` is committed, so building needs no Python. A new mark goes
into `ORDER` in the generator and `enum mark` in `gui/marks.h`, in the same
place in both.
