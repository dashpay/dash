# Multiprocess Dash

_This document describes usage of the multiprocess feature. For design information, see the [design/multiprocess.md](design/multiprocess.md) file._

## Build Option

On unix systems, the `-DENABLE_IPC=ON` option can be passed to CMake to build `dash-node` alongside `dashd`, and `dash-gui` alongside `dash-qt` when the GUI is enabled.

## Debugging

The `-debug=ipc` command line option can be used to see requests and responses between processes.

## Installation

Specifying `-DENABLE_IPC=ON` requires [Cap'n Proto](https://capnproto.org/) to be installed. See [build-unix.md](build-unix.md) and [build-osx.md](build-osx.md) for information about installing dependencies.

### Depends installation

Alternately the [depends system](../depends) can be used to avoid need to install local dependencies. A simple way to get started is to pass the `MULTIPROCESS=1` [dependency option](../depends#dependency-options) to make:

```
cd <DASH_SOURCE_DIRECTORY>
make -C depends NO_QT=1 MULTIPROCESS=1
# Set host platform to output of gcc -dumpmachine or clang -dumpmachine or check the depends/ directory for the generated subdirectory name
HOST_PLATFORM="x86_64-pc-linux-gnu"
cmake -B build --toolchain=depends/$HOST_PLATFORM/toolchain.cmake
cmake --build build
build/src/dash-node -regtest -printtoconsole -debug=ipc
DASHD=dash-node build/test/functional/test_runner.py
```

The toolchain file supplies settings and library locations from depends, so there
is no need to pass `-DENABLE_IPC=ON` separately when using the depends
system (it is controlled by the `MULTIPROCESS=1` option).

### Cross-compiling

When cross-compiling and not using depends, native code generation tools from [libmultiprocess](https://github.com/bitcoin-core/libmultiprocess) and [Cap'n Proto](https://capnproto.org/) are required. They can be passed to the cmake build by specifying `-DMPGEN_EXECUTABLE=/path/to/mpgen -DCAPNP_EXECUTABLE=/path/to/capnp -DCAPNPC_CXX_EXECUTABLE=/path/to/capnpc-c++` options.

## Usage

`dash-node` is a drop-in replacement for `dashd`, and `dash-gui` is a drop-in replacement for `dash-qt`, and there are no differences in use or external behavior between the new and old executables. But internally after backporting [bitcoin#10102](https://github.com/bitcoin/bitcoin/pull/10102), `dash-gui` will spawn a `dash-node` process to run P2P and RPC code, communicating with it across a socket pair, and `dash-node` will spawn `dash-wallet` to run wallet code, also communicating over a socket pair. This will let node, wallet, and GUI code run in separate address spaces for better isolation, and allow future improvements like being able to start and stop components independently on different machines and environments.
[bitcoin#19460](https://github.com/bitcoin/bitcoin/pull/19460) also adds a new `dash-node` `-ipcbind` option and a `dash-wallet` `-ipcconnect` option to allow new wallet processes to connect to an existing node process.
And [bitcoin#19461](https://github.com/bitcoin/bitcoin/pull/19461) adds a new `dash-gui` `-ipcconnect` option to allow new GUI processes to connect to an existing node process.
