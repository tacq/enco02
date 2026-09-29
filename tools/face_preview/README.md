# Face preview (Mac, browser)

Check visual changes to the robot face — ear twitches, hair wind, blinks, mouth, expressions,
petals — on the Mac before flashing.

```
python3 tools/face_preview/export.py     # after every asset rebuild / display.cpp tuning change
open tools/face_preview/index.html       # opens from disk, no server, no network
```

- `export.py` reads the generated `firmware/main/face_assets_*.c` (the exact bytes that get
  flashed) and the ear tuning tables in `firmware/main/display.cpp`, and writes `data/*.js`
  (git-ignored).
- `face_engine.js` is a function-by-function port of the animation code in `display.cpp`:
  the same RGB565 pixels, fixed-point warps (`WarpEarsChunk`, `WarpHairChunk` / `WarpSpan`),
  springs (`UpdateEars`), and 80 ms face tick and 50 ms motion tick. **If you change that code
  in `display.cpp`, change the matching function here too.**
- `snapshot.js` renders frames headlessly to PNG with node:
  `node tools/face_preview/snapshot.js fox out/ shake 0,60,130,1000`.

The page can trigger every predefined animation: 抖耳朵 and the idle ear scripts (with mirror
and gain), each expression (with its ear pose and accent), speaking, sleepy, and ambient
idle/listening faces. You can also switch characters, slow the clock down to 0.1×, pause and
step 50 ms at a time. Debug overlays show the ear rig weights and the hair wind bands, and you
can turn the warps off to compare against the un-warped frame.

Not simulated: the status bar is only approximated (set its height to match), there are no
alert or timer cards, and the device's frame rate (~10–11 fps under load) isn't copied — the
preview renders at the browser's frame rate, while the animation state still advances on the
firmware's timers.
