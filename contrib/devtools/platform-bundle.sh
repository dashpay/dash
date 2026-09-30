#!/usr/bin/env bash
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C
set -euo pipefail

# Produces the crate bundle that depends/packages/platform_cxx.mk builds
# dash-platform-cxx from, for a given dashpay/platform commit.
#
# The bundle holds a Cargo workspace trimmed to packages/rs-platform-cxx, the
# commit's Cargo.lock pruned to that workspace (entries are only removed, never
# changed), every locked crate vendored with `cargo vendor --locked
# --versioned-dirs`, the Tenderdash source archive tenderdash-proto's build
# script generates its protobuf code from (tenderdash/tenderdash-<tag>.zip, for
# the rs-tenderdash-abci tag the lock pins), and a .cargo/config.toml that
# replaces all crate sources with the vendored directory and sets
# TENDERDASH_COMMITISH to that same tag. Vendored crates outside the build
# closure of dash-platform-cxx (dev-dependencies and crates only other
# workspace members use) are reduced to their manifest and empty target files:
# Cargo needs them to resolve the lock, never to build. The archive is written
# with a fixed file order, owner, mode and mtime, so the same commit yields the
# same sha256 on every machine that uses the pinned Rust version, GNU tar and
# GNU gzip (and as long as GitHub serves the same Tenderdash zip).
#
# This is the only step that downloads crates. The maintainer bumping the
# pin runs it, updates the hashes in platform_cxx.mk from its output, and
# uploads the Platform tarball and the bundle to the depends sources mirror;
# reviewers rerun it to reproduce the hash.

usage() {
    echo "Usage: $0 <dashpay/platform commit>" >&2
    echo >&2
    echo "Writes platform-<commit>.tar.gz and platform-cxx-crates-<commit>.tar.gz to" >&2
    echo "SOURCES_PATH (default: depends/sources), and keeps the downloaded" >&2
    echo "tenderdash-<tag>.zip there." >&2
    exit 1
}

[ $# -eq 1 ] || usage
COMMIT="$1"
[[ "$COMMIT" =~ ^[0-9a-f]{40}$ ]] || { echo "error: expected a full 40-character commit hash" >&2; exit 1; }

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOURCES_PATH="${SOURCES_PATH:-$TOPDIR/depends/sources}"
mkdir -p "$SOURCES_PATH"
# The script changes directory below; a relative path must not follow it.
SOURCES_PATH="$(cd "$SOURCES_PATH" && pwd)"
# shellcheck disable=SC2016
RUST_VERSION="$(sed -n 's/^\$(package)_version:=//p' "$TOPDIR/depends/packages/native_rust.mk")"
CARGO="${CARGO:-cargo}"
TAR="${TAR:-tar}"
GZIP_PROG="${GZIP_PROG:-gzip}"

# cargo vendor output (normalized manifests) depends on the Cargo version.
export RUSTUP_TOOLCHAIN="${RUSTUP_TOOLCHAIN:-$RUST_VERSION}"
case "$("$CARGO" --version)" in
    "cargo $RUST_VERSION "*) ;;
    *) echo "error: $CARGO is not Cargo $RUST_VERSION (the version pinned in native_rust.mk)" >&2; exit 1 ;;
esac
case "$("$TAR" --version)" in
    *"GNU tar"*) ;;
    *) echo "error: $TAR is not GNU tar; set TAR" >&2; exit 1 ;;
esac
case "$("$GZIP_PROG" --version)" in
    *"Free Software Foundation"*) ;;
    *) echo "error: $GZIP_PROG is not GNU gzip; set GZIP_PROG" >&2; exit 1 ;;
esac

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/platform-bundle.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT
# Cargo looks for configuration above the physical directory it runs in.
WORKDIR="$(cd "$WORKDIR" && pwd -P)"
# A private Cargo home keeps the cached registries and the Cargo home's
# config.toml out of the bundle. Cargo also reads .cargo/config.toml from every
# parent of the directory it runs in, so the work directory must have none
# (TMPDIR under a home directory with ~/.cargo/config.toml fails here).
export CARGO_HOME="$WORKDIR/cargo-home"
dir="$WORKDIR"
while :; do
    dir="$(dirname "$dir")"
    for config in "$dir/.cargo/config" "$dir/.cargo/config.toml"; do
        if [ -e "$config" ]; then
            echo "error: $config would configure Cargo; set TMPDIR to a directory outside its tree" >&2
            exit 1
        fi
    done
    [ "$dir" != / ] || break
done

# Downloads $1 to SOURCES_PATH/$2, unless a copy there passes the integrity
# test $3 (a partial or corrupt one is downloaded again).
fetch() {
    local url="$1" file="$2" test="$3"
    if [ -f "$SOURCES_PATH/$file" ] && $test "$SOURCES_PATH/$file" 2> /dev/null; then
        return
    fi
    rm -f "$SOURCES_PATH/$file"
    curl --location --fail --silent --show-error --retry 3 -o "$SOURCES_PATH/$file.temp" "$url"
    $test "$SOURCES_PATH/$file.temp"
    mv "$SOURCES_PATH/$file.temp" "$SOURCES_PATH/$file"
}

gzip_test() {
    "$GZIP_PROG" -t "$1"
}

zip_test() {
    python3 -c 'import sys, zipfile; sys.exit(zipfile.ZipFile(sys.argv[1]).testzip() is not None)' "$1"
}

