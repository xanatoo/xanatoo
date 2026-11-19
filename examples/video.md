# Video

A link to a video file plays it in the **Media** panel, and the page stays
where it is. The picture sits above the player's controls:

- click the picture, or the play button, to pause and go on,
- click or drag the bar to go to that point,
- back 10 s, forward 30 s, stop, and the *Vol* slider as for [sound](sound.md).

The panel grows to fit the video when it floats. Drag its edges for a
bigger picture, maximize it, or dock it at a window edge. Closing the panel
stops the video.

## Try it

- [Test pattern](video/pattern.mp4) (MP4: H.264 and AAC)
- [Mandelbrot zoom](video/mandelbrot.webm) (WebM: VP9 and Opus)

Both clips were synthesised for this page.

## Formats

The desktop builds decode through **FFmpeg**: MP4, WebM, Matroska,
QuickTime, Ogg and AVI files, and audio such as AAC (`.m4a`) and Opus.
Remote `http(s)` videos stream: they start before the download ends.
Android uses the phone's own decoders (*MediaCodec*), so what plays there
depends on the device. H.264 and AAC in MP4 work everywhere.

Builds without a decoder (Windows, the browser) leave these links to an
external player, such as the *Watch with mpv* links on video pages.
