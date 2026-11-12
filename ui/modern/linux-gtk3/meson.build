# Loomora Modern UI for Linux: C + GTK 3 (3.10 API subset), plain executable, no Flatpak.
#
#   meson setup <builddir> ui/modern/linux-gtk3
#   ninja -C <builddir>        ->  <builddir>/loomora-gtk3
#
# The C++11 backend of core/ is compiled here into the static library libloomora.a (same sources
# as core/CMakeLists.txt) and linked with the C++ runtime statically (ADR 0001, rule 4), so the
# executable depends only on the shared GTK and system libraries of the distribution.
project(
  'loomora-gtk3',
  ['c', 'cpp'],
  version: '0.1.0',
  default_options: [
    'c_std=c99',
    'cpp_std=c++11',
    'warning_level=2',
    'buildtype=release',
  ],
  meson_version: '>=0.53.0',
)

# Floor of the UI: the compiler flags every GTK/GDK symbol newer than 3.10.
gtk_floor = [
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_10',
  '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_3_10',
]

gtk_dep = dependency('gtk+-3.0', version: '>=3.10')

core_include = include_directories('../../../core/include')
core_src = [
  '../../../core/src/c_api.cpp',
  '../../../core/src/eol.cpp',
  '../../../core/src/greet.cpp',
  '../../../core/src/probe.cpp',
  '../../../core/src/utf8.cpp',
  '../../../core/src/version.cpp',
]

libloomora = static_library(
  'loomora',
  core_src,
  include_directories: core_include,
  cpp_args: ['-pedantic', '-fvisibility=hidden', '-fvisibility-inlines-hidden'],
  override_options: ['cpp_std=c++11'],
)

executable(
  'loomora-gtk3',
  ['main.c', 'probe.c'],
  include_directories: core_include,
  dependencies: gtk_dep,
  link_with: libloomora,
  c_args: gtk_floor,
  link_args: ['-static-libstdc++', '-static-libgcc'],
  install: true,
)
