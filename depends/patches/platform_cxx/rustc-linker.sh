#!/bin/sh
export LC_ALL=C
set -f
# rustc's `-C linker=` takes a single executable, but the depends compiler is
# a command line (target and sysroot flags, under Guix an `env -u ...` prefix).
# platform_cxx.mk passes that command in DEPENDS_CC and the link flags in
# DEPENDS_LDFLAGS; word splitting them here keeps every part, and set -f keeps
# a flag with a glob character from being expanded.
# shellcheck disable=SC2086
exec $DEPENDS_CC $DEPENDS_LDFLAGS "$@"
