
Debian
====================
This directory contains files used to package jastd/jast-qt
for Debian-based Linux systems. If you compile jastd/jast-qt yourself, there are some useful files here.

## jast: URI support ##


jast-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install jast-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your jast-qt binary to `/usr/bin`
and the `../../share/pixmaps/jast128.png` to `/usr/share/pixmaps`

jast-qt.protocol (KDE)

