VBD Core 0.12.2.3
Build Node Ubuntu 16.4

Run follow it:

sudo apt-get update -y

sudo apt-get install -y libboost-dev libboost-chrono-dev libboost-filesystem-dev libboost-program-options-dev libboost-system-dev libboost-test-dev libboost-thread-dev wget nano bsdmainutils curl libevent-dev automake libdb++-dev build-essential libtool autotools-dev autoconf pkg-config libssl-dev libboost-all-dev libminiupnpc-dev python-virtualenv git software-properties-common python-software-properties g++ virtualenv

sudo add-apt-repository ppa:bitcoin/bitcoin

sudo apt-get update -y

sudo apt-get install libdb4.8-dev libdb4.8++-dev 

sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler 

sudo apt-get upgrade 

adduser vbd

usermod -aG sudo vbd

visudo

# find root ALL=(ALL:ALL) ALL paste the following under neath

vbd ALL=(ALL:ALL) ALL

su vbd

git clone https://github.com/ctphu/VBD

cd VBD

./autogen.sh

./configure

./make

./sudo make install

mkdir ~/.vbdcore

nano ~/.vbdcore/vbd.conf

# Add 6 row below

server=1

listen=1

daemon=1

txindex=1

addnode=45.32.19.62

addnode=104.238.157.165

# Ctrl+X to save it

# Run vbd:
vbdd


Dash Core 0.12.1
=====================

This is the official reference wallet for Dash digital currency and comprises the backbone of the Dash peer-to-peer network. You can [download Dash Core](https://www.dash.org/downloads/) or [build it yourself](#building) using the guides below.

Running
---------------------
The following are some helpful notes on how to run Dash on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/vbd-qt` (GUI) or
- `bin/dashd` (headless)

### Windows

Unpack the files into a directory, and then run vbd-qt.exe.

### OS X

Drag Dash-Qt to your applications folder, and then run Dash-Qt.

### Need Help?

* See the [Dash documentation](https://dashpay.atlassian.net/wiki/display/DOC)
for help and more information.
* Ask for help on [#dashpay](http://webchat.freenode.net?channels=dashpay) on Freenode. If you don't have an IRC client use [webchat here](http://webchat.freenode.net?channels=dashpay).
* Ask for help on the [DashTalk](https://dashtalk.org/) forums.

Building
---------------------
The following are developer notes on how to build Dash Core on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [OS X Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [Gitian Building Guide](gitian-building.md)

Development
---------------------
The Dash Core repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Multiwallet Qt Development](multiwallet-qt.md)
- [Release Notes](release-notes.md)
- [Release Process](release-process.md)
- Source Code Documentation ***TODO***
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [Unit Tests](unit-tests.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)

### Resources
* Discuss on the [DashTalk](https://dashtalk.org/) forums, in the Development & Technical Discussion board.
* Discuss on [#dashpay](http://webchat.freenode.net/?channels=dashpay) on Freenode. If you don't have an IRC client use [webchat here](http://webchat.freenode.net/?channels=dashpay).

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [Files](files.md)
- [Tor Support](tor.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)

License
---------------------
Distributed under the [MIT software license](http://www.opensource.org/licenses/mit-license.php).
This product includes software developed by the OpenSSL Project for use in the [OpenSSL Toolkit](https://www.openssl.org/). This product includes
cryptographic software written by Eric Young ([eay@cryptsoft.com](mailto:eay@cryptsoft.com)), and UPnP software written by Thomas Bernard.
