
Debian
====================
This directory contains files used to package waggoxd/waggox-qt
for Debian-based Linux systems. If you compile waggoxd/waggox-qt yourself, there are some useful files here.

## waggox: URI support ##


waggox-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install waggox-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your waggox-qt binary to `/usr/bin`
and the `../../share/pixmaps/waggox128.png` to `/usr/share/pixmaps`

waggox-qt.protocol (KDE)

