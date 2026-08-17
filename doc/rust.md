# Rust in Dash Core

Dash Core has optional support for components written in Rust, bridged into
the C++ codebase with [cxx](https://cxx.rs/). Rust support is **disabled by
default**: a default `./configure && make` performs no Rust work at all and
requires no Rust tooling.

The Rust code lives in `rust/`. Component crates (currently only `chirp`, a
small smoke-test component) are compiled as dependencies of the umbrella crate
`rust/dashrust`, which cargo builds into a single static library that is
linked into the C++ binaries. The C++ side of each bridge is generated from
the crate's `src/lib.rs` by the `cxxbridge` code generator.

## Toolchain requirements

`--enable-rust` requires exact tool versions, enforced at configure time:

| Tool        | Version    |
|-------------|------------|
| `rustc`     | 1.92.0     |
| `cargo`     | 1.92.0     |
| `cxxbridge` | 1.0.198    |

The version pins live in `configure.ac`, `rust-toolchain.toml` (picked up
automatically by rustup) and the depends packages. To update them, change the
version in `depends/packages/native_rust.mk` and run
`contrib/devtools/update-rust-hashes.py`, and/or change
`depends/packages/native_cxxbridge.mk` and run
`contrib/devtools/update-native-cxxbridge.py`, keeping `configure.ac` in sync.

## Building with depends (recommended)

The depends system can provision the whole Rust toolchain, the Rust standard
library for the target, and offline copies of all crate dependencies:

```bash
make -C depends RUST=1 HOST=x86_64-pc-linux-gnu
./configure --prefix=$(pwd)/depends/x86_64-pc-linux-gnu
make
```

`RUST=1` builds/installs into the depends prefix:

- `native_rust`: the pinned Rust compiler and cargo for the build machine;
- `native_cxxbridge`: the pinned `cxxbridge` code generator;
- `rust_stdlib`: the pre-built Rust standard library for the target triple;
- `rustcxx`: the `rust/cxx.h` header;
- `vendored-sources/`: all crates from the workspace `Cargo.lock`, vendored
  for offline use.

The generated `share/config.site` then makes `./configure` default to
`--enable-rust` with `CARGO`, `RUSTC`, `CXXBRIDGE` and
`RUST_VENDORED_SOURCES` pointing into the depends prefix, so no extra
configure flags are needed. Everything after the depends downloads works
offline.

For an offline sources mirror, `make -C depends RUST=1 download` additionally
fetches the Rust standard libraries for all supported targets
(`download-rust-std`) and creates a pre-vendored crate archive
(`vendor-crates`) in `SOURCES_PATH`.

## Supported hosts

Rust support is deliberately confined to the hosts that are validated in CI
and release builds. `--enable-rust` (and `RUST=1` in depends) fails
explicitly on any other host rather than producing binaries for a target that
is never tested:

- Linux: `x86_64`, `aarch64`, `riscv64` (glibc and musl; depends builds use
  the musl-targeted standard library)
- macOS: `x86_64`, `arm64`
- Windows: `x86_64` (MinGW-w64)

Android is explicitly unsupported. Additional targets can be added later
together with CI lanes that exercise them.

## Building with a system toolchain

Instead of depends, a system Rust toolchain can be used as long as it matches
the pinned versions exactly. With rustup, the pinned toolchain from
`rust-toolchain.toml` is selected automatically; the matching code generator
can be installed with:

```bash
rustup toolchain install 1.92.0
cargo install cxxbridge-cmd --version 1.0.198 --locked
```

Note that rustup resolves `rust-toolchain.toml` from the current working
directory, so for out-of-tree builds export `RUSTUP_TOOLCHAIN=1.92.0` (the
build system propagates it to all cargo invocations).

Configure builds are offline by default, so a system-toolchain build must
either provide vendored crates or opt in to network access:

```bash
# Offline: vendor the workspace dependencies once, then point configure at them
cargo vendor --locked /path/to/vendored-sources
./configure --enable-rust RUST_VENDORED_SOURCES=/path/to/vendored-sources

# Online: let cargo fetch dependencies from the network (developer convenience)
./configure --enable-rust --enable-online-rust
```

In offline mode the build generates a cargo config from
`.cargo/config.toml.offline`, adds the vendored directory as the
`crates-io` replacement, and reconstructs source-replacement stanzas for any
git-sourced crates in `Cargo.lock` via
`contrib/devtools/cargo-vendor-git-sources.sh`. Cargo then runs with
`--locked --offline`. In online mode no config is injected and the
developer's own cargo configuration (e.g. in `~/.cargo`) is left in effect;
cargo still runs with `--locked`.

Useful configure variables (see `./configure --help`):

- `RUST_VENDORED_SOURCES`: directory containing vendored crate sources
  (required for offline builds outside depends);
- `RUSTFLAGS`: defaults to `-C embed-bitcode=yes -C relocation-model=pic`;
- `CARGO_INCREMENTAL`: defaults to `0`;
- `NATIVE_CC`/`NATIVE_CXX`/`NATIVE_AR`: build-machine tools, required when
  cross-compiling (depends sets them automatically).

`--enable-debug` builds the Rust code with cargo's debug profile instead of
the release profile.

## Source tarballs

Source distributions generated from a Rust-enabled tree (`make dist`) ship
the generated C++ bridge sources under `rust/<crate>/gen/` together with a
stamp recording the `cxxbridge` version that produced them. Builds from such
a tarball reuse the shipped artifacts instead of regenerating them, provided
the artifacts are at least as new as the stamp and strictly newer than the
crate's `lib.rs`; editing `lib.rs` forces regeneration with the pinned
`cxxbridge`.
