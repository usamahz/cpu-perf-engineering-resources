# Banner

`misc/cover.avif` at the top of the README is Hokusai's *Under the Wave off
Kanagawa* rendered as a grid of hex glyphs coloured by the pigment under
each cell, the same treatment the reference list gives Monet's Water Lilies.

Source: the Metropolitan Museum of Art's open-access scan, object 45434
(JP1847), file `DP130155.jpg`, 3859x2594, released under CC0; the crop
starts right of the cartouche so the banner carries no text.

Render (Pillow, Menlo from macOS):

    uv run --with pillow python3 render.py --out cover.png \
      --crop 0.095,0.03,1.0,0.66 --cell 10 --base 0.40 --base-hi 0.56 \
      --gain 1.02 --sat 1.25 --lift 4 --jitter 0.12

then encode with `pillow-avif-plugin` at quality 64. The renderer expects
the Met file beside it as `great-wave-met-DP130155.jpg`.

Chosen by a three-judge panel over three other treatments (a bold memory
dump, a dark night build, an assembly listing on the foam); the judges'
one fix, more glyph contrast on the cream paper and no cartouche, is
applied above.
