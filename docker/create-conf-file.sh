#!/bin/bash

#adduser vbd
#usermod -aG sudo vbd
#su vbd

mkdir -p /opt/run/vbd/.vbdcore

cat > /opt/run/vbd/.vbdcore/vbd.conf <<EOF
gen=1
server=1
listen=1
txindex=1
addnode=45.32.19.62
addnode=104.238.157.165
# When running outside Docker, set daemon=1
daemon=0
EOF
