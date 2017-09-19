
Debian
====================
This directory contains files used to package kliqd/kliq-qt
for Debian-based Linux systems. If you compile kliqd/kliq-qt yourself, there are some useful files here.

## kliq: URI support ##


kliq-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install kliq-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your kliq-qt binary to `/usr/bin`
and the `../../share/pixmaps/kliq128.png` to `/usr/share/pixmaps`

kliq-qt.protocol (KDE)

