#!/bin/bash

#adduser vbd
#usermod -aG sudo vbd
#su vbd

ln -f -s /opt/run/vbd/data ~/.vbdcore
ln -f -s /opt/run/vbd/.vbdcore/vbd.conf ~/.vbdcore/

vbdd -reindex-chainstate -conf=/opt/run/vbd/.vbdcore/vbd.conf -datadir=/opt/run/vbd/data
