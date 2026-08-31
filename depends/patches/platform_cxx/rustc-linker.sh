#!/bin/sh
# rustc's `-C linker=` takes a single executable, but the configured
# compiler is a full command line (target/sysroot flags; under Guix an
# `env -u ...` prefix). CC carries that command in cargo's environment;
# word-splitting it here preserves every part of it.
exec $CC "$@"
