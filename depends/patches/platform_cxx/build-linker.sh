#!/bin/sh
export LC_ALL=C
set -f
# Links build scripts and proc macros, which run on the build machine, with the
# depends build compiler. rustc's `-C linker=` takes a single executable, but
# DEPENDS_BUILD_CC is a command line (on macOS with an -isysroot flag).
# shellcheck disable=SC2086
exec $DEPENDS_BUILD_CC "$@"
