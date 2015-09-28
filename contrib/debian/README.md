
Debian
====================
This directory contains files used to package btxd/btx-qt
for Debian-based Linux systems. If you compile btxd/btx-qt yourself, there are some useful files here.

## btx: URI support ##


btx-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install btx-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your btx-qt binary to `/usr/bin`
and the `../../share/pixmaps/btx128.png` to `/usr/share/pixmaps`

btx-qt.protocol (KDE)

