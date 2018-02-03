#!/bin/bash

#adduser vbd
#usermod -aG sudo vbd
#su vbd

git clone https://github.com/VISCHub/VBD

cd VBD

./autogen.sh

# https://github.com/bitcoin/bitcoin/blob/master/doc/build-unix.md
# ./configure
./configure --with-incompatible-bdb --without-gui

make

make install

make clean
