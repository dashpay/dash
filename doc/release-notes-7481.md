Build System
------------

Dash Core is now built with CMake. The Autotools build system
(`autogen.sh`, `configure` and the `Makefile.am` files) has been removed,
following Bitcoin Core's migration. See the build guides in `doc/` for the
new instructions; in short:

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

Builds using depends pass its toolchain file instead of `--prefix`, for
example `cmake -B build --toolchain depends/x86_64-pc-linux-gnu/toolchain.cmake`.
Configure options now map to CMake options, for example `--disable-wallet`
becomes `-DENABLE_WALLET=OFF` and `--with-gui` becomes `-DBUILD_GUI=ON`; run
`cmake -B build -LH` to list them. Functional tests are run from the build
directory with `build/test/functional/test_runner.py`.
