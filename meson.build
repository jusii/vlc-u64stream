project('vlc-u64stream', 'c',
  version: '0.1.0',
  license: 'Apache-2.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')

vlc_plugin = dependency('vlc-plugin', version: '>=3.0.0')

# VLC plugins are loaded via dlopen; export only the module entry point.
plugin_args = [
  '-DMODULE_STRING="u64stream"',
  # VLC's N_() / config-set macros pass string literals through int casts;
  # silence the resulting warning rather than touching SDK headers.
  '-Wno-int-to-pointer-cast',
]

plugin_link_args = []
if host_machine.system() == 'linux' or host_machine.system() == 'freebsd'
  plugin_link_args += ['-Wl,--no-undefined']
endif

# Default install dir: VLC's per-user plugin path.
# Override with: meson setup build -Dvlc_plugin_dir=/usr/lib/x86_64-linux-gnu/vlc/plugins
vlc_plugin_dir = get_option('vlc_plugin_dir')
if vlc_plugin_dir == ''
  vlc_plugin_dir = join_paths(
    get_option('prefix'),
    get_option('libdir'),
    'vlc', 'plugins', 'access',
  )
endif

shared_module('u64stream_plugin',
  'src/u64stream.c',
  c_args: plugin_args,
  link_args: plugin_link_args,
  dependencies: [vlc_plugin],
  name_prefix: 'lib',
  install: true,
  install_dir: vlc_plugin_dir,
)

# Convenience target: copy the freshly built plugin to ~/.local/share/vlc/plugins
# so it can be tested without `meson install`. Run with:
#   meson compile -C build install-user
# Note: the upstream Ubuntu/Debian VLC package does NOT auto-scan
# ~/.local/share/vlc/plugins — it only scans the system plugin dir. So a
# user-local install must be invoked via:
#   VLC_PLUGIN_PATH=~/.local/share/vlc/plugins vlc u64://@:11000
# The install-user target prints this hint after copying.
user_plugin_dir = join_paths(
  run_command('sh', '-c', 'echo $HOME', check: true).stdout().strip(),
  '.local', 'share', 'vlc', 'plugins',
)

install_user_script = (
  'set -e; mkdir -p "@0@"; cp "@1@/libu64stream_plugin.so" "@0@/"; '
  + 'echo; echo "Installed: @0@/libu64stream_plugin.so"; '
  + 'echo "Test with:"; '
  + 'echo "  VLC_PLUGIN_PATH=@0@ vlc -p u64stream"; '
  + 'echo "  VLC_PLUGIN_PATH=@0@ vlc u64://@:11000"; echo'
).format(user_plugin_dir, meson.current_build_dir())

run_target('install-user',
  command: ['sh', '-c', install_user_script],
)
