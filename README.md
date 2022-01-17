# **Alterdot (ADOT) v1.9.2.1**

<table>
 <tr>
  <th rowspan=2>
   <img src="https://github.com/Alterdot/Alterdot/blob/master/src/qt/res/icons/bitcoin.png" width="76" height="76">
  </th>
  <th>
   master
  </th>
  <th>
   develop
  </th>
 </tr>
 <tr>
  <th>
   
   [![CircleCI](https://circleci.com/gh/Alterdot/Alterdot/tree/master.svg?style=svg)](https://circleci.com/gh/Alterdot/Alterdot/tree/master)
   
  </th>
  <th>
   
   [![CircleCI](https://circleci.com/gh/Alterdot/Alterdot/tree/develop.svg?style=svg)](https://circleci.com/gh/Alterdot/Alterdot/tree/develop)
   
  </th>
 </tr>
</table>

**[alterdot.network](https://alterdot.network)**

**Copyright (c) 2017-2022 Alterdot developers**

**Project previously known as [Bitcreds](https://github.com/Bitcreds/Bitcreds).**

What is Alterdot?
----------------
* Coin Suffix: ADOT
* PoW Mining Algorithm: Argon2d250 until January 30, 2021 6:00:00 PM UTC; Argon2d16000 afterwards
* PoW Difficulty Algorithm: DELTA until block 1,034,000; LWMA afterwards
* PoW Target Spacing: 128 Seconds until block 1,000,000; 360 Seconds afterwards
* PoW Reward per Block: See Below
* PoW Blocks: ~675 per day until block 1,000,000; ~240 per day afterwards
* Coinbase Maturity: 10 Blocks
* Masternode Collateral Amount: 10,000 ADOT
* Masternode Min Confirmation: 16 Blocks
* Masternode Reward: See Below
* Total Coins (until 2 PoW / 2 MN / 2 DevFund reward): ~13,914,700* (after ~5 Years from Genesis)
* Min TX Fee: 0.0001 ADOT

*Some blocks did not pay full rewards therefore the actual total supply is lower.

Alterdot uses peer-to-peer technology to operate securely with no central authority (decentralisation): managing transactions and issuing currency (ADOT) are carried out collectively by the Alterdot network. Alterdot is the name of open source software which enables the use of the currency ADOT.

Alterdot utilises Masternodes, Privatesend and InstantSend to provide anonymous and near instant transaction confirmations.

Alterdot implements Gavin Andresens signature cache optimisation from Bitcoin for significantly faster transaction validation.


**Masternode/Privatesend Network Information**
Utilisation of InstantSend for near-instant transactions and PrivateSend for anonymous transactions.

**MainNet Parameters**
P2P Port = 31000
RPC Port = 31050
Masternodes = 31000
Magic Bytes: 0x2f 0x32 0x45 0x51

**TestNet Parameters**
P2P Port = 31400
RPC Port = 31450
Masternodes = 31400
Magic Bytes: 0x1f 0x22 0x05 0x30

**RegTest Parameters**
P2P Port = 31500
RPC Port = 31550
Masternodes = 31500
Magic Bytes = 0x1f 0x22 0x05 0x2f

**Rewards Structure**

 Year |           Blocks        |   PoW   | Masternodes  | Development Fund |  Total Generated  |
------|-------------------------|---------|--------------|------------------|-------------------|
 2017 |          0 - 375,000    | 10 ADOT |    1 ADOT    |      0 ADOT      |     11 ADOT       |
 2018 |    375,001 - 500,000    |  8 ADOT |    2 ADOT    |      0.5 ADOT    |     10.5 ADOT     |
 2018 |    500,001 - 550,000    |  7 ADOT |    3 ADOT    |      0.5 ADOT    |     10.5 ADOT     |
 2019 |    550,001 - 625,000    |  7 ADOT |    3 ADOT    |      1 ADOT      |     11 ADOT       |
 2019 |    625,001 - 750,000    |  6 ADOT |    4 ADOT    |      0.5 ADOT    |     10.5 ADOT     |
 2019 |    750,001 - 875,000    |  5 ADOT |    5 ADOT    |      0.5 ADOT    |     10.5 ADOT     |
 2020 |    875,001 - 1,000,000  |  4 ADOT |    6 ADOT    |      0.5 ADOT    |     10.5 ADOT     |
 2021 |  1,000,001 - 1,043,800  |  6 ADOT |    6 ADOT    |      2 ADOT      |     14 ADOT       |
 2021 |  1,043,801 - 1,087,600  |  5 ADOT |    5 ADOT    |      2 ADOT      |     12 ADOT       |
 2022 |  1,087,601 - 1,131,400  |  4 ADOT |    4 ADOT    |      2 ADOT      |     10 ADOT       |
 2022 |  1,131,401 - 1,175,200  |  3 ADOT |    3 ADOT    |      2 ADOT      |      8 ADOT       |
 2023 |  1,175,201 - ∞          |  2 ADOT |    2 ADOT    |      2 ADOT      |      6 ADOT       |

UNIX BUILD NOTES
====================
Some notes on how to build Alterdot in Unix.

Note
---------------------
Always use absolute paths to configure and compile Alterdot and the dependencies,
for example, when specifying the the path of the dependency:

    ../dist/configure --enable-cxx --disable-shared --with-pic --prefix=$BDB_PREFIX

Here BDB_PREFIX must absolute path - it is defined using $(pwd) which ensures
the usage of the absolute path.

To Build
---------------------

```bash
./autogen.sh
./configure
make
make install # optional
```

This will build alterdot-qt as well if the dependencies are met.

Dependencies
---------------------

These dependencies are required:

 Library     | Purpose          | Description
 ------------|------------------|----------------------
 libssl      | SSL Support      | Secure communications
 libboost    | Boost            | C++ Library
 libevent    | Networking       | OS independent asynchronous networking

Optional dependencies:

 Library     | Purpose          | Description
 ------------|------------------|----------------------
 miniupnpc   | UPnP Support     | Firewall-jumping support
 libdb4.8    | Berkeley DB      | Wallet storage (only needed when wallet enabled)
 qt          | GUI              | GUI toolkit (only needed when GUI enabled)
 protobuf    | Payments in GUI  | Data interchange format used for payment protocol (only needed when GUI enabled)
 libqrencode | QR codes in GUI  | Optional for generating QR codes (only needed when GUI enabled)
 libzmq3     | ZMQ notification | Optional, allows generating ZMQ notifications (requires ZMQ version >= 4.x)

For the versions used in the release, see [release-process.md](release-process.md) under *Fetch and build inputs*.

System requirements
--------------------

C++ compilers are memory-hungry. It is recommended to have at least 3 GB of
memory available when compiling Alterdot.

Dependency Build Instructions: Ubuntu & Debian
----------------------------------------------
Build requirements:

    sudo apt-get install curl build-essential libtool autotools-dev automake pkg-config python3 bsdmainutils cmake git

**! Important !**
Go to [doc](https://github.com/Alterdot/Alterdot/tree/master/doc) folder for more information, some of the instructions below are outdated but might be helpful to some.

for Ubuntu 12.04 and later or Debian 7 and later libboost-all-dev has to be installed:

    sudo apt-get install libboost-all-dev

 db4.8 packages are available [here](https://launchpad.net/~bitcoin/+archive/bitcoin).
 You can add the repository using the following command:

        sudo add-apt-repository ppa:bitcoin/bitcoin
        sudo apt-get update

 Ubuntu 12.04 and later have packages for libdb5.1-dev and libdb5.1++-dev,
 but using these will break binary wallet compatibility, and is not recommended.

for Debian 7 (Wheezy) and later:
 The oldstable repository contains db4.8 packages.
 Add the following line to /etc/apt/sources.list,
 replacing [mirror] with any official debian mirror.

    deb http://[mirror]/debian/ oldstable main

To enable the change run

    sudo apt-get update

for other Debian & Ubuntu (with ppa):

    sudo apt-get install libdb4.8-dev libdb4.8++-dev

Optional (see --with-miniupnpc and --enable-upnp-default):

    sudo apt-get install libminiupnpc-dev

ZMQ dependencies (provides ZMQ API 4.x):

        sudo apt-get install libzmq3-dev

Dependencies for the GUI: Ubuntu & Debian
-----------------------------------------

If you want to build Bitcred-Qt, make sure that the required packages for Qt development
are installed. Qt 5 is necessary to build the GUI.
If both Qt 4 and Qt 5 are installed, Qt 5 will be used. Pass `--with-gui=qt5` to configure to choose Qt5.
To build without GUI pass `--without-gui`.

For Qt 5 you need the following:

    sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler libcrypto++-dev libdb++-dev

libqrencode (optional) can be installed with:

    sudo apt-get install libqrencode-dev

Once these are installed, they will be found by configure and a alterdot-qt executable will be
built by default.

Notes
-----
The release is built with GCC and then "strip alterdotd" to strip the debug
symbols, which reduces the executable size by about 90%.

miniupnpc
---------

[miniupnpc](http://miniupnp.free.fr/) may be used for UPnP port mapping.  It can be downloaded from [here](
http://miniupnp.tuxfamily.org/files/).  UPnP support is compiled in and
turned off by default.  See the configure options for upnp behavior desired:

    --without-miniupnpc      No UPnP support miniupnp not required
    --disable-upnp-default   (the default) UPnP support turned off by default at runtime
    --enable-upnp-default    UPnP support turned on by default at runtime

To build:

    tar -xzvf miniupnpc-1.6.tar.gz
    cd miniupnpc-1.6
    make
    sudo su
    make install

Berkeley DB
-----------
It is recommended to use Berkeley DB 4.8. If you have to build it yourself:

```bash
ADOT_ROOT=$(pwd)

# Pick some path to install BDB to, here we create a directory within the Alterdot directory
BDB_PREFIX="${ADOT_ROOT}/db4"
mkdir -p $BDB_PREFIX

# Fetch the source and verify that it is not tampered with
wget 'http://download.oracle.com/berkeley-db/db-4.8.30.NC.tar.gz'
echo '12edc0df75bf9abd7f82f821795bcee50f42cb2e5f76a6a281b85732798364ef  db-4.8.30.NC.tar.gz' | sha256sum -c
# -> db-4.8.30.NC.tar.gz: OK
tar -xzvf db-4.8.30.NC.tar.gz

# Build the library and install to our prefix
cd db-4.8.30.NC/build_unix/
#  Note: Do a static build so that it can be embedded into the exectuable, instead of having to find a .so at runtime
../dist/configure --prefix=/usr/local --enable-cxx
make
sudo make install

# Configure Alterdot to use our own-built instance of BDB
cd $BITADOT_ROOT
./configure (other args...) LDFLAGS="-L${BDB_PREFIX}/lib/" CPPFLAGS="-I${BDB_PREFIX}/include/"
```

**Note**: You only need Berkeley DB if the wallet is enabled (see the section *Disable-Wallet mode* below).

Boost
-----
If you need to build Boost yourself:

    sudo su
    ./bootstrap.sh
    ./bjam install


Security
--------
To help make your Alterdot installation more secure by making certain attacks impossible to
exploit even if a vulnerability is found, binaries are hardened by default.
This can be disabled with:

Hardening Flags:

    ./configure --enable-hardening
    ./configure --disable-hardening


Hardening enables the following features:

* Position Independent Executable
    Build position independent code to take advantage of Address Space Layout Randomization
    offered by some kernels. An attacker who is able to cause execution of code at an arbitrary
    memory location is thwarted if he doesn't know where anything useful is located.
    The stack and heap are randomly located by default but this allows the code section to be
    randomly located as well.

    On an Amd64 processor where a library was not compiled with -fPIC, this will cause an error
    such as: "relocation R_X86_64_32 against `......' can not be used when making a shared object;"

    To test that you have built PIE executable, install scanelf, part of paxutils, and use:

        scanelf -e ./alterdotd

    The output should contain:
     TYPE
    ET_DYN

* Non-executable Stack
    If the stack is executable then trivial stack based buffer overflow exploits are possible if
    vulnerable buffers are found. By default, alterdot should be built with a non-executable stack
    but if one of the libraries it uses asks for an executable stack or someone makes a mistake
    and uses a compiler extension which requires an executable stack, it will silently build an
    executable without the non-executable stack protection.

    To verify that the stack is non-executable after compiling use:
    `scanelf -e ./alterdotd`

    the output should contain:
    STK/REL/PTL
    RW- R-- RW-

    The STK RW- means that the stack is readable and writeable but not executable.

Disable-wallet mode
--------------------
When the intention is to run only a P2P node without a wallet, Alterdot may be compiled in
disable-wallet mode with:

    ./configure --disable-wallet

In this case there is no dependency on Berkeley DB 4.8.

Mining is also possible in disable-wallet mode, but only using the `getblocktemplate` RPC call.

AVX2 Mining Optimisations
-------------------------
For increased performance when mining, AVX2 optimisations can be enabled.

This can be done either prior to running the build commands by setting:

    CPPFLAGS=-march=native

or at configure time:

    --enable-avx2

CPUs with AVX2 support:

    Intel
        Haswell processor, Q2 2013
        Haswell E processor, Q3 2014
        Broadwell processor, Q4 2014
        Broadwell E processor, Q3 2016
        Skylake processor, Q3 2015
        Kaby Lake processor, Q3 2016(ULV mobile)/Q1 2017(desktop/mobile)
        Coffee Lake processors, 2017
        Cannon Lake processors, 2018
        Ice Lake processors, 2019
        Comet Lake processors, 2019
    AMD
        Carrizo processors, Q2 2015
        Ryzen processors, Q1 2017

AVX512F Mining Optimisations
-------------------------
For increased performance when mining, AVX512F optimisations can be enabled.

At configure time:

    --enable-avx512f

CPUs with AVX512F support:

    Intel
        Xeon Phi x200/Knights Landing processors, 2016
        Knights Mill processors, 2017
        Skylake-SP processors, 2017
        Skylake-X processors, 2017
        Cannon Lake processors, 2018
        Cascade Lake processors, 2019
        Ice Lake processor, 2019
        Cooper Lake processors, 2020
        Tiger Lake processor, 2020

Whether or not your CPU has AVX2 and AVX512F support is, in general, easily available information on the Web so we recommend looking it up yourself as not all compatibile processors are shown in the lists above. These two Wikipedia articles https://en.wikipedia.org/wiki/Advanced_Vector_Extensions and https://en.wikipedia.org/wiki/AVX-512 offer quite a bit of information in this sense.

Example Build Command
--------------------
Qt Wallet and Deamon, CLI version build:

    ./autogen.sh && ./configure --with-gui && make

CLI and Deamon Only Buld:

    ./autogen.sh && ./configure --without-gui && make
