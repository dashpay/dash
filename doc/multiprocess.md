# Multiprocess Dash

_This document describes usage of the multiprocess feature. For design information, see the [design/multiprocess.md](design/multiprocess.md) file._

## Build Option

On unix systems, the `-DWITH_MULTIPROCESS=ON` option can be passed to CMake to build `dash-node` alongside `dashd`, and `dash-gui` alongside `dash-qt` when the GUI is enabled.

## Debugging

The `-debug=ipc` command line option can be used to see requests and responses between processes.

## Installation

The multiprocess feature requires [Cap'n Proto](https://capnproto.org/) and [libmultiprocess](https://github.com/bitcoin-core/libmultiprocess) as dependencies. A simple way to get started using it without installing these dependencies manually is to use the [depends system](../depends) with the `MULTIPROCESS=1` [dependency option](../depends#dependency-options) passed to make:

```
cd <DASH_SOURCE_DIRECTORY>
make -C depends NO_QT=1 MULTIPROCESS=1
cmake -B build --toolchain depends/x86_64-pc-linux-gnu/toolchain.cmake
cmake --build build
build/src/dash-node -regtest -printtoconsole -debug=ipc
DASHD=dash-node build/test/functional/test_runner.py
```

The toolchain file supplies settings and library locations from depends, so there
is no need to pass `-DWITH_MULTIPROCESS=ON` separately when using the depends
system (it is controlled by the `MULTIPROCESS=1` option).

Alternately, you can install [Cap'n Proto](https://capnproto.org/) and [libmultiprocess](https://github.com/bitcoin-core/libmultiprocess) packages on your system, and just run `cmake -B build -DWITH_MULTIPROCESS=ON` without using the depends system. CMake will locate the installed packages using their CMake package configuration files. See [Installation](https://github.com/bitcoin-core/libmultiprocess#installation) section of the libmultiprocess readme for install steps. See [build-unix.md](build-unix.md) and [build-osx.md](build-osx.md) for information about installing dependencies in general.

## Usage

`dash-node` is a drop-in replacement for `dashd`, and `dash-gui` is a drop-in replacement for `dash-qt`, and there are no differences in use or external behavior between the new and old executables. But internally after backporting [bitcoin#10102](https://github.com/bitcoin/bitcoin/pull/10102), `dash-gui` will spawn a `dash-node` process to run P2P and RPC code, communicating with it across a socket pair, and `dash-node` will spawn `dash-wallet` to run wallet code, also communicating over a socket pair. This will let node, wallet, and GUI code run in separate address spaces for better isolation, and allow future improvements like being able to start and stop components independently on different machines and environments.
[bitcoin#19460](https://github.com/bitcoin/bitcoin/pull/19460) also adds a new `dash-node` `-ipcbind` option and a `dash-wallet` `-ipcconnect` option to allow new wallet processes to connect to an existing node process.
And [bitcoin#19461](https://github.com/bitcoin/bitcoin/pull/19461) adds a new `dash-gui` `-ipcconnect` option to allow new GUI processes to connect to an existing node process.
