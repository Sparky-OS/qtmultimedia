# Native stereo crop regression

`tst_qquickvideocrop` checks native samples, logical placement and the exact
presenter scheduling state without a compositor. Run the placement checks at
`QT_SCALE_FACTOR=1`, `1.25` and `2`; integer logical placement is independent of
output DPR. Fractional placement, transforms, partial opacity and clipping fall
back to the scene graph.

`tst_qquickvideocroplayer` requires an isolated Wayland stereo compositor for
its native submission cases. Run each case in a fresh session with an observing
adapter, setting `QT_STEREO_TEST_BUFFER_DIR` to its output directory. Without
that adapter these cases skip; item properties alone do not prove publication.
The edition proof rig's `wlrec.c` is one such adapter. It must record each actual
declared SHM buffer in `log.jsonl`, with `file`, `width`, `height`, `stride`,
`format` (1 = XRGB8888), `content` (3 = full SBS), and `destination` (one-view
logical width/height), plus the raw BGRA payload. The test validates every RGB
sample and the exact submission count. Bound the recorder to 24 submissions.

`convertingArrivalsAndInvalidation` uses a test QAbstractVideoBuffer to pause the
actual converter while replacements, resize and clear arrive.
`busyPausedResize` additionally requires a test-only release adapter: delay
wl_buffer.release callbacks until `CROP_RELEASE_GATE` exists, then replay them
on the original dispatching thread. The test creates that file after three
buffers are held and a fourth paused resize is pending. No new decoded frame
repairs the resize. The adapter changes no production code or protocol.

`ordinary2D` also runs with isolated X11/GLX software OpenGL. Compare its nonblank
`ordinary2D.png` (written under `CROP_OUT`) against the exact unpatched package,
with identical Qt, scene graph, raster and renderer. Native submission tests do
not establish compositor pixels, installed wallpaper behavior or HDR output.
