# Sound

A link to an audio file plays it, and the page stays where it is. The
player opens in the **Media** panel (also under *Panels → Media*):

- click or drag the bar to go to that point of the file,
- the buttons go back 10 s, play and pause, go forward 30 s, and stop,
- the *Vol* slider sets the loudness.

Closing the panel stops the sound. Like other panels it can float or be
docked at a window edge. **Go → Sound** has the same transport controls.

## Try it

- [Chime](sound/chime.ogg) (Ogg Vorbis)
- [Rising steps](sound/steps.mp3) (MP3)
- [Bell](sound/bell.flac) (FLAC)

These three sounds were synthesised for this page.

## Formats

xanatoo decodes MP3, FLAC, Ogg Vorbis and WAV itself, with the
public-domain *dr_mp3*, *dr_flac*, *dr_wav* and *stb_vorbis*. Local files,
`file:` URLs and `http(s)` links all work. A downloaded file is loaded
completely before it starts.

Opus and AAC (`.m4a`), and video, still go to an external player such as
the *Watch with mpv* links on video pages.
