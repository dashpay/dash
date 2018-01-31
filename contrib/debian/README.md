
Debian
====================
This directory contains files used to package krissd/kriss-qt
for Debian-based Linux systems. If you compile krissd/kriss-qt yourself, there are some useful files here.

## kriss: URI support ##


kriss-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install kriss-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your kriss-qt binary to `/usr/bin`
and the `../../share/pixmaps/kriss128.png` to `/usr/share/pixmaps`

kriss-qt.protocol (KDE)

