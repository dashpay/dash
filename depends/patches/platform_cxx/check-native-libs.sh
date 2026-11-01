#!/bin/sh
export LC_ALL=C
set -f
# Usage: check-native-libs.sh <build log> <vetted libraries> <ignored libraries>
#
# Fails unless every system library rustc reports for the static archive (the
# "native-static-libs" note in the build log) is vetted or ignored, so that
# whoever bumps the Platform pin or the Rust toolchain notices a new one.
log=$1
vetted=$2
ignored=$3

libs=$(sed -n 's/^note: native-static-libs: //p' "$log")
if test -z "$libs"; then
  echo "rustc reported no native-static-libs in $log" >&2
  exit 1
fi
echo "rustc native-static-libs: $libs"

unvetted=
framework=
for word in $libs; do
  if test "$word" = -framework; then
    framework=yes
    continue
  fi
  lib=$word
  if test -n "$framework"; then
    lib="-framework $word"
    framework=
  fi
  case " $vetted $ignored " in
    *" $lib "*) ;;
    *) unvetted="$unvetted $lib" ;;
  esac
done

if test -n "$unvetted"; then
  echo "rustc reports system libraries platform_cxx.mk has not vetted:$unvetted" >&2
  echo "Check each against contrib/guix/symbol-check.py, then add it to platform_cxx_native_libs_<os>, or with the reason to platform_cxx_native_libs_ignored_<os>." >&2
  exit 1
fi
