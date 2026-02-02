#!/bin/bash

default_host_triple=""
default_toolchain=""
IFS=" = "
while read -r name value
do
  value="${value//\"/}"
  if [ "${name}" == "default_host_triple" ]; then
    default_host_triple="${value}"
  elif [ "${name}" == "default_toolchain" ]; then
    default_toolchain="${value}"
  fi
done < ~/.rustup/settings.toml

echo "default_host_triple=${default_host_triple}"
echo "default_toolchain=${default_toolchain}"

rustup component add llvm-tools-preview

rm -rf /tmp/merk-pgo
RUSTFLAGS="-Cprofile-generate=/tmp/merk-pgo" cargo bench rand_rocks
~/.rustup/toolchains/${default_toolchain}/lib/rustlib/${default_host_triple}/bin/llvm-profdata merge -o /tmp/merk-pgo/merged.profdata /tmp/merk-pgo
RUSTFLAGS="-Cprofile-use=/tmp/merk-pgo/merged.profdata" cargo bench
