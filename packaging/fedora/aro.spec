# COPR builds main via .copr/Makefile, which rewrites Version; plain rpmbuild builds the release
Name:           aro
Version:        0.4.0
Release:        1%{?dist}
Summary:        Minimal tiling Wayland compositor with spring animations

License:        MIT
URL:            https://github.com/simeulinuxkaliaiwr/aro
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  pkgconfig(wlroots-0.20)
BuildRequires:  pkgconfig(scenefx-0.5)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(libinput)
BuildRequires:  pkgconfig(gdk-pixbuf-2.0)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-ewmh)
BuildRequires:  pkgconfig(xcb-icccm)

Requires:       xorg-x11-server-Xwayland
Recommends:     librsvg2
Recommends:     xdg-desktop-portal-wlr
Recommends:     xdg-desktop-portal-gtk
Suggests:       foot
Suggests:       fuzzel

%description
aro is a tiling window manager for Wayland, built on wlroots: minimal,
flat, one accent colour, spring-animated. It tiles on demand, moves focus
by where windows are on screen, and comes with its own bar, wallpaper and
aroctl for scripts.

%prep
%autosetup

%build
%meson -Deffects=true -Dxwayland=enabled -Dwallpaper=enabled
%meson_build

%install
%meson_install

%files
%license LICENSE
%doc README.md
%{_bindir}/aro
%{_bindir}/aroctl
%{_bindir}/aropaper
%{_datadir}/wayland-sessions/aro.desktop
%{_datadir}/xdg-desktop-portal/aro-portals.conf
%{_datadir}/icons/hicolor/scalable/apps/aro.svg
%{_datadir}/icons/hicolor/symbolic/apps/aro-symbolic.svg
%{_docdir}/%{name}/config.example
%{_datadir}/backgrounds/aro/
%{_mandir}/man1/aro.1*
%{_mandir}/man1/aroctl.1*
%{_mandir}/man1/aropaper.1*
%{_mandir}/man5/aro.5*

%changelog
* Sun Sep 27 2026 Guilherme <aerofrutiger3000@gmail.com> - 0.4.0-1
- Scratchpad, opacity and blur, richer window rules, touchscreens, HDR, remembered wallpaper

* Sat Sep 26 2026 Guilherme <aerofrutiger3000@gmail.com> - 0.3.0-1
- Scrolling layout, screen and window capture, tablets, gestures, VR leasing

* Sat Sep 26 2026 Guilherme <aerofrutiger3000@gmail.com> - 0.2.0-1
- First Fedora package
