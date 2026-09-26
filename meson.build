project('aro', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

# === project-wide args ===
# meson wants all add_project_arguments() calls before any target is declared.
add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')
# `aroctl version` and `aroctl --version`; the literal keeps its quotes
add_project_arguments('-DARO_VERSION="@0@"'.format(meson.project_version()),
  language: 'c')

cc = meson.get_compiler('c')

if get_option('compositor')
  if get_option('effects')
    add_project_arguments('-DARO_EFFECTS', language: 'c')
  endif

  # XWayland is enabled unless wlroots lacks support.
  #
  # plenty of desktop apps are still X11, but wlroots can be built without it.
  xwl = get_option('xwayland')
  if not xwl.disabled()
    wlroots_pre = dependency('wlroots-0.20')
    if cc.has_header('wlr/xwayland.h', dependencies: wlroots_pre)
      add_project_arguments('-DARO_XWAYLAND', language: 'c')
      message('XWayland: enabled')
    elif xwl.enabled()
      error('-Dxwayland=enabled but wlroots was built without XWayland support')
    else
      warning('wlroots has no XWayland support; building without X11 clients')
    endif
  endif
endif

# === wallpaper client: probed early, the scanner below depends on it ===
# aropaper is a plain Wayland client and builds without the compositor.
wallpaper_opt = get_option('wallpaper')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', required: wallpaper_opt)
wl_client_dep = dependency('wayland-client', required: wallpaper_opt)
build_wallpaper = gdk_pixbuf_dep.found() and wl_client_dep.found()

inc = include_directories(
  'src',
  'src/layout',
  'src/compositor',
  'src/ui',
)

# === Layout tree ===
# intentionally dependency-free.
core = static_library('aro-core',
  ['src/layout/layout.c', 'src/layout/anim.c'],
  include_directories: inc,
)

core_dep = declare_dependency(link_with: core, include_directories: inc)

executable('aro-layout', 'src/layout/layout_harness.c',
  dependencies: core_dep,
  install: false,
)

# === wayland-scanner: the compositor and aropaper both generate code ===
if get_option('compositor') or build_wallpaper
  wayland_protos = dependency('wayland-protocols')
  wl_protocol_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')
  wayland_scanner_dep = dependency('wayland-scanner', native: true)
  wayland_scanner = find_program(
    wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'),
    native: true,
  )
endif

# === compositor ===
# example: meson setup build -Dcompositor=true -Deffects=true
if get_option('compositor')
  proto_xml = [
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
    # wlroots' own headers include these two; they are not shipped
    wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
    wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
    'protocols/wlr-layer-shell-unstable-v1.xml',
    'protocols/wlr-output-power-management-unstable-v1.xml',
  ]

  proto_headers = []
  foreach xml : proto_xml
    proto_headers += custom_target(
      input: xml,
      output: '@BASENAME@-protocol.h',
      command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
    )
  endforeach

  deps = [core_dep]
  if get_option('effects')
    deps += dependency('scenefx-0.5')  # needs to come before wlroots
  endif

  wlroots = dependency('wlroots-0.20')
  # touchpad settings; only when wlroots has the libinput backend
  libinput = dependency('libinput',
    required: wlroots.get_variable('have_libinput_backend') == 'true')

  # wlroots itself must have XWayland support built in.
  deps += [
    wlroots,
    libinput,
    dependency('wayland-server'),
    dependency('xkbcommon'),
    dependency('pixman-1'),
    dependency('pangocairo'),  # text rendering; wlroots doesn't provide it
    cc.find_library('m', required: false),  # sqrt for scale = auto
  ]

  install_data('data/portals.conf',
    install_dir: get_option('datadir') / 'xdg-desktop-portal',
    rename: 'aro-portals.conf',
  )

  install_data('data/aro.desktop',
    install_dir: get_option('datadir') / 'wayland-sessions',
  )

  # shown next to the session name by display managers that show icons
  install_data('data/icons/aro.svg',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
  )
  install_data('data/icons/aro-symbolic.svg',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'symbolic' / 'apps',
  )

  # install the example config somewhere users can find and copy it,
  # since aro doesn't ship a default config.
  install_data('data/config.example',
    install_dir: get_option('datadir') / 'doc' / 'aro',
  )

  aro_sources = [
    'src/compositor/aro.c',
    'src/compositor/config.c',
    'src/compositor/idle.c',
    'src/compositor/ime.c',
    'src/compositor/ipc.c',
    'src/compositor/lock.c',
    'src/compositor/logfile.c',
    'src/compositor/wallpaper.c',
    'src/ui/ui.c',
    'src/ui/bar.c',
    'src/ui/ghost.c',
    'src/ui/notify.c',
    'src/ui/overview.c',
    'src/ui/prompt.c',
    'src/ui/switcher.c',
    'src/ui/text.c',
  ]

  executable('aro',
    aro_sources + proto_headers,
    dependencies: deps,
    include_directories: inc,
    install: true,
  )

  # talks to a running aro over its socket; no wlroots, no libwayland
  executable('aroctl', 'src/aroctl/aroctl.c',
    install: true,
  )
endif

# === wallpaper ===
# the art installs either way: the PNGs are useful to swaybg users too
wallpaper_dir = get_option('prefix') / get_option('datadir') / 'backgrounds' / 'aro'
install_data(
  'data/wallpaper/aro-wallpaper.svg',
  'data/wallpaper/aro-wallpaper-1920x1080.png',
  'data/wallpaper/aro-wallpaper-2560x1440.png',
  'data/wallpaper/aro-wallpaper-3840x2160.png',
  install_dir: wallpaper_dir,
)

if build_wallpaper
  # client side: headers and the interface tables (libwayland-client has
  # only the core ones). layer shell refers to xdg_popup, hence xdg-shell.
  paper_xml = [
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_protocol_dir / 'stable/viewporter/viewporter.xml',
    wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
    'protocols/wlr-layer-shell-unstable-v1.xml',
  ]
  paper_proto = []
  foreach xml : paper_xml
    paper_proto += custom_target(
      input: xml,
      output: '@BASENAME@-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
    paper_proto += custom_target(
      input: xml,
      output: '@BASENAME@-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )
  endforeach

  executable('aropaper',
    ['src/aropaper/aropaper.c'] + paper_proto,
    dependencies: [
      wl_client_dep,
      gdk_pixbuf_dep,
      cc.find_library('m', required: false),  # ceil
    ],
    c_args: ['-DARO_WALLPAPER_DIR="@0@"'.format(wallpaper_dir)],
    install: true,
  )
endif
