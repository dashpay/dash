
Debian
====================
This directory contains files used to package oxd/ox-qt
for Debian-based Linux systems. If you compile oxd/ox-qt yourself, there are some useful files here.

## ox: URI support ##


ox-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install ox-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your ox-qt binary to `/usr/bin`
and the `../../share/pixmaps/ox128.png` to `/usr/share/pixmaps`

ox-qt.protocol (KDE)

