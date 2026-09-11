# marks

The shell's glyph set: the four face buttons, START, SELECT, L, R and HOME,
the badges a list row can carry, and one icon per tab.

The sources are the SVGs in `src/`. Everything else in this directory, and
`gui/marks_data.h`, is generated from them and committed.

## Why they are drawings and not pixel art

The XMB's own foreground glyphs are not hand-pixelled. `tex_cross.png` in
`system_plugin_fg` is a thirteen-pixel texture holding an eleven-pixel shape
drawn as a vector with a two-pixel stroke and rounded terminals, rendered
with the anti-aliasing left in the alpha; `tex_cross_shadow.png` beside it is
twenty-one pixels of the same shape, blurred. That is why the originals look
smooth on a 480x272 screen and why a set drawn a pixel at a time looks like a
spreadsheet next to them.

So ours are drawn the same way. The rules, measured off the originals:

- the shape is eleven pixels for a face button, fifteen for a tab, and sits
  one pixel inside its cell so the anti-aliasing has somewhere to land;
- strokes are two pixels and never one, terminals and joins are round;
- an axis-aligned edge is put on a whole pixel, so a stroke of two covers
  exactly two and does not come out as three greys;
- the colour lives in the ground and never in the glyph: every source is
  white on nothing, and `gui/marks.c` tints it;
- the highlighted state is the same glyph with a wider softer light behind
  it, never a brighter glyph.

## How they reach the build

Each source rasterises to two PNGs. `<name>.png` is the glyph at its own
size, white, coverage in the alpha. `<name>_shadow.png` is the same shape
three pixels larger on each side, black, and blurred -- the twin every XMB
glyph ships, because a white shape on a photograph loses its edge wherever
the photograph is pale.

Both go into one sheet. `gui/marks_data.h` carries that sheet as one byte of
coverage a pixel plus a table saying where each mark's two cells are, and
`gui/marks.c` opens it out into a texture at first use and draws a mark as
two sprites: the shadow cell, then the glyph cell.

## Regenerating

    sh tools/marks/render.sh        # src/*.svg -> the PNGs here
    python3 tools/marks/embed.py    # the PNGs -> gui/marks_data.h

Both from `app/`. The first needs `rsvg-convert` and ImageMagick; the second
needs nothing but a stock Python, and the build needs neither, because the
PNGs and the header are committed.

A new mark is a new SVG in `src/`, a name in `ORDER` in the generator and a
member of `enum mark` in `gui/marks.h`, in the same place in both lists. The
size the shell lays out with is the SVG's own size less the one-pixel margin,
so a mark is resized by resizing its `viewBox` and its drawing together.
