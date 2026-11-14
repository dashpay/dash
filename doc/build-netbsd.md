# NetBSD Build Guide

Updated for NetBSD [9.2](https://netbsd.org/releases/formal-9/NetBSD-9.2.html).

This guide describes how to build dashd, command-line utilities, and GUI on NetBSD.

**This guide has not been tested for building Dash Core and may fail. Please report your results; contributions welcome.**

## Preparation

### 1. Install Required Dependencies

Install the required dependencies the usual way you [install software on NetBSD](https://www.netbsd.org/docs/guide/en/chap-boot.html#chap-boot-pkgsrc).
The example commands below use `pkgin`.

```bash
pkgin install cmake pkg-config git gmake boost libevent zstd

```

NetBSD may ship with an older version of `gcc` than is needed to build C++20 (GCC 11.1+ is required). You should install a newer `gcc` and select it when configuring with CMake.

For example, grab `gcc12`:
```
pkgin install gcc12
```

Then, when configuring, pass the following:
```bash
cmake -B build \
    -DCMAKE_C_COMPILER="/usr/pkg/gcc12/bin/gcc" \
    -DCMAKE_CXX_COMPILER="/usr/pkg/gcc12/bin/g++"
```

See [dependencies.md](dependencies.md) for a complete overview.

### 2. Clone Dash Core Repo

Clone the Dash Core repository to a directory. All build scripts and commands will run from this directory.

```bash
git clone https://github.com/dashpay/dash.git
```

### 3. Install Optional Dependencies

#### Wallet Dependencies

It is not necessary to build wallet functionality to run dashd or the GUI.

###### Descriptor Wallet Support

`sqlite3` is required to enable support for [descriptor wallets](descriptors.md).

```bash
pkgin install sqlite3
```

###### Legacy Wallet Support

`db4` is required to enable support for legacy wallets.

```bash
pkgin install db4
```

#### GUI Dependencies

Dash Core includes a GUI built with the cross-platform Qt Framework. To compile the GUI, install `qt5` and pass `-DBUILD_GUI=ON` to CMake.

```bash
pkgin install qt5
```

The GUI can encode addresses in a QR Code. Install `qrencode`, or pass
`-DWITH_QRENCODE=OFF` to disable this feature.

```bash
pkgin install qrencode
```

#### Test Suite Dependencies

There is an included test suite that is useful for testing code changes when developing.
To run the test suite (recommended), you will need to have Python 3.10+ installed:

```bash
pkgin install python312
```

### Building Dash Core


### 1. Configuration

There are many ways to configure Dash Core. Here is an example that
explicitly disables the wallet and GUI:

```bash
cmake -B build -DENABLE_WALLET=OFF -DBUILD_GUI=OFF
```

For a full list of configuration options, run `cmake -B build -LH`.

### 2. Compile

Build and run the tests:

```bash
cmake --build build     # use "-j N" for N parallel jobs
ctest --test-dir build  # Some tests require Python 3
```
