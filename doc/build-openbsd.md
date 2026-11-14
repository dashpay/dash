# OpenBSD Build Guide

**Updated for OpenBSD [7.3](https://www.openbsd.org/73.html)**

This guide describes how to build dashd, command-line utilities, and GUI on OpenBSD.

## Preparation

### 1. Install Required Dependencies
Run the following as root to install the base dependencies for building.

```bash
pkg_add cmake git gmake libevent boost zstd
# Select Python 3.10 or newer:
pkg_add python
```

See [dependencies.md](dependencies.md) for a complete overview.

### 2. Clone Dash Core Repo
Clone the Dash Core repository to a directory. All build scripts and commands will run from this directory.
``` bash
git clone https://github.com/dashpay/dash.git
```

### 3. Install Optional Dependencies

#### Wallet Dependencies

It is not necessary to build wallet functionality to run either `dashd` or `dash-qt`.

###### Descriptor Wallet Support

`sqlite3` is required to support [descriptor wallets](descriptors.md).

``` bash
pkg_add sqlite3
```

###### Legacy Wallet Support
BerkeleyDB is only required to support legacy wallets.

It is recommended to use Berkeley DB 4.8. You cannot use the BerkeleyDB library
from ports. However you can build it yourself, [using depends](/depends).

```bash
gmake -C depends NO_BOOST=1 NO_LIBEVENT=1 NO_QT=1 NO_SQLITE=1 NO_NATPMP=1 NO_UPNP=1 NO_ZMQ=1 NO_USDT=1
...
to: /path/to/dash/depends/x86_64-unknown-openbsd
```

Then set `BDB_PREFIX`:

```bash
export BDB_PREFIX="/path/to/dash/depends/x86_64-unknown-openbsd"
```

#### GUI Dependencies
###### Qt5

Dash Core includes a GUI built with the cross-platform Qt Framework. To compile the GUI, Qt 5 is required. Pass `-DBUILD_GUI=ON` to CMake.
Install libqrencode for QR code support, or pass `-DWITH_QRENCODE=OFF`.

```bash
pkg_add qt5
```

## Building Dash Core

### 1. Configuration

There are many ways to configure Dash Core, here are a few common examples:

##### Descriptor Wallet and GUI:
This enables the GUI and descriptor wallet support, assuming `sqlite` and `qt5` are installed.

```bash
cmake -B build -DBUILD_GUI=ON
```

##### Descriptor & Legacy Wallet. No GUI:
This enables support for both wallet types and disables the GUI:

```bash
cmake -B build -DBUILD_GUI=OFF -DWITH_BDB=ON \
    -DBerkeleyDB_INCLUDE_DIR:PATH="${BDB_PREFIX}/include"
```

### 2. Compile

```bash
cmake --build build     # use "-j N" for N parallel jobs
ctest --test-dir build  # Some tests require Python 3
```

## Resource limits

If the build runs into out-of-memory errors, the instructions in this section
might help.

The standard ulimit restrictions in OpenBSD are very strict:
```bash
data(kbytes)         1572864
```

This, unfortunately, in some cases not enough to compile some `.cpp` files in the project,
(see issue [#6658](https://github.com/bitcoin/bitcoin/issues/6658)).
If your user is in the `staff` group the limit can be raised with:
```bash
ulimit -d 3000000
```
The change will only affect the current shell and processes spawned by it. To
make the change system-wide, change `datasize-cur` and `datasize-max` in
`/etc/login.conf`, and reboot.
