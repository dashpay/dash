
Debian
====================
This directory contains files used to package paccoind/paccoin-qt
for Debian-based Linux systems. If you compile paccoind/paccoin-qt yourself, there are some useful files here.

## paccoin: URI support ##


paccoin-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install paccoin-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your paccoin-qt binary to `/usr/bin`
and the `../../share/pixmaps/paccoin128.png` to `/usr/share/pixmaps`

paccoin-qt.protocol (KDE)

