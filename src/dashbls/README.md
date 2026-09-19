[![GitHub License](https://img.shields.io/github/license/dashpay/bls-signatures)](https://github.com/dashpay/bls-signatures/blob/main/LICENSE)
[![Library status](https://img.shields.io/github/actions/workflow/status/dashpay/bls-signatures/build-test.yaml?branch=main&style=flat&logo=github&logoColor=white&label=library)](https://github.com/dashpay/bls-signatures/actions/workflows/build-test.yaml?query=branch%3Amain)
[![Binds status](https://img.shields.io/github/actions/workflow/status/dashpay/bls-signatures/build-binds.yml?branch=main&style=flat&logo=github&logoColor=white&label=binds)](https://github.com/dashpay/bls-signatures/actions/workflows/build-binds.yml?query=branch%3Amain)

> [!WARNING]
>
> **This library has not undergone a formal security review.**

`bls-signatures` is a cross-platform library implementing BLS12-381 primitives for Dash built on
the [`blst library`](https://github.com/supranational/blst.git) toolkit with bindings available in
[Python](./binds/python), [Rust](./rust-bindings/), [Go](./go-bindings/) and [Javascript](./js-bindings).

## Dependencies

* A C++17 capable compiler (GCC 9, Clang 7 or higher)
* CMake 3.18 or higher (or Autoconf 2.71 or higher; with libtool and automake)

Additionally, the following dependencies are supplied by the codebase

* [`catch2`](https://github.com/catchorg/Catch2) (for tests)
* [`mimalloc`](https://github.com/microsoft/mimalloc) (for secure memory operations)
* [`blst`](https://github.com/supranational/blst.git) (for cryptographic operations)

## Build library

> [!NOTE]
> Multi-config generators (like Visual Studio) place the binaries under a per-configuration directory, so on Windows the
> executables are at `src\Release\runtest.exe` and `src\Release\runbench.exe`.

```sh
# Create scratchpad directory
mkdir build && cd build

# Configure build files
cmake ..

# Build library with 4 threads
cmake --build . --parallel 4

# Run tests
./src/runtest

# Run benchmarks
./src/runbench
```

## Install Python binds

Releases are published to a [PEP 503](https://peps.python.org/pep-0503/) index hosted on GitHub Pages rather than to
PyPI.

```sh
pip install --extra-index-url https://dashpay.github.io/bls-signatures/pep503/ dashbls
```

> [!IMPORTANT]
> Use `--extra-index-url`, not `--index-url`. Platforms we do not ship a wheel for fall back to the sdist, and building
> it needs `cmake`, `pybind11`, `setuptools_scm` and `setuptools` from PyPI. `--index-url` would replace PyPI with an
> index that carries only `dashbls`, so the build would fail before it started.

## Build Python binds

Our Python binds target Python 3.10 or higher; they depend on the following packages. Sample code is available
at [`binds/python/samples`](binds/python/samples).

* [`pybind11`](https://github.com/pybind/pybind11) (bridging C++ and Python)
* [`pytest`](https://github.com/pytest-dev/pytest) (benchmarks and unit tests, part of optional `[dev]` dependency group)
* [`ruff`](https://github.com/astral-sh/ruff) (linting, part of optional `[dev]` dependency group)

> [!NOTE]
> We recommend using programs like [`uv`](https://github.com/astral-sh/uv) to manage your virtualenv (`venv`) to prevent
> cross-contamination with Python-based native packages or other Python projects.

```sh
# Create a new venv named dashbls
uv venv dashbls

# Enter venv
source dashbls/bin/activate

# Install developer dependencies
uv pip install -e ".[dev]"

# Build binds
uv build

# Run linter and formatter
uv run ruff check
uv run ruff format --check

# Run unit tests
uv run pytest -v binds/python/test_unit.py

# Run benchmarks
uv run pytest -v binds/python/test_bench.py --benchmark-only
```

## BLST license

BLST is used with the
[Apache 2.0 license](https://github.com/supranational/blst/blob/master/LICENSE)

## License

```text
Copyright (c) 2018-present, Chia Network, Inc.
Copyright (c) 2021-present, The Dash Core developers.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

The above license terms are available in [`LICENSE`](./LICENSE) and licensing terms for dependencies are
available in [`NOTICE`](./NOTICE)
