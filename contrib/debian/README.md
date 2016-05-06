
Debian
====================
This directory contains files used to package SarmaCoinsd/SarmaCoins-qt
for Debian-based Linux systems. If you compile SarmaCoinsd/SarmaCoins-qt yourself, there are some useful files here.

## SarmaCoins: URI support ##


SarmaCoins-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install SarmaCoins-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your SarmaCoins-qt binary to `/usr/bin`
and the `../../share/pixmaps/SarmaCoins128.png` to `/usr/share/pixmaps`

SarmaCoins-qt.protocol (KDE)

