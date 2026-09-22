project('aro', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

# ── project-wide arguments ────────────────────────────────────────────────
# Meson requires every add_project_arguments() call to happen before the
# first target is declared, so all of them live here rather than next to the
# code they affect.
add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

if get_option('compositor')
  if get_option('effects')
    add_project_arguments('-DARO_EFFECTS', language: 'c')
  endif

  # XWayland is on unless wlroots cannot do it.
  #
  # Not really optional in practice — half the desktop is still X11 — but
  # wlroots itself can be built without XWayland support, and making this a
  # hard requirement would mean those people cannot build aro at all.
  # So: auto by default, silently off when wlroots lacks it, and a loud
  # error if someone asked for it explicitly and cannot have it.
  xwl = get_option('xwayland')
  if not xwl.disabled()
    cc = meson.get_compiler('c')
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

inc = include_directories('src')

# ── the layout tree ───────────────────────────────────────────────────────
# No dependencies by design. It is the part of the WM that can be wrong in
# interesting ways, so it stays testable without a compositor.
core = static_library('aro-core', ['src/layout.c', 'src/anim.c'])
core_dep = declare_dependency(link_with: core, include_directories: inc)

executable('aro-layout', 'src/layout_harness.c',
  dependencies: core_dep,
  install: false,
)

# ── the compositor ────────────────────────────────────────────────────────
#   meson setup build -Dcompositor=true -Deffects=true
#
# SceneFX 0.5 is a drop-in replacement for wlr_scene adding rounded corners,
# shadows and blur, and it pins wlroots 0.20 — which is why the whole project
# targets 0.20. src/scene.h picks which scene header the build uses, and
# -Deffects=false falls back to plain wlr_scene with no other change.
if get_option('compositor')

  # xdg-shell comes from wayland-protocols; wlr-layer-shell does not exist
  # there, so its XML is vendored under protocols/ the way sway and dwl do.
  # Only the server header is needed — wlroots links the implementation.
  wayland_protos = dependency('wayland-protocols')
  wl_protocol_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')

  wayland_scanner_dep = dependency('wayland-scanner', native: true)
  wayland_scanner = find_program(
    wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'),
    native: true,
  )

  # Neither wlr-* protocol is in wayland-protocols: they live in
  # wlr-protocols, and their generated headers are not installed even
  # though the wlroots headers that #include them are. So the XML is
  # vendored under protocols/ and scanned here, the way sway and dwl do.
  proto_xml = [
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
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

  # LINK ORDER MATTERS. SceneFX exports the same symbols as wlroots' scene
  # API — that is what makes it a drop-in replacement — so whichever library
  # comes first on the link line provides them.
  #
  # Get this backwards and it still builds and still runs: rects are then
  # allocated by wlroots' wlr_scene_rect_create, which has no corners or
  # clipped_region fields, while wlr_scene_rect_set_corner_radius (which only
  # SceneFX has) writes into them anyway. That corrupts the heap and crashes
  # somewhere else entirely, usually inside pixman or free().
  deps = [core_dep]

  if get_option('effects')
    deps += dependency('scenefx-0.5')   # must precede wlroots
  endif

  # wlroots must itself have been built with XWayland support; if it was not,

  deps += [
    dependency('wlroots-0.20'),
    dependency('wayland-server'),
    dependency('xkbcommon'),
    dependency('pixman-1'),
    dependency('pangocairo'),   # text; wlroots has none of its own
  ]

  install_data('portals.conf',
    install_dir: get_option('datadir') / 'xdg-desktop-portal',
    rename: 'aro-portals.conf',
  )

  executable('aro',
    ['src/aro.c', 'src/ui.c', 'src/text.c', 'src/bar.c', 'src/config.c', 'src/notify.c', 'src/prompt.c', 'src/lock.c', 'src/idle.c'] + proto_headers,
    dependencies: deps,
    install: true,
  )
endif
