# EBOOT media

The two files in an `EBOOT.PBP` that are not pictures, made conformant.
Both go beside `ICON0.PNG` and `PIC1.PNG` into `pack-pbp`.

```
sh make-icon1.sh demo.mp4 ICON1.PMF          # 144x80 PSMF, 6 s, no sound
sh make-snd0.sh  theme.wav SND0.AT3          # ATRAC3 132 kbps, 18 s at most
```

Each takes a start offset in seconds as a third argument, and `DURATION`
in the environment sets the length. Needs ffmpeg and a C compiler; the
sound script needs `atracdenc` as well, which is `atracdenc-git` in the
AUR or a cmake build of https://github.com/dcherednik/atracdenc.

What to expect: `ICON1.PMF` comes out between 30 and 150 KB for six
seconds at 30 frames a second, depending on how much moves, and a fifth
less with `FPS=15`; `SND0.AT3` is 16.5 KB a second, so 300 KB for the full
eighteen, or half that with `BITRATE=66`.
