# headless tests: aro runs without a screen and small clients poke at it
test_client_dep = dependency('wayland-client', required: false)
if not test_client_dep.found()
  subdir_done()
endif

test_proto = []
foreach xml : [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
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
foreach name : ['popup-orphan', 'layer-popup', 'extws', 'window']
  test_clients += executable(name, 'clients' / name + '.c', test_proto,
    link_with: test_common,
    dependencies: test_client_dep,
  )
endforeach

test('headless', find_program('run.sh'),
  args: [aro_exe.full_path(), aroctl_exe.full_path(), meson.current_build_dir()],
  depends: [aro_exe, aroctl_exe] + test_clients,
  timeout: 240,
  is_parallel: false,
)
