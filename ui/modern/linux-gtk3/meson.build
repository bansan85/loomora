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

# GTK version policy (CLAUDE.md, docs/implementation/adr/0005-gtk-version-policy.md). The UI always compiles with
# GDK_VERSION_MIN_REQUIRED at 3.10. -Dgtk_api picks GTK 3 (default) or GTK 4; -Dgtk_floor=true
# builds the 3.10 path only and makes the compiler reject every GTK/GDK symbol newer than 3.10
# (the floor build). Newer features are enabled by the macros of gtk_compat.h, from the version of
# the headers, and declared in docs/implementation/validation/fallbacks.json.
gtk_api = get_option('gtk_api')
gtk_floor = get_option('gtk_floor')
if gtk_api == '4' and gtk_floor
  error('gtk_floor proves the GTK 3.10 path: it cannot be combined with gtk_api=4')
endif
gtk_args = []
if gtk_api == '3'
  gtk_args += ['-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_10']
  if gtk_floor
    gtk_args += [
      '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_3_10',
      '-Werror=deprecated-declarations',
    ]
  endif
  gtk_dep = dependency('gtk+-3.0', version: '>=3.10')
else
  gtk_dep = dependency('gtk4', version: '>=4.0')
endif

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
  ['main.c', 'probe.c', 'gtk_compat.c'],
  include_directories: core_include,
  dependencies: gtk_dep,
  link_with: libloomora,
  c_args: gtk_args,
  link_args: ['-static-libstdc++', '-static-libgcc'],
  install: true,
)
