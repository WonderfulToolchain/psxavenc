# psxavenc

psxavenc is an open-source command-line tool allowing for the encoding of PS1-format audio and video data.

## Usage

Run `psxavenc`.

### Examples

Converting a sound file to a 22050Hz SPU sample:

```shell
$ psxavenc -f 22050 -t spu -c 1 -b 4 sound_file.ogg sound_file.snd
```

## Installation

Requirements:

* a recent version of FFmpeg,
* a recent version of Meson.

```shell
$ meson setup build
$ cd build
$ ninja install
```
