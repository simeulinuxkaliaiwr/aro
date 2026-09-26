# headless tests: aro runs without a screen and small clients poke at it
test_client_dep = dependency('wayland-client', required: false)
if not test_client_dep.found()
  subdir_done()
endif

test_proto = []
foreach xml : [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
  wl_protocol_dir / 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml',
  wl_protocol_dir / 'staging/security-context/security-context-v1.xml',
  wl_protocol_dir / 'staging/xdg-dialog/xdg-dialog-v1.xml',
  wl_protocol_dir / 'staging/tearing-control/tearing-control-v1.xml',
  wl_protocol_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  wl_protocol_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  wl_protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  '../protocols/wlr-layer-shell-unstable-v1.xml',
]
  test_proto += custom_target(
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  test_proto += custom_target(
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

test_common = static_library('aro-test-common',
  ['clients/common.c'] + test_proto,
  dependencies: test_client_dep,
)

test_clients = []
foreach name : ['popup-orphan', 'layer-popup', 'extws', 'window', 'globals', 'inhibit', 'sandbox', 'capture']
  test_clients += executable(name, 'clients' / name + '.c', test_proto,
    link_with: test_common,
    dependencies: test_client_dep,
  )
endforeach

foreach mode : ['main', 'multi']
test('headless-' + mode, find_program('run.sh'),
  args: [aro_exe.full_path(), aroctl_exe.full_path(), meson.current_build_dir(), mode],
  depends: [aro_exe, aroctl_exe] + test_clients,
  timeout: 240,
  is_parallel: false,
)
endforeach
