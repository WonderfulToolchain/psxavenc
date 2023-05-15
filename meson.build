project('psxavenc', 'c', default_options: ['c_std=c11'])

add_project_arguments('-D_POSIX_C_SOURCE=201112L', language : 'c')

libm_dep = meson.get_compiler('c').find_library('m')

ffmpeg = [
  dependency('libavformat'),
  dependency('libavcodec'),
  dependency('libavutil'),
  dependency('libswresample'),
  dependency('libswscale')
]

libpsxav = static_library('psxav', [
  'libpsxav/adpcm.c',
  'libpsxav/cdrom.c',
  'libpsxav/libpsxav.h'
])
libpsxav_dep = declare_dependency(include_directories: include_directories('libpsxav'), link_with: libpsxav)

executable('psxavenc', [
  'psxavenc/cdrom.c',
  'psxavenc/decoding.c',
  'psxavenc/filefmt.c',
  'psxavenc/mdec.c',
  'psxavenc/psxavenc.c'
], dependencies: [libm_dep, ffmpeg, libpsxav_dep], install: true)
