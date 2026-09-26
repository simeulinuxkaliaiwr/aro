{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wrapGAppsNoGuiHook,
  wayland,
  wayland-protocols,
  wlroots_0_20,
  scenefx,
  libinput,
  libxkbcommon,
  pixman,
  libdrm,
  libGL,
  pango,
  cairo,
  gdk-pixbuf,
  librsvg,
  libxcb,
  xcbutilwm,
  withEffects ? true,
  withXwayland ? true,
}:

stdenv.mkDerivation {
  pname = "aro";
  version = "0.1.0-unstable";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      ../meson_options.txt
      ../src
      ../protocols
      ../data
    ];
  };

  strictDeps = true;
  depsBuildBuild = [ pkg-config ];
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    wrapGAppsNoGuiHook
  ];

  buildInputs = [
    wayland
    wayland-protocols
    wlroots_0_20
    libinput
    libxkbcommon
    pixman
    libdrm
    pango
    cairo
    gdk-pixbuf
    librsvg
  ]
  ++ lib.optionals withEffects [ scenefx libGL ]
  ++ lib.optionals withXwayland [ libxcb xcbutilwm ];

  mesonFlags = [
    (lib.mesonBool "effects" withEffects)
    (lib.mesonEnable "xwayland" withXwayland)
    (lib.mesonEnable "wallpaper" true)
  ];

  # only aropaper needs the SVG loader; wrapping aro would leak GTK paths into every app it starts
  dontWrapGApps = true;
  postFixup = ''
    gappsWrapperArgs+=(--set GDK_PIXBUF_MODULE_FILE "${librsvg}/${gdk-pixbuf.binaryDir}/loaders.cache")
    wrapGApp $out/bin/aropaper
  '';

  passthru.providedSessions = [ "aro" ];

  meta = {
    description = "Minimal tiling Wayland compositor with spring animations";
    homepage = "https://github.com/simeulinuxkaliaiwr/aro";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "aro";
  };
}
