# Sound

A link to an audio file plays it, and the page stays where it is. The
player sits at the left of the status line:

- the first button plays and pauses,
- clicking the bar goes to that point of the file,
- the square button stops and closes the player.

**Go → Sound** has the same controls, plus *Back 10 s* and *Forward 30 s*.

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
