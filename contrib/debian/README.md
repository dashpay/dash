
Debian
====================
This directory contains files used to package dashd/vbd-qt
for Debian-based Linux systems. If you compile dashd/vbd-qt yourself, there are some useful files here.

## dash: URI support ##


vbd-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install vbd-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your vbd-qt binary to `/usr/bin`
and the `../../share/pixmaps/dash128.png` to `/usr/share/pixmaps`

vbd-qt.protocol (KDE)

