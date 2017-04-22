
Debian
====================
This directory contains files used to package owncoind/owncoin-qt
for Debian-based Linux systems. If you compile owncoind/owncoin-qt yourself, there are some useful files here.

## owncoin: URI support ##


owncoin-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install owncoin-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your owncoin-qt binary to `/usr/bin`
and the `../../share/pixmaps/owncoin128.png` to `/usr/share/pixmaps`

owncoin-qt.protocol (KDE)

