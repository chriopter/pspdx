# sweep.trace

A recorded run of the entropy sweep: one entry per frame, `{ u8 lx, u8 ly,
u16 buttons }`, 1608 samples of a human actually moving the stick.

To replay it instead of collecting input -- useful for testing the animation or
recording a video without fighting the window manager for the keyboard:

```sh
cp testdata/sweep.trace ~/.config/ppsspp/PSPDX.TRACE
touch ~/.config/ppsspp/PSPDX.REPLAY
```

A replayed run marks itself on screen and does not overwrite the stored seed:
the path is public, so it is a development aid, not a source of randomness.
Remove `PSPDX.REPLAY` to go back to collecting.
