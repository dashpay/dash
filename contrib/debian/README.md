
Debian
====================
This directory contains files used to package gamblrd/gamblr-qt
for Debian-based Linux systems. If you compile gamblrd/gamblr-qt yourself, there are some useful files here.

## gamblr: URI support ##


gamblr-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install gamblr-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your gamblr-qt binary to `/usr/bin`
and the `../../share/pixmaps/gamblr128.png` to `/usr/share/pixmaps`

gamblr-qt.protocol (KDE)

