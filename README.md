
# psxavenc

psxavenc is an open-source command-line tool for encoding audio and video data
into formats commonly used on the original PlayStation.

## Installation

Requirements:

- a recent version of FFmpeg libraries (`libavformat`, `libavcodec`,
  `libavutil`, `libswresample`, `libswscale`);
- a recent version of Meson.

```shell
$ meson setup build
$ cd build
$ ninja install
```

## Usage

Run `psxavenc`.

### Examples

Rescale a video file to ≤320x240 pixels (preserving aspect ratio) and encode it
into a 15fps .STR file with 37800 Hz 4-bit stereo audio and 2352-byte sectors,
meant to be played at 2x CD-ROM speed:

```shell
$ psxavenc -t str2cd -f 37800 -b 4 -c 2 -s 320x240 -r 15 -x 2 in.mp4 out.str
```

Convert a mono audio sample to 22050 Hz raw SPU-ADPCM data:

```shell
$ psxavenc -t spu -f 22050 in.ogg out.snd
```

Convert a stereo audio file to a 44100 Hz interleaved .VAG file with 8192-byte
interleave and loop flags set at the end of each interleaved chunk:

```shell
$ psxavenc -t vagi -f 44100 -c 2 -L -i 8192 in.wav out.vag
```

## Supported formats

| Format   | Audio            | Channels | Video | Sector size |
| :------- | :--------------- | :------- | :---- | :---------- |
| `xa`     | XA-ADPCM         | 1 or 2   | None  | 2336 bytes  |
| `xacd`   | XA-ADPCM         | 1 or 2   | None  | 2352 bytes  |
| `spu`    | SPU-ADPCM        | 1        | None  |             |
| `spui`   | SPU-ADPCM        | Any      | None  | Any         |
| `vag`    | SPU-ADPCM        | 1        | None  |             |
| `vagi`   | SPU-ADPCM        | Any      | None  | Any         |
| `str2`   | None or XA-ADPCM | 1 or 2   | BS v2 | 2336 bytes  |
| `str2cd` | None or XA-ADPCM | 1 or 2   | BS v2 | 2352 bytes  |
| `str2v`  | None             |          | BS v2 |             |
| `sbs2`   | None             |          | BS v2 | Any         |

Notes:

- `vag` and `vagi` are similar to `spu` and `spui` respectively, but add a .VAG
  header at the beginning of the file. The header is always 48 bytes long for
  `vag` files, while in the case of `vagi` files it is padded to the size
  specified using the `-a` option (2048 bytes by default). Note that `vagi`
  files with more than 2 channels and/or alignment other than 2048 bytes are not
  standardized.
- The `sbs2` format (used in some System 573 games) is simply a series of
  concatenated BS v2 frames, each padded to the size specified by the `-a`
  option, with no additional headers besides the BS frame headers.