sha256() {
    if command -v sha256sum >/dev/null; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# Prints "name version source" for every [[package]] of a Cargo.lock.
lock_entries() {
    awk '/^\[\[package\]\]$/ { if (name) print name, version, source; name = version = source = "" }
         /^name = /    { name = $3 }
         /^version = / { version = $3 }
         /^source = /  { source = $3 }
         END           { if (name) print name, version, source }' "$1" | sort
}

PLATFORM_ARCHIVE="platform-$COMMIT.tar.gz"
fetch "https://github.com/dashpay/platform/archive/$COMMIT.tar.gz" "$PLATFORM_ARCHIVE" gzip_test

SRC="$WORKDIR/src"
mkdir -p "$SRC"
"$TAR" --strip-components=1 -xzf "$SOURCES_PATH/$PLATFORM_ARCHIVE" -C "$SRC"
cd "$SRC"

# Trim the workspace to the one crate depends builds and prune the lock to it.
cp Cargo.lock "$WORKDIR/Cargo.lock.pinned"
awk '/^members = \[/ { print "members = [\"packages/rs-platform-cxx\"]"; skip = !/\]/; next }
     skip && /^\]/   { skip = 0; next }
     !skip' Cargo.toml > "$WORKDIR/Cargo.toml"
mv "$WORKDIR/Cargo.toml" Cargo.toml
"$CARGO" metadata --format-version 1 > /dev/null
if lock_entries Cargo.lock | comm -13 <(lock_entries "$WORKDIR/Cargo.lock.pinned") - | grep .; then
    echo "error: trimming the workspace changed the locked packages above" >&2
    exit 1
fi

TENDERDASH_TAG="$(awk '/^name = "tenderdash-proto"$/ { found = 1 }
                       found && /^source = / { print; exit }' Cargo.lock |
                  sed -n 's|.*/rs-tenderdash-abci?tag=\(v[^#]*\)#.*|\1|p')"
[ -n "$TENDERDASH_TAG" ] || { echo "error: no tagged tenderdash-proto entry in Cargo.lock" >&2; exit 1; }
TENDERDASH_ARCHIVE="tenderdash-$TENDERDASH_TAG.zip"
fetch "https://github.com/dashpay/tenderdash/archive/$TENDERDASH_TAG.zip" "$TENDERDASH_ARCHIVE" zip_test
mkdir tenderdash
cp "$SOURCES_PATH/$TENDERDASH_ARCHIVE" tenderdash/

rm -rf .cargo
mkdir .cargo
{
    echo "# Generated by contrib/devtools/platform-bundle.sh for dashpay/platform@$COMMIT"
    "$CARGO" vendor --locked --versioned-dirs vendor
    echo
    echo "[env]"
    echo "TENDERDASH_COMMITISH = \"$TENDERDASH_TAG\""
} > "$WORKDIR/config.toml"
grep -qx 'directory = "vendor"' "$WORKDIR/config.toml" || { echo "error: cargo vendor printed no source replacement" >&2; exit 1; }
mv "$WORKDIR/config.toml" .cargo/config.toml
# From here on every crate must come from vendor/, never from the Cargo home.
rm -rf "$CARGO_HOME"

closure() {
    "$CARGO" tree --frozen -p dash-platform-cxx -e normal,build --target all --prefix none --format '{p}' |
        awk '{ sub(/^v/, "", $2); print $1 "-" $2 }' | sort -u
}
closure > "$WORKDIR/closure"
for dir in vendor/*/; do
    dir="${dir%/}"
    grep -qxF "${dir#vendor/}" "$WORKDIR/closure" && continue
    checksum="$(sed -En 's/.*"package":(null|"[0-9a-f]*")}$/\1/p' "$dir/.cargo-checksum.json")"
    find "$dir" ! -type d ! -name '*.rs' ! -path "$dir/Cargo.toml" -delete
    find "$dir" -type f -name '*.rs' -print0 | while IFS= read -r -d '' file; do : > "$file"; done
    find "$dir" -type d -empty -delete
    printf '{"files":{},"package":%s}' "$checksum" > "$dir/.cargo-checksum.json"
done
closure | cmp -s - "$WORKDIR/closure" || { echo "error: the build closure changed after pruning" >&2; exit 1; }

BUNDLE="platform-cxx-crates-$COMMIT.tar.gz"
"$TAR" --create --format=gnu --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
       --mode='u+rw,go+r-w,a+X' Cargo.toml Cargo.lock .cargo/config.toml tenderdash vendor |
    "$GZIP_PROG" -9n > "$SOURCES_PATH/$BUNDLE.temp"
mv "$SOURCES_PATH/$BUNDLE.temp" "$SOURCES_PATH/$BUNDLE"

cat <<EOF
$(sha256 "$SOURCES_PATH/$PLATFORM_ARCHIVE")  $PLATFORM_ARCHIVE
$(sha256 "$SOURCES_PATH/$BUNDLE")  $BUNDLE
$(sha256 "$SOURCES_PATH/$TENDERDASH_ARCHIVE")  $TENDERDASH_ARCHIVE (in the bundle)

depends/packages/platform_cxx.mk:
\$(package)_version=$COMMIT
\$(package)_sha256_hash=$(sha256 "$SOURCES_PATH/$PLATFORM_ARCHIVE")
\$(package)_crates_sha256_hash=$(sha256 "$SOURCES_PATH/$BUNDLE")
EOF
