
Debian
====================
This directory contains files used to package gvidond/gvidon-qt
for Debian-based Linux systems. If you compile gvidond/gvidon-qt yourself, there are some useful files here.

## gvidon: URI support ##


gvidon-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install gvidon-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your gvidon-qt binary to `/usr/bin`
and the `../../share/pixmaps/gvidon128.png` to `/usr/share/pixmaps`

gvidon-qt.protocol (KDE)

