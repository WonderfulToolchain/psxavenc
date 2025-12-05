
# psxavenc

psxavenc is an open-source command-line tool for encoding audio and video data
into formats commonly used on the original PlayStation and PlayStation 2.

## Installation

Requirements:

- a recent version of FFmpeg libraries (`libavformat`, `libavcodec`,
  `libavutil`, `libswresample`, `libswscale`);
- a recent version of Meson.

```shell
$ meson setup build
$ meson compile -C build
$ meson install -C build
```

## Usage

Run `psxavenc -h`.

### Examples

Rescale a video file to ≤320x240 pixels (preserving aspect ratio) and encode it
into a 15 fps version 2 .str file with 37800 Hz 4-bit stereo audio and 2352-byte
sectors, meant to be played at 2x CD-ROM speed:

```shell
$ psxavenc -t strcd -v v2 -f 37800 -b 4 -c 2 -s 320x240 -r 15 -x 2 in.mp4 out.str
```

Convert a mono audio sample to 22050 Hz raw SPU-ADPCM data:

```shell
$ psxavenc -t spu -f 22050 in.ogg out.snd
```

Convert a stereo audio file to a 44100 Hz interleaved .vag file with 2048-byte
interleave and loop flags set at the end of each interleaved chunk:

```shell
$ psxavenc -t vagi -f 44100 -c 2 -L -i 2048 in.wav out.vag
```

## Supported output formats

The output format must be set using the `-t` option.

| Format  | Audio codec          | Audio channels | Video codec   | Sector size |
| :------ | :------------------- | :------------- | :------------ | :---------- |
| `xa`    | XA-ADPCM             | 1 or 2         |               | 2336 bytes  |
| `xacd`  | XA-ADPCM             | 1 or 2         |               | 2352 bytes  |
| `spu`   | SPU-ADPCM            | 1              |               |             |
| `vag`   | SPU-ADPCM            | 1              |               |             |
| `spui`  | SPU-ADPCM            | Any            |               |             |
| `vagi`  | SPU-ADPCM            | Any            |               |             |
| `str`   | XA-ADPCM (optional)  | 1 or 2         | BS v2/v3/v3dc | 2336 bytes  |
| `strcd` | XA-ADPCM (optional)  | 1 or 2         | BS v2/v3/v3dc | 2352 bytes  |
| `strv`  |                      |                | BS v2/v3/v3dc | 2048 bytes  |
| `sbs`   |                      |                | BS v2/v3/v3dc |             |

Notes:

- The `xa`, `xacd`, `str` and `strcd` formats will output files with 2336- or
  2352-byte CD-ROM sectors, containing the appropriate CD-XA subheaders and
  dummy EDC/ECC placeholders in addition to the actual sector data. Such files
  **cannot be added to a disc image as-is** and must instead be parsed by an
  authoring tool capable of rebuilding the EDC/ECC data (as it is dependent on
  the file's absolute location on the disc) and generating a Mode 2 CD-ROM image
  with "native" 2352-byte sectors.

- Similarly, files generated with `-t xa` or `-t xacd` **must be interleaved**
  **with other XA-ADPCM tracks or empty padding using an external tool** before
  they can be played.

- `vag` and `vagi` are similar to `spu` and `spui` respectively, but add a
  [.vag header](https://psx-spx.consoledev.net/cdromfileformats/#cdrom-file-audio-single-samples-vag-sony)
  at the beginning of the file. The header is always 48 bytes long for `vag`
  files, while in the case of `vagi` files it is padded to the size specified
  using the `-a` option (2048 bytes by default). The `vagi` format extends the
  header with the following fields:
  - the file's interleave size at offset `0x08-0x0B` (little endian);
  - the loop start offset in bytes-per-channel, if any, at offset `0x14-0x17`
    (big endian). *Note that this field is specific to psxavenc and not part of*
    *the standard interleaved .vag header*;
  - the file's channel count at offset `0x1E`. *This field is not part of the*
    *interleaved .vag header either, but can be found in other variants of the*
    *format.*

- The `spu` and `vag` formats support encoding a loop point as part of the ADPCM
  data, while `vagi` supports storing one in the header for use by the stream
  driver. If the input file is either a .wav file with sampler metadata (`smpl`
  chunk) or in a format FFmpeg supports parsing cue/chapter markers from, the
  first marker will be used as the loop point by default. The `-l` and `-n`
  options can be used to manually set a loop point or ignore the one present in
  the input file respectively.

- ~~The `strspu` format encodes the input file's audio track as a series of~~
  ~~custom .str chunks (type ID `0x0001` by default) holding interleaved~~
  ~~SPU-ADPCM data in the same format as `spui`, rather than XA-ADPCM. As .str~~
  ~~chunks do not require custom XA subheaders, a file with standard 2048-byte~~
  ~~sectors that does not need any special handling will be generated.~~ *This*
  *format has not yet been implemented.*

- The `strv` format disables audio altogether and is equivalent to `strspu` on
  an input file with no audio track.

- The `sbs` format (used in some System 573 games) consists of a series of
  concatenated BS frames, each padded to the size specified by the `-a` option
  (the default setting is 8192 bytes), with no additional headers besides the BS
  frame headers.

## Supported video codecs

All formats with a video track (`str`, `strcd`, `strv` and `sbs`) can use any of
the codecs listed below. The codec can be set using the `-v` option.

| Codec          | Supported by          | Typ. decoder CPU usage |
| :------------- | :-------------------- | :--------------------- |
| `v2` (default) | All players/decoders  | Medium                 |
| `v3`           | Most players/decoders | High                   |
| `v3dc`         | Few players/decoders  | High                   |

Notes:

- The `v3dc` format is a variant of `v3` with a slightly better compression
  ratio, however most tools and playback libraries (including FFmpeg, jPSXdec
  and earlier versions of Sony's own BS decoder) are unable to decode it
  correctly; its use is thus highly discouraged. Refer to
  [the psx-spx section on DC coefficient encoding](https://psx-spx.consoledev.net/cdromfileformats/#dc-v3)
  for more details.
